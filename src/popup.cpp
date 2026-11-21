#include "app.h"
#include <string.h>

/* ============================================================
   提醒弹窗：分层窗口 + 圆角 + 不抢焦点 + 自动消失
   屏幕选择：主显示器 / 鼠标所在显示器 / 所有显示器（每屏一个窗口）
   每块屏按自己的 DPI 算尺寸与字号，混合缩放下也不会被系统拉伸
   ============================================================ */

#define POP_MAX 8

static HWND    s_wnd[POP_MAX];
static BOOL    s_visible = FALSE;
static int     s_remain = 0;        /* 自动消失倒计时（秒） */
static BOOL    s_clickOnly = FALSE; /* 点击才关闭 */
static HBRUSH  s_bgBrush = NULL;
static HBRUSH  s_edgeBrush = NULL;
static wchar_t s_text[TXT_MAX] = L"";

struct FontSlot { UINT dpi; HFONT f; };
static FontSlot s_fc[POP_MAX];
static int      s_fcN = 0;

static int S(int v) { return MulDiv(v, g_scale, 100); }

/* 窗口所在显示器的 DPI（Win10 1607+），老系统回退系统 DPI */
static UINT DpiOfWindow(HWND h)
{
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow fn = NULL;
    static BOOL tried = FALSE;
    if (!tried) {
        tried = TRUE;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) fn = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
    }
    if (fn) {
        UINT d = fn(h);
        if (d) return d;
    }
    return (UINT)g_dpi;
}

static void FreeFonts(void)
{
    for (int i = 0; i < s_fcN; i++)
        if (s_fc[i].f) DeleteObject(s_fc[i].f);
    s_fcN = 0;
}

static HFONT FontForDpi(UINT dpi, int size)
{
    for (int i = 0; i < s_fcN; i++)
        if (s_fc[i].dpi == dpi) return s_fc[i].f;
    if (s_fcN >= POP_MAX) return s_fc[0].f;

    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight  = -MulDiv(size, (int)dpi, 96);
    lf.lfWeight  = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);

    s_fc[s_fcN].dpi = dpi;
    s_fc[s_fcN].f   = CreateFontIndirectW(&lf);
    s_fcN++;
    return s_fc[s_fcN - 1].f;
}

static HFONT FontOfWindow(HWND h)
{
    UINT dpi = (UINT)(ULONG_PTR)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!dpi) dpi = (UINT)g_dpi;
    for (int i = 0; i < s_fcN; i++)
        if (s_fc[i].dpi == dpi) return s_fc[i].f;
    return s_fcN ? s_fc[0].f : NULL;
}

static void FreeGfx(void)
{
    if (s_bgBrush)   { DeleteObject(s_bgBrush);   s_bgBrush = NULL; }
    if (s_edgeBrush) { DeleteObject(s_edgeBrush); s_edgeBrush = NULL; }
    FreeFonts();
}

static BYTE MixByte(BYTE v, int delta)
{
    int r = (int)v + delta;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return (BYTE)r;
}

static void BuildGfx(const AppSettings* c)
{
    FreeGfx();
    s_bgBrush = CreateSolidBrush(c->bgColor);
    s_edgeBrush = CreateSolidBrush(RGB(MixByte(GetRValue(c->bgColor), 34),
                                       MixByte(GetGValue(c->bgColor), 34),
                                       MixByte(GetBValue(c->bgColor), 34)));
    /* 字体按各窗口的 DPI 惰性创建 */
}

/* ---------- 取一组屏幕（工作区） ---------- */
struct Slot { RECT work; BOOL custom; };
struct MonSet { RECT r[POP_MAX]; int n; };

static BOOL CALLBACK MonEnumProc(HMONITOR mon, HDC hdc, LPRECT lprc, LPARAM lp)
{
    (void)hdc;
    MonSet* set = (MonSet*)lp;
    if (set->n >= POP_MAX) return FALSE;

    /* 取工作区，保证弹窗落在任务栏之外 */
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) set->r[set->n++] = mi.rcWork;
    else                                  set->r[set->n++] = *lprc;
    return TRUE;
}

static int CollectSlots(const AppSettings* c, Slot* out)
{
    if (c->popupPos == 5) {          /* 自定义坐标 = 绝对屏幕坐标，只放一个 */
        out[0].custom = TRUE;
        out[0].work.left   = S(c->customX);
        out[0].work.top    = S(c->customY);
        out[0].work.right  = out[0].work.left + S(c->popupWidth);
        out[0].work.bottom = out[0].work.top + S(c->popupHeight);
        return 1;
    }

    MonSet set;
    set.n = 0;

    if (c->popupMonitor == 2) {
        EnumDisplayMonitors(NULL, NULL, MonEnumProc, (LPARAM)&set);
    } else if (c->popupMonitor == 1) {
        POINT pt;
        pt.x = 0; pt.y = 0;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) { set.r[0] = mi.rcWork; set.n = 1; }
    } else {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) { set.r[0] = wa; set.n = 1; }
    }

    if (set.n <= 0) {                /* 兜底：主显示器工作区 */
        POINT zero;
        zero.x = 0; zero.y = 0;
        HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) { set.r[0] = mi.rcWork; set.n = 1; }
        else {
            set.r[0].left = 0; set.r[0].top = 0;
            set.r[0].right = GetSystemMetrics(SM_CXSCREEN);
            set.r[0].bottom = GetSystemMetrics(SM_CYSCREEN);
            set.n = 1;
        }
    }

    for (int i = 0; i < set.n; i++) {
        out[i].custom = FALSE;
        out[i].work   = set.r[i];
    }
    return set.n;
}

/* 在给定屏幕工作区里按位置预设算出弹窗矩形 */
static void PlaceSlot(const Slot* s, const AppSettings* c, UINT dpi,
                      int w, int hh, RECT* rc)
{
    if (s->custom) {
        rc->left = s->work.left;
        rc->top  = s->work.top;
    } else {
        RECT wa = s->work;
        int  m  = MulDiv(24, (int)dpi, 96);
        int x, y;
        switch (c->popupPos) {
        case 0: x = wa.left + m;              y = wa.top + m; break;
        case 1: x = wa.right - m - w;         y = wa.top + m; break;
        case 2: x = wa.left + m;              y = wa.bottom - m - hh; break;
        case 3: x = wa.right - m - w;         y = wa.bottom - m - hh; break;
        default:                              /* 4 居中 */
            x = wa.left + ((wa.right - wa.left) - w) / 2;
            y = wa.top + ((wa.bottom - wa.top) - hh) / 2;
            break;
        }
        rc->left = x;
        rc->top  = y;
    }
    rc->right  = rc->left + w;
    rc->bottom = rc->top + hh;
}

static LRESULT CALLBACK PopupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONUP:
        PopupHide();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);

        FillRect(hdc, &rc, s_bgBrush ? s_bgBrush : (HBRUSH)(COLOR_WINDOW + 1));
        if (s_edgeBrush) FrameRect(hdc, &rc, s_edgeBrush);

        if (s_text[0]) {
            UINT dpi = (UINT)(ULONG_PTR)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (!dpi) dpi = (UINT)g_dpi;
            int padX = MulDiv(28, (int)dpi, 96);
            int padY = MulDiv(18, (int)dpi, 96);

            RECT tr;
            tr.left = rc.left + padX;
            tr.right = rc.right - padX;
            tr.top = rc.top + padY;
            tr.bottom = rc.bottom - padY;

            int saved = SaveDC(hdc);
            HFONT f = FontOfWindow(h);
            if (f) SelectObject(hdc, f);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, g_cfg.fgColor);

            RECT calc = tr;
            DrawTextW(hdc, s_text, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            int th = calc.bottom - calc.top;
            int avail = tr.bottom - tr.top;
            int y = tr.top + (avail - th) / 2;
            if (y < tr.top) y = tr.top;

            RECT draw;
            draw.left = tr.left; draw.right = tr.right;
            draw.top = y; draw.bottom = y + th;
            DrawTextW(hdc, s_text, -1, &draw, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

            RestoreDC(hdc, saved);
        }

        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void PopupInit(void)
{
    static BOOL reg = FALSE;
    if (reg) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PopupProc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_POPUP_CLASS;
    RegisterClassExW(&wc);
    reg = TRUE;
}

/* 按需建窗口：同一块屏反复提醒时复用，不重复创建 */
static HWND EnsureWindow(int idx)
{
    if (idx < 0 || idx >= POP_MAX) return NULL;
    if (s_wnd[idx] && IsWindow(s_wnd[idx])) return s_wnd[idx];

    PopupInit();
    s_wnd[idx] = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        APP_POPUP_CLASS, L"", WS_POPUP, 0, 0, 100, 100,
        NULL, NULL, g_hinst, NULL);
    return s_wnd[idx];
}

void PopupShow(const AppSettings* c)
{
    if (!c->popupEnabled) return;

    Slot slots[POP_MAX];
    int n = CollectSlots(c, slots);
    if (n <= 0) return;

    lstrcpynW(s_text, c->text, TXT_MAX);
    BuildGfx(c);

    int alpha = c->opacity * 255 / 100;
    if (alpha < 26) alpha = 26;
    if (alpha > 255) alpha = 255;

    for (int i = 0; i < n; i++) {
        HWND h = EnsureWindow(i);
        if (!h) continue;

        /* 先粗放到目标屏，让窗口换来这块屏的 DPI，再按它算真实尺寸 */
        SetWindowPos(h, c->topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     slots[i].work.left, slots[i].work.top, 10, 10,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);

        UINT dpi = DpiOfWindow(h);
        int  w   = MulDiv(c->popupWidth,  (int)dpi, 96);
        int  hh  = MulDiv(c->popupHeight, (int)dpi, 96);

        RECT rc;
        PlaceSlot(&slots[i], c, dpi, w, hh, &rc);

        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)dpi);
        FontForDpi(dpi, c->fontSize);

        SetWindowPos(h, c->topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     rc.left, rc.top, w, hh,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

        int rad = MulDiv(18, (int)dpi, 96);
        if (rad > hh / 3) rad = hh / 3;
        if (rad < 4) rad = 4;
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, hh + 1, rad, rad);
        if (rgn) SetWindowRgn(h, rgn, TRUE);   /* 成功后区域归系统所有 */

        SetLayeredWindowAttributes(h, 0, (BYTE)alpha, LWA_ALPHA);
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);
    }

    s_remain    = c->autoClose ? c->duration : 0;
    s_clickOnly = c->autoClose ? FALSE : TRUE;
    s_visible   = TRUE;
}

void PopupHide(void)
{
    for (int i = 0; i < POP_MAX; i++)
        if (s_wnd[i] && IsWindow(s_wnd[i])) ShowWindow(s_wnd[i], SW_HIDE);
    s_visible = FALSE;
    s_remain  = 0;
}

void PopupTick(void)
{
    if (!s_visible || s_clickOnly) return;
    if (s_remain > 0) {
        s_remain--;
        if (s_remain <= 0) PopupHide();
    }
}

BOOL PopupVisible(void) { return s_visible; }
HWND PopupWindow(void)  { return s_wnd[0]; }
