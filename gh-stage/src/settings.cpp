#include "app.h"
#include <commctrl.h>
#include <string.h>

/* ============================================================
   设置窗口：单页、无多级菜单、修改即生效并自动保存
   ============================================================ */

enum {
    IDC_CHK_SCHED = 1007,
    IDC_CHK_D0 = 1008, IDC_CHK_D1, IDC_CHK_D2, IDC_CHK_D3, IDC_CHK_D4, IDC_CHK_D5, IDC_CHK_D6,
    IDC_ED_SH, IDC_UD_SH, IDC_ED_SM, IDC_UD_SM, IDC_ED_EH, IDC_UD_EH, IDC_ED_EM, IDC_UD_EM,
    IDC_CHK_POPUP, IDC_CHK_TOPMOST,
    IDC_CB_POS, IDC_ED_CX, IDC_ED_CY,
    IDC_ED_W, IDC_ED_H,
    IDC_TB_OPACITY, IDC_LB_OPACITY,
    IDC_BTN_BG, IDC_BTN_FG, IDC_ED_FONT,
    IDC_ED_DUR, IDC_CHK_AUTOCLOSE, IDC_ED_TEXT,
    IDC_CHK_SOUND, IDC_CB_SRC, IDC_BTN_BROWSE, IDC_LB_FILE,
    IDC_TB_VOL, IDC_LB_VOL, IDC_CHK_FADE, IDC_CB_REPEAT,
    IDC_CHK_FSFULL, IDC_BTN_WADD, IDC_BTN_WDEL, IDC_LB_WHITE, IDC_LB_STATUS,
    IDC_CHK_AUTOSTART, IDC_LB_CFGPATH,
    IDC_BTN_PREVIEW, IDC_BTN_TESTSOUND, IDC_BTN_DEFAULT, IDC_BTN_CLOSE,
    /* 弹窗通道节奏 */
    IDC_CHK_P_IV, IDC_ED_P_IV, IDC_UD_P_IV,
    IDC_CHK_P_IDLE, IDC_ED_P_IDLE, IDC_UD_P_IDLE,
    /* 提示音通道节奏 */
    IDC_CHK_S_IV, IDC_ED_S_IV, IDC_UD_S_IV,
    IDC_CHK_S_IDLE, IDC_ED_S_IDLE, IDC_UD_S_IDLE,
    /* 音源文件夹 */
    IDC_BTN_SNDDIR,
    /* 弹窗屏幕选择 */
    IDC_CB_PMON,
    /* 边缘呼吸灯 */
    IDC_CHK_EDGE, IDC_CB_EPRESET, IDC_CB_EMON, IDC_CB_EMODE, IDC_ED_ESECS,
    IDC_CHK_ET, IDC_CHK_EB, IDC_CHK_EL, IDC_CHK_ER,
    IDC_ED_ETHICK, IDC_BTN_ECOLOR, IDC_CHK_EBY_POPUP, IDC_CHK_EBY_SOUND,
    IDC_BTN_EPREVIEW,
    /* 保持屏幕常亮 */
    IDC_CHK_KEEP
};

struct Ui {
    HWND chkPiv, edPiv, udPiv, chkPidle, edPidle, udPidle;
    HWND chkSiv, edSiv, udSiv, chkSidle, edSidle, udSidle;
    HWND chkSched;
    HWND chkDay[7];
    HWND edSh, udSh, edSm, udSm, edEh, udEh, edEm, udEm;
    HWND chkPopup, chkTopmost;
    HWND cbPos, edCx, edCy, edW, edH;
    HWND tbOpacity, lbOpacity;
    HWND btnBg, btnFg, edFont;
    HWND edDur, chkAutoClose, edText;
    HWND chkSound, cbSrc, btnBrowse, lbFile;
    HWND btnSndDir;
    HWND tbVol, lbVol, chkFade, cbRepeat;
    HWND chkFsFull, btnWAdd, btnWDel, lbWhite, lbStatus;
    HWND chkAutoStart, lbCfgPath, chkKeep;
    /* 屏幕选择 + 边缘呼吸灯 */
    HWND cbPMon;
    HWND chkEdge, cbEPreset, cbEMon, cbEMode, edESecs;
    HWND chkET, chkEB, chkEL, chkER;
    HWND edEThick, btnEColor, chkEByPopup, chkEBySound, btnEPreview;
};

static Ui   ui;
static HWND s_hwnd = NULL;
static HFONT s_font = NULL, s_fontSmall = NULL, s_fontBold = NULL;
static HBRUSH s_bk = NULL;
static BOOL s_filling = FALSE;
static BOOL s_dirty = FALSE;
static UINT_PTR s_dirtyTimer = 0;

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void UpdateEnableStates(void);

static int S(int v) { return MulDiv(v, g_scale, 100); }

/* 取窗口所在显示器的 DPI（Win10 1607+，老系统回退系统 DPI） */
static UINT WinDpi(HWND h)
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

/* 主显示器 != 窗口所在显示器时，按窗口 DPI 重算缩放并重建字体 */
static void SyncScaleForWindow(HWND h)
{
    UINT d = WinDpi(h);
    if ((int)d == g_dpi) return;
    g_dpi   = (int)d;
    g_scale = MulDiv(100, g_dpi, 96);
    if (s_font)      { DeleteObject(s_font);      s_font = NULL; }
    if (s_fontSmall) { DeleteObject(s_fontSmall); s_fontSmall = NULL; }
    if (s_fontBold)  { DeleteObject(s_fontBold);  s_fontBold = NULL; }
    SettingsInit();
}

/* ---------------- 字体 / 画笔 ---------------- */
static HFONT MakeFont(int pt, int weight, const wchar_t* face)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));
    lf.lfHeight  = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight  = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

void SettingsInit(void)
{
    static BOOL reg = FALSE;
    if (!s_bk)   s_bk = CreateSolidBrush(RGB(255, 255, 255));
    if (!s_font) {
        s_font      = MakeFont(9,  FW_NORMAL,   L"Microsoft YaHei UI");
        s_fontSmall = MakeFont(8,  FW_NORMAL,   L"Microsoft YaHei UI");
        s_fontBold  = MakeFont(10, FW_SEMIBOLD, L"Microsoft YaHei UI");
    }
    if (reg) return;

    /* trackbar / updown 等公共控件只在这里初始化，常驻期不加载 comctl32 */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_BAR_CLASSES | ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SettingsProc;
    wc.hInstance     = g_hinst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = s_bk;
    wc.lpszClassName = APP_SET_CLASS;
    wc.hIcon         = LoadIconW(g_hinst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm       = wc.hIcon;
    RegisterClassExW(&wc);
    reg = TRUE;
}

/* ---------------- 控件创建 ---------------- */
static HWND Ctl(HWND parent, const wchar_t* cls, const wchar_t* text,
                int x, int y, int w, int h, DWORD style, int id, HFONT font)
{
    HWND hh = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                              S(x), S(y), S(w), S(h), parent,
                              (HMENU)(INT_PTR)id, g_hinst, NULL);
    if (hh && font) SendMessageW(hh, WM_SETFONT, (WPARAM)font, TRUE);
    return hh;
}

static HWND MkLabel(HWND p, const wchar_t* t, int x, int y, int w, int h, int id, HFONT f)
{
    return Ctl(p, L"STATIC", t, x, y, w, h, SS_LEFT | SS_NOPREFIX, id, f ? f : s_font);
}

static HWND MkCheck(HWND p, const wchar_t* t, int x, int y, int w, int id)
{
    return Ctl(p, L"BUTTON", t, x, y, w, 22, BS_AUTOCHECKBOX | WS_TABSTOP, id, s_font);
}

static HWND MkNum(HWND p, int x, int y, int w, int id, int udid, int lo, int hi, int val)
{
    HWND ed = Ctl(p, L"EDIT", L"", x, y, w, 24,
                  WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER | WS_TABSTOP, id, s_font);
    HWND ud = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                              WS_CHILD | WS_VISIBLE | UDS_AUTOBUDDY | UDS_ALIGNRIGHT |
                              UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS,
                              S(x + w - 15), S(y), S(16), S(24), p,
                              (HMENU)(INT_PTR)udid, g_hinst, NULL);
    SendMessageW(ud, UDM_SETRANGE32, (WPARAM)lo, (LPARAM)hi);
    wchar_t buf[16];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", val);
    SetWindowTextW(ed, buf);
    SendMessageW(ud, UDM_SETPOS32, 0, (LPARAM)val);
    return ed;
}

static HWND MkCombo(HWND p, int x, int y, int w, int id)
{
    return Ctl(p, L"COMBOBOX", L"", x, y, w, 200,
               WS_BORDER | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, s_font);
}

static HWND MkBtn(HWND p, const wchar_t* t, int x, int y, int w, int h, int id, DWORD style)
{
    return Ctl(p, L"BUTTON", t, x, y, w, h, BS_PUSHBUTTON | WS_TABSTOP | style, id, s_font);
}

/* ---------------- 音源下拉栏（内置 2 个 + 音源文件夹里的文件） ----------------
   条目种类：0 = 内置音，1 = 音源文件夹里的文件，2 = 「浏览其他文件…」 */
enum { SND_BUILTIN = 0, SND_FILE = 1, SND_BROWSE = 2 };

struct SndItem {
    int     kind;
    wchar_t path[MAX_PATH * 2];
    wchar_t name[MAX_PATH];        /* 显示用文件名 */
};

static SndItem s_snd[SND_MAX + 4];
static int     s_sndCount = 0;

static BOOL IsAudioFile(const wchar_t* name)
{
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return FALSE;
    return (_wcsicmp(dot, L".wav") == 0 || _wcsicmp(dot, L".mp3") == 0);
}

/* 扫一遍音源文件夹，结果按名称排序 */
static void EnumSounds(void)
{
    s_sndCount = 0;

    SndItem tmp[SND_MAX];
    int n = 0;
    const wchar_t* dir = CfgSoundDir();
    wchar_t pat[MAX_PATH * 2] = L"";
    _snwprintf_s(pat, _countof(pat), _TRUNCATE, L"%s\\*.*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!IsAudioFile(fd.cFileName)) continue;
            if (n >= SND_MAX) break;
            tmp[n].kind = SND_FILE;
            _snwprintf_s(tmp[n].path, MAX_PATH * 2, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            lstrcpynW(tmp[n].name, fd.cFileName, MAX_PATH);
            n++;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }

    /* 按文件名升序（插入排序，最多 48 条） */
    for (int i = 1; i < n; i++) {
        SndItem t = tmp[i];
        int j = i - 1;
        while (j >= 0 && _wcsicmp(tmp[j].name, t.name) > 0) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = t;
    }

    s_snd[0].kind = SND_BUILTIN;
    lstrcpynW(s_snd[0].name, L"柔铃（内置①）", MAX_PATH);
    s_snd[0].path[0] = 0;
    s_snd[1].kind = SND_BUILTIN;
    lstrcpynW(s_snd[1].name, L"深呼吸（内置②）", MAX_PATH);
    s_snd[1].path[0] = 0;
    s_sndCount = 2;

    for (int i = 0; i < n; i++) s_snd[s_sndCount++] = tmp[i];

    s_snd[s_sndCount].kind = SND_BROWSE;
    lstrcpynW(s_snd[s_sndCount].name, L"浏览其他文件…", MAX_PATH);
    s_snd[s_sndCount].path[0] = 0;
    s_sndCount++;
}

/* 重建下拉栏内容，并尽量选中配置里那一项 */
static void BuildSrcCombo(void)
{
    EnumSounds();

    SendMessageW(ui.cbSrc, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < s_sndCount; i++)
        SendMessageW(ui.cbSrc, CB_ADDSTRING, 0, (LPARAM)s_snd[i].name);

    int sel = 0;
    if (g_cfg.soundSource == 2 && g_cfg.soundFile[0]) {
        /* 先在文件夹里找，找不到就把这个文件夹外的文件补一条进去，保证能显示出来 */
        int found = -1;
        for (int i = 2; i < s_sndCount; i++) {
            if (s_snd[i].kind == SND_FILE && _wcsicmp(s_snd[i].path, g_cfg.soundFile) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            SndItem ext;
            ext.kind = SND_FILE;
            lstrcpynW(ext.path, g_cfg.soundFile, MAX_PATH * 2);
            const wchar_t* nm = wcsrchr(g_cfg.soundFile, L'\\');
            nm = nm ? nm + 1 : g_cfg.soundFile;
            _snwprintf_s(ext.name, MAX_PATH, _TRUNCATE, L"（文件夹外）%s", nm);
            for (int i = s_sndCount; i > 2; i--) s_snd[i] = s_snd[i - 1];
            s_snd[2] = ext;
            s_sndCount++;
            found = 2;
        }
        sel = found;
    } else {
        sel = (g_cfg.soundSource == 1) ? 1 : 0;
    }
    SendMessageW(ui.cbSrc, CB_SETCURSEL, sel, 0);
}

static HWND MkTrack(HWND p, int x, int y, int w, int id, int lo, int hi, int val)
{
    HWND hh = Ctl(p, TRACKBAR_CLASSW, L"", x, y, w, 26,
                  WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, id, s_font);
    SendMessageW(hh, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(hh, TBM_SETPOS, TRUE, val);
    return hh;
}

/* ---------------- 值读写 ---------------- */
static BOOL Chk(HWND h) { return (BOOL)SendMessageW(h, BM_GETCHECK, 0, 0); }
static void SetChk(HWND h, BOOL on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

static int ReadInt(HWND h, int def)
{
    wchar_t buf[32];
    buf[0] = 0;
    GetWindowTextW(h, buf, _countof(buf));
    if (!buf[0]) return def;
    return (int)wcstol(buf, NULL, 10);
}

static void SetNum(HWND ed, HWND ud, int v)
{
    if (!ed) return;
    wchar_t buf[16];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d", v);
    SetWindowTextW(ed, buf);
    if (ud) SendMessageW(ud, UDM_SETPOS32, 0, (LPARAM)v);
}

static int ClampI(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* 星期：界面顺序 一..日（ID D0..D6），配置下标 0=周日 */
static int ChkIdxToDay(int i) { return (i + 1) % 7; }

/* ---------------- 界面 → 配置 ---------------- */
static void ReadAll(void)
{
    AppSettings* c = &g_cfg;

    c->popupIntervalOn  = Chk(ui.chkPiv);
    c->popupIntervalMin = ClampI(ReadInt(ui.edPiv, c->popupIntervalMin), 1, 120);
    c->popupIdleOn      = Chk(ui.chkPidle);
    c->popupIdleMin     = ClampI(ReadInt(ui.edPidle, c->popupIdleMin), 1, 60);
    c->soundIntervalOn  = Chk(ui.chkSiv);
    c->soundIntervalMin = ClampI(ReadInt(ui.edSiv, c->soundIntervalMin), 1, 120);
    c->soundIdleOn      = Chk(ui.chkSidle);
    c->soundIdleMin     = ClampI(ReadInt(ui.edSidle, c->soundIdleMin), 1, 60);

    c->scheduleEnabled = Chk(ui.chkSched);
    for (int i = 0; i < 7; i++) c->days[ChkIdxToDay(i)] = Chk(ui.chkDay[i]);
    c->startHour   = ClampI(ReadInt(ui.edSh, c->startHour), 0, 23);
    c->startMinute = ClampI(ReadInt(ui.edSm, c->startMinute), 0, 59);
    c->endHour     = ClampI(ReadInt(ui.edEh, c->endHour), 0, 23);
    c->endMinute   = ClampI(ReadInt(ui.edEm, c->endMinute), 0, 59);

    c->popupEnabled = Chk(ui.chkPopup);
    c->topmost      = Chk(ui.chkTopmost);
    c->popupPos     = ClampI((int)SendMessageW(ui.cbPos, CB_GETCURSEL, 0, 0), 0, 5);
    c->customX      = ClampI(ReadInt(ui.edCx, c->customX), -2000, 4000);
    c->customY      = ClampI(ReadInt(ui.edCy, c->customY), -2000, 4000);
    c->popupWidth   = ClampI(ReadInt(ui.edW, c->popupWidth), 160, 1200);
    c->popupHeight  = ClampI(ReadInt(ui.edH, c->popupHeight), 60, 600);
    c->opacity      = ClampI((int)SendMessageW(ui.tbOpacity, TBM_GETPOS, 0, 0), 10, 100);
    c->fontSize     = ClampI(ReadInt(ui.edFont, c->fontSize), 12, 48);
    c->duration     = ClampI(ReadInt(ui.edDur, c->duration), 1, 10);
    c->autoClose    = Chk(ui.chkAutoClose);
    {
        wchar_t t[TXT_MAX];
        t[0] = 0;
        GetWindowTextW(ui.edText, t, TXT_MAX);
        if (t[0]) lstrcpynW(c->text, t, TXT_MAX);
    }

    c->soundEnabled = Chk(ui.chkSound);
    {
        int sel = (int)SendMessageW(ui.cbSrc, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < s_sndCount) {
            if (s_snd[sel].kind == SND_BUILTIN) {
                c->soundSource = sel;                 /* 0 柔铃 / 1 深呼吸 */
            } else if (s_snd[sel].kind == SND_FILE) {
                c->soundSource = 2;
                lstrcpynW(c->soundFile, s_snd[sel].path, MAX_PATH);
            }
            /* SND_BROWSE 只是入口项，不改配置 */
        }
    }
    c->volume       = ClampI((int)SendMessageW(ui.tbVol, TBM_GETPOS, 0, 0), 0, 100);
    c->fadeIn       = Chk(ui.chkFade);
    c->repeatCount  = ((int)SendMessageW(ui.cbRepeat, CB_GETCURSEL, 0, 0) == 1) ? 2 : 1;

    c->pauseOnFullscreen = Chk(ui.chkFsFull);
    c->autoStart         = Chk(ui.chkAutoStart);
    c->keepAwake         = Chk(ui.chkKeep);

    /* 屏幕选择 + 边缘呼吸灯 */
    c->popupMonitor = ClampI((int)SendMessageW(ui.cbPMon, CB_GETCURSEL, 0, 0), 0, 2);

    c->edgeEnabled = Chk(ui.chkEdge);
    c->edgePreset  = ClampI((int)SendMessageW(ui.cbEPreset, CB_GETCURSEL, 0, 0), 0, 5);
    c->edgeMonitor = ClampI((int)SendMessageW(ui.cbEMon, CB_GETCURSEL, 0, 0), 0, 1);
    c->edgeMode    = ClampI((int)SendMessageW(ui.cbEMode, CB_GETCURSEL, 0, 0), 0, 2);
    c->edgeSecs    = ClampI(ReadInt(ui.edESecs, c->edgeSecs), 1, 10);
    {
        int sides = 0;
        if (Chk(ui.chkET)) sides |= 1;
        if (Chk(ui.chkEB)) sides |= 2;
        if (Chk(ui.chkEL)) sides |= 4;
        if (Chk(ui.chkER)) sides |= 8;
        c->edgeSides = sides ? sides : 15;    /* 一条都不勾 = 没得画，回落到四边 */
    }
    c->edgeThick   = ClampI(ReadInt(ui.edEThick, c->edgeThick), 6, 80);
    c->edgeByPopup = Chk(ui.chkEByPopup);
    c->edgeBySound = Chk(ui.chkEBySound);
    if (!c->edgeByPopup && !c->edgeBySound) c->edgeByPopup = TRUE;
}

/* 音源提示行：当前选的是什么 / 文件夹为空时给个指引 */
static void UpdateFileLabel(void)
{
    const AppSettings* c = &g_cfg;
    wchar_t buf[256];

    if (c->soundSource == 2 && c->soundFile[0]) {
        const wchar_t* nm = wcsrchr(c->soundFile, L'\\');
        nm = nm ? nm + 1 : c->soundFile;
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"文件：%s", nm);
    } else if (c->soundSource == 1) {
        lstrcpynW(buf, L"内置提示音② · 深呼吸", _countof(buf));
    } else {
        lstrcpynW(buf, L"内置提示音① · 柔铃", _countof(buf));
    }

    int nFile = 0;
    for (int i = 0; i < s_sndCount; i++) if (s_snd[i].kind == SND_FILE) nFile++;
    if (nFile == 0 && !(c->soundSource == 2 && c->soundFile[0]))
        lstrcpynW(buf, L"音源文件夹为空：把 mp3 / wav 放进去即可在此选择", _countof(buf));

    SetWindowTextW(ui.lbFile, buf);
}

/* ---------------- 配置 → 界面 ---------------- */
static void FillUi(void)
{
    const AppSettings* c = &g_cfg;
    s_filling = TRUE;

    SetChk(ui.chkPiv, c->popupIntervalOn);
    SetNum(ui.edPiv, ui.udPiv, c->popupIntervalMin);
    SetChk(ui.chkPidle, c->popupIdleOn);
    SetNum(ui.edPidle, ui.udPidle, c->popupIdleMin);
    SetChk(ui.chkSiv, c->soundIntervalOn);
    SetNum(ui.edSiv, ui.udSiv, c->soundIntervalMin);
    SetChk(ui.chkSidle, c->soundIdleOn);
    SetNum(ui.edSidle, ui.udSidle, c->soundIdleMin);

    SetChk(ui.chkSched, c->scheduleEnabled);
    for (int i = 0; i < 7; i++) SetChk(ui.chkDay[i], c->days[ChkIdxToDay(i)]);
    SetNum(ui.edSh, ui.udSh, c->startHour);
    SetNum(ui.edSm, ui.udSm, c->startMinute);
    SetNum(ui.edEh, ui.udEh, c->endHour);
    SetNum(ui.edEm, ui.udEm, c->endMinute);

    SetChk(ui.chkPopup, c->popupEnabled);
    SetChk(ui.chkTopmost, c->topmost);
    SendMessageW(ui.cbPos, CB_SETCURSEL, c->popupPos, 0);
    SetNum(ui.edCx, NULL, c->customX);
    SetNum(ui.edCy, NULL, c->customY);
    SetNum(ui.edW, NULL, c->popupWidth);
    SetNum(ui.edH, NULL, c->popupHeight);
    SendMessageW(ui.tbOpacity, TBM_SETPOS, TRUE, c->opacity);
    SetNum(ui.edFont, NULL, c->fontSize);
    SetNum(ui.edDur, NULL, c->duration);
    SetChk(ui.chkAutoClose, c->autoClose);
    SetWindowTextW(ui.edText, c->text);

    SetChk(ui.chkSound, c->soundEnabled);
    BuildSrcCombo();
    SendMessageW(ui.tbVol, TBM_SETPOS, TRUE, c->volume);
    SetChk(ui.chkFade, c->fadeIn);
    SendMessageW(ui.cbRepeat, CB_SETCURSEL, c->repeatCount > 1 ? 1 : 0, 0);

    SetChk(ui.chkFsFull, c->pauseOnFullscreen);
    SetChk(ui.chkAutoStart, c->autoStart);
    SetChk(ui.chkKeep, c->keepAwake);

    /* 屏幕选择 */
    SendMessageW(ui.cbPMon, CB_SETCURSEL, c->popupMonitor, 0);

    /* 边缘呼吸灯 */
    SetChk(ui.chkEdge, c->edgeEnabled);
    SendMessageW(ui.cbEPreset, CB_SETCURSEL, c->edgePreset, 0);
    SendMessageW(ui.cbEMon, CB_SETCURSEL, c->edgeMonitor, 0);
    SendMessageW(ui.cbEMode, CB_SETCURSEL, c->edgeMode, 0);
    SetNum(ui.edESecs, NULL, c->edgeSecs);
    SetChk(ui.chkET, (c->edgeSides & 1) ? TRUE : FALSE);
    SetChk(ui.chkEB, (c->edgeSides & 2) ? TRUE : FALSE);
    SetChk(ui.chkEL, (c->edgeSides & 4) ? TRUE : FALSE);
    SetChk(ui.chkER, (c->edgeSides & 8) ? TRUE : FALSE);
    SetNum(ui.edEThick, NULL, c->edgeThick);
    SetChk(ui.chkEByPopup, c->edgeByPopup);
    SetChk(ui.chkEBySound, c->edgeBySound);
    if (ui.btnEColor) InvalidateRect(ui.btnEColor, NULL, TRUE);

    s_filling = FALSE;

    wchar_t buf[256];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d%%", c->opacity);
    SetWindowTextW(ui.lbOpacity, buf);
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d%%", c->volume);
    SetWindowTextW(ui.lbVol, buf);

    if (c->soundSource == 2 && c->soundFile[0]) {
        const wchar_t* nm = wcsrchr(c->soundFile, L'\\');
        nm = nm ? nm + 1 : c->soundFile;
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"文件：%s", nm);
    } else if (c->soundSource == 1) {
        lstrcpynW(buf, L"内置提示音② · 深呼吸", _countof(buf));
    } else {
        lstrcpynW(buf, L"内置提示音① · 柔铃", _countof(buf));
    }
    SetWindowTextW(ui.lbFile, buf);

    UpdateFileLabel();

    SetWindowTextW(ui.lbCfgPath, CfgPath());

    SendMessageW(ui.lbWhite, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_cfg.wlCount; i++)
        SendMessageW(ui.lbWhite, LB_ADDSTRING, 0, (LPARAM)g_cfg.wl[i]);

    UpdateEnableStates();
    SettingsRefreshStatus();
}

/* ---------------- 联动状态 ---------------- */
static void UpdateEnableStates(void)
{
    BOOL custom = (SendMessageW(ui.cbPos, CB_GETCURSEL, 0, 0) == 5);
    EnableWindow(ui.edCx, custom);
    EnableWindow(ui.edCy, custom);

    BOOL popupOn = Chk(ui.chkPopup);
    EnableWindow(ui.cbPos, popupOn);
    EnableWindow(ui.tbOpacity, popupOn);
    EnableWindow(ui.edW, popupOn);
    EnableWindow(ui.edH, popupOn);
    EnableWindow(ui.edFont, popupOn);
    EnableWindow(ui.btnBg, popupOn);
    EnableWindow(ui.btnFg, popupOn);
    EnableWindow(ui.edText, popupOn);
    EnableWindow(ui.chkTopmost, popupOn);
    EnableWindow(ui.chkAutoClose, popupOn);

    BOOL autoClose = popupOn && Chk(ui.chkAutoClose);
    EnableWindow(ui.edDur, autoClose);

    BOOL soundOn = Chk(ui.chkSound);
    EnableWindow(ui.cbSrc, soundOn);
    EnableWindow(ui.tbVol, soundOn);
    EnableWindow(ui.chkFade, soundOn);
    EnableWindow(ui.cbRepeat, soundOn);
    EnableWindow(ui.btnBrowse, soundOn);

    BOOL schedOn = Chk(ui.chkSched);
    for (int i = 0; i < 7; i++) EnableWindow(ui.chkDay[i], schedOn);
    EnableWindow(ui.edSh, schedOn);
    EnableWindow(ui.edSm, schedOn);
    EnableWindow(ui.edEh, schedOn);
    EnableWindow(ui.edEm, schedOn);

    /* 通道级联动：通道总开关关掉时，它自己的节奏也置灰 */
    EnableWindow(ui.chkPiv,   popupOn);
    EnableWindow(ui.chkPidle, popupOn);
    EnableWindow(ui.edPiv,    popupOn && Chk(ui.chkPiv));
    EnableWindow(ui.edPidle,  popupOn && Chk(ui.chkPidle));

    EnableWindow(ui.chkSiv,   soundOn);
    EnableWindow(ui.chkSidle, soundOn);
    EnableWindow(ui.edSiv,    soundOn && Chk(ui.chkSiv));
    EnableWindow(ui.edSidle,  soundOn && Chk(ui.chkSidle));

    /* 弹窗屏幕选择（弹窗关掉就没意义） */
    EnableWindow(ui.cbPMon, popupOn);

    /* 边缘呼吸灯：总开关关掉时整组置灰；秒数只在「固定秒数」模式可编辑 */
    BOOL edgeOn = Chk(ui.chkEdge);
    EnableWindow(ui.cbEPreset,   edgeOn);
    EnableWindow(ui.cbEMon,      edgeOn);
    EnableWindow(ui.cbEMode,     edgeOn);
    EnableWindow(ui.edESecs,     edgeOn && SendMessageW(ui.cbEMode, CB_GETCURSEL, 0, 0) == 1);
    EnableWindow(ui.chkET,       edgeOn);
    EnableWindow(ui.chkEB,       edgeOn);
    EnableWindow(ui.chkEL,       edgeOn);
    EnableWindow(ui.chkER,       edgeOn);
    EnableWindow(ui.edEThick,    edgeOn);
    EnableWindow(ui.btnEColor,   edgeOn);
    EnableWindow(ui.chkEByPopup, edgeOn);
    EnableWindow(ui.chkEBySound, edgeOn);
    EnableWindow(ui.btnEPreview, edgeOn);
}

/* ---------------- 提交 ---------------- */
static void CommitNow(void)
{
    if (s_filling || !s_hwnd) return;
    ReadAll();
    CfgSave();
    AppOnSettingsChanged();

    wchar_t buf[32];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d%%", g_cfg.opacity);
    SetWindowTextW(ui.lbOpacity, buf);
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d%%", g_cfg.volume);
    SetWindowTextW(ui.lbVol, buf);
    SettingsRefreshStatus();
}

static void TouchDirty(void)
{
    if (s_filling || !s_hwnd) return;
    s_dirty = TRUE;
    if (s_dirtyTimer) KillTimer(s_hwnd, 3);
    s_dirtyTimer = SetTimer(s_hwnd, 3, 400, NULL);
}

/* 手改了「边 / 宽度 / 光色」后，预设栏落到「自定义」 */
static void MarkEdgeCustom(void)
{
    if (s_filling) return;
    s_filling = TRUE;
    SendMessageW(ui.cbEPreset, CB_SETCURSEL, 5, 0);
    s_filling = FALSE;
    g_cfg.edgePreset = 5;
}

/* 选预设 = 一次性把 边 / 宽度 / 光色 三样填好 */
static void ApplyEdgePreset(void)
{
    int idx = (int)SendMessageW(ui.cbEPreset, CB_GETCURSEL, 0, 0);
    int sides = 0, thick = 0;
    COLORREF col = g_cfg.edgeColor;

    if (EdgePresetValues(idx, &sides, &thick, &col)) {
        g_cfg.edgeSides = sides;
        g_cfg.edgeThick = thick;
        g_cfg.edgeColor = col;
        s_filling = TRUE;
        SetChk(ui.chkET, (sides & 1) ? TRUE : FALSE);
        SetChk(ui.chkEB, (sides & 2) ? TRUE : FALSE);
        SetChk(ui.chkEL, (sides & 4) ? TRUE : FALSE);
        SetChk(ui.chkER, (sides & 8) ? TRUE : FALSE);
        SetNum(ui.edEThick, NULL, thick);
        s_filling = FALSE;
        InvalidateRect(ui.btnEColor, NULL, TRUE);
    }
    g_cfg.edgePreset = idx;
    CommitNow();
    UpdateEnableStates();
}

void SettingsRefreshStatus(void)
{
    if (!ui.lbStatus || !IsWindow(ui.lbStatus)) return;
    wchar_t st[160];
    AppStateText(st, _countof(st));
    wchar_t cur[160];
    cur[0] = 0;
    GetWindowTextW(ui.lbStatus, cur, _countof(cur));
    if (lstrcmpW(cur, st) != 0) SetWindowTextW(ui.lbStatus, st);
}

/* 托盘菜单里一键开关通道后，把设置窗的勾选与变灰状态同步过来 */
void SettingsSyncFromConfig(void)
{
    if (!s_hwnd || !IsWindow(s_hwnd)) return;
    FillUi();
    InvalidateRect(s_hwnd, NULL, TRUE);
}

/* ---------------- 颜色 / 文件 ---------------- */
static BOOL PickColor(HWND owner, COLORREF* c)
{
    static COLORREF custom[16];
    CHOOSECOLORW cc;
    ZeroMemory(&cc, sizeof(cc));
    cc.lStructSize  = sizeof(cc);
    cc.hwndOwner    = owner;
    cc.rgbResult    = *c;
    cc.lpCustColors = custom;
    cc.Flags        = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc)) { *c = cc.rgbResult; return TRUE; }
    return FALSE;
}

static void BrowseSound(void)
{
    wchar_t file[MAX_PATH * 2];
    lstrcpynW(file, g_cfg.soundFile, MAX_PATH);
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = s_hwnd;
    ofn.lpstrFilter = L"音频文件 (*.wav;*.mp3)\0*.wav;*.mp3\0"
                      L"WAV 波形 (*.wav)\0*.wav\0"
                      L"MP3 音频 (*.mp3)\0*.mp3\0"
                      L"所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"选择提示音";
    ofn.lpstrInitialDir = CfgSoundDir();
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    lstrcpynW(g_cfg.soundFile, file, MAX_PATH);
    g_cfg.soundSource = 2;
    CfgSave();
    FillUi();
    AppOnSettingsChanged();
}

/* 下拉栏选择变化：内置音 / 音源文件夹里的文件 / 浏览其他文件 */
static void SrcChanged(void)
{
    int sel = (int)SendMessageW(ui.cbSrc, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= s_sndCount) return;

    if (s_snd[sel].kind == SND_BROWSE) {
        BrowseSound();                    /* 内部会重建下拉栏并重新选中 */
        return;
    }

    if (s_snd[sel].kind == SND_BUILTIN) {
        g_cfg.soundSource = sel;          /* 0 柔铃 / 1 深呼吸 */
    } else {
        g_cfg.soundSource = 2;
        lstrcpynW(g_cfg.soundFile, s_snd[sel].path, MAX_PATH);
    }
    CfgSave();
    FillUi();
    AppOnSettingsChanged();
}

/* 打开音源文件夹（不存在就先建） */
static void OpenSoundDir(void)
{
    const wchar_t* dir = CfgSoundDir();
    CreateDirectoryW(dir, NULL);
    ShellExecuteW(s_hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
}

static void WlAdd(void)
{
    if (g_cfg.wlCount >= WL_MAX) {
        MessageBoxW(s_hwnd, L"白名单最多 24 项。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t file[MAX_PATH * 2];
    file[0] = 0;
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = s_hwnd;
    ofn.lpstrFilter = L"程序 (*.exe)\0*.exe\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = L"选择要静默的程序";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    const wchar_t* nm = wcsrchr(file, L'\\');
    nm = nm ? nm + 1 : file;

    for (int i = 0; i < g_cfg.wlCount; i++) {
        if (!_wcsicmp(g_cfg.wl[i], nm)) {
            MessageBoxW(s_hwnd, L"该程序已在白名单中。", L"提示", MB_OK | MB_ICONINFORMATION);
            return;
        }
    }
    lstrcpynW(g_cfg.wl[g_cfg.wlCount], nm, WL_LEN);
    g_cfg.wlCount++;
    CfgSave();
    FillUi();
    AppOnSettingsChanged();
}

static void WlDel(void)
{
    int sel = (int)SendMessageW(ui.lbWhite, LB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= g_cfg.wlCount) return;

    for (int i = sel; i < g_cfg.wlCount - 1; i++)
        lstrcpynW(g_cfg.wl[i], g_cfg.wl[i + 1], WL_LEN);
    g_cfg.wlCount--;
    CfgSave();
    FillUi();
    AppOnSettingsChanged();
}

/* ---------------- 建控件 ---------------- */
static void CreateChildren(HWND hwnd)
{
    const wchar_t* wd[7] = { L"一", L"二", L"三", L"四", L"五", L"六", L"日" };
    int dayIds[7] = { IDC_CHK_D0, IDC_CHK_D1, IDC_CHK_D2, IDC_CHK_D3, IDC_CHK_D4, IDC_CHK_D5, IDC_CHK_D6 };

    /* 建控件时会带初始值，会同步甩出 EN_CHANGE；不压住的话「预设」会被判成自定义 */
    s_filling = TRUE;

    /* ---- A 提醒节奏（弹窗 / 提示音 各自独立计时） ---- */
    Ctl(hwnd, L"BUTTON", L"提醒节奏（弹窗、提示音各自计时）", 16, 14, 428, 224, BS_GROUPBOX, 0, s_fontBold);

    /* 第 1 行：弹窗通道 */
    MkLabel(hwnd, L"弹窗", 30, 48, 44, 18, 0, s_fontBold);
    ui.chkPiv   = MkCheck(hwnd, L"固定间隔", 78, 44, 76, IDC_CHK_P_IV);
    ui.edPiv    = MkNum(hwnd, 158, 40, 52, IDC_ED_P_IV, IDC_UD_P_IV, 1, 120, g_cfg.popupIntervalMin);
    MkLabel(hwnd, L"分钟", 216, 48, 34, 18, 0, NULL);
    ui.chkPidle = MkCheck(hwnd, L"空闲", 254, 44, 52, IDC_CHK_P_IDLE);
    ui.edPidle  = MkNum(hwnd, 310, 40, 52, IDC_ED_P_IDLE, IDC_UD_P_IDLE, 1, 60, g_cfg.popupIdleMin);
    MkLabel(hwnd, L"分钟", 368, 48, 34, 18, 0, NULL);

    /* 第 2 行：提示音通道 */
    MkLabel(hwnd, L"提示音", 30, 86, 44, 18, 0, s_fontBold);
    ui.chkSiv   = MkCheck(hwnd, L"固定间隔", 78, 82, 76, IDC_CHK_S_IV);
    ui.edSiv    = MkNum(hwnd, 158, 78, 52, IDC_ED_S_IV, IDC_UD_S_IV, 1, 120, g_cfg.soundIntervalMin);
    MkLabel(hwnd, L"分钟", 216, 86, 34, 18, 0, NULL);
    ui.chkSidle = MkCheck(hwnd, L"空闲", 254, 82, 52, IDC_CHK_S_IDLE);
    ui.edSidle  = MkNum(hwnd, 310, 78, 52, IDC_ED_S_IDLE, IDC_UD_S_IDLE, 1, 60, g_cfg.soundIdleMin);
    MkLabel(hwnd, L"分钟", 368, 86, 34, 18, 0, NULL);

    ui.chkSched = MkCheck(hwnd, L"仅在生效时段提醒（两条通道都生效）", 30, 120, 300, IDC_CHK_SCHED);

    MkLabel(hwnd, L"星期", 46, 154, 34, 18, 0, NULL);
    for (int i = 0; i < 7; i++)
        ui.chkDay[i] = MkCheck(hwnd, wd[i], 84 + i * 44, 150, 44, dayIds[i]);

    MkLabel(hwnd, L"时段", 46, 186, 34, 18, 0, NULL);
    ui.edSh = MkNum(hwnd, 84, 182, 50, IDC_ED_SH, IDC_UD_SH, 0, 23, g_cfg.startHour);
    MkLabel(hwnd, L":",    136, 186, 8, 18, 0, NULL);
    ui.edSm = MkNum(hwnd, 146, 182, 50, IDC_ED_SM, IDC_UD_SM, 0, 59, g_cfg.startMinute);
    MkLabel(hwnd, L"至",   204, 186, 16, 18, 0, NULL);
    ui.edEh = MkNum(hwnd, 224, 182, 50, IDC_ED_EH, IDC_UD_EH, 0, 23, g_cfg.endHour);
    MkLabel(hwnd, L":",    276, 186, 8, 18, 0, NULL);
    ui.edEm = MkNum(hwnd, 286, 182, 50, IDC_ED_EM, IDC_UD_EM, 0, 59, g_cfg.endMinute);

    /* ---- B 弹窗提醒 ---- */
    Ctl(hwnd, L"BUTTON", L"弹窗提醒", 16, 250, 428, 352, BS_GROUPBOX, 0, s_fontBold);
    ui.chkPopup   = MkCheck(hwnd, L"启用弹窗提醒", 30, 284, 130, IDC_CHK_POPUP);
    ui.chkTopmost = MkCheck(hwnd, L"置顶（不抢输入焦点）", 168, 284, 200, IDC_CHK_TOPMOST);

    MkLabel(hwnd, L"位置", 30, 320, 32, 18, 0, NULL);
    ui.cbPos = MkCombo(hwnd, 66, 316, 116, IDC_CB_POS);
    SendMessageW(ui.cbPos, CB_ADDSTRING, 0, (LPARAM)L"左上角");
    SendMessageW(ui.cbPos, CB_ADDSTRING, 0, (LPARAM)L"右上角");
    SendMessageW(ui.cbPos, CB_ADDSTRING, 0, (LPARAM)L"左下角");
    SendMessageW(ui.cbPos, CB_ADDSTRING, 0, (LPARAM)L"右下角");
    SendMessageW(ui.cbPos, CB_ADDSTRING, 0, (LPARAM)L"屏幕居中");
    SendMessageW(ui.cbPos, CB_ADDSTRING, 0, (LPARAM)L"自定义坐标");
    MkLabel(hwnd, L"X", 192, 320, 14, 18, 0, NULL);
    ui.edCx  = MkNum(hwnd, 208, 316, 66, IDC_ED_CX, 0, -2000, 4000, g_cfg.customX);
    MkLabel(hwnd, L"Y", 282, 320, 14, 18, 0, NULL);
    ui.edCy  = MkNum(hwnd, 298, 316, 66, IDC_ED_CY, 0, -2000, 4000, g_cfg.customY);

    MkLabel(hwnd, L"宽度", 30, 356, 32, 18, 0, NULL);
    ui.edW = MkNum(hwnd, 66, 352, 62, IDC_ED_W, 0, 160, 1200, g_cfg.popupWidth);
    MkLabel(hwnd, L"高度", 138, 356, 32, 18, 0, NULL);
    ui.edH = MkNum(hwnd, 174, 352, 62, IDC_ED_H, 0, 60, 600, g_cfg.popupHeight);
    MkLabel(hwnd, L"透明度", 248, 356, 48, 18, 0, NULL);
    ui.tbOpacity = MkTrack(hwnd, 300, 350, 84, IDC_TB_OPACITY, 10, 100, g_cfg.opacity);
    ui.lbOpacity = MkLabel(hwnd, L"85%", 388, 356, 44, 18, IDC_LB_OPACITY, NULL);

    MkLabel(hwnd, L"背景色", 30, 392, 48, 18, 0, NULL);
    ui.btnBg = MkBtn(hwnd, L"", 82, 388, 54, 26, IDC_BTN_BG, BS_OWNERDRAW);
    MkLabel(hwnd, L"文字色", 148, 392, 48, 18, 0, NULL);
    ui.btnFg = MkBtn(hwnd, L"", 200, 388, 54, 26, IDC_BTN_FG, BS_OWNERDRAW);
    MkLabel(hwnd, L"字号", 266, 392, 32, 18, 0, NULL);
    ui.edFont = MkNum(hwnd, 302, 388, 56, IDC_ED_FONT, 0, 12, 48, g_cfg.fontSize);

    MkLabel(hwnd, L"停留", 30, 428, 32, 18, 0, NULL);
    ui.edDur = MkNum(hwnd, 66, 424, 58, IDC_ED_DUR, 0, 1, 10, g_cfg.duration);
    MkLabel(hwnd, L"秒", 132, 428, 16, 18, 0, NULL);
    ui.chkAutoClose = MkCheck(hwnd, L"自动消失（取消则点击才关闭）", 158, 424, 270, IDC_CHK_AUTOCLOSE);

    MkLabel(hwnd, L"文案", 30, 464, 32, 18, 0, NULL);
    ui.edText = Ctl(hwnd, L"EDIT", L"", 66, 460, 366, 28,
                    WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_ED_TEXT, s_font);
    MkLabel(hwnd, L"提示：弹窗不抢键盘焦点，打字不中断；尺寸随系统缩放自适应。",
            30, 498, 400, 18, 0, s_fontSmall);

    MkLabel(hwnd, L"屏幕", 30, 532, 32, 18, 0, NULL);
    ui.cbPMon = MkCombo(hwnd, 66, 528, 116, IDC_CB_PMON);
    SendMessageW(ui.cbPMon, CB_ADDSTRING, 0, (LPARAM)L"主显示器");
    SendMessageW(ui.cbPMon, CB_ADDSTRING, 0, (LPARAM)L"鼠标所在显示器");
    SendMessageW(ui.cbPMon, CB_ADDSTRING, 0, (LPARAM)L"所有显示器");
    SendMessageW(ui.cbPMon, CB_SETDROPPEDWIDTH, S(150), 0);
    MkLabel(hwnd, L"多显示器时弹窗显示在哪块屏", 190, 532, 250, 18, 0, s_fontSmall);

    /* ---- C 提示音 ---- */
    Ctl(hwnd, L"BUTTON", L"提示音", 470, 14, 394, 190, BS_GROUPBOX, 0, s_fontBold);
    ui.chkSound = MkCheck(hwnd, L"启用提示音", 484, 44, 120, IDC_CHK_SOUND);

    MkLabel(hwnd, L"音源", 484, 78, 32, 18, 0, NULL);
    ui.cbSrc = MkCombo(hwnd, 520, 74, 168, IDC_CB_SRC);
    SendMessageW(ui.cbSrc, CB_SETDROPPEDWIDTH, S(300), 0);   /* 长文件名也能整行看全 */
    ui.btnBrowse = MkBtn(hwnd, L"浏览…", 694, 73, 52, 26, IDC_BTN_BROWSE, 0);
    ui.btnSndDir = MkBtn(hwnd, L"音源文件夹", 750, 73, 82, 26, IDC_BTN_SNDDIR, 0);
    ui.lbFile = MkLabel(hwnd, L"", 484, 104, 380, 16, IDC_LB_FILE, s_fontSmall);

    MkLabel(hwnd, L"音量", 484, 130, 32, 18, 0, NULL);
    ui.tbVol = MkTrack(hwnd, 520, 124, 110, IDC_TB_VOL, 0, 100, g_cfg.volume);
    ui.lbVol = MkLabel(hwnd, L"60%", 634, 130, 44, 18, IDC_LB_VOL, NULL);

    ui.chkFade = MkCheck(hwnd, L"渐强播放", 484, 152, 100, IDC_CHK_FADE);
    MkLabel(hwnd, L"次数", 596, 156, 32, 18, 0, NULL);
    ui.cbRepeat = MkCombo(hwnd, 632, 152, 84, IDC_CB_REPEAT);
    SendMessageW(ui.cbRepeat, CB_ADDSTRING, 0, (LPARAM)L"单次");
    SendMessageW(ui.cbRepeat, CB_ADDSTRING, 0, (LPARAM)L"重复 2 次");

    /* ---- D 场景适配 ---- */
    Ctl(hwnd, L"BUTTON", L"场景适配", 470, 206, 394, 258, BS_GROUPBOX, 0, s_fontBold);
    ui.chkFsFull = MkCheck(hwnd, L"全屏时自动暂停", 484, 236, 170, IDC_CHK_FSFULL);
    MkLabel(hwnd, L"白名单（该程序在前台时自动静默）", 484, 264, 320, 18, 0, s_fontSmall);
    ui.lbWhite = Ctl(hwnd, L"LISTBOX", L"", 484, 286, 380, 112,
                     WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                     IDC_LB_WHITE, s_font);
    ui.btnWAdd = MkBtn(hwnd, L"添加程序…", 484, 406, 96, 26, IDC_BTN_WADD, 0);
    ui.btnWDel = MkBtn(hwnd, L"移除", 588, 406, 64, 26, IDC_BTN_WDEL, 0);
    ui.lbStatus = MkLabel(hwnd, L"", 484, 440, 380, 16, IDC_LB_STATUS, s_fontSmall);

    /* ---- E 常用 ---- */
    Ctl(hwnd, L"BUTTON", L"常用", 470, 476, 394, 126, BS_GROUPBOX, 0, s_fontBold);
    ui.chkAutoStart = MkCheck(hwnd, L"开机自动启动（可随时关闭）", 484, 506, 244, IDC_CHK_AUTOSTART);
    ui.chkKeep      = MkCheck(hwnd, L"保持屏幕常亮", 738, 506, 118, IDC_CHK_KEEP);
    MkLabel(hwnd, L"配置文件：", 484, 538, 66, 16, 0, s_fontSmall);
    ui.lbCfgPath = Ctl(hwnd, L"STATIC", L"", 550, 538, 302, 16,
                       SS_LEFT | SS_PATHELLIPSIS | SS_NOPREFIX, IDC_LB_CFGPATH, s_fontSmall);
    MkLabel(hwnd, L"纯文本 INI，可直接编辑。", 484, 560, 366, 16, 0, s_fontSmall);

    /* ---- F 边缘呼吸灯 ---- */
    Ctl(hwnd, L"BUTTON", L"边缘呼吸灯（弹窗 / 提示音的伴随光效）", 16, 612, 848, 150, BS_GROUPBOX, 0, s_fontBold);
    ui.chkEdge    = MkCheck(hwnd, L"启用边缘呼吸灯", 30, 652, 150, IDC_CHK_EDGE);
    MkLabel(hwnd, L"预设", 188, 656, 32, 18, 0, NULL);
    ui.cbEPreset  = MkCombo(hwnd, 224, 648, 186, IDC_CB_EPRESET);
    for (int i = 0; i < 6; i++)
        SendMessageW(ui.cbEPreset, CB_ADDSTRING, 0, (LPARAM)EdgePresetName(i));
    SendMessageW(ui.cbEPreset, CB_SETDROPPEDWIDTH, S(206), 0);
    MkLabel(hwnd, L"屏幕", 418, 656, 32, 18, 0, NULL);
    ui.cbEMon     = MkCombo(hwnd, 454, 648, 120, IDC_CB_EMON);
    SendMessageW(ui.cbEMon, CB_ADDSTRING, 0, (LPARAM)L"鼠标所在显示器");
    SendMessageW(ui.cbEMon, CB_ADDSTRING, 0, (LPARAM)L"所有显示器");
    SendMessageW(ui.cbEMon, CB_SETDROPPEDWIDTH, S(140), 0);
    MkLabel(hwnd, L"时长", 582, 656, 32, 18, 0, NULL);
    ui.cbEMode    = MkCombo(hwnd, 618, 648, 150, IDC_CB_EMODE);
    SendMessageW(ui.cbEMode, CB_ADDSTRING, 0, (LPARAM)L"跟随弹窗同生同灭");
    SendMessageW(ui.cbEMode, CB_ADDSTRING, 0, (LPARAM)L"固定秒数后熄灭");
    SendMessageW(ui.cbEMode, CB_ADDSTRING, 0, (LPARAM)L"一直呼吸到点击关闭");
    SendMessageW(ui.cbEMode, CB_SETDROPPEDWIDTH, S(172), 0);
    ui.edESecs    = MkNum(hwnd, 776, 648, 44, IDC_ED_ESECS, 0, 1, 10, g_cfg.edgeSecs);
    MkLabel(hwnd, L"秒", 824, 656, 16, 18, 0, NULL);

    MkLabel(hwnd, L"显示边", 30, 696, 44, 18, 0, NULL);
    ui.chkET      = MkCheck(hwnd, L"上", 78, 692, 40, IDC_CHK_ET);
    ui.chkEB      = MkCheck(hwnd, L"下", 120, 692, 40, IDC_CHK_EB);
    ui.chkEL      = MkCheck(hwnd, L"左", 162, 692, 40, IDC_CHK_EL);
    ui.chkER      = MkCheck(hwnd, L"右", 204, 692, 40, IDC_CHK_ER);
    MkLabel(hwnd, L"宽度", 250, 696, 32, 18, 0, NULL);
    ui.edEThick   = MkNum(hwnd, 286, 692, 46, IDC_ED_ETHICK, 0, 6, 80, g_cfg.edgeThick);
    MkLabel(hwnd, L"px", 336, 696, 18, 18, 0, NULL);
    MkLabel(hwnd, L"光色", 368, 696, 32, 18, 0, NULL);
    ui.btnEColor  = MkBtn(hwnd, L"", 404, 688, 48, 26, IDC_BTN_ECOLOR, BS_OWNERDRAW);
    MkLabel(hwnd, L"由谁带", 464, 696, 44, 18, 0, NULL);
    ui.chkEByPopup = MkCheck(hwnd, L"弹窗时亮", 512, 692, 84, IDC_CHK_EBY_POPUP);
    ui.chkEBySound = MkCheck(hwnd, L"提示音时亮", 600, 692, 96, IDC_CHK_EBY_SOUND);
    ui.btnEPreview = MkBtn(hwnd, L"预览呼吸灯", 706, 688, 96, 26, IDC_BTN_EPREVIEW, 0);

    MkLabel(hwnd, L"提示：光带贴屏幕边缘、向内渐隐并缓慢起伏；跟随弹窗即同生同灭，"
                 L"点击关闭模式会接管鼠标点击。",
            30, 730, 820, 18, 0, s_fontSmall);

    /* ---- 底部 ---- */
    MkBtn(hwnd, L"立即预览弹窗", 16, 776, 118, 32, IDC_BTN_PREVIEW, 0);
    MkBtn(hwnd, L"试听提示音", 142, 776, 108, 32, IDC_BTN_TESTSOUND, 0);
    MkBtn(hwnd, L"恢复默认", 258, 776, 96, 32, IDC_BTN_DEFAULT, 0);
    MkLabel(hwnd, L"修改即生效并自动保存", 366, 782, 220, 18, 0, s_fontSmall);
    MkBtn(hwnd, L"关闭", 772, 776, 92, 32, IDC_BTN_CLOSE, BS_DEFPUSHBUTTON);

    s_filling = FALSE;      /* 交给 FillUi 接着填真实配置 */
}

/* 删掉三套字体（必须先把用到它们的子控件销毁，否则字体对象还在被窗口引用） */
static void DropFonts(void)
{
    if (s_font)      { DeleteObject(s_font);      s_font = NULL; }
    if (s_fontSmall) { DeleteObject(s_fontSmall); s_fontSmall = NULL; }
    if (s_fontBold)  { DeleteObject(s_fontBold);  s_fontBold = NULL; }
}

/* ---------------- 窗口过程 ---------------- */
static void SettingsRelayout(HWND h, BOOL rebuildFonts);

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        /* 控件不在这里建：窗口还没落到目标屏上，此时问不到正确的 DPI。
           由 SettingsOpen 定位 + 校正缩放后统一排版。 */
        return 0;

    case WM_DPICHANGED: {
        /* 拖到另一块不同缩放的屏幕上。
           顺序很关键：先把窗口挪到系统建议的位置，再按新 DPI 重排整页——
           反过来做的话，重排出来的尺寸又会被建议矩形改掉，字就会跟控件错位。 */
        if (lp) {
            RECT* sug = (RECT*)lp;
            SetWindowPos(h, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        UINT d = HIWORD(wp);
        if (d && (int)d != g_dpi) {
            g_dpi   = (int)d;
            g_scale = MulDiv(100, g_dpi, 96);
            SettingsRelayout(h, TRUE);      /* 连字体一起按新 DPI 重建 */
        }
        return 0;
    }

    case WM_ACTIVATE:
        /* 从「音源文件夹」拖完文件回来时，把这个目录重新扫一遍 */
        if (LOWORD(wp) != WA_INACTIVE && !s_filling && ui.cbSrc) {
            BuildSrcCombo();
            UpdateFileLabel();
        }
        break;

    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wp, &rc, s_bk);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetBkMode(dc, TRANSPARENT);
        if (id == IDC_LB_FILE || id == IDC_LB_CFGPATH || id == IDC_LB_STATUS)
            SetTextColor(dc, RGB(0x6B, 0x72, 0x80));
        else
            SetTextColor(dc, RGB(0x1F, 0x23, 0x2B));
        return (LRESULT)s_bk;
    }

    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)s_bk;

    case WM_HSCROLL: {
        HWND tb = (HWND)lp;
        wchar_t buf[32];
        if (tb == ui.tbOpacity) {
            int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d%%", v);
            SetWindowTextW(ui.lbOpacity, buf);
        } else if (tb == ui.tbVol) {
            int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%d%%", v);
            SetWindowTextW(ui.lbVol, buf);
        }
        TouchDirty();
        return 0;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlID == IDC_BTN_BG || dis->CtlID == IDC_BTN_FG ||
            dis->CtlID == IDC_BTN_ECOLOR) {
            COLORREF col = (dis->CtlID == IDC_BTN_BG) ? g_cfg.bgColor
                         : (dis->CtlID == IDC_BTN_FG) ? g_cfg.fgColor
                         : g_cfg.edgeColor;
            HBRUSH br = CreateSolidBrush(col);
            RECT r = dis->rcItem;
            InflateRect(&r, -2, -2);
            FillRect(dis->hDC, &r, br);
            DeleteObject(br);
            FrameRect(dis->hDC, &dis->rcItem, (HBRUSH)GetStockObject(GRAY_BRUSH));
            return TRUE;
        }
        break;
    }

    case WM_TIMER:
        if (wp == 3) {
            KillTimer(h, 3);
            s_dirtyTimer = 0;
            CommitNow();
            return 0;
        }
        break;

    case WM_COMMAND: {
        int id   = LOWORD(wp);
        int code = HIWORD(wp);

        if (id == IDC_BTN_BG && code == BN_CLICKED) {
            COLORREF c = g_cfg.bgColor;
            if (PickColor(h, &c)) { g_cfg.bgColor = c; CommitNow(); InvalidateRect(ui.btnBg, NULL, TRUE); }
            return 0;
        }
        if (id == IDC_BTN_FG && code == BN_CLICKED) {
            COLORREF c = g_cfg.fgColor;
            if (PickColor(h, &c)) { g_cfg.fgColor = c; CommitNow(); InvalidateRect(ui.btnFg, NULL, TRUE); }
            return 0;
        }
        if (id == IDC_BTN_PREVIEW && code == BN_CLICKED) {
            CommitNow();
            PopupShow(&g_cfg);
            return 0;
        }
        if (id == IDC_BTN_TESTSOUND && code == BN_CLICKED) {
            CommitNow();
            AudioPlay(&g_cfg);
            return 0;
        }
        if (id == IDC_BTN_DEFAULT && code == BN_CLICKED) {
            if (MessageBoxW(h, L"确认恢复全部默认设置？", L"恢复默认",
                            MB_YESNO | MB_ICONQUESTION) == IDYES) {
                CfgDefaults(&g_cfg);
                AppSetAutoStart(FALSE);
                CfgSave();
                FillUi();
                AppOnSettingsChanged();
                InvalidateRect(h, NULL, TRUE);
            }
            return 0;
        }
        if (id == IDC_BTN_CLOSE && code == BN_CLICKED) {
            SendMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        if (id == IDC_BTN_BROWSE && code == BN_CLICKED) { BrowseSound(); return 0; }
        if (id == IDC_BTN_SNDDIR && code == BN_CLICKED) { OpenSoundDir(); return 0; }

        /* ---- 边缘呼吸灯 ---- */
        if (id == IDC_BTN_ECOLOR && code == BN_CLICKED) {
            COLORREF c = g_cfg.edgeColor;
            if (PickColor(h, &c)) {
                g_cfg.edgeColor = c;
                MarkEdgeCustom();
                CommitNow();
                InvalidateRect(ui.btnEColor, NULL, TRUE);
            }
            return 0;
        }
        if (id == IDC_CB_EPRESET && code == CBN_SELCHANGE) { ApplyEdgePreset(); return 0; }
        if (id == IDC_BTN_EPREVIEW && code == BN_CLICKED) {
            CommitNow();
            EdgePreview();                 /* 不管总开关，预览 4 秒 */
            return 0;
        }
        if ((id == IDC_CHK_ET || id == IDC_CHK_EB ||
             id == IDC_CHK_EL || id == IDC_CHK_ER) && code == BN_CLICKED) {
            MarkEdgeCustom();
            TouchDirty();
            UpdateEnableStates();
            return 0;
        }
        if (id == IDC_ED_ETHICK && code == EN_CHANGE) MarkEdgeCustom();
        if (id == IDC_CB_SRC     && code == CBN_SELCHANGE) { SrcChanged(); return 0; }
        if (id == IDC_BTN_WADD   && code == BN_CLICKED) { WlAdd(); return 0; }
        if (id == IDC_BTN_WDEL   && code == BN_CLICKED) { WlDel(); return 0; }

        if (id == IDC_CHK_AUTOSTART && code == BN_CLICKED)
            AppSetAutoStart(Chk(ui.chkAutoStart));

        if (id == IDC_CHK_KEEP && code == BN_CLICKED) {
            /* 立刻生效；落盘交给下面的 TouchDirty→CommitNow */
            g_cfg.keepAwake = Chk(ui.chkKeep);
            AppApplyKeepAwake();
        }

        if (code == CBN_SELCHANGE) {
            TouchDirty();
            UpdateEnableStates();
        } else if (code == EN_CHANGE) {
            TouchDirty();
        } else if (code == BN_CLICKED) {
            TouchDirty();
            UpdateEnableStates();
        }
        return 0;
    }

    case WM_CLOSE:
        if (s_dirtyTimer) { KillTimer(h, 3); s_dirtyTimer = 0; }
        CommitNow();
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        s_hwnd = NULL;
        /* 设置窗关闭后立刻回收它用掉的页面（字体/主题/公共控件初始化），
           再延迟 1.5 秒补一次，收掉延迟释放的那批页 */
        AppTrimWorkingSet();
        if (g_hwndMain) SetTimer(g_hwndMain, 3, 1500, NULL);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* 把窗口调成"客户区正好 = cw × ch"。
   不用 AdjustWindowRectEx 的公式直接定尺寸：它按线程 DPI 算边框，而我们是 PerMonitorV2，
   窗口在不同屏上边框/标题栏的实际厚度和公式对不上（DWM 还有一圈不可见边框），
   所以改成"实测客户区 → 差多少补多少"，迭代两次就精确收敛。 */
static void FitWindowToClient(HWND h, int cw, int ch)
{
    for (int i = 0; i < 3; i++) {
        RECT wr, cr;
        if (!GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return;
        int dw = cw - cr.right;
        int dh = ch - cr.bottom;
        if (dw == 0 && dh == 0) return;
        SetWindowPos(h, NULL, wr.left, wr.top,
                     (wr.right - wr.left) + dw, (wr.bottom - wr.top) + dh,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

/* 清掉旧控件，按当前 DPI 重建整页并定尺寸（位置不变） */
static void SettingsRelayout(HWND h, BOOL rebuildFonts)
{
    HWND ch = GetWindow(h, GW_CHILD);
    while (ch) {
        HWND nx = GetWindow(ch, GW_HWNDNEXT);
        DestroyWindow(ch);
        ch = nx;
    }
    ZeroMemory(&ui, sizeof(ui));

    /* 控件销毁后才能安全地换字体 */
    if (rebuildFonts) {
        DropFonts();
        SettingsInit();       /* 字体没了，这里会按当前 g_dpi 重造 */
    }

    CreateChildren(h);        /* 内部会压住 EN_CHANGE，避免预设被误判 */
    FillUi();
    UpdateEnableStates();

    FitWindowToClient(h, S(880), S(830));
    InvalidateRect(h, NULL, TRUE);
}

/* ---------------- 打开 / 关闭 ---------------- */
void SettingsOpen(HWND owner)
{
    if (s_hwnd && IsWindow(s_hwnd)) {
        ShowWindow(s_hwnd, IsIconic(s_hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(s_hwnd);
        FillUi();
        return;
    }

    SettingsInit();
    int cw = S(880), chh = S(830);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    RECT rc = { 0, 0, cw, chh };
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_TOOLWINDOW);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    /* 居中于主显示器的工作区（多显示器下不会跑到副屏） */
    POINT pt = { 0, 0 };
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (GetMonitorInfoW(mon, &mi)) wa = mi.rcWork;
    int x = wa.left + ((wa.right - wa.left) - ww) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top)  y = wa.top;

    s_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, APP_SET_CLASS,
                             L"专注守卫 · 设置",
                             style, x, y, ww, wh, NULL, NULL, g_hinst, NULL);
    if (!s_hwnd) return;

    /* 先落位到目标屏，再按那块屏的 DPI 校正缩放并排版 */
    SetWindowPos(s_hwnd, NULL, x, y, ww, wh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SyncScaleForWindow(s_hwnd);
    SettingsRelayout(s_hwnd, TRUE);

    ShowWindow(s_hwnd, SW_SHOW);
    UpdateWindow(s_hwnd);
    SetForegroundWindow(s_hwnd);
    SetFocus(ui.chkPiv);
}

BOOL SettingsIsOpen(void)
{
    return (s_hwnd && IsWindow(s_hwnd)) ? TRUE : FALSE;
}

void SettingsDestroy(void)
{
    if (s_hwnd && IsWindow(s_hwnd)) DestroyWindow(s_hwnd);
    s_hwnd = NULL;
}
