// Build in an x64 Visual Studio Developer Command Prompt for the x64 migemo.dll:
// cl /nologo /std:c++17 /EHsc /utf-8 /W4 kbddnd.cpp /link /SUBSYSTEM:CONSOLE
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <icu.h>
#pragma comment(lib, "icu")
#include <shlobj.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#pragma comment(lib, "shcore")
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <thread>
#include <filesystem>
#include <string>
#include <vector>
#pragma comment(lib, "user32")
#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")
#pragma comment(lib, "gdi32")
#pragma comment(lib, "dwmapi")
#pragma comment(lib, "uuid")

namespace {
using Microsoft::WRL::ComPtr;
constexpr COLORREF transparentColor = RGB(255, 0, 255);
constexpr wchar_t title[] = L"kbddnd";
std::vector<std::wstring> paths;
std::vector<HWND> targets;
size_t selected = 0;
HWND overlay;
bool dropping = false;
bool enterPressedHere = false;

enum class Mode { window, part, position };
Mode mode = Mode::window;
std::vector<HWND> parts;
size_t partIndex = 0;
POINT manual{}; // Target client coordinates
std::wstring notice;

POINT midpoint(const RECT& r) {
    return {r.left + (r.right-r.left)/2, r.top + (r.bottom-r.top)/2};
}

RECT clientOnScreen(HWND w) {
    RECT r{};
    if (GetClientRect(w, &r)) MapWindowPoints(w, nullptr, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

// A presence hint only. Never use a foreign process's interface pointer.
bool oleHint(HWND w) { return GetPropW(w, L"OleDropTargetInterface") != nullptr; }

RECT visiblePart(HWND part, HWND root) {
    RECT r = clientOnScreen(part);
    if (!IsChild(root, part) || !IsWindowVisible(part)) return {};
    for (HWND w = part; w; w = GetAncestor(w, GA_PARENT)) {
        RECT parent = clientOnScreen(w);
        if (!IsWindowEnabled(w) || !IntersectRect(&r, &r, &parent)) return {};
        if (w == root) return r;
    }
    return {};
}

void findParts(HWND root) {
    parts.clear();
    partIndex = 0;
    struct Entry { HWND window; POINT center; };
    struct Search { HWND root; std::vector<Entry> entries; } search{root, {}};
    EnumChildWindows(root, [](HWND child, LPARAM arg) -> BOOL {
        auto& search = *reinterpret_cast<Search*>(arg);
        if (oleHint(child) || (GetWindowLongPtrW(child, GWL_EXSTYLE) & WS_EX_ACCEPTFILES)) {
            RECT r = visiblePart(child, search.root);
            if (!IsRectEmpty(&r)) search.entries.push_back({child, midpoint(r)});
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    std::stable_sort(search.entries.begin(), search.entries.end(), [](const Entry& a, const Entry& b) {
        return a.center.y == b.center.y ? a.center.x < b.center.x : a.center.y < b.center.y;
    });
    for (const auto& entry : search.entries) parts.push_back(entry.window);
}

bool aim(HWND root, POINT& screen) {
    RECT r = clientOnScreen(root);
    if (IsRectEmpty(&r)) return false;
    if (mode == Mode::position) {
        screen = manual;
        return ClientToScreen(root, &screen) && PtInRect(&r, screen);
    }
    if (mode == Mode::part) r = visiblePart(parts[partIndex], root);
    screen = midpoint(r);
    return !IsRectEmpty(&r);
}

void chooseDirection(HWND root, int dx, int dy) {
    POINT from = midpoint(visiblePart(parts[partIndex], root));
    double best = 1e100;
    size_t next = partIndex;
    for (size_t i = 0; i < parts.size(); ++i) {
        RECT r = visiblePart(parts[i], root);
        if (IsRectEmpty(&r)) continue;
        POINT to = midpoint(r);
        double x = to.x-from.x, y = to.y-from.y;
        double along = x*dx + y*dy, across = x*dy - y*dx;
        if (along <= 0) continue;
        double score = along + across*across/along;
        if (score < best) { best = score; next = i; }
    }
    partIndex = next;
}

bool candidate(HWND w) {
    RECT r{};
    DWORD cloaked = 0;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return w != overlay && w != GetShellWindow() && IsWindowVisible(w) &&
        !IsIconic(w) && !cloaked && IsWindowEnabled(w) &&
        GetWindowRect(w, &r) && !IsRectEmpty(&r) &&
        MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
}

std::wstring searchText;

class WindowSearch {
    HMODULE dll = nullptr;
    void* engine = nullptr;
    using Query = unsigned char* (__cdecl*)(void*, const unsigned char*);
    using Release = void (__cdecl*)(void*, unsigned char*);
    using Close = void (__cdecl*)(void*);
    Query query = nullptr;
    Release release = nullptr;
    Close close = nullptr;
    URegularExpression* pattern = nullptr;
public:
    void initialize() {
        wchar_t exe[32768]{};
        if (!GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe))) return;
        auto folder = std::filesystem::path(exe).parent_path();
        const wchar_t* dllName = sizeof(void*) == 8 ? L"migemo.dll" : L"migemo32.dll";
        dll = LoadLibraryExW((folder / dllName).c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return;
        using Open = void* (__cdecl*)(const char*);
        using Enabled = int (__cdecl*)(void*);
        auto open = reinterpret_cast<Open>(GetProcAddress(dll, "migemo_open"));
        auto enabled = reinterpret_cast<Enabled>(GetProcAddress(dll, "migemo_is_enable"));
        query = reinterpret_cast<Query>(GetProcAddress(dll, "migemo_query"));
        release = reinterpret_cast<Release>(GetProcAddress(dll, "migemo_release"));
        close = reinterpret_cast<Close>(GetProcAddress(dll, "migemo_close"));
        if (!open || !enabled || !query || !release || !close) return;
        auto dict = folder / L"dict" / L"migemo-dict";
        wchar_t shortPath[32768]{};
        const wchar_t* path = GetShortPathNameW(dict.c_str(), shortPath, ARRAYSIZE(shortPath))
                            ? shortPath : dict.c_str();
        int size = WideCharToMultiByte(CP_ACP, 0, path, -1, nullptr, 0, nullptr, nullptr);
        std::string narrow(size, 0);
        if (!size || !WideCharToMultiByte(CP_ACP, 0, path, -1, narrow.data(), size, nullptr, nullptr)) return;
        engine = open(narrow.c_str());
        if (engine && !enabled(engine)) { close(engine); engine = nullptr; }
    }
    ~WindowSearch() {
        if (pattern) uregex_close(pattern);
        if (engine) close(engine);
        if (dll) FreeLibrary(dll);
    }
    bool available() const { return engine != nullptr; }

    void compile() {
        if (pattern) { uregex_close(pattern); pattern = nullptr; }
        if (!engine || searchText.empty()) return;
        std::string roman;
        for (wchar_t ch : searchText) {
            if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
            if (!(ch >= L'a' && ch <= L'z') && !(ch >= L'0' && ch <= L'9') && ch != L'-') return;
            roman.push_back(static_cast<char>(ch));
        }
        auto result = query(engine, reinterpret_cast<const unsigned char*>(roman.c_str()));
        if (!result) return;
        auto bytes = reinterpret_cast<const char*>(result);
        int size = MultiByteToWideChar(932, 0, bytes, -1, nullptr, 0);
        std::wstring expanded(size, 0);
        if (size > 1 && MultiByteToWideChar(932, 0, bytes, -1, expanded.data(), size)) {
            UErrorCode status = U_ZERO_ERROR;
            pattern = uregex_open(reinterpret_cast<const UChar*>(expanded.data()), size-1,
                                  UREGEX_CASE_INSENSITIVE, nullptr, &status);
        }
        release(engine, result);
    }
    bool matches(const std::wstring& value) const {
        if (value.empty()) return false;
        if (FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | NORM_IGNORECASE,
                            value.c_str(), -1, searchText.c_str(), -1, nullptr, nullptr, nullptr, 0) >= 0)
            return true;
        if (!pattern) return false;
        UErrorCode status = U_ZERO_ERROR;
        uregex_setText(pattern, reinterpret_cast<const UChar*>(value.data()), static_cast<int>(value.size()), &status);
        bool found = uregex_find(pattern, 0, &status) && U_SUCCESS(status);
        status = U_ZERO_ERROR;
        uregex_setText(pattern, u"", 0, &status);
        return found;
    }
} windowSearch;

std::wstring windowCaption(HWND w) {
    std::wstring name(static_cast<size_t>(GetWindowTextLengthW(w)) + 1, 0);
    name.resize(GetWindowTextW(w, name.data(), static_cast<int>(name.size())));
    return name;
}

std::wstring windowClass(HWND w) {
    wchar_t cls[256]{};
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    return cls;
}

std::wstring windowName(HWND w) {
    auto name = windowCaption(w);
    return name.empty() ? windowClass(w) : name;
}

bool matchesWindow(HWND w) {
    if (searchText.empty() || windowSearch.matches(windowName(w))) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768];
    DWORD length = ARRAYSIZE(path);
    bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != FALSE;
    CloseHandle(process);
    return ok && windowSearch.matches(std::filesystem::path(path).stem().wstring());
}

// Keep existing candidates in display order even if their Z-order changes.
void updateTargets(const std::vector<HWND>& found) {
    HWND previous = targets.empty() ? nullptr : targets[selected];
    targets.erase(std::remove_if(targets.begin(), targets.end(), [&](HWND w) {
        return std::find(found.begin(), found.end(), w) == found.end();
    }), targets.end());
    for (HWND w : found) {
        if (std::find(targets.begin(), targets.end(), w) == targets.end())
            targets.push_back(w);
    }
    auto pos = std::find(targets.begin(), targets.end(), previous);
    selected = pos == targets.end() ? 0 : static_cast<size_t>(pos - targets.begin());
}

void refresh() {
    std::vector<HWND> found;
    EnumWindows([](HWND w, LPARAM arg) -> BOOL {
        if (candidate(w) && matchesWindow(w))
            reinterpret_cast<std::vector<HWND>*>(arg)->push_back(w);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    updateTargets(found);
}

void paint(HWND w) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(w, &ps);
    RECT canvas{};
    GetClientRect(w, &canvas);
    SetDCBrushColor(dc, transparentColor);
    HBRUSH fill = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    FillRect(dc, &canvas, fill);
    RECT r{};
    std::wstring label = L"対象 (0/0)";
    if (!targets.empty() && GetWindowRect(targets[selected], &r)) {
        MapWindowPoints(nullptr, w, reinterpret_cast<POINT*>(&r), 2);
        HBRUSH brush = CreateSolidBrush(RGB(0, 220, 255));
        for (int i = 0; i < 4; ++i) {
            FrameRect(dc, &r, brush);
            InflateRect(&r, -1, -1);
        }
        DeleteObject(brush);
        label = L"対象 (" + std::to_wstring(selected + 1) + L"/" + std::to_wstring(targets.size()) + L")  " + windowName(targets[selected]);
    }
    if (!targets.empty()) {
        HWND root = targets[selected];
        if (mode == Mode::part) {
            HWND part = parts[partIndex];
            RECT bounds = visiblePart(part, root);
            POINT center = midpoint(bounds);
            bool hasPosition = !IsRectEmpty(&bounds) && ScreenToClient(root, &center);
            MapWindowPoints(nullptr, w, reinterpret_cast<POINT*>(&bounds), 2);
            HBRUSH brush = CreateSolidBrush(RGB(255, 190, 32));
            for (int i = 0; i < 3; ++i) {
                FrameRect(dc, &bounds, brush);
                InflateRect(&bounds, -1, -1);
            }
            DeleteObject(brush);
            label = L"部品 (" + std::to_wstring(partIndex + 1) + L"/" + std::to_wstring(parts.size()) + L")";
            if (hasPosition)
                label += L" (" + std::to_wstring(center.x) + L", " + std::to_wstring(center.y) + L")";
            auto cls = windowClass(part);
            auto caption = windowCaption(part);
            if (!cls.empty()) label += L" [" + cls + L"]";
            if (!caption.empty() && caption != cls) label += L" " + caption;
        }
        POINT at{};
        if (aim(root, at)) {
            ScreenToClient(w, &at);
            RECT h{at.x-10, at.y-1, at.x+11, at.y+2};
            RECT v{at.x-1, at.y-10, at.x+2, at.y+11};
            HBRUSH brush = CreateSolidBrush(RGB(255, 100, 20));
            FillRect(dc, &h, brush);
            FillRect(dc, &v, brush);
            DeleteObject(brush);
        }
        if (mode == Mode::position)
            label += L"  位置 (" + std::to_wstring(manual.x) + L", " + std::to_wstring(manual.y) + L")";
    }
    const wchar_t* modeName = L"ウィンドウ選択";
    const wchar_t* keys = L"←→ 選択  Tab 部品  Enter 送信  Esc 戻る";
    if (mode == Mode::part) {
        modeName = L"部品選択";
        keys = L"矢印 選択  Tab 位置  Enter 送信  Esc 戻る";
    } else if (mode == Mode::position) {
        modeName = L"位置選択";
        keys = L"矢印 移動  Ctrl 高速  Shift 1px  Enter 送信  Esc 戻る";
    }
    std::wstring heading = std::wstring(title) + L" [" + modeName + L"] " + searchText;
    SetWindowTextW(w, (heading + L" — " + label).c_str());
    MONITORINFO mi{sizeof(MONITORINFO), {}, {}, 0};
    GetMonitorInfoW(MonitorFromWindow(targets.empty() ? nullptr : targets[selected],
                                      MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT panel = mi.rcWork;
    MapWindowPoints(nullptr, w, reinterpret_cast<POINT*>(&panel), 2);
    InflateRect(&panel, -16, -16);
    LONG bottom = panel.bottom;
    panel.bottom = panel.top + 48 + (notice.empty() ? 0 : 32);
    POINT marker{};
    if (!targets.empty() && aim(targets[selected], marker) && ScreenToClient(w, &marker) && PtInRect(&panel, marker))
        OffsetRect(&panel, 0, bottom - panel.bottom);
    RECT bar = panel;
    bar.bottom = bar.top + 48;
    SetDCBrushColor(dc, RGB(0, 91, 165));
    FillRect(dc, &bar, fill);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    const UINT flags = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    InflateRect(&bar, -8, 0);
    LONG width = bar.right - bar.left;
    RECT keySize{};
    DrawTextW(dc, keys, -1, &keySize, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    LONG sideWidth = std::clamp(keySize.right + 8, width / 4, width * 3 / 8);
    RECT left = bar, center = bar, right = bar;
    left.right = bar.left + sideWidth;
    right.left = bar.right - sideWidth;
    center.left = left.right + 4;
    center.right = right.left - 4;
    DrawTextW(dc, heading.c_str(), -1, &left, flags | DT_LEFT);
    DrawTextW(dc, label.c_str(), -1, &center, flags | DT_CENTER);
    DrawTextW(dc, keys, -1, &right, flags | DT_RIGHT);
    if (!notice.empty()) {
        RECT message = panel;
        message.top = bar.bottom;
        SetDCBrushColor(dc, RGB(255, 246, 190));
        FillRect(dc, &message, fill);
        InflateRect(&message, -8, 0);
        SetTextColor(dc, RGB(0, 0, 0));
        DrawTextW(dc, notice.c_str(), -1, &message, flags | DT_LEFT);
    }
    EndPaint(w, &ps);
}

HWND rootAt(POINT p) { return GetAncestor(WindowFromPoint(p), GA_ROOT); }

// A renderer-backed target may initially return NONE while it processes DragOver.
class DropSource final : public IDropSource {
    ULONG refs = 1;
    HWND target;
    POINT point;
    DWORD effect = DROPEFFECT_NONE;
    ULONGLONG started = GetTickCount64();
    ULONGLONG copySince = 0;
public:
    DropSource(HWND w, POINT p) : target(w), point(p) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown && id != IID_IDropSource) return E_NOINTERFACE;
        *out = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = --refs;
        if (!left) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL esc, DWORD) override {
        POINT pos{};
        if (esc || !GetCursorPos(&pos) || (pos.x != point.x || pos.y != point.y) || rootAt(pos) != target)
            return DRAGDROP_S_CANCEL;
        const auto now = GetTickCount64();
        if ((effect & DROPEFFECT_COPY) && now - copySince >= 120) return DRAGDROP_S_DROP;
        return now - started < 2000 ? S_OK : DRAGDROP_S_CANCEL;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD next) override {
        if ((next & DROPEFFECT_COPY) && !(effect & DROPEFFECT_COPY)) copySince = GetTickCount64();
        effect = next;
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

std::wstring deliver(HWND target, POINT center, HWND expectedPart) {
    RECT bounds = clientOnScreen(target);
    if (!candidate(target) || !PtInRect(&bounds, center))
        return L"送信位置が変わりました。選び直してください。";
    HWND hit = WindowFromPoint(center);
    if (GetAncestor(hit, GA_ROOT) != target) {
        SetForegroundWindow(target);
        SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        if (!candidate(target) || !aim(target, center))
            return L"送信位置が変わりました。選び直してください。";
        hit = WindowFromPoint(center);
        if (GetAncestor(hit, GA_ROOT) != target)
            return L"対象を前面に表示しても送信位置を確認できません。";
    }
    if (expectedPart && hit != expectedPart && !IsChild(expectedPart, hit))
        return L"選んだ部品が隠れています。位置を選び直してください。";

    for (HWND w = hit; w; w = GetAncestor(w, GA_PARENT)) {
        if (oleHint(w)) break;
        if (GetWindowLongPtrW(w, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) {
            size_t bytes = sizeof(DROPFILES) + sizeof(wchar_t);
            for (const auto& s : paths) bytes += (s.size() + 1) * sizeof(wchar_t);
            HGLOBAL block = GlobalAlloc(GHND, bytes);
            auto* header = static_cast<DROPFILES*>(GlobalLock(block));
            if (!header) {
                GlobalFree(block);
                return L"ファイル一覧のメモリを確保できません。";
            }
            header->pFiles = sizeof(DROPFILES);
            header->fWide = TRUE;
            header->pt = center;
            ScreenToClient(w, &header->pt);
            auto* dest = reinterpret_cast<wchar_t*>(header + 1);
            for (const auto& s : paths) {
                std::memcpy(dest, s.c_str(), (s.size() + 1) * sizeof(wchar_t));
                dest += s.size() + 1;
            }
            GlobalUnlock(block);
            if (PostMessageW(w, WM_DROPFILES, reinterpret_cast<WPARAM>(block), 0))
                return {}; // Windows marshals the posted HDROP. The receiver calls DragFinish.
            GlobalFree(block);
            return L"対象ウィンドウに送信できませんでした。";
        }
        if (w == target) break;
    }

    std::vector<PCIDLIST_ABSOLUTE> items;
    HRESULT hr = S_OK;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE item = nullptr;
        hr = SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr);
        if (FAILED(hr)) break;
        items.push_back(item);
    }
    ComPtr<IShellItemArray> array;
    if (SUCCEEDED(hr))
        hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(items.size()), items.data(), &array);
    for (auto item : items) CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(item));
    ComPtr<IDataObject> data;
    if (SUCCEEDED(hr)) hr = array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
    if (FAILED(hr)) return L"ファイルのドロップデータを作成できません。";
    POINT saved{};
    if (!GetCursorPos(&saved) || !SetCursorPos(center.x, center.y))
        return L"指定位置にポインターを移動できません。";
    ComPtr<IDropSource> source;
    source.Attach(new DropSource(target, center));
    DWORD effect = 0;
    // A stationary keyboard-driven drag has no mouse transitions. Wake only
    // the source thread's capture window, using its actual client coordinates.
    const DWORD sourceThread = GetCurrentThreadId();
    std::atomic_bool finished = false;
    std::thread wake([&] {
        while (!finished.load()) {
            Sleep(30);
            GUITHREADINFO info{};
            info.cbSize = sizeof(info);
            if (finished.load() || !GetGUIThreadInfo(sourceThread, &info) || !info.hwndCapture)
                continue;
            if (GetWindowThreadProcessId(info.hwndCapture, nullptr) != sourceThread)
                continue;
            POINT client = center;
            if (ScreenToClient(info.hwndCapture, &client))
                PostMessageW(info.hwndCapture, WM_LBUTTONUP, 0, MAKELPARAM(client.x, client.y));
        }
    });
    hr = DoDragDrop(data.Get(), source.Get(), DROPEFFECT_COPY, &effect);
    finished.store(true);
    wake.join();
    POINT now{};
    if (GetCursorPos(&now) && now.x == center.x && now.y == center.y)
        SetCursorPos(saved.x, saved.y);
    if (hr == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY)) return {};
    return L"選択した位置でファイルが受け付けられなかったか、操作が中断されました。";
}

// Explorer can activate the taskbar during a drag or just after OLE unwinds.
constexpr UINT_PTR focusTimer = 1;
HWND rejectedTarget = nullptr;
HWND taskbarPreview = nullptr;
ULONGLONG restoreUntil = 0;
DWORD inputAtRejection = 0;

bool isTaskbar(HWND window) {
    wchar_t cls[64]{};
    GetClassNameW(window, cls, 64);
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

void restoreSelectorFocus(HWND w) {
    LASTINPUTINFO input{sizeof(input), 0};
    HWND front = GetForegroundWindow();
    if (GetTickCount64() >= restoreUntil ||
        (GetLastInputInfo(&input) && input.dwTime != inputAtRejection) ||
        (front && front != w && front != rejectedTarget && front != taskbarPreview && !isTaskbar(front) &&
         GetAncestor(front, GA_ROOTOWNER) != rejectedTarget)) {
        KillTimer(w, focusTimer);
        return;
    }
    if (front != w) {
        SetForegroundWindow(w);
        if (front && GetForegroundWindow() == front) {
            DWORD other = GetWindowThreadProcessId(front, nullptr);
            DWORD self = GetCurrentThreadId();
            if (other && other != self && !IsHungAppWindow(front) && AttachThreadInput(self, other, TRUE)) {
                SetForegroundWindow(w);
                if (GetForegroundWindow() == w) SetFocus(w);
                AttachThreadInput(self, other, FALSE);
            }
        }
    }
    if (GetForegroundWindow() == w && GetFocus() != w) SetFocus(w);
}

LRESULT CALLBACK procedure(HWND w, UINT msg, WPARAM key, LPARAM lp) {
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !dropping && !notice.empty()) {
        notice.clear();
        InvalidateRect(w, nullptr, FALSE);
    }
    switch (msg) {
    case WM_PAINT: paint(w); return 0;
    case WM_TIMER:
        if (key == focusTimer) restoreSelectorFocus(w);
        return 0;
    case WM_KILLFOCUS:
        enterPressedHere = false;
        return 0;
    case WM_CHAR:
        if (dropping || mode != Mode::window) return 0;
        if (key == 0x15) searchText.clear(); // Ctrl+U
        else if (key >= L' ' && key != 0x7f) searchText.push_back(static_cast<wchar_t>(key));
        else return 0;
        windowSearch.compile();
        refresh();
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (dropping) return 0;
        if (key == VK_BACK && mode == Mode::window) {
            if (searchText.empty()) return 0;
            wchar_t last = searchText.back();
            searchText.pop_back();
            if (IS_LOW_SURROGATE(last) && !searchText.empty() && IS_HIGH_SURROGATE(searchText.back()))
                searchText.pop_back();
            windowSearch.compile();
            refresh();
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        // Ignore repeats from an Enter press that began in the launching console.
        if (key == VK_RETURN && !(lp & (1L << 30))) enterPressedHere = true;
        if (key == VK_RETURN) return 0;
        if ((key == VK_ESCAPE || key == VK_TAB) && (lp & (1L << 30))) return 0;
        if (key == VK_ESCAPE) {
            if (mode == Mode::window) {
                if (searchText.empty()) DestroyWindow(w);
                else SendMessageW(w, WM_CHAR, 0x15, 0);
                return 0;
            }
            mode = mode == Mode::position && !parts.empty() ? Mode::part : Mode::window;
        } else if (key == VK_TAB && mode == Mode::window && !targets.empty()) {
            HWND root = targets[selected];
            // Reveal the target under the overlay while keeping keyboard focus.
            SetWindowPos(root, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
            SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            findParts(root);
            if (parts.empty()) {
                RECT r{};
                GetClientRect(root, &r);
                manual = midpoint(r);
                mode = Mode::position;
                notice = L"部品を検出できません。位置を指定してください。";
            } else mode = Mode::part;
        } else if (key == VK_TAB && mode == Mode::part) {
            HWND root = targets[selected];
            POINT pos{};
            if (!aim(root, pos)) pos = midpoint(clientOnScreen(root));
            ScreenToClient(root, &pos);
            manual = pos;
            mode = Mode::position;
        } else if (key >= VK_LEFT && key <= VK_DOWN) {
            int dx = (key == VK_RIGHT) - (key == VK_LEFT);
            int dy = (key == VK_DOWN) - (key == VK_UP);
            if (mode == Mode::window && dx) {
                refresh();
                if (!targets.empty())
                    selected = (selected + (dx > 0 ? 1 : targets.size()-1)) % targets.size();
            } else if (mode == Mode::part) {
                chooseDirection(targets[selected], dx, dy);
            } else if (mode == Mode::position) {
                HWND root = targets[selected];
                RECT r{};
                if (GetClientRect(root, &r) && !IsRectEmpty(&r)) {
                    // Use monitor DPI even if the receiver itself is DPI-unaware.
                    HMONITOR monitor = MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST);
                    UINT dpi = 96, dpiY = 96;
                    GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
                    int step = GetKeyState(VK_CONTROL) < 0 ? 64 : 16;
                    step = GetKeyState(VK_SHIFT) < 0 ? 1 : MulDiv(step, dpi, 96);
                    manual.x = std::clamp(manual.x + dx*step, 0L, r.right-1);
                    manual.y = std::clamp(manual.y + dy*step, 0L, r.bottom-1);
                }
            }
        }
        InvalidateRect(w, nullptr, FALSE);
        return 0;
    case WM_KEYUP:
        if (key == VK_RETURN && enterPressedHere) {
            enterPressedHere = false;
            if (dropping || targets.empty()) return 0;
            KillTimer(w, focusTimer);
            dropping = true;
            HWND target = targets[selected];
            POINT at{};
            bool valid = aim(target, at);
            // Zero alpha makes a layered window pass hit testing through while
            // keeping its keyboard focus. Hiding it hands activation to another app.
            std::wstring error;
            if (!valid) error = L"対象の位置が変わりました。選び直してください。";
            else if (!SetLayeredWindowAttributes(w, 0, 0, LWA_ALPHA))
                error = L"オーバーレイを透明にできませんでした。";
            else {
                // Taskbar hover can activate the app under an icon before OLE
                // reports rejection. Keep the foreground during this keyboard drag.
                bool locked = isTaskbar(target) && GetForegroundWindow() == w &&
                              LockSetForegroundWindow(LSFW_LOCK);
                error = deliver(target, at, mode == Mode::part ? parts[partIndex] : nullptr);
                if (locked) LockSetForegroundWindow(LSFW_UNLOCK);
            }
            if (error.empty()) DestroyWindow(w);
            else {
                notice = error;
                if (!candidate(target)) { mode = Mode::window; parts.clear(); refresh(); }
                SetLayeredWindowAttributes(w, transparentColor, 220, LWA_COLORKEY | LWA_ALPHA);
                rejectedTarget = target;
                taskbarPreview = isTaskbar(target) ? GetForegroundWindow() : nullptr;
                LASTINPUTINFO input{sizeof(input), 0};
                GetLastInputInfo(&input);
                inputAtRejection = input.dwTime;
                restoreUntil = GetTickCount64() + 600;
                SetTimer(w, focusTimer, 50, nullptr);
                restoreSelectorFocus(w);
                InvalidateRect(w, nullptr, FALSE);
            }
            dropping = false;
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, msg, key, lp);
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) {
        return std::puts("kbddnd 0.3.3\nUsage: kbddnd.exe FILE [FILE ...]") == EOF ? 1 : 0;
    }
    FreeConsole();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::wstring error;
    for (int i = 1; i < argc; ++i) {
        std::error_code ec;
        auto path = std::filesystem::absolute(argv[i], ec);
        if (ec || !std::filesystem::exists(path, ec)) {
            error = std::wstring(L"ファイルを確認できません。\n") + argv[i];
            break;
        }
        paths.push_back(path.wstring());
    }
    if (!error.empty()) {
        MessageBoxW(nullptr, error.c_str(), title, MB_ICONINFORMATION);
        return 1;
    }
    if (FAILED(OleInitialize(nullptr))) return 1;
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = procedure;
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.lpszClassName = L"kbddndSelector";
    if (RegisterClassW(&cls))
        overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
            cls.lpszClassName, title, WS_POPUP, GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN), nullptr, nullptr, instance, nullptr);
    int result = 1;
    if (overlay && SetLayeredWindowAttributes(overlay, transparentColor, 220, LWA_COLORKEY | LWA_ALPHA)) {
        refresh();
        ShowWindow(overlay, SW_SHOW);
        SetForegroundWindow(overlay);
        windowSearch.initialize();
        MSG msg{};
        BOOL status;
        while ((status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        result = status < 0 ? 1 : static_cast<int>(msg.wParam);
    }
    OleUninitialize();
    return result;
}



