#define WIN32_LEAN_AND_MEAN
#define WEBVIEW_IMPLEMENTATION
#include "webview.h"

#include <windows.h>
#include <windowsx.h>
#include <objbase.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#define APP_NAME    L"WebView Browser"
#define APP_VERSION L"1.2.0"

#define IDM_ABOUT         1001
#define ID_ADDRESS        2001
#define ID_GO             2002
#define ID_PLUS           2003
#define ID_CLOSE_CURRENT  2004
#define ID_NEXT_TAB       2005
#define ID_PREV_TAB       2006
#define ID_FOCUS_ADDRESS  2007
#define ID_TAB_BASE       4000
#define ID_TAB_CLOSE_BASE 5000

#define WM_APP_NEWTAB (WM_APP + 1)

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
    HIT_GO
};

static HINSTANCE g_hInstance;
static HWND g_mainWnd;
static HWND g_tabStrip;
static HWND g_toolbar;
static HWND g_container;
static HWND g_addressEdit;
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
static int g_activeTab = -1;

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
    Invoke(ICoreWebView2 *, ICoreWebView2SourceChangedEventArgs *) override {
        int idx = FindTabIndex(m_w);
        if (idx >= 0) {
            LPWSTR src = nullptr;
            if (SUCCEEDED(m_core->get_Source(&src)) && src) {
                g_tabs[idx].url = src;
                CoTaskMemFree(src);
                if (idx == g_activeTab) {
                    SetWindowTextW(g_addressEdit, g_tabs[idx].url.c_str());
                }
            }
        }
        return S_OK;
    }

private:
    webview_t m_w;
    ICoreWebView2 *m_core;
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
        bool over = PtInRect(&g_goRc, pt);
        if ((g_hoverKind == HIT_GO) != over) {
            g_hoverKind = over ? HIT_GO : HIT_NONE;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TrackMouseLeave(hwnd);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hoverKind == HIT_GO) {
            g_hoverKind = HIT_NONE;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        g_pressKind = PtInRect(&g_goRc, pt) ? HIT_GO : HIT_NONE;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (g_pressKind == HIT_GO && PtInRect(&g_goRc, pt)) {
            PostMessageW(g_mainWnd, WM_COMMAND, MAKEWPARAM(ID_GO, 0), 0);
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
    g_boxRc = {padX, padY, trc.right - padX - goW - (int)(8 * g_scale),
               trc.bottom - padY};
    g_goRc = {g_boxRc.right + (int)(8 * g_scale), padY, trc.right - padX,
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
        else if (id == IDM_ABOUT) {
            MessageBoxW(hwnd, L"WebView Browser\nVersion 1.2.0", L"About",
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
    case WM_CLOSE:
        while (!g_tabs.empty()) {
            webview_destroy(g_tabs[0].w);
            g_tabs.erase(g_tabs.begin());
        }
        DestroyWindow(hwnd);
        break;
    case WM_DESTROY:
        g_tabPaintBuffer.Reset();
        g_toolbarPaintBuffer.Reset();
        DeleteObject(g_boxBrush);
        DeleteObject(g_font);
        DeleteObject(g_boldFont);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hInstance = hInstance;

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

    g_mainWnd = CreateWindowExW(
        0, L"BrowserMain", APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 1280, 800, nullptr, nullptr, hInstance, nullptr);
    if (!g_mainWnd) {
        CoUninitialize();
        return 1;
    }

    ACCEL accels[] = {
        {FVIRTKEY | FCONTROL,          'T',    ID_PLUS},
        {FVIRTKEY | FCONTROL,          'W',    ID_CLOSE_CURRENT},
        {FVIRTKEY | FCONTROL,          VK_TAB, ID_NEXT_TAB},
        {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, ID_PREV_TAB},
        {FVIRTKEY | FCONTROL,          'L',    ID_FOCUS_ADDRESS},
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

    CoUninitialize();
    return (int)msg.wParam;

}
