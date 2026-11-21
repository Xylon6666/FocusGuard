#include "app.h"
#include <string.h>
#include <math.h>

/* ============================================================
   提示音引擎
   - 内置音 / WAV：waveOut 单次异步播放，音量·渐强·重复全部预烘焙进缓冲
     （不修改系统音量、无回调线程、播放完立即释放缓冲）
   - MP3：系统 MCI 解码（按需加载，仅播放期间占用）
   ============================================================ */

#define AUD_RATE        44100
#define AUD_MAX_MS      5000      /* 单次采样最长 5 秒（控制内存） */
#define AUD_GAP_MS      320       /* 重复间隔 */
#define AUD_MCI_CAP_MS  8000      /* 外部音频最长播放 8 秒 */
#define MCI_ALIAS       L"fg_snd"

/* ---- WAV 状态 ---- */
static HWAVEOUT s_hwo   = NULL;
static WAVEHDR  s_hdr;
static short*   s_pcm   = NULL;
static DWORD    s_totalMs = 0;
static DWORD64  s_start = 0;

/* ---- MCI 状态 ---- */
static BOOL     s_mci = FALSE;
static DWORD64  s_mciStart = 0;
static int      s_mciRepeat = 0;
static BOOL     s_mciVolOk = FALSE;
static int      s_mciTarget = 0;
static DWORD    s_mciFadeStart = 0;

/* ---- 定时器 ---- */
static UINT_PTR s_timer = 0;

static DWORD64 Now64(void) { return GetTickCount64(); }

/* ================= 定时器管理 ================= */
static void EnsureTimer(void)
{
    if (!s_timer && g_hwndMain)
        s_timer = SetTimer(g_hwndMain, TIMER_AUDIO, 100, NULL);
}

static void MaybeKillTimer(void)
{
    if (!s_hwo && !s_mci && s_timer) {
        KillTimer(g_hwndMain, TIMER_AUDIO);
        s_timer = 0;
    }
}

/* ================= 停播 ================= */
static void CloseWave(void)
{
    if (s_hwo) {
        waveOutReset(s_hwo);
        if (s_hdr.lpData) waveOutUnprepareHeader(s_hwo, &s_hdr, sizeof(s_hdr));
        waveOutClose(s_hwo);
        s_hwo = NULL;
    }
    if (s_pcm) { free(s_pcm); s_pcm = NULL; }
    ZeroMemory(&s_hdr, sizeof(s_hdr));
    s_totalMs = 0;
}

static void CloseMci(void)
{
    if (s_mci || s_mciStart) {
        wchar_t cmd[96];
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"close %s", MCI_ALIAS);
        mciSendStringW(cmd, NULL, 0, NULL);
    }
    s_mci = FALSE;
    s_mciStart = 0;
    s_mciRepeat = 0;
}

void AudioStop(void)
{
    CloseWave();
    CloseMci();
    MaybeKillTimer();
}

/* ================= 内置提示音合成 ================= */
static void SynthTone(int which, DWORD rate, short** outPcm, DWORD* outFrames)
{
    *outPcm = NULL;
    *outFrames = 0;

    if (which <= 0) {
        /* 柔铃：E5 + 高八度泛音，指数衰减，极短起振 */
        DWORD frames = rate * 1300 / 1000;
        short* p = (short*)malloc(frames * sizeof(short));
        if (!p) return;
        for (DWORD i = 0; i < frames; i++) {
            double t   = (double)i / rate;
            double atk = t < 0.05 ? (t / 0.05) : 1.0;
            double dec = exp(-3.05 * t);
            double s   = 0.60 * sin(6.283185307 * 659.25 * t)
                       + 0.20 * sin(6.283185307 * 1318.50 * t)
                       + 0.07 * sin(6.283185307 * 329.63 * t);
            double v = s * atk * dec;
            if (v > 0.99) v = 0.99; if (v < -0.99) v = -0.99;
            p[i] = (short)(v * 32767.0);
        }
        *outPcm = p;
        *outFrames = frames;
    } else {
        /* 深呼吸：缓慢下滑音 + 平滑包络 */
        DWORD frames = rate * 1600 / 1000;
        short* p = (short*)malloc(frames * sizeof(short));
        if (!p) return;
        double phase = 0.0;
        for (DWORD i = 0; i < frames; i++) {
            double u = (double)i / frames;
            double f = 523.25 - 120.0 * u;
            phase += 6.283185307 * f / rate;
            double env = sin(3.141592653 * u);
            env = env * env * (0.55 + 0.45 * env);
            double s = 0.55 * sin(phase) + 0.11 * sin(phase * 2.0);
            double v = s * env;
            if (v > 0.99) v = 0.99; if (v < -0.99) v = -0.99;
            p[i] = (short)(v * 32767.0);
        }
        *outPcm = p;
        *outFrames = frames;
    }
}

/* ================= 音量 / 渐强 ================= */
static void ShapePcm(short* pcm, DWORD frames, DWORD rate, int volume, BOOL fade, BOOL tailFade)
{
    double gain = (double)volume / 100.0;
    if (gain <= 0) gain = 0.0;

    DWORD fadeN = fade ? (DWORD)((double)rate * 0.70) : 0;
    if (fadeN > frames / 2) fadeN = frames / 2;
    DWORD tailN = tailFade ? (DWORD)((double)rate * 0.12) : 0;
    if (tailN > frames / 3) tailN = frames / 3;

    for (DWORD i = 0; i < frames; i++) {
        double env = gain;
        if (fadeN && i < fadeN) {
            double t = (double)i / (double)fadeN;
            env *= t * t * (3.0 - 2.0 * t);      /* smoothstep 渐强 */
        }
        if (tailN && i >= frames - tailN) {
            double t = (double)(frames - i) / (double)tailN;
            env *= t * t * (3.0 - 2.0 * t);
        }
        pcm[i] = (short)((double)pcm[i] * env);
    }
}

/* ================= WAV 解析 ================= */
static BOOL LoadWavMono(const wchar_t* path, short** outPcm, DWORD* outFrames, DWORD* outRate)
{
    *outPcm = NULL; *outFrames = 0; *outRate = 0;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    BYTE hdr[12]; DWORD rd = 0;
    if (!ReadFile(h, hdr, 12, &rd, NULL) || rd != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        CloseHandle(h); return FALSE;
    }

    WORD  fmtTag = 0, ch = 0, bits = 0;
    DWORD rate = 0;
    LARGE_INTEGER dataPos; dataPos.QuadPart = 0;
    DWORD dataSize = 0;
    BOOL haveData = FALSE;

    for (int guard = 0; guard < 32 && !haveData; guard++) {
        BYTE ck[8];
        if (!ReadFile(h, ck, 8, &rd, NULL) || rd != 8) break;
        DWORD sz = *(DWORD*)(ck + 4);

        if (!memcmp(ck, "fmt ", 4)) {
            BYTE fb[40]; ZeroMemory(fb, sizeof(fb));
            DWORD toRead = sz < 40 ? sz : 40;
            if (!ReadFile(h, fb, toRead, &rd, NULL)) break;
            fmtTag = *(WORD*)(fb + 0);
            ch     = *(WORD*)(fb + 2);
            rate   = *(DWORD*)(fb + 4);
            bits   = *(WORD*)(fb + 14);
            if (sz > toRead) SetFilePointer(h, (LONG)(sz - toRead), NULL, FILE_CURRENT);
            if (sz & 1) SetFilePointer(h, 1, NULL, FILE_CURRENT);
        } else if (!memcmp(ck, "data", 4)) {
            dataPos.LowPart = SetFilePointer(h, 0, &dataPos.HighPart, FILE_CURRENT);
            dataSize = sz;
            haveData = TRUE;
        } else {
            SetFilePointer(h, (LONG)(sz + (sz & 1)), NULL, FILE_CURRENT);
        }
    }

    if (!haveData || fmtTag != 1 || !(ch == 1 || ch == 2) || !(bits == 8 || bits == 16) ||
        rate < 8000 || rate > 48000 || dataSize == 0) {
        CloseHandle(h); return FALSE;
    }

    DWORD blockAlign = (DWORD)ch * (bits >> 3);
    DWORD framesAll  = dataSize / blockAlign;
    DWORD framesMax  = (DWORD)((double)rate * AUD_MAX_MS / 1000.0);
    DWORD frames     = framesAll > framesMax ? framesMax : framesAll;
    if (frames == 0) { CloseHandle(h); return FALSE; }

    DWORD bytes = frames * blockAlign;
    BYTE* raw = (BYTE*)malloc(bytes);
    short* mono = (short*)malloc(frames * sizeof(short));
    if (!raw || !mono) { free(raw); free(mono); CloseHandle(h); return FALSE; }

    SetFilePointer(h, dataPos.LowPart, &dataPos.HighPart, FILE_BEGIN);
    DWORD got = 0;
    ReadFile(h, raw, bytes, &got, NULL);
    CloseHandle(h);
    if (got < bytes) frames = got / blockAlign;

    for (DWORD i = 0; i < frames; i++) {
        if (bits == 16) {
            short* p = (short*)(raw + i * blockAlign);
            int v = (ch == 2) ? ((int)p[0] + (int)p[1]) / 2 : (int)p[0];
            mono[i] = (short)v;
        } else {
            BYTE* p = raw + i * blockAlign;
            int v = (ch == 2) ? ((int)p[0] + (int)p[1]) / 2 : (int)p[0];
            mono[i] = (short)((v - 128) << 8);
        }
    }
    free(raw);

    *outPcm = mono;
    *outFrames = frames;
    *outRate = rate;
    return TRUE;
}

/* ================= 合并重复 ================= */
static short* ConcatRepeat(short* mono, DWORD frames, DWORD rate, int repeat, DWORD* outFrames)
{
    DWORD gap   = repeat > 1 ? (DWORD)((double)rate * AUD_GAP_MS / 1000.0) : 0;
    DWORD total = frames * (DWORD)repeat + gap * (DWORD)(repeat - 1);
    short* out  = (short*)malloc((size_t)total * sizeof(short));
    if (!out) return NULL;

    DWORD pos = 0;
    for (int i = 0; i < repeat; i++) {
        memcpy(out + pos, mono, (size_t)frames * sizeof(short));
        pos += frames;
        if (i < repeat - 1 && gap) {
            ZeroMemory(out + pos, (size_t)gap * sizeof(short));
            pos += gap;
        }
    }
    *outFrames = total;
    return out;
}

/* ================= 播放缓冲（waveOut） ================= */
static BOOL PlayBuffer(short* pcm, DWORD frames, DWORD rate)
{
    WAVEFORMATEX fmt;
    ZeroMemory(&fmt, sizeof(fmt));
    fmt.wFormatTag      = WAVE_FORMAT_PCM;
    fmt.nChannels       = 1;
    fmt.nSamplesPerSec  = rate;
    fmt.wBitsPerSample  = 16;
    fmt.nBlockAlign     = 2;
    fmt.nAvgBytesPerSec = rate * 2;
    fmt.cbSize          = 0;

    if (waveOutOpen(&s_hwo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        s_hwo = NULL;
        return FALSE;
    }

    ZeroMemory(&s_hdr, sizeof(s_hdr));
    s_hdr.lpData         = (LPSTR)pcm;
    s_hdr.dwBufferLength = frames * 2;

    if (waveOutPrepareHeader(s_hwo, &s_hdr, sizeof(s_hdr)) != MMSYSERR_NOERROR) {
        waveOutClose(s_hwo); s_hwo = NULL;
        return FALSE;
    }
    if (waveOutWrite(s_hwo, &s_hdr, sizeof(s_hdr)) != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(s_hwo, &s_hdr, sizeof(s_hdr));
        waveOutClose(s_hwo); s_hwo = NULL;
        return FALSE;
    }

    s_totalMs = (DWORD)((double)frames * 1000.0 / rate);
    s_start   = Now64();
    EnsureTimer();
    return TRUE;
}

/* ================= MCI（MP3 等） ================= */
static BOOL MciSend(const wchar_t* cmd, wchar_t* ret, DWORD cch)
{
    return mciSendStringW(cmd, ret, cch, NULL) == 0;
}

static void MciSetVolume(int v)
{
    if (!s_mciVolOk) return;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    wchar_t cmd[96];
    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"setaudio %s volume to %d", MCI_ALIAS, v * 100);
    MciSend(cmd, NULL, 0);
}

static BOOL StartMci(const wchar_t* path, const AppSettings* cfg)
{
    wchar_t cmd[640];

    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"open \"%s\" type mpegvideo alias %s", path, MCI_ALIAS);
    if (!MciSend(cmd, NULL, 0)) {
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"open \"%s\" alias %s", path, MCI_ALIAS);
        if (!MciSend(cmd, NULL, 0)) return FALSE;
    }

    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"set %s time format milliseconds", MCI_ALIAS);
    MciSend(cmd, NULL, 0);

    /* 独立音量：只在 MCI 支持 setaudio 时生效，绝不改动系统主音量 */
    int target = cfg->volume;
    s_mciVolOk = FALSE;
    {
        wchar_t t[96];
        _snwprintf_s(t, _countof(t), _TRUNCATE, L"setaudio %s volume to %d", MCI_ALIAS, target * 100);
        s_mciVolOk = MciSend(t, NULL, 0);
    }
    s_mciTarget = target;
    if (s_mciVolOk) {
        if (cfg->fadeIn) { MciSetVolume(3); s_mciFadeStart = (DWORD)Now64(); }
        else MciSetVolume(target);
    }

    _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"play %s", MCI_ALIAS);
    if (!MciSend(cmd, NULL, 0)) {
        CloseMci();
        return FALSE;
    }

    s_mci       = TRUE;
    s_mciStart  = Now64();
    s_mciRepeat = cfg->repeatCount > 1 ? cfg->repeatCount - 1 : 0;
    EnsureTimer();
    return TRUE;
}

/* ================= 对外：播放 ================= */
void AudioPlay(const AppSettings* cfg)
{
    AudioStop();
    if (!cfg->soundEnabled || cfg->volume <= 0) return;

    /* 1) 本地文件 */
    if (cfg->soundSource == 2 && cfg->soundFile[0]) {
        const wchar_t* dot = wcsrchr(cfg->soundFile, L'.');
        BOOL isMp3 = dot && !_wcsicmp(dot, L".mp3");

        if (isMp3) {
            if (StartMci(cfg->soundFile, cfg)) return;
            /* 系统缺少 MP3 解码器时退回内置音 */
        } else {
            short* mono = NULL; DWORD frames = 0, rate = 0;
            if (LoadWavMono(cfg->soundFile, &mono, &frames, &rate)) {
                BOOL truncated = FALSE;
                {
                    /* 粗略判断：满 5 秒即视为被截断，末段补淡出 */
                    if (frames >= (DWORD)((double)rate * AUD_MAX_MS / 1000.0)) truncated = TRUE;
                }
                ShapePcm(mono, frames, rate, cfg->volume, cfg->fadeIn, truncated);
                DWORD total = 0;
                short* merged = ConcatRepeat(mono, frames, rate, cfg->repeatCount, &total);
                free(mono);
                if (!merged) return;
                if (PlayBuffer(merged, total, rate)) { s_pcm = merged; return; }
                free(merged);
            }
            /* WAV 解析失败 → 退回内置音 */
        }
    }

    /* 2) 内置音 */
    int which = (cfg->soundSource == 1) ? 1 : 0;
    short* mono = NULL; DWORD frames = 0;
    SynthTone(which, AUD_RATE, &mono, &frames);
    if (!mono) return;

    ShapePcm(mono, frames, AUD_RATE, cfg->volume, cfg->fadeIn, FALSE);
    DWORD total = 0;
    short* merged = ConcatRepeat(mono, frames, AUD_RATE, cfg->repeatCount, &total);
    free(mono);
    if (!merged) return;
    if (PlayBuffer(merged, total, AUD_RATE)) s_pcm = merged;
    else free(merged);
}

/* ================= 对外：播放节拍 ================= */
void AudioTick(void)
{
    /* WAV：播放结束即释放缓冲 */
    if (s_hwo && s_pcm) {
        if (Now64() - s_start > (DWORD64)s_totalMs + 350) CloseWave();
    }

    /* MCI：渐强 + 重复 + 时长上限 */
    if (s_mci) {
        if (s_mciVolOk && s_mciFadeStart) {
            DWORD el = (DWORD)(Now64() - s_mciFadeStart);
            if (el < 750) {
                int v = 3 + (int)((s_mciTarget - 3) * (int)el / 750);
                MciSetVolume(v);
            } else {
                MciSetVolume(s_mciTarget);
                s_mciFadeStart = 0;
            }
        }

        wchar_t st[64] = L"";
        wchar_t cmd[96];
        _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"status %s mode", MCI_ALIAS);
        BOOL stopped = FALSE;
        if (MciSend(cmd, st, 64)) {
            if (!_wcsicmp(st, L"stopped")) stopped = TRUE;
        }
        DWORD el = (DWORD)(Now64() - s_mciStart);

        if (stopped || el >= AUD_MCI_CAP_MS) {
            if (s_mciRepeat > 0) {
                s_mciRepeat--;
                _snwprintf_s(cmd, _countof(cmd), _TRUNCATE, L"play %s from 0", MCI_ALIAS);
                if (MciSend(cmd, NULL, 0)) s_mciStart = Now64();
                else CloseMci();
            } else {
                CloseMci();
            }
        }
    }

    MaybeKillTimer();
}

void AudioInit(void)
{
    ZeroMemory(&s_hdr, sizeof(s_hdr));
    s_timer = 0;
}
