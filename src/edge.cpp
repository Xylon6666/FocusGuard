#include "app.h"
#include <time.h>
#include <string.h>

/* ============================================================
   屏幕边缘呼吸灯（EdgeGlow）
   - 每条选中的边 = 一条窄的分层窗口，32bpp DIB 做「由边缘向内
     逐渐透明的渐变」，再用全局 alpha 做正弦式明暗呼吸
   - 渐变 + 峰值 alpha 收敛在 ~47%，画面柔和，只是余光提示
   - 三条时长策略：
       0 跟随弹窗（弹窗在就亮，弹窗灭就熄；无弹窗可跟时亮 3 秒）
       1 固定 1–10 秒（最后 0.5 秒压暗收尾）
       2 一直呼吸到点击（这时光带接收点击，点一下就熄）
   - 非点击模式下窗口带 WS_EX_TRANSPARENT，鼠标完全穿透，不挡操作
   ============================================================ */

#define EDGE_MAX_STRIP (EDGE_MAX_MON * 4)
#define EDGE_PERIOD    2000      /* 一次完整呼吸（亮→暗）2 秒 */
#define EDGE_PEAK      120       /* 最亮时的全局 alpha（0-255），约 47% */
#define EDGE_FADE_MS   500       /* 固定秒数模式收尾的淡出时长 */

struct Strip {
    HWND    wnd;
    HDC     mem;
    HBITMAP bmp;
    SIZE    sz;
    POINT   pos;
};

static Strip  s_st[EDGE_MAX_STRIP];
static int    s_nStrip = 0;
static BOOL   s_active = FALSE;
static BOOL   s_followPopup = FALSE;
static BOOL   s_clickMode = FALSE;
static DWORD  s_endTick = 0;         /* 0 = 不按时间结束 */
static DWORD  s_t0 = 0;
static HDC    s_screen = NULL;

static int S(int v) { return MulDiv(v, g_scale, 100); }

/* ---------------- 预设方案 ---------------- */
BOOL EdgePresetValues(int idx, int* sides, int* thick, COLORREF* color)
{
    switch (idx) {
    case 0: *sides = 15; *thick = 18; *color = RGB(0x4C, 0x8D, 0xFF); return TRUE; /* 柔和整圈 */
    case 1: *sides = 2;  *thick = 26; *color = RGB(0x2F, 0xD3, 0xC0); return TRUE; /* 底边呼吸 */
    case 2: *sides = 12; *thick = 34; *color = RGB(0x8A, 0x6B, 0xFF); return TRUE; /* 左右双栏 */
    case 3: *sides = 1;  *thick = 10; *color = RGB(0xE8, 0xF0, 0xFF); return TRUE; /* 顶边细线 */
    case 4: *sides = 3;  *thick = 22; *color = RGB(0xFF, 0x9A, 0x5C); return TRUE; /* 上下暖光 */
    }
    return FALSE;
}

const wchar_t* EdgePresetName(int idx)
{
    /* 名字要短到能在 186 DIP 宽的下拉框里整行显示，细节（颜色）留给下面的光色块 */
    switch (idx) {
    case 0: return L"柔和整圈 · 四边18px";
    case 1: return L"底边呼吸 · 下26px";
    case 2: return L"左右双栏 · 两栏34px";
    case 3: return L"顶边细线 · 上10px";
    case 4: return L"上下暖光 · 上下22px";
    default: return L"自定义 · 手动选边";
    }
}

/* ---------------- 窗口类 ---------------- */
static LRESULT CALLBACK EdgeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_NCHITTEST:
        /* 点击模式：接管鼠标，点一下即熄；其它模式穿透 */
        return s_clickMode ? HTCLIENT : HTTRANSPARENT;

    case WM_LBUTTONUP:
        if (s_clickMode) EdgeStop();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);      /* 内容由 UpdateLayeredWindow 提供 */
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void EdgeInit(void)
{
    static BOOL reg = FALSE;
    if (reg) return;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = EdgeProc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = APP_EDGE_CLASS;
    RegisterClassExW(&wc);
    reg = TRUE;
}

/* ---------------- 光带绘制 ---------------- */
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

/* 某条边在给定屏幕工作区里的矩形（t 为厚度像素） */
static void SideRect(int side, const RECT* mon, int t, RECT* r)
{
    switch (side) {
    case 0: r->left = mon->left;      r->top = mon->top;       r->right = mon->right; r->bottom = mon->top + t;    break;
    case 1: r->left = mon->left;      r->top = mon->bottom - t; r->right = mon->right; r->bottom = mon->bottom;   break;
    case 2: r->left = mon->left;      r->top = mon->top;       r->right = mon->left + t; r->bottom = mon->bottom; break;
    default: r->left = mon->right - t; r->top = mon->top;      r->right = mon->right; r->bottom = mon->bottom;    break;
    }
}

/* 一条光带：沿「跨厚度」方向做由外向内 255→0 的二次衰减渐变 */
static BOOL MakeStrip(int side, const RECT* mon)
{
    if (s_nStrip >= EDGE_MAX_STRIP) return FALSE;

    int t0 = S(g_cfg.edgeThick);
    if (t0 < 4) t0 = 4;
    RECT r0;
    SideRect(side, mon, t0, &r0);
    int w0 = r0.right - r0.left;
    int h0 = r0.bottom - r0.top;
    if (w0 < 2 || h0 < 2) return FALSE;

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    if (!s_clickMode) ex |= WS_EX_TRANSPARENT;     /* 鼠标完全穿透 */
    HWND wnd = CreateWindowExW(ex, APP_EDGE_CLASS, L"", WS_POPUP,
                               r0.left, r0.top, w0, h0, NULL, NULL, g_hinst, NULL);
    if (!wnd) return FALSE;

    /* 先显示（不抢焦点、不进任务栏），才拿得到这块屏的 DPI，用它定真实厚度 */
    ShowWindow(wnd, SW_SHOWNOACTIVATE);
    UINT dpi = DpiOfWindow(wnd);
    int  t   = MulDiv(g_cfg.edgeThick, (int)dpi, 96);
    if (t < 4) t = 4;
    if (t * 3 > (mon->bottom - mon->top)) t = (mon->bottom - mon->top) / 3;
    if (t * 3 > (mon->right - mon->left)) t = (mon->right - mon->left) / 3;
    if (t < 4) t = 4;

    RECT r;
    SideRect(side, mon, t, &r);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w < 2 || h < 2) { DestroyWindow(wnd); return FALSE; }

    Strip* p = &s_st[s_nStrip];
    ZeroMemory(p, sizeof(*p));
    p->wnd = wnd;
    p->sz.cx = w;
    p->sz.cy = h;
    p->pos.x = r.left;
    p->pos.y = r.top;

    if (!s_screen) s_screen = GetDC(NULL);
    if (!s_screen) { DestroyWindow(wnd); ZeroMemory(p, sizeof(*p)); return FALSE; }

    p->mem = CreateCompatibleDC(s_screen);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;         /* 自上而下，方便按行填 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    p->bmp = CreateDIBSection(s_screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!p->mem || !p->bmp || !bits) {
        if (p->bmp) DeleteObject(p->bmp);
        if (p->mem) DeleteDC(p->mem);
        DestroyWindow(wnd);
        ZeroMemory(p, sizeof(*p));
        return FALSE;
    }
    SelectObject(p->mem, p->bmp);

    /* 渐变：边缘最亮 → 向内衰减到 0；平方衰减更像光晕 */
    DWORD col = g_cfg.edgeColor;
    int R = GetRValue(col), G = GetGValue(col), B = GetBValue(col);
    int across = (side == 0 || side == 1) ? h : w;
    BYTE* base = (BYTE*)bits;

    for (int yy = 0; yy < h; yy++) {
        int d;
        if (side == 0)      d = yy;
        else if (side == 1) d = h - 1 - yy;
        else                d = -1;       /* 左右边按列算 */

        for (int xx = 0; xx < w; xx++) {
            int dd = d;
            if (dd < 0) dd = (side == 2) ? xx : (w - 1 - xx);

            int a;
            if (across <= 1) a = 255;
            else {
                double k = (double)(across - dd) / across;   /* 1 贴边 → 0 内侧 */
                if (k <= 0) a = 0;
                else {
                    a = (int)(k * k * 255.0);
                    if (a > 255) a = 255;
                }
            }

            BYTE* q = base + ((size_t)yy * w + xx) * 4;
            /* 分层窗口要求预乘 alpha：BGRA 顺序 */
            q[0] = (BYTE)((B * a) / 255);
            q[1] = (BYTE)((G * a) / 255);
            q[2] = (BYTE)((R * a) / 255);
            q[3] = (BYTE)a;
        }
    }

    s_nStrip++;
    return TRUE;
}

/* 一次整体刷新（呼吸用一个全局 alpha，几十个像素改动都在这一个调用里） */
static void Paint(BYTE ga)
{
    if (!s_screen) return;
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = ga;
    bf.AlphaFormat         = AC_SRC_ALPHA;

    for (int i = 0; i < s_nStrip; i++) {
        Strip* p = &s_st[i];
        if (!p->wnd || !p->mem) continue;
        POINT src;
        src.x = 0; src.y = 0;
        UpdateLayeredWindow(p->wnd, s_screen, &p->pos, &p->sz, p->mem, &src,
                            0, &bf, ULW_ALPHA);
    }
}

static void KillStrips(void)
{
    for (int i = 0; i < s_nStrip; i++) {
        Strip* p = &s_st[i];
        if (p->wnd) DestroyWindow(p->wnd);
        if (p->bmp) DeleteObject(p->bmp);
        if (p->mem) DeleteDC(p->mem);
        ZeroMemory(p, sizeof(*p));
    }
    s_nStrip = 0;
}

/* ---------------- 显示器集合 ---------------- */
struct EMonSet { RECT r[EDGE_MAX_MON]; int n; };

static BOOL CALLBACK EMonProc(HMONITOR mon, HDC hdc, LPRECT lprc, LPARAM lp)
{
    (void)hdc;
    EMonSet* set = (EMonSet*)lp;
    if (set->n >= EDGE_MAX_MON) return FALSE;

    /* 用工作区而不是整块屏：任务栏在哪都不遮住，和「鼠标所在显示器」口径一致 */
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) set->r[set->n++] = mi.rcWork;
    else                                 set->r[set->n++] = *lprc;
    return TRUE;
}

static int ECollectWorkRects(EMonSet* set)
{
    set->n = 0;
    if (g_cfg.edgeMonitor == 1) {
        EnumDisplayMonitors(NULL, NULL, EMonProc, (LPARAM)set);
    } else {
        POINT pt;
        pt.x = 0; pt.y = 0;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) { set->r[0] = mi.rcWork; set->n = 1; }
    }
    if (set->n <= 0) {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) { set->r[0] = wa; set->n = 1; }
        else {
            set->r[0].left = 0; set->r[0].top = 0;
            set->r[0].right = GetSystemMetrics(SM_CXSCREEN);
            set->r[0].bottom = GetSystemMetrics(SM_CYSCREEN);
            set->n = 1;
        }
    }
    return set->n;
}

/* ---------------- 对外 ---------------- */
void EdgeStart(int src)
{
    if (!g_cfg.edgeEnabled) return;
    BOOL wantPopup = (src & EDGE_ON_POPUP) && g_cfg.edgeByPopup;
    BOOL wantSound = (src & EDGE_ON_SOUND) && g_cfg.edgeBySound;
    if (!wantPopup && !wantSound) return;

    EdgeStop();
    EdgeInit();

    /* 时长策略 */
    if (g_cfg.edgeMode == 2) {
        s_clickMode = TRUE;                        /* 一直呼吸到点击 */
    } else if (g_cfg.edgeMode == 1) {
        s_endTick = GetTickCount() + (DWORD)g_cfg.edgeSecs * 1000;
    } else {
        /* 跟随弹窗：弹窗正在显示就跟着它；本次只是提示音（或弹窗已关）→ 亮 3 秒 */
        if (wantPopup && PopupVisible()) s_followPopup = TRUE;
        else s_endTick = GetTickCount() + 3000;
    }

    EMonSet set;
    int nMon = ECollectWorkRects(&set);
    int sides = g_cfg.edgeSides;
    for (int m = 0; m < nMon; m++)
        for (int s = 0; s < 4; s++)
            if (sides & (1 << s)) MakeStrip(s, &set.r[m]);

    if (s_nStrip <= 0) {                            /* 全失败则安静退出 */
        s_clickMode = FALSE;
        s_endTick = 0;
        if (s_screen) { ReleaseDC(NULL, s_screen); s_screen = NULL; }
        return;
    }

    s_t0 = GetTickCount();
    SetTimer(g_hwndMain, TIMER_EDGE, 50, NULL);
    s_active = TRUE;
    EdgeTick();                                     /* 先画一帧（alpha 近 0）*/
}

void EdgeStop(void)
{
    if (!g_hwndMain) {
        KillStrips();
        s_active = FALSE;
        return;
    }
    KillTimer(g_hwndMain, TIMER_EDGE);
    KillStrips();
    if (s_screen) { ReleaseDC(NULL, s_screen); s_screen = NULL; }
    s_active      = FALSE;
    s_followPopup = FALSE;
    s_clickMode   = FALSE;
    s_endTick     = 0;
}

void EdgeTick(void)
{
    if (!s_active) return;

    /* 跟随弹窗：弹窗一消失就熄 */
    if (s_followPopup && !PopupVisible()) { EdgeStop(); return; }

    BYTE peak = EDGE_PEAK;

    if (s_endTick) {
        int left = (int)(s_endTick - GetTickCount());
        if (left <= 0) { EdgeStop(); return; }
        if (left < EDGE_FADE_MS) peak = (BYTE)((int)peak * left / EDGE_FADE_MS);
    }

    /* 平滑三角波：0 → 1 → 0，峰值处导数为 0，看起来像呼吸而不是闪 */
    DWORD el = GetTickCount() - s_t0;
    int half = EDGE_PERIOD / 2;
    int t = (int)(el % (DWORD)EDGE_PERIOD);
    double x = (t <= half) ? (double)t / half : (double)(EDGE_PERIOD - t) / half;
    double sm = x * x * (3.0 - 2.0 * x);

    BYTE ga = (BYTE)(sm * peak);
    Paint(ga);
}

BOOL EdgeVisible(void) { return s_active; }

/* 设置界面的预览：临时打开总开关与「弹窗触发」，按固定 4 秒亮一遍 */
void EdgePreview(void)
{
    BOOL    saveOn   = g_cfg.edgeEnabled;
    BOOL    saveByP  = g_cfg.edgeByPopup;
    int     saveMode = g_cfg.edgeMode;
    int     saveSecs = g_cfg.edgeSecs;

    g_cfg.edgeEnabled = TRUE;
    g_cfg.edgeByPopup = TRUE;
    g_cfg.edgeMode    = 1;
    g_cfg.edgeSecs    = 4;

    EdgeStart(EDGE_ON_POPUP);

    g_cfg.edgeEnabled = saveOn;
    g_cfg.edgeByPopup = saveByP;
    g_cfg.edgeMode    = saveMode;
    g_cfg.edgeSecs    = saveSecs;
}
