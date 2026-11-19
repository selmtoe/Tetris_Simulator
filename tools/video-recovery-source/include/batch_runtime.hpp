#pragma once

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace tr {

// Format outside the shared ostream and publish a complete protocol line at once.
class BatchLog : public std::ostringstream {
public:
    ~BatchLog() {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        std::cout << str() << std::flush;
    }
};

inline int batchSetting(const char* name, int fallback, int low, int high) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    return end == value || *end ? fallback : static_cast<int>(std::clamp<long>(parsed, low, high));
}

struct ResourceSnapshot {
    bool ramKnown = false, gpuKnown = false;
    double ramFreeMB = 0, commitFreeMB = 0, gpuFreeMB = 0;
};

struct BatchBudget {
    int ram = batchSetting("TR_BATCH_WORKER_RAM_MB", 2048, 256, 65536);
    int commit = batchSetting("TR_BATCH_WORKER_COMMIT_MB", 4096, 64, 65536);
    // Measured peak on an 8 GB laptop GPU is ~1321 MB per worker, so a 1024 MB
    // reservation silently under-booked by 29% and let the scheduler believe a
    // worker would fit when it would not.
    int gpu = batchSetting("TR_BATCH_WORKER_VRAM_MB", 1400, 512, 65536);
    int ramReserve = batchSetting("TR_BATCH_RAM_RESERVE_MB", 3072, 64, 65536);
    int gpuReserve = batchSetting("TR_BATCH_VRAM_RESERVE_MB", 768, 256, 65536);
    bool admits(const ResourceSnapshot& memory, unsigned active) const {
        // `free` is sampled after every already-running worker allocated its
        // peak, so charging those workers a second time rejected the third
        // worker on an 8 GB card while ~2.9 GB was still free. Charge the
        // reserve plus the budget of the worker being admitted; the worker cap
        // bounds how many can be admitted before their allocations show up.
        (void)active;
        const bool ramOk = !memory.ramKnown ? active == 0 :
            memory.ramFreeMB >= ramReserve + ram &&
            memory.commitFreeMB >= ramReserve + commit;
        const bool gpuOk = !memory.gpuKnown ? active == 0 :
            memory.gpuFreeMB >= gpuReserve + gpu;
        return ramOk && gpuOk;
    }
};

// NVML observes all processes without creating a CUDA context. Missing telemetry
// disables extra workers, but never removes matches or changes inference settings.
class ResourceMonitor {
    struct Memory { unsigned long long total, free, used; };
    using Init = int (*)();
    using Handle = int (*)(unsigned, void**);
    using Query = int (*)(void*, Memory*);
    HMODULE library_ = nullptr;
    Init shutdown_ = nullptr;
    Query query_ = nullptr;
    void* device_ = nullptr;
public:
    ResourceMonitor() {
        library_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) return;
        auto init = reinterpret_cast<Init>(GetProcAddress(library_, "nvmlInit_v2"));
        auto handle = reinterpret_cast<Handle>(GetProcAddress(library_, "nvmlDeviceGetHandleByIndex_v2"));
        query_ = reinterpret_cast<Query>(GetProcAddress(library_, "nvmlDeviceGetMemoryInfo"));
        if (!init || !handle || !query_ || init() != 0) { query_ = nullptr; return; }
        shutdown_ = reinterpret_cast<Init>(GetProcAddress(library_, "nvmlShutdown"));
        if (handle(0, &device_) != 0) query_ = nullptr;
    }
    ~ResourceMonitor() {
        if (shutdown_) shutdown_();
        if (library_) FreeLibrary(library_);
    }
    ResourceSnapshot sample() const {
        ResourceSnapshot value;
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        value.ramKnown = GlobalMemoryStatusEx(&memory) != FALSE;
        value.ramFreeMB = memory.ullAvailPhys / 1048576.0;
        value.commitFreeMB = memory.ullAvailPageFile / 1048576.0;
        Memory gpu{};
        value.gpuKnown = query_ && query_(device_, &gpu) == 0;
        value.gpuFreeMB = gpu.free / 1048576.0;
        return value;
    }
};

// Admission is between matches: running matches always retain the exact profile.
// Reserve complete peak budgets for all active jobs, including allocations which
// have not happened yet. This intentionally errs towards fewer workers.
template<class T> class BatchQueue {
    using Clock = std::chrono::steady_clock;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<T> pending_;
    ResourceMonitor resources_;
    bool closed_ = false, cancelled_ = false;
    unsigned active_ = 0;
    BatchBudget budget_;
    Clock::time_point lastLog_{};
public:
    const int workers = batchSetting("TR_BATCH_WORKERS",
        std::max(1u, std::min(3u, std::thread::hardware_concurrency() / 4)), 1, 8);
    void push(T item) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(std::move(item));
        wake_.notify_all();
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        wake_.notify_all();
    }
    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
        wake_.notify_all();
    }
    bool take(T& item, const std::function<void()>& releaseIdle = {}) {
        std::unique_lock<std::mutex> lock(mutex_);
        auto blockedSince = Clock::now();
        // Releasing an idle worker's CUDA/ONNX sessions forces a full model
        // re-initialisation when it is admitted again.  A shortage that clears
        // on its own (the detector's own VRAM spikes, another match finishing)
        // therefore used to cost more than the wait it avoided.  Keep the
        // sessions for a short grace period before giving them back.
        const auto releaseDelay = std::chrono::seconds(
            batchSetting("TR_BATCH_RELEASE_DELAY_SECONDS", 5, 0, 60));
        auto admissionBlockedSince = Clock::now();
        bool firstFailure = true;
        bool released = false;
        for (;;) {
            if (cancelled_ || (closed_ && pending_.empty())) return false;
            if (pending_.empty()) {
                wake_.wait(lock);
                blockedSince = Clock::now();
                firstFailure = true;
                released = false;
                continue;
            }
            const auto memory = resources_.sample();
            // A single worker remains usable with unavailable telemetry. When
            // telemetry exists, even that worker must have its headroom.
            if (budget_.admits(memory, active_)) {
                item = std::move(pending_.front());
                pending_.pop_front();
                ++active_;
                firstFailure = true;
                released = false;
                BatchLog() << "scheduler_admit=1 active=" << active_
                           << " queued=" << pending_.size()
                           << " ram_free_mb=" << memory.ramFreeMB
                           << " vram_free_mb=" << memory.gpuFreeMB << '\n';
                return true;
            }
            const auto now = Clock::now();
            if (firstFailure) {
                admissionBlockedSince = now;
                firstFailure = false;
            }
            if (!released && releaseIdle && now - admissionBlockedSince >= releaseDelay) {
                released = true;
                lock.unlock();
                releaseIdle();
                lock.lock();
                continue;
            }
            if (now - lastLog_ > std::chrono::seconds(5)) {
                lastLog_ = now;
                BatchLog() << "scheduler_wait=memory active=" << active_
                           << " ram_free_mb=" << memory.ramFreeMB
                           << " commit_free_mb=" << memory.commitFreeMB
                           << " vram_free_mb=" << memory.gpuFreeMB
                           << " need_ram_mb=" << (budget_.ramReserve + (active_ + 1) * budget_.ram)
                           << " need_commit_mb=" << (budget_.ramReserve + (active_ + 1) * budget_.commit)
                           << " need_vram_mb=" << (budget_.gpuReserve + (active_ + 1) * budget_.gpu)
                           << " ram_known=" << memory.ramKnown
                           << " gpu_known=" << memory.gpuKnown << '\n';
            }
            if (!active_ && now - blockedSince > std::chrono::seconds(60))
                throw std::runtime_error("Insufficient RAM/VRAM headroom for one analysis worker (60s)");
            wake_.wait_for(lock, std::chrono::milliseconds(250));
            if (active_) blockedSince = Clock::now();
        }
    }
    void finished() {
        std::lock_guard<std::mutex> lock(mutex_);
        --active_;
        wake_.notify_all();
    }
};

} // namespace tr
