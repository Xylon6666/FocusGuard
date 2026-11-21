#include "app.h"
#include <commctrl.h>
#include <string.h>

/* ============================================================
   专注守卫 —— 主程序
   单线程消息循环 · 隐藏消息窗口 · 托盘常驻 · 状态机调度
   ============================================================ */

AppSettings g_cfg;
HINSTANCE   g_hinst = NULL;
HWND        g_hwndMain = NULL;
int         g_dpi = 96;
int         g_scale = 100;

enum {
    ID_TRAY_REMIND = 2001,
    ID_TRAY_PAUSE = 2002,
    ID_PAUSE_10 = 2010,
    ID_PAUSE_30 = 2011,
    ID_PAUSE_60 = 2012,
    ID_PAUSE_CUSTOM = 2013,
    ID_PAUSE_RESUME = 2014,
    ID_TRAY_SETTINGS = 2020,
    ID_TRAY_POPUP  = 2021,     /* 托盘：一键开关弹窗通道 */
    ID_TRAY_SOUND  = 2022,     /* 托盘：一键开关提示音通道 */
    ID_TRAY_KEEP   = 2023,     /* 托盘：保持屏幕常亮 */
    ID_TRAY_EXIT = 2030
};

static HICON   g_icon = NULL;
static BOOL    g_trayAdded = FALSE;
/* 两条独立计时通道：弹窗 / 提示音 */
struct ChanTiming { time_t nextAt; BOOL idleArmed; };
static ChanTiming g_tPopup = { 0, FALSE };
static ChanTiming g_tSound = { 0, FALSE };
static time_t  g_pauseUntil = 0;
static BOOL    g_suppressed = FALSE;
static wchar_t g_state[96] = L"运行中";
static wchar_t g_lastTip[100] = L"";

/* 前台进程缓存 */
static HWND    g_cacheWnd = NULL;
static wchar_t g_cacheExe[MAX_PATH] = L"";

/* ==================== 小工具 ==================== */
static UINT IdleSeconds(void)
{
    LASTINPUTINFO li;
    li.cbSize = sizeof(li);
    li.dwTime = 0;
    if (!GetLastInputInfo(&li)) return 0;
    DWORD now = GetTickCount();
    return (UINT)((now - li.dwTime) / 1000);
}

static void BaseName(const wchar_t* path, wchar_t* out, int cch)
{
    const wchar_t* s = wcsrchr(path, L'\\');
    lstrcpynW(out, s ? s + 1 : path, cch);
}

static BOOL OwnWindow(HWND h)
{
    if (!h) return TRUE;
    if (h == g_hwndMain) return TRUE;
    if (h == PopupWindow()) return TRUE;
    HWND p = GetParent(h);
    while (p) {
        if (p == g_hwndMain || p == PopupWindow()) return TRUE;
        p = GetParent(p);
    }
    return FALSE;
}

/* 前台进程的可执行文件名（按 HWND 缓存，避免每秒 OpenProcess） */
static BOOL FgExeName(wchar_t* out, int cch)
{
    out[0] = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return FALSE;
    if (OwnWindow(fg)) {
        g_cacheWnd = fg;
        g_cacheExe[0] = 0;
        return FALSE;
    }

    if (fg == g_cacheWnd) {
        lstrcpynW(out, g_cacheExe, cch);
        return out[0] ? TRUE : FALSE;
    }

    g_cacheWnd = fg;
    g_cacheExe[0] = 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid) {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (hp) {
            wchar_t full[MAX_PATH * 2];
            DWORD sz = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(hp, 0, full, &sz))
                BaseName(full, g_cacheExe, MAX_PATH);
            CloseHandle(hp);
        }
    }
    lstrcpynW(out, g_cacheExe, cch);
    return out[0] ? TRUE : FALSE;
}

/* 全屏检测：前台窗口覆盖整块显示器 */
static BOOL IsFullscreenFg(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return FALSE;
    if (OwnWindow(fg)) return FALSE;

    wchar_t cls[64] = L"";
    GetClassNameW(fg, cls, _countof(cls));
    if (!_wcsicmp(cls, L"Progman") || !_wcsicmp(cls, L"WorkerW") ||
        !_wcsicmp(cls, L"Shell_TrayWnd") || !_wcsicmp(cls, L"Shell_SecondaryTrayWnd"))
        return FALSE;

    if (!IsWindowVisible(fg) || IsIconic(fg)) return FALSE;

    RECT r;
    if (!GetWindowRect(fg, &r)) return FALSE;

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    HMONITOR hm = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(hm, &mi)) return FALSE;
    RECT m = mi.rcMonitor;

    if (r.left   <= m.left   + 1 && r.top    <= m.top    + 1 &&
        r.right  >= m.right  - 1 && r.bottom >= m.bottom - 1) return TRUE;
    return FALSE;
}

static BOOL WhitelistHit(void)
{
    wchar_t exe[MAX_PATH];
    if (!FgExeName(exe, MAX_PATH)) return FALSE;
    for (int i = 0; i < g_cfg.wlCount; i++)
        if (!_wcsicmp(g_cfg.wl[i], exe)) return TRUE;
    return FALSE;
}

static BOOL InSchedule(void)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (!g_cfg.days[(int)st.wDayOfWeek]) return FALSE;

    int nowMin = (int)st.wHour * 60 + (int)st.wMinute;
    int s = g_cfg.startHour * 60 + g_cfg.startMinute;
    int e = g_cfg.endHour * 60 + g_cfg.endMinute;
    if (s == e) return TRUE;
    if (s < e) return (nowMin >= s && nowMin < e) ? TRUE : FALSE;
    return (nowMin >= s || nowMin < e) ? TRUE : FALSE;   /* 跨午夜 */
}

/* ==================== 状态文本 ==================== */
static void BuildState(time_t now, BOOL fs, BOOL wl, BOOL outside)
{
    if (g_pauseUntil) {
        int mins = (int)((g_pauseUntil - now + 59) / 60);
        if (mins < 1) mins = 1;
        _snwprintf_s(g_state, _countof(g_state), _TRUNCATE, L"已暂停 · 剩余 %d 分钟", mins);
    } else if (fs) {
        lstrcpynW(g_state, L"全屏静默中", _countof(g_state));
    } else if (wl) {
        lstrcpynW(g_state, L"白名单静默中", _countof(g_state));
    } else if (outside) {
        lstrcpynW(g_state, L"非生效时段", _countof(g_state));
    } else if (!g_cfg.popupEnabled && !g_cfg.soundEnabled) {
        lstrcpynW(g_state, L"运行中 · 弹窗与提示音均已停用", _countof(g_state));
    } else {
        /* 两条通道各自显示下一次到点时间 */
        int pm = 0, sm = 0;
        if (g_tPopup.nextAt) {
            pm = (int)((g_tPopup.nextAt - now + 59) / 60);
            if (pm < 1) pm = 1;
        }
        if (g_tSound.nextAt) {
            sm = (int)((g_tSound.nextAt - now + 59) / 60);
            if (sm < 1) sm = 1;
        }
        if (pm && sm) {
            _snwprintf_s(g_state, _countof(g_state), _TRUNCATE,
                         L"运行中 · 弹窗 %d 分 / 提示音 %d 分", pm, sm);
        } else if (pm) {
            _snwprintf_s(g_state, _countof(g_state), _TRUNCATE,
                         L"运行中 · 弹窗约 %d 分钟后提醒", pm);
        } else if (sm) {
            _snwprintf_s(g_state, _countof(g_state), _TRUNCATE,
                         L"运行中 · 提示音约 %d 分钟后响", sm);
        } else {
            int th = 0;
            if (g_cfg.popupIdleOn && g_cfg.popupEnabled) th = g_cfg.popupIdleMin;
            if (g_cfg.soundIdleOn && g_cfg.soundEnabled)
                th = th ? (th < g_cfg.soundIdleMin ? th : g_cfg.soundIdleMin) : g_cfg.soundIdleMin;
            if (th)
                _snwprintf_s(g_state, _countof(g_state), _TRUNCATE,
                             L"运行中 · 空闲 %d 分钟触发", th);
            else
                lstrcpynW(g_state, L"运行中", _countof(g_state));
        }
    }
}

void AppStateText(wchar_t* out, int cch)
{
    lstrcpynW(out, g_state, cch);
}

/* ==================== 托盘 ==================== */
static void TipText(wchar_t* out, int cch)
{
    wchar_t t[128];
    _snwprintf_s(t, _countof(t), _TRUNCATE, L"%s — %s", APP_NAME, g_state);
    lstrcpynW(out, t, cch);
}

static void UpdateTip(BOOL force)
{
    if (!g_trayAdded) return;
    wchar_t tip[100];
    TipText(tip, _countof(tip));
    if (!force && !lstrcmpW(tip, g_lastTip)) return;
    lstrcpynW(g_lastTip, tip, _countof(g_lastTip));

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwndMain;
    nid.uID    = 1;
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, tip, _countof(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static BOOL TrayAdd(void)
{
    if (!g_icon) {
        g_icon = (HICON)LoadImageW(g_hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
        if (!g_icon) g_icon = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_APP));
        if (!g_icon) g_icon = LoadIconW(NULL, IDI_APPLICATION);
    }

    wchar_t tip[100];
    TipText(tip, _countof(tip));
    lstrcpynW(g_lastTip, tip, _countof(g_lastTip));

    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = g_hwndMain;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_MSG;
    nid.hIcon            = g_icon;
    lstrcpynW(nid.szTip, tip, _countof(nid.szTip));

    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) ? TRUE : FALSE;
    return g_trayAdded;
}

static void TrayRemove(void)
{
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = g_hwndMain;
    nid.uID    = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = FALSE;
}

/* ==================== 暂停 / 提醒 ==================== */
static void ResetInterval(time_t now)
{
    g_tPopup.nextAt = (g_cfg.popupEnabled && g_cfg.popupIntervalOn)
                      ? now + (time_t)g_cfg.popupIntervalMin * 60 : 0;
    g_tSound.nextAt = (g_cfg.soundEnabled && g_cfg.soundIntervalOn)
                      ? now + (time_t)g_cfg.soundIntervalMin * 60 : 0;
}

/* 弹窗通道命中一次（只复位自己的下一次） */
static void FirePopup(time_t now)
{
    if (g_cfg.popupEnabled) {
        PopupShow(&g_cfg);
        EdgeStart(EDGE_ON_POPUP);          /* 边缘呼吸灯跟着弹窗亮 */
    }
    g_tPopup.nextAt = (g_cfg.popupEnabled && g_cfg.popupIntervalOn)
                      ? now + (time_t)g_cfg.popupIntervalMin * 60 : 0;
}

/* 提示音通道命中一次 */
static void FireSound(time_t now)
{
    if (g_cfg.soundEnabled) {
        AudioPlay(&g_cfg);
        EdgeStart(EDGE_ON_SOUND);          /* 边缘呼吸灯跟着提示音亮 */
    }
    g_tSound.nextAt = (g_cfg.soundEnabled && g_cfg.soundIntervalOn)
                      ? now + (time_t)g_cfg.soundIntervalMin * 60 : 0;
}

static void SetPause(int minutes)
{
    g_pauseUntil     = time(NULL) + (time_t)minutes * 60;
    g_tPopup.idleArmed = TRUE;
    g_tSound.idleArmed = TRUE;
    UpdateTip(TRUE);
}

void AppTriggerReminder(void)
{
    time_t now = time(NULL);
    int src = 0;
    if (g_cfg.popupEnabled) { PopupShow(&g_cfg); src |= EDGE_ON_POPUP; }
    if (g_cfg.soundEnabled) { AudioPlay(&g_cfg); src |= EDGE_ON_SOUND; }
    if (src) EdgeStart(src);              /* 哪条通道真的响了，就按哪条判定是否带光 */
    ResetInterval(now);
}

void AppOnSettingsChanged(void)
{
    ResetInterval(time(NULL));
    if (!g_cfg.soundEnabled) AudioStop();
    if (!g_cfg.popupEnabled) PopupHide();
    if (!g_cfg.edgeEnabled) EdgeStop();
    UpdateTip(TRUE);
}

/* ==================== 开机自启 ==================== */
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

void AppSetAutoStart(BOOL on)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE,
                        NULL, &k, NULL) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(NULL, exe, _countof(exe));
        wchar_t val[MAX_PATH * 2 + 4];
        _snwprintf_s(val, _countof(val), _TRUNCATE, L"\"%s\"", exe);
        RegSetValueExW(k, APP_RUNKEY, 0, REG_SZ, (const BYTE*)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, APP_RUNKEY);
    }
    RegCloseKey(k);
}

BOOL AppQueryAutoStart(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    wchar_t val[MAX_PATH * 2];
    DWORD cb = sizeof(val);
    DWORD type = 0;
    BOOL on = (RegQueryValueExW(k, APP_RUNKEY, NULL, &type, (BYTE*)val, &cb) == ERROR_SUCCESS
               && type == REG_SZ) ? TRUE : FALSE;
    RegCloseKey(k);
    return on;
}

/* ==================== 自定义暂停时长 ==================== */
static struct {
    BOOL done;
    BOOL ok;
    int  value;
    HWND edit;
} s_ask;

static LRESULT CALLBACK AskProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        int sc = g_scale;
        HWND st = CreateWindowExW(0, L"STATIC", L"暂停分钟数（1 – 1440）",
                                  WS_CHILD | WS_VISIBLE,
                                  MulDiv(18, sc, 100), MulDiv(16, sc, 100),
                                  MulDiv(240, sc, 100), MulDiv(20, sc, 100),
                                  h, NULL, g_hinst, NULL);
        SendMessageW(st, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        s_ask.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"30",
                                     WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_CENTER | WS_TABSTOP,
                                     MulDiv(18, sc, 100), MulDiv(42, sc, 100),
                                     MulDiv(90, sc, 100), MulDiv(26, sc, 100),
                                     h, (HMENU)10, g_hinst, NULL);
        SendMessageW(s_ask.edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        HWND ok = CreateWindowExW(0, L"BUTTON", L"确定",
                                  WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                                  MulDiv(122, sc, 100), MulDiv(42, sc, 100),
                                  MulDiv(64, sc, 100), MulDiv(26, sc, 100),
                                  h, (HMENU)IDOK, g_hinst, NULL);
        SendMessageW(ok, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        HWND cc = CreateWindowExW(0, L"BUTTON", L"取消",
                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                                  MulDiv(192, sc, 100), MulDiv(42, sc, 100),
                                  MulDiv(64, sc, 100), MulDiv(26, sc, 100),
                                  h, (HMENU)IDCANCEL, g_hinst, NULL);
        SendMessageW(cc, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);

        SetFocus(s_ask.edit);
        SendMessageW(s_ask.edit, EM_SETSEL, 0, -1);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDOK) {
            wchar_t buf[32];
            buf[0] = 0;
            GetWindowTextW(s_ask.edit, buf, _countof(buf));
            int v = (int)wcstol(buf, NULL, 10);
            if (v < 1) v = 1;
            if (v > 1440) v = 1440;
            s_ask.value = v;
            s_ask.ok = TRUE;
            s_ask.done = TRUE;
            DestroyWindow(h);
            return 0;
        }
        if (id == IDCANCEL) {
            s_ask.ok = FALSE;
            s_ask.done = TRUE;
            DestroyWindow(h);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        s_ask.ok = FALSE;
        s_ask.done = TRUE;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        s_ask.done = TRUE;
        s_ask.edit = NULL;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 返回暂停分钟数；取消返回 -1 */
static int AskMinutes(HWND owner, int defVal)
{
    static BOOL reg = FALSE;
    if (!reg) {
        WNDCLASSEXW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize        = sizeof(wc);
        wc.lpfnWndProc   = AskProc;
        wc.hInstance     = g_hinst;
        wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"FocusGuard.AskMinutes";
        RegisterClassExW(&wc);
        reg = TRUE;
    }

    s_ask.done  = FALSE;
    s_ask.ok    = FALSE;
    s_ask.value = defVal;

    int cw = MulDiv(272, g_scale, 100);
    int chh = MulDiv(84, g_scale, 100);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT rc = { 0, 0, cw, chh };
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_DLGMODALFRAME);

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.left + ((wa.right - wa.left) - (rc.right - rc.left)) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - (rc.bottom - rc.top)) / 2;

    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FocusGuard.AskMinutes",
                             L"暂停提醒", style,
                             x, y, rc.right - rc.left, rc.bottom - rc.top,
                             owner, NULL, g_hinst, NULL);
    if (!h) return -1;

    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);

    MSG msg;
    while (!s_ask.done) {
        if (!GetMessageW(&msg, NULL, 0, 0)) { PostQuitMessage(0); break; }
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    UpdateTip(TRUE);
    return s_ask.ok ? s_ask.value : -1;
}

/* ==================== 托盘菜单 ==================== */

/* 保持屏幕常亮：ES_CONTINUOUS 一次设定就持续生效，关掉时用 ES_CONTINUOUS 单发复位。
   只用这两个标志：不阻止系统睡眠写盘，只压住"显示器休眠"与"系统空闲进入睡眠"的计时器。 */
void AppApplyKeepAwake(void)
{
    if (g_cfg.keepAwake)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    else
        SetThreadExecutionState(ES_CONTINUOUS);
}

/* 托盘里一键开关某条通道：改配置 → 落盘 → 复位计时并立刻生效 → 同步设置窗与提示气泡 */
static void ToggleChannel(int id)
{
    if (id == ID_TRAY_POPUP)      g_cfg.popupEnabled = !g_cfg.popupEnabled;
    else if (id == ID_TRAY_SOUND) g_cfg.soundEnabled = !g_cfg.soundEnabled;
    else if (id == ID_TRAY_KEEP)  g_cfg.keepAwake    = !g_cfg.keepAwake;
    else return;

    CfgSave();
    if (id == ID_TRAY_KEEP) AppApplyKeepAwake();   /* 显示常亮只有这一个副作用 */
    AppOnSettingsChanged();      /* 关掉的通道：隐藏弹窗 / 停止播放；重新开启：按新节奏排下一次 */
    SettingsSyncFromConfig();    /* 设置窗正开着就同步勾选与变灰状态 */
    UpdateTip(TRUE);
}

static void ShowTrayMenu(void)
{
    POINT pt;
    GetCursorPos(&pt);

    void AppTrimWorkingSet(void);   /* 见下方定义 */

    HMENU sub = CreatePopupMenu();
    AppendMenuW(sub, MF_STRING, ID_PAUSE_10, L"暂停 10 分钟");
    AppendMenuW(sub, MF_STRING, ID_PAUSE_30, L"暂停 30 分钟");
    AppendMenuW(sub, MF_STRING, ID_PAUSE_60, L"暂停 1 小时");
    AppendMenuW(sub, MF_STRING, ID_PAUSE_CUSTOM, L"暂停自定义时长…");
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING | (g_pauseUntil ? MF_ENABLED : MF_GRAYED),
                ID_PAUSE_RESUME, L"恢复提醒");

    HMENU top = CreatePopupMenu();
    AppendMenuW(top, MF_STRING, ID_TRAY_REMIND, L"立即提醒一次");
    AppendMenuW(top, MF_STRING | (g_cfg.popupEnabled ? MF_CHECKED : 0),
                ID_TRAY_POPUP, L"弹窗提醒");
    AppendMenuW(top, MF_STRING | (g_cfg.soundEnabled ? MF_CHECKED : 0),
                ID_TRAY_SOUND, L"提示音");
    AppendMenuW(top, MF_STRING | (g_cfg.keepAwake ? MF_CHECKED : 0),
                ID_TRAY_KEEP, L"保持屏幕常亮");
    AppendMenuW(top, MF_BYPOSITION | MF_POPUP, (UINT_PTR)sub, L"暂停提醒");
    AppendMenuW(top, MF_STRING, ID_TRAY_SETTINGS, L"打开设置");
    AppendMenuW(top, MF_SEPARATOR, 0, NULL);
    AppendMenuW(top, MF_STRING, ID_TRAY_EXIT, L"退出程序");

    SetForegroundWindow(g_hwndMain);
    int cmd = (int)TrackPopupMenu(top, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                  pt.x, pt.y, 0, g_hwndMain, NULL);
    DestroyMenu(top);
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);

    /* 弹出一次菜单会把 comctl32 / 主题 / 菜单窗口一并拉进进程（本机实测 +4.3 MB、+170 句柄），
       菜单收起后立刻把页面还给系统，常驻值回到 0.2 MB 一档，下次弹菜单再按需换页。 */
    AppTrimWorkingSet();

    switch (cmd) {
    case ID_TRAY_REMIND: AppTriggerReminder(); break;
    case ID_TRAY_POPUP:
    case ID_TRAY_SOUND:
    case ID_TRAY_KEEP:
        ToggleChannel(cmd);
        break;
    case ID_PAUSE_10:    SetPause(10); break;
    case ID_PAUSE_30:    SetPause(30); break;
    case ID_PAUSE_60:    SetPause(60); break;
    case ID_PAUSE_CUSTOM: {
        int v = AskMinutes(g_hwndMain, 45);
        if (v > 0) SetPause(v);
        break;
    }
    case ID_PAUSE_RESUME:
        g_pauseUntil = 0;
        ResetInterval(time(NULL));
        UpdateTip(TRUE);
        break;
    case ID_TRAY_SETTINGS: SettingsOpen(g_hwndMain); break;
    case ID_TRAY_EXIT:     DestroyWindow(g_hwndMain); break;
    }
}

/* ==================== 每秒调度 ==================== */
static BOOL s_prevPopup = FALSE;

/* 空闲时主动收缩工作集：托盘常驻进程不需要把页面留在物理内存 */
void AppTrimWorkingSet(void)
{
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void OnTick(void)
{
    time_t now = time(NULL);

    /* 保持屏幕常亮：IE 的显示器计时器在部分机器上需要周期性重申，20 秒补一次，成本可忽略 */
    if (g_cfg.keepAwake) {
        static int s_kaTick = 0;
        if (++s_kaTick >= 20) { s_kaTick = 0; AppApplyKeepAwake(); }
    }

    /* 暂停到期 */
    if (g_pauseUntil && now >= g_pauseUntil) {
        g_pauseUntil = 0;
        g_tPopup.idleArmed = TRUE;
        g_tSound.idleArmed = TRUE;
        ResetInterval(now);
    }

    PopupTick();

    /* 弹窗刚消失：顺手把工作集收回来 */
    {
        BOOL vis = PopupVisible();
        if (s_prevPopup && !vis) AppTrimWorkingSet();
        s_prevPopup = vis;
    }

    BOOL fs      = g_cfg.pauseOnFullscreen ? IsFullscreenFg() : FALSE;
    BOOL wl      = (g_cfg.wlCount > 0) ? WhitelistHit() : FALSE;
    BOOL outside = g_cfg.scheduleEnabled ? !InSchedule() : FALSE;
    BOOL sup     = (g_pauseUntil != 0) || fs || wl || outside;
    BOOL was     = g_suppressed;
    g_suppressed = sup;

    if (sup) {
        if (!was) {                                   /* 进入静默：不补偿触发 */
            g_tPopup.idleArmed = TRUE;
            g_tSound.idleArmed = TRUE;
        }
    } else {
        if (was) {                                    /* 退出静默：两条通道重新计时 */
            ResetInterval(now);
            g_tPopup.idleArmed = TRUE;
            g_tSound.idleArmed = TRUE;
        }

        /* 固定间隔：两条通道各自到期各自触发，互不影响 */
        if (g_tPopup.nextAt && now >= g_tPopup.nextAt) FirePopup(now);
        if (g_tSound.nextAt && now >= g_tSound.nextAt) FireSound(now);

        /* 空闲检测：两条通道各自有阈值与已触发标记 */
        if (g_cfg.popupIdleOn || g_cfg.soundIdleOn) {
            int idle = (int)IdleSeconds();

            if (g_cfg.popupIdleOn && g_cfg.popupEnabled) {
                if (idle >= g_cfg.popupIdleMin * 60) {
                    if (!g_tPopup.idleArmed) { g_tPopup.idleArmed = TRUE; FirePopup(now); }
                } else {
                    g_tPopup.idleArmed = FALSE;
                }
            }

            if (g_cfg.soundIdleOn && g_cfg.soundEnabled) {
                if (idle >= g_cfg.soundIdleMin * 60) {
                    if (!g_tSound.idleArmed) { g_tSound.idleArmed = TRUE; FireSound(now); }
                } else {
                    g_tSound.idleArmed = FALSE;
                }
            }
        }
    }

    BuildState(now, fs, wl, outside);
    UpdateTip(FALSE);
    SettingsRefreshStatus();
}

/* ==================== 主窗口过程 ==================== */
static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY_MSG:
        switch (LOWORD(lp)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowTrayMenu();
            break;
        case WM_LBUTTONDBLCLK:
            SettingsOpen(h);
            break;
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_TRAY_POPUP || LOWORD(wp) == ID_TRAY_SOUND ||
            LOWORD(wp) == ID_TRAY_KEEP) {
            ToggleChannel((int)LOWORD(wp));
            return 0;
        }
        return 0;

    case WM_APP_OPEN_SETTINGS:
        SettingsOpen(h);
        return 0;

    case WM_APP_REMIND:
        AppTriggerReminder();
        return 0;

    case WM_TIMER:
        if (wp == TIMER_MAIN) { OnTick(); return 0; }
        if (wp == TIMER_AUDIO) { AudioTick(); return 0; }
        if (wp == TIMER_EDGE) { EdgeTick(); return 0; }
        if (wp == 3) { KillTimer(h, 3); AppTrimWorkingSet(); return 0; }
        return 0;

    case WM_QUERYENDSESSION:
    case WM_ENDSESSION:
        CfgSave();
        return TRUE;

    case WM_DESTROY:
        KillTimer(h, TIMER_MAIN);
        EdgeStop();                       /* 光带窗口/DIB 显式释放，退出无残留 */
        SetThreadExecutionState(ES_CONTINUOUS);   /* 退出即恢复系统正常息屏/睡眠 */
        TrayRemove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ==================== 入口 ==================== */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow)
{
    (void)hPrev; (void)nShow;
    g_hinst = hInst;

    /* 单实例 */
    HANDLE mu = CreateMutexW(NULL, TRUE, APP_MUTEX);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND old = FindWindowW(APP_CLASS, NULL);
        if (old) {
            if (wcsstr(lpCmdLine, L"--remind"))
                PostMessageW(old, WM_APP_REMIND, 0, 0);
            else
                PostMessageW(old, WM_APP_OPEN_SETTINGS, 0, 0);
        }
        if (mu) CloseHandle(mu);
        return 0;
    }

    /* DPI 感知（清单未生效时兜底） */
    SetProcessDPIAware();
    {
        /* 清单生效（PerMonitorV2）时，GetDeviceCaps(屏幕DC) 可能给回 96，
           先用 GetDpiForSystem 拿系统 DPI，再回退到老接口 */
        UINT d = 0;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) {
            typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
            PFN_GetDpiForSystem f = (PFN_GetDpiForSystem)GetProcAddress(u, "GetDpiForSystem");
            if (f) d = f();
        }
        if ((int)d <= 0) {
            HDC dc = GetDC(NULL);
            d = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(NULL, dc);
        }
        g_dpi = (int)d;
        if (g_dpi <= 0) g_dpi = 96;
        g_scale = MulDiv(100, g_dpi, 96);
    }

    /* 通用控件（trackbar / updown）推迟到设置窗打开时再初始化，
       避免常驻期就把 comctl32 及其依赖拉进进程 */

    CfgInit();
    CfgSoundDir();                    /* 顺手把音源文件夹建好 */
    g_cfg.autoStart = AppQueryAutoStart();
    AppApplyKeepAwake();              /* 上次退出时是开着就继续开着 */

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = APP_CLASS;
    if (!RegisterClassExW(&wc)) return 0;

    g_hwndMain = CreateWindowExW(0, APP_CLASS, APP_NAME, WS_POPUP,
                                 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwndMain) return 0;

    PopupInit();
    AudioInit();
    SettingsInit();
    TrayAdd();

    time_t now = time(NULL);
    ResetInterval(now);

    /* 启动时若已长时间空闲，先标记为"已提醒"，避免一开机就弹 */
    {
        int idle = (int)IdleSeconds();
        g_tPopup.idleArmed = (g_cfg.popupIdleOn && g_cfg.popupEnabled &&
                              idle >= g_cfg.popupIdleMin * 60) ? TRUE : FALSE;
        g_tSound.idleArmed = (g_cfg.soundIdleOn && g_cfg.soundEnabled &&
                              idle >= g_cfg.soundIdleMin * 60) ? TRUE : FALSE;
    }

    OnTick();
    SetTimer(g_hwndMain, TIMER_MAIN, 1000, NULL);
    SetTimer(g_hwndMain, 3, 3000, NULL);   /* 启动 3 秒后收缩一次工作集 */

    /* 调试/验证用参数 */
    if (lpCmdLine && lpCmdLine[0]) {
        if (wcsstr(lpCmdLine, L"--settings")) SettingsOpen(g_hwndMain);
        else if (wcsstr(lpCmdLine, L"--remind")) AppTriggerReminder();
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (SettingsIsOpen() && IsDialogMessageW(GetForegroundWindow(), &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SettingsDestroy();
    AudioStop();
    CfgSave();
    TrayRemove();
    if (mu) CloseHandle(mu);
    return 0;
}
