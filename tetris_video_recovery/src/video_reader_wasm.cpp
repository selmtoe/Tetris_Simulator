#include "video_reader.hpp"

#ifdef __EMSCRIPTEN__

namespace tr {

struct VideoReader::Impl {};

VideoReader::VideoReader() = default;
VideoReader::~VideoReader() { close(); }

bool VideoReader::open(const std::filesystem::path&, std::string& error) {
    error = "Browser builds receive decoded frames from HTMLVideoElement";
    return false;
}

bool VideoReader::read(Frame&, bool& endOfStream, std::string& error) {
    endOfStream = true;
    error = "Browser builds receive decoded frames from HTMLVideoElement";
    return false;
}

void VideoReader::close() {
    impl_.reset();
    width_ = height_ = 0;
    durationSeconds_ = frameRate_ = 0;
}

} // namespace tr

#endif
