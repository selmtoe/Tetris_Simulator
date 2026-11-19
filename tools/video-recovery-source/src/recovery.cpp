#include "recovery.hpp"
#include "recovery_quality_json.hpp"

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_GPU_ULTRA_DEFAULT)
#include "d3d11_board_preprocessor.hpp"
#include "d3d11_queue_preprocessor.hpp"
#if defined(TR_NVDEC_DEFAULT)
#include "nvdec_cuda_preprocessor.hpp"
#endif
#endif

#include "onnx_model.hpp"
#include "neural_ranker.hpp"
#include "queue_onnx_model.hpp"
#include "tetris_engine.hpp"
#include "vision.hpp"

#ifndef __EMSCRIPTEN__
#include <windows.h>
#include <bcrypt.h>
#else
#include "wasm_sha256.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <future>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <ostream>
#include <set>
#include <sstream>
#include <thread>

namespace tr {

#if defined(TR_BROWSER_PARITY_DIAGNOSTIC)
#include "browser_parity_trace.inc"
#endif

namespace {

bool samePieces(const std::vector<Cell>& a, const std::vector<Cell>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

bool sameQueue(const QueueObservation& a, const QueueObservation& b) {
    return a.hold == b.hold && samePieces(a.next, b.next);
}

bool validQueue(const QueueObservation& queue) {
    if (queue.next.size() < 3) return false;
    // Keep accepting shorter simulator/video queue windows, but never allow a
    // non-piece in any slot that was actually observed.  Checking only the
    // first three slots would let a corrupted tail participate in a later
    // transition and make the decoder invent a placement.
    return std::all_of(queue.next.begin(), queue.next.end(), [](Cell cell) {
        return isPiece(cell);
    });
}

struct QueueTransition {
    std::string action = "none";
    bool nextAdvanced = false;
    bool holdChanged = false;
    bool plausible = false;
    int slideCount = 0;
};

QueueTransition checkQueueTransition(const QueueObservation& observed,
                                     const QueueObservation& last,
                                     Cell active) {
    // The queue is a sliding window. A real lock can advance it by one, and
    // two locks can be hidden between two video scans, but an unrelated
    // random 5-piece string cannot suddenly replace it. The old implementation
    // only checked one slide and accepted any confirmed value; that allowed
    // visual effects to permanently poison the queue state.
    QueueTransition result;
    if (!validQueue(observed) || !validQueue(last)) return result;
    result.holdChanged = observed.hold != last.hold;

    const auto overlapMatches = [&](int offset, int required) {
        const int available = std::min(static_cast<int>(observed.next.size()),
                                       static_cast<int>(last.next.size()) - offset);
        if (available < required) return false;
        for (int i = 0; i < required; ++i) {
            if (observed.next[static_cast<std::size_t>(i)] !=
                last.next[static_cast<std::size_t>(offset + i)]) return false;
        }
        return true;
    };

    // A zero-slide observation must match all visible slots. Checking only
    // the first three slots is exactly what let OJSIO replace OJSIT in the
    // old decoder: the bad fifth slot was then treated as a confirmed queue.
    const bool nextSame = observed.next.size() == last.next.size() &&
                          samePieces(observed.next, last.next);
    if (nextSame && !result.holdChanged) {
        result.plausible = true;
        return result;
    }
    // Restore the original strict overlap rule: all four visible overlap
    // slots for one advance, or all three for two advances.
    for (int shift = 1; shift <= 2; ++shift) {
        const int required = shift == 1 ? 4 : 3;
        if (overlapMatches(shift, required)) {
            result.slideCount = shift;
            result.nextAdvanced = true;
            result.plausible = !result.holdChanged;
            break;
        }
    }

    // A hold transition and a queue slide can be real, but two simultaneous
    // slides plus a hold change are ambiguous at this sampling rate. Reject
    // that combination instead of guessing an active piece.
    if (result.holdChanged && result.nextAdvanced) {
        result.plausible = result.slideCount == 1;
    } else if (result.holdChanged && nextSame) {
        result.plausible = true;
    }
    if (!result.plausible) return result;

    if (active == Cell::Empty) {
        if (!result.holdChanged && result.nextAdvanced) result.action = "spawn";
        return result;
    }
    if (!result.holdChanged && result.nextAdvanced) {
        result.action = "place";
    } else if (result.holdChanged && result.nextAdvanced && last.hold == Cell::Empty) {
        result.action = observed.hold == last.next[0] ? "first_hold" : "place";
    } else if (result.holdChanged && nextSame) {
        result.action = observed.hold == active ? "hold" : "none";
    } else if (result.holdChanged && result.nextAdvanced) {
        result.action = observed.hold == last.next[0] ? "place_and_hold" : "place";
    }
    return result;
}

QueueObservation intermediateQueueAfterOneSlide(const QueueObservation& before,
                                                const QueueObservation& after);

struct QueuePhase {
    double start = 0;
    double end = 0;
    Cell hold = Cell::Empty;
    std::vector<Cell> next;
    Cell active = Cell::Empty;
    std::string action;
    bool queueManuallyFixed = false;
    // The active piece came from HOLD.  This is separate from `action` because
    // the queue/hold animation can be visible several scans before the lock.
    bool holdUsed = false;
};

constexpr std::size_t QueueWindowSize = 5;
constexpr double SpeculativeSeedPenalty = 25.0;

bool stableQueueResyncEnabled() {
    const char* configured = std::getenv("TR_QUEUE_STABLE_RESYNC");
    return !configured || std::string(configured) != "0";
}

// v73: a NEXT column advances by one piece per lock (two only around a first
// HOLD). Over 170k decoded transitions in 23 sets, advances of 3+ came only
// from video defects, cuts, or a frozen mid-slide frame. A resync may jump
// more than this only when enough time passed for that many locks.
int queueResyncMaxQuickAdvance() {
    static const int value = [] {
        const char* v = std::getenv("TR_QUEUE_RESYNC_MAX_ADVANCE");
        return v ? std::max(1, std::atoi(v)) : 2;
    }();
    return value;
}

double queueResyncSecondsPerExtraPiece() {
    static const double value = [] {
        const char* v = std::getenv("TR_QUEUE_RESYNC_SECONDS_PER_PIECE");
        return v ? std::atof(v) : 0.4;
    }();
    return value;
}

// v73: a NEXT run this short (in 100 Hz samples, about one 30 fps frame) is a mid-slide blend far more often
// than a real queue. It is not evidence: skipping it costs nothing, so a
// one-frame misread cannot commit a wrong tail piece.
int queueShortRunSamples() {
    static const int value = [] {
        const char* v = std::getenv("TR_QUEUE_SHORT_RUN_SAMPLES");
        return v ? std::max(0, std::atoi(v)) : 4;
    }();
    return value;
}

// v55: when the HOLD CNN marks a frame unclear, stabilizeQueueCnnObservation
// substitutes the previous trusted HOLD. That substituted value is not image
// evidence; scoring it as such lets a stale HOLD outvote the board (P2's HOLD
// box is unclear ~31% of the time vs ~5% for P1). With this enabled a run is
// HOLD evidence only through its clearly classified samples.
bool holdUnclearNotEvidenceEnabled() {
    const char* configured = std::getenv("TR_HOLD_UNCLEAR_NOT_EVIDENCE");
    return configured && std::string(configured) == "1";
}

bool holdSampleIsEvidence(const QueueRecognitionSample& sample, bool unclearIsEvidence) {
    return sample.manuallyEdited || unclearIsEvidence || !sample.observation.holdUnclear;
}

bool lockBeforeHoldPreferenceEnabled() {
    const char* configured = std::getenv("TR_QUEUE_LOCK_BEFORE_HOLD");
    return !configured || std::string(configured) != "0";
}

bool hasFullQueue(const QueueObservation& queue) {
    return queue.next.size() >= QueueWindowSize &&
           std::all_of(queue.next.begin(), queue.next.begin() + QueueWindowSize, [](Cell cell) {
               return isPiece(cell);
           });
}

std::array<Cell, QueueWindowSize> queueWindow(const QueueObservation& queue) {
    std::array<Cell, QueueWindowSize> result{};
    for (std::size_t i = 0; i < QueueWindowSize && i < queue.next.size(); ++i) result[i] = queue.next[i];
    return result;
}

std::vector<Cell> queuePieces(const std::array<Cell, QueueWindowSize>& window) {
    return {window.begin(), window.end()};
}

std::uint8_t pieceMask(Cell piece) {
    if (!isPiece(piece)) return 0;
    return static_cast<std::uint8_t>(1u << (static_cast<unsigned>(piece) - static_cast<unsigned>(Cell::I)));
}

struct QueueRun {
    std::size_t first = 0;
    std::size_t last = 0;
    QueueObservation observation;
    bool manual = false;
};

struct QueueSequenceNode {
    std::array<Cell, QueueWindowSize> window{};
    // Offset of the next generated tail piece in its 7-bag and the known
    // members of that bag.  The initial partial bag only constrains the
    // pieces visible in the video; every later complete bag is exact.
    std::uint8_t nextBagOffset = 0;
    std::uint8_t currentBagMask = 0;
    double score = 0;
    int parent = -1;
    int event = -1;
    int advance = 0;
    int totalAdvance = 0;
    // Last run that was explained by a real queue transition. Fallback rows
    // intentionally do not move this anchor: a later long, stable window can
    // then repair a bad 7-bag tail correction instead of remaining attached
    // to a stale queue for the rest of the match.
    int lastTrustedEvent = 0;
    bool bagRephase = false;
    // A seed that differs from the first visible queue is only a hypothesis.
    // It becomes trustworthy after a later scan shows the whole hypothesis
    // exactly; otherwise a redraw glitch must not reinforce it.
    bool speculativeSeed = false;
};

double queueEmissionScore(const std::array<Cell, QueueWindowSize>& candidate,
                          const QueueRun& run) {
    const std::size_t observed = std::min<std::size_t>(QueueWindowSize, run.observation.next.size());
    if (observed == 0) return 0;
    const double runWeight = std::min(3.0, 1.0 + std::log2(static_cast<double>(run.last - run.first + 2)));
    double score = 0;
    for (std::size_t i = 0; i < observed; ++i) {
        if (!isPiece(run.observation.next[i])) continue;
        if (candidate[i] == run.observation.next[i]) score += 3.0 * runWeight;
        else score -= 5.0 * runWeight;
    }
    // A row the reviewer changed is a hard annotation, not weak image
    // evidence. The very large mismatch cost keeps it on the decoded path.
    if (run.manual) {
        for (std::size_t i = 0; i < observed; ++i) {
            if (candidate[i] != run.observation.next[i]) return -1000000.0;
        }
    }
    return score;
}

int queueMismatchCount(const std::array<Cell, QueueWindowSize>& candidate,
                       const QueueRun& run) {
    const std::size_t observed = std::min<std::size_t>(QueueWindowSize, run.observation.next.size());
    int mismatches = 0;
    for (std::size_t i = 0; i < observed; ++i) {
        if (isPiece(run.observation.next[i]) && candidate[i] != run.observation.next[i]) ++mismatches;
    }
    return mismatches;
}

bool initializeSevenBag(const std::array<Cell, QueueWindowSize>& window, int phase,
                        bool useSevenBag, std::uint8_t& nextOffset, std::uint8_t& mask) {
    nextOffset = 0;
    mask = 0;
    if (!useSevenBag) return true;
    for (std::size_t i = 0; i < window.size(); ++i) {
        const int offset = (phase + static_cast<int>(i)) % 7;
        if (offset == 0) mask = 0;
        const std::uint8_t bit = pieceMask(window[i]);
        if (bit == 0 || (mask & bit) != 0) return false;
        mask = static_cast<std::uint8_t>(mask | bit);
    }
    nextOffset = static_cast<std::uint8_t>((phase + static_cast<int>(window.size())) % 7);
    if (nextOffset == 0) mask = 0;
    return true;
}

bool appendQueuePiece(QueueSequenceNode& node, Cell piece, bool useSevenBag) {
    if (!isPiece(piece)) return false;
    if (useSevenBag) {
        if (node.nextBagOffset == 0) node.currentBagMask = 0;
        const std::uint8_t bit = pieceMask(piece);
        if ((node.currentBagMask & bit) != 0) return false;
        node.currentBagMask = static_cast<std::uint8_t>(node.currentBagMask | bit);
        node.nextBagOffset = static_cast<std::uint8_t>((node.nextBagOffset + 1) % 7);
        if (node.nextBagOffset == 0) node.currentBagMask = 0;
    }
    std::move(node.window.begin() + 1, node.window.end(), node.window.begin());
    node.window.back() = piece;
    return true;
}

bool sameQueueNodeState(const QueueSequenceNode& a, const QueueSequenceNode& b) {
    return a.window == b.window && a.nextBagOffset == b.nextBagOffset &&
           a.currentBagMask == b.currentBagMask && a.speculativeSeed == b.speculativeSeed &&
           a.lastTrustedEvent == b.lastTrustedEvent;
}

int queueOverlapMismatch(const std::array<Cell, QueueWindowSize>& before,
                         const std::array<Cell, QueueWindowSize>& after,
                         int advance) {
    int mismatches = 0;
    for (std::size_t index = 0;
         index + static_cast<std::size_t>(advance) < QueueWindowSize; ++index) {
        mismatches += before[index + static_cast<std::size_t>(advance)] != after[index];
    }
    return mismatches;
}

bool betterQueueNode(const QueueSequenceNode& a, const QueueSequenceNode& b) {
    if (std::abs(a.score - b.score) > .000001) return a.score > b.score;
    if (a.totalAdvance != b.totalAdvance) return a.totalAdvance < b.totalAdvance;
    return a.advance < b.advance;
}

void pruneQueueBeam(std::vector<QueueSequenceNode>& candidates, int width) {
    std::sort(candidates.begin(), candidates.end(), betterQueueNode);
    std::vector<QueueSequenceNode> unique;
    unique.reserve(std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(width)));
    for (const QueueSequenceNode& candidate : candidates) {
        const bool alreadyPresent = std::any_of(unique.begin(), unique.end(), [&](const QueueSequenceNode& kept) {
            return sameQueueNodeState(candidate, kept);
        });
        if (alreadyPresent) continue;
        unique.push_back(candidate);
        if (unique.size() >= static_cast<std::size_t>(width)) break;
    }
    candidates = std::move(unique);
}

std::vector<QueueRun> queueRunsFromSamples(const std::vector<QueueRecognitionSample>& samples,
                                            std::size_t first) {
    std::vector<QueueRun> runs;
    for (std::size_t i = first; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        const bool split = runs.empty() || sample.manuallyEdited || runs.back().manual ||
                           !samePieces(sample.observation.next, runs.back().observation.next);
        if (split) {
            runs.push_back({i, i, sample.observation, sample.manuallyEdited});
        } else {
            runs.back().last = i;
        }
    }
    return runs;
}

// フレームレートに応じて NEXT 観測の「確定に必要な回数」を調整する。
// queue_confirm は 30fps を基準とした回数なので、60fps では 2 倍必要になる。
static std::atomic<int> gQueueConfirmEffective{0};

int resolveQueueConfirmSamples(const Settings& settings) {
    const int override = gQueueConfirmEffective.load(std::memory_order_relaxed);
    if (override > 0) return override;
    return std::max(1, settings.queueConfirmSamples);
}

void decodeQueueSequence(std::vector<QueueRecognitionSample>& samples, const Settings& settings) {
    struct DecodeSnapshot {
        QueueObservation decoded;
        bool sequenceCorrected = false;
        bool holdCorrected = false;
        bool rejected = false;
    };
    std::vector<DecodeSnapshot> previous(samples.size());
    std::uint64_t newestManualRevision = 0;
    std::size_t newestManualIndex = samples.size();
    std::size_t legacyManualIndex = samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        previous[i] = {samples[i].decoded, samples[i].sequenceCorrected,
                       samples[i].holdCorrected, samples[i].rejected};
        if (samples[i].manuallyEdited) {
            legacyManualIndex = std::min(legacyManualIndex, i);
            if (samples[i].manualRevision > newestManualRevision) {
                newestManualRevision = samples[i].manualRevision;
                newestManualIndex = i;
            } else if (samples[i].manualRevision == newestManualRevision &&
                       samples[i].manualRevision != 0) {
                newestManualIndex = std::min(newestManualIndex, i);
            }
        }
    }
    // An edit is a hard anchor for the row itself. Preserve the already
    // decoded prefix before the newest edit while allowing the suffix to be
    // rebuilt from that anchor. Older files/tests have no revision field, so
    // fall back to the first manual row for those samples.
    std::size_t preserveBefore = newestManualRevision != 0
        ? newestManualIndex : legacyManualIndex;
    // With no manual queue anchor there is no reviewed prefix to freeze.
    // The old samples.size() sentinel accidentally restored every previous
    // decoded row and made an explicit reanalysis appear to do nothing.
    if (preserveBefore == samples.size()) preserveBefore = 0;
    for (auto& sample : samples) {
        sample.decoded = {};
        sample.sequenceCorrected = false;
        sample.holdCorrected = false;
        sample.activeCorrected = false;
        sample.rejected = false;
    }
    const auto first = std::find_if(samples.begin(), samples.end(), [](const QueueRecognitionSample& sample) {
        return hasFullQueue(sample.observation);
    });
    if (first == samples.end()) return;
    const std::size_t firstIndex = static_cast<std::size_t>(std::distance(samples.begin(), first));
    const auto runs = queueRunsFromSamples(samples, firstIndex);
    if (runs.empty()) return;
    std::vector<QueueSequenceNode> seedCandidates;
    const auto initial = queueWindow(runs.front().observation);
    std::array<Cell, QueueWindowSize> candidate = initial;
    const int correctionBudget = runs.front().manual ? 0 : 1;
    const auto generateSeeds = [&](auto&& self, std::size_t slot, int edits) -> void {
        if (slot == candidate.size()) {
            for (int phase = 0; phase < (settings.queueUseSevenBag ? 7 : 1); ++phase) {
                QueueSequenceNode node;
                node.window = candidate;
                if (!initializeSevenBag(node.window, phase, settings.queueUseSevenBag,
                                        node.nextBagOffset, node.currentBagMask)) continue;
                // Do not let a one-slot correction of the first trusted
                // window become a second, self-reinforcing history.  Without
                // this cost, a later redraw glitch can make that speculative
                // seed outscore the exact first observation and permanently
                // move the whole beam onto a different queue.
                node.score = queueEmissionScore(node.window, runs.front()) - edits * SpeculativeSeedPenalty;
                node.event = 0;
                node.lastTrustedEvent = 0;
                node.speculativeSeed = edits > 0;
                seedCandidates.push_back(node);
            }
            return;
        }
        const Cell observed = initial[slot];
        candidate[slot] = observed;
        self(self, slot + 1, edits);
        if (edits >= correctionBudget) return;
        for (Cell piece : Pieces) {
            if (piece == observed) continue;
            candidate[slot] = piece;
            self(self, slot + 1, edits + 1);
        }
        candidate[slot] = observed;
    };
    generateSeeds(generateSeeds, 0, 0);
    pruneQueueBeam(seedCandidates, settings.queueBeamWidth);
    if (seedCandidates.empty()) return;

    std::vector<QueueSequenceNode> nodes;
    nodes.reserve(runs.size() * static_cast<std::size_t>(settings.queueBeamWidth));
    std::vector<int> beam;
    beam.reserve(seedCandidates.size());
    for (const auto& seed : seedCandidates) {
        beam.push_back(static_cast<int>(nodes.size()));
        nodes.push_back(seed);
    }

    for (std::size_t event = 1; event < runs.size(); ++event) {
        std::vector<QueueSequenceNode> candidates;
        candidates.reserve(beam.size() * 16);
        for (const int parentIndex : beam) {
            const QueueSequenceNode& parent = nodes[static_cast<std::size_t>(parentIndex)];
            for (int advance = 0; advance <= 2; ++advance) {
                std::vector<QueueSequenceNode> frontier{parent};
                for (int shift = 0; shift < advance; ++shift) {
                    std::vector<QueueSequenceNode> appended;
                    appended.reserve(frontier.size() * Pieces.size());
                    for (const auto& state : frontier) {
                        for (Cell piece : Pieces) {
                            QueueSequenceNode next = state;
                            if (appendQueuePiece(next, piece, settings.queueUseSevenBag)) appended.push_back(next);
                        }
                    }
                    frontier = std::move(appended);
                    if (frontier.empty()) break;
                }
                for (auto& state : frontier) {
                    // A bag correction may repair a single bad colour class,
                    // but it must not replace most of a visible queue with a
                    // hypothetical 7-bag continuation.  That is how a short
                    // visual effect such as SIIIO used to turn the stable
                    // SJTZL queue into an unrelated TZLIO path.
                    const int mismatchLimit = runs[event].manual ? 0 : 1;
                    if (state.speculativeSeed &&
                        !samePieces(queuePieces(state.window), runs[event].observation.next)) continue;
                    if (queueMismatchCount(state.window, runs[event]) > mismatchLimit) continue;
                    state.parent = parentIndex;
                    state.event = static_cast<int>(event);
                    state.advance = advance;
                    state.totalAdvance = parent.totalAdvance + advance;
                    state.lastTrustedEvent = static_cast<int>(event);
                    state.bagRephase = false;
                    // A real advance is slightly less likely than staying on
                    // the same queue, but image evidence dominates whenever
                    // four/five overlapping slots support a slide.
                    state.score = parent.score + queueEmissionScore(state.window, runs[event])
                                  - (advance == 1 ? .75 : advance == 2 ? 2.25 : 0.0);
                    if (state.speculativeSeed &&
                        samePieces(queuePieces(state.window), runs[event].observation.next)) {
                        state.speculativeSeed = false;
                    }
                    candidates.push_back(std::move(state));
                }
            }
            // A one-slot CNN error at the tail of an otherwise stable window
            // can force the strict 7-bag state onto the wrong phase. The next
            // real slide then becomes impossible even when four visible slots
            // overlap. Recover only when (1) the previously trusted row was
            // actually corrected, (2) the new raw window is stable for at
            // least 100 ms at the 100 Hz queue cadence, and (3) at most one
            // overlap slot disagrees. This is deliberately not a generic
            // reset: ordinary accepted NEXT histories never enter this path.
            const int trustedEvent = parent.lastTrustedEvent;
            const bool hasTrustedEvent = trustedEvent >= 0 &&
                static_cast<std::size_t>(trustedEvent) < event;
            const bool priorWasCorrected = hasTrustedEvent &&
                queueMismatchCount(parent.window, runs[static_cast<std::size_t>(trustedEvent)]) > 0;
            const std::size_t stableSamples = runs[event].last - runs[event].first + 1;
            const double stableGap = hasTrustedEvent
                ? samples[runs[event].first].timeSeconds -
                  samples[runs[static_cast<std::size_t>(trustedEvent)].first].timeSeconds
                : 0.0;
            bool hasExactVisualSlide = false;
            if (hasFullQueue(runs[event].observation)) {
                const auto rawTarget = queueWindow(runs[event].observation);
                for (int advance = 1;
                     advance < static_cast<int>(QueueWindowSize); ++advance) {
                    hasExactVisualSlide = hasExactVisualSlide ||
                        queueOverlapMismatch(parent.window, rawTarget, advance) == 0;
                }
            }
            if (stableQueueResyncEnabled() && !runs[event].manual &&
                settings.queueUseSevenBag &&
                (priorWasCorrected || hasExactVisualSlide) &&
                stableSamples >= 10 && stableGap >= .30 && hasFullQueue(runs[event].observation)) {
                const auto target = queueWindow(runs[event].observation);
                for (int advance = 1; advance <= static_cast<int>(QueueWindowSize); ++advance) {
                    const int overlapMismatch = queueOverlapMismatch(parent.window, target, advance);
                    const int mismatchLimit = priorWasCorrected ? 1 : 0;
                    if (overlapMismatch > mismatchLimit ||
                        (advance == static_cast<int>(QueueWindowSize) &&
                         !priorWasCorrected)) continue;
                    if (advance > queueResyncMaxQuickAdvance() &&
                        stableGap < queueResyncSecondsPerExtraPiece() * advance) continue;
                    for (int phase = 0; phase < 7; ++phase) {
                        QueueSequenceNode reset = parent;
                        reset.window = target;
                        if (!initializeSevenBag(reset.window, phase, true,
                                                reset.nextBagOffset,
                                                reset.currentBagMask)) continue;
                        reset.parent = parentIndex;
                        reset.event = static_cast<int>(event);
                        reset.advance = advance;
                        reset.totalAdvance = parent.totalAdvance + advance;
                        reset.lastTrustedEvent = static_cast<int>(event);
                        reset.bagRephase = true;
                        reset.speculativeSeed = false;
                        reset.score = parent.score + queueEmissionScore(reset.window, runs[event])
                            - 3.0 - advance * .75 - overlapMismatch * 1.5;
                        candidates.push_back(std::move(reset));
                    }
                }
            }
            if (!runs[event].manual) {
                // Keep the last trusted queue when this scan cannot be
                // explained by the bounded legal slide/re-synchronization
                // transitions with at most one visual correction. A later
                // clean scan can still resume the normal transition path.
                QueueSequenceNode fallback = parent;
                fallback.parent = parentIndex;
                fallback.event = static_cast<int>(event);
                fallback.advance = 0;
                fallback.totalAdvance = parent.totalAdvance;
                fallback.bagRephase = false;
                const bool shortRun = runs[event].last - runs[event].first + 1 <=
                                      static_cast<std::size_t>(queueShortRunSamples());
                fallback.score = shortRun
                    ? parent.score
                    : parent.score + queueEmissionScore(parent.window, runs[event]) - 20.0;
                candidates.push_back(std::move(fallback));
            }
        }
        pruneQueueBeam(candidates, settings.queueBeamWidth);
        if (candidates.empty()) break;
        beam.clear();
        beam.reserve(candidates.size());
        for (const auto& state : candidates) {
            beam.push_back(static_cast<int>(nodes.size()));
            nodes.push_back(state);
        }
    }
    if (beam.empty()) return;

    std::vector<std::array<Cell, QueueWindowSize>> decoded(runs.size());
    int nodeIndex = beam.front();
    while (nodeIndex >= 0) {
        const auto& node = nodes[static_cast<std::size_t>(nodeIndex)];
        if (node.bagRephase && std::getenv("TR_QUEUE_RESYNC_DIAGNOSTICS")) {
            const auto& run = runs[static_cast<std::size_t>(node.event)];
            std::cerr << "queue_resync event=" << node.event
                      << " time=" << samples[run.first].timeSeconds
                      << " advance=" << node.advance << " window=";
            for (const Cell piece : node.window) std::cerr << cellChar(piece);
            std::cerr << '\n';
        }
        if (node.event >= 0 && static_cast<std::size_t>(node.event) < decoded.size()) {
            decoded[static_cast<std::size_t>(node.event)] = node.window;
        }
        nodeIndex = node.parent;
    }
    // First materialize the complete-history queue path. HOLD validation must
    // consume this logical path rather than the raw colour-classifier window;
    // otherwise a simultaneous tail/head redraw can masquerade as a queue
    // transition and make a transient HOLD value durable.
    for (std::size_t event = 0; event < runs.size(); ++event) {
        const auto correctedNext = queuePieces(decoded[event]);
        for (std::size_t i = runs[event].first; i <= runs[event].last; ++i) {
            auto& sample = samples[i];
            sample.decoded = sample.observation;
            sample.decoded.next = correctedNext;
            sample.sequenceCorrected = !samePieces(sample.observation.next, correctedNext);
            const int mismatchLimit = runs[event].manual ? 0 : 1;
            sample.rejected = !validQueue(sample.observation) ||
                              queueMismatchCount(decoded[event], runs[event]) > mismatchLimit;
        }
    }
    for (std::size_t i = 0; i < preserveBefore && i < samples.size(); ++i) {
        // If this prefix did not have a previous decoded queue (for example a
        // freshly constructed test/browser row), keep the normal result.
        if (previous[i].decoded.next.empty()) continue;
        samples[i].decoded = previous[i].decoded;
        samples[i].sequenceCorrected = previous[i].sequenceCorrected;
        samples[i].holdCorrected = previous[i].holdCorrected;
        samples[i].rejected = previous[i].rejected;
    }
}

class QueuePhaseBuilder {
public:
    explicit QueuePhaseBuilder(int confirmSamples, double startSeconds = 0)
        : confirmSamples_(confirmSamples), startSeconds_(startSeconds),
          currentStart_(startSeconds) {}

    void push(double time, const QueueObservation& observed) {
        const bool wasFirstDetected = firstDetected_;
        QueueTransition possible;
        if (wasFirstDetected) {
            possible = checkQueueTransition(observed, lastStable_, active_);
            const bool isStableQueue = sameQueue(observed, lastStable_) ||
                                       (possible.plausible && possible.action == "none" && possible.holdChanged == false);
            if (!isStableQueue && !possible.plausible) {
                // Keep the raw row for later review, but do not let an
                // impossible recognition enter the confirmation counter.
                QueueRecognitionSample rejected{time, active_, observed, false, false};
                rejected.rejected = true;
                rawSamples_.push_back(std::move(rejected));
                return;
            }
        }
        if (sameQueue(observed, pending_)) {
            ++pendingCount_;
        } else {
            pending_ = observed;
            pendingCount_ = 1;
        }
        rawSamples_.push_back({time, active_, observed, pendingCount_ >= confirmSamples_, false});
        if (pendingCount_ < confirmSamples_) return;

        if (!firstDetected_ && validQueue(observed)) {
            lastStable_ = observed;
            active_ = Cell::Empty;
            phases_.push_back({startSeconds_, time, Cell::Empty, observed.next,
                               Cell::Empty, "init", false});
            currentStart_ = time;
            activeFromHold_ = false;
            firstDetected_ = true;
            return;
        }
        if (!firstDetected_) return;

        QueueTransition transition = checkQueueTransition(observed, lastStable_, active_);
        if (transition.action == "spawn" || transition.action == "first_hold") {
            if (transition.slideCount >= 2 && lastStable_.next.size() >= 2) {
                const QueueObservation intermediate = intermediateQueueAfterOneSlide(lastStable_, observed);
                phases_.push_back({time, time, lastStable_.hold, intermediate.next,
                                   lastStable_.next[0], "spawn_skipped", false});
                active_ = lastStable_.next[1];
                activeFromHold_ = false;
            } else if (transition.action == "first_hold") {
                // The current piece went into an empty HOLD slot.  The queue
                // has advanced, so the newly controlled piece is now the
                // first item of the observed queue.
                active_ = observed.next.empty() ? Cell::Empty : observed.next[0];
                activeFromHold_ = true;
            } else if (activeFromHold_) {
                // Some videos show the HOLD change one scan before the queue
                // advances.  In that case lastStable_.next[0] is still the
                // newly spawned piece; keep the pending HOLD provenance when
                // it becomes active.
                active_ = lastStable_.next.empty() ? Cell::Empty : lastStable_.next[0];
            } else {
                active_ = lastStable_.next[0];
                activeFromHold_ = false;
            }
        } else if (transition.action == "hold") {
            // An empty HOLD slot does not provide a replacement active piece
            // yet.  Keep the hold provenance pending until the delayed queue
            // slide exposes the next active piece.
            active_ = lastStable_.hold == Cell::Empty ? Cell::Empty : lastStable_.hold;
            activeFromHold_ = true;
        } else if (transition.action == "place" || transition.action == "place_and_hold") {
            phases_.push_back({currentStart_, time, lastStable_.hold, lastStable_.next, active_, transition.action,
                               false, activeFromHold_ || transition.action == "place_and_hold"});
            currentStart_ = time;
            if (transition.slideCount >= 2 && lastStable_.next.size() >= 2) {
                const QueueObservation intermediate = intermediateQueueAfterOneSlide(lastStable_, observed);
                phases_.push_back({time, time, lastStable_.hold, intermediate.next,
                                   lastStable_.next[0], "place_skipped", false});
                active_ = lastStable_.next[1];
                activeFromHold_ = false;
            } else if (transition.action == "place_and_hold") {
                // The queue head was moved into HOLD while the old active
                // piece was locked.  The old HOLD piece is now active.
                active_ = lastStable_.hold;
                activeFromHold_ = true;
            } else {
                active_ = lastStable_.next[0];
                activeFromHold_ = false;
            }
        }
        // A HOLD-only change is a state transition only when it produced a
        // legal action. If ACTIVE/NEXT do not support that action, retain the
        // last logical HOLD; otherwise a classifier glitch can become durable
        // even after the complete-history queue decoder rejected it.
        if (transition.plausible &&
            (transition.nextAdvanced ||
             (transition.holdChanged && transition.action != "none"))) {
            lastStable_ = observed;
        }
        // The active mino may change while processing a stable transition, so
        // store the post-transition internal state on this exact scan row.
        rawSamples_.back().active = active_;
    }

    std::vector<QueuePhase> finish(double duration) const {
        auto phases = phases_;
        if (currentStart_ < duration) {
            phases.push_back({currentStart_, duration, lastStable_.hold, lastStable_.next, active_, "end", false,
                              activeFromHold_});
        }
        for (auto& phase : phases) {
            phase.queueManuallyFixed = std::any_of(rawSamples_.begin(), rawSamples_.end(), [&](const QueueRecognitionSample& sample) {
                return sample.manuallyEdited && sample.timeSeconds >= phase.start - .000001 &&
                       sample.timeSeconds <= phase.end + .000001;
            });
        }
        return phases;
    }

    // Scheduling board samples during automatic video analysis only needs
    // the phase boundaries accumulated so far. Unlike finish(), this preview
    // deliberately skips the manual-edit scan over every historical sample;
    // the authoritative global decoder still runs once at the end and owns
    // the exported result.
    std::vector<QueuePhase> preview(double duration) const {
        auto phases = phases_;
        if (currentStart_ < duration) {
            phases.push_back({currentStart_, duration, lastStable_.hold, lastStable_.next,
                              active_, "end", false, activeFromHold_});
        }
        return phases;
    }

    const std::vector<QueueRecognitionSample>& rawSamples() const { return rawSamples_; }

private:
    int confirmSamples_ = 2;
    double startSeconds_ = 0;
    QueueObservation pending_;
    int pendingCount_ = 0;
    QueueObservation lastStable_;
    Cell active_ = Cell::Empty;
    bool activeFromHold_ = false;
    double currentStart_ = 0;
    bool firstDetected_ = false;
    std::vector<QueuePhase> phases_;
    std::vector<QueueRecognitionSample> rawSamples_;
};

enum class LogicalQueueOperation {
    Spawn,
    Hold,
    HoldPending,
    Lock,
};

struct LogicalQueueState {
    Cell active = Cell::Empty;
    Cell hold = Cell::Empty;
    std::array<Cell, QueueWindowSize> queue{};
    bool canHold = true;
    bool activeFromHold = false;
    // HOLD is visible before the NEXT animation has shifted.  The active
    // replacement is resolved from queue[0] when that delayed slide arrives.
    bool pendingFirstHold = false;
};

struct LogicalQueueRun {
    std::size_t first = 0;
    std::size_t last = 0;
    std::size_t confirmed = 0;
    std::array<Cell, QueueWindowSize> queue{};
    Cell observedHold = Cell::Empty;
    bool holdIsAnchor = false;
    bool holdEvidenceReliable = true;
    // Samples whose HOLD was actually classified (not carried forward).
    std::size_t holdEvidenceSamples = 0;
    bool weightByEvidenceSamples = false;
};

struct LogicalQueueNode {
    LogicalQueueState state;
    double score = 0;
    int parent = -1;
    int event = -1;
    int queueAdvance = 0;
    int totalHolds = 0;
    std::vector<LogicalQueueOperation> operations;
    // Distinct operation histories can rejoin the exact same ACTIVE/HOLD/NEXT
    // state while having locked a different sequence (for example IT versus
    // HOLD,T,lock,HOLD,I,lock = TI). Keep that distinction until board
    // evidence has had a chance to choose between them.
    std::string lockedSignature;
};

bool sameLogicalQueueState(const LogicalQueueState& left, const LogicalQueueState& right) {
    return left.active == right.active && left.hold == right.hold &&
           left.queue == right.queue && left.canHold == right.canHold &&
           left.activeFromHold == right.activeFromHold &&
           left.pendingFirstHold == right.pendingFirstHold;
}

int exactQueueAdvance(const std::array<Cell, QueueWindowSize>& before,
                      const std::array<Cell, QueueWindowSize>& after) {
    if (before == after) return 0;
    // Three or more pieces can pass while a full-screen effect obscures NEXT.
    // The observation decoder emits such a jump only through its tightly
    // gated stable-window resynchronizer, so the logical layer must be able
    // to replay the same bounded jump.
    const int maximumAdvance = stableQueueResyncEnabled()
        ? static_cast<int>(QueueWindowSize) : 2;
    for (int advance = 1; advance <= maximumAdvance; ++advance) {
        bool matches = true;
        for (std::size_t i = 0; i + static_cast<std::size_t>(advance) < QueueWindowSize; ++i) {
            if (after[i] != before[i + static_cast<std::size_t>(advance)]) {
                matches = false;
                break;
            }
        }
        if (matches) return advance;
    }
    return -1;
}

Cell logicalHoldEvidence(const QueueRecognitionSample& sample) {
    // HOLD is decoded again as part of the complete legal state path.  The
    // holdCorrected flag can come from an earlier automatic preparation pass,
    // so treating it as a hard anchor would make a first-pass correction
    // undo itself on the second pass. Only explicit manual rows are anchors.
    return sample.observation.hold;
}

std::vector<LogicalQueueRun> logicalQueueRuns(const std::vector<QueueRecognitionSample>& samples,
                                              int confirmSamples, bool unclearHoldIsEvidence) {
    std::vector<LogicalQueueRun> result;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        if (!hasFullQueue(sample.decoded)) continue;
        const auto queue = queueWindow(sample.decoded);
        const Cell hold = logicalHoldEvidence(sample);
        const bool anchor = sample.manuallyEdited;
        const bool reliableHold = holdSampleIsEvidence(sample, unclearHoldIsEvidence);
        const bool split = result.empty() || result.back().queue != queue ||
                           result.back().observedHold != hold || result.back().holdIsAnchor || anchor;
        if (split) {
            result.push_back({i, i, i, queue, hold, anchor, reliableHold,
                              static_cast<std::size_t>(reliableHold ? 1 : 0),
                              !unclearHoldIsEvidence});
        } else {
            result.back().last = i;
            result.back().holdEvidenceReliable = result.back().holdEvidenceReliable || reliableHold;
            if (reliableHold) ++result.back().holdEvidenceSamples;
        }
    }
    for (auto& run : result) {
        run.weightByEvidenceSamples = !unclearHoldIsEvidence;
        const std::size_t needed = static_cast<std::size_t>(std::max(1, confirmSamples) - 1);
        run.confirmed = run.holdIsAnchor ? run.first : std::min(run.last, run.first + needed);
    }
    // Puyo Puyo Tetris 2 does not redraw HOLD and NEXT atomically. A buffered
    // HOLD immediately after a lock can update the HOLD box a few scans before
    // NEXT slides. If that short HOLD-only run is scored as a complete event,
    // the logical decoder commits to HOLD->lock even though the ordinary game
    // order was lock->HOLD (and therefore reports the wrong placed piece).
    //
    // Defer only this tightly bounded, non-manual leading edge. The following
    // advancing run still carries the same HOLD observation, so no image
    // evidence is discarded; it is merely evaluated together with the queue
    // advance, where logicalOperationOrderScore can choose the legal order.
    constexpr double HoldLeadingQueueMaximumSeconds = .060001;
    constexpr double HoldLeadingMinimumControlledSeconds = .25;
    for (std::size_t i = 1; i + 1 < result.size(); ++i) {
        auto& run = result[i];
        const auto& previous = result[i - 1];
        const auto& next = result[i + 1];
        if (previous.holdIsAnchor || run.holdIsAnchor || next.holdIsAnchor ||
            !isPiece(previous.observedHold) || !isPiece(run.observedHold) ||
            run.queue != previous.queue ||
            run.observedHold == previous.observedHold ||
            next.observedHold != run.observedHold ||
            exactQueueAdvance(run.queue, next.queue) <= 0) {
            continue;
        }
        const double leadSeconds = samples[next.confirmed].timeSeconds -
                                   samples[run.confirmed].timeSeconds;
        const double controlledSeconds = samples[run.confirmed].timeSeconds -
                                         samples[previous.confirmed].timeSeconds;
        if (std::getenv("TR_QUEUE_HOLD_LEAD_DIAGNOSTICS")) {
            std::cerr << "hold_lead_candidate time="
                      << samples[run.confirmed].timeSeconds
                      << " next=" << samples[next.confirmed].timeSeconds
                      << " lead=" << leadSeconds
                      << " controlled=" << controlledSeconds
                      << " previous_hold=" << cellChar(previous.observedHold)
                      << " new_hold=" << cellChar(run.observedHold)
                      << " active_first=" << cellChar(samples[next.first].active)
                      << " active_confirmed=" << cellChar(samples[next.confirmed].active)
                      << " active_last=" << cellChar(samples[next.last].active) << '\n';
        }
        // A deliberate spawn HOLD followed by an immediate hard drop produces
        // the same two UI edges in the opposite order. Do not reverse such a
        // fast action; the lock->buffered-HOLD interpretation is eligible only
        // after the outgoing piece was controlled for a meaningful interval.
        if (controlledSeconds >= HoldLeadingMinimumControlledSeconds &&
            leadSeconds >= 0 && leadSeconds <= HoldLeadingQueueMaximumSeconds) {
            run.holdEvidenceReliable = false;
        }
    }
    // At the first frame of a fresh recording, HOLD and NEXT[0] can both be
    // classified as the same bright active-piece effect.  That cannot prove
    // a pre-existing HOLD: there has not yet been a queue spawn.  More
    // importantly, an unchanged copy of that same pixels is not a later HOLD
    // event.  Keep it untrusted until the HOLD image actually changes.
    if (!result.empty()) {
        const auto& firstSample = samples[result.front().first];
        const Cell suspicious = result.front().observedHold;
        const bool startsWithDuplicatedHead = firstSample.timeSeconds <= .050001 &&
            isPiece(suspicious) && suspicious == result.front().queue.front() &&
            !result.front().holdIsAnchor;
        if (startsWithDuplicatedHead) {
            for (auto& run : result) {
                if (run.observedHold != suspicious || run.holdIsAnchor) break;
                run.holdEvidenceReliable = false;
            }
        }
    }
    return result;
}

void shiftLogicalQueue(LogicalQueueState& state,
                       const std::array<Cell, QueueWindowSize>& target,
                       int totalAdvance, int alreadyConsumed) {
    std::move(state.queue.begin() + 1, state.queue.end(), state.queue.begin());
    const int tail = static_cast<int>(QueueWindowSize) - totalAdvance + alreadyConsumed;
    state.queue.back() = target[static_cast<std::size_t>(tail)];
}

void enumerateLogicalTransitions(const LogicalQueueState& initial,
                                 const std::array<Cell, QueueWindowSize>& target,
                                 int requiredAdvance,
                                 std::vector<std::pair<LogicalQueueState,
                                                       std::vector<LogicalQueueOperation>>>& output) {
    const int maxOperations = requiredAdvance * 2 + 2;
    std::function<void(LogicalQueueState, int, std::vector<LogicalQueueOperation>)> visit;
    visit = [&](LogicalQueueState state, int consumed,
                std::vector<LogicalQueueOperation> operations) {
        if (consumed == requiredAdvance && state.queue == target) {
            output.push_back({state, operations});
        }
        if (static_cast<int>(operations.size()) >= maxOperations) return;

        if (isPiece(state.active) && state.canHold) {
            LogicalQueueState held = state;
            if (isPiece(held.hold)) {
                std::swap(held.active, held.hold);
                held.canHold = false;
                held.activeFromHold = true;
                auto nextOperations = operations;
                nextOperations.push_back(LogicalQueueOperation::Hold);
                visit(held, consumed, std::move(nextOperations));
            } else if (consumed < requiredAdvance) {
                held.hold = held.active;
                held.active = held.queue.front();
                shiftLogicalQueue(held, target, requiredAdvance, consumed);
                held.canHold = false;
                held.activeFromHold = true;
                held.pendingFirstHold = false;
                auto nextOperations = operations;
                nextOperations.push_back(LogicalQueueOperation::Hold);
                visit(held, consumed + 1, std::move(nextOperations));
            }
            if (state.hold == Cell::Empty) {
                // The UI can draw the new HOLD one scan before NEXT moves.
                // Record that as one pending legal operation instead of
                // inventing an older hidden HOLD to force both observations
                // into the same instant.
                LogicalQueueState pending = state;
                pending.hold = pending.active;
                pending.active = Cell::Empty;
                pending.canHold = false;
                pending.activeFromHold = true;
                pending.pendingFirstHold = true;
                auto nextOperations = operations;
                nextOperations.push_back(LogicalQueueOperation::HoldPending);
                visit(pending, consumed, std::move(nextOperations));
            }
        }

        if (consumed >= requiredAdvance) return;
        LogicalQueueState advanced = state;
        auto nextOperations = operations;
        const bool completesPendingHold = advanced.pendingFirstHold;
        if (isPiece(advanced.active)) {
            nextOperations.push_back(LogicalQueueOperation::Lock);
        } else {
            nextOperations.push_back(LogicalQueueOperation::Spawn);
        }
        advanced.active = advanced.queue.front();
        shiftLogicalQueue(advanced, target, requiredAdvance, consumed);
        advanced.canHold = !completesPendingHold;
        advanced.activeFromHold = completesPendingHold;
        advanced.pendingFirstHold = false;
        visit(advanced, consumed + 1, std::move(nextOperations));
    };
    visit(initial, 0, {});
}

double logicalHoldScore(Cell logical, const LogicalQueueRun& run) {
    if (run.holdIsAnchor && logical != run.observedHold) return -1000000.0;
    if (!run.holdEvidenceReliable) return 0;
    const double length = run.weightByEvidenceSamples
        ? static_cast<double>(std::max<std::size_t>(1, run.holdEvidenceSamples))
        : static_cast<double>(run.last - run.first + 1);
    const double weight = std::min(6.0, 1.0 + std::log2(length + 1.0));
    return logical == run.observedHold ? 4.0 * weight : -4.0 * weight;
}

double logicalOperationOrderScore(
    const std::vector<LogicalQueueOperation>& operations) {
    if (!lockBeforeHoldPreferenceEnabled()) return 0;
    const auto lock = std::find(
        operations.begin(), operations.end(), LogicalQueueOperation::Lock);
    const auto hold = std::find(
        operations.begin(), operations.end(), LogicalQueueOperation::Hold);
    if (lock == operations.end() || hold == operations.end()) return 0;
    // When a lock and a non-empty HOLD swap land in the same 10 ms queue
    // scan, both orders have the same final HOLD/NEXT state. HOLD->lock would
    // require dropping the swapped-in piece within that same scan; lock->HOLD
    // is the game's ordinary buffered spawn action. This tiny tie-break never
    // overrides image evidence and only chooses between the identical final
    // logical states.
    return lock < hold ? .20 : -.20;
}

void pruneLogicalQueueBeam(std::vector<LogicalQueueNode>& candidates, int width) {
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        if (std::abs(left.score - right.score) > .000001) return left.score > right.score;
        return left.totalHolds < right.totalHolds;
    });
    std::vector<LogicalQueueNode> unique;
    unique.reserve(std::min<std::size_t>(candidates.size(), static_cast<std::size_t>(width)));
    for (auto& candidate : candidates) {
        const bool found = std::any_of(unique.begin(), unique.end(), [&](const auto& kept) {
            return sameLogicalQueueState(candidate.state, kept.state);
        });
        if (found) continue;
        unique.push_back(std::move(candidate));
        if (unique.size() >= static_cast<std::size_t>(width)) break;
    }
    candidates = std::move(unique);
}

LogicalQueueState applyLogicalOperation(LogicalQueueState state,
                                        LogicalQueueOperation operation,
                                        const std::array<Cell, QueueWindowSize>& target,
                                        int totalAdvance, int& consumed) {
    if (operation == LogicalQueueOperation::HoldPending) {
        state.hold = state.active;
        state.active = Cell::Empty;
        state.canHold = false;
        state.activeFromHold = true;
        state.pendingFirstHold = true;
        return state;
    }
    if (operation == LogicalQueueOperation::Hold) {
        if (isPiece(state.hold)) {
            std::swap(state.active, state.hold);
        } else {
            state.hold = state.active;
            state.active = state.queue.front();
            shiftLogicalQueue(state, target, totalAdvance, consumed++);
            state.pendingFirstHold = false;
        }
        state.canHold = false;
        state.activeFromHold = true;
        return state;
    }
    const bool completesPendingHold = state.pendingFirstHold;
    state.active = state.queue.front();
    shiftLogicalQueue(state, target, totalAdvance, consumed++);
    state.canHold = !completesPendingHold;
    state.activeFromHold = completesPendingHold;
    state.pendingFirstHold = false;
    return state;
}

std::string logicalLockedPieces(
        LogicalQueueState state,
        const std::vector<LogicalQueueOperation>& operations,
        const std::array<Cell, QueueWindowSize>& target,
        int totalAdvance) {
    std::string result;
    int consumed = 0;
    for (const LogicalQueueOperation operation : operations) {
        if (operation == LogicalQueueOperation::Lock) {
            result.push_back(isPiece(state.active) ? cellChar(state.active) : '-');
        }
        state = applyLogicalOperation(
            state, operation, target, totalAdvance, consumed);
    }
    return result;
}

std::vector<QueuePhase> decodeLogicalQueuePhases(
    std::vector<QueueRecognitionSample>& samples,
    const Settings& settings, double duration, double startSeconds = 0,
    std::vector<std::vector<QueuePhase>>* alternativePhases = nullptr) {
    const auto runs = logicalQueueRuns(
        samples, resolveQueueConfirmSamples(settings),
        settings.queueUnclearHoldIsEvidence && !holdUnclearNotEvidenceEnabled());
    if (runs.empty()) return {};

    LogicalQueueNode seed;
    seed.state.queue = runs.front().queue;
    seed.state.hold = runs.front().holdEvidenceReliable
        ? runs.front().observedHold : Cell::Empty;
    // NEXT can remain unchanged for a few scans after the first piece enters
    // HOLD. Infer that initial active piece only for a confirmed new match,
    // with empty-HOLD evidence before and persistent occupied HOLD afterward.
    // One startup-empty sample is not evidence: a late clip can already have
    // an occupied HOLD before the recognizer initializes (first50 #30/#34).
    const bool openingEmptyHoldConfirmed = runs.front().holdEvidenceReliable &&
        runs.front().observedHold == Cell::Empty &&
        runs.front().last - runs.front().first + 1 >=
            static_cast<std::size_t>(resolveQueueConfirmSamples(settings));
    if (settings.forceEmptyOpening && seed.state.hold == Cell::Empty &&
        openingEmptyHoldConfirmed && !std::getenv("TR_DISABLE_OPENING_HOLD_SEED")) {
        for (std::size_t openingRun = 1; openingRun < runs.size(); ++openingRun) {
            const auto& run = runs[openingRun];
            if (run.queue != seed.state.queue) break;
            if (!run.holdEvidenceReliable || !isPiece(run.observedHold)) continue;
            std::size_t evidence = 0;
            bool sustained = false;
            for (std::size_t evidenceRun = openingRun;
                 evidenceRun < runs.size(); ++evidenceRun) {
                const auto& nextRun = runs[evidenceRun];
                if (!nextRun.holdEvidenceReliable ||
                    nextRun.observedHold != run.observedHold) break;
                evidence += nextRun.last - nextRun.first + 1;
                if (nextRun.queue != seed.state.queue) {
                    sustained = exactQueueAdvance(seed.state.queue, nextRun.queue) == 1 &&
                        evidence >= static_cast<std::size_t>(resolveQueueConfirmSamples(settings));
                    break;
                }
            }
            if (!sustained) continue;
            seed.state.active = run.observedHold;
            std::cerr << "opening_active_from_hold=" << cellChar(seed.state.active)
                      << " time=" << samples[run.confirmed].timeSeconds << '\n';
            break;
        }
    }
    seed.score = logicalHoldScore(seed.state.hold, runs.front());
    seed.event = 0;
    std::vector<LogicalQueueNode> nodes{seed};
    std::vector<int> beam{0};
    struct RejoinedLogicalPath {
        std::size_t event = 0;
        LogicalQueueNode node;
    };
    std::vector<RejoinedLogicalPath> rejoinedPaths;

    for (std::size_t event = 1; event < runs.size(); ++event) {
        std::vector<LogicalQueueNode> candidates;
        for (const int parentIndex : beam) {
            const auto& parent = nodes[static_cast<std::size_t>(parentIndex)];
            const int advance = exactQueueAdvance(parent.state.queue, runs[event].queue);
            if (advance < 0) continue;
            std::vector<std::pair<LogicalQueueState, std::vector<LogicalQueueOperation>>> transitions;
            enumerateLogicalTransitions(parent.state, runs[event].queue, advance, transitions);
            for (auto& transition : transitions) {
                const double emission = logicalHoldScore(transition.first.hold, runs[event]);
                if (emission <= -999999.0) continue;
                const int holds = static_cast<int>(std::count_if(
                    transition.second.begin(), transition.second.end(), [](const auto operation) {
                        return operation == LogicalQueueOperation::Hold ||
                               operation == LogicalQueueOperation::HoldPending;
                    }));
                LogicalQueueNode node;
                node.state = transition.first;
                node.score = parent.score + emission - holds * .35 +
                             logicalOperationOrderScore(transition.second);
                node.parent = parentIndex;
                node.event = static_cast<int>(event);
                node.queueAdvance = advance;
                node.totalHolds = parent.totalHolds + holds;
                if (alternativePhases) {
                    node.lockedSignature = parent.lockedSignature;
                    node.lockedSignature.push_back('|');
                    node.lockedSignature += logicalLockedPieces(
                        parent.state, transition.second, runs[event].queue,
                        advance);
                }
                node.operations = std::move(transition.second);
                candidates.push_back(std::move(node));
            }
        }
        if (alternativePhases && rejoinedPaths.size() < 1024) {
            std::vector<std::size_t> ranking(candidates.size());
            std::iota(ranking.begin(), ranking.end(), 0);
            std::stable_sort(ranking.begin(), ranking.end(), [&](std::size_t left,
                                                                 std::size_t right) {
                if (std::abs(candidates[left].score - candidates[right].score) > .000001) {
                    return candidates[left].score > candidates[right].score;
                }
                return candidates[left].totalHolds < candidates[right].totalHolds;
            });
            std::vector<std::size_t> leaders;
            std::vector<int> variants;
            for (const std::size_t candidateIndex : ranking) {
                const auto& candidate = candidates[candidateIndex];
                std::size_t group = leaders.size();
                for (std::size_t index = 0; index < leaders.size(); ++index) {
                    if (sameLogicalQueueState(
                            candidate.state, candidates[leaders[index]].state)) {
                        group = index;
                        break;
                    }
                }
                if (group == leaders.size()) {
                    leaders.push_back(candidateIndex);
                    variants.push_back(0);
                    continue;
                }
                const auto& leader = candidates[leaders[group]];
                // A confident but wrong HOLD class can be roughly 48 score
                // points better than the physically correct branch for one
                // run. The whole purpose of this fallback is to let board
                // evidence overrule exactly that case, so retain a few much
                // lower queue-only branches after they legally rejoin.
                if (variants[group] >= 8 ||
                    leader.score - candidate.score > 100.000001 ||
                    leader.lockedSignature == candidate.lockedSignature) {
                    continue;
                }
                rejoinedPaths.push_back({event, candidate});
                ++variants[group];
                if (rejoinedPaths.size() >= 1024) break;
            }
        }
        pruneLogicalQueueBeam(candidates, std::max(32, settings.queueBeamWidth));
        if (candidates.empty()) return {};
        beam.clear();
        for (auto& candidate : candidates) {
            beam.push_back(static_cast<int>(nodes.size()));
            nodes.push_back(std::move(candidate));
        }
    }

    auto annotateRun = [&](std::size_t event, const LogicalQueueState& before,
                           const LogicalQueueState& after) {
        const auto& run = runs[event];
        for (std::size_t i = run.first; i <= run.last; ++i) {
            const LogicalQueueState& state = i < run.confirmed ? before : after;
            auto& sample = samples[i];
            if (!sample.manuallyEdited) sample.active = state.active;
            sample.stable = i >= run.confirmed;
            sample.decoded.hold = state.hold;
            sample.holdCorrected = sample.observation.hold != state.hold;
        }
    };

    const auto nodePath = [&](int finalNode) {
        std::vector<int> path(runs.size(), -1);
        int nodeIndex = finalNode;
        while (nodeIndex >= 0) {
            const auto& node = nodes[static_cast<std::size_t>(nodeIndex)];
            path[static_cast<std::size_t>(node.event)] = nodeIndex;
            nodeIndex = node.parent;
        }
        return path;
    };

    const auto buildPath = [&](const std::vector<int>& path, bool annotate) {
        if (std::any_of(path.begin(), path.end(),
                        [](int value) { return value < 0; })) {
            return std::vector<QueuePhase>{};
        }

        // フェーズ境界を「変化を最初に観測したサンプル」に置くか、
        // 「確定したサンプル」に置くか。既定は最初の観測（実際のスライドに近い）。
        const bool boundaryAtFirst = [] {
            const char* v = std::getenv("TR_QUEUE_BOUNDARY");
            return v && std::string(v) == "first";
        }();
        const double firstTime = samples[boundaryAtFirst ? runs.front().first
                                                         : runs.front().confirmed].timeSeconds;
        LogicalQueueState replay =
            nodes[static_cast<std::size_t>(path.front())].state;
        if (annotate) annotateRun(0, replay, replay);
        std::vector<QueuePhase> phases;
        phases.push_back({startSeconds, firstTime, replay.hold,
                          queuePieces(replay.queue), Cell::Empty, "init",
                          false, false});
        double currentStart = firstTime;

        for (std::size_t event = 1; event < runs.size(); ++event) {
            const auto before = replay;
            const auto& node = nodes[static_cast<std::size_t>(path[event])];
            const double time = samples[boundaryAtFirst ? runs[event].first
                                                        : runs[event].confirmed].timeSeconds;
            int consumed = 0;
            bool spawnedInThisEvent = false;
            int locksInThisEvent = 0;
            for (const auto operation : node.operations) {
                if (operation == LogicalQueueOperation::Lock) {
                    std::string action = "place";
                    if (spawnedInThisEvent && currentStart == time) {
                        action = "spawn_skipped";
                    } else if (locksInThisEvent > 0 && currentStart == time) {
                        action = "place_skipped";
                    }
                    phases.push_back({
                        currentStart, time, replay.hold,
                        queuePieces(replay.queue), replay.active, action,
                        false, replay.activeFromHold});
                    currentStart = time;
                    ++locksInThisEvent;
                } else if (operation == LogicalQueueOperation::Spawn) {
                    spawnedInThisEvent = true;
                    if (!replay.pendingFirstHold) currentStart = time;
                }
                replay = applyLogicalOperation(
                    replay, operation, runs[event].queue,
                    node.queueAdvance, consumed);
            }
            if (annotate) annotateRun(event, before, replay);
        }

        if (currentStart < duration || phases.empty()) {
            phases.push_back({
                currentStart, duration, replay.hold, queuePieces(replay.queue),
                replay.active, "end", false, replay.activeFromHold});
        }
        return phases;
    };

    const std::vector<int> selectedPath = nodePath(beam.front());
    std::vector<QueuePhase> selected = buildPath(selectedPath, true);
    if (alternativePhases) {
        alternativePhases->clear();
        std::set<std::string> signatures;
        const bool diagnostics =
            std::getenv("TR_QUEUE_LOGICAL_NBEST_DIAGNOSTICS") != nullptr;
        if (diagnostics) {
            std::cerr << "logical_nbest_final_nodes=" << beam.size() << '\n';
        }
        const auto appendAlternative = [&](const std::vector<int>& path,
                                           const std::string& signature,
                                           double score, int holds) {
            if (!signatures.insert(signature).second) return;
            if (diagnostics) {
                std::cerr << "logical_nbest_candidate="
                          << (alternativePhases->size() + 1)
                          << " score=" << score
                          << " holds=" << holds
                          << " locks=" << signature << '\n';
            }
            std::vector<QueuePhase> candidate = buildPath(path, false);
            if (candidate.empty()) return;
            alternativePhases->push_back(std::move(candidate));
        };
        const auto& selectedNode = nodes[static_cast<std::size_t>(beam.front())];
        appendAlternative(selectedPath, selectedNode.lockedSignature,
                          selectedNode.score, selectedNode.totalHolds);
        for (const auto& rejoined : rejoinedPaths) {
            if (alternativePhases->size() >= 512 ||
                rejoined.event >= selectedPath.size()) break;
            const int selectedAtEvent = selectedPath[rejoined.event];
            if (selectedAtEvent < 0 || !sameLogicalQueueState(
                    rejoined.node.state,
                    nodes[static_cast<std::size_t>(selectedAtEvent)].state)) {
                continue;
            }
            const int alternateIndex = static_cast<int>(nodes.size());
            nodes.push_back(rejoined.node);
            std::vector<int> alternatePath = selectedPath;
            int cursor = alternateIndex;
            while (cursor >= 0) {
                const auto& node = nodes[static_cast<std::size_t>(cursor)];
                if (node.event > static_cast<int>(rejoined.event)) break;
                alternatePath[static_cast<std::size_t>(node.event)] = cursor;
                cursor = node.parent;
            }
            appendAlternative(alternatePath, rejoined.node.lockedSignature,
                              rejoined.node.score, rejoined.node.totalHolds);
        }
    }
    return selected;
}

std::vector<QueuePhase> buildQueuePhasesFromSamples(std::vector<QueueRecognitionSample>& samples,
                                                    const Settings& settings, double duration,
                                                    double startSeconds = 0,
                                                    std::vector<std::vector<QueuePhase>>* alternatives = nullptr) {
    decodeQueueSequence(samples, settings);
    auto phases = decodeLogicalQueuePhases(
        samples, settings, duration, startSeconds, alternatives);
    if (phases.empty()) {
        // Keep a reviewable result if a manually edited history is itself
        // contradictory. The normal automatic path is the global legal-state
        // decoder above; this fallback exists only so the editor can expose
        // and repair an impossible manual anchor.
        QueuePhaseBuilder fallback(resolveQueueConfirmSamples(settings), startSeconds);
        for (const auto& sample : samples) fallback.push(
            sample.timeSeconds, sample.decoded.next.empty() ? sample.observation : sample.decoded);
        phases = fallback.finish(duration);
    }
    for (auto& phase : phases) {
        // A phase is closed by placing the active mino immediately before its
        // end.  Its first raw sample can still describe the previous piece
        // during a hold/queue-slide animation (the opening T->Hold, Z spawn
        // is a concrete example), so choose the latest pre-boundary sample.
        // Prefer an explicit editor choice over inferred evidence.
        const QueueRecognitionSample* activeSample = nullptr;
        for (const auto& sample : samples) {
            if (sample.timeSeconds < phase.start - .000001 || sample.timeSeconds >= phase.end - .000001) continue;
            if (!isPiece(sample.active)) continue;
            if (!activeSample || (sample.manuallyEdited && !activeSample->manuallyEdited) ||
                (sample.manuallyEdited == activeSample->manuallyEdited && sample.timeSeconds > activeSample->timeSeconds)) {
                activeSample = &sample;
            }
        }
        // ACTIVE is derived from the legal queue/HOLD state. Only an
        // explicit editor choice may override that inference.
        if (activeSample && activeSample->manuallyEdited) {
            phase.active = activeSample->active;
        }
        phase.queueManuallyFixed = std::any_of(samples.begin(), samples.end(), [&](const QueueRecognitionSample& sample) {
            return sample.manuallyEdited && sample.timeSeconds >= phase.start - .000001 &&
                   sample.timeSeconds <= phase.end + .000001;
        });
    }
    return phases;
}

#include "incremental_phase_decoder.inc"

Board cleanUpBoard(const VisibleBoard& visible) {
    Board board = VisionAnalyzer::makeFullBoard(visible);
    int firstNonGarbageRow = -1;
    for (int y = BoardHeight - 1; y >= 0; --y) {
        bool includesGarbage = false;
        for (int x = 0; x < BoardWidth; ++x) {
            if (board[index(x, y)] == Cell::Garbage) { includesGarbage = true; break; }
        }
        if (!includesGarbage) { firstNonGarbageRow = y; break; }
    }
    // TR_NO_GARBAGE_CLEANUP=1（実験用、既定オフ）: 浮いたおじゃまの消去をしない。
    static const bool noGarbageCleanup = [] {
        const char* v = std::getenv("TR_NO_GARBAGE_CLEANUP");
        return v && v[0] == '1';
    }();
    if (firstNonGarbageRow != -1 && !noGarbageCleanup) {
        for (int y = firstNonGarbageRow - 1; y >= 0; --y) {
            for (int x = 0; x < BoardWidth; ++x) {
                if (board[index(x, y)] == Cell::Garbage) board[index(x, y)] = Cell::Empty;
            }
        }
    }
    return board;
}

struct BoardRequest {
    double time = 0;
    int phase = 0;
};

struct PrefetchedBoardSlot {
    BoardObservation observation;
    std::int64_t sourceTime100ns = 0;
    // Diagnostics only (TR_BOARD_FRAME_DIAGNOSTICS /
    // TR_BOARD_PREFETCH_DIAGNOSTICS): the decoder's delivery ordinal of the
    // frame this observation came from and a fingerprint of its pixels. They
    // let an A/B run prove whether two containers sampled the same frame
    // instead of inferring it from timestamps that differ by only 0.5 ms.
    std::uint64_t frameOrdinal = 0;
    std::uint64_t frameHash = 0;
};

struct RetainedPrefetchFrame {
    std::int64_t time100ns = 0;
    Frame p1;
    Frame p2;

    std::size_t bytes() const { return p1.bgra.size() + p2.bgra.size(); }
};

struct BoardRequestPrefetch {
    std::map<std::int64_t, std::shared_ptr<PrefetchedBoardSlot>> p1;
    std::map<std::int64_t, std::shared_ptr<PrefetchedBoardSlot>> p2;
    std::size_t ringPeakBytes = 0;
    std::uint64_t retainedFrames = 0;
    std::uint64_t evictedFrames = 0;
    std::uint64_t submittedRequests = 0;
};

// The fast hybrid path deliberately analyzes a tiny Media Foundation prefix
// before opening the NVDEC continuation.  Historically each half constructed
// a fresh queue session and two fresh board sessions even though the model
// files and player layouts cannot change inside one attempt.  This cache is
// attempt-local (never global), so it can safely retain those sessions across
// the boundary without changing ordinary analyze/continue calls.
struct ReusableAnalysisModels {
    std::filesystem::path boardPath;
    std::filesystem::path queuePath;
    std::unique_ptr<OnnxBoardModel> p1Board;
    std::unique_ptr<OnnxBoardModel> p2Board;
    std::unique_ptr<OnnxQueueModel> queue;
    std::uint64_t createdSessions = 0;
    std::uint64_t reusedSessions = 0;
    double initializationSeconds = 0;

    OnnxBoardModel* board(bool player1, const std::filesystem::path& path,
                          std::string& error) {
        const auto normalized = path.lexically_normal();
        if (!boardPath.empty() && boardPath != normalized) {
            error = "Reusable board-model path changed inside one analysis attempt";
            return nullptr;
        }
        boardPath = normalized;
        auto& model = player1 ? p1Board : p2Board;
        if (model) {
            ++reusedSessions;
            return model.get();
        }
        const auto started = std::chrono::steady_clock::now();
        model = std::make_unique<OnnxBoardModel>(path, error);
        initializationSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (!model->ready()) {
            model.reset();
            return nullptr;
        }
        ++createdSessions;
        return model.get();
    }

    OnnxQueueModel* queueModel(const std::filesystem::path& path,
                               std::string& error) {
        const auto normalized = path.lexically_normal();
        if (!queuePath.empty() && queuePath != normalized) {
            error = "Reusable queue-model path changed inside one analysis attempt";
            return nullptr;
        }
        queuePath = normalized;
        if (queue) {
            ++reusedSessions;
            return queue.get();
        }
        const auto started = std::chrono::steady_clock::now();
        queue = std::make_unique<OnnxQueueModel>(path, error);
        initializationSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (!queue->ready()) {
            queue.reset();
            return nullptr;
        }
        ++createdSessions;
        return queue.get();
    }

    void disableQueueModel() { queue.reset(); }
};

std::int64_t boardRequestKey(double time) {
    return static_cast<std::int64_t>(std::llround(time * 1000000000.0));
}

// Container A/B instrumentation. A fragmented MP4 and an indexed MP4 of the
// same bitstream must yield the same board observation for the same request;
// when they do not, the useful question is whether the sampler read a
// different frame or classified the same frame differently. The delivery
// ordinal answers the first half and the pixel fingerprint the second, so both
// are recorded without changing any sampling decision.
bool boardFrameDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* framePath = std::getenv("TR_BOARD_FRAME_DIAGNOSTICS");
        if (framePath && *framePath != '\0') return true;
        const char* prefetchPath = std::getenv("TR_BOARD_PREFETCH_DIAGNOSTICS");
        return prefetchPath && *prefetchPath != '\0';
    }();
    return enabled;
}

std::uint64_t boardPixelFingerprint(const std::vector<std::uint8_t>& bgra) {
    // FNV-1a: order sensitive, no multiplication by zero for uniform frames, and
    // cheap enough to run on every sampled board rectangle.
    std::uint64_t hash = 1469598103934665603ull;
    for (const std::uint8_t byte : bgra) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}

// One diagnostic run covers every match, so the board CSVs are appended rather
// than truncated: `pass` numbers the analyze range and `request_index` numbers
// the request inside it, which keeps two containers comparable even when their
// request timestamps drift.
std::atomic<std::uint64_t> boardFrameDiagnosticsPass{0};

std::ofstream openBoardDiagnostics(const char* path, bool& writeHeader) {
    std::ifstream existing(path, std::ios::binary | std::ios::ate);
    writeHeader = !existing || existing.tellg() <= 0;
    existing.close();
    std::ofstream stream(path, std::ios::out | std::ios::app);
    return stream;
}

std::vector<BoardRequest> makeBoardRequests(const std::vector<QueuePhase>& phases, int onnxSamples) {
    std::vector<BoardRequest> result;
    for (int i = 0; i < static_cast<int>(phases.size()); ++i) {
        for (const double time : boardSampleTimes(phases[i].start, phases[i].end, onnxSamples)) {
            result.push_back({time, i});
        }
    }
    return result;
}

#ifndef __EMSCRIPTEN__

unsigned boardWorkerCount() {
    if (const char* configured = std::getenv("TR_BOARD_WORKERS")) {
        const int value = std::atoi(configured);
        if (value > 0) return static_cast<unsigned>(std::clamp(value, 1, 16));
    }
    const unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) return 4;
    // Each ONNX session is configured for one intra-op thread.  Keeping the
    // number of concurrent requests bounded avoids oversubscribing small
    // machines while using the available CPU on the current 24-thread host.
    return std::clamp(hardware / 4u, 2u, 8u);
}

class BoardWorkerPool {
public:
    explicit BoardWorkerPool(unsigned count) {
        maxQueued_ = std::max<std::size_t>(count * 2u, 1u);
        workers_.reserve(count);
        for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] { run(); });
    }

    BoardWorkerPool(const BoardWorkerPool&) = delete;
    BoardWorkerPool& operator=(const BoardWorkerPool&) = delete;

    ~BoardWorkerPool() {
        wait();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        workAvailable_.notify_all();
        for (auto& worker : workers_) worker.join();
    }

    void submit(std::function<void()> task) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            spaceAvailable_.wait(lock, [this] { return stopping_ || queue_.size() < maxQueued_; });
            if (stopping_) return;
            queue_.push_back(std::move(task));
        }
        workAvailable_.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return queue_.empty() && active_ == 0; });
    }

private:
    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                workAvailable_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
                ++active_;
                spaceAvailable_.notify_one();
            }
            task();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                --active_;
                if (queue_.empty() && active_ == 0) idle_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable spaceAvailable_;
    std::condition_variable idle_;
    std::size_t active_ = 0;
    std::size_t maxQueued_ = 1;
    bool stopping_ = false;
};

#else

// The browser calls the same analysis in a single WASM instance.  Keeping
// this adapter synchronous preserves submission order and avoids requiring
// SharedArrayBuffer/cross-origin isolation on GitHub Pages.
unsigned boardWorkerCount() { return 1; }

class BoardWorkerPool {
public:
    explicit BoardWorkerPool(unsigned) {}
    BoardWorkerPool(const BoardWorkerPool&) = delete;
    BoardWorkerPool& operator=(const BoardWorkerPool&) = delete;
    void submit(std::function<void()> task) { task(); }
    void wait() {}
};

#endif

// One-pass board capture. The queue scan already decodes every frame of the
// range and keeps a raw NV12 ring of the two board rectangles, so the board
// observations can be produced from that decode instead of a second full-range
// decode. `collectOnePassBoardRepair` is the bounded safety net for the
// handful of requests whose frame left the ring before the phase that owns
// them was confirmed.
struct MissingBoardRequest {
    BoardRequest request;
    bool player1 = true;
};
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT) && defined(TR_NVDEC_DEFAULT)
bool collectOnePassBoardRepair(
    VideoReader& reader, NvdecCudaPreprocessor& preprocessor,
    const std::vector<MissingBoardRequest>& missing,
    VisionAnalyzer* p1Vision, VisionAnalyzer* p2Vision,
    BoardWorkerPool* pool, BoardRequestPrefetch& destination,
    std::size_t& fulfilled, std::string& error);
#endif

bool collectQueuePhases(const std::filesystem::path& input, const Settings& settings, Status& status,
                        std::vector<QueuePhase>& p1, std::vector<QueuePhase>& p2,
                        std::vector<QueueRecognitionSample>& p1Samples,
                        std::vector<QueueRecognitionSample>& p2Samples,
                        double& durationOut, std::string& error,
                         double analysisEndSeconds = -1, double startSeconds = 0,
                         const std::filesystem::path* boardModelPath = nullptr,
                         BoardRequestPrefetch* boardPrefetch = nullptr,
                         const std::filesystem::path* queueModelPath = nullptr,
                         ReusableAnalysisModels* reusableModels = nullptr,
                         Frame* playerNameFrame = nullptr,
                         int* playerNameSourceWidth = nullptr,
                         int* playerNameSourceHeight = nullptr) {
    VideoReader reader;
    std::unique_ptr<VideoReader> hybridPixelReader;
#ifndef __EMSCRIPTEN__
    bool nvdecFrontEnabled = false;
    // NVDEC remains an explicitly selectable experimental backend. On the
    // current RTX 5060 system its per-frame CUDA/D3D ownership transition is
    // slower than Media Foundation's native D3D11 decode path and its colour
    // conversion changes observations enough to trigger recovery retries.
    if (const char* configured = std::getenv("TR_NVDEC_FRONT")) {
        nvdecFrontEnabled = std::string(configured) != "0";
    }
    nvdecFrontEnabled = nvdecFrontEnabled && boardPrefetch &&
        boardModelPath && queueModelPath;
    char hybridQueueValue[16]{};
    const DWORD hybridQueueLength = GetEnvironmentVariableA(
        "TR_GPU_HYBRID_QUEUE", hybridQueueValue,
        static_cast<DWORD>(std::size(hybridQueueValue)));
    const bool hybridQueueEnabled = hybridQueueLength > 0 &&
        hybridQueueLength < std::size(hybridQueueValue) && std::atoi(hybridQueueValue) != 0;
    if (hybridQueueEnabled) {
        char previousGpuMode[16]{};
        const DWORD previousGpuModeLength = GetEnvironmentVariableA(
            "TR_GPU_VIDEO", previousGpuMode, static_cast<DWORD>(std::size(previousGpuMode)));
        const bool hadPreviousGpuMode = previousGpuModeLength > 0 &&
            previousGpuModeLength < std::size(previousGpuMode);
        SetEnvironmentVariableA("TR_GPU_VIDEO", "0");
        const bool timingOpened = reader.open(input, error);
        if (timingOpened) {
            hybridPixelReader = std::make_unique<VideoReader>();
            SetEnvironmentVariableA("TR_GPU_VIDEO", "2");
            if (!hybridPixelReader->open(input, error)) hybridPixelReader.reset();
        }
        if (hadPreviousGpuMode) SetEnvironmentVariableA("TR_GPU_VIDEO", previousGpuMode);
        else SetEnvironmentVariableA("TR_GPU_VIDEO", nullptr);
        if (!timingOpened || !hybridPixelReader) return false;
        if (hybridPixelReader->width() != reader.width() ||
            hybridPixelReader->height() != reader.height()) {
            error = "Hybrid GPU reader dimensions do not match the legacy timing reader";
            return false;
        }
    } else if (!reader.open(input, error, false, false, nvdecFrontEnabled)) {
        return false;
    }
#else
    constexpr bool hybridQueueEnabled = false;
    if (!reader.open(input, error)) return false;
#endif
    std::unique_ptr<OnnxBoardModel> ownedP1Model;
    std::unique_ptr<OnnxBoardModel> ownedP2Model;
    std::unique_ptr<OnnxQueueModel> ownedQueueModel;
    OnnxBoardModel* p1Model = nullptr;
    OnnxBoardModel* p2Model = nullptr;
    OnnxQueueModel* queueModel = nullptr;
    std::unique_ptr<OnnxQueueModel> ownedLabelModel;
    OnnxQueueModel* labelModel = nullptr;
    std::unique_ptr<OnnxQueueModel> ownedHoldModel;
    OnnxQueueModel* holdModel = nullptr;
    if (settings.queueCnnEnabled && queueModelPath && std::filesystem::exists(*queueModelPath)) {
        if (reusableModels) {
            queueModel = reusableModels->queueModel(*queueModelPath, error);
        } else {
            ownedQueueModel = std::make_unique<OnnxQueueModel>(*queueModelPath, error);
            queueModel = ownedQueueModel.get();
        }
        if (!queueModel || !queueModel->ready()) return false;
        // TR_QUEUE_LABEL_MODEL: 変化検知（確信度・unclear）は queueModel のまま、
        // ピースの値だけを別モデルで置き換える実験用フック。
        if (const char* labelPath = std::getenv("TR_QUEUE_LABEL_MODEL")) {
            if (*labelPath && std::filesystem::exists(labelPath)) {
                std::string labelError;
                ownedLabelModel = std::make_unique<OnnxQueueModel>(labelPath, labelError);
                if (ownedLabelModel && ownedLabelModel->ready()) {
                    labelModel = ownedLabelModel.get();
                } else {
                    ownedLabelModel.reset();
                }
            }
        }
        // TR_QUEUE_HOLD_MODEL: HOLD スロット（各プレイヤー6枚ごとの先頭）だけを
        // 別モデルで置き換える実験用フック。
        if (const char* holdPath = std::getenv("TR_QUEUE_HOLD_MODEL")) {
            if (*holdPath && std::filesystem::exists(holdPath)) {
                std::string holdError;
                ownedHoldModel = std::make_unique<OnnxQueueModel>(holdPath, holdError);
                if (ownedHoldModel && ownedHoldModel->ready()) {
                    holdModel = ownedHoldModel.get();
                    std::cerr << "queue_hold_model=" << holdPath << '\n';
                } else {
                    std::cerr << "queue_hold_model_error=" << holdError << '\n';
                    ownedHoldModel.reset();
                }
            } else if (*holdPath) {
                std::cerr << "queue_hold_model_error=missing " << holdPath << '\n';
            }
        }
    }
    if (boardPrefetch && boardModelPath && settings.player1Enabled) {
        if (reusableModels) {
            p1Model = reusableModels->board(true, *boardModelPath, error);
        } else {
            ownedP1Model = std::make_unique<OnnxBoardModel>(*boardModelPath, error);
            p1Model = ownedP1Model.get();
        }
        if (!p1Model || !p1Model->ready()) return false;
    }
    if (boardPrefetch && boardModelPath && settings.player2Enabled) {
        if (reusableModels) {
            p2Model = reusableModels->board(false, *boardModelPath, error);
        } else {
            ownedP2Model = std::make_unique<OnnxBoardModel>(*boardModelPath, error);
            p2Model = ownedP2Model.get();
        }
        if (!p2Model || !p2Model->ready()) return false;
    }
    VisionAnalyzer p1Vision(reader.width(), reader.height(), settings.p1, p1Model);
    VisionAnalyzer p2Vision(reader.width(), reader.height(), settings.p2, p2Model);
    if (playerNameSourceWidth) *playerNameSourceWidth = reader.width();
    if (playerNameSourceHeight) *playerNameSourceHeight = reader.height();
    bool playerNameCaptureAttempted = false;
#ifndef __EMSCRIPTEN__
#ifdef TR_QUEUE_ROI_DEFAULT
    bool queueRoiEnabled = true;
#else
    bool queueRoiEnabled = false;
#endif
#ifdef TR_QUEUE_SPARSE_ROI_DEFAULT
    bool queueSparseRoiEnabled = true;
#else
    bool queueSparseRoiEnabled = false;
#endif
    char queueRoiValue[16]{};
    const DWORD queueRoiLength = GetEnvironmentVariableA(
        "TR_QUEUE_ROI", queueRoiValue, static_cast<DWORD>(std::size(queueRoiValue)));
    if (queueRoiLength > 0 && queueRoiLength < std::size(queueRoiValue)) {
        queueRoiEnabled = std::atoi(queueRoiValue) != 0;
    }
    char queueSparseRoiValue[16]{};
    const DWORD queueSparseRoiLength = GetEnvironmentVariableA(
        "TR_QUEUE_SPARSE_ROI", queueSparseRoiValue,
        static_cast<DWORD>(std::size(queueSparseRoiValue)));
    if (queueSparseRoiLength > 0 && queueSparseRoiLength < std::size(queueSparseRoiValue)) {
        queueSparseRoiEnabled = std::atoi(queueSparseRoiValue) != 0;
    }
    // Sparse frames retain full-frame addressing while transferring only the
    // requested rectangles. This now works for both legacy RGB32 and D3D11,
    // and lets the bounded board ring share the same decode without forcing a
    // full 1920x1344 readback on every delivered frame.
    queueSparseRoiEnabled = queueSparseRoiEnabled && !hybridPixelReader;
    queueRoiEnabled = queueRoiEnabled && !queueSparseRoiEnabled &&
        !hybridPixelReader && !boardPrefetch;
    PixelRegion queueReadRegion{0, 0, reader.width(), reader.height()};
    std::vector<PixelRegion> queueSparseRegions;
    if (queueRoiEnabled || queueSparseRoiEnabled) {
        bool haveRegion = false;
        const auto include = [&](const PixelRegion& region) {
            if (region.width <= 0 || region.height <= 0) return;
            queueSparseRegions.push_back(region);
            if (!haveRegion) {
                queueReadRegion = region;
                haveRegion = true;
                return;
            }
            const int x0 = std::min(queueReadRegion.x, region.x);
            const int y0 = std::min(queueReadRegion.y, region.y);
            const int x1 = std::max(queueReadRegion.x + queueReadRegion.width,
                                    region.x + region.width);
            const int y1 = std::max(queueReadRegion.y + queueReadRegion.height,
                                    region.y + region.height);
            queueReadRegion = {x0, y0, x1 - x0, y1 - y0};
        };
        const auto includeQueue = [&](VisionAnalyzer& vision) {
            if (reader.usesD3D11()) {
                for (const PixelRegion& region : vision.queuePixelRegions()) include(region);
            } else {
                include(vision.queuePixelRegion());
            }
        };
        if (settings.player1Enabled) includeQueue(p1Vision);
        if (settings.player2Enabled) includeQueue(p2Vision);
        if (boardPrefetch && settings.player1Enabled) include(p1Vision.boardPixelRegion());
        if (boardPrefetch && settings.player2Enabled) include(p2Vision.boardPixelRegion());
        if (!haveRegion) {
            queueRoiEnabled = false;
            queueSparseRoiEnabled = false;
        }
        if ((queueRoiEnabled || queueSparseRoiEnabled) &&
            std::getenv("TR_GPU_DIAGNOSTICS")) {
            std::cerr << "queue_readback_roi=" << queueReadRegion.x << ',' << queueReadRegion.y
                      << ',' << queueReadRegion.width << ',' << queueReadRegion.height
                      << " sparse=" << (queueSparseRoiEnabled ? 1 : 0);
            for (const auto& region : queueSparseRegions) {
                std::cerr << " part=" << region.x << ',' << region.y << ','
                          << region.width << ',' << region.height;
            }
            std::cerr << '\n';
        }
    }
    if (queueSparseRoiEnabled) {
        reader.setGpuConversionRegions(queueSparseRegions);
    } else if (queueRoiEnabled) {
        reader.setGpuConversionRegions({queueReadRegion});
    }
#else
    constexpr bool queueRoiEnabled = false;
    constexpr bool queueSparseRoiEnabled = false;
    PixelRegion queueReadRegion{0, 0, reader.width(), reader.height()};
    std::vector<PixelRegion> queueSparseRegions;
#endif
    const double duration = reader.durationSeconds();
    durationOut = duration;
    // NEXT 観測の確定回数をソースのフレームレートに合わせる（30fps基準）。
    if (const double sourceFps = reader.frameRate(); sourceFps > 1.0) {
        const double scaled = static_cast<double>(settings.queueConfirmSamples) * (sourceFps / 30.0);
        const int effective = static_cast<int>(std::clamp(std::lround(scaled), 1L, 12L));
        gQueueConfirmEffective.store(effective, std::memory_order_relaxed);
        if (const char* debug = std::getenv("TR_QUEUE_CONFIRM_DEBUG")) {
            if (std::string(debug) != "0") {
                std::fprintf(stderr, "[confirm] fps=%.2f base=%d effective=%d\n",
                             sourceFps, settings.queueConfirmSamples, effective);
                std::fflush(stderr);
            }
        }
    }
    // Fragmented MP4 (streaming downloads) reports no overall duration; the
    // caller-supplied range end is then the only reliable stop point.
    const double analysisEnd = analysisEndSeconds > 0
        ? (duration > 0 ? std::min(analysisEndSeconds, duration) : analysisEndSeconds)
        : duration;
    const double firstScan = startSeconds > 0
        ? std::ceil((startSeconds - .0000001) / settings.sampleIntervalSeconds) * settings.sampleIntervalSeconds
        : 0;
    if (startSeconds > 0) {
        if (!reader.seek(startSeconds, error)) return false;
        if (hybridPixelReader && !hybridPixelReader->seek(startSeconds, error)) return false;
    }
    double nextScan = std::max(0.0, firstScan);
    if (analysisEnd > nextScan && settings.sampleIntervalSeconds > 0) {
        const auto additional = static_cast<std::size_t>(
            std::ceil((analysisEnd - nextScan) / settings.sampleIntervalSeconds));
        if (settings.player1Enabled) p1Samples.reserve(p1Samples.size() + additional);
        if (settings.player2Enabled) p2Samples.reserve(p2Samples.size() + additional);
    }
    Frame frame;
    Frame lastFrame;
    Frame hybridCurrentFrame;
    bool haveHybridCurrentFrame = false;
    bool hybridPixelEos = false;
    bool haveFrame = false;
    bool eos = false;
    std::uint64_t scans = 0;
    std::optional<QueueObservation> lastP1Observation;
    std::optional<QueueObservation> lastP2Observation;
    // These belong to the recognition layer, not the queue decoder. Missing
    // CNN evidence repeats the last fully trusted CNN state so it cannot
    // manufacture a transition. No legacy colour value is substituted.
    std::optional<QueueObservation> trustedP1CnnObservation;
    std::optional<QueueObservation> trustedP2CnnObservation;
    std::unique_ptr<BoardWorkerPool> prefetchWorkers;
    std::deque<std::shared_ptr<RetainedPrefetchFrame>> recentFrames;
    // Every retained, queued and actively inferred frame comes from this one
    // fixed pool. use_count()==1 means that only the pool owns the slot. This
    // makes the configured payload budget cover the ring and worker captures,
    // instead of bounding only the deque while hidden shared_ptr copies keep
    // evicted frame buffers alive.
    std::vector<std::shared_ptr<RetainedPrefetchFrame>> prefetchSlots;
    std::size_t recentFrameBytes = 0;
    std::size_t prefetchSlotBytes = 0;
    std::size_t maxPrefetchSlots = 0;
    double latestEvictedFrameTime = -std::numeric_limits<double>::infinity();
    std::atomic<bool> prefetchFailed{false};
    std::atomic<std::uint64_t> prefetchWorkerNanoseconds{0};
    std::mutex prefetchErrorMutex;
    std::string prefetchError;
    std::optional<QueueObservation> confirmedP1;
    std::optional<QueueObservation> confirmedP2;
    std::vector<float> queueCnnBatch;
    std::vector<double> queueDarkRatios;
    int boardPrefetchMegabytes = 512;
#ifdef TR_GPU_MAX_DEFAULT
    bool gpuBoardStreamEnabled = true;
#else
    bool gpuBoardStreamEnabled = false;
#endif
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    struct PendingQueueRecognition {
        double frameTime = 0;
        QueueObservation p1Legacy;
        QueueObservation p2Legacy;
        std::size_t p1Offset = 0;
        std::size_t p2Offset = 0;
    };
    // Eight decoded frames are the historical default. Wider batches are
    // available to separately named experiments while observations remain in
    // delivery order.
#ifdef TR_QUEUE_OPTIMIZED_DEFAULT
    int queueFrameBatch = 16;
#else
    int queueFrameBatch = 8;
#endif
#ifndef __EMSCRIPTEN__
    char queueFrameBatchValue[16]{};
    const DWORD queueFrameBatchLength = GetEnvironmentVariableA(
        "TR_QUEUE_FRAME_BATCH", queueFrameBatchValue,
        static_cast<DWORD>(std::size(queueFrameBatchValue)));
    if (queueFrameBatchLength > 0 && queueFrameBatchLength < std::size(queueFrameBatchValue)) {
        queueFrameBatch = std::clamp(std::atoi(queueFrameBatchValue), 1, 32);
    }
    char boardPrefetchMemoryValue[16]{};
    const DWORD boardPrefetchMemoryLength = GetEnvironmentVariableA(
        "TR_BOARD_PREFETCH_MB", boardPrefetchMemoryValue,
        static_cast<DWORD>(std::size(boardPrefetchMemoryValue)));
    if (boardPrefetchMemoryLength > 0 &&
        boardPrefetchMemoryLength < std::size(boardPrefetchMemoryValue)) {
        boardPrefetchMegabytes = std::clamp(std::atoi(boardPrefetchMemoryValue), 256, 8192);
    }
#endif
    std::vector<PendingQueueRecognition> pendingQueueRecognitions;
    pendingQueueRecognitions.reserve(static_cast<std::size_t>(queueFrameBatch));
    bool pendingP1PrefetchRefresh = false;
    bool pendingP2PrefetchRefresh = false;
    double lastPrefetchRefreshEnd = startSeconds;
    constexpr double PrefetchRefreshIntervalSeconds = 0.5;
    const bool queueTimingEnabled = std::getenv("TR_QUEUE_TIMING") != nullptr;
    double queueReadSeconds = 0;
    double queuePreprocessSeconds = 0;
    double queueStatusSeconds = 0;
    double queueInferenceSeconds = 0;
    double prefetchPhaseRebuildSeconds = 0;
    double prefetchScheduleSeconds = 0;
    double prefetchRetainSeconds = 0;
    double prefetchWaitSeconds = 0;
    double finalQueueDecodeSeconds = 0;
    std::uint64_t deliveredQueueFrames = 0;
    std::uint64_t queueCallbackFrames = 0;
    std::uint64_t queueScanFrames = 0;
    std::uint64_t queueDuplicateFrames = 0;
    double queueLastFrameTime = -1.0;
    std::uint64_t inferredQueueFrames = 0;
    std::uint64_t queueInferenceFlushes = 0;
    std::uint64_t adaptiveQueueInferredFrames = 0;
    std::uint64_t adaptiveQueueReusedFrames = 0;
    std::uint64_t adaptiveQueueInferredSlots = 0;
    std::uint64_t adaptiveQueueReusedSlots = 0;
    std::uint64_t adaptiveQueueInferenceRuns = 0;
    double backgroundQueueInferenceSeconds = 0;
#if defined(_WIN32) && defined(TR_GPU_ULTRA_DEFAULT)
    bool nvdecCudaDirectEnabled = false;
#if defined(TR_NVDEC_DEFAULT)
    if (const char* configured = std::getenv("TR_NVDEC_DIRECT")) {
        nvdecCudaDirectEnabled = std::string(configured) != "0";
    }
    nvdecCudaDirectEnabled = nvdecCudaDirectEnabled && nvdecFrontEnabled &&
        reader.usesNvdecCuda() && boardPrefetch && queueModel &&
        (p1Model || p2Model);
#endif
    if (const char* configured = std::getenv("TR_ULTRA_GPU_BOARD_STREAM")) {
        gpuBoardStreamEnabled = std::string(configured) != "0";
    }
    gpuBoardStreamEnabled = gpuBoardStreamEnabled && boardPrefetch &&
        reader.usesD3D11() && p1Model && p1Model->cudaReady() &&
        !hybridPixelReader;
    std::unique_ptr<D3D11BoardPreprocessor> gpuBoardPreprocessor;
    std::deque<D3D11BoardFrameHandle> gpuBoardFrames;
    std::uint64_t gpuBoardDeliveryOrder = 0;
    double gpuBoardRetainSeconds = 0;
    double boardCommitSeconds = 0;
    double gpuBoardInferenceSeconds = 0;
    std::uint64_t boardRequestMapHash = 14695981039346656037ull;
    std::uint64_t boardRequestMapCount = 0;
    const auto recordBoardRequestMapping = [&boardRequestMapHash,
                                             &boardRequestMapCount](
            bool player1, const BoardRequest& request,
            const D3D11BoardFrameHandle& source) {
        const auto mixFixed64 = [&boardRequestMapHash](std::uint64_t value) {
            // Fixed little-endian encoding makes the diagnostic independent
            // of host object layout, padding, and signed integer formatting.
            for (unsigned byte = 0; byte < 8; ++byte) {
                boardRequestMapHash ^= (value >> (byte * 8)) & 0xffu;
                boardRequestMapHash *= 1099511628211ull;
            }
        };
        mixFixed64(player1 ? 1u : 2u);
        mixFixed64(static_cast<std::uint64_t>(request.phase));
        mixFixed64(static_cast<std::uint64_t>(boardRequestKey(request.time)));
        mixFixed64(source.deliveryOrder);
        mixFixed64(static_cast<std::uint64_t>(source.sourceTime100ns));
        ++boardRequestMapCount;
    };
    if (gpuBoardStreamEnabled) {
        if (!nvdecCudaDirectEnabled) {
            gpuBoardPreprocessor = std::make_unique<D3D11BoardPreprocessor>();
        }
    }
#if defined(TR_NVDEC_DEFAULT)
    std::unique_ptr<NvdecCudaPreprocessor> nvdecCudaPreprocessor;
    struct PendingNvdecBoardRequest {
        BoardRequest request;
        D3D11BoardFrameHandle source;
        bool player1 = false;
    };
    bool nvdecBoardBatchEnabled = false;
    bool nvdecBoardBatchScalarCnn = false;
    if (const char* configured = std::getenv(
            "TR_NVDEC_BOARD_BATCH_SCALAR_CNN")) {
        nvdecBoardBatchScalarCnn = std::string(configured) != "0";
    }
    if (const char* configured = std::getenv("TR_NVDEC_BOARD_BATCH")) {
        nvdecBoardBatchEnabled = std::string(configured) != "0";
    }
    nvdecBoardBatchEnabled =
        (nvdecBoardBatchEnabled || nvdecBoardBatchScalarCnn) &&
        nvdecCudaDirectEnabled;
    std::vector<PendingNvdecBoardRequest> pendingNvdecBoardRequests;
    std::uint64_t nvdecBoardBatchFlushes = 0;
    std::uint64_t nvdecBoardBatchRequests = 0;
    std::uint64_t nvdecBoardBatchFallbacks = 0;
    std::size_t nvdecBoardBatchMaximum = 0;
    // 既定は旧挙動（2フレームに1回）。TR_NVDEC_QUEUE_FRAME_STRIDE=1 で毎フレーム。
    int nvdecQueueFrameStride = 2;
    if (const char* configured = std::getenv("TR_NVDEC_QUEUE_FRAME_STRIDE")) {
        nvdecQueueFrameStride = std::clamp(std::atoi(configured), 1, 6);
    }
    int nvdecBoardFrameStride = 1;
    if (const char* configured = std::getenv("TR_NVDEC_BOARD_FRAME_STRIDE")) {
        nvdecBoardFrameStride = std::clamp(std::atoi(configured), 1, 6);
    }
    bool nvdecMappedZeroCopyEnabled = false;
    if (const char* configured = std::getenv("TR_NVDEC_MAPPED_ZERO_COPY")) {
        nvdecMappedZeroCopyEnabled = std::string(configured) != "0";
    }
    if (nvdecCudaDirectEnabled) {
        nvdecCudaPreprocessor = std::make_unique<NvdecCudaPreprocessor>();
    }
#endif
    bool gpuQueuePreprocessEnabled = true;
    if (const char* configured = std::getenv("TR_ULTRA_GPU_QUEUE_PREPROCESS")) {
        gpuQueuePreprocessEnabled = std::string(configured) != "0";
    }
    if (const char* configured = std::getenv("TR_GPU_QUEUE_ONNX")) {
        gpuQueuePreprocessEnabled = gpuQueuePreprocessEnabled &&
            std::string(configured) != "0";
    }
    // The CPU bounded-board experiment still requires sparse readback.  The
    // GPU board ring, in contrast, consumes the same delivered D3D11 texture
    // and can therefore run together with queue zero-copy.
    gpuQueuePreprocessEnabled = gpuQueuePreprocessEnabled && reader.usesD3D11() &&
        queueModel && (!boardPrefetch || gpuBoardStreamEnabled) &&
        !hybridPixelReader && queueFrameBatch >= 1 &&
        queueFrameBatch <= static_cast<int>(D3D11QueuePreprocessor::MaximumFrames);
    std::unique_ptr<D3D11QueuePreprocessor> gpuQueuePreprocessor;
    std::deque<std::vector<PendingQueueRecognition>> gpuQueuePipelinePending;
    if (gpuQueuePreprocessEnabled) {
        if (!nvdecCudaDirectEnabled) {
            gpuQueuePreprocessor = std::make_unique<D3D11QueuePreprocessor>();
        }
    }
    // The single-dispatch experiment is retained for reproducible A/B, but
    // is not the production default: mixing the large FP64 board workload
    // with the small FP32 queue resize reduced occupancy on RTX 5060. Direct
    // decoder-texture sharing remains enabled independently in both stages.
    bool gpuFusedPreprocessEnabled = false;
    if (const char* configured = std::getenv("TR_GPU_FUSED_DISPATCH")) {
        gpuFusedPreprocessEnabled = std::string(configured) != "0";
    }
    gpuFusedPreprocessEnabled = gpuFusedPreprocessEnabled &&
        gpuQueuePreprocessEnabled && gpuBoardStreamEnabled;
    if (std::getenv("TR_GPU_DIAGNOSTICS")) {
        std::cerr << "gpu_queue_preprocess=" << (gpuQueuePreprocessEnabled ? 1 : 0)
                  << " gpu_board_stream=" << (gpuBoardStreamEnabled ? 1 : 0)
                  << " gpu_fused_preprocess=" << (gpuFusedPreprocessEnabled ? 1 : 0)
                  << " nvdec_cuda_direct=" << (nvdecCudaDirectEnabled ? 1 : 0)
#if defined(TR_NVDEC_DEFAULT)
                  << " nvdec_queue_stride=" << nvdecQueueFrameStride
                  << " nvdec_board_stride=" << nvdecBoardFrameStride
#endif
                  << '\n';
    }
#endif
#endif
    const std::size_t maxPrefetchPayloadBytes =
        static_cast<std::size_t>(boardPrefetchMegabytes) * 1024ull * 1024ull;
#ifdef TR_GPU_MAX_DEFAULT
    bool incrementalPrefetchPhaseEnabled = true;
#else
    bool incrementalPrefetchPhaseEnabled = false;
#endif
#ifndef __EMSCRIPTEN__
    if (const char* configured = std::getenv("TR_INCREMENTAL_PREFETCH_PHASES")) {
        incrementalPrefetchPhaseEnabled = std::string(configured) != "0";
    }
#endif
    incrementalPrefetchPhaseEnabled = incrementalPrefetchPhaseEnabled && boardPrefetch;
    IncrementalGlobalQueueDecoder p1IncrementalPrefetchBuilder(settings);
    IncrementalGlobalQueueDecoder p2IncrementalPrefetchBuilder(settings);
    if (incrementalPrefetchPhaseEnabled) {
        p1IncrementalPrefetchBuilder.reset(p1Samples);
        p2IncrementalPrefetchBuilder.reset(p2Samples);
    }
#ifndef __EMSCRIPTEN__
    if (boardPrefetch && std::getenv("TR_GPU_DIAGNOSTICS")) {
        std::cerr << "incremental_prefetch_phases="
                  << (incrementalPrefetchPhaseEnabled ? 1 : 0) << '\n';
    }
#endif
    queueCnnBatch.reserve(static_cast<std::size_t>(12 * 3 * 48 * 48)
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
                          * static_cast<std::size_t>(queueFrameBatch)
#endif
    );
    queueDarkRatios.reserve(static_cast<std::size_t>(12)
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
                            * static_cast<std::size_t>(queueFrameBatch)
#endif
    );
    if (boardPrefetch) {
        boardPrefetch->p1.clear();
        boardPrefetch->p2.clear();
        boardPrefetch->ringPeakBytes = 0;
        boardPrefetch->retainedFrames = 0;
        boardPrefetch->evictedFrames = 0;
        boardPrefetch->submittedRequests = 0;
        if (!gpuBoardStreamEnabled) {
            prefetchWorkers = std::make_unique<BoardWorkerPool>(
                std::max(1u, boardWorkerCount()));
        }
        if (!p1Samples.empty()) confirmedP1 = p1Samples.back().observation;
        if (!p2Samples.empty()) confirmedP2 = p2Samples.back().observation;
    }
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT) && defined(TR_NVDEC_DEFAULT)
    // One-pass board capture. The scan already decodes every frame of the
    // range, so retain the raw NV12 board rectangle in the ring and run the
    // established CPU board analyzer on that decode. This is the same analyzer
    // and the same NV12-to-BGRA arithmetic the range fallback used, so every
    // board observation stays bit-identical while the second full-range decode
    // disappears. TR_ONEPASS_BOARD=0 restores the GPU board-image ring.
    bool onePassBoardEnabled = nvdecCudaDirectEnabled && boardPrefetch &&
        nvdecCudaPreprocessor;
    if (const char* configured = std::getenv("TR_ONEPASS_BOARD")) {
        onePassBoardEnabled =
            onePassBoardEnabled && std::string(configured) != "0";
    }
    std::vector<MissingBoardRequest> missingBoardRequests;
    std::mutex missingBoardMutex;
    std::uint64_t onePassBoardRequests = 0;
    std::uint64_t onePassBoardExported = 0;
    double onePassExportSeconds = 0;
    // The CPU board analyzer is the expensive half of an observation and is
    // independent per request, so it runs on its own bounded pool exactly like
    // the full-range fallback did.
    std::unique_ptr<BoardWorkerPool> onePassBoardPool;
    if (onePassBoardEnabled) {
        onePassBoardPool = std::make_unique<BoardWorkerPool>(
            std::max(1u, boardWorkerCount()));
    }
#endif

    if (!p1Samples.empty() && p1Samples.back().observation.cnnUsed) {
        trustedP1CnnObservation = p1Samples.back().observation;
    }
    if (!p2Samples.empty() && p2Samples.back().observation.cnnUsed) {
        trustedP2CnnObservation = p2Samples.back().observation;
    }

    const auto appendScan = [&](double time, const QueueObservation* p1Observation,
                                const QueueObservation* p2Observation) {
        if (settings.player1Enabled) {
            p1Samples.push_back({time, Cell::Empty, *p1Observation});
            if (incrementalPrefetchPhaseEnabled) {
                p1IncrementalPrefetchBuilder.push(time, *p1Observation);
            }
        }
        if (settings.player2Enabled) {
            p2Samples.push_back({time, Cell::Empty, *p2Observation});
            if (incrementalPrefetchPhaseEnabled) {
                p2IncrementalPrefetchBuilder.push(time, *p2Observation);
            }
        }
        ++scans;
    };

#if defined(__EMSCRIPTEN__) && !defined(TR_BROWSER_FULL)
    // Shared scheduling code records these durations even when the native
    // diagnostic logger and worker pool are absent.
    double prefetchPhaseRebuildSeconds = 0;
    double prefetchScheduleSeconds = 0;
    double prefetchRetainSeconds = 0;
    double finalQueueDecodeSeconds = 0;
    double prefetchWaitSeconds = 0;
#endif
    const auto recognizeQueues = [&](const Frame& source) {
        QueueObservation p1Legacy;
        QueueObservation p2Legacy;
        if (settings.player1Enabled) p1Legacy = p1Vision.observeQueue(source);
        if (settings.player2Enabled) p2Legacy = p2Vision.observeQueue(source);
#if defined(__EMSCRIPTEN__) && !defined(TR_BROWSER_FULL)
        // Browser queue inference is supplied before this recovery stage by
        // the WASM bridge. The native optional CNN is not linked into WASM.
        if (settings.player1Enabled) lastP1Observation = std::move(p1Legacy);
        if (settings.player2Enabled) lastP2Observation = std::move(p2Legacy);
        return;
#else
        if (!queueModel) {
            if (settings.player1Enabled) lastP1Observation = std::move(p1Legacy);
            if (settings.player2Enabled) lastP2Observation = std::move(p2Legacy);
            return;
        }

        queueCnnBatch.clear();
        queueDarkRatios.clear();
        std::size_t p1Offset = 0;
        std::size_t p2Offset = 0;
        if (settings.player1Enabled) {
            p1Offset = queueDarkRatios.size();
            p1Vision.appendQueueCnnInput(source, queueCnnBatch, queueDarkRatios,
                                        settings.queueCnnDarkPixelLimit);
        }
        if (settings.player2Enabled) {
            p2Offset = queueDarkRatios.size();
            p2Vision.appendQueueCnnInput(source, queueCnnBatch, queueDarkRatios,
                                        settings.queueCnnDarkPixelLimit);
        }
        std::string queueError;
        auto predictions = queueModel->infer(queueCnnBatch, queueDarkRatios.size(), queueError);
        if (holdModel && predictions.size() == queueDarkRatios.size()) {
            std::string holdError;
            const auto holdPredictions =
                holdModel->infer(queueCnnBatch, queueDarkRatios.size(), holdError);
            if (holdPredictions.size() == predictions.size()) {
                for (std::size_t i = 0; i < predictions.size(); ++i) {
                    if (i % 6 == 0) predictions[i] = holdPredictions[i];
                }
            }
        }
        std::vector<QueueCnnPrediction> labelPredictions;
        if (labelModel) {
            std::string labelError;
            labelPredictions = labelModel->infer(queueCnnBatch, queueDarkRatios.size(), labelError);
            if (labelPredictions.size() != predictions.size()) labelPredictions.clear();
        }
        const auto* labelPtr = labelPredictions.empty() ? nullptr : &labelPredictions;
        if (predictions.size() != queueDarkRatios.size()) {
            // The colour path and the global queue decoder are deliberately
            // retained as a safe runtime fallback. A broken optional NEXT
            // model must not make an otherwise recoverable video unusable.
            if (reusableModels) reusableModels->disableQueueModel();
            else ownedQueueModel.reset();
            queueModel = nullptr;
            status.setMessage("Queue CNN failed; continuing with legacy colour recognition: " + queueError);
            if (settings.player1Enabled) lastP1Observation = std::move(p1Legacy);
            if (settings.player2Enabled) lastP2Observation = std::move(p2Legacy);
            return;
        }
        if (settings.player1Enabled) {
            lastP1Observation = stabilizeQueueCnnObservation(
                p1Vision.applyQueueCnn(
                    p1Legacy, predictions, p1Offset, queueDarkRatios, p1Offset, settings, labelPtr),
                trustedP1CnnObservation);
        }
        if (settings.player2Enabled) {
            lastP2Observation = stabilizeQueueCnnObservation(
                p2Vision.applyQueueCnn(
                    p2Legacy, predictions, p2Offset, queueDarkRatios, p2Offset, settings, labelPtr),
                trustedP2CnnObservation);
        }
#endif
    };

#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    const auto enqueueQueueRecognition = [&](const Frame& source, double frameTime) {
        const auto preprocessStarted = std::chrono::steady_clock::now();
        PendingQueueRecognition pending;
        pending.frameTime = frameTime;
        if (settings.player1Enabled) pending.p1Legacy = p1Vision.observeQueue(source);
        if (settings.player2Enabled) pending.p2Legacy = p2Vision.observeQueue(source);
        if (settings.player1Enabled) {
            pending.p1Offset = queueDarkRatios.size();
            p1Vision.appendQueueCnnInput(source, queueCnnBatch, queueDarkRatios,
                                        settings.queueCnnDarkPixelLimit);
        }
        if (settings.player2Enabled) {
            pending.p2Offset = queueDarkRatios.size();
            p2Vision.appendQueueCnnInput(source, queueCnnBatch, queueDarkRatios,
                                        settings.queueCnnDarkPixelLimit);
        }
        pendingQueueRecognitions.push_back(std::move(pending));
        queuePreprocessSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - preprocessStarted).count();
    };
#endif

#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT) && defined(TR_NVDEC_DEFAULT)
    // Produce one board observation from the raw NV12 ring slot this scan just
    // filled. The BGRA export happens here (it owns the CUDA stream); the CPU
    // analyzer runs on the board worker pool so it overlaps with decoding.
    const auto submitOnePassBoard = [&](const BoardRequest& request,
                                        const D3D11BoardFrameHandle& handle,
                                        bool player1) {
        if (!onePassBoardEnabled) return false;
        if (!nvdecCudaPreprocessor->rawBoardRingEnabled()) {
            // Cannot export from this ring; leave the request to the bounded
            // repair so the whole scan still avoids a second full-range decode.
            std::lock_guard<std::mutex> lock(missingBoardMutex);
            missingBoardRequests.push_back({request, player1});
            return true;
        }
        VisionAnalyzer& vision = player1 ? p1Vision : p2Vision;
        // Mapped board pixels are read at full-frame coordinates, so the
        // retained crop must cover exactly the analyzer's own rectangle.
        const PixelRegion region = vision.boardPixelRegion();
        std::vector<std::uint8_t> bgra;
        std::string exportError;
        const auto exportStarted = std::chrono::steady_clock::now();
        if (!nvdecCudaPreprocessor->readBoardBgra(
                handle, player1, region.x, region.y, region.width,
                region.height, bgra, exportError)) {
            onePassExportSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - exportStarted).count();
            if (std::getenv("TR_GPU_DIAGNOSTICS")) {
                std::cerr << "onepass_board_export_fallback="
                          << (player1 ? 1 : 2) << " t=" << request.time
                          << " error=\"" << exportError << "\"\n";
            }
            std::lock_guard<std::mutex> lock(missingBoardMutex);
            missingBoardRequests.push_back({request, player1});
            return true;
        }
        onePassExportSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - exportStarted).count();
        ++onePassBoardRequests;
        ++onePassBoardExported;
        const std::int64_t sourceTime = handle.sourceTime100ns;
        auto slot = std::make_shared<PrefetchedBoardSlot>();
        slot->sourceTime100ns = sourceTime;
        if (boardFrameDiagnosticsEnabled()) {
            slot->frameOrdinal = handle.deliveryOrder;
            slot->frameHash = boardPixelFingerprint(bgra);
        }
        auto& destination = player1 ? boardPrefetch->p1 : boardPrefetch->p2;
        destination.emplace(boardRequestKey(request.time), slot);
        ++boardPrefetch->submittedRequests;
        if (onePassBoardPool) {
            onePassBoardPool->submit(
                [analyzer = &vision, bgra = std::move(bgra), region, request,
                 sourceTime, slot]() mutable {
                Frame frame;
                frame.width = region.width;
                frame.height = region.height;
                frame.sourceX = region.x;
                frame.sourceY = region.y;
                frame.time100ns = sourceTime;
                frame.bgra = std::move(bgra);
                BoardObservation observation = analyzer->analyzeBoard(frame);
                observation.timeSeconds = request.time;
                slot->observation = std::move(observation);
            });
        } else {
            Frame frame;
            frame.width = region.width;
            frame.height = region.height;
            frame.sourceX = region.x;
            frame.sourceY = region.y;
            frame.time100ns = sourceTime;
            frame.bgra = std::move(bgra);
            BoardObservation observation = vision.analyzeBoard(frame);
            observation.timeSeconds = request.time;
            slot->observation = std::move(observation);
        }
        return true;
    };
#endif

    const auto submitPrefetch = [&](const BoardRequest& request, bool player1) {
        if (!boardPrefetch || prefetchFailed.load() ||
            request.time < startSeconds - .000001) return;
        auto& destination = player1 ? boardPrefetch->p1 : boardPrefetch->p2;
        const std::int64_t key = boardRequestKey(request.time);
        if (destination.find(key) != destination.end()) return;
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT) && defined(TR_NVDEC_DEFAULT)
        if (nvdecBoardBatchEnabled && std::any_of(
                pendingNvdecBoardRequests.begin(),
                pendingNvdecBoardRequests.end(),
                [&](const PendingNvdecBoardRequest& pending) {
                    return pending.player1 == player1 &&
                        boardRequestKey(pending.request.time) == key;
                })) return;
#endif
        const double threshold = request.time - .000001;
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_GPU_ULTRA_DEFAULT)
        if (gpuBoardStreamEnabled) {
            bool boardProcessorReady = false;
#if defined(TR_NVDEC_DEFAULT)
            if (nvdecCudaDirectEnabled) {
                boardProcessorReady = nvdecCudaPreprocessor &&
                    nvdecCudaPreprocessor->ready();
            } else
#endif
            {
                boardProcessorReady = gpuBoardPreprocessor &&
                    gpuBoardPreprocessor->ready();
            }
            if (!boardProcessorReady || gpuBoardFrames.empty() ||
                latestEvictedFrameTime >= threshold) {
#if defined(TR_NVDEC_DEFAULT)
                if (onePassBoardEnabled) {
                    // The deliverable frame already left the ring. The one-pass
                    // path resolves it with a bounded local decode instead of
                    // discarding the whole scan.
                    std::lock_guard<std::mutex> lock(missingBoardMutex);
                    missingBoardRequests.push_back({request, player1});
                }
#endif
                return;
            }
            const auto source = std::find_if(
                gpuBoardFrames.begin(), gpuBoardFrames.end(),
                [threshold](const D3D11BoardFrameHandle& candidate) {
                    return static_cast<double>(candidate.sourceTime100ns) /
                               10000000.0 >= threshold;
                });
            bool sourceValid = source != gpuBoardFrames.end();
            if (sourceValid) {
#if defined(TR_NVDEC_DEFAULT)
                sourceValid = nvdecCudaDirectEnabled
                    ? nvdecCudaPreprocessor->valid(*source)
                    : gpuBoardPreprocessor->valid(*source);
#else
                sourceValid = gpuBoardPreprocessor->valid(*source);
#endif
            }
            if (!sourceValid) {
#if defined(TR_NVDEC_DEFAULT)
                if (onePassBoardEnabled) {
                    std::lock_guard<std::mutex> lock(missingBoardMutex);
                    missingBoardRequests.push_back({request, player1});
                }
#endif
                return;
            }
            // Record the selected delivery-order mapping before choosing the
            // scalar or batched inference path. Destination and pending-list
            // duplicate checks have already run above, so each accepted
            // request contributes exactly once in either mode.
            recordBoardRequestMapping(player1, request, *source);
#if defined(TR_NVDEC_DEFAULT)
            if (nvdecCudaDirectEnabled && onePassBoardEnabled &&
                submitOnePassBoard(request, *source, player1)) {
                return;
            }
            if (nvdecCudaDirectEnabled && nvdecBoardBatchEnabled) {
                // This pending handle is flushed before decoding resumes. The
                // ring generation therefore cannot be overwritten between
                // request-to-frame selection and tensor extraction.
                pendingNvdecBoardRequests.push_back(
                    {request, *source, player1});
                return;
            }
#endif
            const auto inferenceStarted = std::chrono::steady_clock::now();
            D3D11BoardInference inference;
            std::string gpuError;
            bool inferred = false;
#if defined(TR_NVDEC_DEFAULT)
            if (nvdecCudaDirectEnabled) {
                inferred = nvdecCudaPreprocessor->inferBoard(
                    *source, player1, inference, gpuError);
            } else
#endif
            {
                inferred = gpuBoardPreprocessor->infer(
                    *source, player1, inference, gpuError);
            }
            if (!inferred) {
                prefetchFailed.store(true);
                prefetchError = gpuError.empty()
                    ? "GPU board prefetch failed" : std::move(gpuError);
                return;
            }
            gpuBoardInferenceSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - inferenceStarted).count();
            Frame timingFrame;
            timingFrame.time100ns = inference.sourceTime100ns;
            auto slot = std::make_shared<PrefetchedBoardSlot>();
            slot->sourceTime100ns = inference.sourceTime100ns;
            if (boardFrameDiagnosticsEnabled()) slot->frameOrdinal = inference.deliveryOrder;
            VisionAnalyzer& vision = player1 ? p1Vision : p2Vision;
            slot->observation = vision.analyzeBoardWithProbabilities(
                timingFrame, inference.probabilities, inference.classic, true);
            slot->observation.timeSeconds = request.time;
            destination.emplace(key, std::move(slot));
            ++boardPrefetch->submittedRequests;
            return;
        }
#endif
        if (!prefetchWorkers) return;
        // 1.mov contains presentation timestamps that are not monotonic in
        // Source Reader delivery order.  The exact second pass selects the
        // first *delivered* frame at/after the request, so a timestamp-sorted
        // lower_bound silently selected a later frame.  Preserve delivery
        // order here.  If a qualifying frame was evicted, leave the slot
        // missing and let the caller perform the exact second pass.
        if (recentFrames.empty() || latestEvictedFrameTime >= threshold) return;
        const auto source = std::find_if(recentFrames.begin(), recentFrames.end(),
            [threshold](const std::shared_ptr<RetainedPrefetchFrame>& candidate) {
                return static_cast<double>(candidate->time100ns) / 10000000.0 >= threshold;
            });
        if (source == recentFrames.end()) return;
        auto slot = std::make_shared<PrefetchedBoardSlot>();
        slot->sourceTime100ns = (*source)->time100ns;
        if (boardFrameDiagnosticsEnabled()) {
            slot->frameHash = boardPixelFingerprint(
                player1 ? (*source)->p1.bgra : (*source)->p2.bgra);
        }
        destination.emplace(key, slot);
        ++boardPrefetch->submittedRequests;
        prefetchWorkers->submit([&, frame = *source, slot, player1, request] {
            if (prefetchFailed.load() || status.cancel.load()) return;
            const auto workerStarted = std::chrono::steady_clock::now();
            try {
                VisionAnalyzer& vision = player1 ? p1Vision : p2Vision;
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
                slot->observation = vision.analyzeBrowserGpuRetainedBoard(player1 ? frame->p1 : frame->p2);
#else
                slot->observation = vision.analyzeRetainedBoard(player1 ? frame->p1 : frame->p2);
#endif
                slot->observation.timeSeconds = request.time;
                if (!slot->observation.recognitionError.empty()) {
                    if (!prefetchFailed.exchange(true)) {
                        std::lock_guard<std::mutex> lock(prefetchErrorMutex);
                        prefetchError = slot->observation.recognitionError;
                    }
                }
            } catch (const std::exception& exception) {
                if (!prefetchFailed.exchange(true)) {
                    std::lock_guard<std::mutex> lock(prefetchErrorMutex);
                    prefetchError = std::string("Board prefetch failed: ") + exception.what();
                }
            } catch (...) {
                if (!prefetchFailed.exchange(true)) {
                    std::lock_guard<std::mutex> lock(prefetchErrorMutex);
                    prefetchError = "Board prefetch failed";
                }
            }
            prefetchWorkerNanoseconds.fetch_add(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - workerStarted).count()),
                std::memory_order_relaxed);
        });
    };

    const auto flushNvdecBoardBatch = [&]() -> bool {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT) && defined(TR_NVDEC_DEFAULT)
        if (!nvdecBoardBatchEnabled || pendingNvdecBoardRequests.empty()) {
            return true;
        }
        if (!nvdecCudaPreprocessor ||
            nvdecCudaPreprocessor->boardBatchCapacity() == 0) {
            prefetchFailed.store(true);
            prefetchError = "NVDEC board batch preprocessor is not ready";
            pendingNvdecBoardRequests.clear();
            return false;
        }
        const std::size_t capacity =
            nvdecCudaPreprocessor->boardBatchCapacity();
        for (std::size_t first = 0;
             first < pendingNvdecBoardRequests.size(); first += capacity) {
            const std::size_t count = std::min(
                capacity, pendingNvdecBoardRequests.size() - first);
            std::vector<NvdecCudaBoardRequest> requests;
            requests.reserve(count);
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto& pending =
                    pendingNvdecBoardRequests[first + offset];
                requests.push_back({pending.source, pending.player1});
            }

            const auto inferenceStarted = std::chrono::steady_clock::now();
            std::vector<D3D11BoardInference> inferences;
            std::string gpuError;
            bool inferred = nvdecCudaPreprocessor->inferBoards(
                requests, inferences, gpuError);
            ++nvdecBoardBatchFlushes;
            nvdecBoardBatchRequests += count;
            nvdecBoardBatchMaximum = std::max(
                nvdecBoardBatchMaximum, count);
            if (!inferred || inferences.size() != count) {
                // Batching is only an optimization. A fixed-batch ONNX model,
                // provider hiccup, or unsupported dynamic shape must not make
                // an otherwise valid recovery fail. The retained handles are
                // still valid here because decoding cannot resume until this
                // flush returns, so retry the exact same ordered requests on
                // the established scalar path.
                ++nvdecBoardBatchFallbacks;
                const std::string batchError = gpuError;
                inferences.clear();
                inferences.reserve(count);
                inferred = true;
                for (const auto& request : requests) {
                    D3D11BoardInference scalarInference;
                    std::string scalarError;
                    if (!nvdecCudaPreprocessor->inferBoard(
                            request.handle, request.player1,
                            scalarInference, scalarError)) {
                        inferred = false;
                        gpuError = scalarError.empty() ? batchError : scalarError;
                        break;
                    }
                    inferences.push_back(std::move(scalarInference));
                }
            }
            gpuBoardInferenceSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - inferenceStarted).count();
            if (!inferred || inferences.size() != count) {
                prefetchFailed.store(true);
                prefetchError = gpuError.empty()
                    ? "GPU board batch prefetch failed" : std::move(gpuError);
                pendingNvdecBoardRequests.clear();
                return false;
            }

            // `inferBoards` returns in request order. Keep the complete
            // (player, phase, sample-time, delivery ordinal, source PTS)
            // association in the pending item and selected handle until the
            // observation is committed to its exact request key.
            const auto commitStarted = std::chrono::steady_clock::now();
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto& pending =
                    pendingNvdecBoardRequests[first + offset];
                const auto& inference = inferences[offset];
                Frame timingFrame;
                timingFrame.time100ns = inference.sourceTime100ns;
                auto slot = std::make_shared<PrefetchedBoardSlot>();
                slot->sourceTime100ns = inference.sourceTime100ns;
                if (boardFrameDiagnosticsEnabled()) {
                    slot->frameOrdinal = inference.deliveryOrder;
                }
                VisionAnalyzer& vision = pending.player1 ? p1Vision : p2Vision;
                slot->observation = vision.analyzeBoardWithProbabilities(
                    timingFrame, inference.probabilities,
                    inference.classic, true);
                slot->observation.timeSeconds = pending.request.time;
                auto& destination = pending.player1
                    ? boardPrefetch->p1 : boardPrefetch->p2;
                destination.emplace(
                    boardRequestKey(pending.request.time), std::move(slot));
                ++boardPrefetch->submittedRequests;
            }
            boardCommitSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - commitStarted).count();
        }
        pendingNvdecBoardRequests.clear();
#endif
        return true;
    };

    const auto schedulePrefetch = [&](const std::vector<QueuePhase>& phases,
                                      bool player1, bool includeTail) {
        if (phases.empty()) return;
        const std::size_t count = includeTail ? phases.size() : phases.size() - 1;
        std::vector<QueuePhase> closed(phases.begin(), phases.begin() + count);
        for (const BoardRequest& request : makeBoardRequests(closed, settings.onnxSamples)) {
            submitPrefetch(request, player1);
        }
    };

    const auto confirmedChanged = [&](const std::vector<QueueRecognitionSample>& samples,
                                      std::optional<QueueObservation>& confirmed) {
        const std::size_t needed = static_cast<std::size_t>(resolveQueueConfirmSamples(settings));
        if (samples.size() < needed) return false;
        const QueueObservation& candidate = samples.back().observation;
        for (std::size_t offset = 1; offset < needed; ++offset) {
            if (!sameQueue(candidate, samples[samples.size() - 1 - offset].observation)) return false;
        }
        if (confirmed && sameQueue(*confirmed, candidate)) return false;
        confirmed = candidate;
        return true;
    };

    const auto prefetchClosedPhases = [&](double currentEnd, bool includeTail,
                                          bool refreshP1, bool refreshP2) {
        if (!boardPrefetch) return;
        bool havePrefetchFrame = !recentFrames.empty();
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_GPU_ULTRA_DEFAULT)
        if (gpuBoardStreamEnabled) havePrefetchFrame = !gpuBoardFrames.empty();
#endif
        if (!havePrefetchFrame) return;
        std::vector<QueuePhase> currentP1;
        std::vector<QueuePhase> currentP2;
        if (settings.player1Enabled && refreshP1) {
            const auto rebuildStarted = std::chrono::steady_clock::now();
            if (incrementalPrefetchPhaseEnabled) {
                currentP1 = p1IncrementalPrefetchBuilder.preview(currentEnd);
            } else {
                auto copy = p1Samples;
                currentP1 = buildQueuePhasesFromSamples(copy, settings, currentEnd);
            }
            prefetchPhaseRebuildSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - rebuildStarted).count();
            const auto scheduleStarted = std::chrono::steady_clock::now();
            schedulePrefetch(currentP1, true, includeTail);
            prefetchScheduleSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - scheduleStarted).count();
        }
        if (settings.player2Enabled && refreshP2) {
            const auto rebuildStarted = std::chrono::steady_clock::now();
            if (incrementalPrefetchPhaseEnabled) {
                currentP2 = p2IncrementalPrefetchBuilder.preview(currentEnd);
            } else {
                auto copy = p2Samples;
                currentP2 = buildQueuePhasesFromSamples(copy, settings, currentEnd);
            }
            prefetchPhaseRebuildSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - rebuildStarted).count();
            const auto scheduleStarted = std::chrono::steady_clock::now();
            schedulePrefetch(currentP2, false, includeTail);
            prefetchScheduleSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - scheduleStarted).count();
        }
        // Do not carry generation-tagged handles into the next decode
        // callback: the ring may recycle their slots there. This boundary
        // combines both players (normally up to six requests) while keeping
        // every selected delivery-order frame immutable until inference ends.
        if (!flushNvdecBoardBatch()) return;
        // Do not prune at the currently decoded open-phase boundary.  The
        // global queue decoder can move that boundary backwards after later
        // evidence arrives.  The byte cap in the decode loop bounds memory;
        // requests older than that window deliberately fall back above.
    };

#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    // TR_QUEUE_SCHEDULE: 旧解析の「NEXT変更時刻」を外部TSVで与え、
    // その時刻だけでキュー観測を更新する実験用モード。
    // 形式: match_index<TAB>start_seconds<TAB>p1_times(カンマ区切り)<TAB>p2_times
    std::vector<double> scheduleP1, scheduleP2;
    bool queueScheduleEnabled = false;
    if (const char* schedulePath = std::getenv("TR_QUEUE_SCHEDULE")) {
        std::ifstream scheduleFile(schedulePath);
        std::string line;
        while (std::getline(scheduleFile, line)) {
            std::istringstream row(line);
            std::string indexText, startText, p1Text, p2Text;
            if (!std::getline(row, indexText, '\t')) continue;
            if (!std::getline(row, startText, '\t')) continue;
            if (!std::getline(row, p1Text, '\t')) continue;
            if (!std::getline(row, p2Text, '\t')) continue;
            double rowStart = 0;
            try { rowStart = std::stod(startText); } catch (...) { continue; }
            if (std::abs(rowStart - startSeconds) > 0.6) continue;
            const auto parseTimes = [](const std::string& text) {
                std::vector<double> out;
                std::stringstream ss(text);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    if (item.empty()) continue;
                    try { out.push_back(std::stod(item)); } catch (...) {}
                }
                std::sort(out.begin(), out.end());
                return out;
            };
            scheduleP1 = parseTimes(p1Text);
            scheduleP2 = parseTimes(p2Text);
            queueScheduleEnabled = true;
            std::fprintf(stderr, "[sched] loaded start=%.3f p1=%zu p2=%zu (engine start=%.3f)\n",
                         rowStart, scheduleP1.size(), scheduleP2.size(), startSeconds);
            std::fflush(stderr);
            break;
        }
    }
    std::size_t nextScheduleP1 = 0;
    std::size_t nextScheduleP2 = 0;
    std::optional<QueueObservation> frozenP1;
    std::optional<QueueObservation> frozenP2;
    const auto commitQueueRecognitions = [&](std::vector<PendingQueueRecognition>& pendingBatch,
                                              const std::vector<QueueCnnPrediction>& predictions,
                                              const std::vector<double>& darkRatios,
                                              bool cnnReady,
                                              const std::vector<QueueCnnPrediction>* labelPtr) {
        bool refreshP1Prefetch = false;
        bool refreshP2Prefetch = false;
        double refreshEnd = 0;
        const bool schedDebug = std::getenv("TR_QUEUE_SCHEDULE_DEBUG") != nullptr;
        for (auto& pending : pendingBatch) {
            const double relativeTime = pending.frameTime - startSeconds;
            // 予定時刻かどうか（観測の更新自体は毎フレーム行う）
            const bool fireP1 = queueScheduleEnabled &&
                (nextScheduleP1 < scheduleP1.size() && relativeTime + 1e-6 >= scheduleP1[nextScheduleP1]);
            const bool fireP2 = queueScheduleEnabled &&
                (nextScheduleP2 < scheduleP2.size() && relativeTime + 1e-6 >= scheduleP2[nextScheduleP2]);
            if (schedDebug) {
                std::fprintf(stderr, "[sched] rel=%.3f fire1=%d fire2=%d n1=%zu/%zu n2=%zu/%zu\n",
                             relativeTime, static_cast<int>(fireP1), static_cast<int>(fireP2),
                             nextScheduleP1, scheduleP1.size(), nextScheduleP2, scheduleP2.size());
                std::fflush(stderr);
            }
            if (cnnReady) {
                if (settings.player1Enabled) {
                    auto p1Cnn = p1Vision.applyQueueCnn(pending.p1Legacy, predictions, pending.p1Offset,
                                                        darkRatios, pending.p1Offset, settings, labelPtr);
                    if (schedDebug) { std::fprintf(stderr, "[sched] p1 observed\n"); std::fflush(stderr); }
                    auto stabilized = stabilizeQueueCnnObservation(std::move(p1Cnn),
                                                                  trustedP1CnnObservation);
                    if (queueScheduleEnabled) {
                        if (fireP1) {
                            // 予定時刻: 現在の観測をそのまま採用して凍結解除
                            lastP1Observation = stabilized;
                            frozenP1 = stabilized;
                            ++nextScheduleP1;
                        } else if (frozenP1) {
                            // 予定時刻まで、出力は前回の値に凍結
                            lastP1Observation = frozenP1;
                        } else {
                            lastP1Observation = stabilized;
                            frozenP1 = stabilized;
                        }
                    } else {
                        lastP1Observation = stabilized;
                    }
                }
                if (settings.player2Enabled) {
                    auto p2Cnn = p2Vision.applyQueueCnn(pending.p2Legacy, predictions, pending.p2Offset,
                                                        darkRatios, pending.p2Offset, settings, labelPtr);
                    auto stabilized2 = stabilizeQueueCnnObservation(std::move(p2Cnn),
                                                                    trustedP2CnnObservation);
                    if (queueScheduleEnabled) {
                        if (fireP2) {
                            lastP2Observation = stabilized2;
                            frozenP2 = stabilized2;
                            ++nextScheduleP2;
                        } else if (frozenP2) {
                            lastP2Observation = frozenP2;
                        } else {
                            lastP2Observation = stabilized2;
                            frozenP2 = stabilized2;
                        }
                    } else {
                        lastP2Observation = stabilized2;
                    }
                }
            } else {
                if (settings.player1Enabled) lastP1Observation = std::move(pending.p1Legacy);
                if (settings.player2Enabled) lastP2Observation = std::move(pending.p2Legacy);
            }
            while (nextScan <= pending.frameTime + .000001 &&
                   (duration <= 0 || nextScan < analysisEnd)) {
                appendScan(nextScan,
                           lastP1Observation ? &*lastP1Observation : nullptr,
                           lastP2Observation ? &*lastP2Observation : nullptr);
                nextScan += settings.sampleIntervalSeconds;
            }
            if (boardPrefetch) {
                const bool p1PhaseChanged = settings.player1Enabled &&
                    confirmedChanged(p1Samples, confirmedP1);
                const bool p2PhaseChanged = settings.player2Enabled &&
                    confirmedChanged(p2Samples, confirmedP2);
                refreshP1Prefetch = refreshP1Prefetch || p1PhaseChanged;
                refreshP2Prefetch = refreshP2Prefetch || p2PhaseChanged;
                refreshEnd = std::min(pending.frameTime, analysisEnd);
            }
        }
        // A queue inference batch preserves delivery order. Rebuilding the
        // global 7-bag prefix after every one of its eight frames repeated the
        // same O(history) work several times. Refresh each changed player at
        // most once per batch; the maximum scheduling delay is one batch
        // (~0.21 s for 1.mov) and is included in the 320 MiB ring budget.
        pendingP1PrefetchRefresh = pendingP1PrefetchRefresh || refreshP1Prefetch;
        pendingP2PrefetchRefresh = pendingP2PrefetchRefresh || refreshP2Prefetch;
        if ((pendingP1PrefetchRefresh || pendingP2PrefetchRefresh) &&
            refreshEnd >= lastPrefetchRefreshEnd + PrefetchRefreshIntervalSeconds) {
            prefetchClosedPhases(refreshEnd, false,
                                 pendingP1PrefetchRefresh, pendingP2PrefetchRefresh);
            pendingP1PrefetchRefresh = false;
            pendingP2PrefetchRefresh = false;
            lastPrefetchRefreshEnd = refreshEnd;
        }
        pendingBatch.clear();
    };

    const auto flushQueueRecognitions = [&]() -> bool {
        if (pendingQueueRecognitions.empty()) return true;
        ++queueInferenceFlushes;
        std::string queueError;
        const auto inferenceStarted = std::chrono::steady_clock::now();
        auto predictions = queueModel
            ? queueModel->infer(queueCnnBatch, queueDarkRatios.size(), queueError)
            : std::vector<QueueCnnPrediction>{};
        if (holdModel && predictions.size() == queueDarkRatios.size()) {
            std::string holdError;
            const auto holdPredictions =
                holdModel->infer(queueCnnBatch, queueDarkRatios.size(), holdError);
            if (holdPredictions.size() == predictions.size()) {
                for (std::size_t i = 0; i < predictions.size(); ++i) {
                    if (i % 6 == 0) predictions[i] = holdPredictions[i];
                }
            }
        }
        std::vector<QueueCnnPrediction> labelPredictions;
        if (labelModel && !predictions.empty()) {
            std::string labelError;
            labelPredictions = labelModel->infer(queueCnnBatch, queueDarkRatios.size(), labelError);
            if (labelPredictions.size() != predictions.size()) labelPredictions.clear();
        }
        const auto* labelPtr = labelPredictions.empty() ? nullptr : &labelPredictions;
        queueInferenceSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - inferenceStarted).count();
        const bool cnnReady = queueModel && predictions.size() == queueDarkRatios.size();
        if (!cnnReady && queueModel) {
            if (reusableModels) reusableModels->disableQueueModel();
            else ownedQueueModel.reset();
            queueModel = nullptr;
            status.setMessage("Queue CNN failed; continuing with legacy colour recognition: " + queueError);
        }
        commitQueueRecognitions(pendingQueueRecognitions, predictions, queueDarkRatios, cnnReady, labelPtr);
        queueCnnBatch.clear();
        queueDarkRatios.clear();
        return true;
    };

#if defined(_WIN32) && defined(TR_GPU_ULTRA_DEFAULT)
    const auto commitGpuQueueBatch = [&](std::vector<PendingQueueRecognition>& pendingBatch,
                                          D3D11QueueBatchResult& gpuResult) -> bool {
        if (gpuResult.frames.size() != pendingBatch.size() ||
            gpuResult.predictions.size() != gpuResult.frames.size() *
                                               D3D11QueuePreprocessor::SlotsPerFrame) {
            error = "GPU queue preprocessing returned an inconsistent batch";
            return false;
        }
        std::vector<double> gpuDarkRatios(
            gpuResult.frames.size() * D3D11QueuePreprocessor::SlotsPerFrame);
        for (std::size_t frameIndex = 0; frameIndex < gpuResult.frames.size(); ++frameIndex) {
            auto& pending = pendingBatch[frameIndex];
            const std::size_t frameOffset = frameIndex * D3D11QueuePreprocessor::SlotsPerFrame;
            pending.p1Offset = frameOffset;
            pending.p2Offset = frameOffset + 6;
            const auto makeLegacy = [&](std::size_t slotOffset, const VisionAnalyzer& vision) {
                std::array<std::array<std::uint64_t, 4>, 6> colorStats{};
                for (std::size_t slot = 0; slot < 6; ++slot) {
                    const auto& source = gpuResult.frames[frameIndex].slots[slotOffset + slot];
                    colorStats[slot] = {
                        source.sumR, source.sumG, source.sumB, source.colorCount
                    };
                }
                return vision.observeQueueFromColorStats(colorStats);
            };
            if (settings.player1Enabled) pending.p1Legacy = makeLegacy(0, p1Vision);
            if (settings.player2Enabled) pending.p2Legacy = makeLegacy(6, p2Vision);
            for (std::size_t slot = 0; slot < D3D11QueuePreprocessor::SlotsPerFrame; ++slot) {
                const auto& source = gpuResult.frames[frameIndex].slots[slot];
                gpuDarkRatios[frameOffset + slot] = source.cropPixelCount
                    ? static_cast<double>(source.darkCount) / source.cropPixelCount
                    : 0.0;
            }
        }
        commitQueueRecognitions(pendingBatch, gpuResult.predictions, gpuDarkRatios, true, nullptr);
        return true;
    };

    const auto flushGpuQueueRecognitions = [&]() -> bool {
        if (pendingQueueRecognitions.empty()) return true;
        ++queueInferenceFlushes;
        bool pipelined = false;
#if defined(TR_NVDEC_DEFAULT)
        if (nvdecCudaDirectEnabled) {
            pipelined = nvdecCudaPreprocessor &&
                nvdecCudaPreprocessor->pipelineEnabled();
        } else
#endif
        {
            pipelined = gpuQueuePreprocessor &&
                gpuQueuePreprocessor->pipelineEnabled();
        }
        if (pipelined) {
            gpuQueuePipelinePending.push_back(std::move(pendingQueueRecognitions));
            pendingQueueRecognitions.clear();
            pendingQueueRecognitions.reserve(static_cast<std::size_t>(queueFrameBatch));
        }
        D3D11QueueBatchResult gpuResult;
        const auto inferenceStarted = std::chrono::steady_clock::now();
        bool flushed = false;
#if defined(TR_NVDEC_DEFAULT)
        if (nvdecCudaDirectEnabled) {
            flushed = nvdecCudaPreprocessor &&
                nvdecCudaPreprocessor->flushQueue(gpuResult, error);
        } else
#endif
        {
            flushed = gpuQueuePreprocessor &&
                gpuQueuePreprocessor->flush(gpuResult, error);
        }
        if (!flushed) {
            if (error.empty()) error = "GPU queue preprocessor is not ready";
            return false;
        }
        queueInferenceSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - inferenceStarted).count();
        adaptiveQueueInferredFrames += gpuResult.inferredFrames;
        adaptiveQueueReusedFrames += gpuResult.reusedFrames;
        adaptiveQueueInferredSlots += gpuResult.inferredSlots;
        adaptiveQueueReusedSlots += gpuResult.reusedSlots;
        adaptiveQueueInferenceRuns += gpuResult.inferenceRuns;
        backgroundQueueInferenceSeconds += gpuResult.backgroundInferenceSeconds;
        if (pipelined) {
            if (gpuResult.frames.empty()) return true;
            if (gpuQueuePipelinePending.empty()) {
                error = "GPU queue pipeline returned a batch with no pending observations";
                return false;
            }
            auto completedPending = std::move(gpuQueuePipelinePending.front());
            gpuQueuePipelinePending.pop_front();
            return commitGpuQueueBatch(completedPending, gpuResult);
        }
        return commitGpuQueueBatch(pendingQueueRecognitions, gpuResult);
    };

    const auto finishGpuQueuePipeline = [&]() -> bool {
        bool pipelined = false;
#if defined(TR_NVDEC_DEFAULT)
        if (nvdecCudaDirectEnabled) {
            pipelined = nvdecCudaPreprocessor &&
                nvdecCudaPreprocessor->pipelineEnabled();
        } else
#endif
        {
            pipelined = gpuQueuePreprocessor &&
                gpuQueuePreprocessor->pipelineEnabled();
        }
        if (!pipelined) return true;
        D3D11QueueBatchResult gpuResult;
        const auto waitStarted = std::chrono::steady_clock::now();
        bool finished = false;
#if defined(TR_NVDEC_DEFAULT)
        if (nvdecCudaDirectEnabled) {
            finished = nvdecCudaPreprocessor->finishQueuePipeline(
                gpuResult, error);
        } else
#endif
        {
            finished = gpuQueuePreprocessor->finishPipeline(gpuResult, error);
        }
        if (!finished) return false;
        queueInferenceSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - waitStarted).count();
        if (!gpuResult.frames.empty()) {
            adaptiveQueueInferredFrames += gpuResult.inferredFrames;
            adaptiveQueueReusedFrames += gpuResult.reusedFrames;
            adaptiveQueueInferredSlots += gpuResult.inferredSlots;
            adaptiveQueueReusedSlots += gpuResult.reusedSlots;
            adaptiveQueueInferenceRuns += gpuResult.inferenceRuns;
            backgroundQueueInferenceSeconds += gpuResult.backgroundInferenceSeconds;
            if (gpuQueuePipelinePending.empty()) {
                error = "GPU queue pipeline finished with no pending observations";
                return false;
            }
            auto completedPending = std::move(gpuQueuePipelinePending.front());
            gpuQueuePipelinePending.pop_front();
            if (!commitGpuQueueBatch(completedPending, gpuResult)) return false;
        }
        if (!gpuQueuePipelinePending.empty()) {
            error = "GPU queue pipeline left uncommitted observation batches";
            return false;
        }
        return true;
    };
#endif
#endif

    while (!eos) {
        if (status.cancel.load()) { error = "Recovery cancelled"; return false; }
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
        const auto queueReadStarted = std::chrono::steady_clock::now();
#endif
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_GPU_ULTRA_DEFAULT)
        if (gpuQueuePreprocessEnabled) {
            std::int64_t gpuTimestamp100ns = 0;
            bool deliveredGpuFrame = false;
            double gpuAppendSeconds = 0;
#if defined(TR_NVDEC_DEFAULT)
            if (nvdecCudaDirectEnabled) {
                if (nvdecMappedZeroCopyEnabled &&
                    !nvdecCudaPreprocessor->ready()) {
                    OnnxBoardModel* boardModel = p1Model ? p1Model : p2Model;
                    if (!nvdecCudaPreprocessor->initialize(
                            reader.width(), reader.height(), settings,
                            boardModel, queueModel,
                            maxPrefetchPayloadBytes, error)) {
                        return false;
                    }
                }
                if (!reader.readCudaNv12Sample(
                        gpuTimestamp100ns, eos, error,
                        [&](const void* nv12, std::size_t pitch,
                            int sourceHeight, int matrix, bool fullRange,
                            std::int64_t timestamp100ns,
                            std::string& callbackError) {
                            // 2026-11-06: 同一PTSのフレームは丸ごと捨てる（無駄取り排除）
                            static thread_local std::int64_t lastDeliveredTime100ns = -1;
                            const bool queueDedupe = [] {
                                const char* v = std::getenv("TR_QUEUE_DEDUPE");
                                return v && std::string(v) != "0";
                            }();
                            if (queueDedupe && lastDeliveredTime100ns == timestamp100ns) return true;
                            lastDeliveredTime100ns = timestamp100ns;
                            deliveredGpuFrame = true;
                            ++deliveredQueueFrames;
                            const double frameTime =
                                static_cast<double>(timestamp100ns) / 10000000.0;
                            // 2026-11-06: 毎フレームNEXT認識する（nextScan の位相ずれで
                            // 半分のフレームが間引かれていたのを修正）。
                            const bool scanDue =
                                (duration <= 0 || frameTime < analysisEnd) &&
                                ((deliveredQueueFrames - 1) %
                                     static_cast<std::uint64_t>(nvdecQueueFrameStride) == 0);
                            ++queueCallbackFrames;
                            // 2026-11-06: 同じPTSのフレームは2回処理しない（無駄取り排除）
                            const bool duplicateFrame =
                                queueLastFrameTime >= 0.0 &&
                                std::abs(frameTime - queueLastFrameTime) < 1e-6;
                            if (duplicateFrame) ++queueDuplicateFrames;
                            queueLastFrameTime = frameTime;
                            if (scanDue) ++queueScanFrames;
                            if (!nvdecCudaPreprocessor->ready()) {
                                OnnxBoardModel* boardModel = p1Model ? p1Model : p2Model;
                                if (!nvdecCudaPreprocessor->initialize(
                                        reader.width(), reader.height(), settings,
                                        boardModel, queueModel,
                                        maxPrefetchPayloadBytes, callbackError)) {
                                    return false;
                                }
                            }
                            const bool retainBoard =
                                ((deliveredQueueFrames - 1) %
                                     static_cast<std::uint64_t>(
                                         nvdecBoardFrameStride) == 0);
                            if (retainBoard && gpuBoardFrames.size() >=
                                nvdecCudaPreprocessor->capacity()) {
                                latestEvictedFrameTime = std::max(
                                    latestEvictedFrameTime,
                                    static_cast<double>(
                                        gpuBoardFrames.front().sourceTime100ns) /
                                        10000000.0);
                                gpuBoardFrames.pop_front();
                                ++boardPrefetch->evictedFrames;
                            }
                            const auto preprocessStarted =
                                std::chrono::steady_clock::now();
                            D3D11BoardFrameHandle retained;
                            if (!nvdecCudaPreprocessor->processFrame(
                                    nv12, pitch, sourceHeight, matrix, fullRange,
                                    timestamp100ns, gpuBoardDeliveryOrder++,
                                    retainBoard, scanDue,
                                    retained, callbackError)) {
                                return false;
                            }
                            if (playerNameFrame &&
                                !playerNameCaptureAttempted &&
                                frameTime + .000001 >= startSeconds) {
                                playerNameCaptureAttempted = true;
                                std::string captureError;
                                if (!capturePlayerNameFrameFromNv12(
                                        nv12, pitch, reader.width(),
                                        reader.height(), sourceHeight, matrix,
                                        fullRange,
                                        nvdecCudaPreprocessor->cudaStreamHandle(),
                                        settings, *playerNameFrame,
                                        captureError) &&
                                    std::getenv("TR_GPU_DIAGNOSTICS")) {
                                    std::cerr
                                        << "player_name_reuse_warning=\""
                                        << captureError << "\"\n";
                                }
                            }
                            const double preprocessElapsed =
                                std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() -
                                    preprocessStarted).count();
                            gpuBoardRetainSeconds += preprocessElapsed;
                            if (scanDue) gpuAppendSeconds += preprocessElapsed;
                            if (retainBoard) {
                                gpuBoardFrames.push_back(retained);
                                ++boardPrefetch->retainedFrames;
                                boardPrefetch->ringPeakBytes = std::max(
                                    boardPrefetch->ringPeakBytes,
                                    gpuBoardFrames.size() *
                                        nvdecCudaPreprocessor->bytesPerFrame());
                            }
                            if (scanDue) {
                                PendingQueueRecognition pending;
                                pending.frameTime = frameTime;
                                pendingQueueRecognitions.push_back(
                                    std::move(pending));
                                ++inferredQueueFrames;
                                // DecodeMapped may deliver more than one
                                // display frame for a packet (notably during
                                // EOS drain). Apply the same batch boundary
                                // before accepting another callback so the
                                // fixed 32-frame CUDA queue tensor cannot
                                // overflow and phase commits retain delivery
                                // order.
                                if (nvdecMappedZeroCopyEnabled &&
                                    pendingQueueRecognitions.size() >=
                                        static_cast<std::size_t>(
                                            queueFrameBatch) &&
                                    !flushGpuQueueRecognitions()) {
                                    callbackError = error;
                                    return false;
                                }
                            }
                            return true;
                        }, nvdecCudaPreprocessor->cudaStreamHandle())) {
                    return false;
                }
            } else
#endif
            if (!reader.readD3D11Sample(
                    gpuTimestamp100ns, eos, error,
                    [&](ID3D11Device* device, ID3D11DeviceContext* context,
                        ID3D11Texture2D* texture, UINT subresource,
                        std::int64_t timestamp100ns, std::string& callbackError) {
                        deliveredGpuFrame = true;
                        ++deliveredQueueFrames;
                        const double frameTime = static_cast<double>(timestamp100ns) / 10000000.0;
                        const bool scanDue = nextScan <= frameTime + .000001 &&
                            (duration <= 0 || nextScan < analysisEnd);
                        bool fusedFrame = false;
#if defined(_WIN32) && defined(TR_GPU_ULTRA_DEFAULT)
                        if (gpuBoardStreamEnabled) {
                            const auto retainStarted = std::chrono::steady_clock::now();
                            if (!gpuBoardPreprocessor->ready() &&
                                !gpuBoardPreprocessor->initialize(
                                    device, context, reader.width(), reader.height(), settings,
                                    p1Model, maxPrefetchPayloadBytes, callbackError)) {
                                return false;
                            }
                            if (gpuBoardFrames.size() >= gpuBoardPreprocessor->capacity()) {
                                latestEvictedFrameTime = std::max(
                                    latestEvictedFrameTime,
                                    static_cast<double>(gpuBoardFrames.front().sourceTime100ns) /
                                        10000000.0);
                                gpuBoardFrames.pop_front();
                                ++boardPrefetch->evictedFrames;
                            }
                            D3D11BoardFrameHandle retained;
                            if (scanDue && gpuFusedPreprocessEnabled) {
                                if (!gpuQueuePreprocessor->ready() &&
                                    !gpuQueuePreprocessor->initialize(
                                        device, context, reader.width(), reader.height(),
                                        settings, queueModel, callbackError)) {
                                    return false;
                                }
                                if (!gpuQueuePreprocessor->appendFused(
                                        texture, subresource, *gpuBoardPreprocessor,
                                        timestamp100ns, gpuBoardDeliveryOrder++, retained,
                                        callbackError)) {
                                    return false;
                                }
                                fusedFrame = true;
                            } else if (!gpuBoardPreprocessor->retain(
                                           texture, subresource, timestamp100ns,
                                           gpuBoardDeliveryOrder++, retained, callbackError)) {
                                return false;
                            }
                            gpuBoardFrames.push_back(retained);
                            ++boardPrefetch->retainedFrames;
                            boardPrefetch->ringPeakBytes = std::max(
                                boardPrefetch->ringPeakBytes,
                                gpuBoardFrames.size() *
                                    gpuBoardPreprocessor->bytesPerFrame());
                            gpuBoardRetainSeconds += std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - retainStarted).count();
                        }
#endif
                        if (!scanDue) return true;
                        const auto appendStarted = std::chrono::steady_clock::now();
                        if (!gpuQueuePreprocessor->ready() &&
                            !gpuQueuePreprocessor->initialize(
                                device, context, reader.width(), reader.height(),
                                settings, queueModel, callbackError)) {
                            return false;
                        }
                        if (!fusedFrame &&
                            !gpuQueuePreprocessor->append(texture, subresource, callbackError)) {
                            return false;
                        }
                        PendingQueueRecognition pending;
                        pending.frameTime = frameTime;
                        pendingQueueRecognitions.push_back(std::move(pending));
                        ++inferredQueueFrames;
                        gpuAppendSeconds += std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - appendStarted).count();
                        return true;
                    })) {
                return false;
            }
            const double gpuReadAndAppendSeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - queueReadStarted).count();
            queuePreprocessSeconds += gpuAppendSeconds;
            queueReadSeconds += std::max(0.0, gpuReadAndAppendSeconds - gpuAppendSeconds);
            if (eos || !deliveredGpuFrame) continue;
            haveFrame = true;
            const double frameTime = static_cast<double>(gpuTimestamp100ns) / 10000000.0;
            if (pendingQueueRecognitions.size() >= static_cast<std::size_t>(queueFrameBatch) &&
                !flushGpuQueueRecognitions()) return false;
            const int pct = analysisEnd > 0
                ? static_cast<int>(std::clamp(frameTime / analysisEnd * 35.0, 0.0, 35.0))
                : 0;
            status.progress.store(pct);
            const auto statusStarted = std::chrono::steady_clock::now();
            status.setMessage("Pass 1/3: GPU queue scan " + std::to_string(scans));
            queueStatusSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - statusStarted).count();
            if (analysisEnd > 0 && frameTime >= analysisEnd - .000001) break;
            continue;
        }
#endif
        if (hybridPixelReader) {
            Frame timingFrame;
            bool timingEos = false;
            if (!reader.readFiltered(timingFrame, timingEos, error,
                                     [](std::int64_t) { return false; })) return false;
            const std::int64_t targetTime = std::max<std::int64_t>(0, timingFrame.time100ns);
            while (!timingEos && !hybridPixelEos &&
                   (!haveHybridCurrentFrame || hybridCurrentFrame.time100ns < targetTime)) {
                if (!hybridPixelReader->read(hybridCurrentFrame, hybridPixelEos, error)) return false;
                if (!hybridPixelEos && !hybridCurrentFrame.bgra.empty()) {
                    haveHybridCurrentFrame = true;
                }
            }
            eos = timingEos || hybridPixelEos;
            if (!eos && haveHybridCurrentFrame) {
                frame = hybridCurrentFrame;
                frame.time100ns = timingFrame.time100ns;
            }
        } else if (queueSparseRoiEnabled) {
            if (!reader.readSparseRegions(frame, eos, error, queueSparseRegions)) return false;
        } else if (queueRoiEnabled) {
            if (!reader.readFilteredRegion(frame, eos, error, queueReadRegion,
                                           [](std::int64_t) { return true; })) return false;
        } else if (!reader.read(frame, eos, error)) {
            return false;
        }
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
        queueReadSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - queueReadStarted).count();
#endif
        if (eos || frame.bgra.empty()) continue;
        std::swap(frame, lastFrame);
        haveFrame = true;
        const double frameTime = static_cast<double>(lastFrame.time100ns) / 10000000.0;
        // Original code scans at every 0.01 s seek point. With sequential
        // decoding we reuse the current decoded frame for all such points
        // until the next decoded frame arrives; this preserves its
        // two-identical-scan confirmation behavior while avoiding seeks.
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
        ++deliveredQueueFrames;
        // Match the production stream profile's alternate-frame NEXT
        // recognition, counting from the decoded keyframe like NVDEC.
        const bool scanDue = nextScan <= frameTime + .000001 &&
            (duration <= 0 || nextScan < analysisEnd) &&
            ((deliveredQueueFrames - 1) % 2 == 0);
#else
        const bool scanDue = nextScan <= frameTime + .000001 &&
            (duration <= 0 || nextScan < analysisEnd);
#endif
        bool queueRecognitionBatched = false;
        if (scanDue) {
            // Several 0.01-second scan points commonly map to the same
            // decoded 30/60-fps frame. Vision is deterministic for a frame,
            // so recognize each player's queue once and copy that exact
            // observation to every corresponding timestamp.
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
            queueRecognitionBatched = queueModel && queueFrameBatch > 1;
            if (queueRecognitionBatched) enqueueQueueRecognition(lastFrame, frameTime);
            else recognizeQueues(lastFrame);
#else
            recognizeQueues(lastFrame);
#endif
        }
        if (scanDue && !queueRecognitionBatched) {
            // 2026-11-06 案1: 1フレームにつき1サンプルだけ記録する。
            double scanPoint = nextScan;
            while (scanPoint + settings.sampleIntervalSeconds <= frameTime + .000001) {
                scanPoint += settings.sampleIntervalSeconds;
            }
            appendScan(scanPoint,
                       lastP1Observation ? &*lastP1Observation : nullptr,
                       lastP2Observation ? &*lastP2Observation : nullptr);
            nextScan = scanPoint + settings.sampleIntervalSeconds;
        }
        if (boardPrefetch && !gpuBoardStreamEnabled) {
            std::shared_ptr<RetainedPrefetchFrame> owned;
            const auto findFreeSlot = [&]() -> std::shared_ptr<RetainedPrefetchFrame> {
                const auto available = std::find_if(
                    prefetchSlots.begin(), prefetchSlots.end(),
                    [](const auto& candidate) { return candidate.use_count() == 1; });
                return available == prefetchSlots.end() ? nullptr : *available;
            };
            owned = findFreeSlot();
            if (!owned && (maxPrefetchSlots == 0 || prefetchSlots.size() < maxPrefetchSlots)) {
                owned = std::make_shared<RetainedPrefetchFrame>();
                prefetchSlots.push_back(owned);
            }
            if (!owned) {
                // Free exactly the oldest delivery-order slot. If a worker is
                // still reading it, wait for the bounded worker queue rather
                // than allocating outside the declared pool.
                if (recentFrames.empty()) {
                    prefetchWorkers->wait();
                } else {
                    latestEvictedFrameTime = std::max(
                        latestEvictedFrameTime,
                        static_cast<double>(recentFrames.front()->time100ns) / 10000000.0);
                    auto evicted = std::move(recentFrames.front());
                    recentFrames.pop_front();
                    recentFrameBytes -= evicted->bytes();
                    ++boardPrefetch->evictedFrames;
                    evicted.reset();
                    if (!(owned = findFreeSlot())) prefetchWorkers->wait();
                }
                if (!owned) owned = findFreeSlot();
                if (!owned) {
                    error = "Bounded board ring could not reclaim a completed slot";
                    return false;
                }
            }
            owned->time100ns = lastFrame.time100ns;
            const auto retainStarted = std::chrono::steady_clock::now();
            if (settings.player1Enabled) p1Vision.retainBoardFrame(lastFrame, owned->p1);
            if (settings.player2Enabled) p2Vision.retainBoardFrame(lastFrame, owned->p2);
            prefetchRetainSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - retainStarted).count();
            if (prefetchSlotBytes == 0) {
                prefetchSlotBytes = owned->bytes();
                if (prefetchSlotBytes == 0) {
                    error = "Bounded board ring received an empty retained frame";
                    return false;
                }
                maxPrefetchSlots = std::max<std::size_t>(
                    1, maxPrefetchPayloadBytes / prefetchSlotBytes);
            } else if (owned->bytes() != prefetchSlotBytes) {
                error = "Bounded board ring frame geometry changed during decoding";
                return false;
            }
            recentFrameBytes += owned->bytes();
            recentFrames.push_back(std::move(owned));
            ++boardPrefetch->retainedFrames;
            boardPrefetch->ringPeakBytes = std::max(
                boardPrefetch->ringPeakBytes,
                prefetchSlots.size() * prefetchSlotBytes);
            // Retain only the currently open queue phases.  A long static
            // segment may exceed the cap; missing request times then take
            // the unchanged second-pass path instead of approximating data.
            while (recentFrames.size() > 1 && recentFrameBytes > maxPrefetchPayloadBytes) {
                latestEvictedFrameTime = std::max(
                    latestEvictedFrameTime,
                    static_cast<double>(recentFrames.front()->time100ns) / 10000000.0);
                auto evicted = std::move(recentFrames.front());
                recentFrames.pop_front();
                recentFrameBytes -= evicted->bytes();
                ++boardPrefetch->evictedFrames;
            }
            const bool p1PhaseChanged = settings.player1Enabled && confirmedChanged(p1Samples, confirmedP1);
            const bool p2PhaseChanged = settings.player2Enabled && confirmedChanged(p2Samples, confirmedP2);
            const bool phaseChanged = p1PhaseChanged || p2PhaseChanged;
            if (phaseChanged) {
                prefetchClosedPhases(std::min(frameTime, analysisEnd), false,
                                     p1PhaseChanged, p2PhaseChanged);
            }
        }
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
        if (pendingQueueRecognitions.size() >= static_cast<std::size_t>(queueFrameBatch)) {
            if (!flushQueueRecognitions()) return false;
        }
#endif
        const int pct = analysisEnd > 0 ? static_cast<int>(std::clamp(frameTime / analysisEnd * 35.0, 0.0, 35.0)) : 0;
        status.progress.store(pct);
        status.setMessage("Pass 1/3: queue scan " + std::to_string(scans));
        // A prefix analysis must stop decoding at the requested boundary.
        // Previously only the sampling loop stopped; Media Foundation still
        // decoded the rest of a long source video, making a 30-second run
        // look frozen and defeating the purpose of chunked analysis.
        if (analysisEnd > 0 && frameTime >= analysisEnd - .000001) break;
    }
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
#if defined(_WIN32) && defined(TR_GPU_ULTRA_DEFAULT)
    if (gpuQueuePreprocessEnabled) {
        if (!flushGpuQueueRecognitions()) return false;
        if (!finishGpuQueuePipeline()) return false;
    } else
#endif
    if (!flushQueueRecognitions()) return false;
        if (queueTimingEnabled) {
            std::cerr << "queue_read_s=" << queueReadSeconds
                      << " queue_preprocess_s=" << queuePreprocessSeconds
                      << " queue_inference_s=" << queueInferenceSeconds
                      << " queue_delivered_frames=" << deliveredQueueFrames
                      << " queue_stride=" << nvdecQueueFrameStride
                      << " queue_callback_frames=" << queueCallbackFrames
                      << " queue_scan_frames=" << queueScanFrames
                      << " queue_duplicate_frames=" << queueDuplicateFrames
                      << " queue_inferred_frames=" << inferredQueueFrames
                      << " queue_inference_flushes=" << queueInferenceFlushes
                      << " queue_adaptive_inferred_frames=" << adaptiveQueueInferredFrames
                      << " queue_adaptive_reused_frames=" << adaptiveQueueReusedFrames
                      << " queue_adaptive_inferred_slots=" << adaptiveQueueInferredSlots
                      << " queue_adaptive_reused_slots=" << adaptiveQueueReusedSlots
                      << " queue_adaptive_inference_runs=" << adaptiveQueueInferenceRuns
                      << " queue_background_inference_s=" << backgroundQueueInferenceSeconds
                      << " queue_status_s=" << queueStatusSeconds
                      << " queue_frame_batch=" << queueFrameBatch << '\n';
        }
#endif
    if (!haveFrame) { error = "No decodable video frame was found"; return false; }
    if (duration > 0 && nextScan < analysisEnd) {
        if ((settings.player1Enabled && !lastP1Observation) ||
            (settings.player2Enabled && !lastP2Observation)) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && defined(TR_GPU_ULTRA_DEFAULT)
            if (gpuQueuePreprocessEnabled) {
                error = "GPU queue preprocessing produced no observation before the analysis tail";
                return false;
            }
#endif
            recognizeQueues(lastFrame);
        }
        // 末尾の埋め合わせは従来どおり（フレームが無い区間を補間する）
        while (nextScan < analysisEnd) {
            appendScan(nextScan,
                       lastP1Observation ? &*lastP1Observation : nullptr,
                       lastP2Observation ? &*lastP2Observation : nullptr);
            nextScan += settings.sampleIntervalSeconds;
        }
    }
    status.setMessage("Pass 1/3: global 7-bag NEXT reconstruction");
    const auto finalQueueDecodeStarted = std::chrono::steady_clock::now();
    p1 = settings.player1Enabled
        ? buildQueuePhasesFromSamples(p1Samples, settings, analysisEnd, startSeconds)
                                 : std::vector<QueuePhase>{};
    p2 = settings.player2Enabled
        ? buildQueuePhasesFromSamples(p2Samples, settings, analysisEnd, startSeconds)
                                 : std::vector<QueuePhase>{};
    finalQueueDecodeSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - finalQueueDecodeStarted).count();
    if (boardPrefetch) {
        schedulePrefetch(p1, true, true);
        schedulePrefetch(p2, false, true);
        flushNvdecBoardBatch();
        const auto waitStarted = std::chrono::steady_clock::now();
        if (prefetchWorkers) prefetchWorkers->wait();
        prefetchWaitSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - waitStarted).count();
        if (status.cancel.load()) { error = "Recovery cancelled"; return false; }
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT) && defined(TR_NVDEC_DEFAULT)
        if (onePassBoardEnabled) {
            if (onePassBoardPool) onePassBoardPool->wait();
            std::vector<MissingBoardRequest> pending;
            {
                std::lock_guard<std::mutex> lock(missingBoardMutex);
                pending = missingBoardRequests;
            }
            std::size_t fulfilled = 0;
            std::string retryError;
            const auto retryStarted = std::chrono::steady_clock::now();
            if (!collectOnePassBoardRepair(
                    reader, *nvdecCudaPreprocessor, pending,
                    settings.player1Enabled ? &p1Vision : nullptr,
                    settings.player2Enabled ? &p2Vision : nullptr,
                    onePassBoardPool.get(), *boardPrefetch, fulfilled,
                    retryError)) {
                error = retryError.empty()
                    ? "One-pass board repair failed" : std::move(retryError);
                return false;
            }
            const double retrySeconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - retryStarted).count();
            if (std::getenv("TR_GPU_DIAGNOSTICS") || queueTimingEnabled) {
                std::cerr << "onepass_board_requests=" << onePassBoardRequests
                          << " exported=" << onePassBoardExported
                          << " ring_misses=" << pending.size()
                          << " repaired=" << fulfilled
                          << " export_s=" << onePassExportSeconds
                          << " repair_s=" << retrySeconds << '\n';
            }
        }
#endif
        if (prefetchFailed.load()) {
            std::lock_guard<std::mutex> lock(prefetchErrorMutex);
            error = prefetchError.empty() ? "Board prefetch failed" : prefetchError;
            return false;
        }
    }
#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    if (boardPrefetch && queueTimingEnabled) {
        std::cerr << "prefetch_phase_rebuild_s=" << prefetchPhaseRebuildSeconds
                  << " prefetch_schedule_s=" << prefetchScheduleSeconds
                  << " prefetch_retain_s=" << prefetchRetainSeconds
                  << " prefetch_worker_cpu_s="
                  << static_cast<double>(prefetchWorkerNanoseconds.load(std::memory_order_relaxed)) /
                         1000000000.0
                  << " final_queue_decode_s=" << finalQueueDecodeSeconds
                  << " prefetch_wait_s=" << prefetchWaitSeconds;
#if defined(_WIN32) && defined(TR_GPU_ULTRA_DEFAULT)
        std::ostringstream boardRequestMapHashText;
        boardRequestMapHashText << std::hex << std::setfill('0')
                                << std::setw(16) << boardRequestMapHash;
        std::cerr << " gpu_board_retain_s=" << gpuBoardRetainSeconds
                  << " gpu_board_inference_s=" << gpuBoardInferenceSeconds
                  << " board_commit_s=" << boardCommitSeconds
                  << " board_request_map_hash="
                  << boardRequestMapHashText.str()
                  << " board_request_map_count=" << boardRequestMapCount;
#if defined(TR_NVDEC_DEFAULT)
        if (nvdecBoardBatchEnabled) {
            std::cerr << " board_batch_size="
                      << (nvdecCudaPreprocessor
                              ? nvdecCudaPreprocessor->boardBatchCapacity() : 0)
                      << " board_batch_mode="
                      << (nvdecBoardBatchScalarCnn
                              ? "preprocess_batch_scalar_cnn"
                              : "full_cnn_batch")
                      << " board_batch_flushes=" << nvdecBoardBatchFlushes
                      << " board_batch_requests=" << nvdecBoardBatchRequests
                      << " board_batch_fallbacks=" << nvdecBoardBatchFallbacks
                      << " board_batch_max=" << nvdecBoardBatchMaximum;
        }
#endif
#endif
        std::cerr << '\n';
    }
#endif
    return true;
}

bool legacyOpeningBoardEnabled() {
#ifdef __EMSCRIPTEN__
    return false;
#else
    if (const char* configured = std::getenv("TR_LEGACY_OPENING_BOARD")) {
        return std::string(configured) != "0";
    }
    return true;
#endif
}

bool collectLegacyOpeningBoardSamples(
    const std::filesystem::path& input,
    const std::vector<BoardRequest>& allP1Requests,
    const std::vector<BoardRequest>& allP2Requests,
    VisionAnalyzer* p1Vision, VisionAnalyzer* p2Vision,
    Status& status,
    std::vector<BoardObservation>& p1Opening,
    std::vector<BoardObservation>& p2Opening,
    std::vector<std::int64_t>& p1SourceTimes,
    std::vector<std::int64_t>& p2SourceTimes,
    std::string& error) {
    std::vector<BoardRequest> p1Requests;
    std::vector<BoardRequest> p2Requests;
    std::copy_if(allP1Requests.begin(), allP1Requests.end(),
                 std::back_inserter(p1Requests),
                 [](const BoardRequest& request) { return request.phase == 0; });
    std::copy_if(allP2Requests.begin(), allP2Requests.end(),
                 std::back_inserter(p2Requests),
                 [](const BoardRequest& request) { return request.phase == 0; });
    if (p1Requests.empty() && p2Requests.empty()) return true;

    // The MF advanced/D3D11 converter changes very dark RGB values enough to
    // cross several board-CNN decision boundaries on 2.mov. Later phases are
    // constrained by legal placements, but phase 0 is an unconditional beam
    // anchor. Re-read only that tiny opening range through the established
    // software RGB32 converter; do not force the initial board to be empty,
    // because a recording may legitimately start mid-game.
    VideoReader reader;
    if (!reader.open(input, error, false, true)) return false;
    status.setMessage("Pass 2/3: legacy RGB32 verification of the opening board");

    std::size_t p1Index = 0;
    std::size_t p2Index = 0;
    Frame frame;
    Frame lastUsableFrame;
    bool haveFrame = false;
    bool eos = false;
    const auto analyze = [&](VisionAnalyzer* vision, const BoardRequest& request,
                             std::vector<BoardObservation>& destination,
                             std::vector<std::int64_t>& sourceTimes,
                             const Frame& source) -> bool {
        if (!vision) return true;
        BoardObservation observation = vision->analyzeBoard(source);
        if (!observation.recognitionError.empty()) {
            error = observation.recognitionError;
            return false;
        }
        observation.timeSeconds = request.time;
        destination.push_back(std::move(observation));
        sourceTimes.push_back(source.time100ns);
        return true;
    };
    const auto fulfill = [&](const Frame& source) -> bool {
        const double frameTime = static_cast<double>(source.time100ns) / 10000000.0;
        while (p1Index < p1Requests.size() &&
               p1Requests[p1Index].time <= frameTime + .000001) {
            if (!analyze(p1Vision, p1Requests[p1Index], p1Opening,
                         p1SourceTimes, source)) return false;
            ++p1Index;
        }
        while (p2Index < p2Requests.size() &&
               p2Requests[p2Index].time <= frameTime + .000001) {
            if (!analyze(p2Vision, p2Requests[p2Index], p2Opening,
                         p2SourceTimes, source)) return false;
            ++p2Index;
        }
        return true;
    };

    while (!eos && (p1Index < p1Requests.size() || p2Index < p2Requests.size())) {
        if (status.cancel.load()) { error = "Recovery cancelled"; return false; }
        if (!reader.read(frame, eos, error)) return false;
        if (eos || frame.bgra.empty()) continue;
        haveFrame = true;
        lastUsableFrame = frame;
        if (!fulfill(frame)) return false;
    }
    if (!haveFrame) {
        error = "No legacy RGB32 frame was available for the opening board";
        return false;
    }
    while (p1Index < p1Requests.size()) {
        if (!analyze(p1Vision, p1Requests[p1Index], p1Opening,
                     p1SourceTimes, lastUsableFrame)) return false;
        ++p1Index;
    }
    while (p2Index < p2Requests.size()) {
        if (!analyze(p2Vision, p2Requests[p2Index], p2Opening,
                     p2SourceTimes, lastUsableFrame)) return false;
        ++p2Index;
    }
    return true;
}

bool replacePrefetchedOpeningBoardSamplesWithLegacyRgb(
    const std::filesystem::path& input,
    const std::filesystem::path& modelPath,
    const Settings& settings,
    const std::vector<QueuePhase>& p1Phases,
    const std::vector<QueuePhase>& p2Phases,
    Status& status,
    std::vector<std::vector<BoardObservation>>& p1Samples,
    std::vector<std::vector<BoardObservation>>& p2Samples,
    std::string& error,
    ReusableAnalysisModels* reusableModels = nullptr) {
    if (!legacyOpeningBoardEnabled()) return true;

    // A full-stream GPU prefetch normally bypasses collectBoardSamples(),
    // which is where phase 0 is protected from Media Foundation's advanced
    // RGB conversion.  Recreate only the analyzers needed for that tiny
    // legacy re-read, then replace phase 0 without forcing it to be empty.
    VideoReader geometry;
    if (!geometry.open(input, error, false, true)) return false;

    std::unique_ptr<OnnxBoardModel> ownedP1Model;
    std::unique_ptr<OnnxBoardModel> ownedP2Model;
    OnnxBoardModel* p1Model = nullptr;
    OnnxBoardModel* p2Model = nullptr;
    if (settings.player1Enabled) {
        if (reusableModels) {
            p1Model = reusableModels->board(true, modelPath, error);
        } else {
            ownedP1Model = std::make_unique<OnnxBoardModel>(modelPath, error);
            p1Model = ownedP1Model.get();
        }
        if (!p1Model || !p1Model->ready()) return false;
    }
    if (settings.player2Enabled) {
        if (reusableModels) {
            p2Model = reusableModels->board(false, modelPath, error);
        } else {
            ownedP2Model = std::make_unique<OnnxBoardModel>(modelPath, error);
            p2Model = ownedP2Model.get();
        }
        if (!p2Model || !p2Model->ready()) return false;
    }
    VisionAnalyzer p1Vision(geometry.width(), geometry.height(), settings.p1, p1Model);
    VisionAnalyzer p2Vision(geometry.width(), geometry.height(), settings.p2, p2Model);

    const auto p1Requests = makeBoardRequests(p1Phases, settings.onnxSamples);
    const auto p2Requests = makeBoardRequests(p2Phases, settings.onnxSamples);
    std::vector<BoardObservation> p1Opening;
    std::vector<BoardObservation> p2Opening;
    std::vector<std::int64_t> p1SourceTimes;
    std::vector<std::int64_t> p2SourceTimes;
    if (!collectLegacyOpeningBoardSamples(
            input, p1Requests, p2Requests,
            settings.player1Enabled ? &p1Vision : nullptr,
            settings.player2Enabled ? &p2Vision : nullptr,
            status, p1Opening, p2Opening,
            p1SourceTimes, p2SourceTimes, error)) return false;

    if (!p1Opening.empty() && !p1Samples.empty()) p1Samples[0] = std::move(p1Opening);
    if (!p2Opening.empty() && !p2Samples.empty()) p2Samples[0] = std::move(p2Opening);
    return true;
}

// Bounded safety net for the one-pass board capture. A request whose delivered
// frame had already left the ring is resolved by seeking the scan's own decoder
// into a two second window around it and converting the board rectangle of the
// frame that request actually selected. Reusing the running VideoReader keeps a
// single D3D11 device per worker, and the same NV12-to-BGRA arithmetic keeps the
// observation bytes identical to the full-range fallback.
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && \
    defined(TR_GPU_ULTRA_DEFAULT) && defined(TR_NVDEC_DEFAULT)
bool collectOnePassBoardRepair(
    VideoReader& reader, NvdecCudaPreprocessor& preprocessor,
    const std::vector<MissingBoardRequest>& missing,
    VisionAnalyzer* p1Vision, VisionAnalyzer* p2Vision,
    BoardWorkerPool* pool, BoardRequestPrefetch& destination,
    std::size_t& fulfilled, std::string& error) {
    fulfilled = 0;
    if (missing.empty()) return true;
    std::vector<BoardRequest> pendingP1;
    std::vector<BoardRequest> pendingP2;
    for (const MissingBoardRequest& item : missing) {
        if (item.player1) {
            if (destination.p1.find(boardRequestKey(item.request.time)) == destination.p1.end())
                pendingP1.push_back(item.request);
        } else {
            if (destination.p2.find(boardRequestKey(item.request.time)) == destination.p2.end())
                pendingP2.push_back(item.request);
        }
    }
    if (pendingP1.empty() && pendingP2.empty()) return true;
    // The serve loop below hands every pending request at/before the current
    // frame to that frame, so each list must be in time order. Missing
    // requests are recorded in arrival order; unsorted, an early request
    // queued behind a later one was served by the later cluster's frame
    // (2026-11-13: FMhM 360p opening boards read from +36..+93 s, the
    // type-A opening warnings). TR_ONEPASS_REPAIR_UNSORTED=1 restores it.
    if (!std::getenv("TR_ONEPASS_REPAIR_UNSORTED")) {
        const auto byTime = [](const BoardRequest& a, const BoardRequest& b) {
            return a.time < b.time;
        };
        std::stable_sort(pendingP1.begin(), pendingP1.end(), byTime);
        std::stable_sort(pendingP2.begin(), pendingP2.end(), byTime);
    }
    if (!p1Vision && !p2Vision) {
        error = "One-pass board repair has no board analyzer";
        return false;
    }

    std::vector<double> times;
    times.reserve(pendingP1.size() + pendingP2.size());
    for (const BoardRequest& request : pendingP1) times.push_back(request.time);
    for (const BoardRequest& request : pendingP2) times.push_back(request.time);
    std::sort(times.begin(), times.end());
    constexpr double ClusterGapSeconds = 2.0;
    std::vector<std::pair<double, double>> clusters;
    for (const double time : times) {
        if (clusters.empty() || time > clusters.back().second + ClusterGapSeconds) {
            clusters.emplace_back(time, time);
        } else {
            clusters.back().second = std::max(clusters.back().second, time);
        }
    }

    std::size_t p1Index = 0;
    std::size_t p2Index = 0;
    // Diagnostics only: a monotonic delivery ordinal for the frames this
    // bounded repair decodes, so an A/B run can tell a one-frame difference
    // from a classification difference.
    std::uint64_t repairFrameOrdinal = 0;
    const auto commit = [&](VisionAnalyzer* vision, const BoardRequest& request,
                            bool player1, const Frame& source) {
        auto slot = std::make_shared<PrefetchedBoardSlot>();
        slot->sourceTime100ns = source.time100ns;
        if (boardFrameDiagnosticsEnabled()) {
            slot->frameOrdinal = repairFrameOrdinal++;
            slot->frameHash = boardPixelFingerprint(source.bgra);
        }
        if (pool) {
            pool->submit([analyzer = vision, frame = source, request, slot]() mutable {
                BoardObservation observation = analyzer->analyzeBoard(frame);
                observation.timeSeconds = request.time;
                slot->observation = std::move(observation);
            });
        } else {
            BoardObservation observation = vision->analyzeBoard(source);
            observation.timeSeconds = request.time;
            slot->observation = std::move(observation);
        }
        auto& map = player1 ? destination.p1 : destination.p2;
        map[boardRequestKey(request.time)] = std::move(slot);
        ++fulfilled;
    };

    for (const auto& cluster : clusters) {
        const double seekTime = std::max(0.0, cluster.first - 1.0);
        if (!reader.seek(seekTime, error)) return false;
        bool eos = false;
        std::int64_t lastTimestamp = 0;
        while (!eos) {
            if (!reader.readCudaNv12Sample(
                    lastTimestamp, eos, error,
                    [&](const void* nv12, std::size_t pitch, int surfaceHeight,
                        int matrix, bool fullRange, std::int64_t frameTimestamp,
                        std::string& callbackError) {
                        const double frameTime =
                            static_cast<double>(frameTimestamp) / 10000000.0;
                        const auto serve = [&](VisionAnalyzer* vision,
                                               std::vector<BoardRequest>& pending,
                                               std::size_t& index,
                                               bool player1) -> bool {
                            if (!vision) return true;
                            while (index < pending.size() &&
                                   pending[index].time <= frameTime + .000001) {
                                const PixelRegion region = vision->boardPixelRegion();
                                Frame frame;
                                frame.width = region.width;
                                frame.height = region.height;
                                frame.sourceX = region.x;
                                frame.sourceY = region.y;
                                frame.time100ns = frameTimestamp;
                                if (!preprocessor.convertNv12ToBgra(
                                        nv12, pitch, surfaceHeight, matrix,
                                        fullRange, region.x, region.y,
                                        region.width, region.height, frame.bgra,
                                        callbackError)) {
                                    return false;
                                }
                                commit(vision, pending[index], player1, frame);
                                ++index;
                            }
                            return true;
                        };
                        if (!serve(p1Vision, pendingP1, p1Index, true)) return false;
                        if (!serve(p2Vision, pendingP2, p2Index, false)) return false;
                        return true;
                    },
                    preprocessor.cudaStreamHandle())) {
                return false;
            }
            if (p1Index >= pendingP1.size() && p2Index >= pendingP2.size()) break;
            if (!eos && lastTimestamp > 0 &&
                static_cast<double>(lastTimestamp) / 10000000.0 >
                    cluster.second + 2.0) break;
        }
    }
    if (pool) pool->wait();
    if (p1Index < pendingP1.size() || p2Index < pendingP2.size()) {
        error = "One-pass board repair could not reach every request";
        return false;
    }
    return true;
}
#endif

bool collectBoardSamples(const std::filesystem::path& input, const std::filesystem::path& modelPath,
                         const Settings& settings, const std::vector<QueuePhase>& p1Phases,
                         const std::vector<QueuePhase>& p2Phases, Status& status,
                          std::vector<std::vector<BoardObservation>>& p1Samples,
                          std::vector<std::vector<BoardObservation>>& p2Samples, std::string& error,
                          double startSeconds = 0,
                          ReusableAnalysisModels* reusableModels = nullptr) {
    VideoReader reader;
#ifdef TR_MF_BOARD_ADVANCED_PROCESSING_DEFAULT
    constexpr bool advancedBoardVideoProcessing = true;
#else
    constexpr bool advancedBoardVideoProcessing = false;
#endif
    // Range analysis normally gets every board observation from the bounded
    // GPU prefetch ring.  If an early request has already been evicted, this
    // function is the exact second-pass fallback.  Media Foundation can
    // report EOS immediately after SetCurrentPosition on very large MP4
    // files (the 4-hour/3.4-GB stream that exposed this bug), even though the
    // same range was just decoded successfully by NVDEC.  Reuse NVDEC for
    // non-zero ranges so the fallback has the same reliable seek semantics
    // as the first pass.  Full-video analysis keeps its established Media
    // Foundation path.
    const bool preferNvdecForRangeFallback = startSeconds > .000001;
    if (!reader.open(input, error, advancedBoardVideoProcessing, false,
                     preferNvdecForRangeFallback)) return false;

    std::unique_ptr<OnnxBoardModel> ownedP1Model;
    std::unique_ptr<OnnxBoardModel> ownedP2Model;
    OnnxBoardModel* p1Model = nullptr;
    OnnxBoardModel* p2Model = nullptr;
    if (settings.player1Enabled) {
        if (reusableModels) {
            p1Model = reusableModels->board(true, modelPath, error);
        } else {
            ownedP1Model = std::make_unique<OnnxBoardModel>(modelPath, error);
            p1Model = ownedP1Model.get();
        }
        if (!p1Model || !p1Model->ready()) return false;
    }
    if (settings.player2Enabled) {
        if (reusableModels) {
            p2Model = reusableModels->board(false, modelPath, error);
        } else {
            ownedP2Model = std::make_unique<OnnxBoardModel>(modelPath, error);
            p2Model = ownedP2Model.get();
        }
        if (!p2Model || !p2Model->ready()) return false;
    }
    VisionAnalyzer p1Vision(reader.width(), reader.height(), settings.p1, p1Model);
    VisionAnalyzer p2Vision(reader.width(), reader.height(), settings.p2, p2Model);
    // Range fallback uses exactly the same source frames and interpolation
    // weights as the full-frame path. These regions include every corner
    // pixel sampled by VisionAnalyzer; only unused conversion/readback is
    // removed. Keep the full-video and non-NVDEC paths unchanged.
    const bool useBoardRoi = reusableModels && preferNvdecForRangeFallback &&
        reader.usesNvdecCuda();
    PixelRegion boardReadRegion{0, 0, reader.width(), reader.height()};
    std::vector<PixelRegion> boardRegions;
    if (useBoardRoi) {
        if (settings.player1Enabled) boardRegions.push_back(p1Vision.boardPixelRegion());
        if (settings.player2Enabled) boardRegions.push_back(p2Vision.boardPixelRegion());
        for (std::size_t i = 0; i < boardRegions.size(); ++i) {
            const PixelRegion& region = boardRegions[i];
            if (i == 0) { boardReadRegion = region; continue; }
            const int x0 = std::min(boardReadRegion.x, region.x);
            const int y0 = std::min(boardReadRegion.y, region.y);
            const int x1 = std::max(boardReadRegion.x + boardReadRegion.width, region.x + region.width);
            const int y1 = std::max(boardReadRegion.y + boardReadRegion.height, region.y + region.height);
            boardReadRegion = {x0, y0, x1 - x0, y1 - y0};
        }
        reader.setGpuConversionRegions(boardRegions);
        if (std::getenv("TR_GPU_DIAGNOSTICS")) {
            std::cerr << "board_second_pass_roi=" << boardReadRegion.x << ',' << boardReadRegion.y
                      << ',' << boardReadRegion.width << ',' << boardReadRegion.height << '\n';
        }
    }
    auto p1Requests = makeBoardRequests(p1Phases, settings.onnxSamples);
    auto p2Requests = makeBoardRequests(p2Phases, settings.onnxSamples);
    if (startSeconds > 0) {
        const auto afterStart = [startSeconds](const BoardRequest& request) {
            return request.time >= startSeconds - .000001;
        };
        p1Requests.erase(std::remove_if(p1Requests.begin(), p1Requests.end(),
                                        [&](const BoardRequest& request) { return !afterStart(request); }), p1Requests.end());
        p2Requests.erase(std::remove_if(p2Requests.begin(), p2Requests.end(),
                                        [&](const BoardRequest& request) { return !afterStart(request); }), p2Requests.end());
        if (!reader.seek(startSeconds, error)) return false;
    }
    p1Samples.assign(p1Phases.size(), {});
    p2Samples.assign(p2Phases.size(), {});
    std::vector<BoardObservation> p1Flat(p1Requests.size());
    std::vector<BoardObservation> p2Flat(p2Requests.size());
    std::vector<std::int64_t> p1SourceTimes(p1Requests.size());
    std::vector<std::int64_t> p2SourceTimes(p2Requests.size());
    std::vector<std::uint64_t> p1FrameOrdinals(p1Requests.size());
    std::vector<std::uint64_t> p2FrameOrdinals(p2Requests.size());
    std::vector<std::uint64_t> p1FrameHashes(p1Requests.size());
    std::vector<std::uint64_t> p2FrameHashes(p2Requests.size());
    std::size_t p1Index = 0;
    std::size_t p2Index = 0;
    const std::size_t totalRequests = p1Requests.size() + p2Requests.size();
    if (totalRequests == 0) {
        error = "Queue reconstruction produced no board sample requests";
        return false;
    }
    std::atomic<std::size_t> completeRequests{0};
    std::atomic<bool> failed{false};
    std::mutex errorMutex;
    std::string workerError;
    BoardWorkerPool workers(std::max(1u, boardWorkerCount()));
    Frame frame;
    Frame lastFrame;
    std::shared_ptr<const Frame> lastOwnedFrame;
    bool haveFrame = false;
    bool eos = false;

    const auto failWorker = [&](const std::string& message) {
        if (!failed.exchange(true)) {
            std::lock_guard<std::mutex> lock(errorMutex);
            workerError = message;
        }
    };
    const auto submit = [&](const std::shared_ptr<const Frame>& source, const BoardRequest& request,
                            bool player1, std::size_t requestIndex,
                            std::uint64_t frameOrdinal, std::uint64_t frameHash) {
        if (player1) {
            p1SourceTimes[requestIndex] = source->time100ns;
            p1FrameOrdinals[requestIndex] = frameOrdinal;
            p1FrameHashes[requestIndex] = frameHash;
        } else {
            p2SourceTimes[requestIndex] = source->time100ns;
            p2FrameOrdinals[requestIndex] = frameOrdinal;
            p2FrameHashes[requestIndex] = frameHash;
        }
        workers.submit([&, source, request, player1, requestIndex] {
            if (failed.load() || status.cancel.load()) return;
            try {
                VisionAnalyzer& vision = player1 ? p1Vision : p2Vision;
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
                auto observation = vision.analyzeBrowserGpuBoard(*source);
#else
                auto observation = vision.analyzeBoard(*source);
#endif
                if (!observation.recognitionError.empty()) {
                    failWorker(observation.recognitionError);
                    return;
                }
                observation.timeSeconds = request.time;
                if (player1) p1Flat[requestIndex] = std::move(observation);
                else p2Flat[requestIndex] = std::move(observation);
                completeRequests.fetch_add(1);
            } catch (const std::exception& exception) {
                failWorker(std::string("Board analysis worker failed: ") + exception.what());
            } catch (...) {
                failWorker("Board analysis worker failed");
            }
        });
    };

    const double lastRequestTime = totalRequests > 0
        ? std::max(p1Requests.empty() ? 0.0 : p1Requests.back().time,
                   p2Requests.empty() ? 0.0 : p2Requests.back().time)
        : 0.0;
    std::uint64_t boardPassDecodedFrames = 0;
    const bool boardPassTiming = std::getenv("TR_MATCH_TIMING") != nullptr;
    const auto boardPassStarted = std::chrono::steady_clock::now();

    while (!eos && !failed.load()) {
        if (totalRequests == 0) break;
        if (status.cancel.load()) { error = "Recovery cancelled"; return false; }
        const auto needsBoardPixels = [&](std::int64_t timestamp) {
            const double candidateTime = static_cast<double>(timestamp) / 10000000.0;
            return (p1Index < p1Requests.size() &&
                    p1Requests[p1Index].time <= candidateTime + .000001) ||
                   (p2Index < p2Requests.size() &&
                    p2Requests[p2Index].time <= candidateTime + .000001);
        };
        if (useBoardRoi) {
            if (!reader.readFilteredRegion(frame, eos, error, boardReadRegion, needsBoardPixels)) return false;
        } else if (!reader.readFiltered(frame, eos, error, needsBoardPixels)) return false;
        if (eos) continue;
        ++boardPassDecodedFrames;
        if (frame.width > 0 && frame.height > 0) haveFrame = true;
        const double decodedFrameTime = static_cast<double>(frame.time100ns) / 10000000.0;
        if (frame.bgra.empty()) {
            if (decodedFrameTime >= lastRequestTime - .000001) break;
            continue;
        }
        std::swap(frame, lastFrame);
        const double frameTime = static_cast<double>(lastFrame.time100ns) / 10000000.0;
        const bool hasRequest =
            (p1Index < p1Requests.size() && p1Requests[p1Index].time <= frameTime + .000001) ||
            (p2Index < p2Requests.size() && p2Requests[p2Index].time <= frameTime + .000001);
        if (hasRequest) {
            auto source = std::make_shared<Frame>(std::move(lastFrame));
            lastOwnedFrame = source;
            const std::uint64_t frameOrdinal = boardPassDecodedFrames;
            const std::uint64_t frameHash = boardFrameDiagnosticsEnabled()
                ? boardPixelFingerprint(source->bgra) : 0;
            while (p1Index < p1Requests.size() && p1Requests[p1Index].time <= frameTime + .000001) {
                submit(source, p1Requests[p1Index], true, p1Index, frameOrdinal, frameHash);
                ++p1Index;
            }
            while (p2Index < p2Requests.size() && p2Requests[p2Index].time <= frameTime + .000001) {
                submit(source, p2Requests[p2Index], false, p2Index, frameOrdinal, frameHash);
                ++p2Index;
            }
        }
        const int pct = totalRequests > 0
            ? 35 + static_cast<int>(completeRequests.load() * 45 / totalRequests)
            : 80;
        status.progress.store(std::min(pct, 80));
        status.setMessage("Pass 2/3: ONNX board inference " + std::to_string(completeRequests.load()) + "/" + std::to_string(totalRequests));
        // Board extraction only needs to reach the final requested sample.
        // Do not decode the unrequested tail of the source video.
        if (frameTime >= lastRequestTime - .000001) break;
    }
    if (!haveFrame) { error = "No decodable video frame was found"; return false; }

    std::shared_ptr<const Frame> fallbackSource;
    if (!lastFrame.bgra.empty()) fallbackSource = std::make_shared<Frame>(std::move(lastFrame));
    else fallbackSource = lastOwnedFrame;
    const std::uint64_t fallbackOrdinal = boardPassDecodedFrames;
    const std::uint64_t fallbackHash =
        (fallbackSource && boardFrameDiagnosticsEnabled())
            ? boardPixelFingerprint(fallbackSource->bgra) : 0;
    while (p1Index < p1Requests.size()) {
        if (failed.load() || status.cancel.load()) break;
        submit(fallbackSource, p1Requests[p1Index], true, p1Index,
               fallbackOrdinal, fallbackHash);
        ++p1Index;
    }
    while (p2Index < p2Requests.size()) {
        if (failed.load() || status.cancel.load()) break;
        submit(fallbackSource, p2Requests[p2Index], false, p2Index,
               fallbackOrdinal, fallbackHash);
        ++p2Index;
    }
    workers.wait();
    if (boardPassTiming) {
        std::cerr << "board_pass_detail decoded_frames=" << boardPassDecodedFrames
                  << " requests=" << totalRequests
                  << " roi=" << boardReadRegion.width << 'x' << boardReadRegion.height
                  << " elapsed_s="
                  << std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - boardPassStarted).count()
                  << " analyzed=" << completeRequests.load() << '\n';
    }
    if (status.cancel.load()) { error = "Recovery cancelled"; return false; }
    if (failed.load()) {
        std::lock_guard<std::mutex> lock(errorMutex);
        error = workerError.empty() ? "Board analysis worker failed" : workerError;
        return false;
    }

    for (std::size_t i = 0; i < p1Requests.size(); ++i) {
        p1Samples[p1Requests[i].phase].push_back(std::move(p1Flat[i]));
    }
    for (std::size_t i = 0; i < p2Requests.size(); ++i) {
        p2Samples[p2Requests[i].phase].push_back(std::move(p2Flat[i]));
    }
    if (startSeconds <= .000001 && legacyOpeningBoardEnabled()) {
        std::vector<BoardObservation> p1Opening;
        std::vector<BoardObservation> p2Opening;
        std::vector<std::int64_t> p1OpeningSourceTimes;
        std::vector<std::int64_t> p2OpeningSourceTimes;
        if (!collectLegacyOpeningBoardSamples(
                input, p1Requests, p2Requests,
                settings.player1Enabled ? &p1Vision : nullptr,
                settings.player2Enabled ? &p2Vision : nullptr,
                status, p1Opening, p2Opening,
                p1OpeningSourceTimes, p2OpeningSourceTimes, error)) return false;
        if (!p1Opening.empty() && !p1Samples.empty()) p1Samples[0] = std::move(p1Opening);
        if (!p2Opening.empty() && !p2Samples.empty()) p2Samples[0] = std::move(p2Opening);
        std::size_t opening = 0;
        for (std::size_t i = 0; i < p1Requests.size(); ++i) {
            if (p1Requests[i].phase == 0 && opening < p1OpeningSourceTimes.size()) {
                p1SourceTimes[i] = p1OpeningSourceTimes[opening++];
            }
        }
        opening = 0;
        for (std::size_t i = 0; i < p2Requests.size(); ++i) {
            if (p2Requests[i].phase == 0 && opening < p2OpeningSourceTimes.size()) {
                p2SourceTimes[i] = p2OpeningSourceTimes[opening++];
            }
        }
    }
#ifndef __EMSCRIPTEN__
    if (const char* diagnosticsPath = std::getenv("TR_BOARD_FRAME_DIAGNOSTICS")) {
        if (*diagnosticsPath != '\0') {
            bool writeHeader = false;
            std::ofstream diagnostics = openBoardDiagnostics(diagnosticsPath, writeHeader);
            if (diagnostics) {
                if (writeHeader) {
                    diagnostics << "player,phase,request_seconds,source_seconds,delay_seconds,"
                                   "pass,request_index,frame_ordinal,frame_hash\n";
                }
                const std::uint64_t pass =
                    boardFrameDiagnosticsPass.fetch_add(1) + 1;
                const auto writeRequests = [&](const char* player,
                                               const std::vector<BoardRequest>& requests,
                                               const std::vector<std::int64_t>& sourceTimes,
                                               const std::vector<std::uint64_t>& ordinals,
                                               const std::vector<std::uint64_t>& hashes) {
                    for (std::size_t i = 0; i < requests.size(); ++i) {
                        const double sourceSeconds = static_cast<double>(sourceTimes[i]) / 10000000.0;
                        diagnostics << player << ',' << requests[i].phase << ','
                                    << std::setprecision(12) << requests[i].time << ',' << sourceSeconds << ','
                                    << (sourceSeconds - requests[i].time) << ','
                                    << pass << ',' << i << ',' << ordinals[i] << ','
                                    << std::hex << hashes[i] << std::dec << '\n';
                    }
                };
                writeRequests("p1", p1Requests, p1SourceTimes, p1FrameOrdinals, p1FrameHashes);
                writeRequests("p2", p2Requests, p2SourceTimes, p2FrameOrdinals, p2FrameHashes);
            }
        }
    }
#endif
    return true;
}

bool collectPrefetchedBoardSamples(const Settings& settings,
                                   const std::vector<QueuePhase>& p1Phases,
                                   const std::vector<QueuePhase>& p2Phases,
                                   const BoardRequestPrefetch& prefetch,
                                   Status& status,
                                   std::vector<std::vector<BoardObservation>>& p1Samples,
                                   std::vector<std::vector<BoardObservation>>& p2Samples,
                                   double startSeconds) {
    auto p1Requests = makeBoardRequests(p1Phases, settings.onnxSamples);
    auto p2Requests = makeBoardRequests(p2Phases, settings.onnxSamples);
    const auto keepNew = [startSeconds](const BoardRequest& request) {
        return request.time >= startSeconds - .000001;
    };
    p1Requests.erase(std::remove_if(p1Requests.begin(), p1Requests.end(),
                                    [&](const BoardRequest& request) { return !keepNew(request); }),
                     p1Requests.end());
    p2Requests.erase(std::remove_if(p2Requests.begin(), p2Requests.end(),
                                    [&](const BoardRequest& request) { return !keepNew(request); }),
                     p2Requests.end());
    std::size_t p1Missing = 0;
    std::size_t p2Missing = 0;
    double firstP1Missing = -1;
    double firstP2Missing = -1;
    for (const BoardRequest& request : p1Requests) {
        if (prefetch.p1.find(boardRequestKey(request.time)) == prefetch.p1.end()) {
            if (p1Missing++ == 0) firstP1Missing = request.time;
        }
    }
    for (const BoardRequest& request : p2Requests) {
        if (prefetch.p2.find(boardRequestKey(request.time)) == prefetch.p2.end()) {
            if (p2Missing++ == 0) firstP2Missing = request.time;
        }
    }
#ifndef __EMSCRIPTEN__
    if (std::getenv("TR_GPU_DIAGNOSTICS")) {
        std::cerr << "board_ring_peak_bytes=" << prefetch.ringPeakBytes
                  << " retained_frames=" << prefetch.retainedFrames
                  << " evicted_frames=" << prefetch.evictedFrames
                  << " submitted_requests=" << prefetch.submittedRequests
                  << " p1_hits=" << (p1Requests.size() - p1Missing) << '/' << p1Requests.size()
                  << " p2_hits=" << (p2Requests.size() - p2Missing) << '/' << p2Requests.size()
                  << " first_p1_miss=" << firstP1Missing
                  << " first_p2_miss=" << firstP2Missing << '\n';
    }
#endif
    if (p1Missing != 0 || p2Missing != 0) return false;

    p1Samples.assign(p1Phases.size(), {});
    p2Samples.assign(p2Phases.size(), {});
    for (const BoardRequest& request : p1Requests) {
        p1Samples[static_cast<std::size_t>(request.phase)].push_back(
            prefetch.p1.at(boardRequestKey(request.time))->observation);
    }
    for (const BoardRequest& request : p2Requests) {
        p2Samples[static_cast<std::size_t>(request.phase)].push_back(
            prefetch.p2.at(boardRequestKey(request.time))->observation);
    }
#ifndef __EMSCRIPTEN__
    if (const char* diagnosticsPath = std::getenv("TR_BOARD_PREFETCH_DIAGNOSTICS")) {
        if (*diagnosticsPath != '\0') {
            bool writeHeader = false;
            std::ofstream diagnostics = openBoardDiagnostics(diagnosticsPath, writeHeader);
            if (diagnostics) {
                if (writeHeader) {
                    diagnostics << "player,phase,request_seconds,source_seconds,delay_seconds,"
                                   "pass,request_index,frame_ordinal,frame_hash\n";
                }
                const std::uint64_t pass =
                    boardFrameDiagnosticsPass.fetch_add(1) + 1;
                const auto writeRequests = [&](const char* player,
                                               const std::vector<BoardRequest>& requests,
                                               const auto& slots) {
                    for (std::size_t index = 0; index < requests.size(); ++index) {
                        const BoardRequest& request = requests[index];
                        const auto& slot = slots.at(boardRequestKey(request.time));
                        const double sourceSeconds = static_cast<double>(slot->sourceTime100ns) / 10000000.0;
                        diagnostics << player << ',' << request.phase << ','
                                    << std::setprecision(12) << request.time << ',' << sourceSeconds << ','
                                    << (sourceSeconds - request.time) << ','
                                    << pass << ',' << index << ',' << slot->frameOrdinal << ','
                                    << std::hex << slot->frameHash << std::dec << '\n';
                    }
                };
                writeRequests("p1", p1Requests, prefetch.p1);
                writeRequests("p2", p2Requests, prefetch.p2);
            }
        }
    }
#endif
    status.progress.store(80);
    status.setMessage("Pass 2/3: reused boards inferred during the queue pass");
    return true;
}

void assignBoardObservation(TimelineStep& step, const BoardObservation& averaged) {
    step.confidence = averaged.confidence;
    step.probabilities = averaged.probabilities;
    step.observed = cleanUpBoard(averaged.board);
    for (int cell = 0; cell < BoardWidth * VisibleRows; ++cell) {
        const Cell cleaned = step.observed[BoardWidth * VisibleRows + cell];
        if (cleaned == averaged.board[cell]) continue;
        const std::size_t base = static_cast<std::size_t>(cell) * CellClassCount;
        std::fill_n(step.probabilities.begin() + base, CellClassCount, 0.0f);
        step.probabilities[probabilityIndex(cell, cleaned)] = 1.0f;
        step.confidence[cell] = 255;
    }
    step.board = step.observed;
    step.fullBoard = step.observed;
}

void makeOpeningTimelineEmpty(TimelineStep& step);

std::vector<TimelineStep> buildRawTimeline(const std::vector<QueuePhase>& phases,
                                           const std::vector<std::vector<BoardObservation>>& samples) {
    std::vector<TimelineStep> result;
    result.reserve(phases.size());
    for (std::size_t i = 0; i < phases.size(); ++i) {
        const BoardObservation averaged = i < samples.size() ? VisionAnalyzer::aggregate(samples[i]) : BoardObservation{};
        TimelineStep step;
        step.startSeconds = phases[i].start;
        step.timeSeconds = phases[i].end;
        step.piece = phases[i].active;
        step.action = phases[i].action;
        step.hold = phases[i].hold;
        step.next = phases[i].next;
        step.holdUsed = phases[i].holdUsed;
        step.queueManuallyFixed = phases[i].queueManuallyFixed;
        assignBoardObservation(step, averaged);
        result.push_back(std::move(step));
    }
    // v58: the piece that spawned at GO is never in the NEXT window, so the
    // init phase has no active piece and the first transition could not
    // place anything; when that piece locked before the first queue slide
    // it vanished and every later identical piece was shifted by one
    // (match 27 P1). With TR_OPENING_PIECE_FLEXIBLE=1 the first transition
    // compares all seven minos and "no lock" against the board, exactly as
    // the bounded board-driven repair does.
    if (const char* opening = std::getenv("TR_OPENING_PIECE_FLEXIBLE")) {
        if (std::string(opening) == "1" && !result.empty() &&
            !isPiece(result.front().piece) && !result.front().manuallyFixed) {
            result.front().pieceIdentityFlexible = true;
        }
    }
    // v61: a player may hard-drop the GO piece and the next one before the
    // first in-match queue frame (match 5 P1: Z dropped within 0.05 s, T
    // already falling at the first sample). The first observed board then
    // holds two pieces while only one transition precedes it, so the board
    // repair later re-labels unrelated pieces to compensate. With
    // TR_OPENING_TWO_PIECES=1, when the init board is empty, the opening
    // transition is flexible and the next observed board holds exactly
    // eight cells (two pieces, no clear), a second flexible step without
    // board evidence is inserted so both locks can be explained.
    // 7-10 cells: two pieces (8) with one cell of read noise either way;
    // a single lock would show about four.
    if (const char* two = std::getenv("TR_OPENING_TWO_PIECES")) {
        if (std::string(two) == "1" && result.size() >= 2 &&
            result.front().pieceIdentityFlexible && !result[1].manuallyFixed) {
            const auto occupiedCells = [](const Board& board) {
                int count = 0;
                for (const Cell cell : board) if (cell != Cell::Empty) ++count;
                return count;
            };
            if (std::getenv("TR_OPENING_TWO_PIECES_DEBUG"))
                std::cerr << "opening_two_pieces_check init=" << occupiedCells(result.front().observed)
                          << " next=" << occupiedCells(result[1].observed)
                          << " steps=" << result.size() << " t1=" << result[1].timeSeconds
                          << " piece1=" << cellChar(result[1].piece) << '\n';
            // The init sample may already show the first dropped piece; a match
            // always starts empty, so the opening is emptied before inserting
            // (forceKnownEmptyOpening does the same later in other paths).
            if (occupiedCells(result[1].observed) >= 7 &&
                occupiedCells(result[1].observed) <= 10) {
                if (occupiedCells(result.front().observed) != 0) {
                    makeOpeningTimelineEmpty(result.front());
                }
                TimelineStep extra = result.front();
                extra.observed = Board{};
                extra.board = Board{};
                extra.fullBoard = Board{};
                extra.confidence.fill(0);
                extra.probabilities.fill(1.0f / static_cast<float>(CellClassCount));
                extra.pieceIdentityFlexible = true;
                result.insert(result.begin() + 1, std::move(extra));
                std::cerr << "opening_two_pieces=1 inserted_flexible_step=1\n";
            }
        }
    }
    return result;
}

QueueObservation intermediateQueueAfterOneSlide(const QueueObservation& before,
                                                const QueueObservation& after) {
    QueueObservation result = before;
    result.next.clear();
    for (std::size_t i = 1; i < before.next.size(); ++i) result.next.push_back(before.next[i]);
    if (!after.next.empty()) result.next.push_back(after.next.front());
    if (result.next.size() > after.next.size()) result.next.resize(after.next.size());
    for (std::size_t i = 0; i + 1 < result.nextUnclear.size(); ++i) {
        result.nextUnclear[i] = before.nextUnclear[i + 1];
        result.nextPieceConfidence[i] = before.nextPieceConfidence[i + 1];
        result.nextUnclearProbability[i] = before.nextUnclearProbability[i + 1];
    }
    result.nextUnclear.back() = after.nextUnclear.front();
    result.nextPieceConfidence.back() = after.nextPieceConfidence.front();
    result.nextUnclearProbability.back() = after.nextUnclearProbability.front();
    return result;
}

std::vector<TimelineStep> buildRawTimelineFromFlatBoards(const std::vector<QueuePhase>& phases,
                                                         const std::vector<BoardObservation>& flatSamples) {
    std::vector<std::vector<BoardObservation>> grouped(phases.size());
    for (const auto& sample : flatSamples) {
        for (std::size_t phase = 0; phase < phases.size(); ++phase) {
            const bool last = phase + 1 == phases.size();
            if (sample.timeSeconds >= phases[phase].start - .000001 &&
                (sample.timeSeconds < phases[phase].end - .000001 || last)) {
                grouped[phase].push_back(sample);
                break;
            }
        }
    }
    // Queue edits can move a phase boundary away from the original ONNX
    // request slots. Reuse the nearest retained board observation rather than
    // silently replacing that phase with an all-empty board; the legal-move
    // beam can then still evaluate the corrected queue immediately.
    if (!flatSamples.empty()) {
        for (std::size_t phase = 0; phase < phases.size(); ++phase) {
            if (!grouped[phase].empty()) continue;
            const double target = (phases[phase].start + phases[phase].end) * .5;
            const auto nearest = std::min_element(flatSamples.begin(), flatSamples.end(),
                [target](const BoardObservation& left, const BoardObservation& right) {
                    return std::abs(left.timeSeconds - target) < std::abs(right.timeSeconds - target);
                });
            grouped[phase].push_back(*nearest);
        }
    }
    return buildRawTimeline(phases, grouped);
}

void makeOpeningBoardEmpty(BoardObservation& observation) {
    observation.board.fill(Cell::Empty);
    observation.confidence.fill(255);
    observation.probabilities.fill(0.0f);
    for (int cell = 0; cell < BoardWidth * VisibleRows; ++cell) {
        observation.probabilities[probabilityIndex(cell, Cell::Empty)] = 1.0f;
    }
    observation.quality = 1.0;
}

void makeOpeningTimelineEmpty(TimelineStep& step) {
    step.hold = Cell::Empty;
    step.holdUsed = false;
    step.observed.fill(Cell::Empty);
    step.board.fill(Cell::Empty);
    step.fullBoard.fill(Cell::Empty);
    step.confidence.fill(255);
    step.probabilities.fill(0.0f);
    for (int cell = 0; cell < BoardWidth * VisibleRows; ++cell) {
        step.probabilities[probabilityIndex(cell, Cell::Empty)] = 1.0f;
    }
}

bool forceKnownEmptyOpening(
        std::vector<QueueRecognitionSample>& queue,
        std::vector<BoardObservation>& boards,
        const std::vector<QueuePhase>& phases,
        std::vector<TimelineStep>& raw) {
    // Preserve the old path byte-for-byte when the opening was already
    // recognised correctly.  This makes the stream-only correction unable to
    // perturb any previously healthy match.
    const bool openingAlreadyEmpty = !boards.empty() && !queue.empty() &&
        queue.front().observation.hold == Cell::Empty &&
        std::all_of(boards.front().board.begin(), boards.front().board.end(),
                    [](Cell cell) { return cell == Cell::Empty; });
    if (openingAlreadyEmpty) return false;

    if (!queue.empty()) {
        // Only the time-zero anchor is a known fact. Later samples in the same
        // queue phase may contain a legitimate first-piece HOLD and remain as
        // evidence for the normal queue decoder.
        queue.front().observation.hold = Cell::Empty;
        queue.front().decoded.hold = Cell::Empty;
    }
    if (!phases.empty()) {
        const double openingEnd = phases.front().end;
        for (auto& observation : boards) {
            if (observation.timeSeconds >= openingEnd - .000001) continue;
            makeOpeningBoardEmpty(observation);
        }
    } else if (!boards.empty()) {
        makeOpeningBoardEmpty(boards.front());
    }
    if (!raw.empty()) {
        makeOpeningTimelineEmpty(raw.front());
    }
    return true;
}

struct PageEntry {
    double time = 0;
    std::string board;
    std::string activePiece;
    std::string hold;
    std::string next;
    std::string operationPiece;
    int operationX = 0;
    int operationY = 0;
    int operationRotation = 0;
    bool operationHold = false;
    // Garbage that was applied after the previous page's lock to produce
    // this page.  The event replay can therefore store only its hole columns
    // instead of falling back to a complete board checkpoint.
    GarbageRise garbage;
};

std::string operationRotationName(int rotation) {
    switch (rotation & 3) {
    case 1: return "right";
    case 2: return "reverse";
    case 3: return "left";
    default: return "spawn";
    }
}

std::string boardString(const Board& board) {
    std::string result;
    result.reserve(board.size());
    for (Cell cell : board) result.push_back(cellChar(cell));
    return result;
}

std::string sanitizePieces(const std::vector<Cell>& pieces) { return pieceString(pieces); }
std::string sanitizePiece(Cell piece) { return isPiece(piece) ? std::string(1, cellChar(piece)) : std::string(); }

void writeQueueRecognitionJson(std::ostream& out, const std::vector<QueueRecognitionSample>& samples) {
    out << '[';
    for (std::size_t i = 0; i < samples.size(); ++i) {
        if (i) out << ',';
        const auto& sample = samples[i];
        out << "{\"time\":" << std::fixed << std::setprecision(3) << sample.timeSeconds
            << ",\"active\":\"" << sanitizePiece(sample.active)
            << "\",\"hold\":\"" << sanitizePiece(sample.observation.hold)
            << "\",\"next\":\"" << sanitizePieces(sample.observation.next)
            << "\",\"decodedHold\":\"" << sanitizePiece(sample.decoded.hold)
            << "\",\"decodedNext\":\"" << sanitizePieces(sample.decoded.next)
            << "\",\"stable\":" << (sample.stable ? "true" : "false")
            << ",\"manual\":" << (sample.manuallyEdited ? "true" : "false")
            << ",\"sequenceCorrected\":" << (sample.sequenceCorrected ? "true" : "false")
            << ",\"holdCorrected\":" << (sample.holdCorrected ? "true" : "false")
            << ",\"activeCorrected\":" << (sample.activeCorrected ? "true" : "false")
            << ",\"queueCnn\":" << (sample.observation.cnnUsed ? "true" : "false")
            << ",\"holdUnclear\":" << (sample.observation.holdUnclear ? "true" : "false")
            << ",\"nextUnclear\":[";
        for (std::size_t slot = 0; slot < sample.observation.nextUnclear.size(); ++slot) {
            if (slot) out << ',';
            out << (sample.observation.nextUnclear[slot] ? "true" : "false");
        }
        out << ']'
            << ",\"rejected\":" << (sample.rejected ? "true" : "false") << '}';
    }
    out << ']';
}

std::string simulatorQueueForStep(const TimelineStep& step) {
    // `next` is the visible queue after the active mino was spawned.  The
    // Simulator has no separate active-mino field, so put it back at the
    // queue head.  This makes every exported page chronological: the mino
    // about to be controlled is always the first visible item.
    std::string queue = sanitizePiece(step.piece) + sanitizePieces(step.next);
    return queue;
}

std::vector<PageEntry> pageEntries(const std::vector<TimelineStep>& timeline) {
    // A TimelineStep is a state while `piece` is being controlled. Its board
    // becomes the result of placing the previous step's active piece. The
    // original HTML flattened every state at `timeSeconds` and copied only
    // `next`; that made the active mino appear from nowhere in the viewer.
    // Serialize phase starts instead, and retain the previous queue during a
    // line-clear landing frame.
    std::vector<PageEntry> entries;
    double lastTime = -1;
    const auto append = [&](double requestedTime, const Board& board, const TimelineStep& queueStep,
                            const TimelineStep* operationStep) {
        double time = requestedTime;
        if (time <= lastTime) time = lastTime + .01;
        PageEntry entry;
        entry.time = time;
        entry.board = boardString(board);
        entry.activePiece = sanitizePiece(queueStep.piece);
        entry.hold = sanitizePiece(queueStep.hold);
        entry.next = simulatorQueueForStep(queueStep);
        entry.garbage = queueStep.garbage;
        if (operationStep && isPiece(operationStep->placedPiece)) {
            entry.operationPiece = sanitizePiece(operationStep->placedPiece);
            entry.operationX = operationStep->placementX;
            entry.operationY = operationStep->placementY;
            entry.operationRotation = operationStep->placementRotation;
            entry.operationHold = queueStep.holdUsed;
        }
        entries.push_back(std::move(entry));
        lastTime = time;
    };
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        const auto& step = timeline[i];
        // Timeline[i].board is the field before the current active piece is
        // locked. The solver stores that exact lock on timeline[i + 1], so
        // export the page as (field, operation) without a board-diff guess.
        const TimelineStep* operationStep = i + 1 < timeline.size() ? &timeline[i + 1] : nullptr;
        append(step.startSeconds, step.board, step, operationStep);
    }
    return entries;
}

std::vector<std::pair<char, int>> encodeRle(const std::string& data) {
    std::vector<std::pair<char, int>> result;
    if (data.empty()) return result;
    char previous = data[0];
    int count = 1;
    for (std::size_t i = 1; i < data.size(); ++i) {
        if (data[i] == previous) ++count;
        else { result.push_back({previous, count}); previous = data[i]; count = 1; }
    }
    result.push_back({previous, count});
    return result;
}

std::string difference(const std::string& previous, const std::string& current) {
    std::string result(current.size(), 'E');
    for (std::size_t i = 0; i < current.size(); ++i) {
        if (previous[i] != current[i]) result[i] = current[i];
    }
    return result;
}

std::string jsonEscape(const std::string& value) {
    std::string result;
    for (char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c; break;
        }
    }
    return result;
}

std::string jsonRle(const std::string& data) {
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const auto [value, count] : encodeRle(data)) {
        if (!first) out << ',';
        first = false;
        out << "[\"" << value << "\"," << count << ']';
    }
    out << ']';
    return out.str();
}

std::string base64(const std::string& text) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve((text.size() + 2) / 3 * 4);
    std::uint32_t value = 0;
    int bits = -6;
    for (unsigned char c : text) {
        value = (value << 8) | c;
        bits += 8;
        while (bits >= 0) { result.push_back(table[(value >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) result.push_back(table[((value << 8) >> (bits + 8)) & 0x3F]);
    while (result.size() % 4) result.push_back('=');
    return result;
}

void writeTrajectoryRepairJson(std::ostream& out,
                               const TrajectoryRepairReport& report) {
    out << "{\"attempted\":" << (report.attempted ? "true" : "false")
        << ",\"adopted\":" << (report.adopted ? "true" : "false")
        << ",\"garbageHypothesesSearched\":"
        << (report.garbageHypothesesSearched ? "true" : "false")
        << ",\"neuralRankerUsed\":"
        << (report.neuralRankerUsed ? "true" : "false")
        << ",\"player\":" << report.player
        << ",\"firstSuspiciousPhase\":" << report.firstSuspiciousPhase
        << ",\"firstSuspiciousSeconds\":" << std::fixed << std::setprecision(6)
        << report.firstSuspiciousSeconds
        << ",\"initialBeamWidth\":" << report.initialBeamWidth
        << ",\"finalBeamWidth\":" << report.finalBeamWidth
        << ",\"initialSeverePhases\":" << report.initialSeverePhases
        << ",\"finalSeverePhases\":" << report.finalSeverePhases
        << ",\"initialMinimumMatchRatio\":" << report.initialMinimumMatchRatio
        << ",\"finalMinimumMatchRatio\":" << report.finalMinimumMatchRatio
        << ",\"timeBudgetExceeded\":"
        << (report.timeBudgetExceeded ? "true" : "false")
        << ",\"searchSeconds\":" << report.searchSeconds
        << ",\"cause\":\"" << jsonEscape(report.cause) << "\"}";
}

void writePlayerNameJson(std::ostream& out,
                         const PlayerNameRecognition& recognition) {
    out << "{\"text\":\"" << jsonEscape(recognition.text)
        << "\",\"confidence\":" << std::fixed << std::setprecision(6)
        << recognition.confidence
        << ",\"attempted\":" << (recognition.attempted ? "true" : "false")
        << ",\"engine\":\"" << jsonEscape(recognition.engine)
        << "\",\"error\":\"" << jsonEscape(recognition.error)
        << "\",\"crop\":[" << recognition.crop.x << ','
        << recognition.crop.y << ',' << recognition.crop.w << ','
        << recognition.crop.h << "]}";
}

std::string htmlEscape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '&': output << "&amp;"; break;
        case '<': output << "&lt;"; break;
        case '>': output << "&gt;"; break;
        case '"': output << "&quot;"; break;
        case '\'': output << "&#39;"; break;
        default: output << static_cast<char>(byte); break;
        }
    }
    return output.str();
}

constexpr std::size_t EventCheckpointInterval = 64;

std::string replaySequence(const std::vector<PageEntry>& pages);
std::string visibleNextForPage(const PageEntry& page);

char base36Digit(int value) {
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10);
}

std::string base36Number(int value) {
    if (value < 36) return std::string(1, base36Digit(value));
    return std::string{base36Digit(value / 36), base36Digit(value % 36)};
}

std::string compactBoard(const std::string& value) {
    const std::string board = value.size() == static_cast<std::size_t>(BoardWidth * BoardHeight)
        ? value : std::string(BoardWidth * BoardHeight, '_');
    int firstRow = 0;
    while (firstRow < BoardHeight) {
        bool empty = true;
        for (int x = 0; x < BoardWidth; ++x) {
            const char cell = board[static_cast<std::size_t>(firstRow * BoardWidth + x)];
            if (cell != '_' && cell != 'E') { empty = false; break; }
        }
        if (!empty) break;
        ++firstRow;
    }
    if (firstRow == BoardHeight) return {};
    return base36Number(firstRow) + '.'
        + board.substr(static_cast<std::size_t>(firstRow * BoardWidth));
}

Cell pagePieceCell(const std::string& piece) {
    if (piece.empty()) return Cell::Empty;
    switch (piece.front()) {
    case 'I': return Cell::I;
    case 'L': return Cell::L;
    case 'O': return Cell::O;
    case 'Z': return Cell::Z;
    case 'T': return Cell::T;
    case 'J': return Cell::J;
    case 'S': return Cell::S;
    default: return Cell::Empty;
    }
}

std::string applyPageOperation(const PageEntry& page) {
    if (page.operationPiece.empty() ||
        page.board.size() != static_cast<std::size_t>(BoardWidth * BoardHeight)) return page.board;
    const Cell piece = pagePieceCell(page.operationPiece);
    if (!isPiece(piece)) return page.board;
    const auto shape = TetrisEngine::shape(piece, page.operationRotation);
    std::string full = page.board;
    for (const auto& cell : shape.cells) {
        const int x = page.operationX + cell[0];
        const int y = page.operationY + cell[1];
        if (x < 0 || x >= BoardWidth || y < 0 || y >= BoardHeight) return page.board;
        full[static_cast<std::size_t>(y * BoardWidth + x)] = page.operationPiece.front();
    }
    std::vector<std::string> rows;
    rows.reserve(BoardHeight);
    for (int y = 0; y < BoardHeight; ++y) {
        const std::string row = full.substr(static_cast<std::size_t>(y * BoardWidth), BoardWidth);
        if (std::any_of(row.begin(), row.end(), [](char cell) { return cell == '_' || cell == 'E'; })) {
            rows.push_back(row);
        }
    }
    std::string result(static_cast<std::size_t>(BoardHeight - static_cast<int>(rows.size())) * BoardWidth, '_');
    for (const auto& row : rows) result += row;
    return result;
}

std::string applyEncodedGarbage(const std::string& board, const std::string& holes,
                                const std::string& fills) {
    if (board.size() != static_cast<std::size_t>(BoardWidth * BoardHeight) || holes.empty() ||
        holes.size() > static_cast<std::size_t>(BoardHeight)) return board;
    std::string result = board.substr(holes.size() * BoardWidth);
    for (std::size_t row = 0; row < holes.size(); ++row) {
        const int hole = holes[row] >= '0' && holes[row] <= '9' ? holes[row] - '0' : -1;
        if (hole < 0 || hole >= BoardWidth) return board;
        const char fill = fills.size() == holes.size() ? fills[row] : (fills.empty() ? 'G' : fills.front());
        std::string added(BoardWidth, fill);
        added[static_cast<std::size_t>(hole)] = '_';
        result += added;
    }
    return result;
}

std::optional<std::pair<std::string, std::string>> exactEncodedGarbage(
    const std::string& before, const std::string& after, const GarbageRise& rise) {
    if (rise.lines <= 0 || rise.lines > BoardHeight ||
        rise.holeMasks.size() != static_cast<std::size_t>(rise.lines)) return std::nullopt;
    std::string holes;
    holes.reserve(rise.holeMasks.size());
    for (const std::uint16_t mask : rise.holeMasks) {
        int hole = -1;
        for (int x = 0; x < BoardWidth; ++x) {
            if ((mask & static_cast<std::uint16_t>(1u << x)) == 0) continue;
            if (hole >= 0) return std::nullopt;
            hole = x;
        }
        if (hole < 0) return std::nullopt;
        holes.push_back(base36Digit(hole));
    }
    if (applyEncodedGarbage(before, holes, "G") == after) return std::pair{holes, std::string("G")};
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> detectEncodedGarbage(
    const std::string& before, const std::string& after) {
    if (before.size() != after.size() || before.size() != static_cast<std::size_t>(BoardWidth * BoardHeight)) {
        return std::nullopt;
    }
    for (int lines = 1; lines <= VisibleRows; ++lines) {
        const std::size_t retained = static_cast<std::size_t>((BoardHeight - lines) * BoardWidth);
        if (after.compare(0, retained, before, static_cast<std::size_t>(lines * BoardWidth), retained) != 0) continue;
        std::string holes;
        std::string fills;
        bool valid = true;
        for (int y = BoardHeight - lines; y < BoardHeight; ++y) {
            int hole = -1;
            char fill = 0;
            for (int x = 0; x < BoardWidth; ++x) {
                const char cell = after[static_cast<std::size_t>(y * BoardWidth + x)];
                if (cell == '_' || cell == 'E') {
                    if (hole >= 0) { valid = false; break; }
                    hole = x;
                } else if (!fill) {
                    fill = cell;
                } else if (cell != fill) {
                    valid = false;
                    break;
                }
            }
            if (!valid || hole < 0 || !fill) { valid = false; break; }
            holes.push_back(base36Digit(hole));
            fills.push_back(fill);
        }
        if (!valid) continue;
        const bool uniform = std::all_of(fills.begin(), fills.end(),
                                         [&](char cell) { return cell == fills.front(); });
        return std::pair{holes, uniform ? std::string(1, fills.front()) : fills};
    }
    return std::nullopt;
}

bool samePageOperation(const PageEntry& left, const PageEntry& right) {
    return left.operationPiece == right.operationPiece &&
        (left.operationPiece.empty() ||
         (left.operationX == right.operationX && left.operationY == right.operationY &&
          (left.operationRotation & 3) == (right.operationRotation & 3) &&
          left.operationHold == right.operationHold));
}

void writeEventOperation(std::ostringstream& out, const PageEntry& page) {
    if (page.operationPiece.empty()) { out << '0'; return; }
    out << "[\"" << page.operationPiece << "\"," << (page.operationRotation & 3)
        << ',' << page.operationX << ',' << page.operationY;
    if (page.operationHold) out << ",1";
    out << ']';
}

void writeEventDelta(std::ostringstream& out, const PageEntry& current,
                     const PageEntry* previous, std::size_t advanceCount) {
    const bool first = previous == nullptr;
    std::string expected = first ? current.board : applyPageOperation(*previous);
    std::optional<std::pair<std::string, std::string>> garbage;
    if (!first && expected != current.board) {
        garbage = exactEncodedGarbage(expected, current.board, current.garbage);
        if (!garbage) garbage = detectEncodedGarbage(expected, current.board);
        if (garbage) expected = applyEncodedGarbage(expected, garbage->first, garbage->second);
    }
    const bool checkpoint = advanceCount > 1 && advanceCount % EventCheckpointInterval == 0;
    const bool exactBoard = (!first && expected != current.board) || checkpoint;

    out << '{';
    bool wrote = false;
    const auto key = [&](const char* name) {
        if (wrote) out << ',';
        wrote = true;
        out << '\"' << name << "\":";
    };
    if (exactBoard) {
        key("b"); out << '\"' << jsonEscape(compactBoard(current.board)) << '\"';
    } else if (garbage) {
        key("g"); out << "[\"" << garbage->first << "\",\"" << garbage->second << "\"]";
    }
    if (first || current.activePiece != previous->activePiece) {
        key("a"); out << '\"' << jsonEscape(current.activePiece) << '\"';
    }
    if (first || current.hold != previous->hold) {
        key("h"); out << '\"' << jsonEscape(current.hold) << '\"';
    }
    const std::string currentNext = visibleNextForPage(current);
    const std::string previousNext = first ? std::string() : visibleNextForPage(*previous);
    if (first || currentNext != previousNext) {
        key("n"); out << '\"' << jsonEscape(currentNext) << '\"';
    }
    if (first || !samePageOperation(current, *previous)) {
        key("o"); writeEventOperation(out, current);
    }
    out << '}';
}

void writeEventInitial(std::ostringstream& out, const std::vector<PageEntry>& pages) {
    const PageEntry blank{0, std::string(BoardWidth * BoardHeight, '_')};
    const auto& first = pages.empty() ? blank : pages.front();
    out << "[\"" << jsonEscape(compactBoard(first.board)) << "\",\""
        << jsonEscape(first.hold) << "\",\"" << jsonEscape(replaySequence(pages)) << "\"]";
}

long long eventMilliseconds(double seconds) {
    return std::max<long long>(0, static_cast<long long>(std::llround(seconds * 1000.0)));
}

std::string eventReplayJson(const std::vector<PageEntry>& p1,
                            const std::vector<PageEntry>& p2,
                            bool twoPlayers) {
    std::ostringstream out;
    out << "{\"v\":4,\"f\":\"te1\",\"m\":" << (twoPlayers ? 2 : 1)
        << ",\"c\":0,\"x\":" << EventCheckpointInterval
        << ",\"cases\":[{\"i\":\"video-replay\",\"n\":\"Video replay\",\"k\":\"r\",\"m\":"
        << (twoPlayers ? 2 : 1) << ",\"s\":{\"1\":";
    writeEventInitial(out, p1);
    out << ",\"2\":";
    if (twoPlayers) writeEventInitial(out, p2);
    else writeEventInitial(out, {});
    out << "},\"e\":[";

    if (!twoPlayers) {
        long long previousTime = 0;
        for (std::size_t index = 0; index < p1.size(); ++index) {
            if (index) out << ',';
            const long long time = eventMilliseconds(p1[index].time);
            out << '[' << (index ? std::max<long long>(0, time - previousTime) : 0) << ',';
            writeEventDelta(out, p1[index], index ? &p1[index - 1] : nullptr, index + 1);
            out << ",0]";
            previousTime = time;
        }
    } else {
        std::vector<double> times;
        times.reserve(p1.size() + p2.size());
        for (const auto& page : p1) times.push_back(page.time);
        for (const auto& page : p2) times.push_back(page.time);
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        std::size_t p1Index = 0;
        std::size_t p2Index = 0;
        std::size_t lastP1 = std::numeric_limits<std::size_t>::max();
        std::size_t lastP2 = std::numeric_limits<std::size_t>::max();
        std::size_t p1Advances = 0;
        std::size_t p2Advances = 0;
        long long previousTime = 0;
        bool firstEvent = true;
        for (const double eventTime : times) {
            while (p1Index + 1 < p1.size() && p1[p1Index + 1].time <= eventTime) ++p1Index;
            while (p2Index + 1 < p2.size() && p2[p2Index + 1].time <= eventTime) ++p2Index;
            if (p1Index >= p1.size() || p2Index >= p2.size()) continue;
            const bool p1Advanced = firstEvent || p1Index != lastP1;
            const bool p2Advanced = firstEvent || p2Index != lastP2;
            if (!p1Advanced && !p2Advanced) continue;
            if (!firstEvent) out << ',';
            const long long time = eventMilliseconds(eventTime);
            out << '[' << (firstEvent ? 0 : std::max<long long>(0, time - previousTime)) << ',';
            if (p1Advanced) {
                ++p1Advances;
                const PageEntry* previous = lastP1 == std::numeric_limits<std::size_t>::max() ? nullptr : &p1[lastP1];
                writeEventDelta(out, p1[p1Index], previous, p1Advances);
            } else out << '0';
            out << ',';
            if (p2Advanced) {
                ++p2Advances;
                const PageEntry* previous = lastP2 == std::numeric_limits<std::size_t>::max() ? nullptr : &p2[lastP2];
                writeEventDelta(out, p2[p2Index], previous, p2Advances);
            } else out << '0';
            out << ']';
            if (p1Advanced) lastP1 = p1Index;
            if (p2Advanced) lastP2 = p2Index;
            previousTime = time;
            firstEvent = false;
        }
    }
    out << "]}]}";
    return out.str();
}

std::string fileUrl(const std::filesystem::path& path, const std::string& fragment) {
    const std::string source = std::filesystem::absolute(path).generic_u8string();
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(source.size() + 16);
    for (const unsigned char value : source) {
        const bool safe = (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') || value == '-' || value == '_' || value == '.' ||
            value == '~' || value == '/' || value == ':';
        if (safe) encoded.push_back(static_cast<char>(value));
        else {
            encoded.push_back('%');
            encoded.push_back(hex[value >> 4]);
            encoded.push_back(hex[value & 15]);
        }
    }
    return (encoded.empty() || encoded.front() != '/' ? "file:///" : "file://") + encoded + '#' + fragment;
}

std::string singlePagesJson(const std::vector<PageEntry>& pages) {
    std::ostringstream out;
    out << '[';
    std::string previous;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (i) out << ',';
        const auto& page = pages[i];
        const std::string encoded = previous.empty() ? page.board : difference(previous, page.board);
        out << "{\"p1\":{\"b\":" << jsonRle(encoded)
            << ",\"h\":\"" << jsonEscape(page.hold)
            << "\",\"n\":\"" << jsonEscape(page.next) << "\"";
        if (!page.operationPiece.empty()) {
            out << ",\"o\":{\"type\":\"" << page.operationPiece
                << "\",\"rotation\":\"" << operationRotationName(page.operationRotation)
                << "\",\"x\":" << page.operationX
                << ",\"y\":" << page.operationY;
            out << ",\"coordinateSpace\":\"simulator\"";
            if (page.operationHold) out << ",\"hold\":true";
            out << '}';
        }
        out << "}}";
        previous = page.board;
    }
    out << ']';
    return out.str();
}

std::string combinedPagesJson(const std::vector<PageEntry>& p1, const std::vector<PageEntry>& p2) {
    std::vector<double> times;
    times.reserve(p1.size() + p2.size());
    for (const auto& page : p1) times.push_back(page.time);
    for (const auto& page : p2) times.push_back(page.time);
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    std::size_t p1Index = 0;
    std::size_t p2Index = 0;
    std::string previous1;
    std::string previous2;
    std::string lastState;
    std::ostringstream out;
    out << '[';
    bool first = true;
    for (const double time : times) {
        while (p1Index + 1 < p1.size() && p1[p1Index + 1].time <= time) ++p1Index;
        while (p2Index + 1 < p2.size() && p2[p2Index + 1].time <= time) ++p2Index;
        if (p1Index >= p1.size() || p2Index >= p2.size()) continue;
        const auto& a = p1[p1Index];
        const auto& b = p2[p2Index];
        const auto operationState = [](const PageEntry& page) {
            return page.operationPiece + ':' + std::to_string(page.operationX) + ':'
                + std::to_string(page.operationY) + ':' + std::to_string(page.operationRotation)
                + ':' + (page.operationHold ? "1" : "0");
        };
        const std::string state = a.board + '\x1F' + b.board + '\x1F' + a.hold + '\x1F' + a.next + '\x1F' + operationState(a)
            + '\x1F' + b.hold + '\x1F' + b.next + '\x1F' + operationState(b);
        if (state == lastState) continue;
        lastState = state;
        if (!first) out << ',';
        first = false;
        const std::string diff1 = previous1.empty() ? a.board : difference(previous1, a.board);
        const std::string diff2 = previous2.empty() ? b.board : difference(previous2, b.board);
        out << "{\"p1\":{\"b\":" << jsonRle(diff1)
            << ",\"h\":\"" << jsonEscape(a.hold) << "\",\"n\":\"" << jsonEscape(a.next) << "\"";
        if (!a.operationPiece.empty()) {
            out << ",\"o\":{\"type\":\"" << a.operationPiece
                << "\",\"rotation\":\"" << operationRotationName(a.operationRotation)
                << "\",\"x\":" << a.operationX << ",\"y\":" << a.operationY;
            out << ",\"coordinateSpace\":\"simulator\"";
            if (a.operationHold) out << ",\"hold\":true";
            out << '}';
        }
        out << "}"
            << ",\"p2\":{\"b\":" << jsonRle(diff2)
            << ",\"h\":\"" << jsonEscape(b.hold) << "\",\"n\":\"" << jsonEscape(b.next) << "\"";
        if (!b.operationPiece.empty()) {
            out << ",\"o\":{\"type\":\"" << b.operationPiece
                << "\",\"rotation\":\"" << operationRotationName(b.operationRotation)
                << "\",\"x\":" << b.operationX << ",\"y\":" << b.operationY;
            out << ",\"coordinateSpace\":\"simulator\"";
            if (b.operationHold) out << ",\"hold\":true";
            out << '}';
        }
        out << "}}";
        previous1 = a.board;
        previous2 = b.board;
    }
    out << ']';
    return out.str();
}

std::string boardMatrixJson(const std::string& board) {
    std::ostringstream out;
    out << '[';
    for (int y = 0; y < BoardHeight; ++y) {
        if (y) out << ',';
        out << '[';
        for (int x = 0; x < BoardWidth; ++x) {
            if (x) out << ',';
            const char cell = y * BoardWidth + x < static_cast<int>(board.size())
                ? board[static_cast<std::size_t>(y * BoardWidth + x)] : '_';
            if (cell == '_' || cell == 'E') out << "null";
            else out << '"' << cell << '"';
        }
        out << ']';
    }
    out << ']';
    return out.str();
}

std::string replaySequence(const std::vector<PageEntry>& pages) {
    std::string sequence;
    std::string previousVisibleQueue;
    for (const auto& page : pages) {
        if (page.next.empty()) continue;
        const std::string active = page.activePiece;
        const std::string visible = active.empty() ? page.next
            : (page.next.rfind(active, 0) == 0 ? page.next.substr(active.size()) : page.next);
        if (sequence.empty()) {
            sequence = active + visible;
            previousVisibleQueue = visible;
            continue;
        }
        std::size_t overlap = 0;
        const std::size_t maxOverlap = std::min(previousVisibleQueue.size(), visible.size());
        for (std::size_t candidate = maxOverlap; candidate > 0; --candidate) {
            if (previousVisibleQueue.compare(previousVisibleQueue.size() - candidate, candidate,
                                             visible, 0, candidate) == 0) {
                overlap = candidate;
                break;
            }
        }
        if (overlap < visible.size()) sequence += visible.substr(overlap);
        previousVisibleQueue = visible;
    }
    return sequence;
}

void writeOperationJson(std::ostringstream& out, const PageEntry& page) {
    if (page.operationPiece.empty()) return;
    out << ",\"o\":{\"type\":\"" << page.operationPiece
        << "\",\"rotation\":\"" << operationRotationName(page.operationRotation)
        << "\",\"x\":" << page.operationX
        << ",\"y\":" << page.operationY;
    out << ",\"coordinateSpace\":\"simulator\"";
    if (page.operationHold) out << ",\"holdUsed\":true";
    out << '}';
}

std::string visibleNextForPage(const PageEntry& page) {
    if (page.activePiece.empty()) return page.next;
    return page.next.rfind(page.activePiece, 0) == 0
        ? page.next.substr(page.activePiece.size())
        : page.next;
}

void writeReplayPageJson(std::ostringstream& out, const PageEntry& page, bool includeOperation = true) {
    const std::string visibleNext = visibleNextForPage(page);
    out << "{\"board\":" << boardMatrixJson(page.board)
        << ",\"active\":\"" << jsonEscape(page.activePiece) << "\""
        << ",\"hold\":\"" << jsonEscape(page.hold)
        << "\",\"next\":\"" << jsonEscape(visibleNext)
        << "\",\"n\":\"" << jsonEscape(page.next) << "\"";
    if (includeOperation) writeOperationJson(out, page);
    out << '}';
}

void writeReplayInitialJson(std::ostringstream& out, const std::vector<PageEntry>& pages) {
    const PageEntry blank;
    const auto& first = pages.empty() ? blank : pages.front();
    out << "{\"board\":" << boardMatrixJson(first.board)
        << ",\"hold\":\"" << jsonEscape(first.hold)
        << "\",\"sequence\":\"" << jsonEscape(replaySequence(pages)) << "\"}";
}

std::string collectionSimulatorJson(const std::vector<PageEntry>& p1,
                                    const std::vector<PageEntry>& p2,
                                    bool twoPlayers) {
    std::ostringstream json;
    json << "{\"v\":3,\"m\":\"" << (twoPlayers ? "2P" : "1P")
         << "\",\"currentCase\":0,\"cases\":[{\"id\":\"video-replay\","
         << "\"name\":\"Video replay\",\"kind\":\"replay\",\"gameMode\":\""
         << (twoPlayers ? "2P" : "1P") << "\",\"initial\":{\"p1\":";
    writeReplayInitialJson(json, p1);
    json << ",\"p2\":";
    writeReplayInitialJson(json, p2);
    json << "},\"pages\":[";

    if (!twoPlayers) {
        for (std::size_t i = 0; i < p1.size(); ++i) {
            if (i) json << ',';
            json << "{\"p1\":";
            writeReplayPageJson(json, p1[i]);
            json << '}';
        }
    } else {
        std::vector<double> times;
        for (const auto& page : p1) times.push_back(page.time);
        for (const auto& page : p2) times.push_back(page.time);
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        std::size_t p1Index = 0;
        std::size_t p2Index = 0;
        bool firstPage = true;
        for (const double time : times) {
            while (p1Index + 1 < p1.size() && p1[p1Index + 1].time <= time) ++p1Index;
            while (p2Index + 1 < p2.size() && p2[p2Index + 1].time <= time) ++p2Index;
            if (p1Index >= p1.size() || p2Index >= p2.size()) continue;
            if (!firstPage) json << ',';
            firstPage = false;
            const auto& a = p1[p1Index];
            const auto& b = p2[p2Index];
            json << "{\"p1\":";
            // A carried page is still the same simulator state. Its
            // highlighted lock must survive a page update belonging only to
            // the other player; otherwise 2P replay makes minos flicker.
            writeReplayPageJson(json, a, true);
            json << ",\"p2\":";
            writeReplayPageJson(json, b, true);
            json << '}';
        }
    }
    json << "]}]}";
    return json.str();
}

std::string simulatorUrl(const std::vector<PageEntry>& p1,
                         const std::vector<PageEntry>& p2,
                         bool twoPlayers) {
    return "https://selmtoe.github.io/Tetris_Simulator/F/index.html#"
        + base64(eventReplayJson(p1, p2, twoPlayers));
}

// The viewer and the simulator both read the position from ?page=N (1-based,
// clamped to the replay length). Without it a replay link always opens page 1,
// which is the empty board the match started from, so every generated link
// names its position explicitly.
std::string withPageNumber(const std::string& url, std::size_t pageNumber) {
    if (pageNumber == 0) return url;
    const auto hash = url.find('#');
    const std::string head = hash == std::string::npos ? url : url.substr(0, hash);
    const std::string tail = hash == std::string::npos ? std::string() : url.substr(hash);
    const char separator = head.find('?') == std::string::npos ? '?' : '&';
    return head + separator + "page=" + std::to_string(pageNumber) + tail;
}

// Page 1 is the empty opening board, so a link that does not name a position
// opens the last recorded one. Callers still reach any position through an
// explicit ?page=N. Keep the count identical to the page list that
// collectionSimulatorJson() writes: one page per distinct timestamp in 2P.
std::size_t replayPageCount(const std::vector<PageEntry>& p1,
                            const std::vector<PageEntry>& p2,
                            bool twoPlayers) {
    if (!twoPlayers) return p1.size();
    std::vector<double> times;
    times.reserve(p1.size() + p2.size());
    for (const auto& page : p1) times.push_back(page.time);
    for (const auto& page : p2) times.push_back(page.time);
    std::sort(times.begin(), times.end());
    return static_cast<std::size_t>(std::unique(times.begin(), times.end()) - times.begin());
}

std::string boardRowsJson(const Board& board) {
    std::ostringstream out;
    out << '[';
    for (int y = 0; y < BoardHeight; ++y) {
        if (y) out << ',';
        out << '"';
        for (int x = 0; x < BoardWidth; ++x) out << cellChar(board[index(x, y)]);
        out << '"';
    }
    out << ']';
    return out.str();
}

std::string garbageMasksJson(const GarbageRise& garbage) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < garbage.holeMasks.size(); ++i) {
        if (i) out << ',';
        out << garbage.holeMasks[i];
    }
    out << ']';
    return out.str();
}

void writeGarbageJson(std::ostringstream& out, const GarbageRise& garbage) {
    out << "{\"lines\":" << garbage.lines
        << ",\"holeMasks\":" << garbageMasksJson(garbage)
        << ",\"matchRatio\":" << std::fixed << std::setprecision(4) << garbage.matchRatio
        << ",\"manual\":" << (garbage.manuallySpecified ? "true" : "false")
        << ",\"holesCertain\":" << (garbage.holesCertain ? "true" : "false") << '}';
}

void writePlacementJson(std::ostringstream& out, const TimelineStep& step, bool holdUsed) {
    out << "{\"piece\":\"" << sanitizePiece(step.placedPiece)
        << "\",\"x\":" << step.placementX
        << ",\"y\":" << step.placementY
        << ",\"rotation\":" << step.placementRotation
        << ",\"clearedLines\":" << step.clearedLines
        << ",\"coordinateSpace\":\"simulator\""
        << ",\"holdUsed\":" << (holdUsed ? "true" : "false") << '}';
}

void writeTimelineJson(std::ostringstream& out, const std::vector<TimelineStep>& timeline) {
    out << '[';
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        if (i) out << ',';
        const auto& step = timeline[i];
        out << "{\"start\":" << std::fixed << std::setprecision(3) << step.startSeconds
            << ",\"time\":" << step.timeSeconds
            << ",\"active\":\"" << sanitizePiece(step.piece)
            << "\",\"action\":\"" << jsonEscape(step.action)
            << "\",\"hold\":\"" << sanitizePiece(step.hold)
            << "\",\"next\":\"" << sanitizePieces(step.next)
            << "\",\"simulatorNext\":\"" << simulatorQueueForStep(step)
            << "\",\"shiftLines\":" << step.garbage.lines
            << ",\"shiftMatchRatio\":" << step.garbage.matchRatio
            << ",\"garbage\":";
        writeGarbageJson(out, step.garbage);
        out << ",\"placement\":";
        // The placement stored on this row is the lock of the previous
        // timeline state.  Carry that state's HOLD flag with the placement so
        // the standalone recovery JSON remains replayable too.
        writePlacementJson(out, step, i > 0 && timeline[i - 1].holdUsed);
        out
            << ",\"score\":" << step.score
            << ",\"manual\":" << (step.manuallyFixed ? "true" : "false")
            << ",\"recoveryUnresolved\":"
            << (step.recoveryUnresolved ? "true" : "false")
            << ",\"orderAutomaticallyResolved\":"
            << (step.orderAutomaticallyResolved ? "true" : "false")
            << ",\"queueManual\":" << (step.queueManuallyFixed ? "true" : "false")
            << ",\"observed\":" << boardRowsJson(step.observed)
            << ",\"board\":" << boardRowsJson(step.board)
            << ",\"fullBoard\":" << boardRowsJson(step.fullBoard) << '}';
    }
    out << ']';
}

int trainingQueueFlags(const QueueRecognitionSample& sample) {
    return (sample.stable ? 1 : 0) |
        (sample.manuallyEdited ? 2 : 0) |
        (sample.sequenceCorrected ? 4 : 0) |
        (sample.holdCorrected ? 8 : 0) |
        (sample.activeCorrected ? 16 : 0) |
        (sample.observation.cnnUsed ? 32 : 0) |
        (sample.observation.holdUnclear ? 64 : 0) |
        (sample.rejected ? 128 : 0);
}

int trainingNextUnclearMask(const QueueRecognitionSample& sample) {
    int result = 0;
    for (std::size_t index = 0; index < sample.observation.nextUnclear.size(); ++index) {
        if (sample.observation.nextUnclear[index]) result |= 1 << index;
    }
    return result;
}

void writeTrainingQueueJson(std::ostream& out, const std::vector<QueueRecognitionSample>& samples) {
    out << '[';
    long long previousTime = 0;
    std::array<std::string, 5> previousStrings{};
    int previousFlags = 0;
    int previousUnclear = 0;
    bool first = true;
    for (const auto& sample : samples) {
        if (!first) out << ',';
        const long long time = eventMilliseconds(sample.timeSeconds);
        const std::array<std::string, 5> strings{{
            sanitizePiece(sample.active), sanitizePiece(sample.observation.hold),
            sanitizePieces(sample.observation.next), sanitizePiece(sample.decoded.hold),
            sanitizePieces(sample.decoded.next)
        }};
        const int flags = trainingQueueFlags(sample);
        const int unclear = trainingNextUnclearMask(sample);
        std::array<std::string, 7> fields;
        for (std::size_t index = 0; index < strings.size(); ++index) {
            fields[index] = !first && strings[index] == previousStrings[index]
                ? "null" : "\"" + jsonEscape(strings[index]) + "\"";
        }
        fields[5] = !first && flags == previousFlags ? "null" : std::to_string(flags);
        fields[6] = !first && unclear == previousUnclear ? "null" : std::to_string(unclear);
        std::size_t fieldCount = fields.size();
        while (fieldCount > 0 && fields[fieldCount - 1] == "null") --fieldCount;
        out << '[' << (first ? time : std::max<long long>(0, time - previousTime));
        for (std::size_t index = 0; index < fieldCount; ++index) out << ',' << fields[index];
        out << ']';
        previousTime = time;
        previousStrings = strings;
        previousFlags = flags;
        previousUnclear = unclear;
        first = false;
    }
    out << ']';
}

void writeTrainingGarbage(std::ostream& out, const GarbageRise& garbage) {
    const int flags = (garbage.manuallySpecified ? 1 : 0) | (garbage.holesCertain ? 2 : 0);
    if (garbage.lines <= 0 && garbage.holeMasks.empty() && flags == 0) { out << '0'; return; }
    out << '[' << std::max(0, garbage.lines) << ',' << garbageMasksJson(garbage)
        << ',' << std::fixed << std::setprecision(4) << garbage.matchRatio << ',' << flags << ']';
}

void writeTrainingPlacement(std::ostream& out, const TimelineStep& step, bool holdUsed) {
    if (!isPiece(step.placedPiece)) { out << '0'; return; }
    out << "[\"" << sanitizePiece(step.placedPiece) << "\"," << step.placementX
        << ',' << step.placementY << ',' << (step.placementRotation & 3)
        << ',' << std::max(0, step.clearedLines) << ',' << (holdUsed ? 1 : 0) << ']';
}

void writeTrainingLabelsJson(std::ostream& out, const std::vector<TimelineStep>& timeline) {
    out << '[';
    for (std::size_t index = 0; index < timeline.size(); ++index) {
        if (index) out << ',';
        const auto& step = timeline[index];
        const int flags = (step.manuallyFixed ? 1 : 0) |
                          (step.queueManuallyFixed ? 2 : 0) |
                          (step.orderAutomaticallyResolved ? 4 : 0) |
                          (step.recoveryUnresolved ? 8 : 0);
        out << '[' << eventMilliseconds(step.startSeconds) << ',' << eventMilliseconds(step.timeSeconds)
            << ",\"" << sanitizePiece(step.piece) << "\",\"" << jsonEscape(step.action)
            << "\",\"" << sanitizePiece(step.hold) << "\",\"" << sanitizePieces(step.next) << "\",";
        writeTrainingGarbage(out, step.garbage);
        out << ',';
        writeTrainingPlacement(out, step, index > 0 && timeline[index - 1].holdUsed);
        out << ',' << std::fixed << std::setprecision(4) << step.score << ',' << flags
            << ",\"" << jsonEscape(compactBoard(boardString(step.observed)))
            << "\",\"" << jsonEscape(compactBoard(boardString(step.board)))
            << "\",\"" << jsonEscape(compactBoard(boardString(step.fullBoard))) << "\"]";
    }
    out << ']';
}

std::string sha256File(const std::filesystem::path& path) {
#ifdef __EMSCRIPTEN__
    return wasmSha256File(path);
#else
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD returned = 0;
    std::vector<unsigned char> object;
    std::vector<unsigned char> digest;

    const auto close = [&] {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &returned, 0) != 0) {
        close();
        return {};
    }
    object.resize(objectLength);
    digest.resize(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) != 0) {
        close();
        return {};
    }
    std::ifstream source(path, std::ios::binary);
    if (!source) {
        close();
        return {};
    }
    // Keep the hashing chunk off the Windows UI thread's limited stack.  A
    // 1 MiB std::array here can overflow the default stack exactly when the
    // reviewer presses Export.
    std::vector<char> buffer(1024 * 1024);
    while (source) {
        source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = source.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) != 0) {
            close();
            return {};
        }
    }
    if (BCryptFinishHash(hash, digest.data(), hashLength, 0) != 0) {
        close();
        return {};
    }
    close();
    static constexpr char Hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (unsigned char value : digest) {
        result.push_back(Hex[value >> 4]);
        result.push_back(Hex[value & 15]);
    }
    return result;
#endif
}

bool writeTrainingAnnotation(const std::filesystem::path& input, const std::filesystem::path& directory,
                              const std::vector<TimelineStep>& p1, const std::vector<TimelineStep>& p2,
                              RecoveryOutput& output, std::string& error) {
    std::error_code ec;
    const auto trainingDirectory = directory / L"training";
    std::filesystem::create_directories(trainingDirectory, ec);
    if (ec) { error = "Could not create training annotation directory: " + ec.message(); return false; }
    const std::string hash = sha256File(input);
    if (hash.empty()) { error = "Could not calculate SHA-256 for the source video"; return false; }
    const std::string shortHash = hash.substr(0, 16);
    const std::wstring wideShortHash(shortHash.begin(), shortHash.end());
    const std::wstring stem = input.stem().wstring();
    std::filesystem::path extension = input.extension();
    if (extension.empty()) extension = L".video";

    output.trainingDatasetDirectory = trainingDirectory;
    output.trainingVideoPath.clear();
    output.trainingAnnotationPath.clear();
    output.trainingManifestPath.clear();
    output.trainingIndexPath = trainingDirectory / L"dataset-index.v2.jsonl";

    std::filesystem::path relativeVideo;
    std::string storage = "external";
    if (output.includeSourceVideoInTraining) {
        const auto videoDirectory = trainingDirectory / L"video";
        std::filesystem::create_directories(videoDirectory, ec);
        if (ec) { error = "Could not create training video directory: " + ec.message(); return false; }
        const auto storedVideo = videoDirectory / (stem + L"_" + wideShortHash + extension.wstring());
        if (std::filesystem::exists(storedVideo, ec)) {
            if (ec) { error = "Could not inspect training video path: " + ec.message(); return false; }
            storage = "existing";
        } else {
            ec.clear();
            // A same-volume hard link is instant and remains valid if the
            // original video is renamed or deleted. Fall back to a normal
            // copy for another volume or a filesystem without hard links.
            std::filesystem::create_hard_link(input, storedVideo, ec);
            if (!ec) {
                storage = "hardlink";
            } else {
                ec.clear();
                std::filesystem::copy_file(input, storedVideo, std::filesystem::copy_options::none, ec);
                if (ec) { error = "Could not save source video for training: " + ec.message(); return false; }
                storage = "copy";
            }
        }
        output.trainingVideoPath = storedVideo;
        relativeVideo = std::filesystem::path("video") / storedVideo.filename();
    }

    const std::string sourceVideoPath = relativeVideo.empty() ? input.generic_u8string() : relativeVideo.generic_u8string();
    const std::string relativeVideoPath = relativeVideo.generic_u8string();
    std::uintmax_t sourceBytes = std::filesystem::file_size(input, ec);
    if (ec) { sourceBytes = 0; ec.clear(); }

    const auto temporaryAnnotation = trainingDirectory /
        (stem + L"_" + wideShortHash + L"_training.v2.pending.json");
    std::ofstream file(temporaryAnnotation, std::ios::binary | std::ios::trunc);
    if (!file) { error = "Could not write training annotation"; return false; }
    const auto p1Pages = pageEntries(p1);
    const auto p2Pages = pageEntries(p2);
    file << "{\"schema\":\"tetris-video-recovery.training/v2\""
         << ",\"source\":{\"videoPath\":\"" << jsonEscape(sourceVideoPath) << "\""
         << ",\"originalVideoPath\":\"" << jsonEscape(input.generic_u8string()) << "\""
         << ",\"sha256\":\"" << hash << "\",\"bytes\":" << sourceBytes << "}"
         << ",\"dataset\":{\"selfContained\":" << (!relativeVideo.empty() ? "true" : "false")
         << ",\"videoFile\":\"" << jsonEscape(relativeVideoPath) << "\""
         << ",\"videoStorage\":\"" << storage << "\"}"
         << ",\"stateConvention\":\"timeline[i] is the board/queue state while active is controlled; placing timeline[i].active produces timeline[i+1].board\""
         << ",\"annotationPolicy\":\"human-approved legal reconstruction; manual=true marks an explicitly chosen candidate\""
         << ",\"review\":{\"approvedByExplicitExport\":" << (output.humanApproved ? "true" : "false") << '}'
         << ",\"trajectoryRepair\":";
    writeTrajectoryRepairJson(file, output.trajectoryRepair);
    file << ",\"recoveryQuality\":";
    writeRecoveryQualityJson(file, evaluateRecoveryQualityForReport(p1, p2), output.sourceStartSeconds);
    file << ",\"playerNames\":{\"p1\":";
    writePlayerNameJson(file, output.p1PlayerName);
    file << ",\"p2\":";
    writePlayerNameJson(file, output.p2PlayerName);
    file << "},\"session\":{\"videoDurationSeconds\":" << std::fixed
         << std::setprecision(6) << output.videoDurationSeconds
         << ",\"analyzedUntilSeconds\":" << output.analyzedUntilSeconds
         << ",\"sourceStartSeconds\":" << output.sourceStartSeconds
         << ",\"sourceEndSeconds\":" << output.sourceEndSeconds
         << ",\"sourceVideoDurationSeconds\":"
         << output.sourceVideoDurationSeconds << '}'
         << ",\"encoding\":{\"replay\":\"te1\",\"labels\":\"timeline-labels/v1\""
         << ",\"observations\":\"compact-board/v1\",\"queueObservations\":\"queue-delta/v1\""
         << ",\"timeUnit\":\"milliseconds\",\"checkpointInterval\":" << EventCheckpointInterval << '}'
         << ",\"integrity\":{\"sourceSha256\":\"" << hash << "\",\"counts\":{"
         << "\"p1Labels\":" << p1.size() << ",\"p2Labels\":" << p2.size()
         << ",\"p1QueueObservations\":" << output.queueObservationsP1.size()
         << ",\"p2QueueObservations\":" << output.queueObservationsP2.size() << "}}"
         << ",\"replay\":" << eventReplayJson(p1Pages, p2Pages, !p2.empty())
         << ",\"queueObservations\":{\"format\":\"queue-delta/v1\""
         << ",\"columns\":[\"dtMs\",\"active\",\"hold\",\"next\",\"decodedHold\",\"decodedNext\",\"flags\",\"nextUnclearMask\"]"
         << ",\"p1\":";
    writeTrainingQueueJson(file, output.queueObservationsP1);
    file << ",\"p2\":";
    writeTrainingQueueJson(file, output.queueObservationsP2);
    file << "},\"labels\":{\"format\":\"timeline-labels/v1\""
         << ",\"columns\":[\"startMs\",\"timeMs\",\"active\",\"action\",\"hold\",\"next\",\"garbage\",\"placement\",\"score\",\"flags\",\"observed\",\"board\",\"fullBoard\"]"
         << ",\"p1\":";
    writeTrainingLabelsJson(file, p1);
    file << ",\"p2\":";
    writeTrainingLabelsJson(file, p2);
    file << "}}\n";
    file.close();
    if (!file) { error = "Could not finish training annotation"; return false; }

    const std::string annotationHash = sha256File(temporaryAnnotation);
    if (annotationHash.empty()) { error = "Could not calculate training annotation SHA-256"; return false; }
    const std::wstring wideAnnotationHash(annotationHash.begin(), annotationHash.begin() + 16);
    output.trainingAnnotationPath = trainingDirectory /
        (stem + L"_" + wideShortHash + L"_" + wideAnnotationHash + L"_training.v2.json");
    if (std::filesystem::exists(output.trainingAnnotationPath, ec)) {
        ec.clear();
        std::filesystem::remove(temporaryAnnotation, ec);
    } else {
        ec.clear();
        std::filesystem::rename(temporaryAnnotation, output.trainingAnnotationPath, ec);
        if (ec) { error = "Could not finalize training annotation: " + ec.message(); return false; }
    }
    const std::uintmax_t annotationBytes = std::filesystem::file_size(output.trainingAnnotationPath, ec);
    if (ec) { error = "Could not inspect training annotation: " + ec.message(); return false; }

    output.trainingManifestPath = trainingDirectory /
        (stem + L"_" + wideShortHash + L"_" + wideAnnotationHash + L"_manifest.v2.json");
    std::ofstream manifest(output.trainingManifestPath, std::ios::binary | std::ios::trunc);
    if (!manifest) { error = "Could not write training manifest"; return false; }
    manifest << "{\"schema\":\"tetris-video-recovery.dataset-manifest/v2\""
             << ",\"annotationFile\":\"" << jsonEscape(output.trainingAnnotationPath.filename().generic_u8string()) << "\""
             << ",\"annotationSha256\":\"" << annotationHash << "\",\"annotationBytes\":" << annotationBytes
             << ",\"videoFile\":\"" << jsonEscape(relativeVideoPath) << "\""
             << ",\"videoStorage\":\"" << storage << "\""
             << ",\"sourceSha256\":\"" << hash << "\""
             << ",\"formats\":{\"replay\":\"te1\",\"labels\":\"timeline-labels/v1\",\"queue\":\"queue-delta/v1\"}"
             << ",\"counts\":{\"p1Labels\":" << p1.size() << ",\"p2Labels\":" << p2.size()
             << ",\"p1QueueObservations\":" << output.queueObservationsP1.size()
             << ",\"p2QueueObservations\":" << output.queueObservationsP2.size() << '}'
             << ",\"immutable\":true,\"approved\":" << (output.humanApproved ? "true" : "false") << "}\n";
    manifest.close();
    if (!manifest) { error = "Could not finish training manifest"; return false; }

    bool indexed = false;
    {
        std::ifstream existingIndex(output.trainingIndexPath, std::ios::binary);
        if (existingIndex) {
            std::ostringstream contents;
            contents << existingIndex.rdbuf();
            indexed = contents.str().find(annotationHash) != std::string::npos;
        }
    }
    if (!indexed) {
        std::ofstream indexFile(output.trainingIndexPath, std::ios::binary | std::ios::app);
        if (!indexFile) { error = "Could not update training dataset index"; return false; }
        indexFile << "{\"schema\":\"tetris-video-recovery.dataset-index-entry/v2\""
                  << ",\"sourceSha256\":\"" << hash << "\",\"annotationSha256\":\"" << annotationHash
                  << "\",\"annotationFile\":\"" << jsonEscape(output.trainingAnnotationPath.filename().generic_u8string())
                  << "\",\"manifestFile\":\"" << jsonEscape(output.trainingManifestPath.filename().generic_u8string())
                  << "\",\"videoFile\":\"" << jsonEscape(relativeVideoPath)
                  << "\",\"approved\":" << (output.humanApproved ? "true" : "false") << "}\n";
        if (!indexFile) { error = "Could not finish training dataset index"; return false; }
    }
    return true;
}

bool writeOutputs(const std::filesystem::path& input, const std::filesystem::path& directory,
                  const std::vector<TimelineStep>& p1, const std::vector<TimelineStep>& p2,
                  bool twoPlayers, bool includeTrainingDataset,
                  RecoveryOutput& output, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) { error = "Could not create output directory: " + ec.message(); return false; }
    const auto name = input.stem().wstring() + L"_tetris_recovered";
    output.jsonPath = directory / (name + L".json");
    // Real Internet Shortcut files can be double-clicked to open the URL in
    // the user's default browser.  The old .url.txt files only opened a text
    // editor and made the hand-off unnecessarily error-prone.
    output.p1UrlPath = directory / (name + L"_P1.url");
    output.p2UrlPath = directory / (name + L"_P2.url");
    output.combinedUrlPath = directory / (name + L"_2P.url");
    output.linksPath = directory / (name + L"_links.html");
    output.reportPath = directory / (name + L"_report.html");

    auto p1Pages = pageEntries(p1);
    auto p2Pages = pageEntries(p2);
    PageEntry blank;
    blank.board = std::string(BoardWidth * BoardHeight, '_');
    if (p1Pages.empty()) p1Pages.push_back(blank);
    if (p2Pages.empty()) p2Pages.push_back(blank);
    // Keep the position inside every link. The payload after '#' is unchanged;
    // only ?page=<1-based position> is added in front of it.
    const std::string p1BaseUrl = simulatorUrl(p1Pages, p2Pages, false);
    const std::string p2BaseUrl = simulatorUrl(p2Pages, p1Pages, false);
    const std::string combinedBaseUrl = twoPlayers ? simulatorUrl(p1Pages, p2Pages, true) : p1BaseUrl;
    const std::size_t p1PageCount = replayPageCount(p1Pages, p2Pages, false);
    const std::size_t p2PageCount = replayPageCount(p2Pages, p1Pages, false);
    const std::size_t combinedPageCount = twoPlayers
        ? replayPageCount(p1Pages, p2Pages, true) : p1PageCount;
    output.p1Url = withPageNumber(p1BaseUrl, p1PageCount);
    output.p2Url = withPageNumber(p2BaseUrl, p2PageCount);
    output.combinedUrl = twoPlayers ? withPageNumber(combinedBaseUrl, combinedPageCount) : output.p1Url;
    const std::string simulatorData = eventReplayJson(p1Pages, p2Pages, twoPlayers);

    {
        std::ofstream p1File(output.p1UrlPath);
        std::ofstream p2File(output.p2UrlPath);
        std::ofstream combinedFile(output.combinedUrlPath);
        // Keep the Internet Shortcut target itself short. Windows first
        // passes a .url target through the shell association and can reject
        // a replay long before Chromium sees it. The local loader performs
        // the final in-browser navigation and also remains a clickable
        // fallback when automatic navigation is disabled.
        // The shortcut opens the local links page, which forwards both the
        // fragment and ?page=N to the replay. Carry the position here too so a
        // double-click lands on the same board the page shows.
        p1File << "[InternetShortcut]\nURL="
               << withPageNumber(fileUrl(output.linksPath, "p1"), p1PageCount) << '\n';
        p2File << "[InternetShortcut]\nURL="
               << withPageNumber(fileUrl(output.linksPath, "p2"), p2PageCount) << '\n';
        combinedFile << "[InternetShortcut]\nURL="
                     << withPageNumber(fileUrl(output.linksPath, "combined"), combinedPageCount) << '\n';
        if (!p1File || !p2File || !combinedFile) { error = "Could not write simulator URL files"; return false; }
    }
    {
        std::ofstream json(output.jsonPath);
        if (!json) { error = "Could not write JSON output"; return false; }
        std::ostringstream p1Json;
        std::ostringstream p2Json;
        writeTimelineJson(p1Json, p1);
        writeTimelineJson(p2Json, p2);
        json << "{\"version\":7,\"pageFormat\":\"event-replay/v1\",\"input\":\"" << jsonEscape(input.u8string())
             << "\",\"sourceRange\":{\"startSeconds\":" << std::fixed
             << std::setprecision(6) << output.sourceStartSeconds
             << ",\"endSeconds\":" << output.sourceEndSeconds
             << ",\"videoDurationSeconds\":" << output.sourceVideoDurationSeconds << '}'
             << ",\"algorithm\":\"video-analysis-html-compatible\",\"playerNames\":{\"p1\":";
        writePlayerNameJson(json, output.p1PlayerName);
        json << ",\"p2\":";
        writePlayerNameJson(json, output.p2PlayerName);
        json << "},\"p1\":" << p1Json.str()
             << ",\"p2\":" << p2Json.str()
             << ",\"queueObservations\":{\"p1\":";
        writeQueueRecognitionJson(json, output.queueObservationsP1);
        json << ",\"p2\":";
        writeQueueRecognitionJson(json, output.queueObservationsP2);
        json << "}"
             << ",\"simulatorData\":" << simulatorData
             << ",\"urls\":{\"p1\":\"" << jsonEscape(output.p1Url)
             << "\",\"p2\":\"" << jsonEscape(output.p2Url)
             << "\",\"combined\":\"" << jsonEscape(output.combinedUrl) << "\"}}\n";
    }
    if (includeTrainingDataset &&
        !writeTrainingAnnotation(input, directory, p1, p2, output, error)) return false;
    {
        std::ofstream report(output.reportPath);
        if (!report) { error = "Could not write HTML report"; return false; }
        report << "<!doctype html><meta charset='utf-8'><title>Tetris Recovery Report</title>"
               << "<style>body{font-family:system-ui;background:#111;color:#eee;padding:20px}table{border-collapse:collapse}td,th{border:1px solid #555;padding:4px}.board{font:12px monospace;white-space:pre}</style>"
               << "<h1>Tetris recovery report</h1><p>P1 プレイヤー："
               << htmlEscape(output.p1PlayerName.text.empty()
                    ? std::string("未認識") : output.p1PlayerName.text)
               << " / P2 プレイヤー："
               << htmlEscape(output.p2PlayerName.text.empty()
                    ? std::string("未認識") : output.p2PlayerName.text)
               << "</p><p>Algorithm: NEXT-timed CNN probability averaging with probability-aware legal-move beam scoring. Rows marked <em>manual</em> were fixed from a generated legal candidate before export.</p>";
        const auto reportPlayer = [&](const char* title, const std::vector<TimelineStep>& timeline) {
            report << "<h2>" << title << " (" << timeline.size() << " phases)</h2><table><tr><th>#</th><th>start</th><th>end</th><th>active</th><th>action</th><th>hold</th><th>next</th><th>shift</th><th>score</th><th>manual</th><th>board</th></tr>";
            for (std::size_t i = 0; i < timeline.size(); ++i) {
                const auto& step = timeline[i];
                report << "<tr><td>" << i << "</td><td>" << step.startSeconds << "</td><td>" << step.timeSeconds
                       << "</td><td>" << sanitizePiece(step.piece) << "</td><td>" << step.action
                       << "</td><td>" << sanitizePiece(step.hold) << "</td><td>" << sanitizePieces(step.next)
                       << "</td><td>" << step.garbage.lines << "</td><td>" << step.score
                       << "</td><td>" << (step.manuallyFixed ? "yes" : "") << "</td><td class='board'>";
                for (int y = VisibleRows; y < BoardHeight; ++y) {
                    for (int x = 0; x < BoardWidth; ++x) report << cellChar(step.board[index(x, y)]);
                    report << "<br>";
                }
                report << "</td></tr>";
            }
            report << "</table>";
        };
        reportPlayer("P1", p1);
        reportPlayer("P2", p2);
        report << "<h2>Simulator URLs</h2><p><a href='" << jsonEscape(output.p1Url) << "'>P1</a></p>"
               << "<p><a href='" << jsonEscape(output.p2Url) << "'>P2</a></p>"
               << "<p><a href='" << jsonEscape(output.combinedUrl) << "'>2P</a></p>";
    }
    {
        std::ofstream links(output.linksPath);
        if (!links) { error = "Could not write simulator links page"; return false; }
        links << "<!doctype html><meta charset='utf-8'><title>Tetris Simulator links</title>"
              << "<style>body{font-family:system-ui;background:#111;color:#eee;padding:24px}a{display:block;margin:16px 0;color:#7cf;font-size:1.1rem}.pager{display:flex;gap:8px;align-items:center;margin:18px 0;font-size:.95rem}.pager input{width:5.5em;padding:4px 6px;background:#1d212a;color:#eee;border:1px solid #444;border-radius:6px}.pager button{background:#262c38;color:#eee;border:1px solid #444;border-radius:6px;padding:4px 10px;font-size:1rem;cursor:pointer}</style>"
              << "<h1>Tetris Simulator</h1>"
              << "<p>P1 プレイヤー："
              << htmlEscape(output.p1PlayerName.text.empty()
                    ? std::string("未認識") : output.p1PlayerName.text)
              << "<br>P2 プレイヤー："
              << htmlEscape(output.p2PlayerName.text.empty()
                    ? std::string("未認識") : output.p2PlayerName.text)
              << "</p>"
              // The links carry ?page=N so the viewer opens a real position
              // instead of the empty opening board on page 1.
              << "<div class='pager'>局面番号 <input id='pageInput' type='number' min='1' max='"
              << combinedPageCount << "' value='" << combinedPageCount
              << "' aria-label='局面番号（1始まり）'>"
              << "<button type='button' id='pagePrev' aria-label='前の局面'>‹</button>"
              << "<button type='button' id='pageNext' aria-label='次の局面'>›</button>"
              << "<span>/ " << combinedPageCount << "（1始まり）</span></div>"
              << "<a id='link-p1' href='" << jsonEscape(output.p1Url) << "'>Open P1 simulator</a>"
              << "<a id='link-p2' href='" << jsonEscape(output.p2Url) << "'>Open P2 simulator</a>"
              << "<a id='link-combined' href='" << jsonEscape(output.combinedUrl) << "'>Open 2P simulator</a>"
              << "<script>const u={p1:\"" << jsonEscape(p1BaseUrl)
              << "\",p2:\"" << jsonEscape(p2BaseUrl)
              << "\",combined:\"" << jsonEscape(combinedBaseUrl)
              << "\"};const n={p1:" << p1PageCount << ",p2:" << p2PageCount
              << ",combined:" << combinedPageCount << "};"
              << "const withPage=(url,page)=>{const i=url.indexOf('#');"
                 "const head=i<0?url:url.slice(0,i),tail=i<0?'':url.slice(i);"
                 "return head+(head.indexOf('?')<0?'?':'&')+'page='+page+tail;};"
              << "const input=document.getElementById('pageInput');"
              << "const apply=value=>{const raw=String(value).trim();"
                 "let page=raw===''?n.combined:Math.round(Number(raw));"
                 "if(!isFinite(page))page=n.combined;page=Math.max(1,page);"
                 "input.value=page;"
                 "for(const key of Object.keys(u)){const link=document.getElementById('link-'+key);"
                 "if(link)link.href=withPage(u[key],Math.min(page,n[key]));}};"
              << "input.addEventListener('change',()=>apply(input.value));"
              << "document.getElementById('pagePrev').addEventListener('click',()=>apply(Number(input.value)-1));"
              << "document.getElementById('pageNext').addEventListener('click',()=>apply(Number(input.value)+1));"
              << "apply(n.combined);"
              // A shortcut can also carry ?page=N itself; forward it.
              << "const k=location.hash.slice(1);"
              << "if(u[k]){const requested=parseInt(new URLSearchParams(location.search).get('page'),10);"
                 "const page=isFinite(requested)&&requested>=1?Math.min(requested,n[k]):n[k];"
                 "location.replace(withPage(u[k],page));}</script>";
        if (!links) { error = "Could not finish simulator links page"; return false; }
    }
    return true;
}

bool parsePair(const std::string& value, std::array<double, 2>& out) {
    std::stringstream stream(value);
    char comma = 0;
    return static_cast<bool>(stream >> out[0] >> comma >> out[1]) && comma == ',';
}

bool parseRect(const std::string& value, LayoutRect& out) {
    std::stringstream stream(value);
    char c1 = 0, c2 = 0, c3 = 0;
    return static_cast<bool>(stream >> out.x >> c1 >> out.y >> c2 >> out.w >> c3 >> out.h) &&
           c1 == ',' && c2 == ',' && c3 == ',';
}

void trimString(std::string& value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
}

} // namespace

std::vector<double> boardSampleTimes(double startSeconds, double endSeconds, int requestedSamples) {
    // 連鎖（ライン消し）直後のアニメーションを避ける除外時間。設定で変更できる。
    const auto envSeconds = [](const char* name, double fallback) {
        if (const char* v = std::getenv(name)) {
            try {
                const double parsed = std::stod(v);
                if (parsed >= 0.0 && parsed <= 3.0) return parsed;
            } catch (...) {
            }
        }
        return fallback;
    };
    const double LineClearTailSeconds = envSeconds("TR_BOARD_TAIL_SECONDS", .85);
    constexpr double BorderlineDurationSeconds = .90;
    const double BorderlineTailSeconds = envSeconds("TR_BOARD_TAIL_BORDERLINE_SECONDS", .80);

    const int samples = std::max(3, requestedSamples);
    const double duration = std::max(0.0, endSeconds - startSeconds);
    double usableDuration = duration;
    if (duration > LineClearTailSeconds) {
        const double excludedTail = duration <= BorderlineDurationSeconds
            ? BorderlineTailSeconds : LineClearTailSeconds;
        usableDuration = std::max(0.0, duration - excludedTail);
    }

    std::vector<double> result;
    result.reserve(static_cast<std::size_t>(samples));
    // Diagnostic-only sampling offset for separately named experimental
    // builds.  Zero preserves the historical beginning-of-section requests.
    // A value of 0.5 samples the centre of each section, avoiding a frame
    // delivered exactly on a queue-transition boundary without increasing
    // the number of CNN inferences.
#ifdef TR_GPU_SAFE_SAMPLING_DEFAULT
    double sectionOffset = 0.5;
#else
    double sectionOffset = 0.0;
#endif
#ifndef __EMSCRIPTEN__
    if (const char* configured = std::getenv("TR_BOARD_SAMPLE_SECTION_OFFSET")) {
        try {
            sectionOffset = std::clamp(std::stod(configured), 0.0, 0.95);
        } catch (...) {
            sectionOffset = 0.0;
        }
    }
#endif
    for (int sample = 0; sample < samples; ++sample) {
        // Use the beginning of each equal section, including the exact NEXT
        // change timestamp for the first inference in the default profile.
        result.push_back(startSeconds + usableDuration *
            (static_cast<double>(sample) + sectionOffset) / samples);
    }
    return result;
}

bool loadSettings(const std::filesystem::path& path, Settings& settings, std::string& error) {
    if (!std::filesystem::exists(path)) return true;
    std::ifstream file(path);
    if (!file) { error = "Could not open configuration file: " + path.string(); return false; }
    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        trimString(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size() - 2); continue; }
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        std::string key = line.substr(0, equal);
        std::string value = line.substr(equal + 1);
        trimString(key);
        trimString(value);
        try {
            if (section == "video" && key == "sample_interval") settings.sampleIntervalSeconds = std::stod(value);
            else if (section == "video" && key == "initial_analysis_seconds") settings.initialAnalysisSeconds = std::stod(value);
            else if (section == "video" && key == "queue_confirm") settings.queueConfirmSamples = std::stoi(value);
            else if (section == "video" && key == "queue_beam_width") settings.queueBeamWidth = std::stoi(value);
            else if (section == "video" && key == "queue_7bag") settings.queueUseSevenBag = std::stoi(value) != 0;
            else if (section == "video" && key == "queue_cnn") settings.queueCnnEnabled = std::stoi(value) != 0;
            else if (section == "video" && key == "queue_cnn_unclear_threshold") settings.queueCnnUnclearThreshold = std::stod(value);
            else if (section == "video" && key == "queue_cnn_piece_confidence_threshold") settings.queueCnnPieceConfidenceThreshold = std::stod(value);
            else if (section == "video" && key == "queue_cnn_dark_pixel_limit") settings.queueCnnDarkPixelLimit = std::stoi(value);
            else if (section == "video" && key == "queue_cnn_dark_ratio_threshold") settings.queueCnnDarkRatioThreshold = std::stod(value);
            else if (section == "video" && key == "force_exact_onnx") settings.forceExactOnnxMatch = std::stoi(value) != 0;
            else if (section == "video" && key == "perfect_clear_effect_guard") settings.perfectClearEffectGuard = std::stoi(value) != 0;
            else if (section == "video" && key == "perfect_clear_effect_lookback_steps") settings.perfectClearEffectLookbackSteps = std::stoi(value);
            else if (section == "video" && key == "perfect_clear_effect_beam_width") settings.perfectClearEffectBeamWidth = std::stoi(value);
            else if (section == "video" && key == "perfect_clear_effect_observation_weight") settings.perfectClearEffectObservationWeight = std::stod(value);
            else if (section == "video" && key == "onnx_samples") settings.onnxSamples = std::stoi(value);
            else if (section == "video" && key == "beam_width") settings.beamWidth = std::stoi(value);
            else if (section == "video" && key == "shift_threshold") settings.shiftThreshold = std::stod(value);
            else if (section == "video" && key == "order_ambiguity_score_margin") settings.orderAmbiguityScoreMargin = std::stod(value);
            else if (section == "video" && key == "score_exact") settings.weights.exactMatch = std::stod(value);
            else if (section == "video" && key == "score_empty") settings.weights.emptyMatch = std::stod(value);
            else if (section == "video" && key == "score_color") settings.weights.colorMismatch = std::stod(value);
            else if (section == "video" && key == "score_missing") settings.weights.missingBlock = std::stod(value);
            else if (section == "video" && key == "score_extra") settings.weights.extraBlock = std::stod(value);
            else if (section == "video" && key == "score_placed_color") settings.weights.placedColorMatch = std::stod(value);
            else if (section == "video" && key == "score_placed_color_mismatch") settings.weights.placedColorMismatch = std::stod(value);
            else if (section == "video" && key == "score_placed_complete") settings.weights.placedColorComplete = std::stod(value);
            else if (section == "video" && key == "score_hole") settings.weights.garbageHoleMatch = std::stod(value);
            else if (section == "video" && key == "score_hole_mismatch") settings.weights.garbageHoleMismatch = std::stod(value);
            else if (section == "video" && key == "accurate_fallback_enabled") settings.accurateFallbackEnabled = std::stoi(value) != 0;
            else if (section == "video" && key == "accurate_fallback_beam_width") settings.accurateFallbackBeamWidth = std::stoi(value);
            else if (section == "video" && key == "accurate_fallback_ranker") settings.accurateFallbackRankerPath = std::filesystem::u8path(value);
            else if (section == "player_name_ocr" && key == "enabled") settings.playerNameOcr.enabled = std::stoi(value) != 0;
            else if (section == "player_name_ocr" && key == "prefer_onnx") settings.playerNameOcr.preferOnnx = std::stoi(value) != 0;
            else if (section == "player_name_ocr" && key == "reference_width") settings.playerNameOcr.referenceWidth = std::stod(value);
            else if (section == "player_name_ocr" && key == "reference_height") settings.playerNameOcr.referenceHeight = std::stod(value);
            else if (section == "player_name_ocr" && key == "upscale") settings.playerNameOcr.upscale = std::stoi(value);
            else if (section == "player_name_ocr" && key == "languages") settings.playerNameOcr.languages = value;
            else if (section == "player_name_ocr" && key == "onnx_model") settings.playerNameOcr.onnxModelPath = std::filesystem::u8path(value);
            else if (section == "player_name_ocr" && key == "dictionary") settings.playerNameOcr.dictionaryPath = std::filesystem::u8path(value);
            else if (section == "player_name_ocr" && key == "executable") settings.playerNameOcr.executablePath = std::filesystem::u8path(value);
            else if (section == "player_name_ocr" && key == "tessdata") settings.playerNameOcr.tessdataPath = std::filesystem::u8path(value);
            else if (section == "players" && key == "p1_enabled") settings.player1Enabled = std::stoi(value) != 0;
            else if (section == "players" && key == "p2_enabled") settings.player2Enabled = std::stoi(value) != 0;
            else if ((section == "p1" || section == "p2") && key == "board") {
                parseRect(value, section == "p1" ? settings.p1.board : settings.p2.board);
            } else if ((section == "p1" || section == "p2") && key == "hold") {
                parsePair(value, section == "p1" ? settings.p1.hold : settings.p2.hold);
            } else if ((section == "p1" || section == "p2") && key == "name_crop") {
                parseRect(value, section == "p1" ? settings.p1.name : settings.p2.name);
            } else if ((section == "p1" || section == "p2") && key == "next") {
                // Legacy first-next override. The remaining locations retain
                // the source file's exact 122/175/225/275/325 layout.
                parsePair(value, section == "p1" ? settings.p1.next[0] : settings.p2.next[0]);
            } else if ((section == "p1" || section == "p2") && key.size() == 5 && key.rfind("next", 0) == 0 && key[4] >= '1' && key[4] <= '5') {
                const int index = key[4] - '1';
                parsePair(value, section == "p1" ? settings.p1.next[index] : settings.p2.next[index]);
            }
        } catch (...) {
            error = "Invalid configuration value: " + key;
            return false;
        }
    }
    settings.sampleIntervalSeconds = std::clamp(settings.sampleIntervalSeconds, .001, 1.0);
    settings.initialAnalysisSeconds = std::max(0.0, settings.initialAnalysisSeconds);
    settings.queueConfirmSamples = std::clamp(settings.queueConfirmSamples, 1, 6);
    settings.queueBeamWidth = std::clamp(settings.queueBeamWidth, 8, 512);
    settings.queueCnnUnclearThreshold = std::clamp(settings.queueCnnUnclearThreshold, 0.0, 1.0);
    settings.queueCnnPieceConfidenceThreshold = std::clamp(
        settings.queueCnnPieceConfidenceThreshold, 0.0, 1.0);
    settings.queueCnnDarkPixelLimit = std::clamp(settings.queueCnnDarkPixelLimit, 0, 255);
    settings.queueCnnDarkRatioThreshold = std::clamp(settings.queueCnnDarkRatioThreshold, 0.0, 1.0);
    settings.perfectClearEffectLookbackSteps = std::clamp(settings.perfectClearEffectLookbackSteps, 0, 6);
    settings.perfectClearEffectBeamWidth = std::clamp(settings.perfectClearEffectBeamWidth, 1, 2000);
    settings.perfectClearEffectObservationWeight = std::clamp(
        settings.perfectClearEffectObservationWeight, 0.0, 1.0);
    settings.onnxSamples = std::clamp(settings.onnxSamples, 3, 20);
    settings.beamWidth = std::clamp(settings.beamWidth, 1, 2000);
    settings.accurateFallbackBeamWidth = std::clamp(
        settings.accurateFallbackBeamWidth, 32, 2000);
    settings.repairTimeBudgetSeconds = std::clamp(
        settings.repairTimeBudgetSeconds, 0.0, 3600.0);
    // Operators can bound the repair ladder without editing the shared ini.
    // Zero (the default) preserves the historical unbounded behaviour.
    if (const char* configured = std::getenv("TR_REPAIR_TIME_BUDGET_SECONDS")) {
        try {
            settings.repairTimeBudgetSeconds = std::clamp(
                std::stod(configured), 0.0, 3600.0);
        } catch (...) {
        }
    }
    settings.shiftThreshold = std::clamp(settings.shiftThreshold, 0.0, 1.0);
    settings.orderAmbiguityScoreMargin = std::clamp(settings.orderAmbiguityScoreMargin, 0.0, 1000.0);
    settings.playerNameOcr.referenceWidth = std::max(
        1.0, settings.playerNameOcr.referenceWidth);
    settings.playerNameOcr.referenceHeight = std::max(
        1.0, settings.playerNameOcr.referenceHeight);
    settings.playerNameOcr.upscale = std::clamp(
        settings.playerNameOcr.upscale, 1, 6);
    const auto resolveOcrPath = [&](std::filesystem::path& configured) {
        if (configured.empty()) return;
        if (configured.is_relative()) configured = path.parent_path() / configured;
        configured = std::filesystem::absolute(configured).lexically_normal();
    };
    resolveOcrPath(settings.playerNameOcr.executablePath);
    resolveOcrPath(settings.playerNameOcr.tessdataPath);
    resolveOcrPath(settings.playerNameOcr.onnxModelPath);
    resolveOcrPath(settings.playerNameOcr.dictionaryPath);
    settings.accurateFallbackRanker.reset();
    if (settings.accurateFallbackEnabled &&
        !settings.accurateFallbackRankerPath.empty()) {
        std::filesystem::path rankerPath = settings.accurateFallbackRankerPath;
        if (rankerPath.is_relative()) rankerPath = path.parent_path() / rankerPath;
        rankerPath = std::filesystem::absolute(rankerPath).lexically_normal();
        auto ranker = std::make_shared<NeuralRanker>();
        if (!ranker->load(rankerPath, error)) return false;
        settings.accurateFallbackRankerPath = rankerPath;
        settings.accurateFallbackRanker = std::move(ranker);
    }
#ifdef TR_AGGRESSIVE_DEFAULTS
    settings.onnxSamples = 3;
    settings.beamWidth = 32;
#endif
    return true;
}

struct TrajectoryAnomaly {
    bool detected = false;
    int player = 0;
    std::size_t phase = 0;
    double timeSeconds = 0;
    double matchRatio = 1;
};

double visibleBoardMatchRatio(const TimelineStep& step) {
    int matched = 0;
    for (int cell = BoardWidth * VisibleRows;
         cell < BoardWidth * BoardHeight; ++cell) {
        if (step.board[static_cast<std::size_t>(cell)] ==
            step.observed[static_cast<std::size_t>(cell)]) ++matched;
    }
    return static_cast<double>(matched) /
        static_cast<double>(BoardWidth * VisibleRows);
}

TrajectoryAnomaly detectTrajectoryAnomaly(const std::vector<TimelineStep>& timeline,
                                           int player) {
    constexpr double PerfectClearObservationGraceSeconds = 3.0;
    // A legal trajectory can have one very poor sample group immediately
    // before a multi-line clear (4/5.mov reached 0.87 and recovered on the
    // next phase).  Reserve the one-shot alarm for a much deeper collapse;
    // the repeated threshold catches sustained drift such as 5/6.mov.
    constexpr double ImmediateFailureRatio = .80;
    constexpr double RepeatedFailureRatio = .95;
    double perfectClearGraceUntil = -1;
    int consecutiveLow = 0;

    for (std::size_t phase = 0; phase < timeline.size(); ++phase) {
        const TimelineStep& step = timeline[phase];
        const bool boardEmpty = std::none_of(
            step.board.begin(), step.board.end(),
            [](Cell cell) { return cell != Cell::Empty; });
        if (step.clearedLines > 0 && boardEmpty) {
            perfectClearGraceUntil = std::max(
                perfectClearGraceUntil,
                step.timeSeconds + PerfectClearObservationGraceSeconds);
        }
        if (step.action == "end" || step.timeSeconds <= perfectClearGraceUntil + .000001) {
            consecutiveLow = 0;
            continue;
        }

        const double ratio = visibleBoardMatchRatio(step);
        consecutiveLow = ratio < RepeatedFailureRatio ? consecutiveLow + 1 : 0;
        if (ratio < ImmediateFailureRatio || consecutiveLow >= 2) {
            return {true, player, phase, step.timeSeconds, ratio};
        }
    }
    return {};
}

TrajectoryAnomaly detectTrajectoryAnomaly(const std::vector<TimelineStep>& p1,
                                           const std::vector<TimelineStep>& p2) {
    const TrajectoryAnomaly first = detectTrajectoryAnomaly(p1, 1);
    const TrajectoryAnomaly second = detectTrajectoryAnomaly(p2, 2);
    if (!first.detected) return second;
    if (!second.detected || first.timeSeconds <= second.timeSeconds) return first;
    return second;
}

struct TrajectoryHealth {
    int unresolved = 0;
    int severe = 0;
    int low = 0;
    double minimum = 1;
    double average = 1;
};

TrajectoryHealth trajectoryHealth(const std::vector<TimelineStep>& timeline) {
    TrajectoryHealth health;
    double sum = 0;
    int count = 0;
    for (const TimelineStep& step : timeline) {
        if (step.action == "end") continue;
        if (step.recoveryUnresolved) ++health.unresolved;
        const double ratio = visibleBoardMatchRatio(step);
        health.severe += ratio < .80;
        health.low += ratio < .90;
        health.minimum = std::min(health.minimum, ratio);
        sum += ratio;
        ++count;
    }
    health.average = count ? sum / count : 1;
    return health;
}

bool healthierTrajectory(const TrajectoryHealth& trial,
                         const TrajectoryHealth& baseline) {
    if (trial.unresolved != baseline.unresolved) {
        return trial.unresolved < baseline.unresolved;
    }
    if (trial.severe != baseline.severe) return trial.severe < baseline.severe;
    if (trial.low != baseline.low) return trial.low < baseline.low;
    if (std::abs(trial.average - baseline.average) > .000001) {
        return trial.average > baseline.average;
    }
    return trial.minimum > baseline.minimum + .000001;
}

// v59: true when `piece` was clearly read as NEXT[0] during the half second
// before the turn that starts at `turnStartSeconds` (the mino left NEXT[0]
// when it spawned). Used to stop the board-driven identity repair from
// replacing a clearly observed mino just to fit the board.
bool nextHeadClearlyObserved(const std::vector<QueueRecognitionSample>& samples,
                             double turnStartSeconds, Cell piece) {
    if (!isPiece(piece)) return false;
    int total = 0, clear = 0;
    for (const auto& sample : samples) {
        if (sample.timeSeconds < turnStartSeconds - 0.5) continue;
        if (sample.timeSeconds >= turnStartSeconds) break;
        ++total;
        const auto& obs = sample.observation;
        const bool headUnclear = !obs.nextUnclear.empty() && obs.nextUnclear[0];
        // The CNN's own (stabilized) reading, not the decoder's corrected
        // queue: the evidence must be independent of the path being repaired.
        if (!headUnclear && obs.cnnUsed && !obs.next.empty() &&
            obs.next[0] == piece) ++clear;
    }
    return total >= 5 && clear * 10 >= total * 8;
}

// NEXT/HOLD recognition normally determines the active mino before the board
// beam starts.  A visually wrong but shift-consistent five-piece window can
// therefore pass both the queue-transition decoder and the 7-bag check.  The
// old fallback only widened placement/garbage hypotheses while keeping that
// wrong mino sequence immutable, so it could never repair this failure mode.
//
// Run an expensive identity/alignment lane only after the solved boards prove
// that the ordinary path is already broken.  The lane compares all seven
// pieces plus a false-slide/no-lock hypothesis in a short look-back window,
// then freezes the selected identities and validates them again with the
// ordinary one-piece-per-phase beam. Healthy matches never enter this code.
bool repairSuspiciousTrajectoryForPlayer(const Settings& baseSettings,
                                         RecoveryOutput& output,
                                         int player,
                                         TrajectoryRepairReport& report);

bool repairSuspiciousQueueIdentityForPlayer(const Settings& baseSettings,
                                            RecoveryOutput& output,
                                            int player) {
    // Keep the per-player entry point independently switchable as well. The
    // review/re-solve path calls this function directly, bypassing the
    // two-player wrapper; without this guard an A/B regression run claiming
    // to disable queue repair would silently leave it enabled.
    if (std::getenv("TR_DISABLE_QUEUE_BOARD_REPAIR")) return false;
    auto& raw = player == 1 ? output.rawP1 : output.rawP2;
    auto& solved = player == 1 ? output.p1 : output.p2;
    if (raw.size() != solved.size() || raw.size() < 3) return false;

    const std::vector<TimelineStep> originalRaw = raw;
    const std::vector<TimelineStep> originalSolved = solved;
    bool adoptedAny = false;
    // One bad visual run can hide a second one later in the same match. Keep
    // the retry bounded, but re-detect after every objectively better path.
    for (int attempt = 0; attempt < 6; ++attempt) {
        const TrajectoryAnomaly anomaly = detectTrajectoryAnomaly(solved, player);
        if (!anomaly.detected) break;
        TrajectoryHealth baselineHealth = trajectoryHealth(solved);

        // A missed HOLD transition commonly reverses exactly two adjacent
        // locks and then rejoins the same queue state.  The general all-piece
        // lane is both expensive and prone to pruning this short alternate
        // history.  Test only the physically characteristic A,H -> H,A pair:
        // the second decoded piece is also the HOLD value seen around the
        // pair.  This lane is anomaly-only and changes no queue observations.
        constexpr std::size_t PairLookbackPhases = 24;
        constexpr std::size_t PairLookaheadPhases = 6;
        const std::size_t pairFirst = anomaly.phase > PairLookbackPhases
            ? anomaly.phase - PairLookbackPhases : 0;
        const std::size_t pairLast = std::min(
            raw.size() - 2, anomaly.phase + PairLookaheadPhases);
        Settings pairSettings = baseSettings;
        pairSettings.beamWidth = std::clamp(
            std::max(128, baseSettings.beamWidth * 4), 1, 192);
        pairSettings.completeLegalMoveSearch = true;
        pairSettings.garbageBacktracking = true;
        // Compare identities with equal search effort. A better placement
        // found only because the swapped lane uses a wider beam must not be
        // credited to swapping the pieces (AB66: correct TI became IT).
        if (!std::getenv("TR_DISABLE_SAME_ORDER_WIDENING")) {
            auto widenedOriginal = TetrisEngine::beamSearch(raw, pairSettings);
            if (widenedOriginal.size() == raw.size()) {
                const auto widenedHealth = trajectoryHealth(widenedOriginal);
                if (healthierTrajectory(widenedHealth, baselineHealth) &&
                    widenedHealth.minimum + .02 >= baselineHealth.minimum) {
                    std::cerr << "queue_same_order_wider_beam=1 player=" << player
                              << " severe=" << baselineHealth.severe << "->"
                              << widenedHealth.severe << " minimum="
                              << baselineHealth.minimum << "->" << widenedHealth.minimum << '\n';
                    solved = std::move(widenedOriginal);
                    baselineHealth = widenedHealth;
                    adoptedAny = true;
                    if (player == 1) output.beamCacheP1.reset();
                    else output.beamCacheP2.reset();
                    if (!detectTrajectoryAnomaly(solved, player).detected) break;
                }
            }
        }
        std::vector<TimelineStep> bestPairRaw;
        std::vector<TimelineStep> bestPairSolved;
        TrajectoryHealth bestPairHealth = baselineHealth;
        std::size_t bestPairSource = 0;
        std::vector<std::size_t> pairSources;
        for (std::size_t source = pairFirst; source <= pairLast; ++source) {
            if (raw[source].manuallyFixed || raw[source + 1].manuallyFixed ||
                raw[source].queueManuallyFixed ||
                raw[source + 1].queueManuallyFixed) continue;
            const Cell first = raw[source].piece;
            const Cell second = raw[source + 1].piece;
            if (!isPiece(first) || !isPiece(second) || first == second) continue;
            if (raw[source].hold != second && raw[source + 1].hold != second) continue;
            // The ambiguous rejoining history exists when HOLD and the NEXT
            // head show the same mino: ordinary locks yield A,H, while two
            // HOLDs can legally yield H,A and return to the same state.
            if (raw[source].next.empty() || raw[source].next.front() != second) continue;
            pairSources.push_back(source);
        }
        const std::size_t earliestPair = pairSources.empty() ? 0 : pairSources.front();
        const std::size_t latestPair = pairSources.empty() ? 0 : pairSources.back();
        std::stable_sort(pairSources.begin(), pairSources.end(),
            [&](std::size_t left, std::size_t right) {
                const auto localRatio = [&](std::size_t source) {
                    const std::size_t firstStep = std::min(source + 1, solved.size() - 1);
                    const std::size_t secondStep = std::min(source + 2, solved.size() - 1);
                    return std::min(visibleBoardMatchRatio(solved[firstStep]),
                                    visibleBoardMatchRatio(solved[secondStep]));
                };
                const double leftRatio = localRatio(left);
                const double rightRatio = localRatio(right);
                if (std::abs(leftRatio - rightRatio) > .000001) {
                    return leftRatio < rightRatio;
                }
                const auto distance = [&](std::size_t source) {
                    const std::size_t phase = source + 1;
                    return phase > anomaly.phase ? phase - anomaly.phase
                                                 : anomaly.phase - phase;
                };
                return distance(left) < distance(right);
            });
        // Preserve the strongest local mismatch plus both temporal edges of
        // the bounded anomaly window. A wrong pair can remain superficially
        // close to the image until a later line clear, so distance alone is
        // not a safe ranker. This still caps the expensive solves at three.
        if (!pairSources.empty()) {
            std::vector<std::size_t> shortlist{pairSources.front(),
                                               earliestPair, latestPair};
            std::sort(shortlist.begin(), shortlist.end());
            shortlist.erase(std::unique(shortlist.begin(), shortlist.end()),
                            shortlist.end());
            pairSources = std::move(shortlist);
        }
        for (const std::size_t source : pairSources) {
            const Cell first = raw[source].piece;
            const Cell second = raw[source + 1].piece;

            std::vector<TimelineStep> candidateRaw = raw;
            candidateRaw[source].piece = second;
            candidateRaw[source + 1].piece = first;
            std::vector<TimelineStep> candidateSolved =
                TetrisEngine::beamSearch(candidateRaw, pairSettings);
            if (candidateSolved.size() != candidateRaw.size()) continue;
            const TrajectoryHealth candidateHealth =
                trajectoryHealth(candidateSolved);
            if (!healthierTrajectory(candidateHealth, bestPairHealth)) continue;
            bestPairHealth = candidateHealth;
            bestPairRaw = std::move(candidateRaw);
            bestPairSolved = std::move(candidateSolved);
            bestPairSource = source;
        }
        if (!bestPairRaw.empty() &&
            bestPairHealth.severe < baselineHealth.severe &&
            bestPairHealth.minimum + .02 >= baselineHealth.minimum) {
            std::cerr << "queue_double_hold_pair_repair=1 player=" << player
                      << " attempt=" << (attempt + 1)
                      << " source=" << bestPairSource
                      << " before=" << cellChar(raw[bestPairSource].piece)
                      << cellChar(raw[bestPairSource + 1].piece)
                      << " after=" << cellChar(bestPairRaw[bestPairSource].piece)
                      << cellChar(bestPairRaw[bestPairSource + 1].piece)
                      << " severe=" << baselineHealth.severe << "->"
                      << bestPairHealth.severe
                      << " minimum=" << baselineHealth.minimum << "->"
                      << bestPairHealth.minimum << '\n';
            raw = std::move(bestPairRaw);
            solved = std::move(bestPairSolved);
            if (player == 1) output.beamCacheP1.reset();
            else output.beamCacheP2.reset();
            adoptedAny = true;
            if (!detectTrajectoryAnomaly(solved, player).detected) break;
            // Re-detect and test the next bounded HOLD pair before opening the
            // much broader all-piece lane. Several missed HOLD crops in one
            // match should be repaired as several cheap legal pair histories,
            // not as one unconstrained rewrite.
            continue;
        }

        // First compare the operation histories that the logical queue beam
        // previously collapsed on rejoin. These candidates preserve actual
        // HOLD/NEXT physics and can express IT<->TI as well as an inserted or
        // omitted lock without opening all seven minos at every phase.
        auto queueSamples = player == 1
            ? output.queueObservationsP1 : output.queueObservationsP2;
        std::vector<std::vector<QueuePhase>> logicalAlternatives;
        const char* holdUnclearLane = std::getenv("TR_REPAIR_HOLD_UNCLEAR_LANE");
        const bool holdUnclearLaneEnabled =
            holdUnclearLane && std::string(holdUnclearLane) == "1";
        // In the HOLD-unclear lane the re-decoded "selected" path may itself be
        // the alternative we need (e.g. HOLD swap instead of a direct lock), so
        // every candidate, including index 0, is compared with the path the
        // ordinary decoder chose.
        std::vector<QueuePhase> ordinaryPhases;
        if (holdUnclearLaneEnabled) {
            auto ordinarySamples = queueSamples;
            ordinaryPhases = buildQueuePhasesFromSamples(
                ordinarySamples, baseSettings, output.videoDurationSeconds, 0);
            Settings queueDecodeSettings = baseSettings;
            queueDecodeSettings.queueUnclearHoldIsEvidence = false;
            buildQueuePhasesFromSamples(
                queueSamples, queueDecodeSettings, output.videoDurationSeconds, 0,
                &logicalAlternatives);
            std::cerr << "queue_hold_unclear_lane=1 player=" << player
                      << " alternatives=" << logicalAlternatives.size() << '\n';
        } else {
            buildQueuePhasesFromSamples(
                queueSamples, baseSettings, output.videoDurationSeconds, 0,
                &logicalAlternatives);
        }
        Settings logicalSettings = baseSettings;
        // This is a screening pass across a handful of legal queue histories,
        // not the final high-accuracy placement retry. Keep it cheap; the
        // chosen path is still eligible for the ordinary widened fallback.
        logicalSettings.beamWidth = std::clamp(
            std::max(64, baseSettings.beamWidth * 2), 1, 96);
        logicalSettings.completeLegalMoveSearch = false;
        logicalSettings.garbageBacktracking = false;
        std::vector<TimelineStep> bestLogicalRaw;
        std::vector<TimelineStep> bestLogicalSolved;
        TrajectoryHealth bestLogicalHealth = baselineHealth;
        std::size_t logicalCandidateIndex = 0;
        const auto firstDifferenceSeconds = [](
                const std::vector<QueuePhase>& selected,
                const std::vector<QueuePhase>& alternate) {
            const std::size_t count = std::min(selected.size(), alternate.size());
            for (std::size_t index = 0; index < count; ++index) {
                if (selected[index].active != alternate[index].active ||
                    selected[index].hold != alternate[index].hold ||
                    selected[index].next != alternate[index].next ||
                    selected[index].action != alternate[index].action ||
                    std::abs(selected[index].start - alternate[index].start) > .0005 ||
                    std::abs(selected[index].end - alternate[index].end) > .0005) {
                    return std::min(selected[index].start, alternate[index].start);
                }
            }
            if (selected.size() != alternate.size()) {
                return count < selected.size() ? selected[count].start
                    : alternate[count].start;
            }
            return std::numeric_limits<double>::infinity();
        };
        std::vector<std::pair<double, std::size_t>> rankedAlternatives;
        if (!logicalAlternatives.empty()) {
            const bool compareWithOrdinary = holdUnclearLaneEnabled && !ordinaryPhases.empty();
            const std::vector<QueuePhase>& reference = compareWithOrdinary
                ? ordinaryPhases : logicalAlternatives.front();
            for (std::size_t index = compareWithOrdinary ? 0 : 1;
                 index < logicalAlternatives.size(); ++index) {
                const double changedAt = firstDifferenceSeconds(
                    reference, logicalAlternatives[index]);
                if (!std::isfinite(changedAt) ||
                    changedAt > anomaly.timeSeconds + 1.0 ||
                    changedAt < anomaly.timeSeconds - 15.0) continue;
                rankedAlternatives.push_back({
                    std::abs(anomaly.timeSeconds - changedAt), index});
            }
        }
        std::stable_sort(rankedAlternatives.begin(), rankedAlternatives.end());
        if (rankedAlternatives.size() > 8) rankedAlternatives.resize(8);
        for (const auto [distance, index] : rankedAlternatives) {
            (void)distance;
            std::vector<TimelineStep> candidateRaw = buildRawTimelineFromFlatBoards(
                logicalAlternatives[index], player == 1
                    ? output.boardObservationsP1 : output.boardObservationsP2);
            if (candidateRaw.size() < 3) continue;
            std::vector<TimelineStep> candidateSolved =
                TetrisEngine::beamSearch(candidateRaw, logicalSettings);
            if (candidateSolved.size() != candidateRaw.size()) continue;
            const TrajectoryHealth candidateHealth =
                trajectoryHealth(candidateSolved);
            if (!healthierTrajectory(candidateHealth, bestLogicalHealth)) continue;
            bestLogicalHealth = candidateHealth;
            bestLogicalRaw = std::move(candidateRaw);
            bestLogicalSolved = std::move(candidateSolved);
            logicalCandidateIndex = index + 1;
        }
        const int logicalSevereGain =
            baselineHealth.severe - bestLogicalHealth.severe;
        const int requiredLogicalGain = std::max(2, baselineHealth.severe / 4);
        if (!bestLogicalRaw.empty() &&
            logicalSevereGain >= requiredLogicalGain &&
            bestLogicalHealth.minimum + .02 >= baselineHealth.minimum) {
            std::cerr << "queue_logical_rejoin_repair=1 player=" << player
                      << " attempt=" << (attempt + 1)
                      << " anomaly_phase=" << anomaly.phase
                      << " candidate=" << logicalCandidateIndex << '/'
                      << logicalAlternatives.size()
                      << " phases=" << raw.size() << "->"
                      << bestLogicalRaw.size()
                      << " severe=" << baselineHealth.severe << "->"
                      << bestLogicalHealth.severe
                      << " minimum=" << baselineHealth.minimum << "->"
                      << bestLogicalHealth.minimum << '\n';
            raw = std::move(bestLogicalRaw);
            solved = std::move(bestLogicalSolved);
            if (player == 1) output.beamCacheP1.reset();
            else output.beamCacheP2.reset();
            adoptedAny = true;
            // Re-detect using the newly selected logical history. If it is
            // healthy, avoid the broader identity lane entirely.
            if (!detectTrajectoryAnomaly(solved, player).detected) break;
            baselineHealth = trajectoryHealth(solved);
        }

        constexpr std::size_t LookbackPhases = 18;
        constexpr std::size_t LookaheadPhases = 12;
        const std::size_t firstSource = anomaly.phase > LookbackPhases
            ? anomaly.phase - LookbackPhases : 0;
        const std::size_t lastSource = std::min(
            raw.size() - 2, anomaly.phase + LookaheadPhases);

        std::vector<TimelineStep> flexibleRaw = raw;
        for (std::size_t phase = firstSource; phase <= lastSource; ++phase) {
            if (!flexibleRaw[phase].manuallyFixed &&
                !flexibleRaw[phase].queueManuallyFixed) {
                flexibleRaw[phase].pieceIdentityFlexible = true;
            }
        }

        Settings flexibleSettings = baseSettings;
        flexibleSettings.beamWidth = std::clamp(
            std::max(256, baseSettings.beamWidth * 8), 1, 512);
        flexibleSettings.completeLegalMoveSearch = true;
        flexibleSettings.garbageBacktracking = true;
        flexibleSettings.forceExactOnnxMatch = true;
        std::vector<TimelineStep> flexible =
            TetrisEngine::beamSearch(flexibleRaw, flexibleSettings);
        if (flexible.size() != raw.size()) break;

        std::vector<TimelineStep> correctedRaw = raw;
        std::size_t corrections = 0;
        const char* respectEnv = std::getenv("TR_REPAIR_RESPECT_CLEAR_NEXT");
        const bool respectClearNext = respectEnv && std::string(respectEnv) == "1";
        const auto& queueSamplesForRespect = player == 1 ? output.queueObservationsP1 : output.queueObservationsP2;
        int keptClearNext = 0;
        std::ostringstream beforePieces;
        std::ostringstream afterPieces;
        for (std::size_t source = firstSource; source <= lastSource; ++source) {
            if (correctedRaw[source].manuallyFixed ||
                correctedRaw[source].queueManuallyFixed) continue;
            const Cell selected = flexible[source + 1].placedPiece;
            const Cell previous = correctedRaw[source].piece;
            if (isPiece(previous)) beforePieces << cellChar(previous);
            else beforePieces << '-';
            if (isPiece(selected)) afterPieces << cellChar(selected);
            else afterPieces << '-';
            if (selected == previous) continue;
            if (respectClearNext && isPiece(previous) &&
                nextHeadClearlyObserved(queueSamplesForRespect, correctedRaw[source].startSeconds, previous)) {
                ++keptClearNext;
                continue;
            }
            correctedRaw[source].piece = selected;
            correctedRaw[source].uncertain = false;
            correctedRaw[source].pieceIdentityFlexible = false;
            ++corrections;
        }
        if (respectClearNext && keptClearNext > 0) {
            std::cerr << "queue_board_repair_kept_clear_next=" << keptClearNext
                      << " player=" << player << '\n';
        }
        // A short corrupted queue run can insert/delete or replace several
        // phases, but an unconstrained rewrite of a whole match is not an
        // acceptable automatic correction.
        if (corrections == 0 || corrections > 12) break;

        Settings validationSettings = baseSettings;
        validationSettings.beamWidth = std::clamp(
            std::max(256, baseSettings.beamWidth * 8), 1, 512);
        validationSettings.completeLegalMoveSearch = true;
        validationSettings.garbageBacktracking = true;
        std::vector<TimelineStep> validated =
            TetrisEngine::beamSearch(correctedRaw, validationSettings);
        if (validated.size() != raw.size()) break;
        const TrajectoryHealth validatedHealth = trajectoryHealth(validated);
        // Identity repair must remove at least one severe board mismatch. A
        // tiny mean-score win is insufficient evidence for overriding NEXT.
        if (!healthierTrajectory(validatedHealth, baselineHealth) ||
            validatedHealth.severe >= baselineHealth.severe) break;

        std::cerr << "queue_board_repair=1 player=" << player
                  << " attempt=" << (attempt + 1)
                  << " anomaly_phase=" << anomaly.phase
                  << " window=" << firstSource << '-' << lastSource
                  << " corrections=" << corrections
                  << " pieces_before=" << beforePieces.str()
                  << " pieces_after=" << afterPieces.str()
                  << " severe=" << baselineHealth.severe << "->"
                  << validatedHealth.severe
                  << " minimum=" << baselineHealth.minimum << "->"
                  << validatedHealth.minimum << '\n';
        raw = std::move(correctedRaw);
        solved = std::move(validated);
        if (player == 1) output.beamCacheP1.reset();
        else output.beamCacheP2.reset();
        adoptedAny = true;
    }
    // Automatic queue repair is transactional.  A partial rewrite that still
    // leaves a sustained anomaly is more dangerous than the explicit warning
    // produced by the original path, because it can move the first bad turn.
    // Keep partial attempts only as diagnostics and restore the input unless
    // the complete player trajectory is healthy at the end.
    const TrajectoryHealth originalHealth = trajectoryHealth(originalSolved);
    const TrajectoryHealth finalHealth = trajectoryHealth(solved);
    const bool safePartialGain =
        finalHealth.severe + 10 <= originalHealth.severe &&
        // A corrected NEXT identity can move the single worst comparison by
        // one sampled animation frame even while removing dozens of sustained
        // mismatches.  Use the same two-point tolerance as the individual
        // candidate gate; otherwise the transaction restores the known-wrong
        // queue before the placement/garbage backtracker can finish it.
        finalHealth.minimum + .02 >= originalHealth.minimum;
    if (adoptedAny && detectTrajectoryAnomaly(solved, player).detected &&
        !safePartialGain) {
        raw = originalRaw;
        solved = originalSolved;
        if (player == 1) output.beamCacheP1.reset();
        else output.beamCacheP2.reset();
        std::cerr << "queue_identity_repair_rollback=1 player=" << player
                  << " reason=remaining-trajectory-anomaly\n";
        return false;
    }
    // v71: a partial identity repair (an anomaly still remains) may have
    // replaced correct NEXT identities while the real error was a placement
    // the backtracking ladder fixes on the original identities (yeiP 24 P1:
    // queue repair rewrote Z,L to I,Z with severe 116 -> 91; the ladder on the
    // original identities reaches 116 -> 1). Run the same ladder on both and
    // switch to the original identities only when that result is strictly
    // healthier. Otherwise the partial repair is restored exactly, so healthy
    // players and complete repairs are never affected.
    // TR_QUEUE_PARTIAL_COMPARE=0 disables.
    static const bool partialCompare = [] {
        const char* v = std::getenv("TR_QUEUE_PARTIAL_COMPARE");
        return !(v && std::string(v) == "0");
    }();
    if (partialCompare && adoptedAny && detectTrajectoryAnomaly(solved, player).detected) {
        const std::vector<TimelineStep> repairedRaw = raw;
        const std::vector<TimelineStep> repairedSolved = solved;
        TrajectoryRepairReport repairedReport;
        repairSuspiciousTrajectoryForPlayer(baseSettings, output, player, repairedReport);
        const TrajectoryHealth repairedLadder = trajectoryHealth(solved);

        raw = originalRaw;
        solved = originalSolved;
        if (player == 1) output.beamCacheP1.reset();
        else output.beamCacheP2.reset();
        TrajectoryRepairReport originalReport;
        repairSuspiciousTrajectoryForPlayer(baseSettings, output, player, originalReport);
        const TrajectoryHealth originalLadder = trajectoryHealth(solved);

        const bool useOriginal = !repairedReport.timeBudgetExceeded &&
            !originalReport.timeBudgetExceeded &&
            healthierTrajectory(originalLadder, repairedLadder);
        std::cerr << "queue_partial_compare=1 player=" << player
                  << " repaired_ladder_severe=" << repairedLadder.severe
                  << " original_ladder_severe=" << originalLadder.severe
                  << " repaired_min=" << repairedLadder.minimum
                  << " original_min=" << originalLadder.minimum
                  << " chosen=" << (useOriginal ? "original-identities" : "partial-repair") << '\n';
        if (player == 1) output.beamCacheP1.reset();
        else output.beamCacheP2.reset();
        if (useOriginal) return true;  // raw holds the original identities
        raw = repairedRaw;
        solved = repairedSolved;
    }
    return adoptedAny;
}

// False certifies that both players' raw and solved timelines were left
// unchanged (including transactional rollback). Both players must be checked.
bool repairSuspiciousQueueIdentity(const Settings& settings,
                                   RecoveryOutput& output) {
    if (std::getenv("TR_DISABLE_QUEUE_BOARD_REPAIR")) return false;
    bool changed = false;
    if (settings.player1Enabled) {
        changed = repairSuspiciousQueueIdentityForPlayer(settings, output, 1) || changed;
    }
    if (settings.player2Enabled) {
        changed = repairSuspiciousQueueIdentityForPlayer(settings, output, 2) || changed;
    }
    return changed;
}

bool neuralTieBreaksVisibleAmbiguity(
        const std::vector<TimelineStep>& neural,
        const TrajectoryHealth& neuralHealth,
        const std::vector<TimelineStep>& linear,
        const TrajectoryHealth& linearHealth) {
    // A total-match tie can still hide a spatially different visible board,
    // notably the two equal-ratio placements in corrected match 19. The NN's
    // row/column features were trained for exactly that ambiguity. Conversely,
    // match 11 differs only in the four unobserved rows during a garbage rise;
    // no image evidence can decide it, so retain the deterministic linear path.
    if (healthierTrajectory(neuralHealth, linearHealth) ||
        healthierTrajectory(linearHealth, neuralHealth) ||
        neural.size() != linear.size()) return false;
    // Phase 0 is the opening anchor and the terminal phase is frequently
    // clipped by match segmentation. Neither is a supervised placement (the
    // dataset intentionally excludes both), so they cannot decide a tie.
    for (std::size_t phase = 1; phase + 1 < neural.size(); ++phase) {
        const auto visible = static_cast<std::size_t>(BoardWidth * VisibleRows);
        if (!std::equal(
                neural[phase].board.end() - visible,
                neural[phase].board.end(),
                linear[phase].board.end() - visible)) return true;
    }
    return false;
}

std::string trajectoryDifferenceCause(const std::vector<TimelineStep>& baseline,
                                      const std::vector<TimelineStep>& repaired) {
    const std::size_t count = std::min(baseline.size(), repaired.size());
    for (std::size_t phase = 0; phase < count; ++phase) {
        const TimelineStep& before = baseline[phase];
        const TimelineStep& after = repaired[phase];
        if (before.board == after.board && before.fullBoard == after.fullBoard &&
            before.garbage.lines == after.garbage.lines &&
            before.garbage.holeMasks == after.garbage.holeMasks &&
            before.placedPiece == after.placedPiece &&
            before.placementX == after.placementX &&
            before.placementY == after.placementY &&
            before.placementRotation == after.placementRotation) continue;
        if (before.garbage.lines != after.garbage.lines ||
            before.garbage.holeMasks != after.garbage.holeMasks) {
            return "garbage-rise-backtracking";
        }
        return "placement-lookahead";
    }
    return "no-improvement";
}

namespace {

// One repair ladder re-runs the entire match at beam 128-256 with the garbage
// graph and the accurate ranker enabled. Measured against the fast pass that
// is 6-14x the cost, so a single pathological match can dominate a batch even
// when the repair is eventually rejected. The ladder therefore honours an
// optional wall-clock ceiling. The beam reports one progress step per phase,
// which is fine-grained enough to stop within a fraction of a second.
class RepairBudgetExceeded : public std::exception {
public:
    const char* what() const noexcept override {
        return "trajectory repair time budget exceeded";
    }
};

class RepairBudget {
public:
    explicit RepairBudget(double seconds) : seconds_(seconds) {
        if (seconds_ > 0) {
            deadline_ = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(
                    static_cast<long long>(seconds_ * 1000.0));
        }
    }
    bool enabled() const { return seconds_ > 0; }
    bool exceeded() const { return exceeded_; }
    // Returns an empty timeline when the ceiling was hit. Callers must test
    // exceeded() before using the result; an empty vector is never a valid
    // repair because the fast pass always produced raw.size() phases.
    std::vector<TimelineStep> search(const std::vector<TimelineStep>& raw,
                                     const Settings& settings) {
        if (!enabled()) return TetrisEngine::beamSearch(raw, settings);
        try {
            return TetrisEngine::beamSearch(raw, settings, [this](
                    std::size_t, std::size_t, std::size_t, std::size_t) {
                if (std::chrono::steady_clock::now() < deadline_) return;
                exceeded_ = true;
                throw RepairBudgetExceeded();
            });
        } catch (const RepairBudgetExceeded&) {
            return {};
        }
    }
private:
    double seconds_ = 0;
    bool exceeded_ = false;
    std::chrono::steady_clock::time_point deadline_{};
};

} // namespace

bool repairSuspiciousTrajectoryForPlayer(const Settings& baseSettings,
                                         RecoveryOutput& output,
                                         int player,
                                         TrajectoryRepairReport& report) {
    const auto& currentTimeline = player == 1 ? output.p1 : output.p2;
    TrajectoryAnomaly anomaly = detectTrajectoryAnomaly(currentTimeline, player);
    if (!anomaly.detected) {
        const auto unresolved = std::find_if(
            currentTimeline.begin(), currentTimeline.end(),
            [](const TimelineStep& step) { return step.recoveryUnresolved; });
        if (unresolved != currentTimeline.end()) {
            const std::size_t phase = static_cast<std::size_t>(
                unresolved - currentTimeline.begin());
            anomaly = {true, player, phase, unresolved->timeSeconds,
                       visibleBoardMatchRatio(*unresolved)};
        }
    }
    if (!anomaly.detected) return false;

    report = {};
    report.attempted = true;
    report.player = anomaly.player;
    report.firstSuspiciousPhase = anomaly.phase;
    report.firstSuspiciousSeconds = anomaly.timeSeconds;
    report.initialBeamWidth = baseSettings.beamWidth;
    report.finalBeamWidth = baseSettings.beamWidth;

    const auto& raw = anomaly.player == 1 ? output.rawP1 : output.rawP2;
    auto& solved = anomaly.player == 1 ? output.p1 : output.p2;
    const TrajectoryHealth initial = trajectoryHealth(solved);
    report.initialSeverePhases = initial.severe;
    report.initialMinimumMatchRatio = initial.minimum;

    Settings widened = baseSettings;
    widened.beamWidth = std::clamp(
        std::max(128, baseSettings.beamWidth * 4), 1, 500);
    // v79: every ladder rung may spawn above a stack that a big garbage rise
    // pushed over the field (XTveY 89 P2). Healthy players never get here.
    widened.extraSpawnRows = true;

    RepairBudget budget(baseSettings.repairTimeBudgetSeconds);
    const auto ladderStarted = std::chrono::steady_clock::now();
    std::vector<TimelineStep> best;
    int bestBeamWidth = widened.beamWidth;
    TrajectoryHealth bestHealth;
    std::string cause;

    // Returns false when the optional ceiling stopped the ladder. `best` is not
    // a usable timeline in that case, so the caller must keep the fast pass.
    const bool ladderCompleted = [&]() -> bool {
    best = budget.search(raw, widened);
    if (budget.exceeded()) return false;
    bestHealth = trajectoryHealth(best);
    if (best.size() != raw.size()) bestHealth.unresolved =
        std::numeric_limits<int>::max();
    cause = trajectoryDifferenceCause(solved, best);

    // A false rise can still leave a superficially legal trajectory whose
    // worst frame is just above the severe threshold.  Match 130 did exactly
    // that: widening the placement beam hid the later collapse while retaining
    // a false one-line rise immediately before it.  Therefore a rise close to
    // the first sustained mismatch always opens the no-rise/alternate-count
    // search, even when placement lookahead alone appears to recover.  This is
    // bounded to an already suspicious player; healthy matches keep the fast
    // path and never pay for the expanded garbage graph.
    const std::size_t riseWindowFirst = anomaly.phase > 4
        ? anomaly.phase - 4 : 0;
    const std::size_t riseWindowLast = std::min(
        solved.size(), anomaly.phase + 3);
    const bool nearbyRise = std::any_of(
        solved.begin() + static_cast<std::ptrdiff_t>(riseWindowFirst),
        solved.begin() + static_cast<std::ptrdiff_t>(riseWindowLast),
        [](const TimelineStep& step) { return step.garbage.lines > 0; });
    if (nearbyRise || !healthierTrajectory(bestHealth, initial) ||
        bestHealth.severe > 0 || bestHealth.unresolved > 0) {
        Settings garbageRetry = widened;
        garbageRetry.garbageBacktracking = true;
        report.garbageHypothesesSearched = true;
        std::vector<TimelineStep> garbageTrial =
            budget.search(raw, garbageRetry);
        if (budget.exceeded()) return false;
        TrajectoryHealth garbageHealth = trajectoryHealth(garbageTrial);
        if (garbageTrial.size() != raw.size()) garbageHealth.unresolved =
            std::numeric_limits<int>::max();
        if (healthierTrajectory(garbageHealth, bestHealth)) {
            best = std::move(garbageTrial);
            bestHealth = garbageHealth;
            cause = "garbage-rise-backtracking";
        }
        // Second stage, only while the player is still unhealthy: also try
        // every hole column for the alternate rise counts (J-up 18: five rows
        // whose top two, cleared at once, had a different hole).  Healthy
        // and already-repaired players never pay for this wider graph.
        // TR_GARBAGE_ALT_HOLES=0 disables it.
        static const bool altHoles = [] {
            const char* v = std::getenv("TR_GARBAGE_ALT_HOLES");
            return !(v && std::string(v) == "0");
        }();
        if (altHoles && (bestHealth.severe > 0 || bestHealth.unresolved > 0)) {
            Settings altRetry = garbageRetry;
            altRetry.garbageAltHoleVariants = true;
            std::vector<TimelineStep> altTrial = budget.search(raw, altRetry);
            if (budget.exceeded()) return false;
            TrajectoryHealth altHealth = trajectoryHealth(altTrial);
            if (altTrial.size() != raw.size()) altHealth.unresolved =
                std::numeric_limits<int>::max();
            if (healthierTrajectory(altHealth, bestHealth)) {
                best = std::move(altTrial);
                bestHealth = altHealth;
                cause = "garbage-hole-backtracking";
            }
        }
    }

    // The accurate ranker is deliberately last: healthy matches and the
    // ordinary fast pass never evaluate it. A failed match gets one complete
    // legal/garbage graph with a substantially wider beam, and the result is
    // adopted only when objective trajectory health improves.
    if (baseSettings.accurateFallbackEnabled &&
        baseSettings.accurateFallbackRanker &&
        baseSettings.accurateFallbackRanker->valid()) {
        // Compare the nonlinear model with the GA-trained linear baseline
        // stored in the same artifact. Keeping the old runtime weights here
        // made match 11 choose the NN's unobservable hidden-row guess even
        // though the learned linear path was exactly correct.
        Settings learnedLinearRetry = widened;
        learnedLinearRetry.beamWidth = std::max(
            widened.beamWidth, baseSettings.accurateFallbackBeamWidth);
        learnedLinearRetry.completeLegalMoveSearch = true;
        learnedLinearRetry.garbageBacktracking = true;
        learnedLinearRetry.weights =
            baseSettings.accurateFallbackRanker->baselineWeights();
        std::vector<TimelineStep> learnedLinearTrial =
            budget.search(raw, learnedLinearRetry);
        if (budget.exceeded()) return false;
        TrajectoryHealth learnedLinearHealth =
            trajectoryHealth(learnedLinearTrial);
        if (learnedLinearTrial.size() != raw.size()) {
            learnedLinearHealth.unresolved = std::numeric_limits<int>::max();
        }
        if (std::getenv("TR_FALLBACK_DIAGNOSTICS")) {
            std::cerr << std::setprecision(12)
                      << "fallback_trial=learned-linear"
                      << " player=" << anomaly.player
                      << " unresolved=" << learnedLinearHealth.unresolved
                      << " severe=" << learnedLinearHealth.severe
                      << " low=" << learnedLinearHealth.low
                      << " minimum=" << learnedLinearHealth.minimum
                      << " average=" << learnedLinearHealth.average
                      << " baseline_better="
                      << healthierTrajectory(bestHealth, learnedLinearHealth)
                      << '\n';
        }
        // Inside the accuracy fallback, an observational tie belongs to the
        // learned baseline rather than the historical hand-tuned weights.
        // The latter otherwise survives only because it happened to run first
        // and gives the NN the wrong comparison path (match 11 regression).
        if (!healthierTrajectory(bestHealth, learnedLinearHealth)) {
            best = std::move(learnedLinearTrial);
            bestHealth = learnedLinearHealth;
            bestBeamWidth = learnedLinearRetry.beamWidth;
            cause = "learned-linear-fallback";
        }

        const bool skipNeuralForDiagnostics =
            std::getenv("TR_FALLBACK_SKIP_NEURAL") != nullptr;
        if (!skipNeuralForDiagnostics) {
        Settings neuralRetry = widened;
        neuralRetry.beamWidth = std::max(
            widened.beamWidth, baseSettings.accurateFallbackBeamWidth);
        neuralRetry.completeLegalMoveSearch = true;
        neuralRetry.garbageBacktracking = true;
        neuralRetry.forceExactOnnxMatch = false;
        neuralRetry.useAccurateFallbackRanker = true;
        report.neuralRankerUsed = true;
        report.garbageHypothesesSearched = true;
        std::vector<TimelineStep> neuralTrial =
            budget.search(raw, neuralRetry);
        if (budget.exceeded()) return false;
        TrajectoryHealth neuralHealth = trajectoryHealth(neuralTrial);
        if (neuralTrial.size() != raw.size()) neuralHealth.unresolved =
            std::numeric_limits<int>::max();
        const bool neuralHealthier = healthierTrajectory(neuralHealth, bestHealth);
        const bool neuralVisibleTie = neuralTieBreaksVisibleAmbiguity(
            neuralTrial, neuralHealth, best, bestHealth);
        if (std::getenv("TR_FALLBACK_DIAGNOSTICS")) {
            std::cerr << std::setprecision(12)
                      << "fallback_trial=neural"
                      << " player=" << anomaly.player
                      << " unresolved=" << neuralHealth.unresolved
                      << " severe=" << neuralHealth.severe
                      << " low=" << neuralHealth.low
                      << " minimum=" << neuralHealth.minimum
                      << " average=" << neuralHealth.average
                      << " healthier=" << neuralHealthier
                      << " visible_tie=" << neuralVisibleTie
                      << '\n';
        }
        if (neuralHealthier || neuralVisibleTie) {
            const std::string neuralDifference =
                trajectoryDifferenceCause(solved, neuralTrial);
            best = std::move(neuralTrial);
            bestHealth = neuralHealth;
            bestBeamWidth = neuralRetry.beamWidth;
            cause = neuralDifference == "garbage-rise-backtracking"
                ? "neural-garbage-backtracking"
                : "neural-ranker-fallback";
        }
        }
    }

    return true;
    }();

    if (!ladderCompleted) {
        // The ceiling stopped the ladder. Keep the fast-pass trajectory exactly
        // as it was and let the caller surface a correction request: a partial
        // ladder must never be mistaken for a verified repair.
        report.timeBudgetExceeded = true;
        report.cause = "time-budget-exceeded";
        report.searchSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - ladderStarted).count();
        report.finalBeamWidth = baseSettings.beamWidth;
        report.finalSeverePhases = initial.severe;
        report.finalMinimumMatchRatio = initial.minimum;
        std::cerr << std::setprecision(6)
                  << "trajectory_backtrack=1 player=" << report.player
                  << " phase=" << report.firstSuspiciousPhase
                  << " cause=" << report.cause
                  << " garbage_search=" << report.garbageHypothesesSearched
                  << " neural_ranker=" << report.neuralRankerUsed
                  << " adopted=0"
                  << " budget_seconds=" << baseSettings.repairTimeBudgetSeconds
                  << " search_seconds=" << report.searchSeconds
                  << " severe_before=" << report.initialSeverePhases
                  << " minimum_before=" << report.initialMinimumMatchRatio
                  << " beam=" << report.initialBeamWidth << "->"
                  << report.finalBeamWidth << '\n';
        return true;
    }

    if (healthierTrajectory(bestHealth, initial)) {
        solved = std::move(best);
        report.adopted = true;
        report.cause = std::move(cause);
        report.finalBeamWidth = bestBeamWidth;
        output.beamCacheP1.reset();
        output.beamCacheP2.reset();
    } else {
        report.cause = "no-improvement";
    }
    const TrajectoryHealth final = report.adopted
        ? trajectoryHealth(solved) : initial;
    report.finalSeverePhases = final.severe;
    report.finalMinimumMatchRatio = final.minimum;
    std::cerr << "trajectory_backtrack=1 player=" << report.player
              << " phase=" << report.firstSuspiciousPhase
              << " cause=" << report.cause
              << " garbage_search=" << report.garbageHypothesesSearched
              << " neural_ranker=" << report.neuralRankerUsed
              << " adopted=" << report.adopted
              << " severe_before=" << report.initialSeverePhases
              << " severe_after=" << report.finalSeverePhases
              << " minimum_before=" << report.initialMinimumMatchRatio
              << " minimum_after=" << report.finalMinimumMatchRatio
              << " beam=" << report.initialBeamWidth << "->"
              << report.finalBeamWidth << '\n';
    return true;
}

void repairSuspiciousTrajectory(const Settings& baseSettings,
                                RecoveryOutput& output,
                                bool unchangedIdentityAlreadyChecked = false) {
    // Piece identity belongs ahead of placement/garbage backtracking. The
    // latter cannot repair a trajectory while it is forced to place the wrong
    // NEXT-derived mino at every transition.
    if (!unchangedIdentityAlreadyChecked) {
        repairSuspiciousQueueIdentity(baseSettings, output);
    } else if (std::getenv("TR_GPU_DIAGNOSTICS")) {
        std::cerr << "queue_identity_check_reused=1 unchanged=1\n";
    }
    output.trajectoryRepair = {};
    TrajectoryRepairReport aggregate;
    int attemptedPlayers = 0;
    int adoptedPlayers = 0;
    for (int player = 1; player <= 2; ++player) {
        TrajectoryRepairReport local;
        if (!repairSuspiciousTrajectoryForPlayer(
                baseSettings, output, player, local)) continue;
        ++attemptedPlayers;
        adoptedPlayers += local.adopted;
        if (!aggregate.attempted ||
            local.firstSuspiciousSeconds < aggregate.firstSuspiciousSeconds) {
            aggregate.player = local.player;
            aggregate.firstSuspiciousPhase = local.firstSuspiciousPhase;
            aggregate.firstSuspiciousSeconds = local.firstSuspiciousSeconds;
        }
        aggregate.attempted = true;
        aggregate.adopted = aggregate.adopted || local.adopted;
        aggregate.garbageHypothesesSearched =
            aggregate.garbageHypothesesSearched || local.garbageHypothesesSearched;
        aggregate.neuralRankerUsed =
            aggregate.neuralRankerUsed || local.neuralRankerUsed;
        aggregate.initialBeamWidth = local.initialBeamWidth;
        aggregate.finalBeamWidth = std::max(
            aggregate.finalBeamWidth, local.finalBeamWidth);
        aggregate.initialSeverePhases += local.initialSeverePhases;
        aggregate.finalSeverePhases += local.finalSeverePhases;
        aggregate.initialMinimumMatchRatio = std::min(
            aggregate.initialMinimumMatchRatio, local.initialMinimumMatchRatio);
        aggregate.finalMinimumMatchRatio = std::min(
            aggregate.finalMinimumMatchRatio, local.finalMinimumMatchRatio);
        aggregate.timeBudgetExceeded =
            aggregate.timeBudgetExceeded || local.timeBudgetExceeded;
        aggregate.searchSeconds = std::max(
            aggregate.searchSeconds, local.searchSeconds);
        if (local.adopted) aggregate.cause = local.cause;
    }
    if (attemptedPlayers > 1) {
        aggregate.player = 0;
        if (adoptedPlayers > 1) aggregate.cause = "multi-player-fallback";
    }
    // A ceiling hit on either player means the match was not fully verified,
    // even if the other player was repaired successfully.
    if (aggregate.timeBudgetExceeded) aggregate.cause = "time-budget-exceeded";
    if (aggregate.attempted && aggregate.cause.empty()) {
        aggregate.cause = "no-improvement";
    }
    output.trajectoryRepair = std::move(aggregate);
}


bool firstRecoveryUnresolved(const RecoveryOutput& output,
                             int& player,
                             std::size_t& phase) {
    const auto firstFor = [](const std::vector<TimelineStep>& timeline) {
        const auto found = std::find_if(
            timeline.begin(), timeline.end(),
            [](const TimelineStep& step) { return step.recoveryUnresolved; });
        return found == timeline.end()
            ? timeline.size()
            : static_cast<std::size_t>(found - timeline.begin());
    };
    const std::size_t p1 = firstFor(output.p1);
    const std::size_t p2 = firstFor(output.p2);
    player = 0;
    phase = 0;
    if (p1 < output.p1.size() &&
        (p2 >= output.p2.size() ||
         output.p1[p1].timeSeconds <= output.p2[p2].timeSeconds)) {
        player = 1;
        phase = p1;
        return true;
    }
    if (p2 < output.p2.size()) {
        player = 2;
        phase = p2;
        return true;
    }
    return false;
}

static RecoveryQualityReport evaluateRecoveryQualityImpl(
        const std::vector<TimelineStep>& p1,
        const std::vector<TimelineStep>& p2,
        bool ignoreClearAnimations);

RecoveryQualityReport evaluateRecoveryQuality(
        const std::vector<TimelineStep>& p1,
        const std::vector<TimelineStep>& p2) {
    // 修復の意思決定には従来どおりの厳しい判定を使う。
    return evaluateRecoveryQualityImpl(p1, p2, false);
}

RecoveryQualityReport evaluateRecoveryQualityForReport(
        const std::vector<TimelineStep>& p1,
        const std::vector<TimelineStep>& p2) {
    // 出力（品質警告）は、ライン消去直後の読めない区間を除外して評価する。
    return evaluateRecoveryQualityImpl(p1, p2, true);
}

RecoveryQualityReport evaluateRecoveryQualityImpl(
        const std::vector<TimelineStep>& p1,
        const std::vector<TimelineStep>& p2,
        bool ignoreClearAnimations) {
    RecoveryQualityReport report;
    constexpr double PerfectClearObservationGraceSeconds = 1.6;

    const auto evaluatePlayer = [&](const std::vector<TimelineStep>& timeline,
                                    int player) {
        const double minBoardConfidence = [] {
            if (const char* v = std::getenv("TR_BOARD_MIN_CONFIDENCE")) {
                try {
                    const double parsed = std::stod(v);
                    if (parsed >= 0.0 && parsed <= 255.0) return parsed;
                } catch (...) {
                }
            }
            return 0.0;
        }();
        double perfectClearGraceUntil = -1;
        double clearAnimationUntil = -1;
        // ライン消去の直後は盤面がアニメーションして読み取れない。
        // その区間を「解析の不一致」として数えないための猶予。
        const double clearGraceSeconds = [] {
            if (const char* v = std::getenv("TR_CLEAR_GRACE_SECONDS")) {
                try {
                    const double parsed = std::stod(v);
                    if (parsed >= 0.0 && parsed <= 3.0) return parsed;
                } catch (...) {
                }
            }
            return 0.6;
        }();
        std::size_t runFirst = 0;
        std::size_t runLast = 0;
        int runCount = 0;
        double runMinimum = 1;

        const auto finishRun = [&] {
            if (runCount >= report.requiredConsecutivePhases) {
                const TimelineStep& first = timeline[runFirst];
                const TimelineStep& last = timeline[runLast];
                report.issues.push_back({
                    player, runFirst, runLast,
                    first.timeSeconds, last.timeSeconds,
                    runMinimum, runCount});
            }
            runCount = 0;
            runMinimum = 1;
        };

        for (std::size_t phase = 0; phase < timeline.size(); ++phase) {
            const TimelineStep& step = timeline[phase];
            if (step.recoveryUnresolved) {
                finishRun();
                continue;
            }
            const bool boardEmpty = std::none_of(
                step.board.begin(), step.board.end(),
                [](Cell cell) { return cell != Cell::Empty; });
            if (step.clearedLines > 0 && boardEmpty) {
                perfectClearGraceUntil = std::max(
                    perfectClearGraceUntil,
                    step.timeSeconds + PerfectClearObservationGraceSeconds);
            }
            if (ignoreClearAnimations && step.clearedLines > 0) {
                clearAnimationUntil = std::max(clearAnimationUntil,
                                               step.timeSeconds + clearGraceSeconds);
            }
            if (step.action == "end" ||
                step.timeSeconds <= perfectClearGraceUntil + .000001) {
                finishRun();
                continue;
            }
            if (ignoreClearAnimations && clearAnimationUntil >= 0 &&
                step.timeSeconds <= clearAnimationUntil + .000001) {
                // 消去アニメーション中は盤面が読めないので判定材料にしない。
                finishRun();
                continue;
            }

            // 盤面観測の平均自信度が低い（=ライン消去のアニメーション等で
            // 読めていない）フレームは、判定材料にしない。
            if (minBoardConfidence > 0.0 && !step.confidence.empty()) {
                double sum = 0.0;
                std::size_t count = 0;
                for (int cell = 0; cell < BoardWidth * VisibleRows; ++cell) {
                    const Cell cellValue = step.observed[BoardWidth * VisibleRows + cell];
                    if (cellValue == Cell::Empty) continue;
                    sum += step.confidence[static_cast<std::size_t>(cell)];
                    ++count;
                }
                if (count >= 12) {
                    const double mean = sum / static_cast<double>(count);
                    if (mean < minBoardConfidence) {
                        finishRun();
                        continue;
                    }
                }
            }
            const double ratio = visibleBoardMatchRatio(step);
            if (ratio < report.matchRatioThreshold) {
                if (!runCount) runFirst = phase;
                runLast = phase;
                ++runCount;
                runMinimum = std::min(runMinimum, ratio);
            } else {
                finishRun();
            }
        }
        finishRun();
    };

    evaluatePlayer(p1, 1);
    evaluatePlayer(p2, 2);
    std::sort(report.issues.begin(), report.issues.end(),
              [](const RecoveryQualityIssue& left,
                 const RecoveryQualityIssue& right) {
        if (left.startSeconds != right.startSeconds) {
            return left.startSeconds < right.startSeconds;
        }
        return left.player < right.player;
    });
    const auto isMove = [](const TimelineStep& step) {
        return step.action == "place" || step.action == "place_skipped";
    };
    const auto resolved = [](const std::vector<TimelineStep>& timeline) {
        return std::none_of(timeline.begin(), timeline.end(),
            [](const TimelineStep& step) { return step.recoveryUnresolved; });
    };
    bool terminalOnly = !report.issues.empty() && resolved(p1) && resolved(p2);
    for (auto& issue : report.issues) {
        const auto& timeline = issue.player == 1 ? p1 : p2;
        std::vector<std::size_t> moves;
        for (std::size_t phase = 0; phase < timeline.size(); ++phase) {
            if (isMove(timeline[phase])) moves.push_back(phase);
        }
        if (moves.empty() || timeline.back().action != "end") {
            terminalOnly = false;
            continue;
        }
        issue.lastMovePhase = moves.back();
        issue.movesThroughEnd = static_cast<std::size_t>(std::count_if(
            moves.begin(), moves.end(), [&](std::size_t phase) { return phase >= issue.firstPhase; }));
        const auto firstAllowed = moves[moves.size() > RecoveryQualityReport::TerminalMoveLimit
            ? moves.size() - RecoveryQualityReport::TerminalMoveLimit : 0];
        terminalOnly = terminalOnly && issue.firstPhase >= firstAllowed &&
            issue.lastPhase <= moves.back() &&
            std::all_of(timeline.begin() + issue.firstPhase,
                        timeline.begin() + issue.lastPhase + 1, isMove);
    }
    if (terminalOnly) report.terminalIssues.swap(report.issues);
    return report;
}

void forceRecoveryOpeningEmpty(RecoveryOutput& output, const Settings* settings) {
    const auto normalizePlayer = [settings](int player,
            std::vector<QueueRecognitionSample>& queue,
            std::vector<QueueRecognitionSample>& originalQueue,
            std::vector<BoardObservation>& boards,
            std::vector<TimelineStep>& raw,
            std::vector<TimelineStep>& solved) {
        const bool openingAlreadyEmpty = !boards.empty() && !queue.empty() &&
            queue.front().observation.hold == Cell::Empty &&
            std::all_of(boards.front().board.begin(), boards.front().board.end(),
                        [](Cell cell) { return cell == Cell::Empty; });
        if (openingAlreadyEmpty) return;
        if (!queue.empty()) {
            queue.front().observation.hold = Cell::Empty;
            queue.front().decoded.hold = Cell::Empty;
        }
        if (!originalQueue.empty()) {
            originalQueue.front().observation.hold = Cell::Empty;
            originalQueue.front().decoded.hold = Cell::Empty;
        }
        if (!boards.empty()) makeOpeningBoardEmpty(boards.front());
        if (!raw.empty()) makeOpeningTimelineEmpty(raw.front());
        if (!solved.empty()) makeOpeningTimelineEmpty(solved.front());
        // v58c: emptying the already-solved phase 0 must not leave phase 1
        // holding cells that no exported placement explains (the init step
        // had no active piece, so phase 1 inherited phase 0's cells without a
        // lock; match 51 P2 in the H1 run). Solve the player again from the
        // empty opening, letting the first transition choose any mino.
        const char* resolve = std::getenv("TR_OPENING_RESOLVE");
        if (!settings || !resolve || std::string(resolve) != "1") return;
        if (solved.size() < 2 || raw.size() != solved.size()) return;
        if (isPiece(solved[1].placedPiece) || solved[1].board == solved[0].board) return;
        std::vector<TimelineStep> retryRaw = raw;
        if (!isPiece(retryRaw.front().piece)) retryRaw.front().pieceIdentityFlexible = true;
        std::vector<TimelineStep> retried = TetrisEngine::beamSearch(retryRaw, *settings);
        std::cerr << "opening_resolve=1 player=" << player
                  << " phases=" << retried.size() << '/' << raw.size() << '\n';
        if (retried.size() == raw.size()) {
            raw = std::move(retryRaw);
            solved = std::move(retried);
        }
    };
    normalizePlayer(1, output.queueObservationsP1,
                    output.originalQueueObservationsP1,
                    output.boardObservationsP1, output.rawP1, output.p1);
    normalizePlayer(2, output.queueObservationsP2,
                    output.originalQueueObservationsP2,
                    output.boardObservationsP2, output.rawP2, output.p2);
}

bool solveTimelinesParallel(const Settings& settings,
                            const std::vector<TimelineStep>& rawP1,
                            const std::vector<TimelineStep>& rawP2,
                            std::vector<TimelineStep>& solvedP1,
                            std::vector<TimelineStep>& solvedP2,
                            Status* status = nullptr,
                            std::shared_ptr<BeamSearchCache>* cacheP1 = nullptr,
                            std::shared_ptr<BeamSearchCache>* cacheP2 = nullptr,
                            std::string* error = nullptr) {
    const std::size_t p1Total = settings.player1Enabled && rawP1.size() > 1 ? rawP1.size() - 1 : 0;
    const std::size_t p2Total = settings.player2Enabled && rawP2.size() > 1 ? rawP2.size() - 1 : 0;
    const std::size_t total = p1Total + p2Total;
    std::atomic<std::size_t> p1Done{0};
    std::atomic<std::size_t> p2Done{0};

    const auto progressFor = [&](int player) -> BeamProgressCallback {
        return [&, player](std::size_t completed, std::size_t /*phaseTotal*/,
                            std::size_t beamSize, std::size_t candidateCount) {
            if (player == 1) p1Done.store(std::min(completed, p1Total), std::memory_order_relaxed);
            else p2Done.store(std::min(completed, p2Total), std::memory_order_relaxed);
            if (!status) return;
            const std::size_t completedAll = p1Done.load(std::memory_order_relaxed) +
                                             p2Done.load(std::memory_order_relaxed);
            const int percent = total == 0
                ? 99
                : 84 + static_cast<int>(16.0 * static_cast<double>(completedAll) / static_cast<double>(total));
            status->progress.store(std::clamp(percent, 84, 99), std::memory_order_relaxed);
            status->setMessage("Pass 3/3: beam search P1 " + std::to_string(p1Done.load()) + "/" +
                               std::to_string(p1Total) + ", P2 " + std::to_string(p2Done.load()) + "/" +
                               std::to_string(p2Total) + " (beam " + std::to_string(beamSize) +
                               ", candidates " + std::to_string(candidateCount) + ")");
        };
    };
    const BeamProgressCallback p1Progress = status ? progressFor(1) : BeamProgressCallback{};
    const BeamProgressCallback p2Progress = status ? progressFor(2) : BeamProgressCallback{};
    const auto solveP1 = [&]() {
        if (!settings.player1Enabled) return std::vector<TimelineStep>{};
        return cacheP1
            ? TetrisEngine::beamSearchIncremental(rawP1, settings, *cacheP1, p1Progress)
            : TetrisEngine::beamSearch(rawP1, settings, p1Progress);
    };
    const auto solveP2 = [&]() {
        if (!settings.player2Enabled) return std::vector<TimelineStep>{};
        return cacheP2
            ? TetrisEngine::beamSearchIncremental(rawP2, settings, *cacheP2, p2Progress)
            : TetrisEngine::beamSearch(rawP2, settings, p2Progress);
    };
    const auto firstUnresolvedPhase = [](const std::vector<TimelineStep>& timeline) {
        const auto found = std::find_if(
            timeline.begin(), timeline.end(),
            [](const TimelineStep& step) { return step.recoveryUnresolved; });
        return found == timeline.end()
            ? timeline.size()
            : static_cast<std::size_t>(found - timeline.begin());
    };
    const auto retryCompleteReachability = [&]() {
        if (settings.completeLegalMoveSearch) return;
        Settings retry = settings;
        retry.completeLegalMoveSearch = true;
        const auto retryPlayer = [&](int player,
                                     const std::vector<TimelineStep>& raw,
                                     std::vector<TimelineStep>& solved,
                                     std::shared_ptr<BeamSearchCache>* cache) {
            const std::size_t failedAt = firstUnresolvedPhase(solved);
            if (failedAt >= solved.size()) return;
            if (status) {
                status->setMessage(
                    "Candidate 0 at P" + std::to_string(player) + " phase " +
                    std::to_string(failedAt) +
                    ": retrying complete soft-drop reachability");
            }
            std::size_t suspicious = failedAt;
            int consecutiveLow = 0;
            for (std::size_t phase = 0; phase < failedAt; ++phase) {
                const double ratio = visibleBoardMatchRatio(solved[phase]);
                consecutiveLow = ratio < .95 ? consecutiveLow + 1 : 0;
                if (ratio < .80 || consecutiveLow >= 2) {
                    suspicious = phase;
                    break;
                }
            }
            const std::size_t anchor = suspicious > 8 ? suspicious - 8 : 0;
            std::vector<TimelineStep> complete;
            bool localRetry = anchor > 0;
            if (localRetry) {
                std::vector<TimelineStep> anchoredRaw = raw;
                TimelineStep& fixed = anchoredRaw[anchor];
                const TimelineStep& known = solved[anchor];
                fixed.manuallyFixed = true;
                fixed.board = known.board;
                fixed.fullBoard = known.fullBoard;
                fixed.garbage = known.garbage;
                fixed.placedPiece = known.placedPiece;
                fixed.placementX = known.placementX;
                fixed.placementY = known.placementY;
                fixed.placementRotation = known.placementRotation;
                fixed.clearedLines = known.clearedLines;
                fixed.score = known.score;
                complete = TetrisEngine::recomputeFrom(
                    anchoredRaw, solved, anchor, retry);
                if (anchor < complete.size()) {
                    complete[anchor].manuallyFixed = raw[anchor].manuallyFixed;
                }
            } else {
                complete = TetrisEngine::beamSearch(raw, retry);
            }
            const std::size_t remaining = firstUnresolvedPhase(complete);
            bool repaired = complete.size() == raw.size() &&
                            remaining >= complete.size();
            if (!repaired && localRetry) {
                complete = TetrisEngine::beamSearch(raw, retry);
                repaired = complete.size() == raw.size() &&
                           firstUnresolvedPhase(complete) >= complete.size();
                localRetry = false;
            }
            std::cerr << "complete_reachability_retry=1 player=" << player
                      << " phase=" << failedAt
                      << " anchor=" << anchor
                      << " local=" << localRetry
                      << " repaired=" << repaired << '\n';
            if (repaired) {
                solved = std::move(complete);
                if (cache) cache->reset();
            }
        };
        if (settings.player1Enabled) retryPlayer(1, rawP1, solvedP1, cacheP1);
        if (settings.player2Enabled) retryPlayer(2, rawP2, solvedP2, cacheP2);
    };
    const auto validate = [&]() {
        const int failedPlayer = settings.player1Enabled && !rawP1.empty() && solvedP1.empty()
            ? 1
            : (settings.player2Enabled && !rawP2.empty() && solvedP2.empty() ? 2 : 0);
        if (failedPlayer != 0) {
            if (error) {
            const std::size_t failedPhase = failedPlayer == 1
                ? p1Done.load(std::memory_order_relaxed)
                : p2Done.load(std::memory_order_relaxed);
                *error = "Legal-move recovery returned no reviewable timeline for P" +
                         std::to_string(failedPlayer) + " at phase " +
                         std::to_string(failedPhase);
            }
            return false;
        }
        const std::size_t p1Phase = firstUnresolvedPhase(solvedP1);
        const std::size_t p2Phase = firstUnresolvedPhase(solvedP2);
        const int unresolvedPlayer = p1Phase < solvedP1.size()
            ? 1 : (p2Phase < solvedP2.size() ? 2 : 0);
        const std::size_t unresolvedPhase = unresolvedPlayer == 1 ? p1Phase : p2Phase;
        if (unresolvedPlayer != 0 && status) {
            status->setMessage(
                "Automatic legal-move recovery stopped at P" +
                std::to_string(unresolvedPlayer) + " phase " +
                std::to_string(unresolvedPhase) +
                "; opening correction review");
        }
        if (error) error->clear();
        return true;
    };
#ifdef __EMSCRIPTEN__
    // WebAssembly builds run in the ordinary single-threaded browser
    // environment.  Keep the exact same beamSearch calls and ordering while
    // avoiding std::async, which would require a pthread-enabled deployment
    // and SharedArrayBuffer on GitHub Pages.
    solvedP1 = solveP1();
    solvedP2 = solveP2();
    retryCompleteReachability();
    return validate();
#else
    if (settings.player1Enabled && settings.player2Enabled) {
        auto p1Future = std::async(std::launch::async, solveP1);
        solvedP2 = solveP2();
        solvedP1 = p1Future.get();
        retryCompleteReachability();
        return validate();
    }
    solvedP1 = solveP1();
    solvedP2 = solveP2();
    retryCompleteReachability();
    return validate();
#endif
}

bool refinePlacementOrderObservations(
    const std::filesystem::path& input,
    const std::filesystem::path& modelPath,
    const Settings& settings,
    Status& status,
    RecoveryOutput& output,
    std::string& error,
    ReusableAnalysisModels* reusableModels,
    bool* unchangedIdentityChecked = nullptr) {
    // Early exits cannot certify a completed identity check. The caller may
    // reuse a true result only before any settings or observations change.
    if (unchangedIdentityChecked) *unchangedIdentityChecked = false;
    bool enabled = false;
#ifdef TR_GPU_SAFE_SAMPLING_DEFAULT
    enabled = true;
#endif
    if (const char* configured = std::getenv("TR_FAST_ORDER_REFINEMENT")) {
        enabled = std::string(configured) != "0";
    }
    if (!enabled || (output.rawP1.empty() && output.rawP2.empty())) return true;

    int extraSamples = 8;
    if (const char* configured = std::getenv("TR_FAST_ORDER_REFINEMENT_SAMPLES")) {
        extraSamples = std::clamp(std::atoi(configured), 3, 20);
    }
    double detectionMargin = std::max(0.0, settings.orderAmbiguityScoreMargin);
#ifdef TR_GPU_SAFE_SAMPLING_DEFAULT
    // The final build must revisit every plausible commuting same-piece pair,
    // not only the handful close enough for the old reviewer warning. The
    // wider gate changes no placement by itself; it only collects the extra
    // probability evidence used by the direct resolver below.
    detectionMargin = std::max(detectionMargin, 1000.0);
#endif
    if (const char* configured = std::getenv("TR_FAST_ORDER_REFINEMENT_MARGIN")) {
        try {
            detectionMargin = std::clamp(std::stod(configured), 0.0, 1000.0);
        } catch (...) {
            detectionMargin = std::max(0.0, settings.orderAmbiguityScoreMargin);
        }
    }

    Settings detectionSettings = settings;
    detectionSettings.orderAmbiguityScoreMargin = detectionMargin;
    const auto p1Issues = TetrisEngine::placementOrderAmbiguities(
        output.rawP1, output.p1, detectionSettings);
    const auto p2Issues = TetrisEngine::placementOrderAmbiguities(
        output.rawP2, output.p2, detectionSettings);
    const auto p1Phases = settings.player1Enabled
        ? buildQueuePhasesFromSamples(
              output.queueObservationsP1, settings, output.analyzedUntilSeconds)
        : std::vector<QueuePhase>{};
    const auto p2Phases = settings.player2Enabled
        ? buildQueuePhasesFromSamples(
              output.queueObservationsP2, settings, output.analyzedUntilSeconds)
        : std::vector<QueuePhase>{};
    const TrajectoryAnomaly p1QueueAnomaly =
        detectTrajectoryAnomaly(output.p1, 1);
    const TrajectoryAnomaly p2QueueAnomaly =
        detectTrajectoryAnomaly(output.p2, 2);
    if (p1Issues.empty() && p2Issues.empty() &&
        !p1QueueAnomaly.detected && !p2QueueAnomaly.detected) return true;

    struct Target {
        int player = 0;
        std::size_t phase = 0;
        double start = 0;
        double end = 0;
        int samples = 0;
        bool queueRepair = false;
    };
    std::vector<Target> targets;
    targets.reserve(p1Issues.size() + p2Issues.size());
    for (const auto& issue : p1Issues) {
        if (issue.firstStep < p1Phases.size()) {
            const auto& phase = p1Phases[issue.firstStep];
            targets.push_back({1, issue.firstStep, phase.start, phase.end,
                               extraSamples, false});
        }
    }
    for (const auto& issue : p2Issues) {
        if (issue.firstStep < p2Phases.size()) {
            const auto& phase = p2Phases[issue.firstStep];
            targets.push_back({2, issue.firstStep, phase.start, phase.end,
                               extraSamples, false});
        }
    }
    const auto appendQueueRepairTarget = [&](const TrajectoryAnomaly& anomaly,
                                             const std::vector<QueuePhase>& phases) {
        if (!anomaly.detected || phases.size() < 3) return;
        constexpr std::size_t QueueRepairLookbackPhases = 22;
        constexpr std::size_t QueueRepairLookaheadPhases = 4;
        const std::size_t first = anomaly.phase > QueueRepairLookbackPhases
            ? anomaly.phase - QueueRepairLookbackPhases : 1;
        const std::size_t last = std::min(
            phases.size() - 1, anomaly.phase + QueueRepairLookaheadPhases);
        if (first >= phases.size() || last < first) return;
        const int samples = std::clamp(
            static_cast<int>((last - first + 1) *
                             static_cast<std::size_t>(extraSamples)),
            extraSamples, 216);
        targets.push_back({anomaly.player, first, phases[first].start,
                           phases[last].end, samples, true});
    };
    appendQueueRepairTarget(p1QueueAnomaly, p1Phases);
    appendQueueRepairTarget(p2QueueAnomaly, p2Phases);
    std::sort(targets.begin(), targets.end(), [](const Target& left, const Target& right) {
        if (left.start != right.start) return left.start < right.start;
        if (left.player != right.player) return left.player < right.player;
        return left.phase < right.phase;
    });

    VideoReader reader;
#ifdef TR_MF_BOARD_ADVANCED_PROCESSING_DEFAULT
    constexpr bool advancedBoardVideoProcessing = true;
#else
    constexpr bool advancedBoardVideoProcessing = false;
#endif
    // Broken trajectories need a working dense seek/decode path: Media
    // Foundation can report immediate EOF on downloaded streams (AB18).
    // Do not also enable previously unavailable commuting-pair corrections on
    // healthy trajectories: effect/garbage frames can reverse a correct pair
    // even though its final board is identical (AB46). Retain their established
    // reader until that independent temporal-scoring issue is resolved.
    // The MF-only review companion must remain free of NVDEC imports.
#if defined(TR_NVDEC_DEFAULT)
    const bool preferNvdec = (p1QueueAnomaly.detected || p2QueueAnomaly.detected) &&
        !std::getenv("TR_REFINEMENT_LEGACY_READER");
#else
    constexpr bool preferNvdec = false;
#endif
    if (!reader.open(input, error, advancedBoardVideoProcessing, false,
                     preferNvdec)) return false;
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
    reader.useBrowserMfConversion(!p1QueueAnomaly.detected && !p2QueueAnomaly.detected);
#endif

    std::unique_ptr<OnnxBoardModel> ownedP1Model;
    std::unique_ptr<OnnxBoardModel> ownedP2Model;
    OnnxBoardModel* p1Model = nullptr;
    OnnxBoardModel* p2Model = nullptr;
    const bool needsP1Model = std::any_of(
        targets.begin(), targets.end(), [](const Target& target) {
            return target.player == 1;
        });
    const bool needsP2Model = std::any_of(
        targets.begin(), targets.end(), [](const Target& target) {
            return target.player == 2;
        });
    if (needsP1Model) {
        if (reusableModels) p1Model = reusableModels->board(true, modelPath, error);
        else {
            ownedP1Model = std::make_unique<OnnxBoardModel>(modelPath, error);
            p1Model = ownedP1Model.get();
        }
        if (!p1Model || !p1Model->ready()) return false;
    }
    if (needsP2Model) {
        if (reusableModels) p2Model = reusableModels->board(false, modelPath, error);
        else {
            ownedP2Model = std::make_unique<OnnxBoardModel>(modelPath, error);
            p2Model = ownedP2Model.get();
        }
        if (!p2Model || !p2Model->ready()) return false;
    }
    std::unique_ptr<VisionAnalyzer> p1Vision;
    std::unique_ptr<VisionAnalyzer> p2Vision;
    if (p1Model) {
        p1Vision = std::make_unique<VisionAnalyzer>(
            reader.width(), reader.height(), settings.p1, p1Model);
    }
    if (p2Model) {
        p2Vision = std::make_unique<VisionAnalyzer>(
            reader.width(), reader.height(), settings.p2, p2Model);
    }

    std::vector<BoardObservation> extraP1;
    std::vector<BoardObservation> extraP2;
    std::vector<std::vector<BoardObservation>> extraByTarget(targets.size());
    const double sourceOffset = std::max(0.0, output.sourceStartSeconds);
    double refineSeekSeconds = 0;
    double refineDecodeSeconds = 0;
    double refineAnalyzeSeconds = 0;
    for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        if (status.cancel.load()) {
            error = "Recovery cancelled";
            return false;
        }
        const Target& target = targets[targetIndex];
        const auto requestTimes = boardSampleTimes(
            target.start, target.end, target.samples);
        if (requestTimes.empty()) continue;
        status.setMessage(
            "Adaptive order sampling " + std::to_string(targetIndex + 1) + "/" +
            std::to_string(targets.size()));
        const auto seekStarted = std::chrono::steady_clock::now();
        if (!reader.seek(target.start + sourceOffset, error)) return false;
        refineSeekSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - seekStarted).count();
        std::size_t requestIndex = 0;
        Frame frame;
        bool endOfStream = false;
        auto& destination = target.player == 1 ? extraP1 : extraP2;
        VisionAnalyzer& vision = target.player == 1 ? *p1Vision : *p2Vision;
        while (requestIndex < requestTimes.size() && !endOfStream) {
            const auto needPixels = [&](std::int64_t timestamp) {
                return requestTimes[requestIndex] + sourceOffset <=
                    static_cast<double>(timestamp) / 10000000.0 + .000001;
            };
            const auto decodeStarted = std::chrono::steady_clock::now();
            if (!reader.readFiltered(frame, endOfStream, error, needPixels)) return false;
            refineDecodeSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - decodeStarted).count();
            if (endOfStream || frame.bgra.empty()) continue;
            const double frameSeconds =
                static_cast<double>(frame.time100ns) / 10000000.0;
            const auto analyzeStarted = std::chrono::steady_clock::now();
            BoardObservation observation = vision.analyzeBoard(frame);
            refineAnalyzeSeconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - analyzeStarted).count();
            if (!observation.recognitionError.empty()) {
                error = observation.recognitionError;
                return false;
            }
            while (requestIndex < requestTimes.size() &&
                   requestTimes[requestIndex] + sourceOffset <=
                       frameSeconds + .000001) {
                observation.timeSeconds = requestTimes[requestIndex];
                destination.push_back(observation);
                extraByTarget[targetIndex].push_back(observation);
                ++requestIndex;
            }
        }
        if (requestIndex != requestTimes.size()) {
            error = "Adaptive order sampling reached end of video before its requests";
            return false;
        }
    }

#if defined(TR_BROWSER_PARITY_DIAGNOSTIC)
    writeBrowserParityBoards(extraP1, extraP2, ".extra.json");
#endif
    auto refinedBoardsP1 = output.boardObservationsP1;
    auto refinedBoardsP2 = output.boardObservationsP2;
    refinedBoardsP1.insert(refinedBoardsP1.end(), extraP1.begin(), extraP1.end());
    refinedBoardsP2.insert(refinedBoardsP2.end(), extraP2.begin(), extraP2.end());
    auto refinedRawP1 = buildRawTimelineFromFlatBoards(p1Phases, refinedBoardsP1);
    auto refinedRawP2 = buildRawTimelineFromFlatBoards(p2Phases, refinedBoardsP2);

    // v69: the rebuilt raw timeline re-derives every identity from NEXT, which
    // silently discarded a validated board-driven identity repair from the
    // first pass (r8NZ match 12 P1: the repair removed an opening phantom T
    // with severe 11->0, refinement restored it and the later repair window
    // could no longer reach it). When the phase layout is unchanged, keep the
    // first-pass identities and take only the denser board evidence.
    static const bool keepIdentity = [] {
        const char* v = std::getenv("TR_REFINE_KEEP_IDENTITY");
        return !(v && std::string(v) == "0");
    }();
    const auto keepRepairedIdentity = [](int player, std::vector<TimelineStep>& refined,
                                         const std::vector<TimelineStep>& previous) {
        if (refined.size() != previous.size()) return;
        for (std::size_t i = 0; i < refined.size(); ++i) {
            if (std::abs(refined[i].startSeconds - previous[i].startSeconds) > .000001 ||
                std::abs(refined[i].timeSeconds - previous[i].timeSeconds) > .000001) return;
        }
        std::size_t kept = 0;
        for (std::size_t i = 0; i < refined.size(); ++i) {
            if (refined[i].manuallyFixed || refined[i].queueManuallyFixed) continue;
            if (refined[i].piece == previous[i].piece &&
                refined[i].pieceIdentityFlexible == previous[i].pieceIdentityFlexible) continue;
            refined[i].piece = previous[i].piece;
            refined[i].uncertain = previous[i].uncertain;
            refined[i].pieceIdentityFlexible = previous[i].pieceIdentityFlexible;
            ++kept;
        }
        if (kept) std::cerr << "refine_kept_repaired_identity=" << kept << " player=" << player << '\n';
    };
    if (keepIdentity) {
        keepRepairedIdentity(1, refinedRawP1, output.rawP1);
        keepRepairedIdentity(2, refinedRawP2, output.rawP2);
    }

    // Aggregating several board frames intentionally suppresses the falling
    // mino, which is correct for settled-board recovery but throws away an
    // independent identity signal when NEXT/HOLD is wrong. During the
    // anomaly-only dense pass, vote from same-colour cells that appear on an
    // otherwise empty settled-board location. Effects rarely form the same
    // colour in multiple frames, while a controlled tetromino normally does.
    const auto applyActivePieceVotes = [&](int player,
                                           const std::vector<QueuePhase>& phases,
                                           std::vector<TimelineStep>& refinedRaw,
                                           const std::vector<TimelineStep>& baseline) {
        if (phases.size() != refinedRaw.size() ||
            baseline.size() != refinedRaw.size()) return std::size_t{0};
        std::vector<std::array<int, 7>> votes(phases.size());
        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            const Target& target = targets[targetIndex];
            if (target.player != player || !target.queueRepair) continue;
            for (const BoardObservation& observation : extraByTarget[targetIndex]) {
                const auto found = std::upper_bound(
                    phases.begin(), phases.end(), observation.timeSeconds,
                    [](double time, const QueuePhase& phase) {
                        return time < phase.start;
                    });
                if (found == phases.begin()) continue;
                const std::size_t phase = static_cast<std::size_t>(
                    std::prev(found) - phases.begin());
                if (phase == 0 || phase + 1 >= phases.size() ||
                    observation.timeSeconds >= phases[phase].end - .000001) {
                    continue;
                }
                std::array<int, 7> extra{};
                for (int y = VisibleRows; y < BoardHeight; ++y) {
                    for (int x = 0; x < BoardWidth; ++x) {
                        const int boardCell = index(x, y);
                        const Cell observed = observation.board[
                            static_cast<std::size_t>((y - VisibleRows) * BoardWidth + x)];
                        if (!isPiece(observed) ||
                            baseline[phase].board[boardCell] != Cell::Empty) continue;
                        const auto piece = std::find(Pieces.begin(), Pieces.end(), observed);
                        if (piece != Pieces.end()) {
                            ++extra[static_cast<std::size_t>(piece - Pieces.begin())];
                        }
                    }
                }
                int winner = -1;
                int winnerCells = 0;
                int runnerUpCells = 0;
                for (int piece = 0; piece < 7; ++piece) {
                    const int count = extra[static_cast<std::size_t>(piece)];
                    if (count > winnerCells) {
                        runnerUpCells = winnerCells;
                        winnerCells = count;
                        winner = piece;
                    } else {
                        runnerUpCells = std::max(runnerUpCells, count);
                    }
                }
                if (winner >= 0 && winnerCells >= 2 &&
                    winnerCells >= runnerUpCells + 1) {
                    votes[phase][static_cast<std::size_t>(winner)] +=
                        winnerCells >= 4 ? 2 : 1;
                }
            }
        }

        std::size_t changed = 0;
        for (std::size_t phase = 1; phase + 1 < phases.size(); ++phase) {
            int winner = -1;
            int winnerVotes = 0;
            int runnerUpVotes = 0;
            for (int piece = 0; piece < 7; ++piece) {
                const int count = votes[phase][static_cast<std::size_t>(piece)];
                if (count > winnerVotes) {
                    runnerUpVotes = winnerVotes;
                    winnerVotes = count;
                    winner = piece;
                } else {
                    runnerUpVotes = std::max(runnerUpVotes, count);
                }
            }
            if (winner < 0 || winnerVotes < 4 ||
                winnerVotes < runnerUpVotes + 2) continue;
            const Cell selected = Pieces[static_cast<std::size_t>(winner)];
            std::cerr << "active_piece_temporal_observation player=" << player
                      << " phase=" << phase
                      << " current=" << cellChar(refinedRaw[phase].piece)
                      << " visible=" << cellChar(selected)
                      << " votes=" << winnerVotes
                      << " runner_up=" << runnerUpVotes << '\n';
            // The visible falling mino is not necessarily the mino that locks:
            // HOLD may exchange it later in the same queue phase.  Preserve
            // this signal for diagnostics and pair-history scoring; never
            // rewrite an individual placement identity from it directly.
        }
        return changed;
    };
    const std::size_t activeVotesP1 = applyActivePieceVotes(
        1, p1Phases, refinedRawP1, output.p1);
    const std::size_t activeVotesP2 = applyActivePieceVotes(
        2, p2Phases, refinedRawP2, output.p2);
    if (activeVotesP1 + activeVotesP2 != 0) {
        output.beamCacheP1.reset();
        output.beamCacheP2.reset();
    }
    std::vector<TimelineStep> refinedP1;
    std::vector<TimelineStep> refinedP2;
    auto refinedCacheP1 = output.beamCacheP1;
    auto refinedCacheP2 = output.beamCacheP2;
    if (!solveTimelinesParallel(
            settings, refinedRawP1, refinedRawP2, refinedP1, refinedP2, &status,
            &refinedCacheP1, &refinedCacheP2, &error)) return false;
    const auto refinementUnresolved = [](const std::vector<TimelineStep>& timeline) {
        return std::any_of(timeline.begin(), timeline.end(),
                           [](const TimelineStep& step) {
                               return step.recoveryUnresolved;
                           });
    };
    if (refinementUnresolved(refinedP1) || refinementUnresolved(refinedP2)) {
        error = "Adaptive order refinement could not preserve a complete legal timeline";
        return false;
    }

    double minimumResolutionGain = 0.0;
    if (const char* configured = std::getenv("TR_FAST_ORDER_RESOLUTION_MIN_GAIN")) {
        try {
            minimumResolutionGain = std::clamp(std::stod(configured), 0.0, 1000.0);
        } catch (...) {
            minimumResolutionGain = 0.0;
        }
    }
    const std::size_t resolvedP1 = TetrisEngine::resolvePlacementOrderAmbiguities(
        refinedRawP1, refinedP1, detectionSettings, minimumResolutionGain);
    const std::size_t resolvedP2 = TetrisEngine::resolvePlacementOrderAmbiguities(
        refinedRawP2, refinedP2, detectionSettings, minimumResolutionGain);

    double temporalVoteRatio = .625;
    if (const char* configured = std::getenv("TR_FAST_ORDER_TEMPORAL_VOTE_RATIO")) {
        try {
            temporalVoteRatio = std::clamp(std::stod(configured), .5, 1.0);
        } catch (...) {
            temporalVoteRatio = .625;
        }
    }
    struct TemporalResolutionCounts {
        std::size_t changed = 0;
        std::size_t confirmed = 0;
    };
    const auto resolveTemporalOrders = [&](int player,
                                           const std::vector<TimelineStep>& refinedRaw,
                                           std::vector<TimelineStep>& refinedSolved) {
        struct TemporalVote {
            std::size_t count = 0;
            TimelineStep first;
            TimelineStep second;
        };
        TemporalResolutionCounts counts;
        Settings perFrameSettings = detectionSettings;
        perFrameSettings.orderAmbiguityScoreMargin = 1.0e9;
        for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
            const Target& target = targets[targetIndex];
            if (target.player != player || target.queueRepair || target.phase == 0 ||
                target.phase + 1 >= refinedSolved.size() ||
                extraByTarget[targetIndex].empty() ||
                refinedRaw[target.phase].manuallyFixed ||
                refinedRaw[target.phase + 1].manuallyFixed) {
                continue;
            }

            const std::size_t firstStep = target.phase;
            const std::size_t secondStep = firstStep + 1;
            const Board selectedIntermediate = refinedSolved[firstStep].board;
            std::vector<TimelineStep> sampleRaw = refinedRaw;
            for (TimelineStep& step : sampleRaw) step.manuallyFixed = true;
            sampleRaw[firstStep].manuallyFixed = false;
            sampleRaw[secondStep].manuallyFixed = false;

            std::size_t selectedVotes = 0;
            std::map<Board, TemporalVote> alternateVotes;
            for (const BoardObservation& sample : extraByTarget[targetIndex]) {
                assignBoardObservation(sampleRaw[firstStep], sample);
                std::vector<TimelineStep> trial = refinedSolved;
                const std::size_t trialChanges =
                    TetrisEngine::resolvePlacementOrderAmbiguities(
                        sampleRaw, trial, perFrameSettings, minimumResolutionGain);
                if (trialChanges == 0 || trial[firstStep].board == selectedIntermediate) {
                    ++selectedVotes;
                    continue;
                }
                TemporalVote& vote = alternateVotes[trial[firstStep].board];
                ++vote.count;
                vote.first = trial[firstStep];
                vote.second = trial[secondStep];
            }

            auto winner = std::max_element(
                alternateVotes.begin(), alternateVotes.end(),
                [](const auto& left, const auto& right) {
                    return left.second.count < right.second.count;
                });
            const std::size_t sampleCount = extraByTarget[targetIndex].size();
            const std::size_t requiredVotes = std::max<std::size_t>(
                3, static_cast<std::size_t>(std::ceil(sampleCount * temporalVoteRatio)));
            const std::size_t alternateCount = winner == alternateVotes.end()
                ? 0 : winner->second.count;
            std::cerr << "order_temporal_vote player=" << player
                      << " first=" << firstStep
                      << " samples=" << sampleCount
                      << " selected=" << selectedVotes
                      << " alternate=" << alternateCount
                      << " required=" << requiredVotes << '\n';
            if (selectedVotes >= requiredVotes && selectedVotes > alternateCount) {
                refinedSolved[firstStep].orderAutomaticallyResolved = true;
                ++counts.confirmed;
                continue;
            }
            if (winner == alternateVotes.end() || alternateCount < requiredVotes ||
                alternateCount <= selectedVotes) {
                continue;
            }

            // The per-frame trial supplies the physically compatible pair.
            // Keep the existing cumulative scores: the endpoint and every
            // later state are identical, while the per-frame score was only
            // used as a temporal order vote rather than as new global data.
            TimelineStep correctedFirst = winner->second.first;
            TimelineStep correctedSecond = winner->second.second;
            correctedFirst.score = refinedSolved[firstStep].score;
            correctedSecond.score = refinedSolved[secondStep].score;
            correctedFirst.orderAutomaticallyResolved = true;
            refinedSolved[firstStep] = std::move(correctedFirst);
            refinedSolved[secondStep] = std::move(correctedSecond);
            ++counts.changed;
            ++counts.confirmed;
        }
        return counts;
    };
    const TemporalResolutionCounts temporalP1 = resolveTemporalOrders(
        1, refinedRawP1, refinedP1);
    const TemporalResolutionCounts temporalP2 = resolveTemporalOrders(
        2, refinedRawP2, refinedP2);
    // Incremental caches describe the pre-resolution parent chain. The two
    // corrected operations reach the same endpoint, but retaining that arena
    // could resurrect the discarded order on a later continuation.
    if (resolvedP1 + temporalP1.changed != 0) refinedCacheP1.reset();
    if (resolvedP2 + temporalP2.changed != 0) refinedCacheP2.reset();

    const auto remainingP1 = TetrisEngine::placementOrderAmbiguities(
        refinedRawP1, refinedP1, detectionSettings);
    const auto remainingP2 = TetrisEngine::placementOrderAmbiguities(
        refinedRawP2, refinedP2, detectionSettings);
    std::cerr << "order_refinement_targets=" << targets.size()
              << " extra_samples=" << (extraP1.size() + extraP2.size())
              << " refine_seek_s=" << refineSeekSeconds
              << " refine_decode_s=" << refineDecodeSeconds
              << " refine_analyze_s=" << refineAnalyzeSeconds
              << " resolved=" << (resolvedP1 + resolvedP2)
              << " temporal_resolved="
              << (temporalP1.changed + temporalP2.changed)
              << " temporal_confirmed="
              << (temporalP1.confirmed + temporalP2.confirmed)
              << " remaining=" << (remainingP1.size() + remainingP2.size()) << '\n';

#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
    // WebCodecs can read dense frames where the established Windows reader
    // returns EOF and keeps the complete baseline. Those additional frames
    // include line-clear/rise effects: a commuting-pair score can prefer a
    // different clear timing without observing the actual clear event (AB46).
    // A healthy timeline must not acquire that unverified physical change.
    // Keep anomaly-driven repair and ordinary same-clear-count order fixes.
    const auto changesClearDuringRise = [](const auto& before, const auto& after) {
        if (before.size() != after.size()) return true;
        for (std::size_t i = 0; i < before.size(); ++i) {
            if (before[i].clearedLines != after[i].clearedLines &&
                (before[i].garbage.lines > 0 || after[i].garbage.lines > 0)) {
                return true;
            }
        }
        return false;
    };
    std::unique_ptr<RecoveryOutput> browserBaseline;
    if (!p1QueueAnomaly.detected && !p2QueueAnomaly.detected &&
        (changesClearDuringRise(output.p1, refinedP1) ||
         changesClearDuringRise(output.p2, refinedP2))) {
        // Judge the completed refinement, including its identity repair.
        // Rejecting its temporary pair here would discard valid new garbage
        // observations that the following wider-beam retry reconciles (#28).
        browserBaseline = std::make_unique<RecoveryOutput>(output);
    }
#endif
    output.boardObservationsP1 = std::move(refinedBoardsP1);
    output.boardObservationsP2 = std::move(refinedBoardsP2);
    output.rawP1 = std::move(refinedRawP1);
    output.rawP2 = std::move(refinedRawP2);
    output.p1 = std::move(refinedP1);
    output.p2 = std::move(refinedP2);
    output.beamCacheP1 = std::move(refinedCacheP1);
    output.beamCacheP2 = std::move(refinedCacheP2);
    // The dense queue-repair target supplies intermediate frames that the
    // original three-sample phase schedule can miss. Re-evaluate NEXT-derived
    // identities only now, after the extra evidence is part of the retained
    // board log.
    const bool identityChanged = repairSuspiciousQueueIdentity(settings, output);
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
    if (browserBaseline &&
        (changesClearDuringRise(browserBaseline->p1, output.p1) ||
         changesClearDuringRise(browserBaseline->p2, output.p2))) {
        output = std::move(*browserBaseline);
        output.beamCacheP1.reset(); output.beamCacheP2.reset();
        error = "Additional order samples changed line-clear timing during a garbage rise; complete baseline preserved";
        return false;
    }
#endif
    if (unchangedIdentityChecked) *unchangedIdentityChecked = !identityChanged;
    return true;
}

bool analyzeVideoImpl(const std::filesystem::path& input,
                      const std::filesystem::path& modelPath,
                      const Settings& settings, Status& status,
                      RecoveryOutput& output, std::string& error,
                      double analysisEndSeconds,
                      ReusableAnalysisModels* reusableModels,
                      bool deferRecovery,
                      double analysisStartSeconds = 0) {
    // A new analysis starts with no stale file paths/URLs.  The caller may
    // later revise `rawP*` / `p*` in the review editor before exporting.
    output = RecoveryOutput{};
    struct PlayerNameTaskResult {
        PlayerNameRecognition p1;
        PlayerNameRecognition p2;
        std::string warning;
    };
    std::future<PlayerNameTaskResult> playerNameTask;
    Frame reusedPlayerNameFrame;
    int reusedPlayerNameSourceWidth = 0;
    int reusedPlayerNameSourceHeight = 0;
    std::vector<QueuePhase> p1Phases;
    std::vector<QueuePhase> p2Phases;
    std::vector<QueueRecognitionSample> p1QueueSamples;
    std::vector<QueueRecognitionSample> p2QueueSamples;
    BoardRequestPrefetch boardPrefetch;
#ifndef __EMSCRIPTEN__
    const char* boardPrefetchValue = std::getenv("TR_BOARD_PREFETCH");
    const bool boardPrefetchEnabled = boardPrefetchValue && std::string(boardPrefetchValue) != "0";
#else
    constexpr bool boardPrefetchEnabled = false;
#endif
    double duration = 0;
    const auto queueModelPath = modelPath.parent_path() / L"next_hold.onnx";
    const double requestedEnd = analysisEndSeconds > 0 ? analysisEndSeconds : settings.initialAnalysisSeconds;
    const double analysisEnd = requestedEnd > 0 ? requestedEnd : -1;
    const double analysisStart = std::max(0.0, analysisStartSeconds);
    if (analysisEnd > 0 && analysisEnd <= analysisStart + .000001) {
        error = "The analysis range must end after it starts";
        return false;
    }
    // Range-level stage timing. Inert unless TR_MATCH_TIMING is set.
    const bool matchTiming = std::getenv("TR_MATCH_TIMING") != nullptr;
    const auto stageElapsed = [](std::chrono::steady_clock::time_point started) {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
    };
    auto stageStarted = std::chrono::steady_clock::now();
    double queuePassSeconds = 0;
    double prefetchBoardSeconds = 0;
    double boardPassSeconds = 0;
    if (!collectQueuePhases(input, settings, status, p1Phases, p2Phases,
                            p1QueueSamples, p2QueueSamples, duration, error, analysisEnd,
                            analysisStart, boardPrefetchEnabled ? &modelPath : nullptr,
                            boardPrefetchEnabled ? &boardPrefetch : nullptr,
                            &queueModelPath, reusableModels,
                            settings.playerNameOcr.enabled
                                ? &reusedPlayerNameFrame : nullptr,
                            &reusedPlayerNameSourceWidth,
                            &reusedPlayerNameSourceHeight)) return false;
    queuePassSeconds = stageElapsed(stageStarted);
    stageStarted = std::chrono::steady_clock::now();
#ifndef __EMSCRIPTEN__
    if (settings.playerNameOcr.enabled &&
        (settings.p1.name.w > 0 || settings.p2.name.w > 0) &&
        !reusedPlayerNameFrame.bgra.empty()) {
        // The queue pass has already decoded this exact source frame. Only a
        // tiny retained name-strip union crosses to this CPU OCR task; no
        // second demuxer/decoder competes with the board pipeline.
        playerNameTask = std::async(
            std::launch::async,
            [frame = std::move(reusedPlayerNameFrame),
             sourceWidth = reusedPlayerNameSourceWidth,
             sourceHeight = reusedPlayerNameSourceHeight,
             settings]() {
                PlayerNameTaskResult result;
                RecoveryOutput names;
                recognizePlayerNamesFromFrame(
                    frame, sourceWidth, sourceHeight, settings,
                    names, result.warning);
                result.p1 = std::move(names.p1PlayerName);
                result.p2 = std::move(names.p2PlayerName);
                return result;
            });
    }
#endif
    output.videoDurationSeconds = duration;
    const double effectiveAnalysisEnd = analysisEnd > 0 ? std::min(analysisEnd, duration) : duration;
    const double effectiveAnalysisStart = std::min(analysisStart, effectiveAnalysisEnd);
    output.analyzedUntilSeconds = effectiveAnalysisEnd;
    output.queueObservationsP1 = p1QueueSamples;
    output.queueObservationsP2 = p2QueueSamples;
    output.originalQueueObservationsP1 = p1QueueSamples;
    output.originalQueueObservationsP2 = p2QueueSamples;

    std::vector<std::vector<BoardObservation>> p1Samples;
    std::vector<std::vector<BoardObservation>> p2Samples;
    const bool reusedPrefetchedBoards = boardPrefetchEnabled &&
        collectPrefetchedBoardSamples(settings, p1Phases, p2Phases, boardPrefetch,
                                      status, p1Samples, p2Samples, effectiveAnalysisStart);
    prefetchBoardSeconds = stageElapsed(stageStarted);
    stageStarted = std::chrono::steady_clock::now();
    if (!reusedPrefetchedBoards &&
        !collectBoardSamples(input, modelPath, settings, p1Phases, p2Phases, status,
                             p1Samples, p2Samples, error, effectiveAnalysisStart,
                             reusableModels)) return false;
    boardPassSeconds = stageElapsed(stageStarted);
    if (matchTiming) {
        std::size_t requests = 0;
        for (const auto& phaseSamples : p1Samples) requests += phaseSamples.size();
        for (const auto& phaseSamples : p2Samples) requests += phaseSamples.size();
        std::cerr << "match_board_timing queue_pass_s=" << queuePassSeconds
                  << " prefetch_wait_s=" << prefetchBoardSeconds
                  << " reused=" << (reusedPrefetchedBoards ? 1 : 0)
                  << " board_pass_s=" << boardPassSeconds
                  << " board_requests=" << requests << '\n';
    }
    if (reusedPrefetchedBoards && effectiveAnalysisStart <= .000001 &&
        !replacePrefetchedOpeningBoardSamplesWithLegacyRgb(
            input, modelPath, settings, p1Phases, p2Phases,
            status, p1Samples, p2Samples, error, reusableModels)) return false;

    for (const auto& group : p1Samples) {
        output.boardObservationsP1.insert(output.boardObservationsP1.end(), group.begin(), group.end());
    }
    for (const auto& group : p2Samples) {
        output.boardObservationsP2.insert(output.boardObservationsP2.end(), group.begin(), group.end());
    }
#if defined(TR_BROWSER_PARITY_DIAGNOSTIC)
    writeBrowserParityBoards(output.boardObservationsP1, output.boardObservationsP2);
#endif

    // The fast hybrid continuation does not consume a solved prefix: it only
    // needs the exact queue/board observations and the resume timestamp.  The
    // continuation rebuilds and solves the complete timeline after appending
    // the suffix, so doing it here as well is provably duplicate work.  Keep
    // the public analysis path unchanged and enable this only for the scoped
    // experimental fast-reuse attempt.
    if (deferRecovery) {
        if (std::getenv("TR_GPU_DIAGNOSTICS")) {
            std::cerr << "fast_prefix_recovery_deferred=1\n";
        }
        status.setMessage("Opening observations anchored; deferring recovery to continuation");
        return true;
    }

    // Queue/board collection uses absolute source timestamps so seeking and
    // decoder delivery remain exact.  The reconstructed match itself starts
    // at zero; retain the source offset separately for review playback.
    if (effectiveAnalysisStart > 0) {
        const auto shiftQueue = [effectiveAnalysisStart](
                std::vector<QueueRecognitionSample>& samples) {
            for (auto& sample : samples) {
                sample.timeSeconds = std::max(
                    0.0, sample.timeSeconds - effectiveAnalysisStart);
            }
        };
        const auto shiftBoards = [effectiveAnalysisStart](
                std::vector<BoardObservation>& samples) {
            for (auto& sample : samples) {
                sample.timeSeconds = std::max(
                    0.0, sample.timeSeconds - effectiveAnalysisStart);
            }
        };
        shiftQueue(output.queueObservationsP1);
        shiftQueue(output.queueObservationsP2);
        shiftQueue(output.originalQueueObservationsP1);
        shiftQueue(output.originalQueueObservationsP2);
        shiftBoards(output.boardObservationsP1);
        shiftBoards(output.boardObservationsP2);
    }
    const double reconstructedDuration =
        effectiveAnalysisEnd - effectiveAnalysisStart;
    if (!(reconstructedDuration > 0)) {
        error = "The source contains no decodable time inside the analysis range";
        return false;
    }
    if (!prepareRecoveryObservations(reconstructedDuration, settings,
                                     output.queueObservationsP1, output.queueObservationsP2,
                                     output.boardObservationsP1, output.boardObservationsP2,
                                     output, error)) return false;
    // The review is intentionally limited to the prefix, but continuation and
    // export still need the source video's real duration.
    if (effectiveAnalysisStart > 0) {
        output.videoDurationSeconds = reconstructedDuration;
        output.analyzedUntilSeconds = reconstructedDuration;
    } else {
        // Preserve the existing partial-analysis/continuation contract for
        // ordinary videos that start at source time zero.
        output.videoDurationSeconds = duration;
        output.analyzedUntilSeconds = effectiveAnalysisEnd;
    }
    output.sourceStartSeconds = effectiveAnalysisStart;
    output.sourceEndSeconds = effectiveAnalysisEnd;
    output.sourceVideoDurationSeconds = duration;
#ifndef __EMSCRIPTEN__
    if (settings.playerNameOcr.enabled &&
        (settings.p1.name.w > 0 || settings.p2.name.w > 0) &&
        !playerNameTask.valid()) {
        // Compatibility fallback for non-NVDEC codecs/builds. Production
        // range analysis normally takes the reused-frame path above.
        playerNameTask = std::async(
            std::launch::async,
            [input, effectiveAnalysisStart, settings]() {
                PlayerNameTaskResult result;
                RecoveryOutput names;
                recognizePlayerNames(input, effectiveAnalysisStart, settings,
                                     names, result.warning);
                result.p1 = std::move(names.p1PlayerName);
                result.p2 = std::move(names.p2PlayerName);
                return result;
            });
    }
#endif
    status.setMessage("Pass 3/3: original-compatible legal-move beam search");
    status.progress.store(84);
    const bool solved = solveTimelinesParallel(
        settings, output.rawP1, output.rawP2, output.p1, output.p2, &status,
        &output.beamCacheP1, &output.beamCacheP2, &error);
#ifndef __EMSCRIPTEN__
    if (playerNameTask.valid()) {
        try {
            PlayerNameTaskResult names = playerNameTask.get();
            output.p1PlayerName = std::move(names.p1);
            output.p2PlayerName = std::move(names.p2);
            if (!names.warning.empty() && std::getenv("TR_GPU_DIAGNOSTICS")) {
                std::cerr << "player_name_ocr_warning=\"" << names.warning
                          << "\"\n";
            }
        } catch (const std::exception& exception) {
            if (std::getenv("TR_GPU_DIAGNOSTICS")) {
                std::cerr << "player_name_ocr_warning=\""
                          << exception.what() << "\"\n";
            }
        }
    }
#endif
    if (!solved) return false;
    int unresolvedPlayer = 0;
    std::size_t unresolvedPhase = 0;
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        repairSuspiciousTrajectory(settings, output);
    }
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        status.progress.store(100);
        status.setMessage(
            "Automatic recovery stopped at P" +
            std::to_string(unresolvedPlayer) + " phase " +
            std::to_string(unresolvedPhase) +
            "; correct the preceding placement in review");
        error.clear();
        return true;
    }
    status.progress.store(99);
    status.setMessage("Pass 3/3: beam search complete; preparing review");
    status.progress.store(100);
    status.setMessage("Analysis complete: review and correct legal candidates before export");
    return true;
}

bool analyzeVideo(const std::filesystem::path& input,
                  const std::filesystem::path& modelPath,
                  const Settings& settings, Status& status,
                  RecoveryOutput& output, std::string& error,
                  double analysisEndSeconds) {
    return analyzeVideoImpl(input, modelPath, settings, status, output, error,
                            analysisEndSeconds, nullptr, false);
}

struct VideoRangeAnalyzer::Impl {
    std::filesystem::path input;
    std::filesystem::path modelPath;
    Settings settings;
    ReusableAnalysisModels models;

    Impl(std::filesystem::path inputValue,
         std::filesystem::path modelValue,
         Settings settingsValue)
        : input(std::move(inputValue)), modelPath(std::move(modelValue)),
          settings(std::move(settingsValue)) {
        // Keep the already validated speed-priority profile used by the
        // production GPU executable. Environment overrides remain available
        // for controlled accuracy/speed benchmarks.
        settings.onnxSamples = 3;
        settings.beamWidth = 32;
        if (const char* configured = std::getenv("TR_FAST_ONNX_SAMPLES")) {
            settings.onnxSamples = std::clamp(std::atoi(configured), 3, 20);
        }
        if (const char* configured = std::getenv("TR_FAST_BEAM_WIDTH")) {
            settings.beamWidth = std::clamp(std::atoi(configured), 1, 2000);
        }
    }
};

VideoRangeAnalyzer::VideoRangeAnalyzer(std::filesystem::path input,
                                       std::filesystem::path modelPath,
                                       Settings settings)
    : impl_(std::make_unique<Impl>(std::move(input), std::move(modelPath),
                                   std::move(settings))) {}

VideoRangeAnalyzer::~VideoRangeAnalyzer() = default;
VideoRangeAnalyzer::VideoRangeAnalyzer(VideoRangeAnalyzer&&) noexcept = default;
VideoRangeAnalyzer& VideoRangeAnalyzer::operator=(VideoRangeAnalyzer&&) noexcept = default;

bool VideoRangeAnalyzer::analyze(double startSeconds, double endSeconds,
                                 Status& status, RecoveryOutput& output,
                                 std::string& error) {
    if (!impl_) {
        error = "The range analyzer is not initialized";
        return false;
    }
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds) ||
        startSeconds < 0 || endSeconds <= startSeconds + .000001) {
        error = "Invalid video analysis range";
        return false;
    }
    status.progress.store(0);
    status.done.store(false);
    status.success.store(false);
    status.cancel.store(false);
    error.clear();
    if (!analyzeVideoImpl(
            impl_->input, impl_->modelPath, impl_->settings, status,
            output, error, endSeconds, &impl_->models, false,
            startSeconds)) {
        status.done.store(true);
        return false;
    }

    int unresolvedPlayer = 0;
    std::size_t unresolvedPhase = 0;
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        status.progress.store(100);
        status.success.store(true);
        status.done.store(true);
        status.setMessage(
            "Automatic recovery stopped at P" +
            std::to_string(unresolvedPlayer) + " phase " +
            std::to_string(unresolvedPhase) +
            "; correction review data was preserved");
        error.clear();
        return true;
    }

    // The refinement pass reuses the same board sessions. It seeks into the
    // original source with sourceStartSeconds while retaining relative match
    // timestamps in every exported reconstruction.
    std::string refinementError;
    bool unchangedIdentityChecked = false;
#if defined(TR_BROWSER_PARITY_DIAGNOSTIC)
    if (const char* tracePath = std::getenv("TR_BROWSER_PARITY_EXPORT")) {
        std::string traceError;
        writeRecoveryAutosave(impl_->input,
            std::string(tracePath) + ".before-refinement.json", output, traceError);
    }
#endif
    if (!refinePlacementOrderObservations(
            impl_->input, impl_->modelPath, impl_->settings, status, output,
            refinementError, &impl_->models, &unchangedIdentityChecked)) {
        if (status.cancel.load()) {
            error = refinementError.empty()
                ? "Recovery cancelled" : std::move(refinementError);
            status.done.store(true);
            return false;
        }
        std::cerr << "range_order_refinement_fallback=1 error="
                  << refinementError << '\n';
    }
    // Refinement may replace a two-placement interval with the timeline that
    // existed before trajectory repair. Run mismatch/garbage backtracking only
    // after every placement-order mutation so a confirmed no-rise path cannot
    // be silently overwritten (match 130 regression).
    if (!output.trajectoryRepair.attempted) {
        // Refinement's final identity pass sees these same inputs. If it made
        // no changes, repeating it cannot improve the result. A changed pass,
        // an early exit, or a failed refinement still takes the ordinary path.
        repairSuspiciousTrajectory(impl_->settings, output,
            unchangedIdentityChecked);
    }
    status.progress.store(100);
    status.success.store(true);
    status.done.store(true);
    status.setMessage("Range analysis complete");
    return true;
}

bool VideoRangeAnalyzer::findQueueMoveStart(
        double goFirstSeconds, double searchEndSeconds,
        OpeningStartSearchResult& result, std::string& error) {
    result = OpeningStartSearchResult{};
    error.clear();
    if (!impl_) {
        error = "The range analyzer is not initialized";
        return false;
    }
    if (!std::isfinite(goFirstSeconds) || !std::isfinite(searchEndSeconds) ||
        goFirstSeconds < 0 || searchEndSeconds <= goFirstSeconds + .000001) {
        error = "Invalid queue-move search range";
        return false;
    }
    constexpr double MaximumSearchSeconds = 3.0;
    searchEndSeconds = std::min(searchEndSeconds, goFirstSeconds + MaximumSearchSeconds);
    const double slotThreshold = [] {
        if (const char* v = std::getenv("TR_QUEUE_MOVE_SLOT_DIFF")) {
            try { const double parsed = std::stod(v); if (parsed > 0 && parsed < 255) return parsed; }
            catch (...) {}
        }
        return 15.0;
    }();
    const int requiredSlots = [] {
        if (const char* v = std::getenv("TR_QUEUE_MOVE_MIN_SLOTS")) {
            try { const int parsed = std::stoi(v); if (parsed >= 1 && parsed <= 8) return parsed; }
            catch (...) {}
        }
        return 3;
    }();

    VideoReader reader;
    if (!reader.open(impl_->input, error, false, false, true)) return false;
    VisionAnalyzer p1Vision(reader.width(), reader.height(), impl_->settings.p1, nullptr);
    VisionAnalyzer p2Vision(reader.width(), reader.height(), impl_->settings.p2, nullptr);
    // Slots the GO graphic does not sweep over (validated on 156 matches).
    std::vector<PixelRegion> slots;
    const auto p1Slots = p1Vision.queuePixelRegions();
    const auto p2Slots = p2Vision.queuePixelRegions();
    if (impl_->settings.player1Enabled) {
        for (std::size_t i = 0; i < p1Slots.size() && i < 3; ++i) slots.push_back(p1Slots[i]);
    }
    if (impl_->settings.player2Enabled) {
        for (const auto& region : p2Slots) slots.push_back(region);
    }
    if (slots.empty()) return true;
#ifndef __EMSCRIPTEN__
    reader.setGpuConversionRegions(slots);
#endif
    if (!reader.seek(std::max(0.0, goFirstSeconds - .2), error)) return false;
    const std::int64_t firstTick = static_cast<std::int64_t>(std::llround(goFirstSeconds * 10000000.0)) - 5000;
    const std::int64_t lastTick = static_cast<std::int64_t>(std::llround(searchEndSeconds * 10000000.0));

    const auto slotPixels = [](const Frame& frame, const PixelRegion& region, std::vector<std::uint8_t>& out) {
        out.clear();
        for (int y = region.y; y < region.y + region.height; ++y) {
            const int fy = y - frame.sourceY;
            if (fy < 0 || fy >= frame.height) continue;
            for (int x = region.x; x < region.x + region.width; ++x) {
                const int fx = x - frame.sourceX;
                if (fx < 0 || fx >= frame.width) continue;
                const std::size_t offset = (static_cast<std::size_t>(fy) * frame.width + fx) * 4;
                if (offset + 2 >= frame.bgra.size()) continue;
                out.push_back(frame.bgra[offset]);
                out.push_back(frame.bgra[offset + 1]);
                out.push_back(frame.bgra[offset + 2]);
            }
        }
    };
    std::vector<std::vector<std::uint8_t>> reference;
    std::vector<std::uint8_t> current;
    Frame frame;
    bool endOfStream = false;
    while (!endOfStream) {
        if (!reader.read(frame, endOfStream, error)) return false;
        if (std::getenv("TR_SEEK_DEBUG") && result.decodedFrames < 5) {
            std::cerr << "seek_frame_debug frame_time=" << frame.time100ns / 10000000.0
                      << " bgra=" << frame.bgra.size()
                      << " eos=" << (endOfStream ? 1 : 0) << '\n' << std::flush;
        }
        if (endOfStream || frame.bgra.empty()) continue;
        ++result.decodedFrames;
        if (frame.time100ns < firstTick) continue;
        if (frame.time100ns > lastTick + 1) break;
        ++result.testedFrames;
        if (reference.empty()) {
            reference.resize(slots.size());
            for (std::size_t i = 0; i < slots.size(); ++i) slotPixels(frame, slots[i], reference[i]);
            continue;
        }
        int changed = 0;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            slotPixels(frame, slots[i], current);
            if (current.empty() || current.size() != reference[i].size()) continue;
            double sum = 0;
            for (std::size_t k = 0; k < current.size(); ++k) {
                sum += std::abs(static_cast<int>(current[k]) - static_cast<int>(reference[i][k]));
            }
            if (sum / static_cast<double>(current.size()) > slotThreshold) ++changed;
        }
        if (changed >= requiredSlots) {
            result.found = true;
            result.startSeconds = static_cast<double>(frame.time100ns) / 10000000.0;
            result.endSeconds = result.startSeconds;
            result.consecutiveFrames = changed;
            return true;
        }
    }
    return true;
}

bool VideoRangeAnalyzer::findEmptyOpeningStart(
        double searchStartSeconds, double searchEndSeconds,
        int requiredConsecutiveFrames, OpeningStartSearchResult& result,
        std::string& error) {
    result = OpeningStartSearchResult{};
    error.clear();
    if (!impl_) {
        error = "The range analyzer is not initialized";
        return false;
    }
    if (!std::isfinite(searchStartSeconds) ||
        !std::isfinite(searchEndSeconds) || searchStartSeconds < 0 ||
        searchEndSeconds <= searchStartSeconds + .000001) {
        error = "Invalid opening-start search range";
        return false;
    }
    requiredConsecutiveFrames = std::clamp(requiredConsecutiveFrames, 1, 30);

    // This is deliberately bounded even if a malformed detection supplies a
    // much wider interval. The known post-GO start search is at most two
    // seconds; decoding a whole match here would defeat the fallback's
    // speed-only-on-error contract.
    constexpr double MaximumSearchSeconds = 3.0;
    searchStartSeconds = std::max(
        searchStartSeconds, searchEndSeconds - MaximumSearchSeconds);

    VideoReader reader;
    // NVDEC is reliable for late seeks in long H.264/AV1 streams and avoids
    // the Media Foundation end-of-stream failure that originally affected
    // long batch runs.
    if (!reader.open(impl_->input, error, false, false, true)) return false;

    OnnxBoardModel* p1Model = nullptr;
    OnnxBoardModel* p2Model = nullptr;
    if (impl_->settings.player1Enabled) {
        p1Model = impl_->models.board(true, impl_->modelPath, error);
        if (!p1Model) return false;
    }
    if (impl_->settings.player2Enabled) {
        p2Model = impl_->models.board(false, impl_->modelPath, error);
        if (!p2Model) return false;
    }
    VisionAnalyzer p1Vision(
        reader.width(), reader.height(), impl_->settings.p1, p1Model);
    VisionAnalyzer p2Vision(
        reader.width(), reader.height(), impl_->settings.p2, p2Model);

#ifndef __EMSCRIPTEN__
    // Only pixels consumed by board/HOLD recognition are transferred. NEXT
    // rectangles are included by queuePixelRegion(), whose documented union
    // also contains every legacy HOLD sample used below.
    std::vector<PixelRegion> regions;
    const auto appendRegions = [&regions](VisionAnalyzer& vision) {
        regions.push_back(vision.boardPixelRegion());
        regions.push_back(vision.queuePixelRegion());
    };
    if (impl_->settings.player1Enabled) appendRegions(p1Vision);
    if (impl_->settings.player2Enabled) appendRegions(p2Vision);
    reader.setGpuConversionRegions(regions);
#endif

    if (!reader.seek(searchStartSeconds, error)) return false;
    const std::int64_t firstTick = static_cast<std::int64_t>(
        std::llround(searchStartSeconds * 10000000.0));
    const std::int64_t lastTick = static_cast<std::int64_t>(
        std::llround(searchEndSeconds * 10000000.0));
    Frame frame;
    bool endOfStream = false;
    int runFrames = 0;
    double runStart = 0;
    double runEnd = 0;
    const auto boardEmpty = [](const VisibleBoard& board) {
        return std::none_of(board.begin(), board.end(), [](Cell cell) {
            return cell != Cell::Empty;
        });
    };
    const auto acceptRun = [&] {
        if (runFrames < requiredConsecutiveFrames) return;
        result.found = true;
        result.startSeconds = runStart;
        result.endSeconds = runEnd;
        result.consecutiveFrames = runFrames;
    };

    while (!endOfStream) {
        if (!reader.read(frame, endOfStream, error)) return false;
        if (std::getenv("TR_SEEK_DEBUG") && result.decodedFrames < 5) {
            std::cerr << "seek_frame_debug frame_time=" << frame.time100ns / 10000000.0
                      << " bgra=" << frame.bgra.size()
                      << " eos=" << (endOfStream ? 1 : 0) << '\n' << std::flush;
        }
        if (endOfStream || frame.bgra.empty()) continue;
        ++result.decodedFrames;
        if (frame.time100ns < firstTick) continue;
        if (frame.time100ns > lastTick + 1) break;
        ++result.testedFrames;

        bool empty = true;
        if (impl_->settings.player1Enabled) {
            BoardObservation observation = p1Vision.analyze(frame);
            if (!observation.recognitionError.empty()) {
                error = observation.recognitionError;
                return false;
            }
            empty = boardEmpty(observation.board) &&
                    observation.queue.hold == Cell::Empty;
        }
        if (empty && impl_->settings.player2Enabled) {
            BoardObservation observation = p2Vision.analyze(frame);
            if (!observation.recognitionError.empty()) {
                error = observation.recognitionError;
                return false;
            }
            empty = boardEmpty(observation.board) &&
                    observation.queue.hold == Cell::Empty;
        }

        const double seconds = static_cast<double>(frame.time100ns) / 10000000.0;
        if (empty) {
            if (!runFrames) runStart = seconds;
            runEnd = seconds;
            ++runFrames;
        } else {
            // Retain the latest complete run. Walking backward from the old
            // start would encounter the end of exactly this same run first;
            // returning its beginning preserves the full clean opening.
            acceptRun();
            runFrames = 0;
        }
    }
    acceptRun();
    return true;
}

std::uint64_t VideoRangeAnalyzer::createdModelSessions() const {
    return impl_ ? impl_->models.createdSessions : 0;
}

std::uint64_t VideoRangeAnalyzer::reusedModelSessions() const {
    return impl_ ? impl_->models.reusedSessions : 0;
}

double VideoRangeAnalyzer::modelInitializationSeconds() const {
    return impl_ ? impl_->models.initializationSeconds : 0;
}

bool continueVideoAnalysisImpl(const std::filesystem::path& input,
                               const std::filesystem::path& modelPath,
                               const Settings& settings, Status& status,
                               RecoveryOutput& output, std::string& error,
                               double resumeFromSeconds,
                               double analysisEndSeconds,
                               ReusableAnalysisModels* reusableModels);

bool analyzeVideoFastFinal(const std::filesystem::path& input,
                           const std::filesystem::path& modelPath,
                           const Settings& settings, Status& status,
                           RecoveryOutput& output, std::string& error,
                           double analysisEndSeconds,
                           double legacyPrefixSeconds) {
#ifdef __EMSCRIPTEN__
    // D3D11 and process environment switches do not exist in the browser.
    return analyzeVideo(input, modelPath, settings, status, output, error,
                        analysisEndSeconds);
#else
    class ScopedEnvironmentValue {
    public:
        ScopedEnvironmentValue(const char* name, const char* value) : name_(name) {
            if (const char* previous = std::getenv(name)) {
                hadPrevious_ = true;
                previous_ = previous;
            }
            set(value);
        }

        ~ScopedEnvironmentValue() {
            _putenv_s(name_.c_str(), hadPrevious_ ? previous_.c_str() : "");
        }

        void set(const char* value) {
            _putenv_s(name_.c_str(), value);
        }

        ScopedEnvironmentValue(const ScopedEnvironmentValue&) = delete;
        ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue&) = delete;

    private:
        std::string name_;
        std::string previous_;
        bool hadPrevious_ = false;
    };

    // 1.mov showed that the D3D11 reader's monotonic timestamps and color
    // conversion cannot establish the opening queue reliably. Preserve the
    // accepted reader for a short prefix, then switch to the much faster
    // D3D11 path after the queue/beam state is anchored. Board prefetch is
    // deliberately disabled: it repeats work and made the full run slower.
    ScopedEnvironmentValue gpuVideo("TR_GPU_VIDEO", "1");
    ScopedEnvironmentValue initialBoardPrefetch("TR_BOARD_PREFETCH", "0");
    const bool gpuOpeningEnabled = [] {
        const char* configured = std::getenv("TR_GPU_FRONT_OPENING");
        return configured && std::string(configured) != "0";
    }();
    bool gpuOpeningBoardPrefetchEnabled = false;
#ifdef TR_GPU_ULTRA_DEFAULT
    const bool ultraCpuBoardStreamEnabled = [] {
        const char* configured = std::getenv("TR_ULTRA_BOARD_STREAM");
        // The fixed ring is available for controlled A/B tests, but is not a
        // speed default until it beats the separate board pass end-to-end.
        return configured && std::string(configured) != "0";
    }();
    const bool ultraGpuBoardStreamEnabled = [] {
        const char* configured = std::getenv("TR_ULTRA_GPU_BOARD_STREAM");
#ifdef TR_GPU_MAX_DEFAULT
        return !configured || std::string(configured) != "0";
#else
        return configured && std::string(configured) != "0";
#endif
    }();
    const bool ultraBoardStreamEnabled =
        ultraCpuBoardStreamEnabled || ultraGpuBoardStreamEnabled;
    gpuOpeningBoardPrefetchEnabled = ultraBoardStreamEnabled;
    const bool ultraSparseReadbackEnabled = [] {
        const char* configured = std::getenv("TR_ULTRA_SPARSE_READBACK");
        return !configured || std::string(configured) != "0";
    }();
    ScopedEnvironmentValue continuationBoardPrefetch(
        "TR_CONTINUATION_BOARD_PREFETCH", ultraBoardStreamEnabled ? "1" : "0");
    std::string boundedBoardMegabytesValue =
        ultraGpuBoardStreamEnabled
#ifdef TR_GPU_FRONT_ANCHOR_DEFAULT
            ? "512"
#else
            ? "384"
#endif
            : "320";
    if (const char* configured = std::getenv("TR_BOARD_PREFETCH_MB")) {
        if (*configured != '\0') boundedBoardMegabytesValue = configured;
    }
    ScopedEnvironmentValue boundedBoardMegabytes(
        "TR_BOARD_PREFETCH_MB", boundedBoardMegabytesValue.c_str());
    ScopedEnvironmentValue sparseGpuReadback(
        "TR_QUEUE_SPARSE_ROI", ultraSparseReadbackEnabled ? "1" : "0");
#else
    ScopedEnvironmentValue continuationBoardPrefetch(
        "TR_CONTINUATION_BOARD_PREFETCH", "0");
#endif

    // These speed-priority settings were validated against the accepted
    // 1.mov final-state output. Keep them local so the exact executable and
    // the user's configuration file are not rewritten.
    Settings fastSettings = settings;
    fastSettings.onnxSamples = 3;
    fastSettings.beamWidth = 32;
    // Optional diagnostics for separately named experiment binaries.  The
    // production fast profile remains 3 samples / beam 32 when these values
    // are absent.  This lets full-video A/B runs isolate whether a failed
    // trajectory was caused by early beam pruning before changing any beam
    // semantics.
    if (const char* configured = std::getenv("TR_FAST_ONNX_SAMPLES")) {
        fastSettings.onnxSamples = std::clamp(std::atoi(configured), 3, 20);
    }
    if (const char* configured = std::getenv("TR_FAST_BEAM_WIDTH")) {
        fastSettings.beamWidth = std::clamp(std::atoi(configured), 1, 2000);
    }

    double requestedLegacyPrefix = legacyPrefixSeconds;
#ifdef TR_GPU_FRONT_ANCHOR_DEFAULT
    requestedLegacyPrefix = 0.1;
#endif
    if (const char* configured = std::getenv("TR_FAST_LEGACY_PREFIX_SECONDS")) {
        try {
            requestedLegacyPrefix = std::stod(configured);
        } catch (...) {
            requestedLegacyPrefix = legacyPrefixSeconds;
        }
    }
    const double safePrefix = std::max(0.1, requestedLegacyPrefix);
    const bool reuseInitializationEnabled = [] {
        const char* configured = std::getenv("TR_FAST_REUSE_INITIALIZATION");
        return configured && std::string(configured) != "0";
    }();
    const auto enabledWithDefault = [](const char* name, bool fallback) {
        const char* configured = std::getenv(name);
        return configured ? std::string(configured) != "0" : fallback;
    };
    const bool reuseModelSessionsEnabled = enabledWithDefault(
        "TR_FAST_REUSE_MODEL_SESSIONS", reuseInitializationEnabled);
    const bool deferPrefixRecoveryEnabled = enabledWithDefault(
        "TR_FAST_DEFER_PREFIX_RECOVERY", reuseInitializationEnabled);
    if (reuseInitializationEnabled) {
        std::cerr << "fast_reuse_initialization=1\n";
    }
    if (reuseModelSessionsEnabled) {
        std::cerr << "fast_reuse_model_sessions=1\n";
    }
    if (deferPrefixRecoveryEnabled) {
        std::cerr << "fast_defer_prefix_recovery=1\n";
    }
    const auto runFastAttempt = [&](const Settings& attemptSettings,
                                     RecoveryOutput& attemptOutput,
                                     bool useReusableModelSessions) {
        ReusableAnalysisModels reusableModels;
        ReusableAnalysisModels* modelCache = useReusableModelSessions
            ? &reusableModels : nullptr;
        const auto reportReuse = [&] {
            if (!useReusableModelSessions) return;
            std::cerr << "fast_model_sessions_created=" << reusableModels.createdSessions
                      << " fast_model_sessions_reused=" << reusableModels.reusedSessions
                      << " fast_model_initialization_s="
                      << reusableModels.initializationSeconds << '\n';
        };
        const auto refineOrder = [&](bool succeeded) {
            if (!succeeded) return false;
            int unresolvedPlayer = 0;
            std::size_t unresolvedPhase = 0;
            if (firstRecoveryUnresolved(
                    attemptOutput, unresolvedPlayer, unresolvedPhase)) {
                return true;
            }
            std::string refinementError;
            if (refinePlacementOrderObservations(
                    input, modelPath, attemptSettings, status, attemptOutput,
                    refinementError, modelCache)) {
                return true;
            }
            if (status.cancel.load()) {
                error = refinementError.empty() ? "Recovery cancelled" : refinementError;
                return false;
            }
            // The adaptive pass is an accuracy enhancement. A decoder seek
            // failure must not discard the already-complete base analysis.
            std::cerr << "order_refinement_fallback=1 error="
                      << refinementError << '\n';
            return true;
        };
        const double configuredEnd = analysisEndSeconds > 0
            ? analysisEndSeconds : attemptSettings.initialAnalysisSeconds;
        gpuVideo.set("1");
        initialBoardPrefetch.set("0");
        if (gpuOpeningEnabled) {
            // Experimental separate build: run the delivery-order-preserving
            // D3D11/GPU stream from the first frame.  The prefetched phase-0
            // board is still replaced by a tiny legacy RGB32 re-read inside
            // analyzeVideo().
            gpuVideo.set("2");
            initialBoardPrefetch.set(gpuOpeningBoardPrefetchEnabled ? "1" : "0");
            status.setMessage("GPU front analysis: decoding from the first frame");
            bool succeeded = analyzeVideoImpl(
                input, modelPath, attemptSettings, status, attemptOutput, error,
                configuredEnd, modelCache, false);
            succeeded = refineOrder(succeeded);
            reportReuse();
            return succeeded;
        }

        const double firstEnd = configuredEnd > 0
            ? std::min(safePrefix, configuredEnd) : safePrefix;
        const bool deferPrefixRecovery = deferPrefixRecoveryEnabled &&
            configuredEnd > firstEnd + .000001;
        if (!analyzeVideoImpl(input, modelPath, attemptSettings, status,
                              attemptOutput, error, firstEnd, modelCache,
                              deferPrefixRecovery)) {
            reportReuse();
            return false;
        }

        const double targetEnd = configuredEnd > 0
            ? std::min(configuredEnd, attemptOutput.videoDurationSeconds)
            : attemptOutput.videoDurationSeconds;
        if (targetEnd <= attemptOutput.analyzedUntilSeconds + .000001) {
            // Only possible for an unusually short source whose duration is
            // below the safe prefix. Complete the deferred work locally so
            // this edge case remains identical to ordinary analyzeVideo().
            if (deferPrefixRecovery) {
                const double duration = attemptOutput.videoDurationSeconds;
                const double analyzedUntil = attemptOutput.analyzedUntilSeconds;
                if (!prepareRecoveryObservations(
                        analyzedUntil, attemptSettings,
                        attemptOutput.queueObservationsP1,
                        attemptOutput.queueObservationsP2,
                        attemptOutput.boardObservationsP1,
                        attemptOutput.boardObservationsP2,
                        attemptOutput, error)) {
                    reportReuse();
                    return false;
                }
                attemptOutput.videoDurationSeconds = duration;
                attemptOutput.analyzedUntilSeconds = analyzedUntil;
                if (!solveTimelinesParallel(
                        attemptSettings, attemptOutput.rawP1, attemptOutput.rawP2,
                        attemptOutput.p1, attemptOutput.p2, &status,
                        &attemptOutput.beamCacheP1, &attemptOutput.beamCacheP2,
                        &error)) {
                    reportReuse();
                    return false;
                }
            }
            const bool succeeded = refineOrder(true);
            reportReuse();
            return succeeded;
        }

        gpuVideo.set("2");
        status.setMessage("Fast final-state analysis: switching remaining video to D3D11");
        bool succeeded = continueVideoAnalysisImpl(
            input, modelPath, attemptSettings, status, attemptOutput, error,
            attemptOutput.analyzedUntilSeconds, targetEnd, modelCache);
        succeeded = refineOrder(succeeded);
        reportReuse();
        return succeeded;
    };

    const auto runFastAttemptWithFallback = [&](const Settings& attemptSettings,
                                                 RecoveryOutput& attemptOutput) {
        if (runFastAttempt(attemptSettings, attemptOutput,
                           reuseModelSessionsEnabled)) {
            return true;
        }
        if (!reuseModelSessionsEnabled || status.cancel.load()) return false;

        // Reusing an ORT/CUDA session is an optimization, not a correctness
        // dependency. If provider state becomes invalid between the anchored
        // prefix and continuation, repeat once with the historical fresh-
        // session behavior and a clean output object.
        std::cerr << "fast_model_session_fallback=1\n";
        RecoveryOutput freshOutput;
        error.clear();
        if (!runFastAttempt(attemptSettings, freshOutput, false)) return false;
        attemptOutput = std::move(freshOutput);
        return true;
    };

    if (!runFastAttemptWithFallback(fastSettings, output)) return false;

    int unresolvedPlayer = 0;
    std::size_t unresolvedPhase = 0;
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        repairSuspiciousTrajectory(fastSettings, output);
    }
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        status.progress.store(100);
        status.setMessage(
            "Automatic recovery stopped at P" +
            std::to_string(unresolvedPlayer) + " phase " +
            std::to_string(unresolvedPhase) +
            "; correction review data was preserved");
        error.clear();
        return true;
    }

    if (!output.trajectoryRepair.attempted) {
        repairSuspiciousTrajectory(fastSettings, output);
    }
    if (output.trajectoryRepair.adopted &&
        output.trajectoryRepair.finalSeverePhases == 0) {
        return true;
    }

    bool trajectoryGuardEnabled = false;
#ifdef TR_GPU_SAFE_SAMPLING_DEFAULT
    trajectoryGuardEnabled = true;
#endif
    if (const char* configured = std::getenv("TR_FAST_TRAJECTORY_GUARD")) {
        trajectoryGuardEnabled = std::string(configured) != "0";
    }
    if (!trajectoryGuardEnabled) return true;

    const TrajectoryAnomaly anomaly = detectTrajectoryAnomaly(output.p1, output.p2);
    if (!anomaly.detected) return true;

    int retrySamples = 7;
    if (const char* configured = std::getenv("TR_FAST_RETRY_ONNX_SAMPLES")) {
        retrySamples = std::clamp(std::atoi(configured), 5, 20);
    }
    Settings retrySettings = fastSettings;
    retrySettings.onnxSamples = std::max(retrySettings.onnxSamples, retrySamples);
    std::cerr << "trajectory_guard_retry=1 player=" << anomaly.player
              << " phase=" << anomaly.phase
              << " time=" << anomaly.timeSeconds
              << " match=" << anomaly.matchRatio
              << " retry_samples=" << retrySettings.onnxSamples << '\n';
    status.setMessage("Trajectory anomaly detected: retrying with stronger board sampling");

    // The validated retry uses the historical section boundaries with seven
    // samples.  This is intentionally independent of the three-sample centred
    // schedule that triggered the guard.
    ScopedEnvironmentValue retrySampleOffset("TR_BOARD_SAMPLE_SECTION_OFFSET", "0");
    RecoveryOutput retryOutput;
    error.clear();
    if (!runFastAttemptWithFallback(retrySettings, retryOutput)) return false;
    const TrajectoryAnomaly retryAnomaly =
        detectTrajectoryAnomaly(retryOutput.p1, retryOutput.p2);
    std::cerr << "trajectory_guard_retry_anomaly="
              << (retryAnomaly.detected ? 1 : 0);
    if (retryAnomaly.detected) {
        std::cerr << " player=" << retryAnomaly.player
                  << " phase=" << retryAnomaly.phase
                  << " time=" << retryAnomaly.timeSeconds
                  << " match=" << retryAnomaly.matchRatio;
    }
    std::cerr << '\n';
    output = std::move(retryOutput);
    return true;
#endif
}

bool continueVideoAnalysisImpl(const std::filesystem::path& input,
                               const std::filesystem::path& modelPath,
                               const Settings& settings, Status& status,
                               RecoveryOutput& output, std::string& error,
                               double resumeFromSeconds,
                               double analysisEndSeconds,
                               ReusableAnalysisModels* reusableModels) {
    if (output.videoDurationSeconds <= 0 || output.analyzedUntilSeconds <= 0) {
        error = "There is no paused native analysis to continue";
        return false;
    }
    const double startSeconds = std::clamp(
        resumeFromSeconds > 0 ? resumeFromSeconds : output.analyzedUntilSeconds,
        0.0, output.videoDurationSeconds);
    const double requestedEnd = analysisEndSeconds > 0
        ? analysisEndSeconds : output.videoDurationSeconds;
    const double endSeconds = std::clamp(requestedEnd, startSeconds, output.videoDurationSeconds);
    if (endSeconds <= startSeconds + .000001) {
        error = "The continuation range must extend beyond its starting point";
        return false;
    }

    double duration = output.videoDurationSeconds;
    const auto queueModelPath = modelPath.parent_path() / L"next_hold.onnx";
    std::vector<QueuePhase> p1Phases;
    std::vector<QueuePhase> p2Phases;
    const auto keepBefore = [startSeconds](const QueueRecognitionSample& sample) {
        return sample.timeSeconds < startSeconds - .000001;
    };
    auto p1QueueSamples = output.queueObservationsP1;
    auto p2QueueSamples = output.queueObservationsP2;
    BoardRequestPrefetch boardPrefetch;
#ifndef __EMSCRIPTEN__
    bool continuationBoardPrefetch = true;
    if (const char* configured = std::getenv("TR_CONTINUATION_BOARD_PREFETCH")) {
        continuationBoardPrefetch = std::string(configured) != "0";
    }
#else
    constexpr bool continuationBoardPrefetch = false;
#endif
    p1QueueSamples.erase(std::remove_if(p1QueueSamples.begin(), p1QueueSamples.end(),
                                        [&](const QueueRecognitionSample& sample) { return !keepBefore(sample); }),
                         p1QueueSamples.end());
    p2QueueSamples.erase(std::remove_if(p2QueueSamples.begin(), p2QueueSamples.end(),
                                        [&](const QueueRecognitionSample& sample) { return !keepBefore(sample); }),
                         p2QueueSamples.end());
    if (!collectQueuePhases(input, settings, status, p1Phases, p2Phases,
                            p1QueueSamples, p2QueueSamples, duration, error,
                            endSeconds, startSeconds,
                            continuationBoardPrefetch ? &modelPath : nullptr,
                            continuationBoardPrefetch ? &boardPrefetch : nullptr,
                            &queueModelPath, reusableModels)) return false;

    std::vector<std::vector<BoardObservation>> p1Samples;
    std::vector<std::vector<BoardObservation>> p2Samples;
    auto p1Boards = output.boardObservationsP1;
    auto p2Boards = output.boardObservationsP2;
    const auto keepBoardBefore = [startSeconds](const BoardObservation& sample) {
        return sample.timeSeconds < startSeconds - .000001;
    };
    p1Boards.erase(std::remove_if(p1Boards.begin(), p1Boards.end(),
                                  [&](const BoardObservation& sample) { return !keepBoardBefore(sample); }),
                   p1Boards.end());
    p2Boards.erase(std::remove_if(p2Boards.begin(), p2Boards.end(),
                                  [&](const BoardObservation& sample) { return !keepBoardBefore(sample); }),
                   p2Boards.end());
    const bool usedPrefetchedBoards = continuationBoardPrefetch &&
        collectPrefetchedBoardSamples(settings, p1Phases, p2Phases, boardPrefetch,
                                      status, p1Samples, p2Samples, startSeconds);
    if (!usedPrefetchedBoards &&
        !collectBoardSamples(input, modelPath, settings, p1Phases, p2Phases, status,
                             p1Samples, p2Samples, error, startSeconds,
                             reusableModels)) return false;
    for (const auto& group : p1Samples) {
        p1Boards.insert(p1Boards.end(), group.begin(), group.end());
    }
    for (const auto& group : p2Samples) {
        p2Boards.insert(p2Boards.end(), group.begin(), group.end());
    }
    output.queueObservationsP1 = std::move(p1QueueSamples);
    output.queueObservationsP2 = std::move(p2QueueSamples);
    if (!prepareRecoveryObservations(duration, settings,
                                     output.queueObservationsP1, output.queueObservationsP2,
                                     std::move(p1Boards), std::move(p2Boards),
                                     output, error, true, startSeconds)) return false;
    output.videoDurationSeconds = duration;
    output.analyzedUntilSeconds = endSeconds;
    status.setMessage("Pass 3/3: beam search after native continuation");
    status.progress.store(84);
    if (!solveTimelinesParallel(
            settings, output.rawP1, output.rawP2, output.p1, output.p2, &status,
            &output.beamCacheP1, &output.beamCacheP2, &error)) return false;
    repairSuspiciousQueueIdentity(settings, output);
    int unresolvedPlayer = 0;
    std::size_t unresolvedPhase = 0;
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        status.progress.store(100);
        status.setMessage(
            "Automatic recovery stopped at P" +
            std::to_string(unresolvedPlayer) + " phase " +
            std::to_string(unresolvedPhase) +
            "; correct the preceding placement in review");
        error.clear();
        return true;
    }
    status.progress.store(100);
    status.setMessage("Analysis complete: review and correct legal candidates before export");
    return true;
}

bool continueVideoAnalysis(const std::filesystem::path& input,
                           const std::filesystem::path& modelPath,
                           const Settings& settings, Status& status,
                           RecoveryOutput& output, std::string& error,
                           double resumeFromSeconds,
                           double analysisEndSeconds) {
    return continueVideoAnalysisImpl(
        input, modelPath, settings, status, output, error,
        resumeFromSeconds, analysisEndSeconds, nullptr);
}

bool prepareRecoveryObservations(double videoDurationSeconds, const Settings& settings,
                                 std::vector<QueueRecognitionSample> p1Queue,
                                 std::vector<QueueRecognitionSample> p2Queue,
                                 std::vector<BoardObservation> p1Boards,
                                 std::vector<BoardObservation> p2Boards,
                                 RecoveryOutput& output, std::string& error,
                                 bool preserveManualCorrections,
                                 double preserveManualUntilSeconds) {
    if (!(videoDurationSeconds > 0)) {
        error = "Browser observation input has no positive video duration";
        return false;
    }
    const RecoveryOutput previous = preserveManualCorrections ? output : RecoveryOutput{};
    output = RecoveryOutput{};
    if (preserveManualCorrections) {
        output.beamCacheP1 = previous.beamCacheP1;
        output.beamCacheP2 = previous.beamCacheP2;
    }
    output.videoDurationSeconds = videoDurationSeconds;
    output.analyzedUntilSeconds = videoDurationSeconds;
    output.queueEditRevision = preserveManualCorrections ? previous.queueEditRevision : 0;
    output.queueObservationsP1 = std::move(p1Queue);
    output.queueObservationsP2 = std::move(p2Queue);
    if (preserveManualCorrections) {
        output.originalQueueObservationsP1 = output.queueObservationsP1;
        output.originalQueueObservationsP2 = output.queueObservationsP2;
        for (std::size_t i = 0; i < previous.originalQueueObservationsP1.size() &&
             i < output.originalQueueObservationsP1.size(); ++i) {
            if (preserveManualUntilSeconds >= 0 &&
                previous.originalQueueObservationsP1[i].timeSeconds > preserveManualUntilSeconds + .000001) continue;
            output.originalQueueObservationsP1[i] = previous.originalQueueObservationsP1[i];
        }
        for (std::size_t i = 0; i < previous.originalQueueObservationsP2.size() &&
             i < output.originalQueueObservationsP2.size(); ++i) {
            if (preserveManualUntilSeconds >= 0 &&
                previous.originalQueueObservationsP2[i].timeSeconds > preserveManualUntilSeconds + .000001) continue;
            output.originalQueueObservationsP2[i] = previous.originalQueueObservationsP2[i];
        }
    } else {
        output.originalQueueObservationsP1 = output.queueObservationsP1;
        output.originalQueueObservationsP2 = output.queueObservationsP2;
    }
    output.boardObservationsP1 = std::move(p1Boards);
    output.boardObservationsP2 = std::move(p2Boards);

    const auto p1Phases = settings.player1Enabled
        ? buildQueuePhasesFromSamples(output.queueObservationsP1, settings, videoDurationSeconds)
        : std::vector<QueuePhase>{};
    const auto p2Phases = settings.player2Enabled
        ? buildQueuePhasesFromSamples(output.queueObservationsP2, settings, videoDurationSeconds)
        : std::vector<QueuePhase>{};

    output.rawP1 = buildRawTimelineFromFlatBoards(p1Phases, output.boardObservationsP1);
    output.rawP2 = buildRawTimelineFromFlatBoards(p2Phases, output.boardObservationsP2);
    if (settings.forceEmptyOpening) {
        const auto occupied = [](const std::vector<BoardObservation>& boards) {
            return boards.empty() ? 0 : static_cast<int>(std::count_if(
                boards.front().board.begin(), boards.front().board.end(),
                [](Cell cell) { return cell != Cell::Empty; }));
        };
        // If both crops already resemble a tall mid-game stack, the scene
        // split itself is suspect rather than a small GO-overlay error. Keep
        // the established downstream reconstruction in that rare case and
        // enforce only its exported time-zero invariant after solving.
        const bool deferToExport = settings.player1Enabled &&
            settings.player2Enabled && occupied(output.boardObservationsP1) >
                BoardWidth * 3 && occupied(output.boardObservationsP2) >
                BoardWidth * 3;
        if (!deferToExport) {
            output.forcedEmptyOpeningApplied =
                forceKnownEmptyOpening(output.queueObservationsP1,
                                       output.boardObservationsP1,
                                       p1Phases, output.rawP1) ||
                output.forcedEmptyOpeningApplied;
            output.forcedEmptyOpeningApplied =
                forceKnownEmptyOpening(output.queueObservationsP2,
                                       output.boardObservationsP2,
                                       p2Phases, output.rawP2) ||
                output.forcedEmptyOpeningApplied;
        }
        // The normalized time-zero queue state is also the exported baseline.
        output.originalQueueObservationsP1 = output.queueObservationsP1;
        output.originalQueueObservationsP2 = output.queueObservationsP2;
    }

    // A chunked browser analysis rebuilds the raw timeline after the trailing
    // queue samples arrive. Keep legal candidates that the reviewer already
    // approved in the completed prefix. Matching both boundaries avoids
    // carrying a correction from the temporary, still-open last phase into a
    // different final phase.
    if (preserveManualCorrections) {
        const auto carry = [preserveManualUntilSeconds](const std::vector<TimelineStep>& oldTimeline,
                                                         std::vector<TimelineStep>& newTimeline) {
            for (const auto& oldStep : oldTimeline) {
                if (!oldStep.manuallyFixed) continue;
                if (preserveManualUntilSeconds >= 0 &&
                    oldStep.timeSeconds > preserveManualUntilSeconds + .000001) continue;
                const auto found = std::find_if(newTimeline.begin(), newTimeline.end(),
                    [&oldStep](const TimelineStep& newStep) {
                        return std::abs(newStep.startSeconds - oldStep.startSeconds) < .0005 &&
                               std::abs(newStep.timeSeconds - oldStep.timeSeconds) < .0005;
                    });
                if (found == newTimeline.end()) continue;
                found->manuallyFixed = true;
                found->board = oldStep.board;
                found->fullBoard = oldStep.fullBoard;
                found->garbage = oldStep.garbage;
                found->placedPiece = oldStep.placedPiece;
                found->placementX = oldStep.placementX;
                found->placementY = oldStep.placementY;
                found->placementRotation = oldStep.placementRotation;
                found->clearedLines = oldStep.clearedLines;
                found->score = oldStep.score;
            }
        };
        carry(previous.rawP1, output.rawP1);
        carry(previous.rawP2, output.rawP2);
    }
    return true;
}

bool solveRecoveryPlayer(RecoveryOutput& output, int player, const Settings& settings, std::string& error) {
    if (player != 1 && player != 2) {
        error = "Player must be 1 or 2";
        return false;
    }
    if (player == 1) {
        output.p1 = settings.player1Enabled
            ? TetrisEngine::beamSearchIncremental(output.rawP1, settings, output.beamCacheP1)
            : std::vector<TimelineStep>{};
    } else {
        output.p2 = settings.player2Enabled
            ? TetrisEngine::beamSearchIncremental(output.rawP2, settings, output.beamCacheP2)
            : std::vector<TimelineStep>{};
    }
    const auto& raw = player == 1 ? output.rawP1 : output.rawP2;
    const auto& solved = player == 1 ? output.p1 : output.p2;
    const bool enabled = player == 1 ? settings.player1Enabled : settings.player2Enabled;
    if (enabled && !raw.empty() && solved.empty()) {
        error = "Legal-move recovery returned no reviewable timeline for P" +
                std::to_string(player);
        return false;
    }
    if (enabled) repairSuspiciousQueueIdentityForPlayer(settings, output, player);
    error.clear();
    return true;
}

bool recoverObservations(double videoDurationSeconds, const Settings& settings,
                         std::vector<QueueRecognitionSample> p1Queue,
                         std::vector<QueueRecognitionSample> p2Queue,
                         std::vector<BoardObservation> p1Boards,
                         std::vector<BoardObservation> p2Boards,
                         RecoveryOutput& output, std::string& error) {
    if (!prepareRecoveryObservations(videoDurationSeconds, settings,
                                     std::move(p1Queue), std::move(p2Queue),
                                     std::move(p1Boards), std::move(p2Boards), output, error)) return false;
    if (!solveRecoveryPlayer(output, 1, settings, error)) return false;
    if (!solveRecoveryPlayer(output, 2, settings, error)) return false;
    return true;
}

bool prepareObservationRequests(double videoDurationSeconds, const Settings& settings,
                                std::vector<QueueRecognitionSample>& p1Queue,
                                std::vector<QueueRecognitionSample>& p2Queue,
                                std::vector<double>& p1BoardTimes,
                                std::vector<double>& p2BoardTimes,
                                std::string& error) {
    if (!(videoDurationSeconds > 0)) {
        error = "Browser observation input has no positive video duration";
        return false;
    }
    p1BoardTimes.clear();
    p2BoardTimes.clear();
    const auto p1Phases = settings.player1Enabled
        ? buildQueuePhasesFromSamples(p1Queue, settings, videoDurationSeconds)
        : std::vector<QueuePhase>{};
    const auto p2Phases = settings.player2Enabled
        ? buildQueuePhasesFromSamples(p2Queue, settings, videoDurationSeconds)
        : std::vector<QueuePhase>{};
    for (const auto& request : makeBoardRequests(p1Phases, settings.onnxSamples)) {
        p1BoardTimes.push_back(request.time);
    }
    for (const auto& request : makeBoardRequests(p2Phases, settings.onnxSamples)) {
        p2BoardTimes.push_back(request.time);
    }
    return true;
}

std::string boardMatrixJson(const std::string& board) {
    std::ostringstream out;
    out << '[';
    for (int y = 0; y < BoardHeight; ++y) {
        if (y) out << ',';
        out << '[';
        for (int x = 0; x < BoardWidth; ++x) {
            if (x) out << ',';
            const char cell = y * BoardWidth + x < static_cast<int>(board.size())
                ? board[static_cast<std::size_t>(y * BoardWidth + x)] : '_';
            if (cell == '_' || cell == 'E') out << "null";
            else out << '"' << cell << '"';
        }
        out << ']';
    }
    out << ']';
    return out.str();
}

bool reanalyzeQueueObservations(const Settings& settings, RecoveryOutput& output, std::string& error) {
    if (output.videoDurationSeconds <= 0) {
        error = "Raw queue log has no video duration";
        return false;
    }
    const auto p1Phases = settings.player1Enabled
        ? buildQueuePhasesFromSamples(output.queueObservationsP1, settings, output.videoDurationSeconds)
        : std::vector<QueuePhase>{};
    const auto p2Phases = settings.player2Enabled
        ? buildQueuePhasesFromSamples(output.queueObservationsP2, settings, output.videoDurationSeconds)
        : std::vector<QueuePhase>{};

    auto rawP1 = buildRawTimelineFromFlatBoards(p1Phases, output.boardObservationsP1);
    auto rawP2 = buildRawTimelineFromFlatBoards(p2Phases, output.boardObservationsP2);
    const auto carryManualAnchors = [](const std::vector<TimelineStep>& previous,
                                       std::vector<TimelineStep>& rebuilt) {
        std::vector<bool> claimed(rebuilt.size(), false);
        for (const TimelineStep& oldStep : previous) {
            if (!oldStep.manuallyFixed) continue;
            std::size_t best = rebuilt.size();
            double bestDistance = std::numeric_limits<double>::infinity();
            for (std::size_t index = rebuilt.size() > 1 ? 1 : 0; index < rebuilt.size(); ++index) {
                if (claimed[index]) continue;
                const double distance = std::abs(rebuilt[index].timeSeconds - oldStep.timeSeconds) +
                                        std::abs(rebuilt[index].startSeconds - oldStep.startSeconds);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }
            if (best == rebuilt.size()) {
                const auto position = std::upper_bound(
                    rebuilt.begin(), rebuilt.end(), oldStep.timeSeconds,
                    [](double time, const TimelineStep& step) { return time < step.timeSeconds; });
                const std::size_t inserted = static_cast<std::size_t>(position - rebuilt.begin());
                rebuilt.insert(position, oldStep);
                claimed.insert(claimed.begin() + static_cast<std::ptrdiff_t>(inserted), true);
                continue;
            }
            TimelineStep& target = rebuilt[best];
            target.manuallyFixed = true;
            target.board = oldStep.board;
            target.fullBoard = oldStep.fullBoard;
            target.garbage = oldStep.garbage;
            target.placedPiece = oldStep.placedPiece;
            target.placementX = oldStep.placementX;
            target.placementY = oldStep.placementY;
            target.placementRotation = oldStep.placementRotation;
            target.clearedLines = oldStep.clearedLines;
            target.score = oldStep.score;
            claimed[best] = true;
        }
    };
    carryManualAnchors(output.rawP1, rawP1);
    carryManualAnchors(output.rawP2, rawP2);
    output.rawP1 = std::move(rawP1);
    output.rawP2 = std::move(rawP2);
    return solveTimelinesParallel(
        settings, output.rawP1, output.rawP2, output.p1, output.p2, nullptr,
        &output.beamCacheP1, &output.beamCacheP2, &error);
}

namespace {

std::vector<TimelineStep>& reviewRawFor(RecoveryOutput& output, int player) {
    return player == 1 ? output.rawP1 : output.rawP2;
}

std::vector<TimelineStep>& reviewSolvedFor(RecoveryOutput& output, int player) {
    return player == 1 ? output.p1 : output.p2;
}

void resetManualAt(std::vector<TimelineStep>& raw, std::size_t index) {
    if (index >= raw.size()) return;
    raw[index].manuallyFixed = false;
    raw[index].board = raw[index].observed;
    raw[index].fullBoard = raw[index].observed;
    raw[index].garbage = {};
    raw[index].placedPiece = Cell::Empty;
    raw[index].placementX = 0;
    raw[index].placementY = 0;
    raw[index].placementRotation = 0;
    raw[index].clearedLines = 0;
    raw[index].score = 0;
}

} // namespace

bool applyCorrectionCandidate(RecoveryOutput& output, int player, std::size_t phase,
                              std::size_t candidateIndex, const Settings& settings,
                              const std::optional<GarbageRise>& overrideGarbage,
                              std::string& error) {
    if (player != 1 && player != 2) { error = "Player must be 1 or 2"; return false; }
    auto& raw = reviewRawFor(output, player);
    auto& solved = reviewSolvedFor(output, player);
    if (phase == 0 || phase >= raw.size() || solved.size() != raw.size()) {
        error = "Correction phase is out of range";
        return false;
    }
    const auto candidates = TetrisEngine::correctionCandidates(raw, solved, phase, settings, overrideGarbage);
    if (candidateIndex >= candidates.size()) {
        error = "Correction candidate is out of range";
        return false;
    }
    const std::vector<TimelineStep> previousRaw = raw;
    const std::vector<TimelineStep> previousSolved = solved;
    const CorrectionCandidate& selected = candidates[candidateIndex];
    raw[phase].manuallyFixed = true;
    raw[phase].board = selected.move.board;
    raw[phase].fullBoard = selected.move.fullBoard;
    raw[phase].garbage = selected.garbage;
    raw[phase].placedPiece = selected.move.piece;
    raw[phase].placementX = selected.move.x;
    raw[phase].placementY = selected.move.y;
    raw[phase].placementRotation = selected.move.rotation;
    raw[phase].clearedLines = selected.move.clearedLines;
    raw[phase].score = selected.observationScore;
    auto recomputed = TetrisEngine::recomputeFrom(raw, solved, phase, settings);
    if (recomputed.empty()) {
        raw = previousRaw;
        solved = previousSolved;
        error = "Legal-move generation produced zero candidates after the correction; change reverted";
        return false;
    }
    solved = std::move(recomputed);
    output.beamCacheP1.reset();
    output.beamCacheP2.reset();
    return true;
}

bool restoreAutomaticFrom(RecoveryOutput& output, int player, std::size_t phase,
                          const Settings& settings, std::string& error) {
    if (player != 1 && player != 2) { error = "Player must be 1 or 2"; return false; }
    auto& raw = reviewRawFor(output, player);
    auto& solved = reviewSolvedFor(output, player);
    if (phase == 0 || phase >= raw.size() || solved.size() != raw.size()) {
        error = "Restore phase is out of range";
        return false;
    }
    // Restore only the selected anchor. Later reviewed anchors are immutable;
    // the engine reconnects this one segment to the next anchor, or leaves a
    // visible discontinuity there if no legal bridge exists.
    const std::vector<TimelineStep> previousRaw = raw;
    const std::vector<TimelineStep> previousSolved = solved;
    resetManualAt(raw, phase);
    auto recomputed = TetrisEngine::recomputeFrom(raw, solved, phase, settings);
    if (recomputed.empty()) {
        raw = previousRaw;
        solved = previousSolved;
        error = "Legal-move generation produced zero candidates after restoring automatic recovery; change reverted";
        return false;
    }
    solved = std::move(recomputed);
    output.beamCacheP1.reset();
    output.beamCacheP2.reset();
    return true;
}

bool writeRecoveryAutosave(const std::filesystem::path& input,
                           const std::filesystem::path& autosavePath,
                           const RecoveryOutput& output,
                           std::string& error) {
    try {
        std::error_code ec;
        if (!autosavePath.parent_path().empty()) {
            std::filesystem::create_directories(autosavePath.parent_path(), ec);
            if (ec) { error = "Could not create autosave directory: " + ec.message(); return false; }
        }
        std::uintmax_t sourceBytes = 0;
        if (!input.empty() && std::filesystem::exists(input, ec)) {
            ec.clear();
            sourceBytes = std::filesystem::file_size(input, ec);
            if (ec) { sourceBytes = 0; ec.clear(); }
        }

        std::filesystem::path pending = autosavePath;
        pending += L".pending";
        std::ofstream file(pending, std::ios::binary | std::ios::trunc);
        if (!file) { error = "Could not open autosave file"; return false; }
        file << "{\"schema\":\"tetris-video-recovery.training/v2\""
             << ",\"source\":{\"videoPath\":\"" << jsonEscape(input.generic_u8string())
             << "\",\"originalVideoPath\":\"" << jsonEscape(input.generic_u8string())
             << "\",\"sha256\":\"\",\"bytes\":" << sourceBytes << "}"
             << ",\"dataset\":{\"selfContained\":false,\"videoFile\":\"\",\"videoStorage\":\"external\"}"
             << ",\"review\":{\"approvedByExplicitExport\":"
             << (output.humanApproved ? "true" : "false") << "}"
             << ",\"trajectoryRepair\":";
        writeTrajectoryRepairJson(file, output.trajectoryRepair);
        file << ",\"recoveryQuality\":";
        writeRecoveryQualityJson(file, evaluateRecoveryQualityForReport(output.p1, output.p2),
                                 output.sourceStartSeconds);
        file << ",\"playerNames\":{\"p1\":";
        writePlayerNameJson(file, output.p1PlayerName);
        file << ",\"p2\":";
        writePlayerNameJson(file, output.p2PlayerName);
        file << "},\"session\":{\"autosave\":true,\"videoDurationSeconds\":"
             << std::fixed << std::setprecision(6) << output.videoDurationSeconds
             << ",\"analyzedUntilSeconds\":" << output.analyzedUntilSeconds
             << ",\"sourceStartSeconds\":" << output.sourceStartSeconds
             << ",\"sourceEndSeconds\":" << output.sourceEndSeconds
             << ",\"sourceVideoDurationSeconds\":"
             << output.sourceVideoDurationSeconds << "}"
             << ",\"integrity\":{\"sourceSha256\":\"\",\"counts\":{"
             << "\"p1Labels\":" << output.p1.size() << ",\"p2Labels\":" << output.p2.size()
             << ",\"p1QueueObservations\":" << output.queueObservationsP1.size()
             << ",\"p2QueueObservations\":" << output.queueObservationsP2.size() << "}}"
             << ",\"queueObservations\":{\"format\":\"queue-delta/v1\",\"p1\":";
        writeTrainingQueueJson(file, output.queueObservationsP1);
        file << ",\"p2\":";
        writeTrainingQueueJson(file, output.queueObservationsP2);
        file << "},\"originalQueueObservations\":{\"format\":\"queue-delta/v1\",\"p1\":";
        writeTrainingQueueJson(file, output.originalQueueObservationsP1);
        file << ",\"p2\":";
        writeTrainingQueueJson(file, output.originalQueueObservationsP2);
        file << "},\"labels\":{\"format\":\"timeline-labels/v1\",\"p1\":";
        writeTrainingLabelsJson(file, output.p1);
        file << ",\"p2\":";
        writeTrainingLabelsJson(file, output.p2);
        file << "}}\n";
        file.close();
        if (!file) { error = "Could not finish autosave file"; return false; }

        std::filesystem::path previousAutosave = autosavePath.parent_path() /
            (autosavePath.stem().wstring() + L".previous" + autosavePath.extension().wstring());
        const bool hadAutosave = std::filesystem::exists(autosavePath, ec);
        if (ec) { error = "Could not inspect prior autosave: " + ec.message(); return false; }

#ifndef __EMSCRIPTEN__
        if (hadAutosave && !CopyFileW(autosavePath.c_str(), previousAutosave.c_str(), FALSE)) {
            error = "Could not preserve prior autosave (Windows error " +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
        if (!MoveFileExW(pending.c_str(), autosavePath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "Could not finalize autosave (Windows error " +
                    std::to_string(GetLastError()) + ")";
            return false;
        }
#else
        if (hadAutosave) {
            std::filesystem::copy_file(autosavePath, previousAutosave,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { error = "Could not preserve prior autosave: " + ec.message(); return false; }
        }
        std::filesystem::remove(autosavePath, ec);
        ec.clear();
        std::filesystem::rename(pending, autosavePath, ec);
        if (ec) { error = "Could not finalize autosave: " + ec.message(); return false; }
#endif
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Could not write autosave: ") + exception.what();
        return false;
    }
}

bool writeRecoveredOutput(const std::filesystem::path& input, const std::filesystem::path& outputDirectory,
                          const Settings& settings, RecoveryOutput& output, std::string& error) {
    int unresolvedPlayer = 0;
    std::size_t unresolvedPhase = 0;
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        error = "P" + std::to_string(unresolvedPlayer) + " phase " +
                std::to_string(unresolvedPhase) +
                " is unresolved; correct the preceding placement before export";
        return false;
    }
    return writeOutputs(input, outputDirectory, output.p1, output.p2,
                        settings.player1Enabled && settings.player2Enabled,
                        true, output, error);
}

bool writeRecoveredSimulatorLinks(const std::filesystem::path& input,
                                  const std::filesystem::path& outputDirectory,
                                  const Settings& settings,
                                  RecoveryOutput& output,
                                  std::string& error) {
    int unresolvedPlayer = 0;
    std::size_t unresolvedPhase = 0;
    if (firstRecoveryUnresolved(output, unresolvedPlayer, unresolvedPhase)) {
        error = "P" + std::to_string(unresolvedPlayer) + " phase " +
                std::to_string(unresolvedPhase) +
                " is unresolved; simulator links were not generated";
        return false;
    }
    return writeOutputs(input, outputDirectory, output.p1, output.p2,
                        settings.player1Enabled && settings.player2Enabled,
                        false, output, error);
}

#ifdef TR_CONSUMER_OUTPUT
bool prepareConsumerSimulatorLinks(const Settings& settings, RecoveryOutput& output,
                                   std::string& error) {
    int player = 0;
    std::size_t phase = 0;
    if (firstRecoveryUnresolved(output, player, phase) ||
        evaluateRecoveryQualityForReport(output.p1, output.p2).suspicious()) {
        error = "Final result did not pass verification";
        return false;
    }
    auto p1 = pageEntries(output.p1), p2 = pageEntries(output.p2);
    PageEntry blank;
    blank.board = std::string(BoardWidth * BoardHeight, '_');
    if (p1.empty()) p1.push_back(blank);
    if (p2.empty()) p2.push_back(blank);
    const bool twoPlayers = settings.player1Enabled && settings.player2Enabled;
    output.p1Url = withPageNumber(simulatorUrl(p1, p2, false), replayPageCount(p1, p2, false));
    output.p2Url = withPageNumber(simulatorUrl(p2, p1, false), replayPageCount(p2, p1, false));
    output.combinedUrl = twoPlayers
        ? withPageNumber(simulatorUrl(p1, p2, true), replayPageCount(p1, p2, true))
        : output.p1Url;
    return true;
}
#endif

bool recoverVideo(const std::filesystem::path& input, const std::filesystem::path& outputDirectory,
                  const std::filesystem::path& modelPath, const Settings& settings, Status& status,
                  RecoveryOutput& output, std::string& error) {
    if (!analyzeVideo(input, modelPath, settings, status, output, error)) return false;
    status.setMessage("Writing simulator URLs and report");
    if (!writeRecoveredOutput(input, outputDirectory, settings, output, error)) return false;
    status.setMessage("Recovery complete");
    return true;
}

} // namespace tr
