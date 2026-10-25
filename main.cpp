#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define WEBVIEW_IMPLEMENTATION
#include "webview.h"
#include "activity_store.h"
#include "notification_manager.h"

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <objbase.h>
#include <roapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define APP_NAME    L"WebView Browser"
#define APP_VERSION L"1.3.0"

#define IDM_ABOUT         1001
#define IDM_HISTORY       1002
#define IDM_DOWNLOADS     1003
#define ID_ADDRESS        2001
#define ID_GO             2002
#define ID_PLUS           2003
#define ID_CLOSE_CURRENT  2004
#define ID_NEXT_TAB       2005
#define ID_PREV_TAB       2006
#define ID_FOCUS_ADDRESS  2007
#define ID_OPEN_HISTORY   2008
#define ID_OPEN_DOWNLOADS 2009
#define ID_TAB_BASE       4000
#define ID_TAB_CLOSE_BASE 5000
#define IDA_HISTORY_TAB   6001
#define IDA_DOWNLOADS_TAB 6002
#define IDA_SEARCH        6003
#define IDA_FILTER        6004
#define IDA_LIST          6005
#define IDA_PRIMARY       6006
#define IDA_SECONDARY     6007
#define IDA_DELETE        6008
#define IDA_CLEAR         6009

#define WM_APP_NEWTAB (WM_APP + 1)
#define WM_APP_ACTIVITY_REFRESH (WM_APP + 2)
#define WM_APP_TOAST_ACTIVATED (WM_APP + 3)

static const COLORREF kStripBg    = RGB(0xF0, 0xF0, 0xF3);
static const COLORREF kActiveTab  = RGB(0xFF, 0xFF, 0xFF);
static const COLORREF kHoverTab   = RGB(0xE7, 0xE9, 0xED);
static const COLORREF kPressTab   = RGB(0xDC, 0xDE, 0xE3);
static const COLORREF kTextActive   = RGB(0x20, 0x21, 0x24);
static const COLORREF kTextInactive = RGB(0x5F, 0x63, 0x68);
static const COLORREF kCloseHover = RGB(0xD9, 0xDB, 0xDF);
static const COLORREF kToolbarBg  = RGB(0xFF, 0xFF, 0xFF);
static const COLORREF kBoxBg      = RGB(0xF1, 0xF2, 0xF6);
static const COLORREF kGoColor    = RGB(0x00, 0x78, 0xD4);
static const COLORREF kGoHover    = RGB(0x10, 0x6E, 0xBE);
static const COLORREF kGoPress    = RGB(0x00, 0x5A, 0x9E);
static constexpr std::wstring_view kNewTabTitle = L"New Tab";

struct Tab {
    webview_t w = nullptr;
    HWND widget = nullptr;
    std::wstring title = L"New Tab";
    std::wstring url;
    EventRegistrationToken nwToken{};
    EventRegistrationToken titleToken{};
    EventRegistrationToken sourceToken{};
    EventRegistrationToken akToken{};
    EventRegistrationToken navigationToken{};
    EventRegistrationToken downloadToken{};
    std::wstring lastHistoryId;
};

struct DownloadRuntime {
    std::wstring id;
    ICoreWebView2DownloadOperation *operation = nullptr;
    EventRegistrationToken bytesToken{};
    EventRegistrationToken stateToken{};
    bool paused = false;
    bool notified = false;
    std::int64_t lastPersistAt = 0;
};

struct TabGeom {
    RECT rc{};
    RECT closeRc{};
};

// Keeps the GDI backing store across paints. Mouse movement can invalidate the
// tab strip many times per second, so allocating a bitmap for every paint is
// unnecessarily expensive.
struct PaintBuffer {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previousBitmap = nullptr;
    int width = 0;
    int height = 0;

    HDC Acquire(HDC target, int requestedWidth, int requestedHeight) {
        if (requestedWidth <= 0 || requestedHeight <= 0) {
            return nullptr;
        }
        if (dc && width == requestedWidth && height == requestedHeight) {
            return dc;
        }
        Reset();
        dc = CreateCompatibleDC(target);
        bitmap = dc ? CreateCompatibleBitmap(target, requestedWidth,
                                             requestedHeight)
                    : nullptr;
        if (!bitmap) {
            Reset();
            return nullptr;
        }
        previousBitmap = SelectObject(dc, bitmap);
        width = requestedWidth;
        height = requestedHeight;
        return dc;
    }

    void Reset() {
        if (dc && previousBitmap) {
            SelectObject(dc, previousBitmap);
        }
        if (bitmap) {
            DeleteObject(bitmap);
        }
        if (dc) {
            DeleteDC(dc);
        }
        dc = nullptr;
        bitmap = nullptr;
        previousBitmap = nullptr;
        width = 0;
        height = 0;
    }
};

enum HitKind {
    HIT_NONE,
    HIT_TAB,
    HIT_CLOSE,
    HIT_PLUS,
    HIT_GO,
    HIT_HISTORY,
    HIT_DOWNLOADS
};

static HINSTANCE g_hInstance;
static HWND g_mainWnd;
static HWND g_tabStrip;
static HWND g_toolbar;
static HWND g_container;
static HWND g_addressEdit;
static HWND g_activityWnd;
static HWND g_activityHistoryButton;
static HWND g_activityDownloadsButton;
static HWND g_activitySearch;
static HWND g_activityFilter;
static HWND g_activityList;
static HWND g_activityPrimary;
static HWND g_activitySecondary;
static HWND g_activityDelete;
static HWND g_activityClear;
static HACCEL g_hAccel = nullptr;
static HFONT g_font;
static HFONT g_boldFont;
static HBRUSH g_boxBrush;
static WNDPROC g_prevEditProc;
static PaintBuffer g_tabPaintBuffer;
static PaintBuffer g_toolbarPaintBuffer;

static std::vector<Tab> g_tabs;
static std::vector<TabGeom> g_tabGeom;
static RECT g_plusRc{};
static RECT g_boxRc{};
static RECT g_goRc{};
static RECT g_historyRc{};
static RECT g_downloadsRc{};
static int g_activeTab = -1;
static bool g_activityDownloadsMode = false;
static bool g_allowClose = false;

static ActivityStore g_activityStore(DefaultActivityStorePath());
static std::map<std::wstring, std::unique_ptr<DownloadRuntime>> g_downloadRuntime;
static std::map<std::wstring, std::vector<std::wstring>> g_pendingRetries;
static std::vector<std::wstring> g_activityVisibleIds;

static float g_scale = 1.0f;
static int g_stripH = 38;
static int g_toolbarH = 42;

static HitKind g_hoverKind = HIT_NONE;
static int g_hoverIdx = -1;
static HitKind g_pressKind = HIT_NONE;
static int g_pressIdx = -1;

static std::string WideToUtf8(const std::wstring &s) {
    if (s.empty()) {
        return "";
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0,
                                  nullptr, nullptr);
    if (len <= 0) {
        return "";
    }
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len,
                        nullptr, nullptr);
    return out;
}

static std::wstring NormalizeUrl(const std::wstring &input) {
    std::wstring s = input;
    size_t b = s.find_first_not_of(L" \t\r\n");
    size_t e = s.find_last_not_of(L" \t\r\n");
    if (b == std::wstring::npos) {
        return L"";
    }
    s = s.substr(b, e - b + 1);
    if (s.rfind(L"http://", 0) == 0 || s.rfind(L"https://", 0) == 0 ||
        s.rfind(L"about:", 0) == 0 || s.rfind(L"file://", 0) == 0) {
        return s;
    }
    if (s.find(L' ') == std::wstring::npos && s.find(L'.') != std::wstring::npos) {
        return L"https://" + s;
    }
    std::wstring q = s;
    for (auto &c : q) {
        if (c == L' ') {
            c = L'+';
        }
    }
    return L"https://cn.bing.com/search?q=" + q;
}

static int FindTabIndex(webview_t w) {
    for (size_t i = 0; i < g_tabs.size(); i++) {
        if (g_tabs[i].w == w) {
            return (int)i;
        }
    }
    return -1;
}

static void RefreshTabStrip(bool recomputeLayout = false);
static void UpdateWindowTitle();
static void CreateTab(const std::wstring &url);
static void OpenActivityWindow(bool downloads, const std::wstring &highlightId = L"");
static void RefreshActivityWindow();
static void UpdateDownloadOperation(const std::wstring &id,
                                    ICoreWebView2DownloadOperation *operation);

static DownloadRuntime *FindDownloadRuntime(const std::wstring &id) {
    auto item = g_downloadRuntime.find(id);
    return item == g_downloadRuntime.end() ? nullptr : item->second.get();
}

static std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

static std::wstring FileNameFromPath(const std::wstring &path) {
    if (path.empty()) return L"download";
    std::filesystem::path value(path);
    return value.filename().wstring().empty() ? L"download" : value.filename().wstring();
}

static std::wstring_view TabLabel(const Tab &tab) {
    return tab.title.empty() ? kNewTabTitle : std::wstring_view(tab.title);
}

template <class T>
class ComHandlerBase : public T {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(T)) {
            *ppv = static_cast<T *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG c = --m_ref;
        if (c == 0) {
            delete this;
        }
        return c;
    }

protected:
    virtual ~ComHandlerBase() = default;
    ULONG m_ref = 1;
};

class NewWindowRequestedHandler
    : public ComHandlerBase<ICoreWebView2NewWindowRequestedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2 *,
           ICoreWebView2NewWindowRequestedEventArgs *args) override {
        std::wstring *target = nullptr;
        LPWSTR uri = nullptr;
        if (SUCCEEDED(args->get_Uri(&uri)) && uri && uri[0] != L'\0') {
            target = new std::wstring(uri);
            CoTaskMemFree(uri);
        }
        PostMessageW(g_mainWnd, WM_APP_NEWTAB, 0, (LPARAM)target);
        args->put_Handled(TRUE);
        return S_OK;
    }
};

class DocumentTitleChangedHandler
    : public ComHandlerBase<ICoreWebView2DocumentTitleChangedEventHandler> {
public:
    explicit DocumentTitleChangedHandler(webview_t w, ICoreWebView2 *core)
        : m_w(w), m_core(core) {
        m_core->AddRef();
    }
    ~DocumentTitleChangedHandler() override { m_core->Release(); }

    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *, IUnknown *) override {
        int idx = FindTabIndex(m_w);
        if (idx >= 0) {
            LPWSTR title = nullptr;
            if (SUCCEEDED(m_core->get_DocumentTitle(&title)) && title) {
                g_tabs[idx].title = title;
                if (!g_tabs[idx].lastHistoryId.empty()) {
                    g_activityStore.UpdateHistoryTitle(g_tabs[idx].lastHistoryId,
                                                       g_tabs[idx].title);
                    if (g_activityWnd) {
                        PostMessageW(g_activityWnd, WM_APP_ACTIVITY_REFRESH, 0, 0);
                    }
                }
                CoTaskMemFree(title);
                RefreshTabStrip(true);
                UpdateWindowTitle();
            }
        }
        return S_OK;
    }

private:
    webview_t m_w;
    ICoreWebView2 *m_core;
};

class SourceChangedHandler
    : public ComHandlerBase<ICoreWebView2SourceChangedEventHandler> {
public:
    explicit SourceChangedHandler(webview_t w, ICoreWebView2 *core)
        : m_w(w), m_core(core) {
        m_core->AddRef();
    }
    ~SourceChangedHandler() override { m_core->Release(); }

    HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2 *, ICoreWebView2SourceChangedEventArgs *args) override {
        int idx = FindTabIndex(m_w);
        if (idx >= 0) {
            LPWSTR src = nullptr;
            if (SUCCEEDED(m_core->get_Source(&src)) && src) {
                g_tabs[idx].url = src;
                CoTaskMemFree(src);
                if (idx == g_activeTab) {
                    SetWindowTextW(g_addressEdit, g_tabs[idx].url.c_str());
                }
                BOOL isNewDocument = TRUE;
                if (SUCCEEDED(args->get_IsNewDocument(&isNewDocument)) && !isNewDocument) {
                    LPWSTR title = nullptr;
                    m_core->get_DocumentTitle(&title);
                    g_tabs[idx].lastHistoryId = g_activityStore.RecordHistory(
                        reinterpret_cast<std::uintptr_t>(m_w), g_tabs[idx].url,
                        title ? title : g_tabs[idx].url,
                        ActivityStore::NowMilliseconds());
                    if (title) CoTaskMemFree(title);
                    if (g_activityWnd) {
                        PostMessageW(g_activityWnd, WM_APP_ACTIVITY_REFRESH, 0, 0);
                    }
                }
            }
        }
        return S_OK;
    }

private:
    webview_t m_w;
    ICoreWebView2 *m_core;
};

class NavigationCompletedHandler
    : public ComHandlerBase<ICoreWebView2NavigationCompletedEventHandler> {
public:
    NavigationCompletedHandler(webview_t w, ICoreWebView2 *core)
        : m_w(w), m_core(core) {
        m_core->AddRef();
    }
    ~NavigationCompletedHandler() override { m_core->Release(); }

    HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *args) override {
        BOOL success = FALSE;
        if (FAILED(args->get_IsSuccess(&success)) || !success) return S_OK;
        const int idx = FindTabIndex(m_w);
        if (idx < 0) return S_OK;
        LPWSTR source = nullptr;
        LPWSTR title = nullptr;
        if (SUCCEEDED(m_core->get_Source(&source)) && source) {
            m_core->get_DocumentTitle(&title);
            g_tabs[idx].lastHistoryId = g_activityStore.RecordHistory(
                reinterpret_cast<std::uintptr_t>(m_w), source,
                title ? title : source, ActivityStore::NowMilliseconds());
            CoTaskMemFree(source);
            if (title) CoTaskMemFree(title);
            if (g_activityWnd) {
                PostMessageW(g_activityWnd, WM_APP_ACTIVITY_REFRESH, 0, 0);
            }
        }
        return S_OK;
    }

private:
    webview_t m_w;
    ICoreWebView2 *m_core;
};

class DownloadBytesChangedHandler
    : public ComHandlerBase<ICoreWebView2BytesReceivedChangedEventHandler> {
public:
    explicit DownloadBytesChangedHandler(std::wstring id) : id_(std::move(id)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2DownloadOperation *sender,
                                     IUnknown *) override {
        UpdateDownloadOperation(id_, sender);
        return S_OK;
    }
private:
    std::wstring id_;
};

class DownloadStateChangedHandler
    : public ComHandlerBase<ICoreWebView2StateChangedEventHandler> {
public:
    explicit DownloadStateChangedHandler(std::wstring id) : id_(std::move(id)) {}
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2DownloadOperation *sender,
                                     IUnknown *) override {
        UpdateDownloadOperation(id_, sender);
        return S_OK;
    }
private:
    std::wstring id_;
};

class DownloadStartingHandler
    : public ComHandlerBase<ICoreWebView2DownloadStartingEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE
    Invoke(ICoreWebView2 *, ICoreWebView2DownloadStartingEventArgs *args) override {
        ICoreWebView2DownloadOperation *operation = nullptr;
        if (FAILED(args->get_DownloadOperation(&operation)) || !operation) return S_OK;

        LPWSTR uri = nullptr;
        LPWSTR resultPath = nullptr;
        operation->get_Uri(&uri);
        args->get_ResultFilePath(&resultPath);
        INT64 total = 0;
        operation->get_TotalBytesToReceive(&total);

        DownloadEntry entry;
        entry.url = uri ? uri : L"";
        entry.savePath = resultPath ? resultPath : L"";
        entry.filename = FileNameFromPath(entry.savePath);
        entry.state = L"progressing";
        entry.totalBytes = total;
        entry.startedAt = ActivityStore::NowMilliseconds();
        auto pending = g_pendingRetries.find(entry.url);
        if (pending != g_pendingRetries.end() && !pending->second.empty()) {
            entry.retryOf = pending->second.front();
            pending->second.erase(pending->second.begin());
            if (pending->second.empty()) g_pendingRetries.erase(pending);
        }
        entry.id = ActivityStore::NewId();
        const std::wstring id = g_activityStore.AddDownload(entry);

        auto runtime = std::make_unique<DownloadRuntime>();
        runtime->id = id;
        runtime->operation = operation;
        auto *bytes = new DownloadBytesChangedHandler(id);
        operation->add_BytesReceivedChanged(bytes, &runtime->bytesToken);
        bytes->Release();
        auto *state = new DownloadStateChangedHandler(id);
        operation->add_StateChanged(state, &runtime->stateToken);
        state->Release();
        g_downloadRuntime[id] = std::move(runtime);

        if (uri) CoTaskMemFree(uri);
        if (resultPath) CoTaskMemFree(resultPath);
        if (g_activityWnd) PostMessageW(g_activityWnd, WM_APP_ACTIVITY_REFRESH, 0, 0);
        InvalidateRect(g_toolbar, nullptr, FALSE);
        return S_OK;
    }
};

class AcceleratorKeyPressedHandler
    : public ComHandlerBase<ICoreWebView2AcceleratorKeyPressedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE
        Invoke(ICoreWebView2Controller*,
            ICoreWebView2AcceleratorKeyPressedEventArgs* args) override {
        COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
        if (FAILED(args->get_KeyEventKind(&kind))) {
            return S_OK;
        }
        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
            kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) {
            return S_OK;
        }

        UINT vk = 0;
        if (FAILED(args->get_VirtualKey(&vk))) {
            return S_OK;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            return S_OK;
        }
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        UINT cmd = 0;
        if (vk == 'T') {
            cmd = ID_PLUS;
        }
        else if (vk == 'W') {
            cmd = ID_CLOSE_CURRENT;
        }
        else if (vk == 'L') {
            cmd = ID_FOCUS_ADDRESS;
        }
        else if (vk == 'H') {
            cmd = ID_OPEN_HISTORY;
        }
        else if (vk == 'J') {
            cmd = ID_OPEN_DOWNLOADS;
        }
        else if (vk == VK_TAB) {
            cmd = shift ? ID_PREV_TAB : ID_NEXT_TAB;
        }
        if (cmd != 0) {
            PostMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
            args->put_Handled(TRUE);
        }
        return S_OK;
    }
};

static std::wstring FormatBytes(std::int64_t value) {
    if (value <= 0) return L"Unknown";
    const wchar_t *units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double amount = static_cast<double>(value);
    int unit = 0;
    while (amount >= 1024.0 && unit < 4) {
        amount /= 1024.0;
        ++unit;
    }
    wchar_t buffer[64];
    swprintf_s(buffer, unit == 0 || amount >= 10.0 ? L"%.0f %s" : L"%.1f %s",
               amount, units[unit]);
    return buffer;
}

static SYSTEMTIME LocalSystemTimeFromUnixMilliseconds(std::int64_t value) {
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<ULONGLONG>(value + 11644473600000LL) * 10000ULL;
    FILETIME utc{ticks.LowPart, ticks.HighPart};
    FILETIME local{};
    SYSTEMTIME result{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &result);
    return result;
}

static std::wstring FormatTimestamp(std::int64_t value) {
    if (value <= 0) return L"";
    SYSTEMTIME time = LocalSystemTimeFromUnixMilliseconds(value);
    wchar_t date[64]{};
    wchar_t clock[64]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &time, nullptr,
                    date, ARRAYSIZE(date), nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &time, nullptr,
                    clock, ARRAYSIZE(clock));
    return std::wstring(date) + L" " + clock;
}

static std::wstring DayGroupLabel(std::int64_t value) {
    SYSTEMTIME item = LocalSystemTimeFromUnixMilliseconds(value);
    SYSTEMTIME today{};
    GetLocalTime(&today);
    if (item.wYear == today.wYear && item.wMonth == today.wMonth && item.wDay == today.wDay) {
        return L"Today";
    }
    FILETIME todayFt{};
    SystemTimeToFileTime(&today, &todayFt);
    ULARGE_INTEGER raw{todayFt.dwLowDateTime, todayFt.dwHighDateTime};
    raw.QuadPart -= 24ULL * 60 * 60 * 10000000ULL;
    FILETIME yesterdayFt{raw.LowPart, raw.HighPart};
    SYSTEMTIME yesterday{};
    FileTimeToSystemTime(&yesterdayFt, &yesterday);
    if (item.wYear == yesterday.wYear && item.wMonth == yesterday.wMonth && item.wDay == yesterday.wDay) {
        return L"Yesterday";
    }
    wchar_t date[96]{};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &item, nullptr,
                    date, ARRAYSIZE(date), nullptr);
    return date;
}

static bool FileExists(const std::wstring &path) {
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void ShowDownloadNotification(const DownloadEntry &entry) {
    const bool completed = entry.state == L"completed";
    if (ShowNativeDownloadNotification(entry.id, entry.filename, entry.totalBytes,
                                       completed, entry.interruptReason)) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_mainWnd;
    data.uID = 1;
    data.uFlags = NIF_ICON | NIF_TIP | NIF_INFO;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, APP_NAME);
    wcscpy_s(data.szInfoTitle, completed ? L"Download complete" : L"Download failed");
    std::wstring message = entry.filename;
    if (completed && entry.totalBytes > 0) message += L" (" + FormatBytes(entry.totalBytes) + L")";
    if (!completed && !entry.interruptReason.empty()) message += L": " + entry.interruptReason;
    wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
    data.dwInfoFlags = completed ? NIIF_INFO : NIIF_ERROR;
    Shell_NotifyIconW(NIM_ADD, &data);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

static void UpdateDownloadOperation(const std::wstring &id,
                                    ICoreWebView2DownloadOperation *operation) {
    DownloadEntry *entry = g_activityStore.FindDownload(id);
    DownloadRuntime *runtime = FindDownloadRuntime(id);
    if (!entry || !operation || !runtime) return;
    operation->get_BytesReceived(&entry->receivedBytes);
    operation->get_TotalBytesToReceive(&entry->totalBytes);
    LPWSTR resultPath = nullptr;
    if (SUCCEEDED(operation->get_ResultFilePath(&resultPath)) && resultPath) {
        entry->savePath = resultPath;
        entry->filename = FileNameFromPath(entry->savePath);
        CoTaskMemFree(resultPath);
    }
    COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    if (SUCCEEDED(operation->get_State(&state))) {
        if (runtime->paused) {
            entry->state = L"paused";
        } else if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
            entry->state = L"completed";
        } else if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
            COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason = COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
            operation->get_InterruptReason(&reason);
            entry->state = reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED
                               ? L"cancelled"
                               : L"failed";
            entry->interruptReason = L"WebView2 interruption code " + std::to_wstring(static_cast<int>(reason));
        } else {
            entry->state = L"progressing";
        }
    }
    const bool isActive = ActivityStore::IsActiveDownloadState(entry->state);
    if (!isActive && !entry->finishedAt) {
        entry->finishedAt = ActivityStore::NowMilliseconds();
    }
    const std::int64_t now = ActivityStore::NowMilliseconds();
    if (isActive && runtime->lastPersistAt && now - runtime->lastPersistAt < 250) {
        return;
    }
    runtime->lastPersistAt = now;
    g_activityStore.Save();
    if (!isActive && !runtime->notified &&
        (entry->state == L"completed" || entry->state == L"failed")) {
        runtime->notified = true;
        ShowDownloadNotification(*entry);
    }
    if (g_activityWnd) PostMessageW(g_activityWnd, WM_APP_ACTIVITY_REFRESH, 0, 0);
    InvalidateRect(g_toolbar, nullptr, FALSE);
    if (!isActive) {
        operation->remove_BytesReceivedChanged(runtime->bytesToken);
        operation->remove_StateChanged(runtime->stateToken);
        operation->Release();
        runtime->operation = nullptr;
        g_downloadRuntime.erase(id);
    }
}

static void ConfigureActivityColumns(bool downloads) {
    while (ListView_DeleteColumn(g_activityList, 0)) {}
    struct Column { const wchar_t *title; int width; };
    const Column historyColumns[] = {{L"Title", 250}, {L"URL", 390}, {L"Visited", 150}};
    const Column downloadColumns[] = {{L"File", 210}, {L"Status", 90}, {L"Progress", 105},
                                      {L"Size", 90}, {L"Started", 140}, {L"Location", 280}};
    const Column *columns = downloads ? downloadColumns : historyColumns;
    const int count = downloads ? ARRAYSIZE(downloadColumns) : ARRAYSIZE(historyColumns);
    for (int index = 0; index < count; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<LPWSTR>(columns[index].title);
        column.cx = columns[index].width;
        column.iSubItem = index;
        ListView_InsertColumn(g_activityList, index, &column);
    }
}

static void SetListCell(int row, int column, const std::wstring &value) {
    ListView_SetItemText(g_activityList, row, column, const_cast<LPWSTR>(value.c_str()));
}

static std::wstring ActivitySearchText() {
    int length = GetWindowTextLengthW(g_activitySearch);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    if (length) GetWindowTextW(g_activitySearch, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return Lowercase(std::move(value));
}

static bool ContainsSearch(const std::wstring &haystack, const std::wstring &query) {
    return query.empty() || Lowercase(haystack).find(query) != std::wstring::npos;
}

static void RefreshActivityWindow() {
    if (!g_activityWnd || !g_activityList) return;
    ListView_DeleteAllItems(g_activityList);
    ListView_RemoveAllGroups(g_activityList);
    g_activityVisibleIds.clear();
    ConfigureActivityColumns(g_activityDownloadsMode);
    const std::wstring query = ActivitySearchText();

    ShowWindow(g_activityFilter, g_activityDownloadsMode ? SW_SHOW : SW_HIDE);
    SetWindowTextW(g_activityHistoryButton, g_activityDownloadsMode ? L"History" : L"History *");
    SetWindowTextW(g_activityDownloadsButton, g_activityDownloadsMode ? L"Downloads *" : L"Downloads");
    SetWindowTextW(g_activityDelete, g_activityDownloadsMode ? L"Delete record" : L"Delete selected");
    SetWindowTextW(g_activityClear, g_activityDownloadsMode ? L"Clear finished" : L"Clear history");

    if (!g_activityDownloadsMode) {
        SetWindowTextW(g_activityPrimary, L"Open");
        ShowWindow(g_activityPrimary, SW_SHOW);
        ShowWindow(g_activitySecondary, SW_HIDE);
        ListView_EnableGroupView(g_activityList, TRUE);
        std::map<std::wstring, int> groups;
        int nextGroup = 1;
        for (const auto &entry : g_activityStore.History()) {
            if (!ContainsSearch(entry.title + L" " + entry.url, query)) continue;
            const std::wstring label = DayGroupLabel(entry.visitedAt);
            int groupId = 0;
            auto found = groups.find(label);
            if (found == groups.end()) {
                groupId = nextGroup++;
                groups[label] = groupId;
                LVGROUP group{};
                group.cbSize = sizeof(group);
                group.mask = LVGF_HEADER | LVGF_GROUPID;
                group.pszHeader = const_cast<LPWSTR>(label.c_str());
                group.iGroupId = groupId;
                ListView_InsertGroup(g_activityList, -1, &group);
            } else {
                groupId = found->second;
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_GROUPID;
            item.iItem = ListView_GetItemCount(g_activityList);
            item.iGroupId = groupId;
            item.pszText = const_cast<LPWSTR>(entry.title.c_str());
            const int row = ListView_InsertItem(g_activityList, &item);
            SetListCell(row, 1, entry.url);
            SetListCell(row, 2, FormatTimestamp(entry.visitedAt));
            g_activityVisibleIds.push_back(entry.id);
        }
    } else {
        ListView_EnableGroupView(g_activityList, FALSE);
        const int filter = ComboBox_GetCurSel(g_activityFilter);
        for (const auto &entry : g_activityStore.Downloads()) {
            bool matches = filter <= 0 ||
                (filter == 1 && ActivityStore::IsActiveDownloadState(entry.state)) ||
                (filter == 2 && entry.state == L"completed") ||
                (filter == 3 && (entry.state == L"failed" || entry.state == L"interrupted" || entry.state == L"cancelled"));
            if (!matches || !ContainsSearch(entry.filename + L" " + entry.url, query)) continue;
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = ListView_GetItemCount(g_activityList);
            item.pszText = const_cast<LPWSTR>(entry.filename.c_str());
            const int row = ListView_InsertItem(g_activityList, &item);
            SetListCell(row, 1, entry.state);
            std::wstring progress = entry.totalBytes > 0
                ? std::to_wstring((entry.receivedBytes * 100) / entry.totalBytes) + L"%"
                : FormatBytes(entry.receivedBytes);
            SetListCell(row, 2, progress);
            SetListCell(row, 3, FormatBytes(entry.totalBytes));
            SetListCell(row, 4, FormatTimestamp(entry.startedAt));
            SetListCell(row, 5, FileExists(entry.savePath) || ActivityStore::IsActiveDownloadState(entry.state)
                                     ? entry.savePath
                                     : L"File missing - " + entry.savePath);
            g_activityVisibleIds.push_back(entry.id);
        }
        ShowWindow(g_activityPrimary, SW_SHOW);
        ShowWindow(g_activitySecondary, SW_SHOW);
    }
}

static std::vector<std::wstring> SelectedActivityIds() {
    std::vector<std::wstring> ids;
    int index = -1;
    while ((index = ListView_GetNextItem(g_activityList, index, LVNI_SELECTED)) != -1) {
        if (index >= 0 && index < static_cast<int>(g_activityVisibleIds.size())) {
            ids.push_back(g_activityVisibleIds[static_cast<std::size_t>(index)]);
        }
    }
    return ids;
}

enum class DownloadAction { Open, Folder, PauseResume, Cancel, Retry, DeleteRecord, DeleteFile };

static void PerformDownloadAction(const std::wstring &id, DownloadAction action) {
    DownloadEntry *entry = g_activityStore.FindDownload(id);
    if (!entry) return;
    DownloadRuntime *runtime = FindDownloadRuntime(id);
    if (action == DownloadAction::Open) {
        if (FileExists(entry->savePath)) ShellExecuteW(g_activityWnd, L"open", entry->savePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else MessageBoxW(g_activityWnd, L"The downloaded file no longer exists.", L"Downloads", MB_OK | MB_ICONINFORMATION);
    } else if (action == DownloadAction::Folder) {
        if (entry->savePath.empty()) return;
        if (FileExists(entry->savePath)) {
            std::wstring argument = L"/select,\"" + entry->savePath + L"\"";
            ShellExecuteW(g_activityWnd, L"open", L"explorer.exe", argument.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(g_activityWnd, L"open", std::filesystem::path(entry->savePath).parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (action == DownloadAction::PauseResume && runtime && runtime->operation) {
        if (runtime->paused) {
            if (SUCCEEDED(runtime->operation->Resume())) runtime->paused = false;
        } else {
            BOOL canResume = FALSE;
            runtime->operation->get_CanResume(&canResume);
            if (canResume && SUCCEEDED(runtime->operation->Pause())) runtime->paused = true;
        }
        UpdateDownloadOperation(id, runtime->operation);
    } else if (action == DownloadAction::Cancel && runtime && runtime->operation) {
        runtime->operation->Cancel();
    } else if (action == DownloadAction::Retry) {
        if (!ActivityStore::IsWebUrl(entry->url)) {
            MessageBoxW(g_activityWnd, L"This download source is not a valid web address.",
                        L"Downloads", MB_OK | MB_ICONWARNING);
            return;
        }
        g_pendingRetries[entry->url].push_back(entry->id);
        CreateTab(entry->url);
    } else if (action == DownloadAction::DeleteRecord) {
        g_activityStore.RemoveDownloads({id}, true);
    } else if (action == DownloadAction::DeleteFile) {
        if (!FileExists(entry->savePath)) {
            MessageBoxW(g_activityWnd, L"The downloaded file no longer exists.", L"Downloads", MB_OK | MB_ICONINFORMATION);
        } else if (MessageBoxW(g_activityWnd, L"Move this file to the Recycle Bin and remove its download record?",
                               L"Delete downloaded file", MB_YESNO | MB_ICONWARNING) == IDYES) {
            std::wstring from = entry->savePath;
            from.push_back(L'\0');
            SHFILEOPSTRUCTW operation{};
            operation.hwnd = g_activityWnd;
            operation.wFunc = FO_DELETE;
            operation.pFrom = from.c_str();
            operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
            if (SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted) {
                g_activityStore.RemoveDownloads({id}, false);
            }
        }
    }
    RefreshActivityWindow();
}

static void UpdateDownloadButtons() {
    if (!g_activityDownloadsMode) return;
    auto ids = SelectedActivityIds();
    DownloadEntry *entry = ids.size() == 1 ? g_activityStore.FindDownload(ids[0]) : nullptr;
    EnableWindow(g_activityPrimary, entry != nullptr);
    EnableWindow(g_activitySecondary, entry != nullptr);
    EnableWindow(g_activityDelete, !ids.empty());
    if (!entry) {
        SetWindowTextW(g_activityPrimary, L"Open");
        SetWindowTextW(g_activitySecondary, L"More");
        return;
    }
    if (entry->state == L"progressing" || entry->state == L"waiting") {
        SetWindowTextW(g_activityPrimary, L"Pause");
        SetWindowTextW(g_activitySecondary, L"Cancel");
    } else if (entry->state == L"paused") {
        SetWindowTextW(g_activityPrimary, L"Resume");
        SetWindowTextW(g_activitySecondary, L"Cancel");
    } else if (entry->state == L"completed") {
        SetWindowTextW(g_activityPrimary, L"Open file");
        SetWindowTextW(g_activitySecondary, L"Open folder");
    } else {
        SetWindowTextW(g_activityPrimary, L"Retry");
        SetWindowTextW(g_activitySecondary, L"Open folder");
    }
}

static void LayoutActivityWindow(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = 14;
    const int top = 14;
    MoveWindow(g_activityHistoryButton, pad, top, 100, 32, TRUE);
    MoveWindow(g_activityDownloadsButton, pad + 108, top, 120, 32, TRUE);
    MoveWindow(g_activitySearch, pad + 240, top,
               static_cast<int>((std::max<LONG>)(180, rc.right - 718)), 32, TRUE);
    MoveWindow(g_activityFilter, rc.right - 456, top, 110, 220, TRUE);
    MoveWindow(g_activityPrimary, rc.right - 338, top, 100, 32, TRUE);
    MoveWindow(g_activitySecondary, rc.right - 230, top, 100, 32, TRUE);
    MoveWindow(g_activityDelete, rc.right - 122, top, 108, 32, TRUE);
    MoveWindow(g_activityClear, rc.right - 144, rc.bottom - 46, 130, 32, TRUE);
    MoveWindow(g_activityList, pad, 58, rc.right - pad * 2,
               static_cast<int>((std::max<LONG>)(80, rc.bottom - 112)), TRUE);
}

static LRESULT CALLBACK ActivityWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_activityHistoryButton = CreateWindowW(L"BUTTON", L"History *", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)IDA_HISTORY_TAB, g_hInstance, nullptr);
        g_activityDownloadsButton = CreateWindowW(L"BUTTON", L"Downloads", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)IDA_DOWNLOADS_TAB, g_hInstance, nullptr);
        g_activitySearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd,
            (HMENU)IDA_SEARCH, g_hInstance, nullptr);
        SendMessageW(g_activitySearch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search title, URL, or file");
        g_activityFilter = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | CBS_DROPDOWNLIST,
            0, 0, 0, 0, hwnd, (HMENU)IDA_FILTER, g_hInstance, nullptr);
        for (const wchar_t *label : {L"All", L"In progress", L"Completed", L"Failed"}) {
            ComboBox_AddString(g_activityFilter, label);
        }
        ComboBox_SetCurSel(g_activityFilter, 0);
        g_activityList = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT |
            LVS_SHOWSELALWAYS, 0, 0, 0, 0, hwnd, (HMENU)IDA_LIST, g_hInstance, nullptr);
        ListView_SetExtendedListViewStyle(g_activityList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        g_activityPrimary = CreateWindowW(L"BUTTON", L"Open", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)IDA_PRIMARY, g_hInstance, nullptr);
        g_activitySecondary = CreateWindowW(L"BUTTON", L"More", WS_CHILD,
            0, 0, 0, 0, hwnd, (HMENU)IDA_SECONDARY, g_hInstance, nullptr);
        g_activityDelete = CreateWindowW(L"BUTTON", L"Delete selected", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)IDA_DELETE, g_hInstance, nullptr);
        g_activityClear = CreateWindowW(L"BUTTON", L"Clear history", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)IDA_CLEAR, g_hInstance, nullptr);
        RefreshActivityWindow();
        return 0;
    case WM_SIZE:
        LayoutActivityWindow(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        auto *limits = reinterpret_cast<MINMAXINFO *>(lp);
        limits->ptMinTrackSize.x = 860;
        limits->ptMinTrackSize.y = 520;
        return 0;
    }
    case WM_APP_ACTIVITY_REFRESH:
        RefreshActivityWindow();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDA_HISTORY_TAB || id == IDA_DOWNLOADS_TAB) {
            g_activityDownloadsMode = id == IDA_DOWNLOADS_TAB;
            RefreshActivityWindow();
        } else if (id == IDA_SEARCH && HIWORD(wp) == EN_CHANGE) {
            RefreshActivityWindow();
        } else if (id == IDA_FILTER && HIWORD(wp) == CBN_SELCHANGE) {
            RefreshActivityWindow();
        } else if (id == IDA_PRIMARY) {
            auto ids = SelectedActivityIds();
            if (ids.size() == 1) {
                if (!g_activityDownloadsMode) {
                    auto found = std::find_if(g_activityStore.History().begin(), g_activityStore.History().end(),
                        [&](const auto &entry) { return entry.id == ids[0]; });
                    if (found != g_activityStore.History().end() && ActivityStore::IsWebUrl(found->url)) {
                        CreateTab(found->url);
                    }
                } else {
                    DownloadEntry *entry = g_activityStore.FindDownload(ids[0]);
                    if (entry && ActivityStore::IsActiveDownloadState(entry->state)) PerformDownloadAction(ids[0], DownloadAction::PauseResume);
                    else if (entry && entry->state == L"completed") PerformDownloadAction(ids[0], DownloadAction::Open);
                    else PerformDownloadAction(ids[0], DownloadAction::Retry);
                }
            }
        } else if (id == IDA_SECONDARY) {
            auto ids = SelectedActivityIds();
            if (ids.size() == 1) {
                DownloadEntry *entry = g_activityStore.FindDownload(ids[0]);
                if (entry && ActivityStore::IsActiveDownloadState(entry->state)) PerformDownloadAction(ids[0], DownloadAction::Cancel);
                else PerformDownloadAction(ids[0], DownloadAction::Folder);
            }
        } else if (id == IDA_DELETE) {
            auto ids = SelectedActivityIds();
            if (ids.empty()) return 0;
            if (g_activityDownloadsMode) g_activityStore.RemoveDownloads(ids, true);
            else if (MessageBoxW(hwnd, L"Delete the selected history records?", L"History", MB_YESNO | MB_ICONQUESTION) == IDYES)
                g_activityStore.RemoveHistory(ids);
            RefreshActivityWindow();
        } else if (id == IDA_CLEAR) {
            if (g_activityDownloadsMode) {
                if (MessageBoxW(hwnd, L"Clear all finished download records? Downloaded files will be kept.",
                                L"Downloads", MB_YESNO | MB_ICONQUESTION) == IDYES)
                    g_activityStore.ClearTerminalDownloads();
            } else if (MessageBoxW(hwnd, L"Clear all browsing history?", L"History",
                                   MB_YESNO | MB_ICONWARNING) == IDYES) {
                g_activityStore.ClearHistory();
            }
            RefreshActivityWindow();
        }
        return 0;
    }
    case WM_NOTIFY: {
        auto *header = reinterpret_cast<NMHDR *>(lp);
        if (header->idFrom == IDA_LIST && header->code == LVN_ITEMCHANGED) {
            UpdateDownloadButtons();
        } else if (header->idFrom == IDA_LIST && header->code == NM_DBLCLK) {
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDA_PRIMARY, BN_CLICKED), 0);
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        if ((HWND)wp == g_activityList && g_activityDownloadsMode) {
            auto ids = SelectedActivityIds();
            if (ids.size() == 1) {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Open file");
                AppendMenuW(menu, MF_STRING, 2, L"Open folder");
                AppendMenuW(menu, MF_STRING, 3, L"Pause / Resume");
                AppendMenuW(menu, MF_STRING, 4, L"Cancel");
                AppendMenuW(menu, MF_STRING, 5, L"Retry");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 6, L"Delete record");
                AppendMenuW(menu, MF_STRING, 7, L"Delete file and record...");
                POINT point{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                             point.x, point.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                const DownloadAction actions[] = {DownloadAction::Open, DownloadAction::Folder,
                    DownloadAction::PauseResume, DownloadAction::Cancel, DownloadAction::Retry,
                    DownloadAction::DeleteRecord, DownloadAction::DeleteFile};
                if (command >= 1 && command <= 7) PerformDownloadAction(ids[0], actions[command - 1]);
            }
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY: {
        g_activityWnd = nullptr;
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static void OpenActivityWindow(bool downloads, const std::wstring &highlightId) {
    g_activityDownloadsMode = downloads;
    if (!g_activityWnd) {
        g_activityWnd = CreateWindowExW(WS_EX_APPWINDOW, L"ActivityCenterWnd",
            L"Activity Center - WebView Browser", WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720, g_mainWnd, nullptr,
            g_hInstance, nullptr);
    }
    RefreshActivityWindow();
    ShowWindow(g_activityWnd, SW_SHOW);
    if (IsIconic(g_activityWnd)) ShowWindow(g_activityWnd, SW_RESTORE);
    SetForegroundWindow(g_activityWnd);
    if (!highlightId.empty()) {
        auto found = std::find(g_activityVisibleIds.begin(), g_activityVisibleIds.end(), highlightId);
        if (found != g_activityVisibleIds.end()) {
            int row = static_cast<int>(found - g_activityVisibleIds.begin());
            ListView_SetItemState(g_activityList, row, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_activityList, row, FALSE);
        }
    }
}

static int MeasureTabWidth(std::wstring_view text) {
    HDC dc = GetDC(g_mainWnd);
    HFONT oldFont = (HFONT)SelectObject(dc, g_boldFont);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.data(), (int)text.size(), &sz);
    SelectObject(dc, oldFont);
    ReleaseDC(g_mainWnd, dc);
    int w = sz.cx + (int)(24 * g_scale);
    if (w < (int)(84 * g_scale)) {
        w = (int)(84 * g_scale);
    }
    if (w > (int)(200 * g_scale)) {
        w = (int)(200 * g_scale);
    }
    return w;
}

static void ComputeTabLayout() {
    g_tabGeom.clear();
    if (!g_tabStrip) {
        return;
    }
    RECT rc{};
    GetClientRect(g_tabStrip, &rc);
    int pad = (int)(4 * g_scale);
    int plusW = (int)(32 * g_scale);
    int closeW = (int)(24 * g_scale);
    int y = (int)(4 * g_scale);
    int tabH = rc.bottom - y - (int)(4 * g_scale);
    int x = pad;

    g_tabGeom.reserve(g_tabs.size());

    for (size_t i = 0; i < g_tabs.size(); i++) {
        int tw = MeasureTabWidth(TabLabel(g_tabs[i]));
        if (x + tw + closeW + (int)(2 * g_scale) > rc.right - pad) {
            break;
        }
        TabGeom g;
        g.rc = {x, y, x + tw, y + tabH};
        g.closeRc = {x + tw, y, x + tw + closeW, y + tabH};
        g_tabGeom.push_back(g);
        x += tw + closeW + (int)(2 * g_scale);
    }
    g_plusRc = {x, y, x + plusW, y + tabH};
}

static void FillRoundRect(HDC dc, const RECT &rc, int r, COLORREF c) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, c);
    SetDCPenColor(dc, c);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

static void FillEllipse(HDC dc, int left, int top, int right, int bottom,
                        COLORREF color) {
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, color);
    SetDCPenColor(dc, color);
    Ellipse(dc, left, top, right, bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
}

static void DrawTextCentered(HDC dc, const RECT &rc, std::wstring_view text,
                             HFONT font, COLORREF color, UINT extraFlags = 0) {
    HFONT old = (HFONT)SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT trc = rc;
    DrawTextW(dc, text.data(), (int)text.size(), &trc,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | extraFlags);
    SelectObject(dc, old);
}

static void PaintTabStrip(HDC dc) {
    RECT rc{};
    GetClientRect(g_tabStrip, &rc);
    HDC mem = g_tabPaintBuffer.Acquire(dc, rc.right, rc.bottom);
    if (!mem) {
        return;
    }

    HBRUSH bg = CreateSolidBrush(kStripBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    int r = (int)(8 * g_scale);

    for (size_t i = 0; i < g_tabGeom.size() && i < g_tabs.size(); i++) {
        RECT trc = g_tabGeom[i].rc;
        bool active = (int)i == g_activeTab;
        bool hovered = g_hoverKind == HIT_TAB && g_hoverIdx == (int)i;
        bool pressed = g_pressKind == HIT_TAB && g_pressIdx == (int)i;

        if (active) {
            RECT arc = trc;
            arc.bottom += r;
            FillRoundRect(mem, arc, r * 2, kActiveTab);
        } else if (pressed) {
            FillRoundRect(mem, trc, r * 2, kPressTab);
        } else if (hovered) {
            FillRoundRect(mem, trc, r * 2, kHoverTab);
        }

        RECT textRc = trc;
        textRc.left += (int)(12 * g_scale);
        textRc.right = g_tabGeom[i].closeRc.left;
        std::wstring_view label = TabLabel(g_tabs[i]);
        label = label.substr(0, (std::min)(label.size(), size_t{30}));
        DrawTextCentered(mem, textRc, label,
                         active ? g_boldFont : g_font,
                         active ? kTextActive : kTextInactive,
                         DT_LEFT);

        bool closeHover = g_hoverKind == HIT_CLOSE && g_hoverIdx == (int)i;
        bool closePress = g_pressKind == HIT_CLOSE && g_pressIdx == (int)i;
        RECT crc = g_tabGeom[i].closeRc;
        int cw = crc.right - crc.left;
        int ch = crc.bottom - crc.top;
        int d = (std::min)(cw, ch) - (int)(6 * g_scale);
        if (d < 8) {
            d = 8;
        }
        int cx = (crc.left + crc.right - d) / 2;
        int cy = (crc.top + crc.bottom - d) / 2;
        if (closeHover || closePress) {
            FillEllipse(mem, cx, cy, cx + d, cy + d,
                        closePress ? kHoverTab : kCloseHover);
        }
        RECT xRc = {cx, cy, cx + d, cy + d};
        DrawTextCentered(mem, xRc, L"\u00D7", g_font,
                         active ? kTextActive : kTextInactive);
    }

    bool plusHover = g_hoverKind == HIT_PLUS;
    bool plusPress = g_pressKind == HIT_PLUS;
    int pw = g_plusRc.right - g_plusRc.left;
    int ph = g_plusRc.bottom - g_plusRc.top;
    int pd = (std::min)(pw, ph) - (int)(6 * g_scale);
    if (pd < 8) {
        pd = 8;
    }
    int px = (g_plusRc.left + g_plusRc.right - pd) / 2;
    int py = (g_plusRc.top + g_plusRc.bottom - pd) / 2;
    if (plusHover || plusPress) {
        FillEllipse(mem, px, py, px + pd, py + pd,
                    plusPress ? kPressTab : kHoverTab);
    }
    RECT pRc = {px, py, px + pd, py + pd};
    DrawTextCentered(mem, pRc, L"+", g_font, kTextInactive);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
}

static void PaintToolbar(HDC dc) {
    RECT rc{};
    GetClientRect(g_toolbar, &rc);
    HDC mem = g_toolbarPaintBuffer.Acquire(dc, rc.right, rc.bottom);
    if (!mem) {
        return;
    }

    HBRUSH bg = CreateSolidBrush(kToolbarBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    int r = (int)(9 * g_scale);
    FillRoundRect(mem, g_boxRc, r * 2, kBoxBg);

    bool hover = g_hoverKind == HIT_GO;
    bool press = g_pressKind == HIT_GO;
    FillRoundRect(mem, g_goRc, r * 2,
                  press ? kGoPress : (hover ? kGoHover : kGoColor));
    DrawTextCentered(mem, g_goRc, L"Go", g_boldFont, RGB(255, 255, 255));

    auto drawActivityButton = [&](const RECT &button, HitKind kind,
                                  const std::wstring &label) {
        const bool buttonHover = g_hoverKind == kind;
        const bool buttonPress = g_pressKind == kind;
        FillRoundRect(mem, button, r * 2,
                      buttonPress ? kPressTab : (buttonHover ? kHoverTab : kBoxBg));
        DrawTextCentered(mem, button, label, g_font, kTextActive);
    };
    drawActivityButton(g_historyRc, HIT_HISTORY, L"History");
    std::wstring downloadsLabel = L"Downloads";
    const auto activeDownloads = g_activityStore.ActiveDownloadCount();
    if (activeDownloads) downloadsLabel += L" (" + std::to_wstring(activeDownloads) + L")";
    drawActivityButton(g_downloadsRc, HIT_DOWNLOADS, downloadsLabel);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
}

static HitKind HitTestStrip(POINT pt, int &idx) {
    idx = -1;
    for (size_t i = 0; i < g_tabGeom.size(); i++) {
        if (PtInRect(&g_tabGeom[i].rc, pt)) {
            idx = (int)i;
            return HIT_TAB;
        }
        if (PtInRect(&g_tabGeom[i].closeRc, pt)) {
            idx = (int)i;
            return HIT_CLOSE;
        }
    }
    if (PtInRect(&g_plusRc, pt)) {
        return HIT_PLUS;
    }
    return HIT_NONE;
}

static void StripDoAction(HitKind kind, int idx) {
    int cmd = 0;
    switch (kind) {
    case HIT_TAB:
        cmd = ID_TAB_BASE + idx;
        break;
    case HIT_CLOSE:
        cmd = ID_TAB_CLOSE_BASE + idx;
        break;
    case HIT_PLUS:
        cmd = ID_PLUS;
        break;
    default:
        return;
    }
    PostMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
}

static void TrackMouseLeave(HWND hwnd) {
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd;
    TrackMouseEvent(&tme);
}

static LRESULT CALLBACK TabStripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        PaintTabStrip(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = -1;
        HitKind kind = HitTestStrip(pt, idx);
        if (kind != g_hoverKind || idx != g_hoverIdx) {
            g_hoverKind = kind;
            g_hoverIdx = idx;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TrackMouseLeave(hwnd);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hoverKind != HIT_NONE) {
            g_hoverKind = HIT_NONE;
            g_hoverIdx = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = -1;
        g_pressKind = HitTestStrip(pt, idx);
        g_pressIdx = idx;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int idx = -1;
        HitKind kind = HitTestStrip(pt, idx);
        if (g_pressKind == kind && kind != HIT_NONE && g_pressIdx == idx) {
            StripDoAction(kind, idx);
        }
        g_pressKind = HIT_NONE;
        g_pressIdx = -1;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK ToolbarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        PaintToolbar(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, kBoxBg);
        return (LRESULT)g_boxBrush;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        HitKind over = PtInRect(&g_goRc, pt) ? HIT_GO
            : PtInRect(&g_historyRc, pt) ? HIT_HISTORY
            : PtInRect(&g_downloadsRc, pt) ? HIT_DOWNLOADS : HIT_NONE;
        if (g_hoverKind != over) {
            g_hoverKind = over;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TrackMouseLeave(hwnd);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hoverKind == HIT_GO || g_hoverKind == HIT_HISTORY ||
            g_hoverKind == HIT_DOWNLOADS) {
            g_hoverKind = HIT_NONE;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_pressKind = PtInRect(&g_goRc, pt) ? HIT_GO
            : PtInRect(&g_historyRc, pt) ? HIT_HISTORY
            : PtInRect(&g_downloadsRc, pt) ? HIT_DOWNLOADS : HIT_NONE;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g_pressKind == HIT_GO && PtInRect(&g_goRc, pt)) {
            PostMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(ID_GO, 0), 0);
        } else if (g_pressKind == HIT_HISTORY && PtInRect(&g_historyRc, pt)) {
            PostMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(ID_OPEN_HISTORY, 0), 0);
        } else if (g_pressKind == HIT_DOWNLOADS && PtInRect(&g_downloadsRc, pt)) {
            PostMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(ID_OPEN_DOWNLOADS, 0), 0);
        }
        g_pressKind = HIT_NONE;
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK AddressEditProc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        SendMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(ID_GO, 0), 0);
        return 0;
    }
    return CallWindowProcW(g_prevEditProc, hwnd, msg, wp, lp);
}

static void LayoutControls() {
    RECT rc{};
    GetClientRect(g_mainWnd, &rc);
    int w = rc.right;
    int h = rc.bottom;

    MoveWindow(g_tabStrip, 0, 0, w, g_stripH, TRUE);
    MoveWindow(g_toolbar, 0, g_stripH, w, g_toolbarH, TRUE);
    int cy = g_stripH + g_toolbarH;
    int ch = h - cy;
    if (ch < 0) {
        ch = 0;
    }
    MoveWindow(g_container, 0, cy, w, ch, TRUE);

    RECT trc{};
    GetClientRect(g_toolbar, &trc);
    int padX = (int)(10 * g_scale);
    int padY = (int)(7 * g_scale);
    int goW = (int)(58 * g_scale);
    int historyW = (int)(76 * g_scale);
    int downloadsW = (int)(112 * g_scale);
    int gap = (int)(8 * g_scale);
    int right = trc.right - padX;
    g_downloadsRc = {right - downloadsW, padY, right, trc.bottom - padY};
    right = g_downloadsRc.left - gap;
    g_historyRc = {right - historyW, padY, right, trc.bottom - padY};
    right = g_historyRc.left - gap;
    g_goRc = {right - goW, padY, right, trc.bottom - padY};
    g_boxRc = {padX, padY, g_goRc.left - gap,
               trc.bottom - padY};

    int ePad = (int)(12 * g_scale);
    MoveWindow(g_addressEdit, g_boxRc.left + ePad,
               g_boxRc.top + (int)(2 * g_scale),
               g_boxRc.right - g_boxRc.left - ePad - (int)(4 * g_scale),
               g_boxRc.bottom - g_boxRc.top - (int)(4 * g_scale), TRUE);

    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size() &&
        g_tabs[g_activeTab].widget) {
        RECT crc{};
        GetClientRect(g_container, &crc);
        MoveWindow(g_tabs[g_activeTab].widget, 0, 0, crc.right, crc.bottom, TRUE);
    }

    ComputeTabLayout();
    InvalidateRect(g_tabStrip, nullptr, TRUE);
    InvalidateRect(g_toolbar, nullptr, TRUE);
}

static void RefreshTabStrip(bool recomputeLayout) {
    if (recomputeLayout) {
        ComputeTabLayout();
    }
    if (g_tabStrip) {
        InvalidateRect(g_tabStrip, nullptr, TRUE);
    }
}

static void UpdateWindowTitle() {
    std::wstring t = APP_NAME;
    if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size() &&
        !g_tabs[g_activeTab].title.empty()) {
        t += L" - " + g_tabs[g_activeTab].title;
    }
    SetWindowTextW(g_mainWnd, t.c_str());
}

static void ActivateTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) {
        return;
    }
    g_activeTab = idx;
    for (size_t i = 0; i < g_tabs.size(); i++) {
        if (g_tabs[i].widget) {
            ShowWindow(g_tabs[i].widget, (int)i == idx ? SW_SHOW : SW_HIDE);
        }
    }
    SetWindowTextW(g_addressEdit, g_tabs[idx].url.c_str());
    LayoutControls();
    UpdateWindowTitle();
    if (g_tabs[idx].widget) {
        SetFocus(g_tabs[idx].widget);
        auto *controller = (ICoreWebView2Controller *)webview_get_native_handle(
            g_tabs[idx].w, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER);
        if (controller) {
            controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
    }
}

static void CreateTab(const std::wstring &url) {
    Tab tab;
    tab.url = url;
    tab.w = webview_create(0, (void *)g_container);
    if (!tab.w) {
        MessageBoxW(g_mainWnd,
                    L"Failed to create WebView. Please install the WebView2 runtime.",
                    APP_NAME, MB_OK | MB_ICONERROR);
        return;
    }
    tab.widget = (HWND)webview_get_native_handle(
        tab.w, WEBVIEW_NATIVE_HANDLE_KIND_UI_WIDGET);
    g_tabs.push_back(tab);
    int idx = (int)g_tabs.size() - 1;

    auto *controller = (ICoreWebView2Controller *)webview_get_native_handle(
        tab.w, WEBVIEW_NATIVE_HANDLE_KIND_BROWSER_CONTROLLER);
    if (controller) {
        auto* ak = new AcceleratorKeyPressedHandler();
        controller->add_AcceleratorKeyPressed(ak, &g_tabs[idx].akToken);
        ak->Release();

        ICoreWebView2* core = nullptr;
        if (SUCCEEDED(controller->get_CoreWebView2(&core))) {
            auto *nw = new NewWindowRequestedHandler();
            core->add_NewWindowRequested(nw, &g_tabs[idx].nwToken);
            nw->Release();

            auto *tt = new DocumentTitleChangedHandler(tab.w, core);
            core->add_DocumentTitleChanged(tt, &g_tabs[idx].titleToken);
            tt->Release();

            auto *sc = new SourceChangedHandler(tab.w, core);
            core->add_SourceChanged(sc, &g_tabs[idx].sourceToken);
            sc->Release();

            auto *navigation = new NavigationCompletedHandler(tab.w, core);
            core->add_NavigationCompleted(navigation, &g_tabs[idx].navigationToken);
            navigation->Release();

            ICoreWebView2_4 *core4 = nullptr;
            if (SUCCEEDED(core->QueryInterface(IID_PPV_ARGS(&core4))) && core4) {
                auto *download = new DownloadStartingHandler();
                core4->add_DownloadStarting(download, &g_tabs[idx].downloadToken);
                download->Release();
                core4->Release();
            }

            core->Release();
        }
    }

    if (!url.empty()) {
        webview_navigate(tab.w, WideToUtf8(url).c_str());
    }
    ActivateTab(idx);
}

static void CloseTab(int idx) {
    if (idx < 0 || idx >= (int)g_tabs.size()) {
        return;
    }
    int oldActive = g_activeTab;
    webview_destroy(g_tabs[idx].w);
    g_tabs.erase(g_tabs.begin() + idx);

    if (g_tabs.empty()) {
        PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
        return;
    }

    int newActive;
    if (idx < oldActive) {
        newActive = oldActive - 1;
    } else if (idx == oldActive) {
        newActive = (std::min)(idx, (int)g_tabs.size() - 1);
    } else {
        newActive = oldActive;
    }
    ActivateTab(newActive);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        UINT dpi = GetDpiForWindow(hwnd);
        g_scale = dpi / 96.0f;
        g_stripH = (int)(38 * g_scale);
        g_toolbarH = (int)(42 * g_scale);

        NONCLIENTMETRICSW ncm{};
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfWeight = FW_BOLD;
        g_boldFont = CreateFontIndirectW(&lf);
        g_boxBrush = CreateSolidBrush(kBoxBg);

        g_tabStrip = CreateWindowExW(
            0, L"TabStripWnd", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            nullptr, g_hInstance, nullptr);
        g_toolbar = CreateWindowExW(
            0, L"ToolbarWnd", nullptr, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            nullptr, g_hInstance, nullptr);
        g_addressEdit = CreateWindowExW(
            0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
            g_toolbar, (HMENU)(INT_PTR)ID_ADDRESS, g_hInstance, nullptr);
        g_container = CreateWindowExW(
            0, L"BrowserContainer", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, hwnd, nullptr,
            g_hInstance, nullptr);

        SendMessageW(g_addressEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
        g_prevEditProc = (WNDPROC)SetWindowLongPtrW(
            g_addressEdit, GWLP_WNDPROC, (LONG_PTR)AddressEditProc);

        HMENU hMenuBar = CreateMenu();
        HMENU hActivity = CreatePopupMenu();
        AppendMenuW(hActivity, MF_STRING, IDM_HISTORY, L"History\tCtrl+H");
        AppendMenuW(hActivity, MF_STRING, IDM_DOWNLOADS, L"Downloads\tCtrl+J");
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hActivity, L"Activity");
        HMENU hHelp = CreatePopupMenu();
        AppendMenuW(hHelp, MF_STRING, IDM_ABOUT, L"About...");
        AppendMenuW(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, L"Help");
        SetMenu(hwnd, hMenuBar);

        CreateTab(L"https://bilibili.com");
        break;
    }
    case WM_SIZE:
        LayoutControls();
        break;
    case WM_GETMINMAXINFO: {
        auto *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 480;
        mmi->ptMinTrackSize.y = 320;
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == ID_GO) {
            wchar_t buf[4096];
            GetWindowTextW(g_addressEdit, buf, 4096);
            std::wstring u = NormalizeUrl(buf);
            if (!u.empty() && g_activeTab >= 0 &&
                g_activeTab < (int)g_tabs.size()) {
                g_tabs[g_activeTab].url = u;
                webview_navigate(g_tabs[g_activeTab].w, WideToUtf8(u).c_str());
                SetWindowTextW(g_addressEdit, u.c_str());
            }
        } else if (id == ID_PLUS) {
            CreateTab(L"https://cn.bing.com");
        } else if (id >= ID_TAB_BASE &&
                   id < ID_TAB_BASE + (int)g_tabs.size()) {
            ActivateTab(id - ID_TAB_BASE);
        } else if (id >= ID_TAB_CLOSE_BASE &&
                   id < ID_TAB_CLOSE_BASE + (int)g_tabs.size()) {
            CloseTab(id - ID_TAB_CLOSE_BASE);
        }
        else if (id == ID_CLOSE_CURRENT) {
            if (g_activeTab >= 0 && g_activeTab < (int)g_tabs.size()) {
                CloseTab(g_activeTab);
            }
        }
        else if (id == ID_NEXT_TAB) {
            int n = (int)g_tabs.size();
            if (n > 1) {
                ActivateTab((g_activeTab + 1) % n);
            }
        }
        else if (id == ID_PREV_TAB) {
            int n = (int)g_tabs.size();
            if (n > 1) {
                ActivateTab((g_activeTab - 1 + n) % n);
            }
        }
        else if (id == ID_FOCUS_ADDRESS) {
            SetFocus(g_addressEdit);
            SendMessageW(g_addressEdit, EM_SETSEL, 0, -1);
        }
        else if (id == ID_OPEN_HISTORY || id == IDM_HISTORY) {
            OpenActivityWindow(false);
        }
        else if (id == ID_OPEN_DOWNLOADS || id == IDM_DOWNLOADS) {
            OpenActivityWindow(true);
        }
        else if (id == IDM_ABOUT) {
            MessageBoxW(hwnd, L"WebView Browser\nVersion " APP_VERSION, L"About",
                MB_OK | MB_ICONINFORMATION);
        }
        break;
    }
    case WM_APP_NEWTAB: {
        std::wstring *u = reinterpret_cast<std::wstring *>(lp);
        CreateTab(u ? *u : L"https://cn.bing.com");
        delete u;
        break;
    }
    case WM_APP_TOAST_ACTIVATED: {
        std::unique_ptr<std::wstring> arguments(reinterpret_cast<std::wstring *>(lp));
        if (!arguments) return 0;
        const std::wstring prefix = L"download:";
        const std::size_t separator = arguments->rfind(L':');
        if (arguments->rfind(prefix, 0) == 0 && separator != std::wstring::npos &&
            separator > prefix.size()) {
            const std::wstring id = arguments->substr(prefix.size(), separator - prefix.size());
            const std::wstring action = arguments->substr(separator + 1);
            DownloadEntry *entry = g_activityStore.FindDownload(id);
            ShowWindow(hwnd, SW_SHOW);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            if (entry && action == L"open" && FileExists(entry->savePath)) {
                ShellExecuteW(hwnd, L"open", entry->savePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            } else if (entry && action == L"retry") {
                g_pendingRetries[entry->url].push_back(entry->id);
                CreateTab(entry->url);
            } else {
                OpenActivityWindow(true, entry ? id : L"");
            }
        }
        return 0;
    }
    case WM_CLOSE:
        if (!g_allowClose && g_activityStore.ActiveDownloadCount() > 0) {
            const std::wstring message = std::to_wstring(g_activityStore.ActiveDownloadCount()) +
                L" download(s) are still in progress.\n\nExit and interrupt them?";
            if (MessageBoxW(hwnd, message.c_str(), L"Downloads in progress",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return 0;
            }
            g_allowClose = true;
            for (auto &[id, runtime] : g_downloadRuntime) {
                DownloadEntry *entry = g_activityStore.FindDownload(id);
                if (entry && ActivityStore::IsActiveDownloadState(entry->state) &&
                    runtime->operation) {
                    runtime->operation->Cancel();
                }
            }
        }
        while (!g_tabs.empty()) {
            webview_destroy(g_tabs[0].w);
            g_tabs.erase(g_tabs.begin());
        }
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY: {
        if (g_activityWnd) {
            DestroyWindow(g_activityWnd);
            g_activityWnd = nullptr;
        }
        for (auto &[id, runtime] : g_downloadRuntime) {
            if (runtime->operation) {
                runtime->operation->remove_BytesReceivedChanged(runtime->bytesToken);
                runtime->operation->remove_StateChanged(runtime->stateToken);
                runtime->operation->Release();
                runtime->operation = nullptr;
            }
        }
        g_downloadRuntime.clear();
        ShutdownNativeNotifications();
        NOTIFYICONDATAW notification{};
        notification.cbSize = sizeof(notification);
        notification.hWnd = hwnd;
        notification.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &notification);
        g_tabPaintBuffer.Reset();
        g_toolbarPaintBuffer.Reset();
        DeleteObject(g_boxBrush);
        DeleteObject(g_font);
        DeleteObject(g_boldFont);
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    RoInitialize(RO_INIT_SINGLETHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hInstance = hInstance;
    g_activityStore.Load();

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"BrowserMain";
    RegisterClassExW(&wc);

    WNDCLASSEXW cc{};
    cc.cbSize = sizeof(cc);
    cc.lpfnWndProc = DefWindowProcW;
    cc.hInstance = hInstance;
    cc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    cc.lpszClassName = L"BrowserContainer";
    RegisterClassExW(&cc);

    WNDCLASSEXW sc{};
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = TabStripProc;
    sc.hInstance = hInstance;
    sc.lpszClassName = L"TabStripWnd";
    RegisterClassExW(&sc);

    WNDCLASSEXW tc{};
    tc.cbSize = sizeof(tc);
    tc.lpfnWndProc = ToolbarProc;
    tc.hInstance = hInstance;
    tc.lpszClassName = L"ToolbarWnd";
    RegisterClassExW(&tc);

    WNDCLASSEXW ac{};
    ac.cbSize = sizeof(ac);
    ac.lpfnWndProc = ActivityWndProc;
    ac.hInstance = hInstance;
    ac.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    ac.hCursor = LoadCursor(nullptr, IDC_ARROW);
    ac.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    ac.lpszClassName = L"ActivityCenterWnd";
    RegisterClassExW(&ac);

    g_mainWnd = CreateWindowExW(
        0, L"BrowserMain", APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 1280, 800, nullptr, nullptr, hInstance, nullptr);
    if (!g_mainWnd) {
        RoUninitialize();
        CoUninitialize();
        return 1;
    }
    InitializeNativeNotifications(g_mainWnd, WM_APP_TOAST_ACTIVATED);

    ACCEL accels[] = {
        {FVIRTKEY | FCONTROL,          'T',    ID_PLUS},
        {FVIRTKEY | FCONTROL,          'W',    ID_CLOSE_CURRENT},
        {FVIRTKEY | FCONTROL,          VK_TAB, ID_NEXT_TAB},
        {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, ID_PREV_TAB},
        {FVIRTKEY | FCONTROL,          'L',    ID_FOCUS_ADDRESS},
        {FVIRTKEY | FCONTROL,          'H',    ID_OPEN_HISTORY},
        {FVIRTKEY | FCONTROL,          'J',    ID_OPEN_DOWNLOADS},
    };
    g_hAccel = CreateAcceleratorTableW(accels, ARRAYSIZE(accels));

    ShowWindow(g_mainWnd, nCmdShow);
    UpdateWindow(g_mainWnd);


    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(g_mainWnd, g_hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }


    if (g_hAccel) {
        DestroyAcceleratorTable(g_hAccel);
        g_hAccel = nullptr;
    }

    RoUninitialize();
    CoUninitialize();
    return (int)msg.wParam;

}
