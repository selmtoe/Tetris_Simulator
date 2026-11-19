#include "training_import.hpp"

#include "tetris_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace tr {
namespace {

class Json {
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    Json() = default;
    explicit Json(bool value) : type_(Type::Boolean), boolean_(value) {}
    explicit Json(double value) : type_(Type::Number), number_(value) {}
    explicit Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    explicit Json(Array value) : type_(Type::Array), array_(std::move(value)) {}
    explicit Json(Object value) : type_(Type::Object), object_(std::move(value)) {}

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }
    bool boolean(bool fallback = false) const { return type_ == Type::Boolean ? boolean_ : fallback; }
    double number(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    const std::string& string() const { return string_; }
    const Array& array() const { return array_; }
    std::size_t size() const { return type_ == Type::Array ? array_.size() : 0; }

    const Json& operator[](std::size_t index) const {
        return type_ == Type::Array && index < array_.size() ? array_[index] : null();
    }
    const Json& operator[](std::string_view key) const {
        if (type_ == Type::Object) {
            const auto found = object_.find(key);
            if (found != object_.end()) return found->second;
        }
        return null();
    }
    bool has(std::string_view key) const {
        return type_ == Type::Object && object_.find(key) != object_.end();
    }

private:
    static const Json& null() { static const Json value; return value; }
    Type type_ = Type::Null;
    bool boolean_ = false;
    double number_ = 0;
    std::string string_;
    Array array_;
    Object object_;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view source) : source_(source) {
        if (source_.size() >= 3 && static_cast<unsigned char>(source_[0]) == 0xef &&
            static_cast<unsigned char>(source_[1]) == 0xbb && static_cast<unsigned char>(source_[2]) == 0xbf) {
            position_ = 3;
        }
    }

    Json parse() {
        Json result = value();
        whitespace();
        if (position_ != source_.size()) fail("Unexpected trailing JSON data");
        return result;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string(message) + " at byte " + std::to_string(position_));
    }
    void whitespace() {
        while (position_ < source_.size() &&
               (source_[position_] == ' ' || source_[position_] == '\n' ||
                source_[position_] == '\r' || source_[position_] == '\t')) ++position_;
    }
    bool consume(char expected) {
        whitespace();
        if (position_ < source_.size() && source_[position_] == expected) { ++position_; return true; }
        return false;
    }
    void literal(std::string_view expected) {
        if (source_.substr(position_, expected.size()) != expected) fail("Invalid JSON literal");
        position_ += expected.size();
    }
    static int hex(char value) {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    }
    unsigned unicodeUnit() {
        if (position_ + 4 > source_.size()) fail("Incomplete JSON Unicode escape");
        unsigned result = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = hex(source_[position_++]);
            if (digit < 0) fail("Invalid JSON Unicode escape");
            result = result * 16 + static_cast<unsigned>(digit);
        }
        return result;
    }
    static void appendUtf8(std::string& result, unsigned value) {
        if (value <= 0x7f) result.push_back(static_cast<char>(value));
        else if (value <= 0x7ff) {
            result.push_back(static_cast<char>(0xc0 | (value >> 6)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else if (value <= 0xffff) {
            result.push_back(static_cast<char>(0xe0 | (value >> 12)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (value >> 18)));
            result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
    }
    std::string string() {
        whitespace();
        if (position_ >= source_.size() || source_[position_++] != '"') fail("Expected JSON string");
        std::string result;
        while (position_ < source_.size()) {
            const unsigned char current = static_cast<unsigned char>(source_[position_++]);
            if (current == '"') return result;
            if (current < 0x20) fail("Control character in JSON string");
            if (current != '\\') { result.push_back(static_cast<char>(current)); continue; }
            if (position_ >= source_.size()) fail("Incomplete JSON escape");
            switch (source_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = unicodeUnit();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff &&
                    position_ + 6 <= source_.size() && source_[position_] == '\\' && source_[position_ + 1] == 'u') {
                    position_ += 2;
                    const unsigned low = unicodeUnit();
                    if (low < 0xdc00 || low > 0xdfff) fail("Invalid JSON surrogate pair");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                }
                appendUtf8(result, codepoint);
                break;
            }
            default: fail("Invalid JSON escape");
            }
        }
        fail("Unterminated JSON string");
    }
    Json number() {
        whitespace();
        const std::size_t begin = position_;
        if (position_ < source_.size() && source_[position_] == '-') ++position_;
        if (position_ >= source_.size()) fail("Invalid JSON number");
        if (source_[position_] == '0') ++position_;
        else {
            if (source_[position_] < '1' || source_[position_] > '9') fail("Invalid JSON number");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        if (position_ < source_.size() && source_[position_] == '.') {
            ++position_;
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9') fail("Invalid JSON fraction");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        if (position_ < source_.size() && (source_[position_] == 'e' || source_[position_] == 'E')) {
            ++position_;
            if (position_ < source_.size() && (source_[position_] == '+' || source_[position_] == '-')) ++position_;
            if (position_ >= source_.size() || source_[position_] < '0' || source_[position_] > '9') fail("Invalid JSON exponent");
            while (position_ < source_.size() && source_[position_] >= '0' && source_[position_] <= '9') ++position_;
        }
        const std::string token(source_.substr(begin, position_ - begin));
        char* end = nullptr;
        const double parsed = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(parsed)) fail("Invalid finite JSON number");
        return Json(parsed);
    }
    Json array() {
        if (!consume('[')) fail("Expected JSON array");
        Json::Array result;
        if (consume(']')) return Json(std::move(result));
        for (;;) {
            result.push_back(value());
            if (consume(']')) break;
            if (!consume(',')) fail("Expected comma in JSON array");
        }
        return Json(std::move(result));
    }
    Json object() {
        if (!consume('{')) fail("Expected JSON object");
        Json::Object result;
        if (consume('}')) return Json(std::move(result));
        for (;;) {
            const std::string key = string();
            if (!consume(':')) fail("Expected colon in JSON object");
            result.insert_or_assign(key, value());
            if (consume('}')) break;
            if (!consume(',')) fail("Expected comma in JSON object");
        }
        return Json(std::move(result));
    }
    Json value() {
        whitespace();
        if (position_ >= source_.size()) fail("Unexpected end of JSON");
        switch (source_[position_]) {
        case 'n': literal("null"); return Json();
        case 't': literal("true"); return Json(true);
        case 'f': literal("false"); return Json(false);
        case '"': return Json(string());
        case '[': return array();
        case '{': return object();
        default: return number();
        }
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

std::string text(const Json& value) {
    return value.type() == Json::Type::String ? value.string() : std::string{};
}

int integer(const Json& value, int fallback = 0) {
    const double number = value.number(fallback);
    if (!std::isfinite(number)) return fallback;
    return static_cast<int>(std::clamp(std::llround(number),
                                      static_cast<long long>(std::numeric_limits<int>::min()),
                                      static_cast<long long>(std::numeric_limits<int>::max())));
}

PlayerNameRecognition playerNameRecognition(const Json& value) {
    PlayerNameRecognition result;
    result.text = text(value["text"]);
    result.confidence = std::clamp(value["confidence"].number(), 0.0, 1.0);
    result.attempted = value["attempted"].boolean();
    result.engine = text(value["engine"]);
    result.error = text(value["error"]);
    const Json& crop = value["crop"];
    if (crop.isArray() && crop.size() >= 4) {
        result.crop = {crop[0].number(), crop[1].number(),
                       crop[2].number(), crop[3].number()};
    }
    return result;
}

Cell piece(const Json& value) {
    const std::string source = text(value);
    if (source.empty()) return Cell::Empty;
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(source.front())))) {
    case 'I': return Cell::I; case 'L': return Cell::L; case 'O': return Cell::O;
    case 'Z': return Cell::Z; case 'T': return Cell::T; case 'J': return Cell::J;
    case 'S': return Cell::S; case 'G': return Cell::Garbage; default: return Cell::Empty;
    }
}

std::vector<Cell> pieces(const Json& value) {
    std::vector<Cell> result;
    for (char character : text(value)) {
        const std::string single(1, character);
        const Cell cell = piece(Json(single));
        if (isPiece(cell)) result.push_back(cell);
    }
    return result;
}

Board boardFromText(const std::string& source) {
    Board result{};
    for (std::size_t index = 0; index < result.size() && index < source.size(); ++index) {
        result[index] = piece(Json(std::string(1, source[index])));
    }
    return result;
}

Board unpackBoard(const Json& value) {
    if (value.isArray()) {
        std::string joined;
        joined.reserve(BoardWidth * BoardHeight);
        for (const Json& row : value.array()) joined += text(row);
        if (value.size() == VisibleRows) joined.insert(0, BoardWidth * VisibleRows, '_');
        return boardFromText(joined);
    }
    const std::string packed = text(value);
    if (packed.empty()) return {};
    if (packed.size() == BoardWidth * BoardHeight && packed.find('.') == std::string::npos) return boardFromText(packed);
    const std::size_t dot = packed.find('.');
    if (dot == std::string::npos) return boardFromText(packed);
    int firstRow = 0;
    for (std::size_t index = 0; index < dot; ++index) {
        const char character = static_cast<char>(std::tolower(static_cast<unsigned char>(packed[index])));
        const int digit = character >= '0' && character <= '9' ? character - '0' :
            character >= 'a' && character <= 'z' ? character - 'a' + 10 : -1;
        if (digit < 0) { firstRow = 0; break; }
        firstRow = std::min(BoardHeight, firstRow * 36 + digit);
    }
    std::string expanded(static_cast<std::size_t>(firstRow * BoardWidth), '_');
    expanded += packed.substr(dot + 1);
    expanded.resize(BoardWidth * BoardHeight, '_');
    return boardFromText(expanded);
}

GarbageRise garbageV1(const Json& value) {
    GarbageRise result;
    result.lines = std::clamp(integer(value["lines"]), 0, VisibleRows);
    if (value["holeMasks"].isArray()) {
        for (const Json& mask : value["holeMasks"].array()) {
            if (result.holeMasks.size() >= static_cast<std::size_t>(result.lines)) break;
            result.holeMasks.push_back(static_cast<std::uint16_t>(std::clamp(integer(mask), 0, 1023)));
        }
    }
    result.matchRatio = value["matchRatio"].number();
    result.manuallySpecified = value["manual"].boolean();
    result.holesCertain = value["holesCertain"].boolean();
    return result;
}

GarbageRise garbageV2(const Json& value) {
    if (!value.isArray()) return {};
    GarbageRise result;
    result.lines = std::clamp(integer(value[0]), 0, VisibleRows);
    if (value[1].isArray()) {
        for (const Json& mask : value[1].array()) {
            if (result.holeMasks.size() >= static_cast<std::size_t>(result.lines)) break;
            result.holeMasks.push_back(static_cast<std::uint16_t>(std::clamp(integer(mask), 0, 1023)));
        }
    }
    result.matchRatio = value[2].number();
    const int flags = integer(value[3]);
    result.manuallySpecified = (flags & 1) != 0;
    result.holesCertain = (flags & 2) != 0;
    return result;
}

struct PlacementFields {
    Cell piece = Cell::Empty;
    int x = 0;
    int y = 0;
    int rotation = 0;
    int clearedLines = 0;
    bool holdUsed = false;
};

PlacementFields placementV1(const Json& value) {
    PlacementFields result;
    result.piece = piece(value["piece"]);
    result.x = integer(value["x"]);
    result.y = integer(value["y"]);
    result.rotation = integer(value["rotation"]) & 3;
    result.clearedLines = std::max(0, integer(value["clearedLines"]));
    result.holdUsed = value["holdUsed"].boolean();
    return result;
}

PlacementFields placementV2(const Json& value) {
    PlacementFields result;
    if (!value.isArray()) return result;
    result.piece = piece(value[0]);
    result.x = integer(value[1]);
    result.y = integer(value[2]);
    result.rotation = integer(value[3]) & 3;
    result.clearedLines = std::max(0, integer(value[4]));
    result.holdUsed = integer(value[5]) != 0;
    return result;
}

void applyPlacement(TimelineStep& step, const PlacementFields& placement) {
    step.placedPiece = placement.piece;
    step.placementX = placement.x;
    step.placementY = placement.y;
    step.placementRotation = placement.rotation;
    step.clearedLines = placement.clearedLines;
}

void restoreHoldProvenance(std::vector<TimelineStep>& timeline,
                           const std::vector<bool>& placementHoldUsed) {
    for (std::size_t index = 1; index < timeline.size() && index < placementHoldUsed.size(); ++index) {
        timeline[index - 1].holdUsed = placementHoldUsed[index];
    }
}

std::vector<TimelineStep> timelineV1(const Json& rows) {
    std::vector<TimelineStep> result;
    std::vector<bool> placementHoldUsed;
    if (!rows.isArray()) return result;
    result.reserve(rows.size());
    placementHoldUsed.reserve(rows.size());
    for (const Json& row : rows.array()) {
        TimelineStep step;
        step.startSeconds = std::max(0.0, row["start"].number());
        step.timeSeconds = std::max(step.startSeconds, row["time"].number(step.startSeconds));
        step.piece = piece(row["active"]);
        step.action = text(row["action"]);
        step.hold = piece(row["hold"]);
        step.next = pieces(row["next"]);
        step.observed = unpackBoard(row["observed"]);
        step.board = unpackBoard(row["board"]);
        step.fullBoard = unpackBoard(row["fullBoard"]);
        step.confidence.fill(255);
        step.garbage = garbageV1(row["garbage"]);
        const PlacementFields placement = placementV1(row["placement"]);
        applyPlacement(step, placement);
        placementHoldUsed.push_back(placement.holdUsed);
        step.score = row["score"].number();
        step.manuallyFixed = row["manual"].boolean();
        step.recoveryUnresolved = row["recoveryUnresolved"].boolean();
        step.queueManuallyFixed = row["queueManual"].boolean();
        result.push_back(std::move(step));
    }
    restoreHoldProvenance(result, placementHoldUsed);
    return result;
}

std::vector<TimelineStep> timelineV2(const Json& rows) {
    std::vector<TimelineStep> result;
    std::vector<bool> placementHoldUsed;
    if (!rows.isArray()) return result;
    result.reserve(rows.size());
    placementHoldUsed.reserve(rows.size());
    for (const Json& row : rows.array()) {
        if (!row.isArray()) throw std::runtime_error("training/v2 label row is not an array");
        TimelineStep step;
        step.startSeconds = std::max(0, integer(row[0])) / 1000.0;
        step.timeSeconds = std::max(integer(row[0]), integer(row[1])) / 1000.0;
        step.piece = piece(row[2]);
        step.action = text(row[3]);
        step.hold = piece(row[4]);
        step.next = pieces(row[5]);
        step.garbage = garbageV2(row[6]);
        const PlacementFields placement = placementV2(row[7]);
        applyPlacement(step, placement);
        placementHoldUsed.push_back(placement.holdUsed);
        step.score = row[8].number();
        const int flags = integer(row[9]);
        step.manuallyFixed = (flags & 1) != 0;
        step.queueManuallyFixed = (flags & 2) != 0;
        step.orderAutomaticallyResolved = (flags & 4) != 0;
        step.recoveryUnresolved = (flags & 8) != 0;
        step.observed = unpackBoard(row[10]);
        step.board = unpackBoard(row[11]);
        step.fullBoard = unpackBoard(row[12]);
        step.confidence.fill(255);
        result.push_back(std::move(step));
    }
    restoreHoldProvenance(result, placementHoldUsed);
    return result;
}

QueueRecognitionSample queueSampleV1(const Json& row, std::uint64_t& revision) {
    QueueRecognitionSample sample;
    sample.timeSeconds = std::max(0.0, row["time"].number());
    sample.active = piece(row["active"]);
    sample.observation.hold = piece(row["hold"]);
    sample.observation.next = pieces(row["next"]);
    sample.decoded.hold = piece(row["decodedHold"]);
    sample.decoded.next = pieces(row["decodedNext"]);
    sample.stable = row["stable"].boolean();
    sample.manuallyEdited = row["manual"].boolean();
    sample.sequenceCorrected = row["sequenceCorrected"].boolean();
    sample.holdCorrected = row["holdCorrected"].boolean();
    sample.activeCorrected = row["activeCorrected"].boolean();
    sample.observation.cnnUsed = row["queueCnn"].boolean();
    sample.observation.holdUnclear = row["holdUnclear"].boolean();
    if (row["nextUnclear"].isArray()) {
        for (std::size_t index = 0; index < sample.observation.nextUnclear.size() && index < row["nextUnclear"].size(); ++index) {
            sample.observation.nextUnclear[index] = row["nextUnclear"][index].boolean();
        }
    }
    sample.rejected = row["rejected"].boolean();
    if (sample.manuallyEdited) sample.manualRevision = ++revision;
    return sample;
}

std::vector<QueueRecognitionSample> queueV1(const Json& rows, std::uint64_t& revision) {
    std::vector<QueueRecognitionSample> result;
    if (!rows.isArray()) return result;
    result.reserve(rows.size());
    for (const Json& row : rows.array()) result.push_back(queueSampleV1(row, revision));
    return result;
}

std::vector<QueueRecognitionSample> queueV2(const Json& rows, std::uint64_t& revision) {
    std::vector<QueueRecognitionSample> result;
    if (!rows.isArray()) return result;
    result.reserve(rows.size());
    std::array<Json, 7> previous;
    long long timeMilliseconds = 0;
    for (const Json& row : rows.array()) {
        if (!row.isArray()) throw std::runtime_error("training/v2 queue row is not an array");
        timeMilliseconds += std::max(0, integer(row[0]));
        for (std::size_t index = 0; index < previous.size(); ++index) {
            if (index + 1 < row.size() && !row[index + 1].isNull()) previous[index] = row[index + 1];
        }
        QueueRecognitionSample sample;
        sample.timeSeconds = timeMilliseconds / 1000.0;
        sample.active = piece(previous[0]);
        sample.observation.hold = piece(previous[1]);
        sample.observation.next = pieces(previous[2]);
        sample.decoded.hold = piece(previous[3]);
        sample.decoded.next = pieces(previous[4]);
        const int flags = integer(previous[5]);
        const int unclear = integer(previous[6]);
        sample.stable = (flags & 1) != 0;
        sample.manuallyEdited = (flags & 2) != 0;
        sample.sequenceCorrected = (flags & 4) != 0;
        sample.holdCorrected = (flags & 8) != 0;
        sample.activeCorrected = (flags & 16) != 0;
        sample.observation.cnnUsed = (flags & 32) != 0;
        sample.observation.holdUnclear = (flags & 64) != 0;
        sample.rejected = (flags & 128) != 0;
        for (std::size_t index = 0; index < sample.observation.nextUnclear.size(); ++index) {
            sample.observation.nextUnclear[index] = (unclear & (1 << index)) != 0;
        }
        if (sample.manuallyEdited) sample.manualRevision = ++revision;
        result.push_back(std::move(sample));
    }
    return result;
}

std::vector<TimelineStep> rawTimeline(const std::vector<TimelineStep>& solved) {
    std::vector<TimelineStep> result = solved;
    for (TimelineStep& step : result) {
        step.recoveryUnresolved = false;
        if (step.manuallyFixed) continue;
        step.board = step.observed;
        step.fullBoard = step.observed;
        step.garbage = {};
        step.placedPiece = Cell::Empty;
        step.placementX = 0;
        step.placementY = 0;
        step.placementRotation = 0;
        step.clearedLines = 0;
        step.score = 0;
    }
    return result;
}

std::vector<BoardObservation> boardObservations(const std::vector<TimelineStep>& raw) {
    std::vector<BoardObservation> result;
    result.reserve(raw.size());
    for (const TimelineStep& step : raw) {
        BoardObservation observation;
        observation.timeSeconds = step.timeSeconds > step.startSeconds
            ? (step.startSeconds + step.timeSeconds) * .5 : step.startSeconds;
        observation.confidence.fill(255);
        observation.queue.hold = step.hold;
        observation.queue.next = step.next;
        observation.quality = 1;
        for (int y = 0; y < VisibleRows; ++y) for (int x = 0; x < BoardWidth; ++x) {
            observation.board[static_cast<std::size_t>(y * BoardWidth + x)] =
                step.observed[static_cast<std::size_t>((y + VisibleRows) * BoardWidth + x)];
        }
        result.push_back(std::move(observation));
    }
    return result;
}

CandidateMove importedLock(const Board& board, Cell placedPiece, int x, int y, int rotation) {
    CandidateMove result;
    result.board = board;
    result.fullBoard = board;
    result.piece = placedPiece;
    result.x = x;
    result.y = y;
    result.rotation = rotation & 3;
    const auto shape = TetrisEngine::shape(placedPiece, result.rotation);
    for (std::size_t index = 0; index < shape.cells.size(); ++index) {
        const int nx = x + shape.cells[index][0];
        const int ny = y + shape.cells[index][1];
        result.cells[index] = {nx, ny};
        ++result.cellCount;
        if (ny >= 0 && ny < BoardHeight) result.fullBoard[tr::index(nx, ny)] = placedPiece;
    }
    int write = BoardHeight - 1;
    for (int row = BoardHeight - 1; row >= 0; --row) {
        bool full = true;
        for (int column = 0; column < BoardWidth; ++column) {
            if (!isOccupied(result.fullBoard[tr::index(column, row)])) { full = false; break; }
        }
        if (full) { ++result.clearedLines; continue; }
        for (int column = 0; column < BoardWidth; ++column) {
            result.board[tr::index(column, write)] = result.fullBoard[tr::index(column, row)];
        }
        --write;
    }
    while (write >= 0) {
        for (int column = 0; column < BoardWidth; ++column) result.board[tr::index(column, write)] = Cell::Empty;
        --write;
    }
    return result;
}

int firstDiscontinuity(const std::vector<TimelineStep>& timeline) {
    const auto unresolved = std::find_if(
        timeline.begin(), timeline.end(),
        [](const TimelineStep& step) { return step.recoveryUnresolved; });
    if (unresolved != timeline.end()) {
        return static_cast<int>(unresolved - timeline.begin());
    }
    for (std::size_t index = 1; index < timeline.size(); ++index) {
        const TimelineStep& previous = timeline[index - 1];
        const TimelineStep& current = timeline[index];
        Board expected = previous.board;
        if (isPiece(current.placedPiece)) {
            const auto shape = TetrisEngine::shape(current.placedPiece, current.placementRotation);
            if (!TetrisEngine::fits(previous.board, shape, current.placementX, current.placementY)) return static_cast<int>(index);
            const CandidateMove move = importedLock(previous.board, current.placedPiece,
                current.placementX, current.placementY, current.placementRotation);
            if (move.clearedLines != current.clearedLines || move.fullBoard != current.fullBoard) return static_cast<int>(index);
            expected = move.board;
        }
        expected = TetrisEngine::applyGarbageRise(expected, current.garbage);
        if (expected != current.board) return static_cast<int>(index);
    }
    return -1;
}

std::filesystem::path jsonPath(const Json& value) {
    const std::string source = text(value);
    return source.empty() ? std::filesystem::path{} : std::filesystem::u8path(source);
}

std::filesystem::path resolveVideo(const Json& root, const std::filesystem::path& annotation) {
    const auto tryValue = [&](const Json& value, bool relativeToAnnotation) {
        std::filesystem::path candidate = jsonPath(value);
        if (candidate.empty()) return std::filesystem::path{};
        if (relativeToAnnotation && candidate.is_relative()) candidate = annotation.parent_path() / candidate;
        std::error_code error;
        if (std::filesystem::exists(candidate, error) && std::filesystem::is_regular_file(candidate, error)) return candidate;
        return std::filesystem::path{};
    };
    // Prefer the original path when it still exists: it preserves the user's
    // original output stem (1.mov -> exports/1). Portable datasets fall back
    // to their self-contained hard link/copy beside the annotation.
    if (auto candidate = tryValue(root["source"]["originalVideoPath"], false); !candidate.empty()) return candidate;
    if (auto candidate = tryValue(root["dataset"]["videoFile"], true); !candidate.empty()) return candidate;
    return tryValue(root["source"]["videoPath"], true);
}

double timelineEnd(const std::vector<TimelineStep>& timeline) {
    double result = 0;
    for (const TimelineStep& step : timeline) result = std::max(result, step.timeSeconds);
    return result;
}

} // namespace

bool loadTrainingAnnotation(const std::filesystem::path& annotationPath,
                            RecoveryOutput& output,
                            TrainingImportInfo& info,
                            std::string& error) {
    try {
        std::ifstream file(annotationPath, std::ios::binary);
        if (!file) { error = "Could not open training annotation"; return false; }
        std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (contents.empty()) { error = "Training annotation is empty"; return false; }
        const Json root = JsonParser(contents).parse();
        const std::string schema = text(root["schema"]);
        if (schema != "tetris-video-recovery.training/v1" && schema != "tetris-video-recovery.training/v2") {
            error = "Expected tetris-video-recovery.training/v1 or /v2";
            return false;
        }

        RecoveryOutput imported;
        std::uint64_t revision = 0;
        bool hasOriginalQueueLog = false;
        if (schema == "tetris-video-recovery.training/v1") {
            imported.p1 = timelineV1(root["players"]["p1"]);
            imported.p2 = timelineV1(root["players"]["p2"]);
            imported.queueObservationsP1 = queueV1(root["queueObservations"]["p1"], revision);
            imported.queueObservationsP2 = queueV1(root["queueObservations"]["p2"], revision);
        } else {
            if (text(root["labels"]["format"]) != "timeline-labels/v1" ||
                text(root["queueObservations"]["format"]) != "queue-delta/v1") {
                error = "Unsupported training/v2 label or queue encoding";
                return false;
            }
            imported.p1 = timelineV2(root["labels"]["p1"]);
            imported.p2 = timelineV2(root["labels"]["p2"]);
            imported.queueObservationsP1 = queueV2(root["queueObservations"]["p1"], revision);
            imported.queueObservationsP2 = queueV2(root["queueObservations"]["p2"], revision);
            if (text(root["originalQueueObservations"]["format"]) == "queue-delta/v1") {
                std::uint64_t originalRevision = 0;
                imported.originalQueueObservationsP1 = queueV2(
                    root["originalQueueObservations"]["p1"], originalRevision);
                imported.originalQueueObservationsP2 = queueV2(
                    root["originalQueueObservations"]["p2"], originalRevision);
                hasOriginalQueueLog = true;
            }
            const Json& counts = root["integrity"]["counts"];
            if (integer(counts["p1Labels"], -1) != static_cast<int>(imported.p1.size()) ||
                integer(counts["p2Labels"], -1) != static_cast<int>(imported.p2.size()) ||
                integer(counts["p1QueueObservations"], -1) != static_cast<int>(imported.queueObservationsP1.size()) ||
                integer(counts["p2QueueObservations"], -1) != static_cast<int>(imported.queueObservationsP2.size())) {
                error = "training/v2 integrity counts do not match the decoded data";
                return false;
            }
        }
        if (imported.p1.empty() && imported.p2.empty()) {
            error = "Training annotation contains no timeline labels";
            return false;
        }

        imported.rawP1 = rawTimeline(imported.p1);
        imported.rawP2 = rawTimeline(imported.p2);
        if (!hasOriginalQueueLog) {
            imported.originalQueueObservationsP1 = imported.queueObservationsP1;
            imported.originalQueueObservationsP2 = imported.queueObservationsP2;
        }
        imported.queueEditRevision = revision;
        imported.boardObservationsP1 = boardObservations(imported.rawP1);
        imported.boardObservationsP2 = boardObservations(imported.rawP2);
        const double timelineDuration = std::max(timelineEnd(imported.p1), timelineEnd(imported.p2));
        imported.analyzedUntilSeconds = root["session"]["analyzedUntilSeconds"].number(timelineDuration);
        imported.videoDurationSeconds = root["session"]["videoDurationSeconds"].number(
            imported.analyzedUntilSeconds);
        imported.sourceStartSeconds = std::max(
            0.0, root["session"]["sourceStartSeconds"].number(0));
        imported.sourceEndSeconds = root["session"]["sourceEndSeconds"].number(
            imported.sourceStartSeconds + imported.videoDurationSeconds);
        imported.sourceVideoDurationSeconds = root["session"]["sourceVideoDurationSeconds"].number(
            imported.sourceEndSeconds);
        imported.humanApproved = root["review"]["approvedByExplicitExport"].boolean();
        imported.p1PlayerName = playerNameRecognition(root["playerNames"]["p1"]);
        imported.p2PlayerName = playerNameRecognition(root["playerNames"]["p2"]);
        const Json& repair = root["trajectoryRepair"];
        imported.trajectoryRepair.attempted = repair["attempted"].boolean();
        imported.trajectoryRepair.adopted = repair["adopted"].boolean();
        imported.trajectoryRepair.garbageHypothesesSearched =
            repair["garbageHypothesesSearched"].boolean();
        imported.trajectoryRepair.neuralRankerUsed =
            repair["neuralRankerUsed"].boolean();
        imported.trajectoryRepair.player = integer(repair["player"]);
        imported.trajectoryRepair.firstSuspiciousPhase = static_cast<std::size_t>(
            std::max(0, integer(repair["firstSuspiciousPhase"])));
        imported.trajectoryRepair.firstSuspiciousSeconds =
            repair["firstSuspiciousSeconds"].number();
        imported.trajectoryRepair.initialBeamWidth = integer(repair["initialBeamWidth"]);
        imported.trajectoryRepair.finalBeamWidth = integer(repair["finalBeamWidth"]);
        imported.trajectoryRepair.initialSeverePhases = integer(repair["initialSeverePhases"]);
        imported.trajectoryRepair.finalSeverePhases = integer(repair["finalSeverePhases"]);
        imported.trajectoryRepair.initialMinimumMatchRatio =
            repair["initialMinimumMatchRatio"].number(1);
        imported.trajectoryRepair.finalMinimumMatchRatio =
            repair["finalMinimumMatchRatio"].number(1);
        imported.trajectoryRepair.cause = text(repair["cause"]);

        TrainingImportInfo importedInfo;
        importedInfo.annotationPath = annotationPath;
        importedInfo.suggestedVideoPath = resolveVideo(root, annotationPath);
        importedInfo.sourceSha256 = text(root["source"]["sha256"]);
        importedInfo.sourceBytes = static_cast<std::uintmax_t>(std::max(0.0, root["source"]["bytes"].number()));
        importedInfo.approvedByExplicitExport = imported.humanApproved;
        const int p1Issue = firstDiscontinuity(imported.p1);
        const int p2Issue = firstDiscontinuity(imported.p2);
        if (p1Issue >= 0 && (p2Issue < 0 || imported.p1[static_cast<std::size_t>(p1Issue)].timeSeconds <=
                                           imported.p2[static_cast<std::size_t>(p2Issue)].timeSeconds)) {
            importedInfo.firstDiscontinuityPlayer = 1;
            importedInfo.firstDiscontinuityIndex = static_cast<std::size_t>(p1Issue);
        } else if (p2Issue >= 0) {
            importedInfo.firstDiscontinuityPlayer = 2;
            importedInfo.firstDiscontinuityIndex = static_cast<std::size_t>(p2Issue);
        }

        output = std::move(imported);
        info = std::move(importedInfo);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Could not decode training annotation: ") + exception.what();
        return false;
    }
}

bool firstTrainingDiscontinuity(const RecoveryOutput& output,
                                int& player,
                                std::size_t& index) {
    const int p1Issue = firstDiscontinuity(output.p1);
    const int p2Issue = firstDiscontinuity(output.p2);
    player = 0;
    index = 0;
    if (p1Issue >= 0 && (p2Issue < 0 || output.p1[static_cast<std::size_t>(p1Issue)].timeSeconds <=
                                       output.p2[static_cast<std::size_t>(p2Issue)].timeSeconds)) {
        player = 1;
        index = static_cast<std::size_t>(p1Issue);
        return true;
    }
    if (p2Issue >= 0) {
        player = 2;
        index = static_cast<std::size_t>(p2Issue);
        return true;
    }
    return false;
}

} // namespace tr
