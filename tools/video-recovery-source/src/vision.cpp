#include "vision.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <cmath>
#include <limits>

namespace tr {

namespace {

#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
thread_local bool browserGpuPreprocessing = false;
double browserCudaBlend(double a, double b, double c, double d, double fx, double fy) {
    const double top = a * (1.0 - fx) + b * fx;
    const double bottom = c * (1.0 - fx) + d * fx;
    const double value = top * (1.0 - fy) + bottom * fy;
    // Inputs are bytes and weights lie in [0,1]. The absolute rounding error
    // of these bounded operations is below 2e-12. Only a value this close to
    // a half-integer can round to a different byte after CUDA's fused math.
    if (std::abs((value - std::floor(value)) - .5) > 2e-12) return value;
    const double exactTop = std::fma(1.0 - fx, a, b * fx);
    const double exactBottom = std::fma(1.0 - fx, c, d * fx);
    return std::fma(1.0 - fy, exactTop, exactBottom * fy);
}
struct BrowserGpuPreprocessingScope {
 bool before = browserGpuPreprocessing;
 BrowserGpuPreprocessingScope() { browserGpuPreprocessing = true; }
 ~BrowserGpuPreprocessingScope() { browserGpuPreprocessing = before; }
};
#endif

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
    const int fullStartX = std::max(0, static_cast<int>(std::floor(cx - radius)));
    const int fullStartY = std::max(0, static_cast<int>(std::floor(cy - radius)));
    const int diameter = static_cast<int>(std::ceil(radius * 2.0));
    const int startX = std::max(0, fullStartX - frame.sourceX);
    const int startY = std::max(0, fullStartY - frame.sourceY);
    const int endX = std::min(frame.width, fullStartX + diameter - frame.sourceX);
    const int endY = std::min(frame.height, fullStartY + diameter - frame.sourceY);
    if (endX <= startX || endY <= startY) return {};

    double totalR = 0, totalG = 0, totalB = 0, count = 0;
    const double radiusSq = radius * radius;
    for (int y = startY; y < endY; ++y) {
        for (int x = startX; x < endX; ++x) {
            const double dx = (x + frame.sourceX) - cx;
            const double dy = (y + frame.sourceY) - cy;
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

BoardImage makeBoardImageUncached(const Frame& frame, const LayoutRect& layout, double cropX, double cropY, double scale) {
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
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
            if (browserGpuPreprocessing) {
            return browserCudaBlend(read(x0, y0, channel), read(x1, y0, channel),
                read(x0, y1, channel), read(x1, y1, channel), fx, fy);
            }
#endif

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

void makeBoardImage(const Frame& frame, const LayoutRect& layout, double cropX, double cropY, double scale,
                    int expectedFrameWidth, int expectedFrameHeight, int imageWidth, int imageHeight,
                    const std::vector<std::array<std::size_t, 4>>& sampleOffsets,
                    const std::vector<std::array<double, 2>>& sampleFractions,
                    BoardImage& image) {
    if (frame.width != expectedFrameWidth || frame.height != expectedFrameHeight ||
        imageWidth <= 0 || imageHeight <= 0 ||
        sampleOffsets.size() != static_cast<std::size_t>(imageWidth) * imageHeight ||
        sampleFractions.size() != sampleOffsets.size()) {
        image = makeBoardImageUncached(frame, layout, cropX, cropY, scale);
        return;
    }

    image.width = imageWidth;
    image.height = imageHeight;
    image.pixels.resize(sampleOffsets.size());
    const auto canvasByte = [](double value) -> std::uint8_t {
        value = std::clamp(value, 0.0, 255.0);
        const int lower = static_cast<int>(std::floor(value));
        const double fraction = value - lower;
        if (fraction > .5 || (fraction == .5 && (lower & 1))) return static_cast<std::uint8_t>(lower + 1);
        return static_cast<std::uint8_t>(lower);
    };
    for (std::size_t i = 0; i < sampleOffsets.size(); ++i) {
        const auto& offsets = sampleOffsets[i];
        const double fx = sampleFractions[i][0];
        const double fy = sampleFractions[i][1];
        const auto blend = [&](int channel) {
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
            if (browserGpuPreprocessing) {
            return browserCudaBlend(frame.bgra[offsets[0] + channel], frame.bgra[offsets[1] + channel],
                frame.bgra[offsets[2] + channel], frame.bgra[offsets[3] + channel], fx, fy);
            }
#endif

            const double top = frame.bgra[offsets[0] + channel] * (1.0 - fx) +
                               frame.bgra[offsets[1] + channel] * fx;
            const double bottom = frame.bgra[offsets[2] + channel] * (1.0 - fx) +
                                  frame.bgra[offsets[3] + channel] * fx;
            return top * (1.0 - fy) + bottom * fy;

        };
        image.pixels[i] = BgrPixel{canvasByte(blend(0)), canvasByte(blend(1)), canvasByte(blend(2))};
    }
}

BoardImage makeBoardImage(const Frame& frame, const LayoutRect& layout, double cropX, double cropY, double scale,
                         int expectedFrameWidth, int expectedFrameHeight, int imageWidth, int imageHeight,
                         const std::vector<std::array<std::size_t, 4>>& sampleOffsets,
                         const std::vector<std::array<double, 2>>& sampleFractions) {
    BoardImage image;
    makeBoardImage(frame, layout, cropX, cropY, scale, expectedFrameWidth, expectedFrameHeight,
                   imageWidth, imageHeight, sampleOffsets, sampleFractions, image);
    return image;
}

std::vector<float> featuresFromBoardImage(const BoardImage& image) {
    std::vector<float> features(200 * 63);
    std::size_t featureIndex = 0;
    const double cellW = static_cast<double>(image.width) / BoardWidth;
    const double cellH = static_cast<double>(image.height) / VisibleRows;
    const int cellPixelWidth = std::max(1, static_cast<int>(std::floor(cellW)));
    const int cellPixelHeight = std::max(1, static_cast<int>(std::floor(cellH)));
    std::array<int, BoardWidth> cellX{};
    std::array<int, VisibleRows> cellY{};
    for (int col = 0; col < BoardWidth; ++col) cellX[col] = static_cast<int>(std::floor(col * cellW));
    for (int row = 0; row < VisibleRows; ++row) cellY[row] = static_cast<int>(std::floor(row * cellH));
    std::array<int, 4> tileX0{}, tileY0{}, tileX1{}, tileY1{};
    for (int tile = 0; tile < 4; ++tile) {
        tileX0[tile] = static_cast<int>(std::floor(tile * (cellPixelWidth / 4.0)));
        tileY0[tile] = static_cast<int>(std::floor(tile * (cellPixelHeight / 4.0)));
        tileX1[tile] = static_cast<int>(std::floor((tile + 1) * (cellPixelWidth / 4.0)));
        tileY1[tile] = static_cast<int>(std::floor((tile + 1) * (cellPixelHeight / 4.0)));
    }
    const int centerX = static_cast<int>(std::floor(cellPixelWidth / 2.0));
    const int centerY = static_cast<int>(std::floor(cellPixelHeight / 2.0));
    const int centerWidth = static_cast<int>(std::floor(cellPixelWidth / 4.0));
    const int centerHeight = static_cast<int>(std::floor(cellPixelHeight / 4.0));
    const int startX = centerX - centerWidth;
    const int endCenterX = centerX + centerWidth;
    const int startY = centerY - centerHeight;
    const int endCenterY = centerY + centerHeight;
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
            const int originX = cellX[col];
            const int originY = cellY[row];
            // This is a literal port of extractCellPixels() followed by
            // extractFeaturesJS().  The temporary channels matter because
            // JavaScript stores them in Float32Array before calculating the
            // statistics and tiles.
            const int pixelCount = cellPixelWidth * cellPixelHeight;
            for (auto& channel : channels) channel.clear();
            for (int localY = 0; localY < cellPixelHeight; ++localY) {
                for (int localX = 0; localX < cellPixelWidth; ++localX) {
                    const auto& p = image.pixels[static_cast<std::size_t>(originY + localY) * image.width + originX + localX];
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
                features[featureIndex++] = result[0];
                features[featureIndex++] = result[1];
            }

            // The 4x4 BGR tiles from extractFeaturesJS().
            for (int tileY = 0; tileY < 4; ++tileY) {
                for (int tileX = 0; tileX < 4; ++tileX) {
                    const int x0 = tileX0[tileX];
                    const int y0 = tileY0[tileY];
                    const int x1 = tileX1[tileX];
                    const int y1 = tileY1[tileY];
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
                        features[featureIndex++] = 0.0f;
                        features[featureIndex++] = 0.0f;
                        features[featureIndex++] = 0.0f;
                    } else {
                        features[featureIndex++] = static_cast<float>(sumB / tileCount);
                        features[featureIndex++] = static_cast<float>(sumG / tileCount);
                        features[featureIndex++] = static_cast<float>(sumR / tileCount);
                    }
                }
            }

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
                features[featureIndex++] = 0.0f;
                features[featureIndex++] = 0.0f;
                features[featureIndex++] = 0.0f;
            } else {
                features[featureIndex++] = static_cast<float>(sumB / centerCount);
                features[featureIndex++] = static_cast<float>(sumG / centerCount);
                features[featureIndex++] = static_cast<float>(sumR / centerCount);
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

void setOneHot(BoardProbabilities& probabilities, int cell, Cell value) {
    const std::size_t base = static_cast<std::size_t>(cell) * CellClassCount;
    std::fill_n(probabilities.begin() + base, CellClassCount, 0.0f);
    probabilities[probabilityIndex(cell, value)] = 1.0f;
}

Cell probabilityWinner(const BoardProbabilities& probabilities, int cell, float& confidence) {
    Cell best = Cell::Empty;
    confidence = probabilities[probabilityIndex(cell, best)];
    for (int value = 1; value < CellClassCount; ++value) {
        const Cell candidate = static_cast<Cell>(value);
        const float probability = probabilities[probabilityIndex(cell, candidate)];
        if (probability > confidence) {
            best = candidate;
            confidence = probability;
        }
    }
    return best;
}

} // namespace

int HorizontalAlignmentScore::value() const {
    // Occupied cells carry most of the evidence. Empty/empty agreement is
    // useful, but it must never make a mostly blank, badly shifted crop win.
    return exactOccupiedCells * 200 +
           exactEmptyCells * 3 +
           occupiedColorMismatches * 10 -
           occupancyMismatches * 120 +
           exactGarbageRows * 250 +
           cleanGarbageRows * 40 -
           garbagePieceCells * 80;
}

void cnnCellsFromBoardImage(const BoardImage& image, std::vector<float>& cells) {
    constexpr int CellSize = 32;
    constexpr int ResizedWidth = BoardWidth * CellSize;
    constexpr int ResizedHeight = VisibleRows * CellSize;
    constexpr std::size_t Plane = CellSize * CellSize;
    constexpr std::size_t ValuesPerCell = Plane * 3;
    cells.resize(static_cast<std::size_t>(BoardWidth) * VisibleRows * ValuesPerCell);

    // Match the training loader: resize the complete board to 320x640 with
    // bilinear sampling, then split it into a row-major 10x20 grid. Resizing
    // the whole board keeps every cell boundary identical across P1/P2 crops.
    // The source dimensions are constant for each player, so cache the axis
    // mapping instead of repeating clamp/floor work for all three channels of
    // every output pixel. Two entries cover the differently sized P1/P2 crops
    // on each worker thread without sharing mutable state between workers.
    struct ResizeMap {
        int width = 0;
        int height = 0;
        std::array<int, ResizedWidth> x0{};
        std::array<int, ResizedWidth> x1{};
        std::array<double, ResizedWidth> fx{};
        std::array<int, ResizedHeight> y0{};
        std::array<int, ResizedHeight> y1{};
        std::array<double, ResizedHeight> fy{};
    };
    thread_local std::array<ResizeMap, 2> maps;
    thread_local std::size_t nextMap = 0;
    ResizeMap* mapping = nullptr;
    for (auto& candidate : maps) {
        if (candidate.width == image.width && candidate.height == image.height) {
            mapping = &candidate;
            break;
        }
    }
    if (!mapping) {
        mapping = &maps[nextMap++ % maps.size()];
        mapping->width = image.width;
        mapping->height = image.height;
        for (int x = 0; x < ResizedWidth; ++x) {
            const double source = std::clamp(
                (x + .5) * image.width / ResizedWidth - .5,
                0.0, static_cast<double>(image.width - 1));
            mapping->x0[static_cast<std::size_t>(x)] = static_cast<int>(std::floor(source));
            mapping->x1[static_cast<std::size_t>(x)] = std::min(
                image.width - 1, mapping->x0[static_cast<std::size_t>(x)] + 1);
            mapping->fx[static_cast<std::size_t>(x)] =
                source - mapping->x0[static_cast<std::size_t>(x)];
        }
        for (int y = 0; y < ResizedHeight; ++y) {
            const double source = std::clamp(
                (y + .5) * image.height / ResizedHeight - .5,
                0.0, static_cast<double>(image.height - 1));
            mapping->y0[static_cast<std::size_t>(y)] = static_cast<int>(std::floor(source));
            mapping->y1[static_cast<std::size_t>(y)] = std::min(
                image.height - 1, mapping->y0[static_cast<std::size_t>(y)] + 1);
            mapping->fy[static_cast<std::size_t>(y)] =
                source - mapping->y0[static_cast<std::size_t>(y)];
        }
    }

    for (int y = 0; y < ResizedHeight; ++y) {
        const int row = y / CellSize;
        const int localY = y % CellSize;
        const int y0 = mapping->y0[static_cast<std::size_t>(y)];
        const int y1 = mapping->y1[static_cast<std::size_t>(y)];
        const double fy = mapping->fy[static_cast<std::size_t>(y)];
        for (int x = 0; x < ResizedWidth; ++x) {
            const int col = x / CellSize;
            const int localX = x % CellSize;
            const std::size_t cell = static_cast<std::size_t>(row * BoardWidth + col);
            const std::size_t pixel = static_cast<std::size_t>(localY * CellSize + localX);
            const int x0 = mapping->x0[static_cast<std::size_t>(x)];
            const int x1 = mapping->x1[static_cast<std::size_t>(x)];
            const double fx = mapping->fx[static_cast<std::size_t>(x)];
            const auto& topLeft = image.at(x0, y0);
            const auto& topRight = image.at(x1, y0);
            const auto& bottomLeft = image.at(x0, y1);
            const auto& bottomRight = image.at(x1, y1);
            const auto resizeChannel = [&](double a, double b, double c, double d) {
#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
            if (browserGpuPreprocessing) {
                // Match the operation order emitted by NVCC for the normal
                // FP64 board kernel, including its fused multiply-adds.
                return static_cast<float>(std::clamp(
                    std::nearbyint(browserCudaBlend(a, b, c, d, fx, fy)), 0.0, 255.0)) / 255.0f;
            }
#endif

                const double top = a * (1.0 - fx) + b * fx;
                const double bottom = c * (1.0 - fx) + d * fx;
                return static_cast<float>(std::clamp(
                    std::nearbyint(top * (1.0 - fy) + bottom * fy), 0.0, 255.0) / 255.0);

            };
            const std::size_t base = cell * ValuesPerCell + pixel;
            cells[base] = resizeChannel(topLeft.r, topRight.r, bottomLeft.r, bottomRight.r);
            cells[base + Plane] = resizeChannel(topLeft.g, topRight.g, bottomLeft.g, bottomRight.g);
            cells[base + Plane * 2] = resizeChannel(topLeft.b, topRight.b, bottomLeft.b, bottomRight.b);
        }
    }
}

std::vector<float> cnnCellsFromBoardImage(const BoardImage& image) {
    std::vector<float> cells;
    cnnCellsFromBoardImage(image, cells);
    return cells;
}

bool HorizontalAlignmentScore::needsRetry() const {
    return targetRows > 0 &&
           (garbagePieceCells > 0 || exactGarbageRows != targetRows);
}

std::vector<int> horizontalAlignmentRows(const VisibleBoard& classic) {
    std::vector<int> result;
    for (int row = 0; row < VisibleRows; ++row) {
        int garbage = 0;
        int empty = 0;
        for (int col = 0; col < BoardWidth; ++col) {
            const Cell cell = classic[index(col, row)];
            if (cell == Cell::Garbage) ++garbage;
            if (cell == Cell::Empty) ++empty;
        }
        if (garbage >= 7 && empty > 0) result.push_back(row);
    }
    return result;
}

HorizontalAlignmentScore scoreHorizontalAlignment(
    const VisibleBoard& classic,
    const BoardObservation& observation,
    const std::vector<int>& targetRows) {
    HorizontalAlignmentScore result;
    result.targetRows = static_cast<int>(targetRows.size());

    for (int row = 0; row < VisibleRows; ++row) {
        for (int col = 0; col < BoardWidth; ++col) {
            const Cell classicCell = classic[index(col, row)];
            const Cell observedCell = observation.board[index(col, row)];
            if (classicCell == observedCell) {
                if (classicCell == Cell::Empty) ++result.exactEmptyCells;
                else ++result.exactOccupiedCells;
            } else if (isOccupied(classicCell) && isOccupied(observedCell)) {
                ++result.occupiedColorMismatches;
            } else {
                ++result.occupancyMismatches;
            }
        }
    }

    for (const int row : targetRows) {
        int classicGarbage = 0;
        int classicEmpty = 0;
        int pieces = 0;
        for (int col = 0; col < BoardWidth; ++col) {
            const Cell classicCell = classic[index(col, row)];
            if (classicCell == Cell::Garbage) ++classicGarbage;
            if (classicCell == Cell::Empty) ++classicEmpty;
            if (isPiece(observation.board[index(col, row)])) ++pieces;
        }
        result.garbagePieceCells += pieces;
        if (classicGarbage < 7) continue;
        if (pieces == 0) ++result.cleanGarbageRows;
        if (classicGarbage == 9 && classicEmpty == 1 && pieces == 0) {
            ++result.exactGarbageRows;
        }
    }
    return result;
}

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

    boardImageWidth_ = std::max(10, static_cast<int>(std::floor(layout_.board.w)));
    boardImageHeight_ = std::max(20, static_cast<int>(std::floor(layout_.board.h)));
    if (model_ != nullptr) {
        const std::size_t pixelCount = static_cast<std::size_t>(boardImageWidth_) * boardImageHeight_;
        boardSampleOffsets_.resize(pixelCount);
        boardSampleFractions_.resize(pixelCount);
        for (int y = 0; y < boardImageHeight_; ++y) {
            for (int x = 0; x < boardImageWidth_; ++x) {
                const double sourceX = crop_.x + layout_.board.x * crop_.scale + (x + .5) * crop_.scale - .5;
                const double sourceY = crop_.y + layout_.board.y * crop_.scale + (y + .5) * crop_.scale - .5;
                const double clampedX = std::clamp(sourceX, 0.0, static_cast<double>(frameWidth_ - 1));
                const double clampedY = std::clamp(sourceY, 0.0, static_cast<double>(frameHeight_ - 1));
                const int x0 = static_cast<int>(std::floor(clampedX));
                const int y0 = static_cast<int>(std::floor(clampedY));
                const int x1 = std::min(frameWidth_ - 1, x0 + 1);
                const int y1 = std::min(frameHeight_ - 1, y0 + 1);
                const std::size_t i = static_cast<std::size_t>(y) * boardImageWidth_ + x;
                boardSampleOffsets_[i] = {
                    (static_cast<std::size_t>(y0) * frameWidth_ + x0) * 4,
                    (static_cast<std::size_t>(y0) * frameWidth_ + x1) * 4,
                    (static_cast<std::size_t>(y1) * frameWidth_ + x0) * 4,
                    (static_cast<std::size_t>(y1) * frameWidth_ + x1) * 4,
                };
                boardSampleFractions_[i] = {clampedX - x0, clampedY - y0};
            }
        }
        int minX = frameWidth_;
        int minY = frameHeight_;
        int maxX = -1;
        int maxY = -1;
        for (const auto& offsets : boardSampleOffsets_) {
            for (const std::size_t offset : offsets) {
                const std::size_t pixel = offset / 4;
                const int x = static_cast<int>(pixel % static_cast<std::size_t>(frameWidth_));
                const int y = static_cast<int>(pixel / static_cast<std::size_t>(frameWidth_));
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
        }
        if (maxX >= minX && maxY >= minY) {
            retainedBoardX_ = minX;
            retainedBoardY_ = minY;
            retainedBoardWidth_ = maxX - minX + 1;
            retainedBoardHeight_ = maxY - minY + 1;
            retainedBoardSampleOffsets_.resize(boardSampleOffsets_.size());
            for (std::size_t i = 0; i < boardSampleOffsets_.size(); ++i) {
                for (std::size_t corner = 0; corner < 4; ++corner) {
                    const std::size_t pixel = boardSampleOffsets_[i][corner] / 4;
                    const int x = static_cast<int>(pixel % static_cast<std::size_t>(frameWidth_));
                    const int y = static_cast<int>(pixel / static_cast<std::size_t>(frameWidth_));
                    retainedBoardSampleOffsets_[i][corner] =
                        (static_cast<std::size_t>(y - retainedBoardY_) * retainedBoardWidth_ +
                         static_cast<std::size_t>(x - retainedBoardX_)) * 4;
                }
            }
        }
    }

    const double queueScale = crop_.scale * (1920.0 / layout_.queueCoordinateWidth);
    const double radius = layout_.queueSampleRadius * queueScale;
    const std::array<std::array<double, 2>, 6> coordinates{{
        layout_.hold, layout_.next[0], layout_.next[1], layout_.next[2], layout_.next[3], layout_.next[4]
    }};
    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        const double cx = crop_.x + coordinates[i][0] * queueScale;
        const double cy = crop_.y + coordinates[i][1] * queueScale;
        const int startX = std::max(0, static_cast<int>(std::floor(cx - radius)));
        const int startY = std::max(0, static_cast<int>(std::floor(cy - radius)));
        const int diameter = static_cast<int>(std::ceil(radius * 2.0));
        const int endX = std::min(frameWidth_, startX + diameter);
        const int endY = std::min(frameHeight_, startY + diameter);
        auto& offsets = queueSampleOffsets_[i];
        offsets.reserve(static_cast<std::size_t>(std::max(0, endX - startX)) * std::max(0, endY - startY));
        const double radiusSq = radius * radius;
        for (int y = startY; y < endY; ++y) {
            for (int x = startX; x < endX; ++x) {
                const double dx = x - cx;
                const double dy = y - cy;
                if (dx * dx + dy * dy <= radiusSq) {
                    offsets.push_back((static_cast<std::size_t>(y) * frameWidth_ + x) * 4);
                }
            }
        }
    }

#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
    const std::array<LayoutRect, 6> cnnRectangles{{
        layout_.queueCnnHold,
        layout_.queueCnnNext[0], layout_.queueCnnNext[1], layout_.queueCnnNext[2],
        layout_.queueCnnNext[3], layout_.queueCnnNext[4]
    }};
    const double contentWidth = crop_.scale * 1920.0;
    const double contentHeight = contentWidth * 9.0 / 16.0;
    for (std::size_t slot = 0; slot < cnnRectangles.size(); ++slot) {
        const auto& rectangle = cnnRectangles[slot];
        auto& mapped = queueCnnCrops_[slot];
        mapped.x0 = std::clamp(static_cast<int>(std::nearbyint(
            crop_.x + rectangle.x * contentWidth / layout_.queueCnnReferenceWidth)), 0, frameWidth_);
        mapped.y0 = std::clamp(static_cast<int>(std::nearbyint(
            crop_.y + rectangle.y * contentHeight / layout_.queueCnnReferenceHeight)), 0, frameHeight_);
        mapped.x1 = std::clamp(static_cast<int>(std::nearbyint(
            crop_.x + (rectangle.x + rectangle.w) * contentWidth / layout_.queueCnnReferenceWidth)), 0, frameWidth_);
        mapped.y1 = std::clamp(static_cast<int>(std::nearbyint(
            crop_.y + (rectangle.y + rectangle.h) * contentHeight / layout_.queueCnnReferenceHeight)), 0, frameHeight_);
        const int sourceWidth = mapped.x1 - mapped.x0;
        const int sourceHeight = mapped.y1 - mapped.y0;
        if (sourceWidth < 1 || sourceHeight < 1) continue;
        int resizedWidth = 48;
        int resizedHeight = 48;
        if (sourceWidth > sourceHeight) {
            resizedHeight = std::max(1, static_cast<int>(std::nearbyint(
                static_cast<double>(sourceHeight) / sourceWidth * 48.0)));
        } else if (sourceHeight > sourceWidth) {
            resizedWidth = std::max(1, static_cast<int>(std::nearbyint(
                static_cast<double>(sourceWidth) / sourceHeight * 48.0)));
        }
        const int padX = static_cast<int>(std::nearbyint((48 - resizedWidth) * .5));
        const int padY = static_cast<int>(std::nearbyint((48 - resizedHeight) * .5));
        mapped.pixels.reserve(static_cast<std::size_t>(resizedWidth) * resizedHeight);
        for (int y = 0; y < resizedHeight; ++y) {
            for (int x = 0; x < resizedWidth; ++x) {
                const double sourceX = mapped.x0 + (x + .5) * sourceWidth / resizedWidth - .5;
                const double sourceY = mapped.y0 + (y + .5) * sourceHeight / resizedHeight - .5;
                const double clampedX = std::clamp(sourceX, static_cast<double>(mapped.x0),
                                                   static_cast<double>(mapped.x1 - 1));
                const double clampedY = std::clamp(sourceY, static_cast<double>(mapped.y0),
                                                   static_cast<double>(mapped.y1 - 1));
                const int x0 = static_cast<int>(std::floor(clampedX));
                const int y0 = static_cast<int>(std::floor(clampedY));
                const int x1 = std::min(mapped.x1 - 1, x0 + 1);
                const int y1 = std::min(mapped.y1 - 1, y0 + 1);
                mapped.pixels.push_back({{
                    (static_cast<std::size_t>(y0) * frameWidth_ + x0) * 4,
                    (static_cast<std::size_t>(y0) * frameWidth_ + x1) * 4,
                    (static_cast<std::size_t>(y1) * frameWidth_ + x0) * 4,
                    (static_cast<std::size_t>(y1) * frameWidth_ + x1) * 4,
                }, clampedX - x0, clampedY - y0,
                static_cast<std::uint8_t>(padX + x), static_cast<std::uint8_t>(padY + y)});
            }
        }
    }
#endif
}

QueueObservation stabilizeQueueCnnObservation(
    QueueObservation observation,
    std::optional<QueueObservation>& trusted) {
    if (!observation.cnnUsed) return observation;

    const bool nextUnclear = std::any_of(
        observation.nextUnclear.begin(), observation.nextUnclear.end(),
        [](bool value) { return value; });
    const bool fullQueue = observation.next.size() >= 5 &&
        std::all_of(observation.next.begin(), observation.next.begin() + 5,
                    [](Cell value) { return isPiece(value); });

    QueueObservation accepted = trusted.value_or(QueueObservation{});
    accepted.cnnUsed = true;
    if (!observation.holdUnclear) accepted.hold = observation.hold;
    if (!nextUnclear && fullQueue) accepted.next = observation.next;

    // Preserve current-frame diagnostics even when its categorical value is
    // suppressed. The queue correction code sees only stabilized values.
    accepted.colors = std::move(observation.colors);
    accepted.holdUnclear = observation.holdUnclear;
    accepted.nextUnclear = observation.nextUnclear;
    accepted.holdPieceConfidence = observation.holdPieceConfidence;
    accepted.nextPieceConfidence = observation.nextPieceConfidence;
    accepted.holdUnclearProbability = observation.holdUnclearProbability;
    accepted.nextUnclearProbability = observation.nextUnclearProbability;

    if (!trusted) trusted = accepted;
    else {
        if (!observation.holdUnclear) trusted->hold = observation.hold;
        if (!nextUnclear && fullQueue) trusted->next = observation.next;
    }
    return accepted;
}

QueueObservation VisionAnalyzer::observeQueue(const Frame& frame) const {
    QueueObservation result;
    const bool cachedGeometryMatches = frame.width == frameWidth_ && frame.height == frameHeight_;
    const double fallbackScale = crop_.scale * (1920.0 / layout_.queueCoordinateWidth);
    const double fallbackRadius = layout_.queueSampleRadius * fallbackScale;
    const std::array<std::array<double, 2>, 6> coordinates{{
        layout_.hold, layout_.next[0], layout_.next[1], layout_.next[2], layout_.next[3], layout_.next[4]
    }};

    for (std::size_t i = 0; i < coordinates.size(); ++i) {
        Rgb color;
        if (cachedGeometryMatches) {
            double totalR = 0, totalG = 0, totalB = 0, count = 0;
            for (const std::size_t offset : queueSampleOffsets_[i]) {
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
            if (count != 0) color = {totalR / count, totalG / count, totalB / count};
        } else {
            const auto& coordinate = coordinates[i];
            color = averageColorNonBlack(frame,
                crop_.x + coordinate[0] * fallbackScale,
                crop_.y + coordinate[1] * fallbackScale,
                fallbackRadius);
        }
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

#if !defined(__EMSCRIPTEN__) || defined(TR_BROWSER_FULL)
QueueObservation VisionAnalyzer::observeQueueFromColorStats(
    const std::array<std::array<std::uint64_t, 4>, 6>& stats) const {
    QueueObservation result;
    for (std::size_t i = 0; i < stats.size(); ++i) {
        const double count = static_cast<double>(stats[i][3]);
        Rgb color;
        if (count != 0) {
            color = {
                static_cast<double>(stats[i][0]) / count,
                static_cast<double>(stats[i][1]) / count,
                static_cast<double>(stats[i][2]) / count,
            };
        }
        result.colors.push_back({
            static_cast<std::uint8_t>(std::clamp(std::lround(color.r), 0l, 255l)),
            static_cast<std::uint8_t>(std::clamp(std::lround(color.g), 0l, 255l)),
            static_cast<std::uint8_t>(std::clamp(std::lround(color.b), 0l, 255l)),
        });

        const bool black = color.r < 50 && color.g < 50 && color.b < 50;
        if (i == 0) {
            if (!black) result.hold = findClosestMinoOnly(color);
        } else {
            if (i == 1 && black) break;
            result.next.push_back(findClosestMinoOnly(color));
        }
    }
    return result;
}

void VisionAnalyzer::appendQueueCnnInput(const Frame& frame, std::vector<float>& batch,
                                         std::vector<double>& darkRatios,
                                         int darkPixelLimit) const {
    constexpr std::size_t Plane = 48 * 48;
    constexpr std::size_t ValuesPerCrop = Plane * 3;
    const std::size_t base = batch.size();
    batch.resize(base + queueCnnCrops_.size() * ValuesPerCrop, -1.0f);
    const bool directFullFrame = frame.sourceX == 0 && frame.sourceY == 0 &&
        frame.width == frameWidth_ && frame.height == frameHeight_;
    constexpr std::size_t MissingOffset = std::numeric_limits<std::size_t>::max();
    const auto localOffset = [&](std::size_t fullOffset) -> std::size_t {
        if (directFullFrame) return fullOffset;
        const std::size_t fullPixel = fullOffset / 4;
        const int fullX = static_cast<int>(fullPixel % static_cast<std::size_t>(frameWidth_));
        const int fullY = static_cast<int>(fullPixel / static_cast<std::size_t>(frameWidth_));
        const int localX = fullX - frame.sourceX;
        const int localY = fullY - frame.sourceY;
        if (localX < 0 || localX >= frame.width || localY < 0 || localY >= frame.height) {
            return MissingOffset;
        }
        return (static_cast<std::size_t>(localY) * frame.width + localX) * 4;
    };
    for (std::size_t slot = 0; slot < queueCnnCrops_.size(); ++slot) {
        const auto& crop = queueCnnCrops_[slot];
        const std::size_t cropBase = base + slot * ValuesPerCrop;
        for (const auto& sample : crop.pixels) {
            // A regional D3D11 readback has a non-zero source origin. Resolve
            // its four bilinear addresses once per output pixel, rather than
            // repeating division/modulo for R, G and B. Pixel values and the
            // floating-point blend order remain identical to the full-frame
            // path.
            std::array<std::size_t, 4> offsets{};
            for (std::size_t corner = 0; corner < offsets.size(); ++corner) {
                offsets[corner] = localOffset(sample.offsets[corner]);
            }
            const auto channelAt = [&](std::size_t offset, int channel) -> double {
                return offset == MissingOffset ? 0.0 : frame.bgra[offset + channel];
            };
            const auto blend = [&](int channel) {
                const double top = channelAt(offsets[0], channel) * (1.0 - sample.fx) +
                                   channelAt(offsets[1], channel) * sample.fx;
                const double bottom = channelAt(offsets[2], channel) * (1.0 - sample.fx) +
                                      channelAt(offsets[3], channel) * sample.fx;
                return top * (1.0 - sample.fy) + bottom * sample.fy;
            };
            const std::size_t pixel = static_cast<std::size_t>(sample.y) * 48 + sample.x;
            batch[cropBase + pixel] = static_cast<float>(blend(2) / 127.5 - 1.0);
            batch[cropBase + Plane + pixel] = static_cast<float>(blend(1) / 127.5 - 1.0);
            batch[cropBase + Plane * 2 + pixel] = static_cast<float>(blend(0) / 127.5 - 1.0);
        }
        std::size_t dark = 0;
        std::size_t count = 0;
        const int localX0 = std::max(0, crop.x0 - frame.sourceX);
        const int localY0 = std::max(0, crop.y0 - frame.sourceY);
        const int localX1 = std::min(frame.width, crop.x1 - frame.sourceX);
        const int localY1 = std::min(frame.height, crop.y1 - frame.sourceY);
        if (localX1 > localX0 && localY1 > localY0) {
            for (int y = localY0; y < localY1; ++y) {
                for (int x = localX0; x < localX1; ++x) {
                    const std::size_t offset = (static_cast<std::size_t>(y) * frame.width + x) * 4;
                    const int maximum = std::max({frame.bgra[offset], frame.bgra[offset + 1],
                                                  frame.bgra[offset + 2]});
                    if (maximum <= darkPixelLimit) ++dark;
                    ++count;
                }
            }
        }
        darkRatios.push_back(count ? static_cast<double>(dark) / count : 0.0);
    }
    // TR_QUEUE_DUMP: モデルに渡す直前の入力をそのまま保存する（検証用）
    if (const char* dumpDir = std::getenv("TR_QUEUE_DUMP")) {
        static std::atomic<int> dumpCounter{0};
        const int index = dumpCounter.fetch_add(1);
        if (index < 8) {
            const std::size_t crops = queueCnnCrops_.size();
            const std::size_t base = batch.size() - crops * ValuesPerCrop;
            std::filesystem::create_directories(dumpDir);
            std::ofstream out(std::filesystem::path(dumpDir) / ("frame" + std::to_string(index) + ".f32"),
                              std::ios::binary);
            out.write(reinterpret_cast<const char*>(batch.data() + base),
                      static_cast<std::streamsize>(crops * ValuesPerCrop * sizeof(float)));
        }
    }
}

std::vector<PixelRegion> VisionAnalyzer::queuePixelRegions() const {
    std::vector<PixelRegion> result;
    result.reserve(queueCnnCrops_.size());
    for (std::size_t slot = 0; slot < queueCnnCrops_.size(); ++slot) {
        const auto& crop = queueCnnCrops_[slot];
        int minX = crop.x1 > crop.x0 ? crop.x0 : frameWidth_;
        int minY = crop.y1 > crop.y0 ? crop.y0 : frameHeight_;
        int maxX = crop.x1 > crop.x0 ? crop.x1 - 1 : -1;
        int maxY = crop.y1 > crop.y0 ? crop.y1 - 1 : -1;
        for (const std::size_t offset : queueSampleOffsets_[slot]) {
            const std::size_t pixel = offset / 4;
            const int x = static_cast<int>(pixel % static_cast<std::size_t>(frameWidth_));
            const int y = static_cast<int>(pixel / static_cast<std::size_t>(frameWidth_));
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
        if (maxX >= minX && maxY >= minY) {
            result.push_back({minX, minY, maxX - minX + 1, maxY - minY + 1});
        }
    }
    return result;
}

PixelRegion VisionAnalyzer::queuePixelRegion() const {
    const auto regions = queuePixelRegions();
    if (regions.empty()) return {0, 0, frameWidth_, frameHeight_};
    int minX = regions.front().x;
    int minY = regions.front().y;
    int maxX = minX + regions.front().width;
    int maxY = minY + regions.front().height;
    for (const auto& region : regions) {
        minX = std::min(minX, region.x);
        minY = std::min(minY, region.y);
        maxX = std::max(maxX, region.x + region.width);
        maxY = std::max(maxY, region.y + region.height);
    }
    return {minX, minY, maxX - minX, maxY - minY};
}

QueueObservation VisionAnalyzer::applyQueueCnn(
    const QueueObservation& legacy,
    const std::vector<QueueCnnPrediction>& predictions,
    std::size_t predictionOffset,
    const std::vector<double>& darkRatios,
    std::size_t darkRatioOffset,
    const Settings& settings,
    const std::vector<QueueCnnPrediction>* labelPredictions) const {
    QueueObservation result = legacy;
    if (predictions.size() < predictionOffset + 6 || darkRatios.size() < darkRatioOffset + 6) return result;
    const bool useLabels = labelPredictions != nullptr &&
        labelPredictions->size() >= predictionOffset + 6;
    const auto pieceOf = [&](std::size_t slot) {
        return useLabels ? (*labelPredictions)[predictionOffset + slot].piece
                         : predictions[predictionOffset + slot].piece;
    };
    result.cnnUsed = true;
    result.next.resize(5, Cell::Empty);
    const auto byteProbability = [](float value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(value * 255.0f), 0l, 255l));
    };
    for (std::size_t slot = 0; slot < 6; ++slot) {
        const auto& prediction = predictions[predictionOffset + slot];
        const bool invalidPiece = slot != 0 && !isPiece(prediction.piece);
        const bool modelUnclear = prediction.unclearProbability >= settings.queueCnnUnclearThreshold;
        const bool lowPieceConfidence =
            prediction.pieceConfidence < settings.queueCnnPieceConfidenceThreshold;
        const bool darkUnclear = darkRatios[darkRatioOffset + slot] < settings.queueCnnDarkRatioThreshold;
        const bool unclear = invalidPiece || modelUnclear || lowPieceConfidence || darkUnclear;
        if (slot == 0) {
            result.holdPieceConfidence = byteProbability(prediction.pieceConfidence);
            result.holdUnclearProbability = byteProbability(prediction.unclearProbability);
            result.holdUnclear = unclear;
            // Keep the CNN guess here for diagnostics. The caller replaces
            // an unclear frame with the previous trusted CNN state before it
            // reaches queue/HOLD correction; legacy colour is never mixed in.
            result.hold = pieceOf(slot);
            continue;
        }
        const std::size_t index = slot - 1;
        result.nextPieceConfidence[index] = byteProbability(prediction.pieceConfidence);
        result.nextUnclearProbability[index] = byteProbability(prediction.unclearProbability);
        result.nextUnclear[index] = unclear;
        // Keep the raw CNN guess for diagnostics. The recognition adapter
        // suppresses the entire NEXT frame if any one of its slots is unclear.
        result.next[index] = pieceOf(slot);
    }
    return result;
}

#endif

std::vector<float> VisionAnalyzer::extractBoardFeatures(const Frame& frame) const {
#if defined(__EMSCRIPTEN__) && !defined(TR_BROWSER_FULL)
    return featuresFromBoardImage(makeBoardImage(frame, layout_.board, crop_.x, crop_.y, crop_.scale,
                                                 frameWidth_, frameHeight_, boardImageWidth_, boardImageHeight_,
                                                 boardSampleOffsets_, boardSampleFractions_));
#else
    return cnnCellsFromBoardImage(makeBoardImage(frame, layout_.board, crop_.x, crop_.y, crop_.scale,
                                                 frameWidth_, frameHeight_, boardImageWidth_, boardImageHeight_,
                                                 boardSampleOffsets_, boardSampleFractions_));
#endif
}

std::vector<float> VisionAnalyzer::boardFeatures(const Frame& frame) const {
    return extractBoardFeatures(frame);
}

BoardAnalysisInput VisionAnalyzer::boardAnalysisInput(const Frame& frame,
                                                      double horizontalShiftPixels) const {
    const double cropX = crop_.x - horizontalShiftPixels;
    const BoardImage image = horizontalShiftPixels == 0
        ? makeBoardImage(frame, layout_.board, cropX, crop_.y, crop_.scale,
                         frameWidth_, frameHeight_, boardImageWidth_, boardImageHeight_,
                         boardSampleOffsets_, boardSampleFractions_)
        : makeBoardImageUncached(frame, layout_.board, cropX, crop_.y, crop_.scale);
#if defined(__EMSCRIPTEN__) && !defined(TR_BROWSER_FULL)
    return {featuresFromBoardImage(image), classicBoardFromImage(image)};
#else
    return {cnnCellsFromBoardImage(image), classicBoardFromImage(image)};
#endif
}

VisibleBoard VisionAnalyzer::classicBoard(const Frame& frame, double horizontalShiftPixels) const {
    const double cropX = crop_.x - horizontalShiftPixels;
    const BoardImage image = horizontalShiftPixels == 0
        ? makeBoardImage(frame, layout_.board, cropX, crop_.y, crop_.scale,
                         frameWidth_, frameHeight_, boardImageWidth_, boardImageHeight_,
                         boardSampleOffsets_, boardSampleFractions_)
        : makeBoardImageUncached(frame, layout_.board, cropX, crop_.y, crop_.scale);
    return classicBoardFromImage(image);
}

#if defined(__EMSCRIPTEN__) && defined(TR_BROWSER_FULL)
BoardObservation VisionAnalyzer::analyzeBrowserGpuBoard(const Frame& frame) const {
    BrowserGpuPreprocessingScope scope;
    return analyzeBoard(frame);
}
BoardObservation VisionAnalyzer::analyzeBrowserGpuRetainedBoard(const Frame& frame) const {
    BrowserGpuPreprocessingScope scope;
    return analyzeRetainedBoard(frame);
}
#endif

BoardObservation VisionAnalyzer::analyzeBoard(const Frame& frame) const {
    BoardObservation result;
    result.timeSeconds = static_cast<double>(frame.time100ns) / 10000000.0;
    if (!model_ || !model_->ready()) {
        result.recognitionError = "ONNX model is not available";
        return result;
    }

    if (frame.sourceX != 0 || frame.sourceY != 0 ||
        frame.width != frameWidth_ || frame.height != frameHeight_) {
        Frame retained;
        retainBoardFrame(frame, retained);
        if (retained.width != retainedBoardWidth_ || retained.height != retainedBoardHeight_) {
            result.recognitionError = "GPU board ROI does not cover the required source pixels";
            return result;
        }
        return analyzeRetainedBoard(retained);
    }

    // The CNN was trained with horizontal displacement, so inference uses the
    // detected frame exactly as-is. Dynamic crop searching would change the
    // distribution the model learned and would multiply ONNX work per frame.
    thread_local BoardImage image;
    makeBoardImage(frame, layout_.board, crop_.x, crop_.y, crop_.scale,
                   frameWidth_, frameHeight_, boardImageWidth_, boardImageHeight_,
                   boardSampleOffsets_, boardSampleFractions_, image);
    // 盤面画像のうち、白飛び（全チャンネルが明るい）画素の割合を測る。
    // ライン消去のアニメーション中はこれが高くなる。
    {
        const std::uint8_t* pixels = reinterpret_cast<const std::uint8_t*>(image.pixels.data());
        const std::size_t count = static_cast<std::size_t>(boardImageWidth_) *
                                  static_cast<std::size_t>(boardImageHeight_);
        std::size_t bright = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t r = pixels[i * 3 + 0];
            const std::uint8_t g = pixels[i * 3 + 1];
            const std::uint8_t b = pixels[i * 3 + 2];
            if (r > 200 && g > 200 && b > 200) ++bright;
        }
        result.effectRatio = count ? static_cast<float>(bright) / static_cast<float>(count) : 0.0f;
        if (const char* dbg = std::getenv("TR_EFFECT_DEBUG")) {
            if (std::string(dbg) != "0") {
                static std::atomic<std::size_t> counter{0};
                const std::size_t n = counter.fetch_add(1);
                if (n < 400) {
                    std::fprintf(stderr, "effect %zu ratio=%.4f\n", n, result.effectRatio);
                }
            }
        }
    }
    thread_local std::vector<float> features;
    cnnCellsFromBoardImage(image, features);
    const VisibleBoard classic = classicBoardFromImage(image);
    std::string error;
    const BoardProbabilities probabilities = model_->inferProbabilities(features, error);
    if (!error.empty()) {
        result.recognitionError = error;
        return result;
    }
    result = analyzeBoardWithProbabilities(frame, probabilities, classic, true);
    result.timeSeconds = static_cast<double>(frame.time100ns) / 10000000.0;
    return result;
}

Frame VisionAnalyzer::retainBoardFrame(const Frame& frame) const {
    Frame retained;
    retainBoardFrame(frame, retained);
    return retained;
}

PixelRegion VisionAnalyzer::boardPixelRegion() const {
    return {retainedBoardX_, retainedBoardY_, retainedBoardWidth_, retainedBoardHeight_};
}

void VisionAnalyzer::retainBoardFrame(const Frame& frame, Frame& retained) const {
    const int localX = retainedBoardX_ - frame.sourceX;
    const int localY = retainedBoardY_ - frame.sourceY;
    const bool covered = localX >= 0 && localY >= 0 &&
        localX + retainedBoardWidth_ <= frame.width &&
        localY + retainedBoardHeight_ <= frame.height;
    if (!covered || retainedBoardWidth_ <= 0 || retainedBoardHeight_ <= 0) {
        retained = frame;
        return;
    }

    retained.width = retainedBoardWidth_;
    retained.height = retainedBoardHeight_;
    retained.sourceX = 0;
    retained.sourceY = 0;
    retained.time100ns = frame.time100ns;
    const std::size_t rowBytes = static_cast<std::size_t>(retainedBoardWidth_) * 4;
    retained.bgra.resize(rowBytes * static_cast<std::size_t>(retainedBoardHeight_));
    for (int y = 0; y < retainedBoardHeight_; ++y) {
        const std::size_t source =
            (static_cast<std::size_t>(localY + y) * frame.width + localX) * 4;
        const std::size_t destination = static_cast<std::size_t>(y) * rowBytes;
        std::copy_n(frame.bgra.begin() + static_cast<std::ptrdiff_t>(source), rowBytes,
                    retained.bgra.begin() + static_cast<std::ptrdiff_t>(destination));
    }
}

BoardObservation VisionAnalyzer::analyzeRetainedBoard(const Frame& retained) const {
    BoardObservation result;
    result.timeSeconds = static_cast<double>(retained.time100ns) / 10000000.0;
    if (!model_ || !model_->ready()) {
        result.recognitionError = "ONNX model is not available";
        return result;
    }
    if (retained.width != retainedBoardWidth_ || retained.height != retainedBoardHeight_ ||
        retainedBoardSampleOffsets_.size() != boardSampleOffsets_.size()) {
        result.recognitionError = "Retained board frame geometry does not match";
        return result;
    }

    thread_local BoardImage image;
    makeBoardImage(retained, layout_.board, 0, 0, 1,
                   retainedBoardWidth_, retainedBoardHeight_,
                   boardImageWidth_, boardImageHeight_, retainedBoardSampleOffsets_,
                   boardSampleFractions_, image);
    thread_local std::vector<float> features;
    cnnCellsFromBoardImage(image, features);
    const VisibleBoard classic = classicBoardFromImage(image);
    std::string error;
    const BoardProbabilities probabilities = model_->inferProbabilities(features, error);
    if (!error.empty()) {
        result.recognitionError = error;
        return result;
    }
    result = analyzeBoardWithProbabilities(retained, probabilities, classic, true);
    result.timeSeconds = static_cast<double>(retained.time100ns) / 10000000.0;
    // 保持フレーム経路でも白飛び率を測る（ライン消去中のサンプル除外に使う）。
    {
        const std::uint8_t* pixels = reinterpret_cast<const std::uint8_t*>(image.pixels.data());
        const std::size_t count = static_cast<std::size_t>(boardImageWidth_) *
                                  static_cast<std::size_t>(boardImageHeight_);
        std::size_t bright = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t r = pixels[i * 3 + 0];
            const std::uint8_t g = pixels[i * 3 + 1];
            const std::uint8_t b = pixels[i * 3 + 2];
            if (r > 200 && g > 200 && b > 200) ++bright;
        }
        result.effectRatio = count ? static_cast<float>(bright) / static_cast<float>(count) : 0.0f;
        if (const char* dbg = std::getenv("TR_EFFECT_DEBUG")) {
            if (std::string(dbg) != "0") {
                static std::atomic<std::size_t> counter2{0};
                const std::size_t n = counter2.fetch_add(1);
                if (n < 400) std::fprintf(stderr, "effectR %zu ratio=%.4f\n", n, result.effectRatio);
            }
        }
    }
    return result;
}

BoardObservation VisionAnalyzer::analyzeBoardWithLabels(const Frame& frame,
                                                        const std::vector<Cell>& labels,
                                                        double horizontalShiftPixels,
                                                        bool enforceLegalGarbageRows) const {
    const double cropX = crop_.x - horizontalShiftPixels;
    const BoardImage image = horizontalShiftPixels == 0
        ? makeBoardImage(frame, layout_.board, cropX, crop_.y, crop_.scale,
                         frameWidth_, frameHeight_, boardImageWidth_, boardImageHeight_,
                         boardSampleOffsets_, boardSampleFractions_)
        : makeBoardImageUncached(frame, layout_.board, cropX, crop_.y, crop_.scale);
    return analyzeBoardWithLabels(frame, labels, classicBoardFromImage(image), enforceLegalGarbageRows);
}

BoardObservation VisionAnalyzer::analyzeBoardWithLabels(const Frame& frame,
                                                        const std::vector<Cell>& labels,
                                                        const VisibleBoard& classic,
                                                        bool enforceLegalGarbageRows) const {
    BoardProbabilities probabilities{};
    for (int cell = 0; cell < BoardWidth * VisibleRows; ++cell) {
        setOneHot(probabilities, cell, cell < static_cast<int>(labels.size())
            ? labels[static_cast<std::size_t>(cell)] : Cell::Empty);
    }
    return analyzeBoardWithProbabilities(frame, probabilities, classic, enforceLegalGarbageRows);
}

BoardObservation VisionAnalyzer::analyzeBoardWithProbabilities(
    const Frame& frame, const BoardProbabilities& probabilities,
    const VisibleBoard& classic, bool enforceLegalGarbageRows) const {
    BoardObservation result;
    result.timeSeconds = static_cast<double>(frame.time100ns) / 10000000.0;
    result.probabilities = probabilities;
    for (int row = 0; row < VisibleRows; ++row) {
        for (int col = 0; col < BoardWidth; ++col) {
            const int cell = index(col, row);
            float confidence = 0;
            result.board[cell] = probabilityWinner(result.probabilities, cell, confidence);
            result.confidence[cell] = static_cast<std::uint8_t>(
                std::clamp(std::lround(confidence * 255.0f), 0l, 255l));
        }
        int classicGarbage = 0;
        int classicEmpty = 0;
        for (int col = 0; col < BoardWidth; ++col) {
            const Cell cell = classic[index(col, row)];
            if (cell == Cell::Garbage) ++classicGarbage;
            if (cell == Cell::Empty) ++classicEmpty;
        }
        // Gx9+empty is a physically legal garbage row and is much stronger
        // evidence than an isolated ONNX mino label.  In particular, do not
        // allow a transient effect to turn the hole into an O/Z/S cell.
        if (enforceLegalGarbageRows && classicGarbage == 9 && classicEmpty == 1) {
            for (int col = 0; col < BoardWidth; ++col) {
                const int cell = index(col, row);
                result.board[cell] = classic[cell];
                result.confidence[cell] = 255;
                setOneHot(result.probabilities, cell, classic[cell]);
            }
        }
    }
    double quality = 0;
    for (const std::uint8_t confidence : result.confidence) quality += confidence;
    result.quality = quality / (BoardWidth * VisibleRows * 255.0);
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
    // 白いエフェクトに覆われたサンプルは捨てる（全部該当するならそのまま使う）。
    const float EffectRejectRatio = [] {
        if (const char* v = std::getenv("TR_EFFECT_REJECT_RATIO")) {
            try {
                const double parsed = std::stod(v);
                if (parsed >= 0.0 && parsed <= 1.0) return static_cast<float>(parsed);
            } catch (...) {
            }
        }
        return 0.25f;
    }();
    std::vector<const BoardObservation*> usable;
    usable.reserve(samples.size());
    for (const BoardObservation& sample : samples) {
        if (sample.effectRatio < EffectRejectRatio) usable.push_back(&sample);
    }
    if (!usable.empty() && usable.size() < samples.size()) {
        std::vector<BoardObservation> filtered;
        filtered.reserve(usable.size());
        for (const BoardObservation* sample : usable) filtered.push_back(*sample);
        return aggregate(filtered);
    }
    result.timeSeconds = samples.back().timeSeconds;
    result.queue = samples.back().queue;
    for (int i = 0; i < BoardWidth * VisibleRows; ++i) {
        const std::size_t base = static_cast<std::size_t>(i) * CellClassCount;
        for (const BoardObservation& sample : samples) {
            float total = 0;
            for (int value = 0; value < CellClassCount; ++value) {
                total += sample.probabilities[base + static_cast<std::size_t>(value)];
            }
            if (total > .000001f) {
                for (int value = 0; value < CellClassCount; ++value) {
                    result.probabilities[base + static_cast<std::size_t>(value)] +=
                        sample.probabilities[base + static_cast<std::size_t>(value)] / total;
                }
            } else {
                result.probabilities[probabilityIndex(i, sample.board[i])] += 1.0f;
            }
        }
        for (int value = 0; value < CellClassCount; ++value) {
            result.probabilities[base + static_cast<std::size_t>(value)] /=
                static_cast<float>(samples.size());
        }
        float confidence = 0;
        result.board[i] = probabilityWinner(result.probabilities, i, confidence);
        result.confidence[i] = static_cast<std::uint8_t>(
            std::clamp(std::lround(confidence * 255.0f), 0l, 255l));
    }
    double quality = 0;
    for (const auto confidence : result.confidence) quality += confidence;
    result.quality = quality / (BoardWidth * VisibleRows * 255.0);
    return result;
}

} // namespace tr
