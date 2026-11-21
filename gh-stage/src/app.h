#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#ifndef STRICT
#define STRICT
#endif

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")

/* ---------------- 应用信息 ---------------- */
#define APP_NAME        L"\u4e13\u6ce8\u5b88\u536b"                 /* 专注守卫 */
#define APP_NAME_EN     L"FocusGuard"
#define APP_VER         L"1.0.0"
#define APP_CLASS       L"FocusGuard.MainWindow"
#define APP_POPUP_CLASS L"FocusGuard.PopupWindow"
#define APP_EDGE_CLASS  L"FocusGuard.EdgeGlow"
#define APP_SET_CLASS   L"FocusGuard.SettingsWindow"
#define APP_MUTEX       L"FocusGuard.SingleInstance.v1"
#define APP_RUNKEY      L"FocusGuard"
#define CFG_FILE        L"FocusGuard.ini"
#define SND_DIR         L"sounds"      /* 用户音源文件夹（exe 同目录下） */
#define SND_MAX         48             /* 下拉栏最多列出的音源文件数 */

/* ---------------- 常量 ---------------- */
#define TXT_MAX         400
#define WL_MAX          24
#define WL_LEN          128
#define IDI_APP         100

/* 计时器 / 消息 */
#define TIMER_MAIN      1
#define TIMER_AUDIO     2
#define TIMER_EDGE      4
#define WM_TRAY_MSG     (WM_APP + 101)
#define WM_APP_OPEN_SETTINGS (WM_APP + 102)
#define WM_APP_REMIND        (WM_APP + 103)

/* ---------------- 设置结构 ---------------- */
struct AppSettings {
    /* 提醒节奏：弹窗与提示音各自独立计时 */
    BOOL popupIntervalOn;
    int  popupIntervalMin;     /* 1..120 */
    BOOL popupIdleOn;
    int  popupIdleMin;         /* 1..60 */
    BOOL soundIntervalOn;
    int  soundIntervalMin;     /* 1..120 */
    BOOL soundIdleOn;
    int  soundIdleMin;         /* 1..60 */

    /* 生效时段 */
    BOOL scheduleEnabled;
    int  startHour, startMinute, endHour, endMinute;   /* 0..23 / 0..59 */
    BOOL days[7];              /* 0=周日 1=周一 ... 6=周六 */

    /* 弹窗 */
    BOOL popupEnabled;
    int  popupPos;             /* 0 左上 1 右上 2 左下 3 右下 4 居中 5 自定义 */
    int  customX, customY;
    int  popupWidth, popupHeight;
    int  opacity;              /* 10..100 */
    COLORREF bgColor, fgColor;
    wchar_t text[TXT_MAX];
    int  fontSize;             /* 12..48 */
    BOOL autoClose;
    int  duration;             /* 1..10 秒 */
    BOOL topmost;
    int  popupMonitor;         /* 0 主显示器 1 鼠标所在 2 所有显示器 */

    /* 边缘呼吸灯（弹窗 / 提示音的伴随光效） */
    BOOL edgeEnabled;
    int  edgeSides;            /* 1 上 2 下 4 左 8 右（可任意组合） */
    int  edgeThick;            /* 6..80，逻辑像素（按系统缩放放大） */
    int  edgePreset;           /* 0..4 内置方案 5 自定义 */
    COLORREF edgeColor;
    BOOL edgeByPopup;          /* 弹窗出现时亮 */
    BOOL edgeBySound;          /* 提示音响起时亮 */
    int  edgeMode;             /* 0 跟随弹窗 1 固定秒数 2 一直亮到点击 */
    int  edgeSecs;             /* 1..10，跟随模式的兜底秒数也用 3 */
    int  edgeMonitor;          /* 0 鼠标所在 1 所有显示器 */

    /* 提示音 */
    BOOL soundEnabled;
    int  soundSource;          /* 0 内置一 1 内置二 2 本地文件 */
    wchar_t soundFile[MAX_PATH];
    int  volume;               /* 0..100 */
    BOOL fadeIn;
    int  repeatCount;          /* 1..2 */

    /* 场景 */
    BOOL pauseOnFullscreen;
    wchar_t wl[WL_MAX][WL_LEN];
    int  wlCount;

    /* 通用 */
    BOOL autoStart;
    BOOL keepAwake;            /* 保持屏幕常亮（不睡屏、不熄屏） */
};

/* ---------------- 全局 ---------------- */
extern AppSettings g_cfg;
extern HINSTANCE   g_hinst;
extern HWND        g_hwndMain;
extern int         g_dpi;
extern int         g_scale;      /* 百分比，96dpi = 100 */

/* ---------------- config.cpp ---------------- */
void  CfgInit(void);                 /* 解析路径 + 载入 */
void  CfgSave(void);
const wchar_t* CfgPath(void);
const wchar_t* CfgSoundDir(void);    /* 用户音源文件夹，首次调用时创建 */
void  CfgDefaults(AppSettings* s);

/* ---------------- audio.cpp ---------------- */
void  AudioInit(void);
void  AudioPlay(const AppSettings* cfg);
void  AudioStop(void);
void  AudioTick(void);               /* 100ms 节拍（仅播放期间） */

/* ---------------- popup.cpp ---------------- */
void  PopupInit(void);
void  PopupShow(const AppSettings* cfg);
void  PopupHide(void);
void  PopupTick(void);               /* 每秒节拍，处理自动消失 */
BOOL  PopupVisible(void);
HWND  PopupWindow(void);             /* 第一个弹窗（通常即唯一那个） */

/* ---------------- edge.cpp ---------------- */
#define EDGE_MAX_MON 8
#define EDGE_ON_POPUP 1              /* EdgeStart 的 src 位 */
#define EDGE_ON_SOUND 2
void  EdgeInit(void);
void  EdgeStart(int src);            /* src = EDGE_ON_POPUP / EDGE_ON_SOUND 的按位或 */
void  EdgeStop(void);
void  EdgeTick(void);                /* 呼吸节拍，由 TIMER_EDGE 驱动 */
void  EdgePreview(void);             /* 设置界面「预览呼吸灯」：无视总开关亮 4 秒 */
BOOL  EdgeVisible(void);
BOOL  EdgePresetValues(int idx, int* sides, int* thick, COLORREF* color);
const wchar_t* EdgePresetName(int idx);

/* ---------------- settings.cpp ---------------- */
void  SettingsInit(void);
void  SettingsOpen(HWND owner);
BOOL  SettingsIsOpen(void);
void  SettingsRefreshStatus(void);
void  SettingsSyncFromConfig(void);   /* 托盘里改了开关后，把设置窗同步过来 */
void  SettingsDestroy(void);

/* ---------------- main.cpp ---------------- */
void  AppOnSettingsChanged(void);    /* 设置变更后重排计时 */
void  AppStateText(wchar_t* out, int cch);
void  AppTriggerReminder(void);
void  AppSetAutoStart(BOOL on);
BOOL  AppQueryAutoStart(void);
void  AppApplyKeepAwake(void);       /* 按 g_cfg.keepAwake 重新设置系统执行状态 */
void  AppTrimWorkingSet(void);       /* 主动收缩工作集，把闲置页面还给系统 */
