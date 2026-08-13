#pragma once

#include "common.hpp"

#include <windows.h>

#include <memory>

namespace tr {

class VideoReader {
public:
    VideoReader();
    ~VideoReader();
    VideoReader(const VideoReader&) = delete;
    VideoReader& operator=(const VideoReader&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);
    bool read(Frame& frame, bool& endOfStream, std::string& error);
    void close();

    int width() const { return width_; }
    int height() const { return height_; }
    double durationSeconds() const { return durationSeconds_; }
    double frameRate() const { return frameRate_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int width_ = 0;
    int height_ = 0;
    double durationSeconds_ = 0;
    double frameRate_ = 0;
};

} // namespace tr
