#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <urlmon.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Lab theme
//
// Mirrors the public Lab palette so the manager reads as the same product:
// background #f4f7f9, surfaces #ffffff, borders #d3dfe3, text #172a35, muted
// #526975 and the teal accent #087e73.  TR_LAB_THEME=0 restores the classic
// Win32 look for comparison.
// ---------------------------------------------------------------------------
namespace labtheme {
constexpr COLORREF Background = RGB(0xf4, 0xf7, 0xf9);
constexpr COLORREF Surface = RGB(0xff, 0xff, 0xff);
constexpr COLORREF SurfaceAlt = RGB(0xfb, 0xfc, 0xfd);
constexpr COLORREF Border = RGB(0xd3, 0xdf, 0xe3);
constexpr COLORREF TextColor = RGB(0x17, 0x2a, 0x35);
constexpr COLORREF Muted = RGB(0x52, 0x69, 0x75);
constexpr COLORREF Accent = RGB(0x08, 0x7e, 0x73);
constexpr COLORREF AccentActive = RGB(0x06, 0x6b, 0x62);
constexpr COLORREF AccentText = RGB(0xff, 0xff, 0xff);
constexpr COLORREF SecondaryText = RGB(0x25, 0x44, 0x4d);
constexpr COLORREF SelectionFill = RGB(0xe9, 0xf3, 0xf1);
constexpr COLORREF SelectionText = RGB(0x0b, 0x4f, 0x47);
constexpr COLORREF HoverFill = RGB(0xee, 0xf6, 0xf4);
constexpr COLORREF DisabledFill = RGB(0xf1, 0xf4, 0xf6);
constexpr int Radius = 7;

bool enabled() {
    wchar_t buffer[8]{};
    const DWORD length = GetEnvironmentVariableW(L"TR_LAB_THEME", buffer,
                                                 static_cast<DWORD>(std::size(buffer)));
    if (length == 0) return true;
    return !(length == 1 && buffer[0] == L'0');
}

HBRUSH backgroundBrush() {
    static HBRUSH brush = CreateSolidBrush(Background);
    return brush;
}
HBRUSH surfaceBrush() {
    static HBRUSH brush = CreateSolidBrush(Surface);
    return brush;
}

HFONT font() {
    static HFONT handle = [] {
        LOGFONTW log{};
        log.lfHeight = -14;
        log.lfWeight = FW_NORMAL;
        log.lfCharSet = DEFAULT_CHARSET;
        log.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(log.lfFaceName, L"Yu Gothic UI");
        return CreateFontIndirectW(&log);
    }();
    return handle;
}

void roundedRect(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    static const bool gdiplusReady = [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
    }();
    if (gdiplusReady) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::RectF box(static_cast<Gdiplus::REAL>(rect.left) + 0.5f,
                                 static_cast<Gdiplus::REAL>(rect.top) + 0.5f,
                                 static_cast<Gdiplus::REAL>(rect.right - rect.left) - 1.0f,
                                 static_cast<Gdiplus::REAL>(rect.bottom - rect.top) - 1.0f);
        Gdiplus::GraphicsPath path;
        const float diameter = static_cast<float>(radius) * 2.0f;
        path.AddArc(box.X, box.Y, diameter, diameter, 180.0f, 90.0f);
        path.AddArc(box.X + box.Width - diameter, box.Y, diameter, diameter, 270.0f, 90.0f);
        path.AddArc(box.X + box.Width - diameter, box.Y + box.Height - diameter,
                    diameter, diameter, 0.0f, 90.0f);
        path.AddArc(box.X, box.Y + box.Height - diameter, diameter, diameter, 90.0f, 90.0f);
        path.CloseFigure();
        Gdiplus::SolidBrush fillBrush(Gdiplus::Color(GetRValue(fill), GetGValue(fill),
                                                     GetBValue(fill)));
        graphics.FillPath(&fillBrush, &path);
        Gdiplus::Pen borderPen(Gdiplus::Color(GetRValue(border), GetGValue(border),
                                              GetBValue(border)), 1.0f);
        graphics.DrawPath(&borderPen, &path);
        return;
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

}  // namespace labtheme

constexpr wchar_t WindowClass[] = L"TetrisStudyManagerWindow";
constexpr UINT WM_STUDY_PROGRESS = WM_APP + 10;
constexpr UINT WM_STUDY_DONE = WM_APP + 11;
constexpr UINT WM_STUDY_AUTOSTART = WM_APP + 12;
constexpr UINT WM_STUDY_PREVIEW_DONE = WM_APP + 13;

enum ControlId {
    IdSource = 100,
    IdDownload,
    IdAnalyze,
    IdOpen,
    IdRefresh,
    IdProgress,
    IdStatus,
    IdSearch,
    IdSort,
    IdList,
    IdDetail,
    IdP1Name,
    IdP2Name,
    IdSaveNames,
    IdReview,
    IdP1Simulator,
    IdP2Simulator,
    IdBothSimulator,
    IdMarkOkay,
    IdConfirmBroken,
    IdOpenFolder,
    IdSaveAllNames,
    IdP1NamePreview,
    IdP2NamePreview,
    IdP1NamePreviewHint,
    IdP2NamePreviewHint,
    IdCheckNameImages,
    IdSelectImageGroup
};

enum class JobKind { Analysis, DownloadAndAnalysis, Refresh, Names, Export, NameImages };

// The list keeps its original row height, so a name plate is shown as a small
// strip.  The panel above the list still shows the readable size.
constexpr int NameThumbWidth = 142;
constexpr int NameThumbHeight = 11;
// Two plates side by side inside one "名前画像" column.
constexpr int NameImageColumn = 4;
constexpr int NameGroupColumns = 5;      // P1組, P2組 follow the image column
// Normalized correlation on the 142x11 luminance grid.  The same plate is a
// pixel-identical overlay graphic, so identical names stay far above this and
// different names stay far below it.
constexpr float NameImageSameThreshold = 0.985f;

struct Json {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    Array array;
    Object object;

    const Json& operator[](std::string_view key) const {
        static const Json empty;
        if (type != Type::Object) return empty;
        const auto found = object.find(key);
        return found == object.end() ? empty : found->second;
    }
    std::string text(std::string fallback = {}) const {
        return type == Type::String ? string : std::move(fallback);
    }
    double value(double fallback = 0) const {
        return type == Type::Number ? number : fallback;
    }
    bool flag(bool fallback = false) const {
        return type == Type::Boolean ? boolean : fallback;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view source) : source_(source) {
        if (source_.size() >= 3 &&
            static_cast<unsigned char>(source_[0]) == 0xef &&
            static_cast<unsigned char>(source_[1]) == 0xbb &&
            static_cast<unsigned char>(source_[2]) == 0xbf) position_ = 3;
    }

    Json parse() {
        Json result = parseValue();
        whitespace();
        if (position_ != source_.size()) fail("unexpected trailing JSON data");
        return result;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::runtime_error(std::string(message) + " at byte " +
                                 std::to_string(position_));
    }
    void whitespace() {
        while (position_ < source_.size()) {
            const char c = source_[position_];
            if (c != ' ' && c != '\r' && c != '\n' && c != '\t') break;
            ++position_;
        }
    }
    bool consume(char c) {
        whitespace();
        if (position_ < source_.size() && source_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }
    void literal(std::string_view token) {
        if (source_.substr(position_, token.size()) != token) fail("invalid JSON literal");
        position_ += token.size();
    }
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    unsigned unicodeUnit() {
        if (position_ + 4 > source_.size()) fail("incomplete unicode escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex(source_[position_++]);
            if (digit < 0) fail("invalid unicode escape");
            value = value * 16 + static_cast<unsigned>(digit);
        }
        return value;
    }
    static void appendUtf8(std::string& output, unsigned value) {
        if (value <= 0x7f) output.push_back(static_cast<char>(value));
        else if (value <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (value >> 6)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else if (value <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (value >> 12)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (value >> 18)));
            output.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (value & 0x3f)));
        }
    }
    std::string parseString() {
        whitespace();
        if (position_ >= source_.size() || source_[position_++] != '"')
            fail("expected JSON string");
        std::string result;
        while (position_ < source_.size()) {
            const unsigned char c = static_cast<unsigned char>(source_[position_++]);
            if (c == '"') return result;
            if (c < 0x20) fail("control character in JSON string");
            if (c != '\\') { result.push_back(static_cast<char>(c)); continue; }
            if (position_ >= source_.size()) fail("incomplete JSON escape");
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
                    position_ + 6 <= source_.size() && source_[position_] == '\\' &&
                    source_[position_ + 1] == 'u') {
                    position_ += 2;
                    const unsigned low = unicodeUnit();
                    if (low < 0xdc00 || low > 0xdfff) fail("invalid surrogate pair");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + low - 0xdc00;
                }
                appendUtf8(result, codepoint);
                break;
            }
            default: fail("invalid JSON escape");
            }
        }
        fail("unterminated JSON string");
    }
    Json parseNumber() {
        whitespace();
        const std::size_t begin = position_;
        if (position_ < source_.size() && source_[position_] == '-') ++position_;
        while (position_ < source_.size() && source_[position_] >= '0' &&
               source_[position_] <= '9') ++position_;
        if (position_ < source_.size() && source_[position_] == '.') {
            ++position_;
            while (position_ < source_.size() && source_[position_] >= '0' &&
                   source_[position_] <= '9') ++position_;
        }
        if (position_ < source_.size() &&
            (source_[position_] == 'e' || source_[position_] == 'E')) {
            ++position_;
            if (position_ < source_.size() &&
                (source_[position_] == '+' || source_[position_] == '-')) ++position_;
            while (position_ < source_.size() && source_[position_] >= '0' &&
                   source_[position_] <= '9') ++position_;
        }
        const std::string token(source_.substr(begin, position_ - begin));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) fail("invalid JSON number");
        Json result; result.type = Json::Type::Number; result.number = value; return result;
    }
    Json parseArray() {
        if (!consume('[')) fail("expected array");
        Json result; result.type = Json::Type::Array;
        if (consume(']')) return result;
        for (;;) {
            result.array.push_back(parseValue());
            if (consume(']')) return result;
            if (!consume(',')) fail("expected array comma");
        }
    }
    Json parseObject() {
        if (!consume('{')) fail("expected object");
        Json result; result.type = Json::Type::Object;
        if (consume('}')) return result;
        for (;;) {
            const std::string key = parseString();
            if (!consume(':')) fail("expected object colon");
            result.object.emplace(key, parseValue());
            if (consume('}')) return result;
            if (!consume(',')) fail("expected object comma");
        }
    }
    Json parseValue() {
        whitespace();
        if (position_ >= source_.size()) fail("expected JSON value");
        if (source_[position_] == '{') return parseObject();
        if (source_[position_] == '[') return parseArray();
        if (source_[position_] == '"') {
            Json result; result.type = Json::Type::String; result.string = parseString(); return result;
        }
        if (source_[position_] == 't') {
            literal("true"); Json result; result.type = Json::Type::Boolean; result.boolean = true; return result;
        }
        if (source_[position_] == 'f') {
            literal("false"); Json result; result.type = Json::Type::Boolean; return result;
        }
        if (source_[position_] == 'n') { literal("null"); return {}; }
        return parseNumber();
    }

    std::string_view source_;
    std::size_t position_ = 0;
};

struct NameCropRect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool available = false;
};

struct MatchItem {
    int key = 0;
    int index = 0;
    fs::path manifest;
    fs::path sourceVideo;
    std::wstring sourceLabel;
    std::wstring status;
    double start = 0;
    double end = 0;
    bool suspicious = false;
    bool terminalAnomaly = false;
    double minimumRatio = 1;
    std::wstring warningDetails;
    std::wstring terminalDetails;
    std::wstring error;
    fs::path autosave;
    fs::path p1Shortcut;
    fs::path p2Shortcut;
    fs::path combinedShortcut;
    std::wstring p1Name;
    std::wstring p2Name;
    NameCropRect p1NameCrop;
    NameCropRect p2NameCrop;
};

struct Decision {
    std::wstring value;
    fs::path video;
};

struct ProgressPayload {
    int percent = 0;
    std::wstring text;
};

struct PreviewPayload {
    std::uint64_t generation = 0;
    HBITMAP p1 = nullptr;
    HBITMAP p2 = nullptr;
    std::wstring p1Message;
    std::wstring p2Message;
    ~PreviewPayload() {
        if (p1) DeleteObject(p1);
        if (p2) DeleteObject(p2);
    }
};

// One name plate reduced to a small strip plus its normalized luminance, which
// is what the mechanical "same displayed name" comparison uses.
struct NameImageSlice {
    HBITMAP thumbnail = nullptr;
    std::vector<float> fingerprint;
    int group = 0;
    NameImageSlice() = default;
    NameImageSlice(const NameImageSlice&) = delete;
    NameImageSlice& operator=(const NameImageSlice&) = delete;
    NameImageSlice(NameImageSlice&& other) noexcept
        : thumbnail(other.thumbnail), fingerprint(std::move(other.fingerprint)),
          group(other.group) { other.thumbnail = nullptr; }
    NameImageSlice& operator=(NameImageSlice&& other) noexcept {
        if (this != &other) {
            if (thumbnail) DeleteObject(thumbnail);
            thumbnail = other.thumbnail;
            other.thumbnail = nullptr;
            fingerprint = std::move(other.fingerprint);
            group = other.group;
        }
        return *this;
    }
    ~NameImageSlice() { if (thumbnail) DeleteObject(thumbnail); }
    void reset() {
        if (thumbnail) DeleteObject(thumbnail);
        thumbnail = nullptr;
        fingerprint.clear();
        group = 0;
    }
};

struct NameImagePair {
    NameImageSlice p1;
    NameImageSlice p2;
};

struct NameImageRow {
    int key = 0;
    fs::path manifest;
    NameImageSlice p1;
    NameImageSlice p2;
};

struct JobResult {
    JobKind kind = JobKind::Refresh;
    int exitCode = 1;
    int matchIndex = 0;
    fs::path manifest;
    fs::path output;
    std::wstring error;
    std::wstring message;
    std::vector<NameImageRow> nameImages;
    int nameImageFailures = 0;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND source = nullptr;
    HWND download = nullptr;
    HWND analyze = nullptr;
    HWND open = nullptr;
    HWND refresh = nullptr;
    HWND progress = nullptr;
    HWND status = nullptr;
    HWND search = nullptr;
    HWND sort = nullptr;
    HWND list = nullptr;
    HWND detail = nullptr;
    HWND p1Name = nullptr;
    HWND p2Name = nullptr;
    HWND p1NamePreview = nullptr;
    HWND p2NamePreview = nullptr;
    HWND p1NamePreviewHint = nullptr;
    HWND p2NamePreviewHint = nullptr;
    HBITMAP p1NamePreviewBitmap = nullptr;
    HBITMAP p2NamePreviewBitmap = nullptr;
    std::atomic<std::uint64_t> previewGeneration{0};
    std::atomic<bool> previewWorkerBusy{false};
    bool previewRetryPending = false;
    int previewKey = -1;
    fs::path previewManifest;
    HWND saveNames = nullptr;
    HWND saveAllNames = nullptr;
    HWND review = nullptr;
    HWND p1Simulator = nullptr;
    HWND p2Simulator = nullptr;
    HWND bothSimulator = nullptr;
    HWND markOkay = nullptr;
    HWND confirmBroken = nullptr;
    HWND openFolder = nullptr;
    HWND checkNameImages = nullptr;
    HWND selectImageGroup = nullptr;
    HFONT font = nullptr;
    fs::path runtime;
    fs::path tools;
    fs::path manifest;
    fs::path project;
    fs::path sourceVideo;
    bool collection = false;
    std::wstring projectTitle;
    std::vector<MatchItem> matches;
    std::map<int, Decision> decisions;
    std::map<int, NameImagePair> nameImages;
    std::vector<std::size_t> visible;
    std::vector<int> detailSelection;
    std::atomic<bool> busy{false};
    int selectedIndex = 0;
    bool reviewerOpened = false;
    bool changingSelection = false;
    std::wstring autoAnalyze;
};

AppState app;

void populateList();
void updateDetails();

struct ListDragSelection {
    bool captured = false;
    bool dragging = false;
    bool initiallySelected = false;
    int firstRow = -1;
    int lastRow = -1;
    POINT start{};
} listDrag;

constexpr UINT_PTR ListDragTimer = 1;

void finishListDrag(HWND list) {
    listDrag = {};
    KillTimer(list, ListDragTimer);
    if (GetCapture() == list) ReleaseCapture();
}

void selectDraggedRows(HWND list, int row) {
    if (row < 0 || row >= ListView_GetItemCount(list)) return;
    app.changingSelection = true;
    const int previous = listDrag.lastRow < 0 ? listDrag.firstRow : listDrag.lastRow;
    for (int i = std::min(previous, row); i <= std::max(previous, row); ++i)
        ListView_SetItemState(list, i, LVIS_SELECTED, LVIS_SELECTED);
    ListView_SetItemState(list, -1, 0, LVIS_FOCUSED);
    ListView_SetItemState(list, row, LVIS_FOCUSED, LVIS_FOCUSED);
    app.changingSelection = false;
    listDrag.lastRow = row;
    updateDetails();
}

int dragRowAt(HWND list, POINT point) {
    const int count = ListView_GetItemCount(list);
    const int top = ListView_GetTopIndex(list);
    RECT row{};
    if (!count || !ListView_GetItemRect(list, top, &row, LVIR_BOUNDS)) return -1;
    RECT area{}; GetClientRect(list, &area);
    if (GetWindowLongPtrW(list, GWL_STYLE) & WS_HSCROLL)
        area.bottom -= GetSystemMetrics(SM_CYHSCROLL);
    point.y = std::min(point.y, std::max(0L, area.bottom - 1));
    const int height = std::max(1L, row.bottom - row.top);
    return std::clamp(top + std::max(0L, point.y - row.top) / height, 0L,
                      static_cast<LONG>(count - 1));
}

void scrollListDrag(HWND list, POINT cursor) {
    RECT area{}, header{}, row{};
    GetClientRect(list, &area);
    GetWindowRect(ListView_GetHeader(list), &header);
    MapWindowPoints(HWND_DESKTOP, list, reinterpret_cast<POINT*>(&header), 2);
    if (GetWindowLongPtrW(list, GWL_STYLE) & WS_HSCROLL)
        area.bottom -= GetSystemMetrics(SM_CYHSCROLL);
    const int direction = cursor.y < header.bottom + 20 ? -1 : cursor.y > area.bottom - 20 ? 1 : 0;
    if (direction && ListView_GetItemRect(list, ListView_GetTopIndex(list), &row, LVIR_BOUNDS)) {
        ListView_Scroll(list, 0, direction * (row.bottom - row.top));
        cursor.y = std::clamp(cursor.y, header.bottom + 1, area.bottom - 1);
        selectDraggedRows(list, dragRowAt(list, cursor));
    }
}

LRESULT CALLBACK listSelectionProcedure(HWND list, UINT message, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR subclassId, DWORD_PTR) {
    const POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if ((wParam & MK_CONTROL) && !(wParam & MK_SHIFT)) {
            LVHITTESTINFO hit{}; hit.pt = point;
            const int row = ListView_HitTest(list, &hit);
            if (row >= 0) {
                SetFocus(list);
                listDrag = {};
                listDrag.captured = true;
                listDrag.firstRow = row;
                listDrag.start = point;
                listDrag.initiallySelected = (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                ListView_SetSelectionMark(list, row);
                SetCapture(list);
                SetTimer(list, ListDragTimer, 60, nullptr);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (listDrag.captured) {
            if (!(wParam & MK_LBUTTON) || !(wParam & MK_CONTROL)) {
                finishListDrag(list);
                return 0;
            }
            if (!listDrag.dragging &&
                (std::abs(point.x - listDrag.start.x) >= GetSystemMetrics(SM_CXDRAG) ||
                 std::abs(point.y - listDrag.start.y) >= GetSystemMetrics(SM_CYDRAG)))
                listDrag.dragging = true;
            if (listDrag.dragging) selectDraggedRows(list, dragRowAt(list, point));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (listDrag.captured) {
            if (listDrag.dragging) selectDraggedRows(list, dragRowAt(list, point));
            else {
                // A stationary Ctrl-click retains native toggle semantics.
                app.changingSelection = true;
                ListView_SetItemState(list, -1, 0, LVIS_FOCUSED);
                ListView_SetItemState(list, listDrag.firstRow,
                    LVIS_FOCUSED | (listDrag.initiallySelected ? 0 : LVIS_SELECTED),
                    LVIS_FOCUSED | LVIS_SELECTED);
                app.changingSelection = false;
                updateDetails();
            }
            finishListDrag(list);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == ListDragTimer && listDrag.captured) {
            if (!(GetKeyState(VK_CONTROL) & 0x8000) || !(GetKeyState(VK_LBUTTON) & 0x8000)) {
                finishListDrag(list);
                return 0;
            }
            if (!listDrag.dragging) return 0;
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(list, &cursor);
            scrollListDrag(list, cursor);
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wParam == VK_CONTROL && listDrag.captured) finishListDrag(list);
        break;
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
        if (listDrag.captured) finishListDrag(list);
        break;
    case WM_NCDESTROY:
        finishListDrag(list);
        RemoveWindowSubclass(list, listSelectionProcedure, subclassId);
        break;
    case WM_NOTIFY: {
        // The column header paints itself with the classic gradient.  Its
        // custom-draw notification is sent to the list, so it is answered here
        // before the list view can swallow it.
        const NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (!header || header->code != NM_CUSTOMDRAW) break;
        if (header->hwndFrom != ListView_GetHeader(list)) break;
        const auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
        if (!draw) break;
        switch (draw->dwDrawStage) {
        case CDDS_PREPAINT:
            return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
            RECT cell = draw->rc;
            HBRUSH fill = CreateSolidBrush(labtheme::SurfaceAlt);
            FillRect(draw->hdc, &cell, fill);
            DeleteObject(fill);
            SetBkMode(draw->hdc, TRANSPARENT);
            SetTextColor(draw->hdc, labtheme::Muted);
            HGDIOBJ oldFont = SelectObject(draw->hdc, app.font);
            wchar_t text[128]{};
            HDITEMW item{};
            item.mask = HDI_TEXT;
            item.pszText = text;
            item.cchTextMax = static_cast<int>(std::size(text));
            Header_GetItem(header->hwndFrom, static_cast<int>(draw->dwItemSpec), &item);
            RECT textRect = cell;
            textRect.left += 8;
            DrawTextW(draw->hdc, text, -1, &textRect,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(draw->hdc, oldFont);
            HPEN pen = CreatePen(PS_SOLID, 1, labtheme::Border);
            HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
            MoveToEx(draw->hdc, cell.left, cell.bottom - 1, nullptr);
            LineTo(draw->hdc, cell.right, cell.bottom - 1);
            MoveToEx(draw->hdc, cell.right - 1, cell.top, nullptr);
            LineTo(draw->hdc, cell.right - 1, cell.bottom - 1);
            SelectObject(draw->hdc, oldPen);
            DeleteObject(pen);
            return CDRF_SKIPDEFAULT;
        }
        default:
            break;
        }
        break;
    }
    }
    return DefSubclassProc(list, message, wParam, lParam);
}

std::wstring fromUtf8(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string toUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, size)), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                      static_cast<int>(value.size()), result.data(), size,
                                      nullptr, nullptr);
    return result;
}

std::string readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream output;
    output << file.rdbuf();
    return output.str();
}

fs::path mediaDirectory(std::string_view key, const fs::path& fallback) {
    const fs::path settings = app.runtime / L"config" / L"study-storage.json";
    if (!fs::is_regular_file(settings)) return fallback;
    const Json storage = JsonParser(readFile(settings)).parse();
    const std::string configured = storage[key].text();
    if (configured.empty()) return fallback;
    const fs::path directory = fs::path(fromUtf8(configured));
    if (!directory.is_absolute())
        throw std::runtime_error("study-storage.json directories must be absolute paths");
    return directory.lexically_normal();
}

std::string jsonEscape(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char c : value) {
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 0x20) output << "\\u" << std::hex << std::setw(4)
                                 << std::setfill('0') << static_cast<int>(c) << std::dec;
            else output << static_cast<char>(c);
        }
    }
    return output.str();
}

std::wstring controlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
    if (length) GetWindowTextW(control, result.data(), length + 1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}

void setText(HWND control, const std::wstring& text) {
    SetWindowTextW(control, text.c_str());
}

std::wstring lower(std::wstring value) {
    if (!value.empty()) CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}

std::wstring trimWhitespace(std::wstring value) {
    const std::wstring_view whitespace = L" \t\r\n";
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::wstring::npos) return {};
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::wstring quoteArgument(const std::wstring& value) {
    if (value.empty()) return L"\"\"";
    if (value.find_first_of(L" \t\n\v\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    unsigned backslashes = 0;
    for (const wchar_t c : value) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslashes, L'\\');
            result.push_back(c);
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

fs::path searchExecutable(const std::wstring& name) {
    wchar_t buffer[32768]{};
    const DWORD size = SearchPathW(nullptr, name.c_str(), nullptr,
                                   static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (size > 0 && size < std::size(buffer)) return fs::path(buffer);
    return {};
}

fs::path absoluteJsonPath(const fs::path& base, const std::string& value) {
    if (value.empty()) return {};
    fs::path path = fs::u8path(value);
    if (path.is_relative()) path = base / path;
    std::error_code error;
    const fs::path normalized = fs::weakly_canonical(path, error);
    return error ? fs::absolute(path) : normalized;
}

std::wstring formatTime(double seconds) {
    const int total = static_cast<int>(std::max(0.0, seconds));
    wchar_t value[32];
    swprintf_s(value, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    return value;
}

std::wstring safeFilename(std::wstring value) {
    for (wchar_t& c : value) {
        if (std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos || c < 32)
            c = L'_';
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) value.pop_back();
    if (value.empty()) value = L"stream";
    if (value.size() > 100) value.resize(100);
    return value;
}

void postProgress(int percent, std::wstring text) {
    auto* payload = new ProgressPayload{std::clamp(percent, 0, 100), std::move(text)};
    if (!PostMessageW(app.window, WM_STUDY_PROGRESS, 0,
                      reinterpret_cast<LPARAM>(payload))) delete payload;
}

bool runCaptured(const std::wstring& command,
                 const std::function<void(const std::string&)>& onLine,
                 std::wstring& error, int& exitCode) {
    exitCode = 1;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        error = L"プロセス出力用パイプを作成できませんでした。";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    const BOOL created = CreateProcessW(
        nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, app.tools.c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (!created) {
        CloseHandle(readPipe);
        error = L"外部処理を開始できませんでした (Windows error " +
                std::to_wstring(GetLastError()) + L")";
        return false;
    }
    std::string pending;
    std::string recentOutput;
    const auto consumeLine = [&](const std::string& line) {
        onLine(line);
        if (!line.empty()) {
            recentOutput.append(line);
            recentOutput.push_back('\n');
            constexpr std::size_t MaxRecentOutput = 12 * 1024;
            if (recentOutput.size() > MaxRecentOutput)
                recentOutput.erase(0, recentOutput.size() - MaxRecentOutput);
        }
    };
    char buffer[4096];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) || read == 0) break;
        pending.append(buffer, buffer + read);
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            pending.erase(0, newline + 1);
            consumeLine(line);
        }
    }
    if (!pending.empty()) consumeLine(pending);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    exitCode = static_cast<int>(code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);
    if (exitCode != 0 && error.empty() && !recentOutput.empty()) {
        while (!recentOutput.empty() &&
               (recentOutput.back() == '\r' || recentOutput.back() == '\n'))
            recentOutput.pop_back();
        error = fromUtf8(recentOutput);
    }
    return true;
}

void setBusy(bool busy, const wchar_t* status = nullptr) {
    app.busy = busy;
    for (HWND control : {app.download, app.analyze, app.open}) {
        EnableWindow(control, !busy);
    }
    EnableWindow(app.refresh, !busy && fs::is_regular_file(app.manifest));
    updateDetails();
    if (status) setText(app.status, status);
}

void startJob(std::function<JobResult()> function) {
    if (app.busy.exchange(true)) return;
    setBusy(true);
    SendMessageW(app.progress, PBM_SETPOS, 0, 0);
    std::thread([function = std::move(function)]() mutable {
        JobResult result;
        try { result = function(); }
        catch (const std::exception& exception) {
            result.exitCode = 1;
            result.error = fromUtf8(exception.what());
        }
        auto* payload = new JobResult(std::move(result));
        if (!PostMessageW(app.window, WM_STUDY_DONE, 0,
                          reinterpret_cast<LPARAM>(payload))) delete payload;
    }).detach();
}

bool downloadFile(const wchar_t* url, const fs::path& destination) {
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) return false;
    fs::path pending = destination;
    pending += L".download";
    DeleteFileW(pending.c_str());
    const HRESULT result = URLDownloadToFileW(nullptr, url, pending.c_str(), 0, nullptr);
    if (FAILED(result)) { DeleteFileW(pending.c_str()); return false; }
    if (!MoveFileExW(pending.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(pending.c_str());
        return false;
    }
    return true;
}

fs::path ensureYtDlp(std::wstring& error) {
    const fs::path directory = app.runtime / L"tools" / L"yt-dlp";
    const fs::path managed = directory / L"yt-dlp.exe";
    const fs::path license = directory / L"LICENSE.txt";
    const fs::path notices = directory / L"THIRD_PARTY_LICENSES.txt";
    if (fs::is_regular_file(managed)) {
        if (fs::is_regular_file(license) && fs::is_regular_file(notices)) return managed;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool licenseOkay = fs::is_regular_file(license) || downloadFile(
            L"https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/LICENSE", license);
        const bool noticesOkay = fs::is_regular_file(notices) || downloadFile(
            L"https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/THIRD_PARTY_LICENSES.txt",
            notices);
        if (SUCCEEDED(com)) CoUninitialize();
        if (licenseOkay && noticesOkay) return managed;
        error = L"yt-dlp本体はありますが、配布に必要なライセンス通知を取得できませんでした。";
        return {};
    }
    const fs::path installed = searchExecutable(L"yt-dlp.exe");
    if (!installed.empty()) return installed;
    postProgress(2, L"yt-dlp公式版を初回セットアップ中…");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool executableOkay = downloadFile(
        L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe", managed);
    if (!executableOkay) {
        if (SUCCEEDED(com)) CoUninitialize();
        error = L"yt-dlp公式版を取得できませんでした。ネット接続を確認してください。";
        return {};
    }
    const bool licenseOkay = downloadFile(
        L"https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/LICENSE", license);
    const bool noticesOkay = downloadFile(
        L"https://raw.githubusercontent.com/yt-dlp/yt-dlp/master/THIRD_PARTY_LICENSES.txt",
        notices);
    if (SUCCEEDED(com)) CoUninitialize();
    if (!licenseOkay || !noticesOkay) {
        error = L"yt-dlp本体は取得しましたが、配布に必要なライセンス通知を取得できませんでした。";
        return {};
    }
    return managed;
}

fs::path ensureDeno(std::wstring& error) {
    const fs::path managedDirectory = app.runtime / L"tools" / L"deno";
    const fs::path managed = managedDirectory / L"deno.exe";
    const fs::path license = managedDirectory / L"LICENSE.md";
    if (fs::is_regular_file(managed)) {
        if (fs::is_regular_file(license)) return managed;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool licenseOkay = downloadFile(
            L"https://raw.githubusercontent.com/denoland/deno/main/LICENSE.md", license);
        if (SUCCEEDED(com)) CoUninitialize();
        if (licenseOkay) return managed;
        error = L"Deno本体はありますが、配布に必要なライセンス通知を取得できませんでした。";
        return {};
    }
    const fs::path installed = searchExecutable(L"deno.exe");
    if (!installed.empty()) return installed;

    const fs::path tar = searchExecutable(L"tar.exe");
    if (tar.empty()) {
        error = L"YouTube解析に必要なDenoを展開できません。Windows標準のtar.exeが見つかりません。";
        return {};
    }
    postProgress(3, L"YouTube用Denoを初回セットアップ中…");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const fs::path archive = managedDirectory / L"deno.zip";
    const bool archiveOkay = downloadFile(
        L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip",
        archive);
    if (!archiveOkay) {
        if (SUCCEEDED(com)) CoUninitialize();
        error = L"YouTube解析に必要なDeno公式版を取得できませんでした。ネット接続を確認してください。";
        return {};
    }
    std::wstring processError;
    int exitCode = 1;
    const std::wstring command = quoteArgument(tar.wstring()) + L" -xf " +
        quoteArgument(archive.wstring()) + L" -C " + quoteArgument(managedDirectory.wstring());
    runCaptured(command, [](const std::string&) {}, processError, exitCode);
    DeleteFileW(archive.c_str());
    if (exitCode != 0 || !fs::is_regular_file(managed)) {
        if (SUCCEEDED(com)) CoUninitialize();
        error = L"Denoの初回展開に失敗しました。";
        if (!processError.empty()) error += L"\r\n" + processError;
        return {};
    }
    const bool licenseOkay = downloadFile(
        L"https://raw.githubusercontent.com/denoland/deno/main/LICENSE.md", license);
    if (SUCCEEDED(com)) CoUninitialize();
    if (!licenseOkay) {
        error = L"Deno本体は取得しましたが、配布に必要なライセンス通知を取得できませんでした。";
        return {};
    }
    return managed;
}

JobResult analyzeVideo(const fs::path& video, JobKind kind,
                       int basePercent = 0, int spanPercent = 100) {
    JobResult result; result.kind = kind;
    const fs::path script = app.tools / L"run-ppt-stream-analysis.ps1";
    if (!fs::is_regular_file(video)) {
        result.error = L"解析する動画が見つかりません: " + video.wstring();
        return result;
    }
    if (!fs::is_regular_file(script)) {
        result.error = L"配信解析スクリプトが見つかりません。";
        return result;
    }
    std::wstring command = L"powershell.exe -NoLogo -NoProfile -STA "
        L"-ExecutionPolicy Bypass -File " + quoteArgument(script.wstring()) +
        L" -VideoPath " + quoteArgument(video.wstring()) + L" -MachineReadable";
    fs::path manifest;
    int exitCode = 1;
    std::wstring processError;
    const bool started = runCaptured(command, [&](const std::string& line) {
        if (line.rfind("study_progress=", 0) == 0) {
            const int percent = std::atoi(line.c_str() + 15);
            const std::size_t status = line.find(" status=");
            std::wstring text = status == std::string::npos
                ? L"配信を解析中…" : fromUtf8(line.substr(status + 8));
            postProgress(basePercent + percent * spanPercent / 100, std::move(text));
        } else if (line.rfind("study_manifest=", 0) == 0) {
            manifest = fs::path(fromUtf8(line.substr(15)));
        }
    }, processError, exitCode);
    result.exitCode = started ? exitCode : 1;
    result.error = processError;
    if (manifest.empty()) {
        manifest = app.runtime / L"data" / L"batches" /
                   video.stem() / L"batch-results.json";
    }
    result.manifest = manifest;
    if (result.exitCode != 0) {
        // Translate the engine's machine-readable failures into the one action
        // the user can take, instead of showing raw stderr.
        if (result.error.find(L"no_matches_detected") != std::wstring::npos) {
            result.error = L"動画は読めましたが、試合を1件も検出できませんでした。\r\n"
                           L"・配信の試合区間が検出できていない可能性があります\r\n"
                           L"・別の動画で試すか、解析ログを確認してください";
        } else if (result.error.find(L"supports only") != std::wstring::npos) {
            result.error = L"この動画の形式はまだ直読みできません。\r\n"
                           L"解析ログに記録された形式名を確認してください。\r\n"
                           L"（VP9/AV1/H.264/HEVC は直読みできます）";
        } else if (result.error.empty()) {
            result.error = L"配信解析に失敗しました。";
        }
        result.error += L"\r\nデバッグログ: " +
                        (manifest.parent_path() / L"analysis-debug.log").wstring();
    }
    return result;
}

JobResult downloadAndAnalyze(std::wstring url) {
    JobResult result; result.kind = JobKind::DownloadAndAnalysis;
    std::wstring error;
    const fs::path ytDlp = ensureYtDlp(error);
    if (ytDlp.empty()) { result.error = std::move(error); return result; }
    const fs::path deno = ensureDeno(error);
    if (deno.empty()) { result.error = std::move(error); return result; }
    const fs::path downloads = mediaDirectory("downloadsDirectory",
        app.runtime / L"data" / L"downloads");
    std::error_code filesystemError;
    fs::create_directories(downloads, filesystemError);
    if (filesystemError) {
        result.error = L"ダウンロード保存先を作成できません。";
        return result;
    }
    const fs::path resultFile = downloads /
        (L"download-result-" + std::to_wstring(GetTickCount64()) + L".txt");
    std::wstring command = quoteArgument(ytDlp.wstring()) +
        L" --ignore-config --newline --no-colors --no-playlist --windows-filenames --encoding utf-8 "
        L"--write-info-json "
        L"--progress-template " +
        quoteArgument(L"download:__YTDLP_PROGRESS__:%(progress._percent_str)s|%(progress._speed_str)s|%(progress._eta_str)s") +
        L" --print-to-file " + quoteArgument(L"video:__YTDLP_ID__:%(id)s") + L" " +
        quoteArgument(resultFile.wstring()) +
        L" --print-to-file " + quoteArgument(L"after_move:__YTDLP_FILE__:%(filepath)s") + L" " +
        quoteArgument(resultFile.wstring()) + L" --paths " + quoteArgument(downloads.wstring()) +
        L" -o " + quoteArgument(L"%(title).120B [%(id)s].%(ext)s");
    const fs::path ffmpeg = searchExecutable(L"ffmpeg.exe");
    if (!ffmpeg.empty()) {
        command += L" --ffmpeg-location " + quoteArgument(ffmpeg.parent_path().wstring()) +
                   L" --merge-output-format mp4 -f " +
                   quoteArgument(L"bestvideo[height<=1080]+bestaudio/best[height<=1080]");
    } else {
        command += L" -f " + quoteArgument(L"best[height<=1080][ext=mp4]/best[height<=1080]/best");
    }
    command += L" --js-runtimes " + quoteArgument(L"deno:" + deno.wstring());
    command += L" " + quoteArgument(url);
    postProgress(4, L"URLから動画情報を取得中…");
    int exitCode = 1;
    std::wstring processError;
    const bool started = runCaptured(command, [&](const std::string& line) {
        constexpr std::string_view progressMarker = "__YTDLP_PROGRESS__:";
        const std::size_t marker = line.find(progressMarker);
        if (marker == std::string::npos) return;
        const std::string tail = line.substr(marker + progressMarker.size());
        const double percent = std::strtod(tail.c_str(), nullptr);
        postProgress(4 + static_cast<int>(std::clamp(percent, 0.0, 100.0) * .26),
                     L"動画をダウンロード中 " + std::to_wstring(static_cast<int>(percent)) + L"%");
    }, processError, exitCode);
    if (!started || exitCode != 0) {
        result.error = processError.empty()
            ? L"yt-dlpによる動画取得に失敗しました。URL、公開状態、ネット接続を確認してください。"
            : processError;
        DeleteFileW(resultFile.c_str());
        return result;
    }
    const std::string downloadedText = readFile(resultFile);
    DeleteFileW(resultFile.c_str());
    std::string videoId;
    std::string downloadedPath;
    constexpr std::string_view IdMarker = "__YTDLP_ID__:";
    constexpr std::string_view FileMarker = "__YTDLP_FILE__:";
    std::istringstream resultLines(downloadedText);
    for (std::string line; std::getline(resultLines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(IdMarker, 0) == 0) videoId = line.substr(IdMarker.size());
        else if (line.rfind(FileMarker, 0) == 0)
            downloadedPath = line.substr(FileMarker.size());
    }
    fs::path video = fs::path(fromUtf8(downloadedPath));
    if (!fs::is_regular_file(video) && !videoId.empty()) {
        const std::wstring idMarker = L"[" + fromUtf8(videoId) + L"]";
        fs::file_time_type newestTime{};
        for (fs::directory_iterator it(downloads, fs::directory_options::skip_permission_denied,
                                       filesystemError), end;
             it != end; it.increment(filesystemError)) {
            if (filesystemError) { filesystemError.clear(); continue; }
            if (!it->is_regular_file(filesystemError) ||
                it->path().stem().wstring().find(idMarker) == std::wstring::npos) continue;
            const std::wstring extension = lower(it->path().extension().wstring());
            if (extension != L".mp4" && extension != L".mkv" && extension != L".webm" &&
                extension != L".mov" && extension != L".m4v") continue;
            const auto modified = it->last_write_time(filesystemError);
            if (!filesystemError && (video.empty() || modified > newestTime)) {
                video = it->path(); newestTime = modified;
            }
        }
    }
    if (!fs::is_regular_file(video)) {
        result.error = L"ダウンロードは完了しましたが、保存動画を特定できませんでした。";
        return result;
    }
    postProgress(30, L"動画取得完了。AI解析を開始します…");
    return analyzeVideo(video, JobKind::DownloadAndAnalysis, 30, 70);
}

JobResult refreshProject() {
    JobResult result; result.kind = JobKind::Refresh; result.manifest = app.manifest;
    const fs::path batch = app.runtime / L"bin" / L"TetrisVideoRecovery_stream.exe";
    const fs::path settings = app.runtime / L"config" / L"tetris_recover.ini";
    if (!fs::is_regular_file(app.manifest)) {
        result.error = L"先に解析プロジェクトを開いてください。"; return result;
    }
    if (!fs::is_regular_file(batch) || !fs::is_regular_file(settings)) {
        result.error = L"品質再判定に必要な解析本体または設定ファイルが見つかりません。";
        return result;
    }
    std::vector<fs::path> manifests;
    if (app.collection) {
        for (const MatchItem& item : app.matches) {
            if (fs::is_regular_file(item.manifest)) manifests.push_back(item.manifest);
        }
        std::sort(manifests.begin(), manifests.end());
        manifests.erase(std::unique(manifests.begin(), manifests.end()), manifests.end());
    } else {
        manifests.push_back(app.manifest);
    }
    if (manifests.empty()) {
        result.error = L"再判定する元解析結果が見つかりません。";
        return result;
    }
    for (std::size_t index = 0; index < manifests.size(); ++index) {
        postProgress(5 + static_cast<int>(index * 90 / manifests.size()),
            L"修正内容を反映して品質を再判定中… " +
            std::to_wstring(index + 1) + L"/" + std::to_wstring(manifests.size()));
        const std::wstring command = quoteArgument(batch.wstring()) + L" --links " +
            quoteArgument(manifests[index].wstring()) + L" " + quoteArgument(settings.wstring());
        int code = 1; std::wstring commandError;
        runCaptured(command, [](const std::string&) {}, commandError, code);
        if (code != 0) {
            result.exitCode = code;
            result.error = commandError.empty()
                ? L"品質再判定に失敗しました: " + manifests[index].wstring()
                : commandError;
            return result;
        }
    }
    result.exitCode = 0;
    postProgress(100, L"再判定完了");
    return result;
}

JobResult saveNamesForMatches(std::vector<MatchItem> items,
                              std::wstring p1, std::wstring p2) {
    JobResult result; result.kind = JobKind::Names; result.manifest = app.manifest;
    const fs::path batch = app.runtime / L"bin" / L"TetrisVideoRecovery_stream.exe";
    const fs::path settings = app.runtime / L"config" / L"tetris_recover.ini";
    p1 = trimWhitespace(std::move(p1));
    p2 = trimWhitespace(std::move(p2));
    if (items.empty()) {
        result.error = L"名前を保存する試合が選択されていません。";
        return result;
    }
    if (p1.empty() && p2.empty()) {
        result.error = L"P1またはP2の少なくとも一方へ、保存する名前を入力してください。";
        return result;
    }
    if (!fs::is_regular_file(batch) || !fs::is_regular_file(settings)) {
        result.error = L"名前保存に必要な解析本体または設定ファイルが見つかりません。";
        return result;
    }

    std::vector<int> failedMatches;
    std::wstring firstFailure;
    int saved = 0;
    for (std::size_t position = 0; position < items.size(); ++position) {
        const MatchItem& item = items[position];
        postProgress(5 + static_cast<int>(position * 75 / items.size()),
            L"プレイヤー名を保存中: 試合 " + std::to_wstring(item.index) +
            L"（" + std::to_wstring(position + 1) + L"/" +
            std::to_wstring(items.size()) + L"）");
        if (!fs::is_regular_file(item.autosave)) {
            failedMatches.push_back(item.index);
            if (firstFailure.empty()) firstFailure = L"autosaveが見つかりません";
            continue;
        }
        const std::wstring effectiveP1 = p1.empty() ? item.p1Name : p1;
        const std::wstring effectiveP2 = p2.empty() ? item.p2Name : p2;
        const std::wstring command = quoteArgument(batch.wstring()) +
            L" --set-player-names " + quoteArgument(item.autosave.wstring()) +
            L" " + quoteArgument(effectiveP1) + L" " + quoteArgument(effectiveP2);
        int code = 1;
        std::wstring error;
        runCaptured(command, [](const std::string&) {}, error, code);
        if (code == 0) {
            ++saved;
        } else {
            failedMatches.push_back(item.index);
            if (firstFailure.empty()) firstFailure = error.empty()
                ? L"解析本体が名前保存に失敗しました" : error;
        }
    }

    int linksCode = 0;
    std::wstring linksError;
    if (saved > 0) {
        postProgress(85, L"一覧とシミュレータを一度だけ更新中…");
        std::vector<fs::path> manifests;
        for (const MatchItem& item : items) {
            if (fs::is_regular_file(item.manifest)) manifests.push_back(item.manifest);
        }
        std::sort(manifests.begin(), manifests.end());
        manifests.erase(std::unique(manifests.begin(), manifests.end()), manifests.end());
        if (manifests.empty() && fs::is_regular_file(result.manifest))
            manifests.push_back(result.manifest);
        for (const fs::path& manifest : manifests) {
            const std::wstring command = quoteArgument(batch.wstring()) + L" --links " +
                quoteArgument(manifest.wstring()) + L" " + quoteArgument(settings.wstring());
            runCaptured(command, [](const std::string&) {}, linksError, linksCode);
            if (linksCode != 0) break;
        }
    }

    if (!failedMatches.empty() || linksCode != 0) {
        std::wostringstream message;
        if (!failedMatches.empty()) {
            message << L"プレイヤー名を保存できなかった試合: ";
            for (std::size_t index = 0; index < failedMatches.size(); ++index) {
                if (index) message << L", ";
                message << failedMatches[index];
            }
            message << L"\r\n成功 " << saved << L" / " << items.size() << L" 試合";
            if (!firstFailure.empty()) message << L"\r\n最初の失敗: " << firstFailure;
        }
        if (linksCode != 0) {
            if (!failedMatches.empty()) message << L"\r\n\r\n";
            message << L"一覧・シミュレータの再構築に失敗しました。";
            if (!linksError.empty()) message << L"\r\n" << linksError;
        }
        result.error = message.str();
        return result;
    }

    result.exitCode = 0;
    result.message = std::to_wstring(saved) + L"試合のプレイヤー名を保存しました";
    postProgress(100, result.message);
    return result;
}

JobResult exportBrokenMatch(MatchItem item) {
    JobResult result; result.kind = JobKind::Export; result.matchIndex = item.key;
    if (!fs::is_regular_file(item.sourceVideo)) {
        result.error = L"元配信動画が見つからないため、この試合を保存できません: " +
                       item.sourceVideo.wstring();
        return result;
    }
    if (!std::isfinite(item.start) || !std::isfinite(item.end) ||
        item.start < 0 || item.end <= item.start) {
        result.error = L"試合区間が不正なため、改善用動画を保存できません。";
        return result;
    }
    const fs::path ffmpeg = searchExecutable(L"ffmpeg.exe");
    if (ffmpeg.empty()) {
        result.error = L"試合動画の保存に必要なffmpeg.exeが見つかりません。"; return result;
    }
    const fs::path directory = mediaDirectory("improvementDirectory",
        app.runtime / L"data" / L"improvement-videos") /
        safeFilename(item.sourceVideo.stem().wstring());
    std::error_code filesystemError;
    fs::create_directories(directory, filesystemError);
    if (filesystemError) { result.error = L"改善用動画フォルダーを作成できません。"; return result; }
    wchar_t fileName[160];
    swprintf_s(fileName, L"match_%03d__%010lld-%010lld.mp4", item.index,
               static_cast<long long>(std::llround(item.start * 1000)),
               static_cast<long long>(std::llround(item.end * 1000)));
    const fs::path output = directory / fileName;
    result.output = output;
    if (!fs::is_regular_file(output)) {
        fs::path pending = output; pending += L".pending.mp4";
        DeleteFileW(pending.c_str());
        const double duration = std::max(0.001, item.end - item.start);
        std::wostringstream common;
        common << quoteArgument(ffmpeg.wstring()) << L" -hide_banner -loglevel warning -y -i "
               << quoteArgument(item.sourceVideo.wstring()) << L" -ss " << std::fixed
               << std::setprecision(6) << item.start << L" -t " << duration
               << L" -map 0:v:0 -map 0:a? -vf "
               << quoteArgument(L"crop=w=floor(min(iw\\,ih*16/9)/2)*2:h=floor(min(ih\\,iw*9/16)/2)*2:x=(iw-ow)/2:y=(ih-oh)/2");
        std::wstring error; int code = 1;
        postProgress(10, L"対象試合を前後余白なしで保存中…");
        std::wstring command = common.str() +
            L" -c:v h264_nvenc -preset p4 -cq 18 -c:a aac -b:a 192k " + quoteArgument(pending.wstring());
        runCaptured(command, [](const std::string&) {}, error, code);
        if (code != 0) {
            DeleteFileW(pending.c_str());
            postProgress(40, L"CPUエンコードで再試行中…");
            command = common.str() +
                L" -c:v libx264 -preset veryfast -crf 18 -c:a aac -b:a 192k " + quoteArgument(pending.wstring());
            runCaptured(command, [](const std::string&) {}, error, code);
        }
        if (code != 0 || !fs::is_regular_file(pending)) {
            DeleteFileW(pending.c_str());
            result.error = error.empty() ? L"対象試合の動画保存に失敗しました。" : error;
            return result;
        }
        if (!MoveFileExW(pending.c_str(), output.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(pending.c_str());
            result.error = L"保存動画を確定できませんでした。"; return result;
        }
    }
    const fs::path metadata = output.parent_path() / (output.stem().wstring() + L".json");
    std::ofstream file(metadata, std::ios::binary | std::ios::trunc);
    file << "{\"schema\":\"tetris-study.improvement-video/v1\",\"source\":\""
         << jsonEscape(item.sourceVideo.generic_u8string()) << "\",\"match\":" << item.index
         << ",\"manifest\":\"" << jsonEscape(item.manifest.generic_u8string())
         << "\",\"autosave\":\"" << jsonEscape(item.autosave.generic_u8string())
         << "\",\"p1_name\":\"" << jsonEscape(toUtf8(item.p1Name))
         << "\",\"p2_name\":\"" << jsonEscape(toUtf8(item.p2Name)) << "\""
         << ",\"start_seconds\":" << std::fixed << std::setprecision(6) << item.start
         << ",\"end_seconds\":" << item.end
         << ",\"no_padding\":true,\"centered_16_9\":true}\n";
    result.exitCode = 0;
    postProgress(100, L"改善用の試合動画を保存しました");
    return result;
}

bool parseNameCrop(const Json& player, NameCropRect& crop) {
    const Json& values = player["crop"];
    if (values.type != Json::Type::Array || values.array.size() != 4) return false;
    for (const Json& value : values.array)
        if (value.type != Json::Type::Number || !std::isfinite(value.number)) return false;
    const NameCropRect parsed{values.array[0].number, values.array[1].number,
                              values.array[2].number, values.array[3].number, true};
    if (parsed.x < 0 || parsed.y < 0 || parsed.width < 8 || parsed.height < 6 ||
        parsed.x > 16384 || parsed.y > 16384 ||
        parsed.width > 16384 || parsed.height > 16384) return false;
    crop = parsed;
    return true;
}

bool parsePlayerNames(const fs::path& autosave, std::wstring& p1, std::wstring& p2,
                      NameCropRect& p1Crop, NameCropRect& p2Crop) {
    if (!fs::is_regular_file(autosave)) return false;
    try {
        // Autosaves can be hundreds of megabytes because they contain every
        // observation. playerNames is deliberately near the beginning, so
        // read only a small prefix instead of parsing the complete autosave.
        constexpr std::size_t PrefixBytes = 64 * 1024;
        std::ifstream file(autosave, std::ios::binary);
        if (!file) return false;
        std::string prefix(PrefixBytes, '\0');
        file.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        prefix.resize(static_cast<std::size_t>(file.gcount()));

        constexpr std::string_view key = "\"playerNames\"";
        const std::size_t keyPosition = prefix.find(key);
        if (keyPosition == std::string::npos) return false;
        const std::size_t colon = prefix.find(':', keyPosition + key.size());
        if (colon == std::string::npos) return false;
        const std::size_t objectBegin = prefix.find('{', colon + 1);
        if (objectBegin == std::string::npos) return false;

        int depth = 0;
        bool inString = false;
        bool escaped = false;
        std::size_t objectEnd = std::string::npos;
        for (std::size_t at = objectBegin; at < prefix.size(); ++at) {
            const char value = prefix[at];
            if (inString) {
                if (escaped) escaped = false;
                else if (value == '\\') escaped = true;
                else if (value == '"') inString = false;
                continue;
            }
            if (value == '"') inString = true;
            else if (value == '{') ++depth;
            else if (value == '}' && --depth == 0) {
                objectEnd = at + 1;
                break;
            }
        }
        if (objectEnd == std::string::npos) return false;
        const Json names = JsonParser(std::string_view(prefix).substr(
            objectBegin, objectEnd - objectBegin)).parse();
        p1 = fromUtf8(names["p1"]["text"].text());
        p2 = fromUtf8(names["p2"]["text"].text());
        parseNameCrop(names["p1"], p1Crop);
        parseNameCrop(names["p2"], p2Crop);
        return true;
    } catch (...) { return false; }
}

void releasePreviewBitmap(HWND control, HBITMAP& current) {
    HBITMAP previous = nullptr;
    if (control) {
        previous = reinterpret_cast<HBITMAP>(SendMessageW(
            control, STM_SETIMAGE, IMAGE_BITMAP, 0));
        ShowWindow(control, SW_HIDE);
    }
    if (previous) DeleteObject(previous);
    if (current && current != previous) DeleteObject(current);
    current = nullptr;
}

void setNamePreviewPlaceholder(HWND image, HWND hint, HBITMAP& bitmap,
                               const std::wstring& text) {
    releasePreviewBitmap(image, bitmap);
    if (hint) {
        setText(hint, text);
        SetWindowPos(hint, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        ShowWindow(hint, SW_SHOW);
    }
}

// Generated names are wider than the panel, so the bitmap is scaled to the
// control width when it is cropped.  Measure it at request time instead of
// hard-coding a size that would clip on a resized window.
int namePreviewWidth(HWND control) {
    RECT area{};
    if (control && GetClientRect(control, &area)) {
        const int width = static_cast<int>(area.right - area.left);
        if (width >= 64) return std::min(width, 640);
    }
    return 320;
}

void applyNamePreview(HWND image, HWND hint, HBITMAP& current, HBITMAP bitmap,
                      const std::wstring& message) {
    if (!image || !hint) {
        if (bitmap) DeleteObject(bitmap);
        return;
    }
    if (!bitmap) {
        setNamePreviewPlaceholder(image, hint, current, message);
        return;
    }
    const HBITMAP previous = reinterpret_cast<HBITMAP>(SendMessageW(
        image, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bitmap)));
    const HBITMAP owned = current;
    current = bitmap;
    if (previous && previous != bitmap) DeleteObject(previous);
    if (owned && owned != previous && owned != bitmap) DeleteObject(owned);
    SetWindowPos(image, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    setText(hint, L"");
    ShowWindow(hint, SW_HIDE);
    ShowWindow(image, SW_SHOW);
}

constexpr int NamePreviewCleared = -2;

void clearNameCropPreviews(const std::wstring& message) {
    app.previewGeneration.fetch_add(1, std::memory_order_relaxed);
    app.previewKey = NamePreviewCleared;
    app.previewManifest.clear();
    app.previewRetryPending = false;
    setNamePreviewPlaceholder(app.p1NamePreview, app.p1NamePreviewHint,
                              app.p1NamePreviewBitmap, message);
    setNamePreviewPlaceholder(app.p2NamePreview, app.p2NamePreviewHint,
                              app.p2NamePreviewBitmap, message);
}

bool previewFileIsReady(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path, error) && !error && fs::file_size(path, error) > 128 && !error;
}

HBITMAP extractNameCropBitmap(const fs::path& ffmpeg, const fs::path& source,
                              double seconds, const NameCropRect& crop,
                              int previewWidth, const fs::path& output,
                              std::wstring& error) {
    if (!crop.available) {
        error = L"解析データに切り抜き位置がありません";
        return nullptr;
    }
    if (!fs::is_regular_file(source)) {
        error = L"元動画が見つかりません";
        return nullptr;
    }
    if (!std::isfinite(seconds) || seconds < 0) {
        error = L"画像を切り抜く時刻が不正です";
        return nullptr;
    }
    if (!previewFileIsReady(output)) {
        std::error_code filesystemError;
        fs::create_directories(output.parent_path(), filesystemError);
        if (filesystemError) {
            error = L"名前画像の一時フォルダーを作成できません";
            return nullptr;
        }
        const int x = static_cast<int>(std::lround(crop.x));
        const int y = static_cast<int>(std::lround(crop.y));
        const int width = static_cast<int>(std::lround(crop.width));
        const int height = static_cast<int>(std::lround(crop.height));
        std::wostringstream timestamp;
        timestamp << std::fixed << std::setprecision(3) << seconds;
        const std::wstring filter = L"crop=" + std::to_wstring(width) + L":" +
            std::to_wstring(height) + L":" + std::to_wstring(x) + L":" +
            std::to_wstring(y) + L",scale=" + std::to_wstring(previewWidth) +
            L":-1:flags=lanczos,unsharp=3:3:0.6";
        const std::wstring command = quoteArgument(ffmpeg.wstring()) +
            L" -hide_banner -loglevel error -nostdin -ss " + timestamp.str() +
            L" -i " + quoteArgument(source.wstring()) +
            L" -map 0:v:0 -an -sn -dn -frames:v 1 -vf " + quoteArgument(filter) +
            L" -f image2 -vcodec bmp -update 1 -y " + quoteArgument(output.wstring());
        int exitCode = 1;
        std::wstring processError;
        if (!runCaptured(command, [](const std::string&) {}, processError, exitCode) ||
            exitCode != 0 || !previewFileIsReady(output)) {
            DeleteFileW(output.c_str());
            error = processError.empty() ? L"名前部分の画像を抽出できませんでした" : processError;
            return nullptr;
        }
    }
    HBITMAP bitmap = reinterpret_cast<HBITMAP>(LoadImageW(
        nullptr, output.c_str(), IMAGE_BITMAP, 0, 0,
        LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    DeleteFileW(output.c_str());
    if (!bitmap) error = L"切り抜いた名前画像を読み込めませんでした";
    return bitmap;
}

void requestNameCropPreviews(const MatchItem& item, int previewWidth) {
    if (!app.p1NamePreview || !app.p2NamePreview) return;
    const std::uint64_t generation =
        app.previewGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    setNamePreviewPlaceholder(app.p1NamePreview, app.p1NamePreviewHint,
                              app.p1NamePreviewBitmap, L"P1 名前部分の画像を抽出中…");
    setNamePreviewPlaceholder(app.p2NamePreview, app.p2NamePreviewHint,
                              app.p2NamePreviewBitmap, L"P2 名前部分の画像を抽出中…");
    // Only one extraction runs at a time.  A newer selection during that run
    // is remembered and started from the completion message, so fast keyboard
    // scrolling cannot pile up ffmpeg processes.
    if (app.previewWorkerBusy.exchange(true, std::memory_order_acq_rel)) {
        app.previewRetryPending = true;
        return;
    }
    const HWND window = app.window;
    const fs::path runtime = app.runtime;
    const int width = std::max(64, previewWidth);
    const std::wstring filePrefix = L"study-name-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(item.key) + L"-" +
        std::to_wstring(generation);
    try {
        std::thread([generation, window, runtime, item, width, filePrefix] {
            auto payload = std::make_unique<PreviewPayload>();
            payload->generation = generation;
            try {
                const fs::path ffmpeg = runtime / L"bin" / L"ffmpeg.exe";
                const fs::path cache = fs::temp_directory_path() / L"TetrisStudyNameCrops";
                const double duration = std::max(0.0, item.end - item.start);
                // Name plates are visible for the whole match; sample a little
                // after the start so a fade-in or the GO overlay is avoided.
                double frameSeconds = std::max(0.0, item.start +
                    std::clamp(duration * 0.2, 0.5, 4.0));
                if (item.end > item.start)
                    frameSeconds = std::min(frameSeconds, std::max(item.start, item.end - 0.05));
                if (!fs::is_regular_file(ffmpeg)) {
                    payload->p1Message = L"ffmpeg.exeが見つかりません";
                    payload->p2Message = payload->p1Message;
                } else {
                    payload->p1 = extractNameCropBitmap(ffmpeg, item.sourceVideo,
                        frameSeconds, item.p1NameCrop, width,
                        cache / (filePrefix + L"-p1.bmp"), payload->p1Message);
                    if (app.previewGeneration.load(std::memory_order_relaxed) == generation) {
                        payload->p2 = extractNameCropBitmap(ffmpeg, item.sourceVideo,
                            frameSeconds, item.p2NameCrop, width,
                            cache / (filePrefix + L"-p2.bmp"), payload->p2Message);
                    } else if (payload->p2Message.empty()) {
                        payload->p2Message = L"選択が変わったため抽出を中断しました";
                    }
                }
            } catch (const std::exception& exception) {
                const std::wstring message = fromUtf8(exception.what());
                if (payload->p1Message.empty()) payload->p1Message = message;
                if (payload->p2Message.empty()) payload->p2Message = message;
            } catch (...) {
                if (payload->p1Message.empty())
                    payload->p1Message = L"名前画像を抽出できませんでした";
                if (payload->p2Message.empty()) payload->p2Message = payload->p1Message;
            }
            app.previewWorkerBusy.store(false, std::memory_order_release);
            PreviewPayload* raw = payload.release();
            if (!PostMessageW(window, WM_STUDY_PREVIEW_DONE, 0,
                              reinterpret_cast<LPARAM>(raw))) delete raw;
        }).detach();
    } catch (const std::exception& exception) {
        // std::thread construction is the only throwing step left here.
        app.previewWorkerBusy.store(false, std::memory_order_release);
        app.previewRetryPending = false;
        const std::wstring message = L"画像処理を開始できません: " +
            fromUtf8(exception.what());
        setNamePreviewPlaceholder(app.p1NamePreview, app.p1NamePreviewHint,
                                  app.p1NamePreviewBitmap, message);
        setNamePreviewPlaceholder(app.p2NamePreview, app.p2NamePreviewHint,
                                  app.p2NamePreviewBitmap, message);
    }
}

// ---------------------------------------------------------------------------
// Name plate image comparison
//
// OCR reads the plate per match, so the same player can come back as slightly
// different text (for example 2Stい??? against 2Sぜい???).  The plate itself is
// a fixed overlay graphic, so the same displayed name produces almost identical
// pixels.  Each match is reduced to a small strip plus a normalized luminance
// vector, and two matches are treated as the same displayed name when the
// correlation is above NameImageSameThreshold.
// ---------------------------------------------------------------------------

double namePlateSeconds(const MatchItem& item) {
    const double duration = std::max(0.0, item.end - item.start);
    // Name plates are visible for the whole match; sample a little after the
    // start so a fade-in or the GO overlay is avoided.
    double seconds = std::max(0.0, item.start + std::clamp(duration * 0.2, 0.5, 4.0));
    if (item.end > item.start)
        seconds = std::min(seconds, std::max(item.start, item.end - 0.05));
    return seconds;
}

RECT nameCropRectangle(const NameCropRect& crop) {
    RECT rectangle{};
    rectangle.left = static_cast<LONG>(std::lround(crop.x));
    rectangle.top = static_cast<LONG>(std::lround(crop.y));
    rectangle.right = rectangle.left + static_cast<LONG>(std::lround(crop.width));
    rectangle.bottom = rectangle.top + static_cast<LONG>(std::lround(crop.height));
    return rectangle;
}

bool namePlateBox(const MatchItem& item, RECT& box, RECT& p1, RECT& p2) {
    if (!item.p1NameCrop.available || !item.p2NameCrop.available) return false;
    p1 = nameCropRectangle(item.p1NameCrop);
    p2 = nameCropRectangle(item.p2NameCrop);
    box.left = std::min(p1.left, p2.left);
    box.top = std::min(p1.top, p2.top);
    box.right = std::max(p1.right, p2.right);
    box.bottom = std::max(p1.bottom, p2.bottom);
    const LONG width = box.right - box.left;
    const LONG height = box.bottom - box.top;
    return width >= 32 && height >= 8 && width <= 3840 && height <= 512;
}

RECT scaledNameRegion(const RECT& region, const RECT& box, int stripWidth, int stripHeight) {
    const double scaleX = static_cast<double>(stripWidth) /
                          std::max<LONG>(1, box.right - box.left);
    const double scaleY = static_cast<double>(stripHeight) /
                          std::max<LONG>(1, box.bottom - box.top);
    RECT scaled{};
    scaled.left = static_cast<LONG>(std::lround((region.left - box.left) * scaleX));
    scaled.top = static_cast<LONG>(std::lround((region.top - box.top) * scaleY));
    scaled.right = scaled.left + static_cast<LONG>(
        std::lround((region.right - region.left) * scaleX));
    scaled.bottom = scaled.top + static_cast<LONG>(
        std::lround((region.bottom - region.top) * scaleY));
    scaled.left = std::clamp<LONG>(scaled.left, 0, stripWidth - 2);
    scaled.top = std::clamp<LONG>(scaled.top, 0, stripHeight - 2);
    scaled.right = std::clamp<LONG>(scaled.right, scaled.left + 2, stripWidth);
    scaled.bottom = std::clamp<LONG>(scaled.bottom, scaled.top + 2, stripHeight);
    return scaled;
}

// Both plates are cut with one seek, so a full project costs one ffmpeg launch
// per match instead of two.
HBITMAP extractNamePlateBitmap(const fs::path& ffmpeg, const fs::path& source,
                               double seconds, const RECT& box, int stripWidth,
                               const fs::path& output, std::wstring& error) {
    if (!fs::is_regular_file(source)) {
        error = L"元動画が見つかりません";
        return nullptr;
    }
    if (!std::isfinite(seconds) || seconds < 0) {
        error = L"画像を切り抜く時刻が不正です";
        return nullptr;
    }
    std::error_code filesystemError;
    fs::create_directories(output.parent_path(), filesystemError);
    if (filesystemError) {
        error = L"名前画像の一時フォルダーを作成できません";
        return nullptr;
    }
    const int x = box.left;
    const int y = box.top;
    const int width = static_cast<int>(box.right - box.left);
    const int height = static_cast<int>(box.bottom - box.top);
    std::wostringstream timestamp;
    timestamp << std::fixed << std::setprecision(3) << seconds;
    const std::wstring filter = L"crop=" + std::to_wstring(width) + L":" +
        std::to_wstring(height) + L":" + std::to_wstring(x) + L":" + std::to_wstring(y) +
        L",scale=" + std::to_wstring(stripWidth) + L":-1:flags=lanczos";
    const std::wstring command = quoteArgument(ffmpeg.wstring()) +
        L" -hide_banner -loglevel error -nostdin -ss " + timestamp.str() +
        L" -i " + quoteArgument(source.wstring()) +
        L" -map 0:v:0 -an -sn -dn -frames:v 1 -vf " + quoteArgument(filter) +
        L" -f image2 -vcodec bmp -update 1 -y " + quoteArgument(output.wstring());
    int exitCode = 1;
    std::wstring processError;
    if (!runCaptured(command, [](const std::string&) {}, processError, exitCode) ||
        exitCode != 0 || !previewFileIsReady(output)) {
        DeleteFileW(output.c_str());
        error = processError.empty() ? L"名前部分の画像を抽出できませんでした" : processError;
        return nullptr;
    }
    HBITMAP bitmap = reinterpret_cast<HBITMAP>(LoadImageW(
        nullptr, output.c_str(), IMAGE_BITMAP, 0, 0,
        LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    DeleteFileW(output.c_str());
    if (!bitmap) error = L"切り抜いた名前画像を読み込めませんでした";
    return bitmap;
}

bool buildNameSlice(HBITMAP strip, const RECT& region, NameImageSlice& slice,
                    std::wstring& error) {
    BITMAP header{};
    if (!GetObjectW(strip, sizeof(header), &header) || header.bmWidth < 4 ||
        header.bmHeight < 2) {
        error = L"名前画像の大きさを取得できません";
        return false;
    }
    HDC screen = GetDC(nullptr);
    if (!screen) {
        error = L"画面デバイスを取得できません";
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = NameThumbWidth;
    info.bmiHeader.biHeight = -NameThumbHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP thumbnail = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits,
                                         nullptr, 0);
    HDC source = CreateCompatibleDC(screen);
    HDC target = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!thumbnail || !bits || !source || !target) {
        if (thumbnail) DeleteObject(thumbnail);
        if (source) DeleteDC(source);
        if (target) DeleteDC(target);
        error = L"名前画像の表示用ビットマップを作成できません";
        return false;
    }
    HGDIOBJ oldSource = SelectObject(source, strip);
    HGDIOBJ oldTarget = SelectObject(target, thumbnail);
    SetStretchBltMode(target, HALFTONE);
    SetBrushOrgEx(target, 0, 0, nullptr);
    StretchBlt(target, 0, 0, NameThumbWidth, NameThumbHeight, source,
               region.left, region.top, region.right - region.left,
               region.bottom - region.top, SRCCOPY);
    SelectObject(target, oldTarget);
    SelectObject(source, oldSource);
    DeleteDC(target);
    DeleteDC(source);
    GdiFlush();

    slice.thumbnail = thumbnail;
    const auto* pixels = static_cast<const std::uint8_t*>(bits);
    const std::size_t count = static_cast<std::size_t>(NameThumbWidth) * NameThumbHeight;
    slice.fingerprint.assign(count, 0.0f);
    double sum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* pixel = pixels + i * 4;
        const float luminance = 0.114f * pixel[0] + 0.587f * pixel[1] + 0.299f * pixel[2];
        slice.fingerprint[i] = luminance;
        sum += luminance;
    }
    const double mean = sum / static_cast<double>(count);
    double variance = 0;
    for (float value : slice.fingerprint) {
        const double difference = value - mean;
        variance += difference * difference;
    }
    const double deviation = std::sqrt(variance / static_cast<double>(count));
    if (deviation < 1.0) {
        // A flat plate carries no readable glyphs; keep it unusable so it can
        // never be matched to a real name.
        slice.reset();
        error = L"名前画像が単色でした";
        return false;
    }
    for (float& value : slice.fingerprint)
        value = static_cast<float>((value - mean) / deviation);
    return true;
}

float nameImageCorrelation(const std::vector<float>& left,
                           const std::vector<float>& right) {
    if (left.empty() || left.size() != right.size()) return 0;
    double dot = 0;
    for (std::size_t i = 0; i < left.size(); ++i) dot += left[i] * right[i];
    return static_cast<float>(dot / static_cast<double>(left.size()));
}

struct NameImageTask {
    int key = 0;
    fs::path manifest;
    fs::path video;
    double seconds = 0;
    RECT box{};
    RECT p1{};
    RECT p2{};
};

bool buildNameImageRow(const fs::path& ffmpeg, const NameImageTask& task,
                       const fs::path& output, NameImageRow& row, std::wstring& error) {
    HBITMAP strip = extractNamePlateBitmap(ffmpeg, task.video, task.seconds,
                                           task.box, 700, output, error);
    if (!strip) return false;
    BITMAP header{};
    bool ok = GetObjectW(strip, sizeof(header), &header) != 0;
    if (ok) {
        const RECT p1 = scaledNameRegion(task.p1, task.box, header.bmWidth, header.bmHeight);
        const RECT p2 = scaledNameRegion(task.p2, task.box, header.bmWidth, header.bmHeight);
        ok = buildNameSlice(strip, p1, row.p1, error) &&
             buildNameSlice(strip, p2, row.p2, error);
    } else {
        error = L"名前画像の大きさを取得できません";
    }
    DeleteObject(strip);
    return ok;
}

JobResult collectNameImages() {
    JobResult result;
    result.kind = JobKind::NameImages;
    std::vector<NameImageTask> tasks;
    tasks.reserve(app.matches.size());
    for (const MatchItem& item : app.matches) {
        RECT box{};
        RECT p1{};
        RECT p2{};
        if (!namePlateBox(item, box, p1, p2)) continue;
        if (!fs::is_regular_file(item.sourceVideo)) continue;
        NameImageTask task;
        task.key = item.key;
        task.manifest = item.manifest;
        task.video = item.sourceVideo;
        task.seconds = namePlateSeconds(item);
        task.box = box;
        task.p1 = p1;
        task.p2 = p2;
        tasks.push_back(std::move(task));
    }
    if (tasks.empty()) {
        result.error = L"名前画像を切り抜ける試合がありません（解析データに切り抜き位置が必要です）。";
        return result;
    }
    const fs::path ffmpeg = app.runtime / L"bin" / L"ffmpeg.exe";
    if (!fs::is_regular_file(ffmpeg)) {
        result.error = L"ffmpeg.exe が見つかりません: " + ffmpeg.wstring();
        return result;
    }
    const fs::path cache = fs::temp_directory_path() / L"TetrisStudyNameCrops";
    std::mutex mutex;
    std::size_t next = 0;
    std::size_t completed = 0;
    int failures = 0;
    std::vector<NameImageRow> rows;
    rows.reserve(tasks.size());
    const int hardware = static_cast<int>(std::thread::hardware_concurrency());
    const int workerCount = std::clamp(hardware / 2, 2, 4);
    const std::wstring prefix = L"name-group-" +
        std::to_wstring(GetCurrentProcessId());
    const auto run = [&] {
        for (;;) {
            std::size_t index = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (next >= tasks.size()) return;
                index = next++;
            }
            const NameImageTask& task = tasks[index];
            NameImageRow row;
            row.key = task.key;
            row.manifest = task.manifest;
            std::wstring error;
            const fs::path output = cache / (prefix + L"-" +
                std::to_wstring(task.key) + L".bmp");
            const bool ok = buildNameImageRow(ffmpeg, task, output, row, error);
            std::size_t done = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (ok) rows.push_back(std::move(row));
                else ++failures;
                done = ++completed;
            }
            postProgress(static_cast<int>(done * 100 / tasks.size()),
                         L"名前画像を照合しています… " + std::to_wstring(done) + L" / " +
                         std::to_wstring(tasks.size()));
        }
    };
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i) workers.emplace_back(run);
    for (std::thread& worker : workers) worker.join();
    std::sort(rows.begin(), rows.end(),
              [](const NameImageRow& a, const NameImageRow& b) { return a.key < b.key; });
    if (rows.empty()) {
        result.error = L"名前画像を1件も取得できませんでした。";
        result.nameImageFailures = failures;
        return result;
    }
    result.nameImages = std::move(rows);
    result.nameImageFailures = failures;
    result.exitCode = 0;
    return result;
}

// Greedy clustering against each group's first member.  Keys are visited in
// order, so the group label follows the first match that shows the plate.
void assignNameImageGroups(std::vector<NameImageRow>& rows) {
    for (int side = 0; side < 2; ++side) {
        std::vector<std::size_t> representatives;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            NameImageSlice& slice = side == 0 ? rows[index].p1 : rows[index].p2;
            if (slice.fingerprint.empty()) continue;
            int assigned = 0;
            float best = NameImageSameThreshold;
            for (std::size_t group = 0; group < representatives.size(); ++group) {
                const NameImageSlice& reference = side == 0
                    ? rows[representatives[group]].p1 : rows[representatives[group]].p2;
                const float score = nameImageCorrelation(slice.fingerprint,
                                                         reference.fingerprint);
                if (score >= best) {
                    best = score;
                    assigned = static_cast<int>(group) + 1;
                }
            }
            if (assigned == 0) {
                representatives.push_back(index);
                assigned = static_cast<int>(representatives.size());
            }
            slice.group = assigned;
        }
    }
}

void releaseNameImages() {
    app.nameImages.clear();
}

int nameImageGroupOf(int key, int side) {
    const auto found = app.nameImages.find(key);
    if (found == app.nameImages.end()) return 0;
    return side == 0 ? found->second.p1.group : found->second.p2.group;
}

// Group label plus how many matches share it and how many OCR spellings the
// group contains, which is exactly where OCR drifted.
std::wstring nameImageGroupText(int side, int group) {
    if (group <= 0) return L"未照合";
    std::size_t count = 0;
    std::map<std::wstring, std::size_t> texts;
    for (const auto& [key, pair] : app.nameImages) {
        const int value = side == 0 ? pair.p1.group : pair.p2.group;
        if (value != group) continue;
        ++count;
        for (const MatchItem& item : app.matches) {
            if (item.key != key) continue;
            ++texts[side == 0 ? item.p1Name : item.p2Name];
            break;
        }
    }
    std::wostringstream text;
    text << L"#" << group << L"（" << count << L"試合";
    if (texts.size() > 1) text << L"・OCR表記 " << texts.size() << L"種";
    text << L"）";
    return text.str();
}

// Matches are rebuilt from the manifest after a name edit.  The images come
// from the video and do not change, so keep the finished comparison instead of
// asking the user to run it again.
void restoreNameImages(std::map<int, NameImagePair>& saved, const fs::path& manifest) {
    if (saved.empty()) return;
    std::map<int, const MatchItem*> available;
    for (const MatchItem& item : app.matches) available.emplace(item.key, &item);
    for (auto& [key, pair] : saved) {
        const auto found = available.find(key);
        if (found == available.end() || found->second->manifest != manifest) {
            continue;
        }
        app.nameImages.emplace(key, std::move(pair));
    }
    if (app.nameImages.empty()) releaseNameImages();
}

void applyNameImageResults(JobResult& result) {
    std::vector<NameImageRow> rows = std::move(result.nameImages);
    assignNameImageGroups(rows);
    releaseNameImages();
    for (NameImageRow& row : rows) {
        NameImagePair pair;
        pair.p1 = std::move(row.p1);
        pair.p2 = std::move(row.p2);
        app.nameImages[row.key] = std::move(pair);
    }
}

std::wstring nameImageSummary(int failures) {
    std::size_t matchCount = 0;
    std::map<int, std::size_t> p1Groups;
    std::map<int, std::size_t> p2Groups;
    std::map<int, std::map<std::wstring, std::size_t>> p1Texts;
    std::map<int, std::map<std::wstring, std::size_t>> p2Texts;
    std::map<int, const MatchItem*> matches;
    for (const MatchItem& item : app.matches) matches.emplace(item.key, &item);
    for (const auto& [key, pair] : app.nameImages) {
        ++matchCount;
        if (pair.p1.group > 0) {
            ++p1Groups[pair.p1.group];
            const auto found = matches.find(key);
            if (found != matches.end())
                ++p1Texts[pair.p1.group][found->second->p1Name];
        }
        if (pair.p2.group > 0) {
            ++p2Groups[pair.p2.group];
            const auto found = matches.find(key);
            if (found != matches.end())
                ++p2Texts[pair.p2.group][found->second->p2Name];
        }
    }
    std::size_t variance = 0;
    std::wstring worstGroup;
    std::size_t worstSpellings = 1;
    const auto inspect = [&](const std::map<int, std::map<std::wstring, std::size_t>>& groups) {
        for (const auto& [group, texts] : groups) {
            (void)group;
            if (texts.size() <= 1) continue;
            ++variance;
            if (texts.size() <= worstSpellings) continue;
            worstSpellings = texts.size();
            worstGroup.clear();
            std::size_t best = 0;
            for (const auto& [text, count] : texts) {
                if (count > best) {
                    best = count;
                    worstGroup = text;
                }
            }
        }
    };
    inspect(p1Texts);
    inspect(p2Texts);
    std::wostringstream text;
    text << L"名前画像を照合しました: " << matchCount << L" 試合 / P1 "
         << p1Groups.size() << L"組 / P2 " << p2Groups.size() << L"組";
    if (variance > 0) {
        text << L"。同じ画像なのにOCR表記が割れている組: " << variance << L"組";
        if (worstSpellings > 1 && !worstGroup.empty())
            text << L"（" << worstGroup << L" は " << worstSpellings << L" 通り）";
    }
    if (failures > 0) text << L" / 画像を取得できなかった試合: " << failures;
    text << L"。同じ組の試合は同じ表示名です。";
    return text.str();
}

void applyQualityReport(MatchItem& item, const Json& quality) {
    item.suspicious = quality["suspicious"].flag(false);
    item.terminalAnomaly = quality["terminal_anomaly"]["present"].flag(false);
    item.minimumRatio = 1;
    const auto details = [&](const Json& issues) {
        std::wostringstream text;
        for (const Json& issue : issues.array) {
            item.minimumRatio = std::min(item.minimumRatio, issue["minimum_match_ratio"].value(1));
            if (!text.str().empty()) text << L"\r\n";
            text << L"P" << static_cast<int>(issue["player"].value())
                 << L" フェーズ" << static_cast<int>(issue["first_phase"].value())
                 << L"–" << static_cast<int>(issue["last_phase"].value())
                 << L" / " << formatTime(issue["source_start_seconds"].value(item.start))
                 << L"–" << formatTime(issue["source_end_seconds"].value(item.start))
                 << L" / 最低一致 " << std::fixed << std::setprecision(1)
                 << issue["minimum_match_ratio"].value(1) * 100 << L"%";
        }
        return text.str();
    };
    item.warningDetails = details(quality["issues"]);
    item.terminalDetails = details(quality["terminal_anomaly"]["issues"]);
}

void applyWarningSidecar() {
    std::map<fs::path, Json> sidecars;
    for (MatchItem& item : app.matches) {
        const fs::path path = item.manifest.parent_path() / L"quality-warnings.json";
        auto found = sidecars.find(path);
        if (found == sidecars.end()) {
            Json root;
            try {
                if (fs::is_regular_file(path)) root = JsonParser(readFile(path)).parse();
            } catch (...) {}
            found = sidecars.emplace(path, std::move(root)).first;
        }
        if (found->second["schema"].text() != "tetris-recovery-quality-warnings/v1") continue;
        for (const Json& warning : found->second["matches"].array) {
            if (static_cast<int>(warning["index"].value()) != item.index) continue;
            item.status = fromUtf8(warning["status"].text(toUtf8(item.status)));
            applyQualityReport(item, warning["quality_warning"]);
            break;
        }
    }
}

void loadLegacyDecisions() {
    const fs::path path = app.manifest.parent_path() / L"quality-review.json";
    if (!fs::is_regular_file(path)) return;
    try {
        const Json root = JsonParser(readFile(path)).parse();
        for (const auto& [key, value] : root["matches"].object) {
            const int index = std::atoi(key.c_str());
            if (index <= 0 || app.decisions.count(index)) continue;
            Decision decision;
            const std::string raw = value["decision"].text();
            decision.value = raw == "confirmed-broken" ? L"broken" : fromUtf8(raw);
            decision.video = absoluteJsonPath(path.parent_path(), value["video"].text());
            app.decisions[index] = std::move(decision);
        }
    } catch (...) {}
}

void saveProject() {
    if (app.project.empty() || app.manifest.empty()) return;
    fs::path pending = app.project; pending += L".pending";
    std::ofstream file(pending, std::ios::binary | std::ios::trunc);
    if (!file) return;
    std::error_code relativeError;
    fs::path manifestPath = fs::relative(app.manifest, app.project.parent_path(), relativeError);
    if (relativeError) manifestPath = app.manifest;
    file << "{\"schema\":\"tetris-study-project/v1\",\"manifest\":\""
         << jsonEscape(manifestPath.generic_u8string()) << "\",\"decisions\":{";
    bool first = true;
    for (const auto& [index, decision] : app.decisions) {
        if (!first) file << ',';
        first = false;
        file << '\"' << index << "\":{\"value\":\""
             << jsonEscape(toUtf8(decision.value)) << "\",\"video\":\""
             << jsonEscape(decision.video.generic_u8string()) << "\"}";
    }
    file << "}}\n";
    file.close();
    if (!file) return;
    MoveFileExW(pending.c_str(), app.project.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void loadProjectDecisions(const Json& projectRoot) {
    for (const auto& [key, value] : projectRoot["decisions"].object) {
        const int index = std::atoi(key.c_str());
        if (index <= 0) continue;
        Decision decision;
        decision.value = fromUtf8(value["value"].text());
        decision.video = absoluteJsonPath(app.project.parent_path(), value["video"].text());
        app.decisions[index] = std::move(decision);
    }
}

bool loadManifestOrProject(const fs::path& input, std::wstring& error) {
    const fs::path previousManifest = app.manifest;
    const fs::path previousProject = app.project;
    const fs::path previousSourceVideo = app.sourceVideo;
    const bool previousCollection = app.collection;
    const std::wstring previousProjectTitle = app.projectTitle;
    const std::vector<MatchItem> previousMatches = app.matches;
    const std::map<int, Decision> previousDecisions = app.decisions;
    const std::vector<int> previousDetailSelection = app.detailSelection;
    const int previousSelectedIndex = app.selectedIndex;
    std::map<int, NameImagePair> previousNameImages = std::move(app.nameImages);
    const auto restorePrevious = [&] {
        app.manifest = previousManifest;
        app.project = previousProject;
        app.sourceVideo = previousSourceVideo;
        app.collection = previousCollection;
        app.projectTitle = previousProjectTitle;
        app.matches = previousMatches;
        app.decisions = previousDecisions;
        app.detailSelection = previousDetailSelection;
        app.selectedIndex = previousSelectedIndex;
        app.nameImages = std::move(previousNameImages);
    };
    try {
        fs::path manifest = input;
        Json projectRoot;
        const bool isProject = lower(input.extension().wstring()) == L".tstudy";
        if (isProject) {
            projectRoot = JsonParser(readFile(input)).parse();
            app.project = fs::absolute(input);
            manifest = absoluteJsonPath(app.project.parent_path(), projectRoot["manifest"].text());
        }
        if (!fs::is_regular_file(manifest)) {
            error = L"解析結果ファイルが見つかりません: " + manifest.wstring();
            restorePrevious();
            return false;
        }
        const Json root = JsonParser(readFile(manifest)).parse();
        if (root["schema"].text().find("tetris-video-recovery.batch/") != 0) {
            error = L"このJSONは配信解析プロジェクトではありません。";
            restorePrevious();
            return false;
        }
        app.manifest = fs::absolute(manifest);
        app.collection = root["collection"].flag(false);
        app.projectTitle = fromUtf8(root["title"].text());
        if (!isProject) app.project = app.manifest.parent_path() / L"tetris-study.tstudy";
        const fs::path rootSource = absoluteJsonPath(app.manifest.parent_path(), root["source"].text());
        app.sourceVideo = app.collection ? fs::path{} : rootSource;
        app.detailSelection.clear();
        app.matches.clear();
        app.decisions.clear();
        std::map<fs::path, Json> childManifests;
        for (const Json& listedValue : root["matches"].array) {
            const int listedIndex = static_cast<int>(listedValue["index"].value());
            const int originalIndex = static_cast<int>(
                listedValue["original_index"].value(listedIndex));
            fs::path itemManifest = app.manifest;
            fs::path itemSource = rootSource;
            const Json* value = &listedValue;
            if (app.collection) {
                itemManifest = absoluteJsonPath(app.manifest.parent_path(),
                                                listedValue["manifest"].text());
                if (!fs::is_regular_file(itemManifest)) {
                    throw std::runtime_error("collection child manifest is missing: " +
                                             itemManifest.generic_u8string());
                }
                auto foundManifest = childManifests.find(itemManifest);
                if (foundManifest == childManifests.end()) {
                    const Json child = JsonParser(readFile(itemManifest)).parse();
                    if (child["schema"].text().find("tetris-video-recovery.batch/") != 0)
                        throw std::runtime_error("collection child is not a batch manifest");
                    foundManifest = childManifests.emplace(itemManifest, child).first;
                }
                const Json& childRoot = foundManifest->second;
                itemSource = absoluteJsonPath(itemManifest.parent_path(),
                                              childRoot["source"].text());
                value = nullptr;
                for (const Json& candidate : childRoot["matches"].array) {
                    if (static_cast<int>(candidate["index"].value()) == originalIndex) {
                        value = &candidate;
                        break;
                    }
                }
                if (!value) {
                    throw std::runtime_error("collection match is missing from child manifest");
                }
            }
            MatchItem item;
            item.key = listedIndex;
            item.index = originalIndex;
            item.manifest = itemManifest;
            item.sourceVideo = itemSource;
            item.sourceLabel = fromUtf8(listedValue["source_label"].text());
            if (item.sourceLabel.empty()) item.sourceLabel = itemSource.filename().wstring();
            item.status = fromUtf8((*value)["status"].text());
            item.start = (*value)["source_range"]["start_seconds"].value();
            item.end = (*value)["source_range"]["end_seconds"].value();
            applyQualityReport(item, (*value)["quality_warning"]);
            item.error = fromUtf8((*value)["error"].text());
            item.autosave = absoluteJsonPath(itemManifest.parent_path(), (*value)["autosave"].text());
            item.p1Shortcut = absoluteJsonPath(itemManifest.parent_path(), (*value)["simulator"]["p1"].text());
            item.p2Shortcut = absoluteJsonPath(itemManifest.parent_path(), (*value)["simulator"]["p2"].text());
            item.combinedShortcut = absoluteJsonPath(itemManifest.parent_path(), (*value)["simulator"]["combined"].text());
            item.p1Name = fromUtf8((*value)["player_names"]["p1"]["text"].text());
            item.p2Name = fromUtf8((*value)["player_names"]["p2"]["text"].text());
            parseNameCrop((*value)["player_names"]["p1"], item.p1NameCrop);
            parseNameCrop((*value)["player_names"]["p2"], item.p2NameCrop);
            // The manifest contains the original OCR result. Manual edits are
            // stored in the autosave, so it must always win when available.
            std::wstring autosaveP1;
            std::wstring autosaveP2;
            NameCropRect autosaveP1Crop;
            NameCropRect autosaveP2Crop;
            if (parsePlayerNames(item.autosave, autosaveP1, autosaveP2,
                                 autosaveP1Crop, autosaveP2Crop)) {
                item.p1Name = std::move(autosaveP1);
                item.p2Name = std::move(autosaveP2);
                if (autosaveP1Crop.available) item.p1NameCrop = autosaveP1Crop;
                if (autosaveP2Crop.available) item.p2NameCrop = autosaveP2Crop;
            }
            app.matches.push_back(std::move(item));
        }
        applyWarningSidecar();
        restoreNameImages(previousNameImages, app.manifest);
        if (isProject) {
            loadProjectDecisions(projectRoot);
        } else if (fs::is_regular_file(app.project)) {
            // batch-results.jsonを直接開いても、同じ解析に紐づく既存の確認結果を失わない。
            try {
                const Json savedProject = JsonParser(readFile(app.project)).parse();
                const fs::path savedManifest = absoluteJsonPath(
                    app.project.parent_path(), savedProject["manifest"].text());
                std::error_code equivalentError;
                const bool sameManifest = fs::equivalent(savedManifest, app.manifest,
                                                         equivalentError);
                if (!equivalentError && sameManifest) loadProjectDecisions(savedProject);
            } catch (...) {
                // 壊れた補助プロジェクトより、正しいbatch manifestの閲覧を優先する。
            }
        }
        if (!app.collection) loadLegacyDecisions();
        saveProject();
        populateList();
        EnableWindow(app.refresh, !app.busy.load());
        const std::wstring displayTitle = app.projectTitle.empty()
            ? (app.collection ? app.manifest.stem().wstring() : app.sourceVideo.stem().wstring())
            : app.projectTitle;
        setText(app.source, app.collection ? displayTitle : app.sourceVideo.wstring());
        std::size_t suspiciousCount = 0;
        std::size_t correctionCount = 0;
        for (const MatchItem& item : app.matches) {
            if (item.suspicious) ++suspiciousCount;
            if (item.status == L"needs_correction") ++correctionCount;
        }
        std::wstring openedStatus = L"プロジェクトを開きました: " +
            std::to_wstring(app.matches.size()) + L" 試合";
        if (app.collection) {
            openedStatus += L"（品質警告 " + std::to_wstring(suspiciousCount) +
                L" / 要修正 " + std::to_wstring(correctionCount) + L"）";
        }
        setText(app.status, openedStatus);
        SetWindowTextW(app.window, (L"ぷよテト研究 - " + displayTitle).c_str());
        return true;
    } catch (const std::exception& exception) {
        restorePrevious();
        populateList();
        updateDetails();
        error = L"プロジェクトを読み込めません: " + fromUtf8(exception.what());
        return false;
    }
}

std::wstring displayState(const MatchItem& item) {
    const bool failed = item.status == L"failed";
    const bool correction = item.status == L"needs_correction";
    const bool danger = failed || correction || item.suspicious;
    const auto decision = app.decisions.find(item.key);
    if (!danger && item.terminalAnomaly) return L"正常（終盤のみ異常）";
    if (!danger && decision != app.decisions.end() && decision->second.value == L"broken")
        return L"修正済み";
    if (decision != app.decisions.end() && decision->second.value == L"okay")
        return danger ? L"確認済み（許容）" : L"正常";
    if (decision != app.decisions.end() && decision->second.value == L"broken")
        return L"破損確認済み・保存済み";
    if (correction) return item.suspicious ? L"要修正＋品質警告" : L"要修正";
    if (failed) return L"解析失敗";
    if (item.suspicious) return L"品質警告";
    return L"正常";
}

void populateList() {
    if (!app.list) return;
    finishListDrag(app.list);
    app.visible.clear();
    const std::wstring filter = lower(controlText(app.search));
    for (std::size_t i = 0; i < app.matches.size(); ++i) {
        const MatchItem& item = app.matches[i];
        const std::wstring haystack = lower(item.sourceLabel + L" " + item.p1Name + L" " +
                                             item.p2Name + L" " + displayState(item) + L" " +
                                             std::to_wstring(item.index));
        if (filter.empty() || haystack.find(filter) != std::wstring::npos) app.visible.push_back(i);
    }
    const int sort = static_cast<int>(SendMessageW(app.sort, CB_GETCURSEL, 0, 0));
    const auto unresolvedWarning = [](const MatchItem& item) {
        const bool danger = item.status == L"failed" || item.status == L"needs_correction" ||
                            item.suspicious;
        const auto decision = app.decisions.find(item.key);
        return danger && (decision == app.decisions.end() ||
                          (decision->second.value != L"okay" &&
                           decision->second.value != L"broken"));
    };
    std::stable_sort(app.visible.begin(), app.visible.end(), [&](std::size_t a, std::size_t b) {
        const MatchItem& left = app.matches[a]; const MatchItem& right = app.matches[b];
        if (sort == 1) return lower(left.p1Name) < lower(right.p1Name);
        if (sort == 2) return lower(left.p2Name) < lower(right.p2Name);
        if (sort == 3) {
            const bool ld = unresolvedWarning(left);
            const bool rd = unresolvedWarning(right);
            if (ld != rd) return ld > rd;
        }
        if (sort == 4) {
            const std::wstring leftSource = lower(left.sourceLabel);
            const std::wstring rightSource = lower(right.sourceLabel);
            if (leftSource != rightSource) return leftSource < rightSource;
        }
        if (sort == 5) {
            const int leftP1 = nameImageGroupOf(left.key, 0);
            const int rightP1 = nameImageGroupOf(right.key, 0);
            if (leftP1 != rightP1) return leftP1 < rightP1;
            const int leftP2 = nameImageGroupOf(left.key, 1);
            const int rightP2 = nameImageGroupOf(right.key, 1);
            if (leftP2 != rightP2) return leftP2 < rightP2;
        }
        return left.key < right.key;
    });
    SendMessageW(app.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(app.list);
    for (std::size_t row = 0; row < app.visible.size(); ++row) {
        const MatchItem& item = app.matches[app.visible[row]];
        std::wstring index = std::to_wstring(item.index);
        LVITEMW entry{}; entry.mask = LVIF_TEXT | LVIF_PARAM;
        entry.iItem = static_cast<int>(row); entry.pszText = index.data();
        entry.lParam = static_cast<LPARAM>(app.visible[row]);
        ListView_InsertItem(app.list, &entry);
        const std::wstring p1 = item.p1Name.empty() ? L"（未読取）" : item.p1Name;
        const std::wstring p2 = item.p2Name.empty() ? L"（未読取）" : item.p2Name;
        const std::wstring state = displayState(item);
        const std::wstring start = formatTime(item.start);
        const std::wstring duration = formatTime(item.end - item.start);
        wchar_t ratio[32];
        if (item.minimumRatio < 1) swprintf_s(ratio, L"%.1f%%", item.minimumRatio * 100);
        else wcscpy_s(ratio, L"-");
        wchar_t groupP1[16] = L"-";
        wchar_t groupP2[16] = L"-";
        const auto image = app.nameImages.find(item.key);
        if (image != app.nameImages.end()) {
            if (image->second.p1.group > 0)
                swprintf_s(groupP1, L"%d", image->second.p1.group);
            if (image->second.p2.group > 0)
                swprintf_s(groupP2, L"%d", image->second.p2.group);
        }
        ListView_SetItemText(app.list, static_cast<int>(row), 1,
                             const_cast<wchar_t*>(item.sourceLabel.c_str()));
        ListView_SetItemText(app.list, static_cast<int>(row), 2, const_cast<wchar_t*>(p1.c_str()));
        ListView_SetItemText(app.list, static_cast<int>(row), 3, const_cast<wchar_t*>(p2.c_str()));
        ListView_SetItemText(app.list, static_cast<int>(row), NameImageColumn, const_cast<wchar_t*>(L""));
        ListView_SetItemText(app.list, static_cast<int>(row), NameGroupColumns, groupP1);
        ListView_SetItemText(app.list, static_cast<int>(row), NameGroupColumns + 1, groupP2);
        ListView_SetItemText(app.list, static_cast<int>(row), 7, const_cast<wchar_t*>(state.c_str()));
        ListView_SetItemText(app.list, static_cast<int>(row), 8, const_cast<wchar_t*>(start.c_str()));
        ListView_SetItemText(app.list, static_cast<int>(row), 9, const_cast<wchar_t*>(duration.c_str()));
        ListView_SetItemText(app.list, static_cast<int>(row), 10, ratio);
    }
    SendMessageW(app.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app.list, nullptr, TRUE);
    if (!app.visible.empty()) {
        int selectedRow = 0;
        for (std::size_t row = 0; row < app.visible.size(); ++row)
            if (app.matches[app.visible[row]].key == app.selectedIndex) selectedRow = static_cast<int>(row);
        ListView_SetItemState(app.list, selectedRow, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(app.list, selectedRow, FALSE);
    } else updateDetails();
}

MatchItem* selectedMatch() {
    if (!app.list) return nullptr;
    const auto atRow = [](int row) -> MatchItem* {
        if (row < 0) return nullptr;
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (!ListView_GetItem(app.list, &item)) return nullptr;
        const std::size_t index = static_cast<std::size_t>(item.lParam);
        return index < app.matches.size() ? &app.matches[index] : nullptr;
    };
    const int focused = ListView_GetNextItem(app.list, -1, LVNI_FOCUSED);
    if (focused >= 0 &&
        (ListView_GetItemState(app.list, focused, LVIS_SELECTED) & LVIS_SELECTED)) {
        if (MatchItem* item = atRow(focused)) return item;
    }
    for (int row = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
         row >= 0; row = ListView_GetNextItem(app.list, row, LVNI_SELECTED)) {
        if (MatchItem* item = atRow(row)) return item;
    }
    return nullptr;
}

std::vector<MatchItem> selectedMatches() {
    std::vector<MatchItem> result;
    if (!app.list) return result;
    for (int row = ListView_GetNextItem(app.list, -1, LVNI_SELECTED);
         row >= 0; row = ListView_GetNextItem(app.list, row, LVNI_SELECTED)) {
        LVITEMW listItem{};
        listItem.mask = LVIF_PARAM;
        listItem.iItem = row;
        if (!ListView_GetItem(app.list, &listItem)) continue;
        const std::size_t index = static_cast<std::size_t>(listItem.lParam);
        if (index < app.matches.size()) result.push_back(app.matches[index]);
    }
    return result;
}

void updateDetails() {
    // Some EDIT controls notify their parent synchronously while createControls()
    // is still constructing the window.  Do not touch not-yet-created controls.
    if (!app.list || !app.detail || !app.p1Name || !app.p2Name || !app.saveNames)
        return;
    MatchItem* item = selectedMatch();
    const std::vector<MatchItem> selected = selectedMatches();
    const bool available = item != nullptr;
    const bool anySelected = !selected.empty();
    const bool mutableAction = available && !app.busy.load();
    EnableWindow(app.p1Name, !app.matches.empty() && !app.busy.load());
    EnableWindow(app.p2Name, !app.matches.empty() && !app.busy.load());
    EnableWindow(app.saveNames, anySelected && !app.busy.load());
    EnableWindow(app.saveAllNames, !app.matches.empty() && !app.busy.load());
    setText(app.saveAllNames, L"全 " + std::to_wstring(app.matches.size()) + L" 試合の名前を一括置換");
    for (HWND control : {app.review, app.markOkay, app.confirmBroken})
        EnableWindow(control, mutableAction);
    for (HWND control : {app.p1Simulator, app.p2Simulator, app.bothSimulator,
                         app.openFolder}) EnableWindow(control, available);
    EnableWindow(app.checkNameImages, !app.matches.empty() && !app.busy.load());
    EnableWindow(app.selectImageGroup,
                 available && !app.busy.load() && !app.nameImages.empty() &&
                 nameImageGroupOf(item->key, 0) > 0 && nameImageGroupOf(item->key, 1) > 0);
    setText(app.saveNames, selected.empty()
        ? L"プレイヤー名を保存（選択なし）"
        : (L"選択した " + std::to_wstring(selected.size()) + L" 試合へ名前を保存"));

    std::vector<int> selectionIds;
    selectionIds.reserve(selected.size());
    for (const MatchItem& selectedItem : selected) selectionIds.push_back(selectedItem.key);
    std::sort(selectionIds.begin(), selectionIds.end());
    const bool selectionChanged = selectionIds != app.detailSelection;
    if (selectionChanged) app.detailSelection = selectionIds;
    // Show the name plates cut from the source video under each name field.
    // Only the focused, single selected match has one unambiguous pair.
    if (app.p1NamePreview && app.p2NamePreview) {
        const bool singleSelection = item != nullptr && selected.size() == 1;
        if (!singleSelection) {
            if (app.previewKey != NamePreviewCleared) {
                clearNameCropPreviews(item
                    ? L"複数選択中は名前部分の画像を表示しません"
                    : L"試合を選択すると名前部分の画像を表示します");
            }
        } else if (app.previewKey != item->key ||
                   app.previewManifest != item->manifest) {
            app.previewKey = item->key;
            app.previewManifest = item->manifest;
            requestNameCropPreviews(*item, namePreviewWidth(app.p1NamePreview));
        }
    }
    if (!item) {
        setText(app.detail, app.matches.empty() ? L"試合を選択してください" :
            L"試合を選択してください。\r\n全試合の名前の一括置換は、絞り込みに関係なく使えます。\r\n空欄側の名前は変更しません。");
        if (selectionChanged) {
            setText(app.p1Name, L"");
            setText(app.p2Name, L"");
        }
        return;
    }
    app.selectedIndex = item->key;
    if (selectionChanged) {
        if (selected.size() == 1) {
            setText(app.p1Name, item->p1Name);
            setText(app.p2Name, item->p2Name);
        } else {
            // 複数選択では誤ってフォーカス行の名前を全試合へ複製しない。
            // 空欄は「変更しない」なので、利用者が変更側だけ入力できる。
            setText(app.p1Name, L"");
            setText(app.p2Name, L"");
        }
    }
    std::wostringstream detail;
    if (selected.size() > 1) {
        detail << selected.size() << L" 試合を選択中\r\n"
               << L"名前は非空欄だけ全選択試合へ上書きします。\r\n"
               << L"修正・シミュレータ操作対象: ";
    }
    // 判定と画像グループは常に見える位置へ置く（詳細欄は高さが限られる）。
    detail << L"判定: " << displayState(*item);
    if (item->minimumRatio < 1) detail << L"　最低一致率 " << std::fixed
                                       << std::setprecision(1) << item->minimumRatio * 100 << L"%";
    detail << L"\r\n";
    if (!app.nameImages.empty()) {
        const int p1Group = nameImageGroupOf(item->key, 0);
        const int p2Group = nameImageGroupOf(item->key, 1);
        if (p1Group > 0 || p2Group > 0) {
            detail << L"画像グループ: P1 " << nameImageGroupText(0, p1Group)
                   << L" / P2 " << nameImageGroupText(1, p2Group) << L"\r\n";
        }
    }
    detail << L"試合 " << item->index << L"　" << formatTime(item->start) << L" ～ "
           << formatTime(item->end) << L"（" << formatTime(item->end - item->start) << L"）\r\n"
           << item->sourceLabel;
    if (!item->warningDetails.empty()) detail << L"\r\n\r\n品質警告区間:\r\n" << item->warningDetails;
    if (!item->terminalDetails.empty()) detail << L"\r\n\r\n終盤異常区間:\r\n" << item->terminalDetails;
    if (!item->error.empty()) detail << L"\r\n\r\n" << item->error;
    setText(app.detail, detail.str());
}

fs::path chooseFile(HWND owner, const wchar_t* title, const wchar_t* filter) {
    wchar_t value[32768]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = owner;
    dialog.lpstrTitle = title; dialog.lpstrFilter = filter;
    dialog.lpstrFile = value; dialog.nMaxFile = static_cast<DWORD>(std::size(value));
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&dialog) ? fs::path(value) : fs::path();
}

std::wstring shellFailureText(INT_PTR code) {
    switch (code) {
    case 0: return L"Windowsが外部アプリを開始できませんでした。";
    case SE_ERR_FNF: return L"対象ファイルが見つかりません。";
    case SE_ERR_PNF: return L"対象フォルダーが見つかりません。";
    case SE_ERR_ACCESSDENIED: return L"対象を開く権限がありません。";
    case SE_ERR_OOM: return L"メモリまたはシステムリソースが不足しています。";
    case SE_ERR_DLLNOTFOUND: return L"関連付けに必要なDLLが見つかりません。";
    case SE_ERR_NOASSOC: return L"この種類を開くアプリがWindowsに関連付けられていません。";
    default: return L"Windows error " + std::to_wstring(code);
    }
}

bool executeShellTarget(std::wstring_view target, const fs::path& workingDirectory,
                        INT_PTR& failureCode) {
    const std::wstring stableTarget(target);
    const HINSTANCE launched = ShellExecuteW(
        app.window, L"open", stableTarget.c_str(), nullptr,
        workingDirectory.empty() ? nullptr : workingDirectory.c_str(), SW_SHOWNORMAL);
    failureCode = reinterpret_cast<INT_PTR>(launched);
    return failureCode > 32;
}

bool executeDefaultBrowser(std::wstring_view url, INT_PTR& failureCode,
                           std::wstring& browserPath) {
    wchar_t executable[32768]{};
    DWORD characters = static_cast<DWORD>(std::size(executable));
    const HRESULT association = AssocQueryStringW(
        ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L"https", L"open",
        executable, &characters);
    if (FAILED(association) || executable[0] == L'\0' ||
        !fs::is_regular_file(fs::path(executable))) {
        failureCode = SE_ERR_NOASSOC;
        return false;
    }
    browserPath = executable;
    const std::wstring parameters = quoteArgument(std::wstring(url));
    const HINSTANCE launched = ShellExecuteW(
        app.window, L"open", executable, parameters.c_str(),
        fs::path(executable).parent_path().c_str(), SW_SHOWNORMAL);
    failureCode = reinterpret_cast<INT_PTR>(launched);
    return failureCode > 32;
}

void shellOpen(const fs::path& path) {
    if (path.empty() || !fs::exists(path)) {
        MessageBoxW(app.window, L"対象ファイルがまだ作成されていません。",
                    L"ぷよテト研究", MB_OK | MB_ICONWARNING); return;
    }
    INT_PTR failure = 0;
    if (!executeShellTarget(path.wstring(), path.parent_path(), failure)) {
        const std::wstring message = L"対象を開けませんでした。\r\n" +
            shellFailureText(failure) + L"\r\n\r\n" + path.wstring();
        MessageBoxW(app.window, message.c_str(), L"ぷよテト研究", MB_OK | MB_ICONERROR);
    }
}

void openSimulatorShortcut(const fs::path& shortcut) {
    if (shortcut.empty() || !fs::is_regular_file(shortcut)) {
        MessageBoxW(app.window, L"シミュレータのリンクがまだ作成されていません。\r\n"
                                L"「修正を再判定」を実行してください。",
                    L"ぷよテト研究", MB_OK | MB_ICONWARNING);
        return;
    }

    // .urlのファイル関連付けは環境によって無反応になるため、中のURLを直接
    // 既定ブラウザへ渡す。生成URLはASCII percent-encodingなのでINI APIで安全に読める。
    std::vector<wchar_t> value(32768, L'\0');
    const DWORD length = GetPrivateProfileStringW(
        L"InternetShortcut", L"URL", L"", value.data(),
        static_cast<DWORD>(value.size()), shortcut.c_str());
    std::wstring url(value.data(), static_cast<std::size_t>(length));
    while (!url.empty() && (url.back() == L' ' || url.back() == L'\t' ||
                            url.back() == L'\r' || url.back() == L'\n')) url.pop_back();
    const std::size_t first = url.find_first_not_of(L" \t\r\n");
    if (first != std::wstring::npos && first != 0) url.erase(0, first);

    // file:///...#p1 をShellExecuteへ直接渡すと、一部のChromium系環境は
    // 成功コードを返しながら空タブだけを開く。HTTPSの既定ブラウザ実体へURLを
    // 引数として渡せば、ローカル振り分けHTMLのfragmentが確実に保持される。
    INT_PTR browserFailure = 0;
    std::wstring browserPath;
    if (!url.empty() && executeDefaultBrowser(url, browserFailure, browserPath)) return;

    INT_PTR directFailure = 0;
    if (!url.empty() && executeShellTarget(url, shortcut.parent_path(), directFailure)) return;

    // URLの直接起動に失敗した場合だけ、WindowsのInternet Shortcut関連付けも試す。
    INT_PTR shortcutFailure = 0;
    if (executeShellTarget(shortcut.wstring(), shortcut.parent_path(), shortcutFailure)) return;

    std::wostringstream message;
    message << L"シミュレータを既定ブラウザで開けませんでした。\r\n";
    if (url.empty()) {
        message << L"リンクファイル内にURL=がありません。";
    } else {
        message << L"既定ブラウザ直接起動: " << shellFailureText(browserFailure) << L"\r\n"
                << L"URL関連付け起動: " << shellFailureText(directFailure) << L"\r\n"
                << L".url起動: " << shellFailureText(shortcutFailure);
    }
    message << L"\r\n\r\n" << shortcut.wstring();
    MessageBoxW(app.window, message.str().c_str(), L"ぷよテト研究", MB_OK | MB_ICONERROR);
}

fs::path latestManifest() {
    const fs::path batches = app.runtime / L"data" / L"batches";
    std::error_code error;
    fs::path best; fs::file_time_type bestTime{};
    if (!fs::is_directory(batches, error)) return {};
    for (fs::recursive_directory_iterator it(batches, fs::directory_options::skip_permission_denied, error), end;
         it != end; it.increment(error)) {
        if (error) { error.clear(); continue; }
        if (!it->is_regular_file(error) || it->path().filename() != L"batch-results.json") continue;
        const auto time = it->last_write_time(error);
        if (!error && (best.empty() || time > bestTime)) { best = it->path(); bestTime = time; }
    }
    return best;
}

void layout(HWND window) {
    RECT area{}; GetClientRect(window, &area);
    const int width = area.right; const int height = area.bottom;
    const int margin = 12; const int gap = 8; const int button = 116;
    int x = margin; int y = margin;
    int sourceWidth = std::max(240, width - margin * 2 - button * 3 - gap * 3);
    MoveWindow(app.source, x, y, sourceWidth, 30, TRUE); x += sourceWidth + gap;
    MoveWindow(app.analyze, x, y, button, 30, TRUE); x += button + gap;
    MoveWindow(app.open, x, y, button, 30, TRUE); x += button + gap;
    MoveWindow(app.refresh, x, y, button, 30, TRUE);
    y += 38;
    MoveWindow(app.progress, margin, y, width - margin * 2, 18, TRUE);
    y += 22;
    MoveWindow(app.status, margin, y, width - margin * 2, 22, TRUE);
    y += 28;
    MoveWindow(app.search, margin, y, 280, 28, TRUE);
    MoveWindow(app.sort, margin + 288, y, 220, 250, TRUE);
    y += 36;
    const int rightWidth = std::min(360, std::max(300, width / 3));
    const int listWidth = std::max(420, width - margin * 2 - rightWidth - gap);
    MoveWindow(app.list, margin, y, listWidth, height - y - margin, TRUE);
    const int rx = margin + listWidth + gap;
    const int half = (rightWidth - gap) / 2;
    int ry = y;
    MoveWindow(app.detail, rx, ry, rightWidth, 120, TRUE); ry += 128;
    MoveWindow(app.p1Name, rx, ry, rightWidth, 28, TRUE); ry += 34;
    MoveWindow(app.p1NamePreview, rx, ry, rightWidth, 32, TRUE);
    MoveWindow(app.p1NamePreviewHint, rx, ry, rightWidth, 32, TRUE); ry += 36;
    MoveWindow(app.p2Name, rx, ry, rightWidth, 28, TRUE); ry += 34;
    MoveWindow(app.p2NamePreview, rx, ry, rightWidth, 32, TRUE);
    MoveWindow(app.p2NamePreviewHint, rx, ry, rightWidth, 32, TRUE); ry += 38;
    MoveWindow(app.checkNameImages, rx, ry, half, 30, TRUE);
    MoveWindow(app.selectImageGroup, rx + half + gap, ry, half, 30, TRUE); ry += 36;
    MoveWindow(app.saveNames, rx, ry, rightWidth, 30, TRUE); ry += 38;
    MoveWindow(app.saveAllNames, rx, ry, rightWidth, 30, TRUE); ry += 36;
    MoveWindow(app.review, rx, ry, rightWidth, 34, TRUE); ry += 42;
    MoveWindow(app.p1Simulator, rx, ry, half, 32, TRUE);
    MoveWindow(app.p2Simulator, rx + half + gap, ry, half, 32, TRUE); ry += 36;
    MoveWindow(app.bothSimulator, rx, ry, rightWidth, 34, TRUE); ry += 40;
    MoveWindow(app.markOkay, rx, ry, half, 34, TRUE);
    MoveWindow(app.confirmBroken, rx + half + gap, ry, half, 34, TRUE); ry += 42;
    MoveWindow(app.openFolder, rx, ry, rightWidth, 32, TRUE);
}

HWND makeControl(DWORD extended, const wchar_t* type, const wchar_t* text,
                 DWORD style, int id) {
    DWORD finalStyle = style;
    if (labtheme::enabled() && std::wstring_view(type) == L"BUTTON") finalStyle |= BS_OWNERDRAW;
    HWND control = CreateWindowExW(extended, type, text, finalStyle | WS_CHILD | WS_VISIBLE,
                                   0, 0, 10, 10, app.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   app.instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(app.font), TRUE);
    return control;
}

void createControls() {
    app.font = CreateFontW(labtheme::enabled() ? -15 : -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
    app.source = makeControl(WS_EX_CLIENTEDGE, L"EDIT", L"動画のURL、または動画ファイルのパス",
                             ES_AUTOHSCROLL, IdSource);
    SendMessageW(app.source, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"URLを貼るか「解析する」で動画を選ぶ"));
    app.analyze = makeControl(0, L"BUTTON", L"解析する", BS_PUSHBUTTON, IdAnalyze);
    app.open = makeControl(0, L"BUTTON", L"プロジェクトを開く", BS_PUSHBUTTON, IdOpen);
    app.refresh = makeControl(0, L"BUTTON", L"修正を再判定", BS_PUSHBUTTON, IdRefresh);
    app.progress = makeControl(0, PROGRESS_CLASSW, L"", PBS_SMOOTH, IdProgress);
    SendMessageW(app.progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    app.status = makeControl(0, L"STATIC", L"動画またはURLから開始できます", SS_LEFT, IdStatus);
    app.search = makeControl(0, L"EDIT", L"", ES_AUTOHSCROLL, IdSearch);
    SendMessageW(app.search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"動画名・試合番号・プレイヤー名・状態で絞り込み"));
    app.sort = makeControl(0, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, IdSort);
    for (const wchar_t* item : {L"試合順", L"P1名順", L"P2名順", L"警告を先頭", L"動画名順",
                                L"画像グループ順"})
        SendMessageW(app.sort, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(app.sort, CB_SETCURSEL, 0, 0);
    app.list = makeControl(0, WC_LISTVIEWW, L"",
                           LVS_REPORT | LVS_SHOWSELALWAYS | LVS_OWNERDRAWFIXED, IdList);
    ListView_SetExtendedListViewStyle(app.list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    SetWindowSubclass(app.list, listSelectionProcedure, 1, 0);
    const wchar_t* columns[] = {L"#", L"動画", L"P1", L"P2", L"名前画像", L"P1組", L"P2組",
                                L"状態", L"開始", L"長さ", L"最低一致"};
    const int widths[] = {40, 96, 78, 78, 288, 46, 46, 88, 70, 66, 62};
    for (int i = 0; i < 11; ++i) {
        LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(columns[i]); column.cx = widths[i]; column.iSubItem = i;
        ListView_InsertColumn(app.list, i, &column);
    }
    app.detail = makeControl(0, L"EDIT", L"試合を選択してください",
                             ES_MULTILINE | ES_READONLY | WS_VSCROLL, IdDetail);
    app.p1Name = makeControl(0, L"EDIT", L"", ES_AUTOHSCROLL, IdP1Name);
    SendMessageW(app.p1Name, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"P1 プレイヤー名"));
    app.p2Name = makeControl(0, L"EDIT", L"", ES_AUTOHSCROLL, IdP2Name);
    SendMessageW(app.p2Name, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"P2 プレイヤー名"));
    app.p1NamePreview = makeControl(0, L"STATIC", L"",
                                    SS_BITMAP | SS_CENTERIMAGE, IdP1NamePreview);
    app.p1NamePreviewHint = makeControl(0, L"STATIC",
                                        L"試合を選択すると名前部分の画像を表示します",
                                        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                                        IdP1NamePreviewHint);
    app.p2NamePreview = makeControl(0, L"STATIC", L"",
                                    SS_BITMAP | SS_CENTERIMAGE, IdP2NamePreview);
    app.p2NamePreviewHint = makeControl(0, L"STATIC", L"",
                                        SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                                        IdP2NamePreviewHint);
    ShowWindow(app.p1NamePreview, SW_HIDE);
    ShowWindow(app.p2NamePreview, SW_HIDE);
    ShowWindow(app.p2NamePreviewHint, SW_HIDE);
    app.saveNames = makeControl(0, L"BUTTON", L"プレイヤー名を保存", BS_PUSHBUTTON, IdSaveNames);
    app.saveAllNames = makeControl(0, L"BUTTON", L"全試合の名前を一括置換", BS_PUSHBUTTON, IdSaveAllNames);
    app.review = makeControl(0, L"BUTTON", L"盤面解析を修正する", BS_PUSHBUTTON, IdReview);
    app.p1Simulator = makeControl(0, L"BUTTON", L"P1シミュレータ", BS_PUSHBUTTON, IdP1Simulator);
    app.p2Simulator = makeControl(0, L"BUTTON", L"P2シミュレータ", BS_PUSHBUTTON, IdP2Simulator);
    app.bothSimulator = makeControl(0, L"BUTTON", L"2P盤面をシミュレータで開く", BS_PUSHBUTTON, IdBothSimulator);
    app.markOkay = makeControl(0, L"BUTTON", L"問題なし", BS_PUSHBUTTON, IdMarkOkay);
    app.confirmBroken = makeControl(0, L"BUTTON", L"壊れている", BS_PUSHBUTTON, IdConfirmBroken);
    app.openFolder = makeControl(0, L"BUTTON", L"この試合の保存先を開く", BS_PUSHBUTTON, IdOpenFolder);
    app.checkNameImages = makeControl(0, L"BUTTON", L"名前画像を照合",
                                      BS_PUSHBUTTON, IdCheckNameImages);
    app.selectImageGroup = makeControl(0, L"BUTTON", L"同じ画像を選択",
                                       BS_PUSHBUTTON, IdSelectImageGroup);
    setBusy(false);
}

void beginAnalysis(const fs::path& video) {
    setText(app.source, video.wstring());
    startJob([video] { return analyzeVideo(video, JobKind::Analysis); });
}

// comctl32 v5 discards custom-draw output in report mode, so the list is owner
// drawn.  Each row: background, the text of every column, and the two name
// strips in the "名前画像" column.
std::wstring listCellText(int row, int column) {
    wchar_t buffer[512] = {};
    LVITEMW item{};
    item.iSubItem = column;
    item.cchTextMax = static_cast<int>(std::size(buffer));
    item.pszText = buffer;
    SendMessageW(app.list, LVM_GETITEMTEXTW, static_cast<WPARAM>(row),
                 reinterpret_cast<LPARAM>(&item));
    return buffer;
}

// Buttons and the page backdrop follow the Lab palette: a very light page,
// white surfaces with 1px #d3dfe3 frames, and one teal primary action.
bool isPrimaryButton(int id) {
    return id == IdSaveNames;
}

bool drawThemedButton(const DRAWITEMSTRUCT& item) {
    if (!labtheme::enabled() || !item.hDC || item.CtlType != ODT_BUTTON) return false;
    const int id = GetDlgCtrlID(item.hwndItem);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = isPrimaryButton(id);
    wchar_t text[512]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    RECT rect = item.rcItem;
    COLORREF fill = labtheme::Surface;
    COLORREF border = labtheme::Border;
    COLORREF textColor = labtheme::SecondaryText;
    if (disabled) {
        fill = labtheme::DisabledFill;
        textColor = labtheme::Muted;
    } else if (primary) {
        fill = pressed ? labtheme::AccentActive : labtheme::Accent;
        border = fill;
        textColor = labtheme::AccentText;
    } else if (pressed) {
        fill = labtheme::HoverFill;
        border = labtheme::Accent;
        textColor = labtheme::SecondaryText;
    }
    labtheme::roundedRect(item.hDC, rect, labtheme::Radius, fill, border);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    HGDIOBJ oldFont = SelectObject(item.hDC, app.font ? app.font : labtheme::font());
    RECT textRect = rect;
    InflateRect(&textRect, -8, 0);
    DrawTextW(item.hDC, text, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item.hDC, oldFont);
    if ((item.itemState & ODS_FOCUS) && !disabled) {
        RECT focus = rect;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
    return true;
}

void paintStudyBackdrop(HWND window, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, labtheme::backgroundBrush());
    if (!labtheme::enabled()) return;
    const HWND framed[] = {app.list, app.detail, app.p1Name, app.p2Name,
                           app.p1NamePreview, app.p2NamePreview, app.search};
    HBRUSH border = CreateSolidBrush(labtheme::Border);
    for (HWND control : framed) {
        if (!control || !IsWindowVisible(control)) continue;
        RECT rect{};
        if (!GetWindowRect(control, &rect)) continue;
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
        --rect.left;
        --rect.top;
        ++rect.right;
        ++rect.bottom;
        FrameRect(dc, &rect, border);
    }
    DeleteObject(border);
}

void drawListRow(const DRAWITEMSTRUCT& drawItem) {
    if (drawItem.itemID == static_cast<UINT>(-1) || !app.list) return;
    const int row = static_cast<int>(drawItem.itemID);
    HDC dc = drawItem.hDC;
    RECT rowRect = drawItem.rcItem;
    const bool selected = (drawItem.itemState & ODS_SELECTED) != 0;
    const bool themed = labtheme::enabled();
    const COLORREF background = themed
        ? (selected ? labtheme::SelectionFill : labtheme::Surface)
        : GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW);
    const COLORREF foreground = themed
        ? (selected ? labtheme::SelectionText : labtheme::TextColor)
        : GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &rowRect, brush);
    DeleteObject(brush);

    const HWND header = ListView_GetHeader(app.list);
    const int columnCount = header ? Header_GetItemCount(header) : 0;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, foreground);
    HGDIOBJ previousFont = SelectObject(dc, app.font);
    int imageX = -1;
    int imageRight = -1;
    int imageY = -1;
    for (int column = 0; column < columnCount; ++column) {
        RECT cell{};
        cell.left = column;
        if (!ListView_GetSubItemRect(app.list, row, column, LVIR_BOUNDS, &cell)) continue;
        RECT visible{};
        if (!IntersectRect(&visible, &cell, &rowRect)) continue;
        if (column == NameImageColumn) {
            imageX = cell.left;
            imageRight = cell.right;
            imageY = cell.top;
            continue;
        }
        RECT textRect = cell;
        textRect.left += 6;
        textRect.right -= 2;
        IntersectRect(&textRect, &textRect, &rowRect);
        if (textRect.right <= textRect.left) continue;
        const std::wstring text = listCellText(row, column);
        if (text.empty()) continue;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, previousFont);

    if (imageX >= 0) {
        LVITEMW entry{};
        entry.mask = LVIF_PARAM;
        entry.iItem = row;
        const auto foundImage = ListView_GetItem(app.list, &entry)
            ? app.nameImages.find(app.matches[static_cast<std::size_t>(entry.lParam)].key)
            : app.nameImages.end();
        if (foundImage != app.nameImages.end()) {
            const int gap = 6;
            const int left = imageX + std::max(2, ((imageRight - imageX) -
                static_cast<int>(NameThumbWidth) * 2 - gap) / 2);
            const int stripTop = imageY + std::max(0L,
                ((rowRect.bottom - rowRect.top) - NameThumbHeight) / 2);
            const auto drawStrip = [&](HBITMAP bitmap, int x) {
                if (!bitmap) return;
                HDC memory = CreateCompatibleDC(dc);
                if (!memory) return;
                HGDIOBJ previous = SelectObject(memory, bitmap);
                BitBlt(dc, x, stripTop, NameThumbWidth, NameThumbHeight, memory, 0, 0,
                       SRCCOPY);
                SelectObject(memory, previous);
                DeleteDC(memory);
            };
            drawStrip(foundImage->second.p1.thumbnail, left);
            drawStrip(foundImage->second.p2.thumbnail, left + NameThumbWidth + gap);
        }
    }
    // Grid lines, matching LVS_EX_GRIDLINES.
    HPEN pen = CreatePen(PS_SOLID, 1, themed ? labtheme::Border : GetSysColor(COLOR_3DLIGHT));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, rowRect.left, rowRect.bottom - 1, nullptr);
    LineTo(dc, rowRect.right, rowRect.bottom - 1);
    for (int column = 1; column < columnCount; ++column) {
        RECT cell{};
        cell.left = column;
        if (!ListView_GetSubItemRect(app.list, row, column, LVIR_BOUNDS, &cell)) continue;
        MoveToEx(dc, cell.left, rowRect.top, nullptr);
        LineTo(dc, cell.left, rowRect.bottom - 1);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void handleCommand(int id, int notification) {
    if (id == IdSearch && notification == EN_CHANGE) { populateList(); return; }
    if (id == IdSort && notification == CBN_SELCHANGE) { populateList(); return; }
    // WM_COMMAND is also emitted for EDIT/COMBO notifications during control
    // creation (for example EN_UPDATE).  Only button clicks belong below.
    if (notification != BN_CLICKED) return;
    if (app.busy.load() && (id == IdSaveNames || id == IdSaveAllNames)) return;
    // One entry point: a URL is downloaded, anything else opens the file picker.
    // The engine decides the container/codec path, so the UI never asks.
    if (id == IdAnalyze || id == IdDownload) {
        const std::wstring source = trimWhitespace(controlText(app.source));
        if (source.find(L"://") != std::wstring::npos) {
            startJob([source] { return downloadAndAnalyze(source); });
            return;
        }
        const wchar_t filter[] = L"動画ファイル\0*.mp4;*.mov;*.m4v;*.mkv;*.avi;*.webm\0すべてのファイル\0*.*\0\0";
        const fs::path path = chooseFile(app.window, L"解析する配信動画を選択", filter);
        if (!path.empty()) beginAnalysis(path);
    } else if (id == IdOpen) {
        const wchar_t filter[] = L"ぷよテト研究プロジェクト\0*.tstudy;batch-results.json\0すべてのJSON\0*.json\0\0";
        const fs::path path = chooseFile(app.window, L"研究プロジェクトを開く", filter);
        if (!path.empty()) { std::wstring error; if (!loadManifestOrProject(path, error))
            MessageBoxW(app.window, error.c_str(), L"ぷよテト研究", MB_OK | MB_ICONERROR); }
    } else if (id == IdRefresh) {
        startJob([] { return refreshProject(); });
    } else if (id == IdCheckNameImages) {
        if (app.matches.empty()) return;
        startJob([] { return collectNameImages(); });
    } else if (id == IdSaveNames || id == IdSaveAllNames) {
        const bool all = id == IdSaveAllNames;
        const std::vector<MatchItem> selection = all ? app.matches : selectedMatches();
        if (selection.empty()) return;
        const std::wstring p1 = trimWhitespace(controlText(app.p1Name));
        const std::wstring p2 = trimWhitespace(controlText(app.p2Name));
        if (p1.empty() && p2.empty()) {
            MessageBoxW(app.window,
                L"P1またはP2の変更したい側へ名前を入力してください。\r\n"
                L"空欄側は現在の名前を保持します。",
                L"ぷよテト研究", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (all) {
            const std::wstring message = L"このプロジェクトの全 " + std::to_wstring(selection.size()) +
                L" 試合の名前を一括置換します。\r\n絞り込み・選択に関係なく、全試合が対象です。\r\n\r\n"
                L"1P: " + (p1.empty() ? L"（変更しない）" : p1) + L"\r\n2P: " +
                (p2.empty() ? L"（変更しない）" : p2);
            if (MessageBoxW(app.window, message.c_str(), L"全試合の名前を一括置換",
                            MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
        }
        startJob([selection, p1, p2] { return saveNamesForMatches(selection, p1, p2); });
    } else {
        MatchItem* item = selectedMatch();
        if (!item) return;
        const MatchItem copy = *item;
        if (id == IdReview) {
            const fs::path reviewer = app.runtime / L"bin" / L"TetrisVideoRecovery_review.exe";
            if (fs::is_regular_file(reviewer) && fs::is_regular_file(copy.autosave)) {
                ShellExecuteW(app.window, L"open", reviewer.c_str(),
                              quoteArgument(copy.autosave.wstring()).c_str(),
                              reviewer.parent_path().c_str(), SW_SHOWNORMAL);
                app.reviewerOpened = true;
            } else MessageBoxW(app.window, L"修正画面またはautosaveが見つかりません。",
                               L"ぷよテト研究", MB_OK | MB_ICONERROR);
        } else if (id == IdP1Simulator) openSimulatorShortcut(copy.p1Shortcut);
        else if (id == IdP2Simulator) openSimulatorShortcut(copy.p2Shortcut);
        else if (id == IdBothSimulator) openSimulatorShortcut(copy.combinedShortcut);
        else if (id == IdMarkOkay) {
            app.decisions[copy.key] = Decision{L"okay", {}};
            saveProject(); populateList(); updateDetails();
        } else if (id == IdConfirmBroken) {
            startJob([copy] { return exportBrokenMatch(copy); });
        } else if (id == IdOpenFolder) {
            shellOpen(copy.autosave.parent_path().parent_path());
        } else if (id == IdSelectImageGroup) {
            const int p1Group = nameImageGroupOf(copy.key, 0);
            const int p2Group = nameImageGroupOf(copy.key, 1);
            if (p1Group <= 0 || p2Group <= 0) {
                MessageBoxW(app.window,
                    L"この試合の名前画像はまだ照合していません。\r\n"
                    L"「名前画像を照合（全試合）」を先に実行してください。",
                    L"ぷよテト研究", MB_OK | MB_ICONINFORMATION);
                return;
            }
            app.changingSelection = true;
            ListView_SetItemState(app.list, -1, 0, LVIS_SELECTED);
            std::size_t selectedCount = 0;
            for (std::size_t row = 0; row < app.visible.size(); ++row) {
                const MatchItem& candidate = app.matches[app.visible[row]];
                if (nameImageGroupOf(candidate.key, 0) != p1Group) continue;
                if (nameImageGroupOf(candidate.key, 1) != p2Group) continue;
                ListView_SetItemState(app.list, static_cast<int>(row),
                                      LVIS_SELECTED, LVIS_SELECTED);
                ++selectedCount;
            }
            app.changingSelection = false;
            std::size_t groupCount = 0;
            for (const auto& [key, pair] : app.nameImages) {
                (void)key;
                if (pair.p1.group == p1Group && pair.p2.group == p2Group) ++groupCount;
            }
            updateDetails();
            std::wstring message = L"同じ画像の " + std::to_wstring(selectedCount) +
                L" 試合を選択しました。名前を入力して保存すると、この組だけをまとめて変更できます。";
            if (groupCount > selectedCount)
                message += L"（絞り込みで非表示の " +
                    std::to_wstring(groupCount - selectedCount) + L" 試合は選択できません）";
            setText(app.status, message);
        }
    }
}

LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        app.window = window; createControls(); layout(window); return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(window);
        limits->ptMinTrackSize.x = MulDiv(1040, static_cast<int>(dpi), 96);
        limits->ptMinTrackSize.y = MulDiv(800, static_cast<int>(dpi), 96);
        return 0;
    }
    case WM_SIZE: layout(window); return 0;
    case WM_COMMAND: handleCommand(LOWORD(wParam), HIWORD(wParam)); return 0;
    case WM_ERASEBKGND:
        if (labtheme::enabled()) {
            RECT area{};
            GetClientRect(window, &area);
            FillRect(reinterpret_cast<HDC>(wParam), &area, labtheme::backgroundBrush());
            return 1;
        }
        break;
    case WM_PAINT:
        if (labtheme::enabled()) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            paintStudyBackdrop(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (labtheme::enabled()) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, labtheme::TextColor);
            SetBkColor(dc, labtheme::Background);
            return reinterpret_cast<LRESULT>(labtheme::backgroundBrush());
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (labtheme::enabled()) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, labtheme::TextColor);
            SetBkColor(dc, labtheme::Surface);
            return reinterpret_cast<LRESULT>(labtheme::surfaceBrush());
        }
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* drawItem = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (drawItem && drawItem->CtlID == IdList) {
            drawListRow(*drawItem);
            return TRUE;
        }
        if (drawItem && drawThemedButton(*drawItem)) return TRUE;
        break;
    }
    case WM_NOTIFY: {
        const NMHDR* header = reinterpret_cast<NMHDR*>(lParam);
        if (header->idFrom == IdList && header->code == LVN_ITEMCHANGED && !app.changingSelection) updateDetails();
        if (header->idFrom == IdList && header->code == NM_DBLCLK) {
            MatchItem* item = selectedMatch();
            if (item) openSimulatorShortcut(item->combinedShortcut);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wParam) != WA_INACTIVE && app.reviewerOpened && !app.busy) {
            app.reviewerOpened = false;
            startJob([] { return refreshProject(); });
        }
        return 0;
    case WM_STUDY_PROGRESS: {
        std::unique_ptr<ProgressPayload> payload(reinterpret_cast<ProgressPayload*>(lParam));
        SendMessageW(app.progress, PBM_SETPOS, payload->percent, 0);
        setText(app.status, payload->text); return 0;
    }
    case WM_STUDY_DONE: {
        std::unique_ptr<JobResult> result(reinterpret_cast<JobResult*>(lParam));
        app.busy = false; setBusy(false);
        if (result->exitCode == 0) {
            if ((result->kind == JobKind::Analysis || result->kind == JobKind::DownloadAndAnalysis) &&
                fs::is_regular_file(result->manifest)) {
                std::wstring error;
                if (!loadManifestOrProject(result->manifest, error)) {
                    MessageBoxW(window, error.c_str(), L"ぷよテト研究", MB_OK | MB_ICONERROR);
                } else if (app.matches.empty()) {
                    // Never leave an empty result looking like a success.
                    MessageBoxW(window,
                        L"解析は終わりましたが、試合を1件も検出できませんでした。\r\n"
                        L"・配信の試合区間が検出できていない可能性があります\r\n"
                        L"・解析ログを確認してください",
                        L"ぷよテト研究", MB_OK | MB_ICONWARNING);
                    setText(app.status, L"試合を1件も検出できませんでした");
                }
            } else if (result->kind == JobKind::Export) {
                app.decisions[result->matchIndex] = Decision{L"broken", result->output};
                saveProject(); populateList(); updateDetails();
                setText(app.status, L"壊れた試合全体を改善用動画へ保存しました（前後余白なし）");
            } else if (result->kind == JobKind::NameImages) {
                applyNameImageResults(*result);
                populateList(); updateDetails();
                setText(app.status, nameImageSummary(result->nameImageFailures));
            } else {
                std::wstring error;
                if (!app.manifest.empty()) loadManifestOrProject(app.project, error);
                if (!error.empty()) MessageBoxW(window, error.c_str(), L"ぷよテト研究", MB_OK | MB_ICONERROR);
                else if (!result->message.empty()) setText(app.status, result->message);
            }
            SendMessageW(app.progress, PBM_SETPOS, 100, 0);
        } else {
            SendMessageW(app.progress, PBM_SETPOS, 0, 0);
            const std::wstring error = result->error.empty() ? L"処理に失敗しました。" : result->error;
            if (result->kind == JobKind::Names && !app.project.empty()) {
                std::wstring reloadError;
                loadManifestOrProject(app.project, reloadError);
                if (!reloadError.empty()) {
                    setText(app.status, reloadError);
                    MessageBoxW(window, reloadError.c_str(), L"ぷよテト研究",
                                MB_OK | MB_ICONERROR);
                }
            }
            setText(app.status, error);
            MessageBoxW(window, error.c_str(), L"ぷよテト研究", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_STUDY_PREVIEW_DONE: {
        std::unique_ptr<PreviewPayload> payload(reinterpret_cast<PreviewPayload*>(lParam));
        if (!payload) return 0;
        if (payload->generation == app.previewGeneration.load(std::memory_order_relaxed)) {
            applyNamePreview(app.p1NamePreview, app.p1NamePreviewHint,
                             app.p1NamePreviewBitmap, payload->p1,
                             L"P1 名前画像: " + payload->p1Message);
            payload->p1 = nullptr;
            applyNamePreview(app.p2NamePreview, app.p2NamePreviewHint,
                             app.p2NamePreviewBitmap, payload->p2,
                             L"P2 名前画像: " + payload->p2Message);
            payload->p2 = nullptr;
        }
        // A newer selection arrived while the worker was busy; start it now.
        if (app.previewRetryPending) {
            app.previewRetryPending = false;
            MatchItem* item = selectedMatch();
            if (item && app.detailSelection.size() == 1 && !app.busy.load())
                requestNameCropPreviews(*item, namePreviewWidth(app.p1NamePreview));
        }
        return 0;
    }
    case WM_STUDY_AUTOSTART:
        if (!app.autoAnalyze.empty()) beginAnalysis(app.autoAnalyze);
        else {
            const fs::path latest = latestManifest();
            if (!latest.empty()) { std::wstring error; loadManifestOrProject(latest, error); }
        }
        return 0;
    case WM_CLOSE:
        if (app.busy && MessageBoxW(window, L"処理中です。アプリを閉じますか？",
                                    L"ぷよテト研究", MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
        DestroyWindow(window); return 0;
    case WM_DESTROY:
        // Invalidate pending extractions and drop preview bitmaps before the
        // owning static controls go away.
        app.previewGeneration.fetch_add(1, std::memory_order_relaxed);
        releasePreviewBitmap(app.p1NamePreview, app.p1NamePreviewBitmap);
        releasePreviewBitmap(app.p2NamePreview, app.p2NamePreviewBitmap);
        releaseNameImages();
        saveProject(); if (app.font) DeleteObject(app.font); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    app.instance = instance;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    app.runtime = fs::path(executable).parent_path().parent_path();
    app.tools = app.runtime.parent_path();

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    fs::path initial;
    if (argc >= 3 && std::wstring_view(argv[1]) == L"--analyze") app.autoAnalyze = argv[2];
    else if (argc >= 2) initial = argv[1];
    if (argv) LocalFree(argv);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = WindowClass;
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) return 1;
    HWND window = CreateWindowExW(0, WindowClass, L"ぷよテト研究",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        1320, 820, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    ShowWindow(window, show); UpdateWindow(window);
    if (!initial.empty()) {
        std::wstring error;
        if (!loadManifestOrProject(initial, error))
            MessageBoxW(window, error.c_str(), L"ぷよテト研究", MB_OK | MB_ICONERROR);
    } else PostMessageW(window, WM_STUDY_AUTOSTART, 0, 0);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
