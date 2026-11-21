#include "app.h"
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

/* ============================================================
   极简 INI 配置：自解析 + UTF-8 存储，不依赖任何框架
   ============================================================ */

static wchar_t s_path[MAX_PATH * 2] = L"";
static BOOL    s_ready = FALSE;

const wchar_t* CfgPath(void) { return s_path; }

/* 用户音源文件夹：与 ini 同目录的 sounds 子目录，启动时创建好，
   用户把 mp3/wav 丢进去即可在设置界面的下拉栏里直接选到 */
static wchar_t s_sndDir[MAX_PATH * 2] = L"";
static BOOL    s_sndReady = FALSE;

const wchar_t* CfgSoundDir(void)
{
    if (s_sndReady) return s_sndDir;
    if (!s_ready) CfgInit();

    lstrcpynW(s_sndDir, s_path, MAX_PATH * 2);
    wchar_t* slash = wcsrchr(s_sndDir, L'\\');
    if (slash) slash[1] = 0;
    wchar_t buf[MAX_PATH * 2] = L"";
    lstrcpynW(buf, s_sndDir, MAX_PATH * 2);
    _snwprintf_s(s_sndDir, _countof(s_sndDir), _TRUNCATE, L"%s%s", buf, SND_DIR);
    CreateDirectoryW(s_sndDir, NULL);
    s_sndReady = TRUE;
    return s_sndDir;
}

/* ---------- 小工具 ---------- */
static void Cat(wchar_t* dst, size_t cap, size_t* len, const wchar_t* fmt, ...)
{
    wchar_t tmp[640];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(tmp, _countof(tmp), _TRUNCATE, fmt, ap);
    va_end(ap);
    size_t t = wcslen(tmp);
    if (*len + t < cap) {
        wmemcpy(dst + *len, tmp, t);
        *len += t;
        dst[*len] = 0;
    }
}

static int ClampI(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int RwInt(const wchar_t* s) { return (int)wcstol(s, NULL, 10); }

/* 旧版 [reminder] 段（interval_on / interval_min / idle_on / idle_min）
   只在新键缺失时作为迁移来源，不再写出 */
static BOOL s_legIvOn = TRUE, s_legIdleOn = TRUE, s_legSeen = FALSE;
static int  s_legIvMin = 25, s_legIdleMin = 5;
static BOOL s_seenPiv, s_seenPidle, s_seenSiv, s_seenSidle;

static void TrimW(wchar_t* s)
{
    if (!s) return;
    wchar_t* p = s;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(wchar_t));
    size_t n = wcslen(s);
    while (n > 0 && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n')) s[--n] = 0;
}

static COLORREF ParseColor(const wchar_t* v, COLORREF def)
{
    if (!v || !*v) return def;
    while (*v == L'#') v++;
    unsigned long n = wcstoul(v, NULL, 16);
    if (n > 0xFFFFFF) return def;
    return RGB((n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF);
}

/* ---------- 默认值 ---------- */
void CfgDefaults(AppSettings* s)
{
    ZeroMemory(s, sizeof(*s));

    s->popupIntervalOn  = TRUE;
    s->popupIntervalMin = 25;
    s->popupIdleOn      = TRUE;
    s->popupIdleMin     = 5;
    s->soundIntervalOn  = TRUE;
    s->soundIntervalMin = 25;
    s->soundIdleOn      = TRUE;
    s->soundIdleMin     = 5;

    s->scheduleEnabled = FALSE;
    s->startHour = 9;  s->startMinute = 0;
    s->endHour   = 18; s->endMinute   = 0;
    s->days[0] = FALSE;               /* 周日 */
    s->days[1] = s->days[2] = s->days[3] = s->days[4] = s->days[5] = TRUE;
    s->days[6] = FALSE;               /* 周六 */

    s->popupEnabled  = TRUE;
    s->popupPos      = 1;             /* 右上角 */
    s->customX = 40; s->customY = 40;
    s->popupWidth  = 420;
    s->popupHeight = 150;
    s->opacity   = 85;
    s->bgColor   = RGB(30, 34, 42);
    s->fgColor   = RGB(255, 255, 255);
    lstrcpynW(s->text, L"\u56de\u5230\u5f53\u524d\u4efb\u52a1\uff0c\u4fdd\u6301\u4e13\u6ce8\u72b6\u6001", TXT_MAX); /* 回到当前任务，保持专注状态 */
    s->fontSize  = 22;
    s->autoClose = TRUE;
    s->duration  = 3;
    s->topmost   = TRUE;
    s->popupMonitor = 0;              /* 主显示器，与旧行为一致 */

    s->soundEnabled = TRUE;
    s->soundSource  = 0;
    s->volume       = 60;
    s->fadeIn       = TRUE;
    s->repeatCount  = 1;

    s->pauseOnFullscreen = TRUE;
    s->wlCount = 0;

    /* 边缘呼吸灯：默认开、柔和整圈、跟随弹窗、鼠标所在屏 */
    s->edgeEnabled = TRUE;
    s->edgeSides   = 15;                              /* 上 下 左 右 都亮 */
    s->edgeThick   = 18;
    s->edgePreset  = 0;
    s->edgeColor   = RGB(0x4C, 0x8D, 0xFF);
    s->edgeByPopup = TRUE;
    s->edgeBySound = FALSE;
    s->edgeMode    = 0;
    s->edgeSecs    = 4;
    s->edgeMonitor = 0;

    s->autoStart = FALSE;
    s->keepAwake = FALSE;
}

/* ---------- 路径 ---------- */
static BOOL EnsureWritable(const wchar_t* p)
{
    HANDLE h = CreateFileW(p, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle(h);
    return TRUE;
}

static void ResolvePath(void)
{
    wchar_t exe[MAX_PATH * 2] = L"";
    GetModuleFileNameW(NULL, exe, _countof(exe));

    wchar_t dir[MAX_PATH * 2] = L"";
    lstrcpynW(dir, exe, _countof(dir));
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;

    wchar_t cand[MAX_PATH * 2] = L"";
    _snwprintf_s(cand, _countof(cand), _TRUNCATE, L"%s%s", dir, CFG_FILE);

    if (EnsureWritable(cand)) {
        lstrcpynW(s_path, cand, _countof(s_path));
        return;
    }

    /* 程序目录不可写（例如装在 Program Files），退到 %APPDATA% */
    wchar_t appdata[MAX_PATH * 2] = L"";
    if (!GetEnvironmentVariableW(L"APPDATA", appdata, _countof(appdata)) || !appdata[0]) {
        lstrcpynW(s_path, cand, _countof(s_path));
        return;
    }
    wchar_t sub[MAX_PATH * 2] = L"";
    _snwprintf_s(sub, _countof(sub), _TRUNCATE, L"%s\\%s", appdata, APP_NAME_EN);
    CreateDirectoryW(sub, NULL);
    _snwprintf_s(s_path, _countof(s_path), _TRUNCATE, L"%s\\%s", sub, CFG_FILE);
}

/* ---------- 读文件 ---------- */
static void ReadText(const wchar_t* path, wchar_t* out, int cch)
{
    out[0] = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;

    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 512 * 1024) { CloseHandle(h); return; }

    char* raw = (char*)malloc(size + 1);
    if (!raw) { CloseHandle(h); return; }
    DWORD read = 0;
    ReadFile(h, raw, size, &read, NULL);
    CloseHandle(h);
    raw[read] = 0;

    const char* src = raw;
    if (read >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        src = raw + 3;
        read -= 3;
    }
    MultiByteToWideChar(CP_UTF8, 0, src, (int)read, out, cch - 1);
    out[cch - 1] = 0;
    free(raw);
}

static void ApplyKey(AppSettings* s, const wchar_t* k, const wchar_t* v)
{
    if      (!_wcsicmp(k, L"interval_on"))  { s_legSeen = TRUE; s_legIvOn = RwInt(v) ? TRUE : FALSE; }
    else if (!_wcsicmp(k, L"interval_min")) { s_legSeen = TRUE; s_legIvMin = RwInt(v); }
    else if (!_wcsicmp(k, L"idle_on"))      { s_legSeen = TRUE; s_legIdleOn = RwInt(v) ? TRUE : FALSE; }
    else if (!_wcsicmp(k, L"idle_min"))     { s_legSeen = TRUE; s_legIdleMin = RwInt(v); }
    else if (!_wcsicmp(k, L"p_iv_on"))      { s_seenPiv = TRUE;   s->popupIntervalOn = RwInt(v) ? TRUE : FALSE; }
    else if (!_wcsicmp(k, L"p_iv_min"))     { s_seenPiv = TRUE;   s->popupIntervalMin = RwInt(v); }
    else if (!_wcsicmp(k, L"p_idle_on"))    { s_seenPidle = TRUE; s->popupIdleOn = RwInt(v) ? TRUE : FALSE; }
    else if (!_wcsicmp(k, L"p_idle_min"))   { s_seenPidle = TRUE; s->popupIdleMin = RwInt(v); }
    else if (!_wcsicmp(k, L"s_iv_on"))      { s_seenSiv = TRUE;   s->soundIntervalOn = RwInt(v) ? TRUE : FALSE; }
    else if (!_wcsicmp(k, L"s_iv_min"))     { s_seenSiv = TRUE;   s->soundIntervalMin = RwInt(v); }
    else if (!_wcsicmp(k, L"s_idle_on"))    { s_seenSidle = TRUE; s->soundIdleOn = RwInt(v) ? TRUE : FALSE; }
    else if (!_wcsicmp(k, L"s_idle_min"))   { s_seenSidle = TRUE; s->soundIdleMin = RwInt(v); }
    else if (!_wcsicmp(k, L"sched_on"))     s->scheduleEnabled = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"start_h"))      s->startHour = RwInt(v);
    else if (!_wcsicmp(k, L"start_m"))      s->startMinute = RwInt(v);
    else if (!_wcsicmp(k, L"end_h"))        s->endHour = RwInt(v);
    else if (!_wcsicmp(k, L"end_m"))        s->endMinute = RwInt(v);
    else if (!_wcsicmp(k, L"days")) {
        for (int i = 0; i < 7 && v[i]; i++) s->days[i] = (v[i] == L'1') ? TRUE : FALSE;
    }
    else if (!_wcsicmp(k, L"popup_on"))   s->popupEnabled = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"pos"))        s->popupPos = RwInt(v);
    else if (!_wcsicmp(k, L"x"))          s->customX = RwInt(v);
    else if (!_wcsicmp(k, L"y"))          s->customY = RwInt(v);
    else if (!_wcsicmp(k, L"w"))          s->popupWidth = RwInt(v);
    else if (!_wcsicmp(k, L"h"))          s->popupHeight = RwInt(v);
    else if (!_wcsicmp(k, L"opacity"))    s->opacity = RwInt(v);
    else if (!_wcsicmp(k, L"bg"))         s->bgColor = ParseColor(v, s->bgColor);
    else if (!_wcsicmp(k, L"fg"))         s->fgColor = ParseColor(v, s->fgColor);
    else if (!_wcsicmp(k, L"text"))       { if (*v) lstrcpynW(s->text, v, TXT_MAX); }
    else if (!_wcsicmp(k, L"font"))       s->fontSize = RwInt(v);
    else if (!_wcsicmp(k, L"autoclose"))  s->autoClose = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"duration"))   s->duration = RwInt(v);
    else if (!_wcsicmp(k, L"topmost"))    s->topmost = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"p_mon"))      s->popupMonitor = RwInt(v);
    else if (!_wcsicmp(k, L"e_on"))       s->edgeEnabled = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"e_sides"))    s->edgeSides = RwInt(v);
    else if (!_wcsicmp(k, L"e_thick"))    s->edgeThick = RwInt(v);
    else if (!_wcsicmp(k, L"e_preset"))   s->edgePreset = RwInt(v);
    else if (!_wcsicmp(k, L"e_color"))    s->edgeColor = ParseColor(v, s->edgeColor);
    else if (!_wcsicmp(k, L"e_by_popup")) s->edgeByPopup = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"e_by_sound")) s->edgeBySound = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"e_mode"))     s->edgeMode = RwInt(v);
    else if (!_wcsicmp(k, L"e_secs"))     s->edgeSecs = RwInt(v);
    else if (!_wcsicmp(k, L"e_mon"))      s->edgeMonitor = RwInt(v);
    else if (!_wcsicmp(k, L"sound_on"))   s->soundEnabled = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"src"))        s->soundSource = RwInt(v);
    else if (!_wcsicmp(k, L"file"))       lstrcpynW(s->soundFile, v, MAX_PATH);
    else if (!_wcsicmp(k, L"volume"))     s->volume = RwInt(v);
    else if (!_wcsicmp(k, L"fade"))       s->fadeIn = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"repeat"))     s->repeatCount = RwInt(v);
    else if (!_wcsicmp(k, L"fullscreen_on")) s->pauseOnFullscreen = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"autostart"))  s->autoStart = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"keep_awake")) s->keepAwake = RwInt(v) ? TRUE : FALSE;
    else if (!_wcsicmp(k, L"count"))      s->wlCount = RwInt(v);
    else if (!_wcsicmp(k, L"item")) {
        /* item=<名称> */
        if (s->wlCount < WL_MAX && *v) {
            for (int i = 0; i < s->wlCount; i++)
                if (!_wcsicmp(s->wl[i], v)) return;
            lstrcpynW(s->wl[s->wlCount], v, WL_LEN);
            s->wlCount++;
        }
    }
}

static void Normalize(AppSettings* s)
{
    s->popupIntervalMin = ClampI(s->popupIntervalMin, 1, 120);
    s->popupIdleMin     = ClampI(s->popupIdleMin, 1, 60);
    s->soundIntervalMin = ClampI(s->soundIntervalMin, 1, 120);
    s->soundIdleMin     = ClampI(s->soundIdleMin, 1, 60);

    s->startHour   = ClampI(s->startHour, 0, 23);
    s->startMinute = ClampI(s->startMinute, 0, 59);
    s->endHour     = ClampI(s->endHour, 0, 23);
    s->endMinute   = ClampI(s->endMinute, 0, 59);

    s->popupPos    = ClampI(s->popupPos, 0, 5);
    s->popupWidth  = ClampI(s->popupWidth, 160, 1200);
    s->popupHeight = ClampI(s->popupHeight, 60, 600);
    s->opacity     = ClampI(s->opacity, 10, 100);
    s->fontSize    = ClampI(s->fontSize, 12, 48);
    s->duration    = ClampI(s->duration, 1, 10);
    s->volume      = ClampI(s->volume, 0, 100);
    s->repeatCount = ClampI(s->repeatCount, 1, 2);
    s->soundSource = ClampI(s->soundSource, 0, 2);
    s->wlCount     = ClampI(s->wlCount, 0, WL_MAX);

    s->popupMonitor = ClampI(s->popupMonitor, 0, 2);
    s->edgeSides    = ClampI(s->edgeSides, 1, 15);
    s->edgeThick    = ClampI(s->edgeThick, 6, 80);
    s->edgePreset   = ClampI(s->edgePreset, 0, 5);
    s->edgeMode     = ClampI(s->edgeMode, 0, 2);
    s->edgeSecs     = ClampI(s->edgeSecs, 1, 10);
    s->edgeMonitor  = ClampI(s->edgeMonitor, 0, 1);
    if (!s->edgeByPopup && !s->edgeBySound) s->edgeByPopup = TRUE;   /* 两个都不勾则等于没开 */

    if (!s->text[0])
        lstrcpynW(s->text, L"\u56de\u5230\u5f53\u524d\u4efb\u52a1\uff0c\u4fdd\u6301\u4e13\u6ce8\u72b6\u6001", TXT_MAX);
}

void CfgInit(void)
{
    if (s_ready) return;
    ResolvePath();
    CfgDefaults(&g_cfg);

    s_legSeen = s_seenPiv = s_seenPidle = s_seenSiv = s_seenSidle = FALSE;

    wchar_t* buf = (wchar_t*)malloc(sizeof(wchar_t) * 131072);
    if (buf) {
        ReadText(s_path, buf, 131072);
        if (buf[0]) {
            wchar_t line[640];
            const wchar_t* p = buf;
            while (*p) {
                const wchar_t* e = p;
                while (*e && *e != L'\r' && *e != L'\n') e++;
                size_t len = (size_t)(e - p);
                if (len > 630) len = 630;
                wmemcpy(line, p, len);
                line[len] = 0;
                p = e;
                while (*p == L'\r' || *p == L'\n') p++;

                TrimW(line);
                if (!line[0] || line[0] == L';' || line[0] == L'#') continue;
                if (line[0] == L'[') continue;

                wchar_t* eq = wcschr(line, L'=');
                if (!eq) continue;
                *eq = 0;
                wchar_t* k = line;
                wchar_t* v = eq + 1;
                TrimW(k);
                TrimW(v);
                ApplyKey(&g_cfg, k, v);
            }
        }
        free(buf);
    }
    /* 老配置文件只有一套节奏（[reminder]），迁移成两条独立节奏 */
    if (s_legSeen) {
        if (!s_seenPiv)   { g_cfg.popupIntervalOn  = s_legIvOn;   g_cfg.popupIntervalMin = s_legIvMin; }
        if (!s_seenPidle) { g_cfg.popupIdleOn      = s_legIdleOn; g_cfg.popupIdleMin     = s_legIdleMin; }
        if (!s_seenSiv)   { g_cfg.soundIntervalOn  = s_legIvOn;   g_cfg.soundIntervalMin = s_legIvMin; }
        if (!s_seenSidle) { g_cfg.soundIdleOn      = s_legIdleOn; g_cfg.soundIdleMin     = s_legIdleMin; }
    }
    Normalize(&g_cfg);
    s_ready = TRUE;

    /* 首次运行：立刻落盘一份默认配置，方便用户查看与编辑 */
    {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(s_path, GetFileExInfoStandard, &fad) || fad.nFileSizeLow == 0)
            CfgSave();
    }
}

/* ---------- 写文件 ---------- */
void CfgSave(void)
{
    if (!s_ready && !s_path[0]) return;

    wchar_t body[6144];
    size_t  n = 0;
    body[0] = 0;

    Cat(body, _countof(body), &n, L"; %s %s \u914d\u7f6e\u6587\u4ef6\uff08\u81ea\u52a8\u751f\u6210\uff0c\u53ef\u76f4\u63a5\u7f16\u8f91\uff09\r\n", APP_NAME_EN, APP_VER);
    Cat(body, _countof(body), &n, L"; \u63d0\u793a\uff1a\u5f39\u7a97\u4e0e\u63d0\u793a\u97f3\u5404\u6709\u4e00\u5957\u72ec\u7acb\u8ba1\u65f6\uff08p_* \u5c5e\u5f39\u7a97\uff0cs_* \u5c5e\u63d0\u793a\u97f3\uff09\r\n");

    Cat(body, _countof(body), &n, L"[schedule]\r\n");
    Cat(body, _countof(body), &n, L"sched_on=%d\r\n", g_cfg.scheduleEnabled ? 1 : 0);
    Cat(body, _countof(body), &n, L"start_h=%d\r\n", g_cfg.startHour);
    Cat(body, _countof(body), &n, L"start_m=%d\r\n", g_cfg.startMinute);
    Cat(body, _countof(body), &n, L"end_h=%d\r\n", g_cfg.endHour);
    Cat(body, _countof(body), &n, L"end_m=%d\r\n", g_cfg.endMinute);
    wchar_t days[8];
    for (int i = 0; i < 7; i++) days[i] = g_cfg.days[i] ? L'1' : L'0';
    days[7] = 0;
    Cat(body, _countof(body), &n, L"days=%s\r\n", days);

    Cat(body, _countof(body), &n, L"[popup]\r\n");
    Cat(body, _countof(body), &n, L"popup_on=%d\r\n", g_cfg.popupEnabled ? 1 : 0);
    Cat(body, _countof(body), &n, L"p_iv_on=%d\r\n", g_cfg.popupIntervalOn ? 1 : 0);
    Cat(body, _countof(body), &n, L"p_iv_min=%d\r\n", g_cfg.popupIntervalMin);
    Cat(body, _countof(body), &n, L"p_idle_on=%d\r\n", g_cfg.popupIdleOn ? 1 : 0);
    Cat(body, _countof(body), &n, L"p_idle_min=%d\r\n", g_cfg.popupIdleMin);
    Cat(body, _countof(body), &n, L"pos=%d\r\n", g_cfg.popupPos);
    Cat(body, _countof(body), &n, L"x=%d\r\n", g_cfg.customX);
    Cat(body, _countof(body), &n, L"y=%d\r\n", g_cfg.customY);
    Cat(body, _countof(body), &n, L"w=%d\r\n", g_cfg.popupWidth);
    Cat(body, _countof(body), &n, L"h=%d\r\n", g_cfg.popupHeight);
    Cat(body, _countof(body), &n, L"opacity=%d\r\n", g_cfg.opacity);
    Cat(body, _countof(body), &n, L"bg=%02X%02X%02X\r\n",
        GetRValue(g_cfg.bgColor), GetGValue(g_cfg.bgColor), GetBValue(g_cfg.bgColor));
    Cat(body, _countof(body), &n, L"fg=%02X%02X%02X\r\n",
        GetRValue(g_cfg.fgColor), GetGValue(g_cfg.fgColor), GetBValue(g_cfg.fgColor));
    Cat(body, _countof(body), &n, L"font=%d\r\n", g_cfg.fontSize);
    Cat(body, _countof(body), &n, L"text=%s\r\n", g_cfg.text);
    Cat(body, _countof(body), &n, L"autoclose=%d\r\n", g_cfg.autoClose ? 1 : 0);
    Cat(body, _countof(body), &n, L"duration=%d\r\n", g_cfg.duration);
    Cat(body, _countof(body), &n, L"topmost=%d\r\n", g_cfg.topmost ? 1 : 0);
    Cat(body, _countof(body), &n, L"p_mon=%d\r\n", g_cfg.popupMonitor);

    Cat(body, _countof(body), &n, L"[sound]\r\n");
    Cat(body, _countof(body), &n, L"sound_on=%d\r\n", g_cfg.soundEnabled ? 1 : 0);
    Cat(body, _countof(body), &n, L"s_iv_on=%d\r\n", g_cfg.soundIntervalOn ? 1 : 0);
    Cat(body, _countof(body), &n, L"s_iv_min=%d\r\n", g_cfg.soundIntervalMin);
    Cat(body, _countof(body), &n, L"s_idle_on=%d\r\n", g_cfg.soundIdleOn ? 1 : 0);
    Cat(body, _countof(body), &n, L"s_idle_min=%d\r\n", g_cfg.soundIdleMin);
    Cat(body, _countof(body), &n, L"src=%d\r\n", g_cfg.soundSource);
    Cat(body, _countof(body), &n, L"file=%s\r\n", g_cfg.soundFile);
    Cat(body, _countof(body), &n, L"volume=%d\r\n", g_cfg.volume);
    Cat(body, _countof(body), &n, L"fade=%d\r\n", g_cfg.fadeIn ? 1 : 0);
    Cat(body, _countof(body), &n, L"repeat=%d\r\n", g_cfg.repeatCount);

    Cat(body, _countof(body), &n, L"[edge]\r\n");
    Cat(body, _countof(body), &n, L"; 边缘呼吸灯：贴屏幕边的半透明光带，明暗缓慢起伏\r\n");
    Cat(body, _countof(body), &n, L"; e_sides 1=上 2=下 4=左 8=右，可相加组合\r\n");
    Cat(body, _countof(body), &n, L"; e_mode 0=跟随弹窗 1=固定秒数 2=一直亮到点击\r\n");
    Cat(body, _countof(body), &n, L"e_on=%d\r\n", g_cfg.edgeEnabled ? 1 : 0);
    Cat(body, _countof(body), &n, L"e_sides=%d\r\n", g_cfg.edgeSides);
    Cat(body, _countof(body), &n, L"e_thick=%d\r\n", g_cfg.edgeThick);
    Cat(body, _countof(body), &n, L"e_preset=%d\r\n", g_cfg.edgePreset);
    Cat(body, _countof(body), &n, L"e_color=%02X%02X%02X\r\n",
        GetRValue(g_cfg.edgeColor), GetGValue(g_cfg.edgeColor), GetBValue(g_cfg.edgeColor));
    Cat(body, _countof(body), &n, L"e_by_popup=%d\r\n", g_cfg.edgeByPopup ? 1 : 0);
    Cat(body, _countof(body), &n, L"e_by_sound=%d\r\n", g_cfg.edgeBySound ? 1 : 0);
    Cat(body, _countof(body), &n, L"e_mode=%d\r\n", g_cfg.edgeMode);
    Cat(body, _countof(body), &n, L"e_secs=%d\r\n", g_cfg.edgeSecs);
    Cat(body, _countof(body), &n, L"e_mon=%d\r\n", g_cfg.edgeMonitor);

    Cat(body, _countof(body), &n, L"[scene]\r\n");
    Cat(body, _countof(body), &n, L"fullscreen_on=%d\r\n", g_cfg.pauseOnFullscreen ? 1 : 0);

    Cat(body, _countof(body), &n, L"[whitelist]\r\n");
    Cat(body, _countof(body), &n, L"count=%d\r\n", g_cfg.wlCount);
    for (int i = 0; i < g_cfg.wlCount; i++)
        Cat(body, _countof(body), &n, L"item=%s\r\n", g_cfg.wl[i]);

    Cat(body, _countof(body), &n, L"[general]\r\n");
    Cat(body, _countof(body), &n, L"autostart=%d\r\n", g_cfg.autoStart ? 1 : 0);
    Cat(body, _countof(body), &n, L"keep_awake=%d\r\n", g_cfg.keepAwake ? 1 : 0);

    /* UTF-8 无 BOM 写入 */
    int need = WideCharToMultiByte(CP_UTF8, 0, body, (int)n, NULL, 0, NULL, NULL);
    if (need <= 0) return;
    char* raw = (char*)malloc(need + 1);
    if (!raw) return;
    WideCharToMultiByte(CP_UTF8, 0, body, (int)n, raw, need, NULL, NULL);
    raw[need] = 0;

    HANDLE h = CreateFileW(s_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(h, raw, (DWORD)need, &wrote, NULL);
        CloseHandle(h);
    }
    free(raw);
}
