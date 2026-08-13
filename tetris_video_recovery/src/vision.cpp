#include "vision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tr {

namespace {

struct Rgb {
    double r = 0;
    double g = 0;
    double b = 0;
};

struct BgrPixel {
    // CanvasRenderingContext2D#getImageData exposes Uint8ClampedArray data.
    // Keep that quantisation before the ONNX feature extraction; feeding the
    // model interpolated floating-point pixels changes several decision-tree
    // thresholds in this particular model.
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
};

struct BoardImage {
    int width = 0;
    int height = 0;
    std::vector<BgrPixel> pixels;

    const BgrPixel& at(int x, int y) const {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
};

Cell minoFromChar(char value) {
    switch (value) {
    case 'I': return Cell::I;
    case 'O': return Cell::O;
    case 'T': return Cell::T;
    case 'L': return Cell::L;
    case 'J': return Cell::J;
    case 'S': return Cell::S;
    case 'Z': return Cell::Z;
    case 'G': return Cell::Garbage;
    default: return Cell::Empty;
    }
}

// Exact palette/order from 動画解析.html's SCAN_COLOR_PALETTE.
const std::array<std::pair<char, std::vector<Rgb>>, 9> ScanPalette{{
    {'N', {{0, 0, 0}, {48, 40, 56}}},
    {'G', {{153, 153, 153}, {216, 216, 216}}},
    {'I', {{1, 152, 153}, {1, 153, 213}, {0, 150, 153}}},
    {'O', {{153, 154, 2}, {249, 185, 0}}},
    {'T', {{152, 0, 153}, {135, 30, 136}}},
    {'L', {{153, 103, 0}, {245, 97, 0}}},
    {'J', {{0, 0, 187}, {0, 75, 165}}},
    {'S', {{16, 151, 31}, {92, 181, 35}}},
    {'Z', {{153, 0, 0}, {218, 24, 34}}},
}};

double colorDistanceSq(const Rgb& a, const Rgb& b) {
    const double dr = a.r - b.r;
    const double dg = a.g - b.g;
    const double db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

Cell findClosestMinoOnly(const Rgb& input) {
    // The original's Object.keys order is I,O,T,L,J,S,Z after NULL/G are
    // filtered. Keeping that order makes palette ties deterministic.
    Cell best = Cell::I;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const auto& [key, colors] : ScanPalette) {
        if (key == 'N' || key == 'G') continue;
        for (const auto& color : colors) {
            const double distance = colorDistanceSq(input, color);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = minoFromChar(key);
            }
        }
    }
    return best;
}

Cell findClosestColor(const Rgb& input) {
    for (const auto& color : ScanPalette[0].second) {
        if (colorDistanceSq(input, color) < 6000.0) return Cell::Empty;
    }
    for (const auto& color : ScanPalette[1].second) {
        if (colorDistanceSq(input, color) < 10000.0) return Cell::Garbage;
    }

    Cell best = Cell::Empty;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (const auto& [key, colors] : ScanPalette) {
        if (key == 'N' || key == 'G') continue;
        for (const auto& color : colors) {
            const double distance = colorDistanceSq(input, color);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = minoFromChar(key);
            }
        }
    }
    return bestDistance > 25000.0 ? Cell::Empty : best;
}

Rgb averageColorNonBlack(const Frame& frame, double cx, double cy, double radius) {
    // Port of getAverageColorNonBlack(). In particular, the rectangle starts
    // at floor(cx-radius) and uses ceil(radius*2), rather than a symmetric
    // inclusive box.
    const int startX = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int startY = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int diameter = static_cast<int>(std::ceil(radius * 2.0));
    const int endX = std::min(frame.width, startX + diameter);
    const int endY = std::min(frame.height, startY + diameter);
    if (endX <= startX || endY <= startY) return {};

    double totalR = 0, totalG = 0, totalB = 0, count = 0;
    const double radiusSq = radius * radius;
    for (int y = startY; y < endY; ++y) {
        for (int x = startX; x < endX; ++x) {
            const double dx = x - cx;
            const double dy = y - cy;
            if (dx * dx + dy * dy > radiusSq) continue;
            const auto offset = (static_cast<std::size_t>(y) * frame.width + x) * 4;
            const double r = frame.bgra[offset + 2];
            const double g = frame.bgra[offset + 1];
            const double b = frame.bgra[offset + 0];
            if (r > 50.0 || g > 50.0 || b > 50.0) {
                totalR += r;
                totalG += g;
                totalB += b;
                ++count;
            }
        }
    }
    if (count == 0) return {};
    return {totalR / count, totalG / count, totalB / count};
}

BoardImage makeBoardImage(const Frame& frame, const LayoutRect& layout, double cropX, double cropY, double scale) {
    BoardImage image;
    image.width = std::max(10, static_cast<int>(std::floor(layout.w)));
    image.height = std::max(20, static_cast<int>(std::floor(layout.h)));
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);

    const auto read = [&](int x, int y, int channel) -> double {
        return frame.bgra[(static_cast<std::size_t>(y) * frame.width + x) * 4 + channel];
    };
    const auto canvasByte = [](double value) -> std::uint8_t {
        // Web IDL's ToUint8Clamp: nearest integer, ties to even.
        value = std::clamp(value, 0.0, 255.0);
        const int lower = static_cast<int>(std::floor(value));
        const double fraction = value - lower;
        if (fraction > .5 || (fraction == .5 && (lower & 1))) return static_cast<std::uint8_t>(lower + 1);
        return static_cast<std::uint8_t>(lower);
    };
    const auto sample = [&](double sourceX, double sourceY) {
        const double x = std::clamp(sourceX, 0.0, static_cast<double>(frame.width - 1));
        const double y = std::clamp(sourceY, 0.0, static_cast<double>(frame.height - 1));
        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(frame.width - 1, x0 + 1);
        const int y1 = std::min(frame.height - 1, y0 + 1);
        const double fx = x - x0;
        const double fy = y - y0;
        const auto blend = [&](int channel) {
            const double top = read(x0, y0, channel) * (1.0 - fx) + read(x1, y0, channel) * fx;
            const double bottom = read(x0, y1, channel) * (1.0 - fx) + read(x1, y1, channel) * fx;
            return top * (1.0 - fy) + bottom * fy;
        };
        return BgrPixel{canvasByte(blend(0)), canvasByte(blend(1)), canvasByte(blend(2))};
    };

    // This is the pixel-center form of CanvasRenderingContext2D.drawImage:
    // drawImage(source, sx, sy, sw, sh, 0, 0, boardW, boardH).
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const double sourceX = cropX + layout.x * scale + (x + .5) * scale - .5;
            const double sourceY = cropY + layout.y * scale + (y + .5) * scale - .5;
            image.pixels[static_cast<std::size_t>(y) * image.width + x] = sample(sourceX, sourceY);
        }
    }
    return image;
}

std::vector<float> featuresFromBoardImage(const BoardImage& image) {
    std::vector<float> features;
    features.reserve(200 * 63);
    const double cellW = static_cast<double>(image.width) / BoardWidth;
    const double cellH = static_cast<double>(image.height) / VisibleRows;
    const int cellPixelWidth = std::max(1, static_cast<int>(std::floor(cellW)));
    const int cellPixelHeight = std::max(1, static_cast<int>(std::floor(cellH)));
    std::array<std::vector<float>, 6> channels;
    for (auto& channel : channels) channel.reserve(cellPixelWidth * cellPixelHeight);
    const auto stats = [](const std::vector<float>& values) {
        double sum = 0;
        for (const float value : values) sum += value;
        const double mean = sum / std::max<std::size_t>(1, values.size());
        double squareDifference = 0;
        for (const float value : values) {
            const double difference = value - mean;
            squareDifference += difference * difference;
        }
        return std::array<float, 2>{static_cast<float>(mean), static_cast<float>(std::sqrt(squareDifference / std::max<std::size_t>(1, values.size())))};
    };

    for (int row = 0; row < VisibleRows; ++row) {
        for (int col = 0; col < BoardWidth; ++col) {
            // Exact extractCellPixels(x, y, w, h) integer conversion.
            const int cellX = static_cast<int>(std::floor(col * cellW));
            const int cellY = static_cast<int>(std::floor(row * cellH));
            // This is a literal port of extractCellPixels() followed by
            // extractFeaturesJS().  The temporary channels matter because
            // JavaScript stores them in Float32Array before calculating the
            // statistics and tiles.
            const int pixelCount = cellPixelWidth * cellPixelHeight;
            for (auto& channel : channels) channel.clear();
            for (int localY = 0; localY < cellPixelHeight; ++localY) {
                for (int localX = 0; localX < cellPixelWidth; ++localX) {
                    const auto& p = image.at(cellX + localX, cellY + localY);
                    const double r = p.r, g = p.g, b = p.b;
                    const double maximum = std::max({r, g, b});
                    const double minimum = std::min({r, g, b});
                    const double difference = maximum - minimum;
                    double hue = 0;
                    if (maximum == minimum) hue = 0;
                    else if (maximum == r) hue = std::fmod(60.0 * (g - b) / difference + 360.0, 360.0);
                    else if (maximum == g) hue = std::fmod(60.0 * (b - r) / difference + 120.0, 360.0);
                    else hue = std::fmod(60.0 * (r - g) / difference + 240.0, 360.0);
                    channels[0].push_back(static_cast<float>(b));
                    channels[1].push_back(static_cast<float>(g));
                    channels[2].push_back(static_cast<float>(r));
                    channels[3].push_back(static_cast<float>(hue / 2.0));
                    channels[4].push_back(static_cast<float>(maximum != 0 ? difference / maximum * 255.0 : 0.0));
                    channels[5].push_back(static_cast<float>(maximum));
                }
            }
            for (const auto& channel : channels) {
                const auto result = stats(channel);
                features.push_back(result[0]);
                features.push_back(result[1]);
            }

            // The 4x4 BGR tiles from extractFeaturesJS().
            for (int tileY = 0; tileY < 4; ++tileY) {
                for (int tileX = 0; tileX < 4; ++tileX) {
                    const int x0 = static_cast<int>(std::floor(tileX * (cellPixelWidth / 4.0)));
                    const int y0 = static_cast<int>(std::floor(tileY * (cellPixelHeight / 4.0)));
                    const int x1 = static_cast<int>(std::floor((tileX + 1) * (cellPixelWidth / 4.0)));
                    const int y1 = static_cast<int>(std::floor((tileY + 1) * (cellPixelHeight / 4.0)));
                    double sumB = 0, sumG = 0, sumR = 0, tileCount = 0;
                    for (int y = y0; y < y1; ++y) {
                        for (int x = x0; x < x1; ++x) {
                            const int pixel = y * cellPixelWidth + x;
                            if (pixel < pixelCount) {
                                sumB += channels[0][pixel];
                                sumG += channels[1][pixel];
                                sumR += channels[2][pixel];
                                ++tileCount;
                            }
                        }
                    }
                    if (tileCount == 0) {
                        features.insert(features.end(), {0.0f, 0.0f, 0.0f});
                    } else {
                        features.push_back(static_cast<float>(sumB / tileCount));
                        features.push_back(static_cast<float>(sumG / tileCount));
                        features.push_back(static_cast<float>(sumR / tileCount));
                    }
                }
            }

            const int centerX = static_cast<int>(std::floor(cellPixelWidth / 2.0));
            const int centerY = static_cast<int>(std::floor(cellPixelHeight / 2.0));
            const int centerWidth = static_cast<int>(std::floor(cellPixelWidth / 4.0));
            const int centerHeight = static_cast<int>(std::floor(cellPixelHeight / 4.0));
            const int startX = centerX - centerWidth;
            const int endCenterX = centerX + centerWidth;
            const int startY = centerY - centerHeight;
            const int endCenterY = centerY + centerHeight;
            double sumB = 0, sumG = 0, sumR = 0, centerCount = 0;
            for (int y = startY; y < endCenterY; ++y) {
                for (int x = startX; x < endCenterX; ++x) {
                    if (x >= 0 && x < cellPixelWidth && y >= 0 && y < cellPixelHeight) {
                        const int pixel = y * cellPixelWidth + x;
                        sumB += channels[0][pixel];
                        sumG += channels[1][pixel];
                        sumR += channels[2][pixel];
                        ++centerCount;
                    }
                }
            }
            if (centerCount == 0) {
                features.insert(features.end(), {0.0f, 0.0f, 0.0f});
            } else {
                features.push_back(static_cast<float>(sumB / centerCount));
                features.push_back(static_cast<float>(sumG / centerCount));
                features.push_back(static_cast<float>(sumR / centerCount));
            }
        }
    }
    return features;
}

VisibleBoard classicBoardFromImage(const BoardImage& image) {
    VisibleBoard result{};
    const double cellW = static_cast<double>(image.width) / BoardWidth;
    const double cellH = static_cast<double>(image.height) / VisibleRows;
    const int sampleSize = std::max(1, static_cast<int>(std::floor(cellW * .25)));
    for (int row = 0; row < VisibleRows; ++row) {
        for (int col = 0; col < BoardWidth; ++col) {
            const double sampleX = (col + .5) * cellW;
            const double sampleY = (row + .5) * cellH;
            const int x0 = std::clamp(static_cast<int>(std::floor(sampleX - sampleSize / 2.0)), 0, image.width - 1);
            const int y0 = std::clamp(static_cast<int>(std::floor(sampleY - sampleSize / 2.0)), 0, image.height - 1);
            const int x1 = std::min(image.width, x0 + sampleSize);
            const int y1 = std::min(image.height, y0 + sampleSize);
            double sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const auto& p = image.at(x, y);
                    sumR += p.r;
                    sumG += p.g;
                    sumB += p.b;
                    ++count;
                }
            }
            result[index(col, row)] = findClosestColor({sumR / count, sumG / count, sumB / count});
        }
    }
    return result;
}

struct VoteCell {
    std::array<int, 9> counts{};
    std::array<int, 9> firstSeen{};

    VoteCell() { firstSeen.fill(-1); }

    void add(Cell cell, int sample) {
        const std::size_t i = static_cast<std::size_t>(cell);
        if (counts[i] == 0) firstSeen[i] = sample;
        ++counts[i];
    }

    Cell winner(int samples, std::uint8_t& confidence) const {
        int bestCount = -1;
        int earliest = std::numeric_limits<int>::max();
        Cell best = Cell::Empty;
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] == 0) continue;
            if (counts[i] > bestCount || (counts[i] == bestCount && firstSeen[i] < earliest)) {
                bestCount = counts[i];
                earliest = firstSeen[i];
                best = static_cast<Cell>(i);
            }
        }
        confidence = static_cast<std::uint8_t>(std::clamp(bestCount * 255 / std::max(1, samples), 0, 255));
        return best;
    }
};

} // namespace

VisionAnalyzer::VisionAnalyzer(int frameWidth, int frameHeight, const PlayerLayout& layout, const OnnxBoardModel* model)
    : frameWidth_(frameWidth), frameHeight_(frameHeight), layout_(layout), model_(model) {
    const double targetAspect = 16.0 / 9.0;
    const double sourceAspect = static_cast<double>(frameWidth_) / std::max(1, frameHeight_);
    double cropWidth = 0, cropX = 0, cropY = 0;
    if (sourceAspect > targetAspect) {
        cropWidth = frameHeight_ * targetAspect;
        cropX = (frameWidth_ - cropWidth) / 2.0;
    } else {
        cropWidth = frameWidth_;
        cropY = (frameHeight_ - cropWidth / targetAspect) / 2.0;
    }
    crop_ = {cropX, cropY, cropWidth / 1920.0};
}

QueueObservation VisionAnalyzer::observeQueue(const Frame& frame) const {
    QueueObservation result;
    const double scale = crop_.scale * (1920.0 / layout_.queueCoordinateWidth);
    const double radius = layout_.queueSampleRadius * scale;
    const std::array<std::array<double, 2>, 6> coordinates{{
        layout_.hold, layout_.next[0], layout_.next[1], layout_.next[2], layout_.next[3], layout_.next[4]
    }};

    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        const auto& coordinate = coordinates[i];
        const Rgb color = averageColorNonBlack(frame,
            crop_.x + coordinate[0] * scale,
            crop_.y + coordinate[1] * scale,
            radius);
        result.colors.push_back({
            static_cast<std::uint8_t>(std::clamp(std::lround(color.r), 0l, 255l)),
            static_cast<std::uint8_t>(std::clamp(std::lround(color.g), 0l, 255l)),
            static_cast<std::uint8_t>(std::clamp(std::lround(color.b), 0l, 255l)),
        });

        const bool black = color.r < 50 && color.g < 50 && color.b < 50;
        if (i == 0) {
            if (!black) result.hold = findClosestMinoOnly(color);
        } else {
            // The source intentionally only stops for a black first preview;
            // subsequent previews are always classified.
            if (i == 1 && black) break;
            result.next.push_back(findClosestMinoOnly(color));
        }
    }
    return result;
}

std::vector<float> VisionAnalyzer::extractBoardFeatures(const Frame& frame) const {
    return featuresFromBoardImage(makeBoardImage(frame, layout_.board, crop_.x, crop_.y, crop_.scale));
}

VisibleBoard VisionAnalyzer::classicBoard(const Frame& frame) const {
    return classicBoardFromImage(makeBoardImage(frame, layout_.board, crop_.x, crop_.y, crop_.scale));
}

BoardObservation VisionAnalyzer::analyzeBoard(const Frame& frame) const {
    BoardObservation result;
    result.timeSeconds = static_cast<double>(frame.time100ns) / 10000000.0;
    if (!model_ || !model_->ready()) {
        result.recognitionError = "ONNX model is not available";
        return result;
    }

    // Keep the ONNX and the original row-recovery fallback together, exactly
    // as analyzeBoardOnly() does in 動画解析.html.
    const BoardImage image = makeBoardImage(frame, layout_.board, crop_.x, crop_.y, crop_.scale);
    const auto labels = model_->infer(featuresFromBoardImage(image), result.recognitionError);
    const auto classic = classicBoardFromImage(image);
    for (int row = 0; row < VisibleRows; ++row) {
        int garbage = 0;
        bool onlyEmptyOrGarbage = true;
        for (int col = 0; col < BoardWidth; ++col) {
            const Cell cell = labels[index(col, row)];
            result.board[index(col, row)] = cell;
            if (cell == Cell::Garbage) ++garbage;
            if (cell != Cell::Empty && cell != Cell::Garbage) onlyEmptyOrGarbage = false;
        }
        if (onlyEmptyOrGarbage && garbage != BoardWidth) {
            int classicGarbage = 0;
            int classicEmpty = 0;
            for (int col = 0; col < BoardWidth; ++col) {
                const Cell cell = classic[index(col, row)];
                if (cell == Cell::Garbage) ++classicGarbage;
                if (cell == Cell::Empty) ++classicEmpty;
            }
            if (classicGarbage == 9 && classicEmpty == 1) {
                for (int col = 0; col < BoardWidth; ++col) result.board[index(col, row)] = classic[index(col, row)];
            }
        }
    }
    result.confidence.fill(255);
    result.quality = 1.0;
    return result;
}

BoardObservation VisionAnalyzer::analyze(const Frame& frame) const {
    BoardObservation result = analyzeBoard(frame);
    result.queue = observeQueue(frame);
    return result;
}

Board VisionAnalyzer::makeFullBoard(const VisibleBoard& visible) {
    Board board{};
    for (int i = 0; i < BoardWidth * VisibleRows; ++i) {
        board[BoardWidth * VisibleRows + i] = visible[i];
    }
    return board;
}

BoardObservation VisionAnalyzer::aggregate(const std::vector<BoardObservation>& samples) {
    BoardObservation result;
    if (samples.empty()) return result;
    result.timeSeconds = samples.back().timeSeconds;
    result.queue = samples.back().queue;
    for (int i = 0; i < BoardWidth * VisibleRows; ++i) {
        VoteCell vote;
        for (int sample = 0; sample < static_cast<int>(samples.size()); ++sample) {
            vote.add(samples[sample].board[i], sample);
        }
        result.board[i] = vote.winner(static_cast<int>(samples.size()), result.confidence[i]);
    }
    double quality = 0;
    for (const auto confidence : result.confidence) quality += confidence;
    result.quality = quality / (BoardWidth * VisibleRows * 255.0);
    return result;
}

} // namespace tr
