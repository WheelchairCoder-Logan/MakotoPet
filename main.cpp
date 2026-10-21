// ============================================================================
//  MakotoPet —— 一个跑在 Windows 桌面上的桌宠：常陆茉子
//  Copyright (c) 2025 MakotoPet contributors
//  Licensed under the MIT License.  See LICENSE / NOTICE in the repository root.
//
//  这是一个个人练手项目，开源的目的是记录"这个东西是怎么做出来的"。
//  整个客户端只有这一个 .cpp，只用 Windows 系统组件，不依赖第三方库：
//
//    · 显示   分层透明窗口 + 序列帧动画 + 磁盘纹理缓存
//    · 交互   托盘图标、右键菜单、拖拽、Ctrl+滚轮缩放、开机自启
//    · 音频   XAudio2 播放 WAV，BGM / 音效 / TTS / RVC 翻唱
//    · 对话   接 OpenAI 兼容接口，中日双语回复
//    · 养成   好感度由大模型每轮打分，累计后逐步解锁服装与功能
//
//  角色名、窗口类名等是本项目的固有设定，直接写在代码里，没有做参数化。
//  想改成别的角色的话，全局替换「茉子」「MakotoPet」并换掉素材即可。
//
//  源码以 UTF-8 **带 BOM** 保存（MSVC 靠 BOM 识别编码，否则中文注释会变乱码）。
//  如果你的编辑器去掉了 BOM，请给 MSVC 加上 /utf-8 编译选项。
// ============================================================================

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <windows.h>
#include <mmsystem.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <xaudio2.h>
#include <cstring>
#include <winhttp.h>
#include <process.h>
#include <fstream>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <utility>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "winhttp.lib")

using namespace std;
using namespace Gdiplus;


// 调试日志开关：默认 0（关闭）。
// 改成 1 会把大模型的原始响应打到 VS 输出窗口——任何挂了调试器或装了
// DebugView 的人都能看到对话正文。排查解析问题时临时打开，用完请改回 0，
// Release 版本不要开。
#ifndef PET_ENABLE_DEBUG_LOG
#define PET_ENABLE_DEBUG_LOG 0
#endif

// 受上面开关控制的调试输出。关着的时候整段会被优化掉，不产生任何开销。
static void PetDbgA(const char* s) {
#if PET_ENABLE_DEBUG_LOG
    OutputDebugStringA(s);
#else
    (void)s;
#endif
}
static void PetDbgW(const wchar_t* s) {
#if PET_ENABLE_DEBUG_LOG
    OutputDebugStringW(s);
#else
    (void)s;
#endif
}

// ================= 全局 =================
ULONG_PTR g_gdiToken = 0;
HWND g_hwnd = nullptr;
#define BASE_W 200
#define BASE_H 300

// ==================== 开发人员开关（发布版务必为 0） ====================
// 改成 1 重新编译：所有养成功能立即全开，不看好感度。
//   · 菜单里每一项都可用，不再灰掉、不再显示"🔒 好感 xx 解锁"
//   · 读档不再把服装退回忍者服
//   · 不会再弹解锁气泡
// 好感度照常累计，菜单顶部照常显示数值，方便测试。
//
// 不想每次改代码，可以在 VS 里新建一个「Debug-Dev」配置：
//   项目属性 → C/C++ → 预处理器 → 预处理器定义，加上 DEV_UNLOCK_ALL=1
// 这样切配置就能在开发版/正式版之间来回切，源文件不用动。
#ifndef DEV_UNLOCK_ALL
#define DEV_UNLOCK_ALL 0
#endif

float g_zoom = 1.0f;
int g_currentW = BASE_W;
int g_currentH = BASE_H;
#define FPS 30
#define FRAME_INTERVAL_MS (1000 / FPS)

#define WM_TRAY (WM_USER + 1)
#define WM_APP_PRELOAD_DONE (WM_APP + 1)   // 素材全量加载完成，可以显示人物了
#define WM_APP_AFFINITY     (WM_APP + 2)   // 对话好感度增减（wParam = delta + 16，便于传负数）
#define AFFINITY_MSG_BIAS   16
#define ID_TRAY_SHOW 100
#define ID_TRAY_AUTOSTART 101
#define ID_TRAY_EXIT 102

#define ID_VOLUME_MUTE  110
#define ID_VOLUME_LOW   111
#define ID_VOLUME_MID   112
#define ID_VOLUME_HIGH  113

#define ID_BGM_SELECT_START 2000
#define ID_CHAT 3000
#define ID_RVC_PICK 4000            // 🎤 弹出文件框，选择一首歌来翻唱
#define ID_RVC_STATUS 4001          // 转换状态显示（不可点击，仅占位）
#define ID_RVC_OCT_DOWN 4100        // 翻唱变调：-1 档（降 RVC_OCTAVE_SEMI 半音）
#define ID_RVC_OCT_ZERO 4101        // 翻唱变调： 0 档（不升不降）
#define ID_RVC_OCT_UP   4102        // 翻唱变调：+1 档（升 RVC_OCTAVE_SEMI 半音）
// 一档 = 多少半音。8 比整八度 12 温和：男声源抬 12 常常过高发尖，
// 8 半音能把音域抬进模型舒适区又不至于失真。改这里要和 bridge 的 RVC_OCTAVE_STEP 一致。
#define RVC_OCTAVE_SEMI 8

enum class AnimState { Idle, Run, Think, Peace };
enum class Costume { Ninja, Daily, School, Dress };

Costume g_currentCostume = Costume::Ninja;
AnimState g_currentState = AnimState::Idle;

vector<Image*> g_currentFrames;
int g_currentFrameIndex = 0;

int g_currentVolume = 700;
int g_savedPosX = -1;
int g_savedPosY = -1;
bool g_hasSavedPos = false;

bool g_bDragging = false;
POINT g_ptLButtonDown = { 0 };
POINT g_dragWindowStart = { 0 };

int g_idleActionTimer = 0;
bool g_isRandomAction = false;

Image* g_pTransitionImg = nullptr;
Image* g_pLoadingImg = nullptr;          // 启动加载期显示的图标（icon.ico）
static int g_loadTick = 0;               // 加载动画节拍
bool g_bTransitioning = false;
Costume g_pendingCostume = Costume::Ninja;
AnimState g_pendingState = AnimState::Idle;
int g_transitionTimer = 0;
const int TRANSITION_DURATION = 40;
vector<Image*> g_pendingFrames;

HDC     g_memDC = nullptr;
HBITMAP g_hMemBmp = nullptr;
HBITMAP g_hOldBmp = nullptr;
int     g_cachedW = 0;
int     g_cachedH = 0;

static bool g_bStartupPlayed = false;
const float CHAR_RATIO = 1.0f;

vector<wstring> g_bgmFileList;
vector<wstring> g_bgmDisplayNameList;
int g_currentBGMIndex = 0;
// 翻唱变调档位：-1 / 0 / +1（男声源配女声模型通常选 +1），存 config.ini 的 [RVC] Octave
// 实际半音数 = 档位 x RVC_OCTAVE_SEMI
int g_rvcOctave = 0;

int g_costumeChangeIndex = 0;

// ================= 性能与运行状态（新增） =================
static Graphics*    g_gfx = nullptr;          // 复用的离屏 Graphics，避免每帧构造/析构
static int          g_winX = 0, g_winY = 0;   // 逻辑窗口位置：拖拽只改这里，不再调用 SetWindowPos
static bool         g_winPosKnown = false;
static ULONGLONG    g_lastPaintMs = 0;        // 上次合成时刻（用于节流）
static bool         g_paintPending = false;   // 被节流掉的那一次，交给下一次定时重绘补上
static bool         g_timerOn = true;         // 隐藏桌宠时停掉定时器，不再空转渲染
static ULONGLONG    g_lastTickMs = 0;         // 上一帧真实时刻（累计陪伴时长用）
static double       g_hourAcc = 0.0;          // 满 3600 秒给 1 点好感
static ULONGLONG    g_lastStatsSaveMs = 0;    // 统计定期落盘
static ULONGLONG    g_lastAchCheckMs = 0;     // 成就定期判定

// ================= XAudio2 音频 =================
static IXAudio2* g_xa = nullptr;
static IXAudio2MasteringVoice* g_master = nullptr;

struct XSound {
    WAVEFORMATEX wfx{};
    std::vector<BYTE> pcm;
    IXAudio2SourceVoice* v = nullptr;
    bool loaded = false;
    bool playing = false;
    void Reset() {
        if (v) { v->Stop(0); v->DestroyVoice(); v = nullptr; }
        pcm.clear(); wfx = {}; loaded = false; playing = false;
    }
};

struct SfxCallback : public IXAudio2VoiceCallback {
    XSound* s = nullptr;
    STDMETHOD_(void, OnBufferEnd)(void*) override { if (s) s->playing = false; }
    STDMETHOD_(void, OnVoiceProcessingPassStart)(UINT32) override {}
    STDMETHOD_(void, OnVoiceProcessingPassEnd)() override {}
    STDMETHOD_(void, OnStreamEnd)() override {}
    STDMETHOD_(void, OnBufferStart)(void*) override {}
    STDMETHOD_(void, OnLoopEnd)(void*) override {}
    STDMETHOD_(void, OnVoiceError)(void*, HRESULT) override {}
};
// ★ 池子必须 >= 声音对象总数，否则 cb->s 被后来的声音覆盖，
//   旧声音的 playing 标志永远不会被 OnBufferEnd 复位。
//   当前对象：bgm/startup/exit/think/peace/tts/rvcCover + costume[3] = 9，留足余量用 16。
static SfxCallback g_sfxCb[16];
static int g_sfxCbIdx = 0;

static XSound g_bgm, g_startup, g_exit, g_think, g_peace, g_costume[3];
static XSound g_tts; // TTS 音频
static XSound g_rvcCover; // ← 新增：RVC 翻唱结果
static std::atomic<bool> g_rvcBusy{false};  // 原子标记：菜单线程与工作线程无锁共享
static HANDLE g_hRvcThread = nullptr;       // 后台线程句柄，退出时等待其结束

// RAII：保证工作线程任何退出路径（含异常/提前 return）都会清除 busy 标记
struct RvcBusyGuard {
    RvcBusyGuard() { g_rvcBusy.store(true); }
    ~RvcBusyGuard() { g_rvcBusy.store(false); }
    RvcBusyGuard(const RvcBusyGuard&) = delete;
    RvcBusyGuard& operator=(const RvcBusyGuard&) = delete;
};

// ===== RVC 转换状态显示（全文件唯一定义，勿重复） =====
enum class RvcStage { Idle, Checking, Separating, Uploading, Inferring, Done, Failed };
static RvcStage  g_rvcStage = RvcStage::Idle;   // 当前阶段
static std::wstring g_rvcStatusText;            // 顶部状态文字
static int       g_rvcStatusTimer = 0;          // 倒计时（帧数），到 0 自动隐藏
static std::wstring g_rvcSourceName;            // 本次源文件名（用于显示）
static std::recursive_mutex g_rvcMtx;           // 递归锁：状态函数可能被嵌套调用
static int       g_rvcProgress = -1;            // 0~100；<0 表示不显示进度条

static const wchar_t* RvcStageText(RvcStage s) {
    switch (s) {
    case RvcStage::Checking:   return L"正在连接 RVC 服务…";
    case RvcStage::Separating: return L"正在分离人声与伴奏…（请耐心等待）";
    case RvcStage::Uploading:  return L"正在上传音频…";
    case RvcStage::Inferring:  return L"正在推理变声…（长音频可能需1-2分钟）";
    case RvcStage::Done:       return L"✅ 转换完成！正在混音播放…";
    case RvcStage::Failed:     return L"❌ 转换失败（服务未启动或模型不可用）";
    default: return L"";
    }
}
static void RvcSetStage(RvcStage s) {
    std::lock_guard<std::recursive_mutex> lk(g_rvcMtx);
    g_rvcStage = s;
    g_rvcStatusText = RvcStageText(s);
    if (s == RvcStage::Done || s == RvcStage::Failed) {
        g_rvcStatusTimer = FPS * 4;   // 成功/失败显示 4 秒后自动消失
        g_rvcProgress = -1;
    } else {
        g_rvcStatusTimer = 0;         // 进行中一直显示
    }
}
// 更新进度百分比（<0 隐藏进度条），由轮询线程调用，状态栏负责绘制
static void RvcSetProgress(int pct) {
    std::lock_guard<std::recursive_mutex> lk(g_rvcMtx);
    g_rvcProgress = pct;
}

// ================= 静态分析安全工具 =================
// 集中提供"带检查"的包装，顺带消除 MSVC /analyze 的 C6011 / C6031 / C6262：
// 不只是为了消警告，窗口已销毁、文件读取失败这些边界情况也确实更安全。

// 取窗口矩形：防御 hwnd 为空或已销毁，并检查返回值
static bool GetWinRect(HWND h, RECT& rc) {
    if (!h || !IsWindow(h)) { SetRectEmpty(&rc); return false; }
    if (!GetWindowRect(h, &rc)) { SetRectEmpty(&rc); return false; }
    return true;
}

// 取 exe 所在目录。两个 API 的返回值都要检查，否则失败时会拿空路径去拼素材路径。
static bool GetExeDir(std::wstring& out) {
    wchar_t buf[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    if (!PathRemoveFileSpecW(buf)) return false;
    out.assign(buf);
    return !out.empty();
}

// 写 ini：写失败不影响运行（读取时都有默认值兜底），显式声明忽略返回值
static void WriteIniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* val, const wchar_t* ini) {
#pragma warning(suppress:6031)
    WritePrivateProfileStringW(sec, key, val, ini);
}
static void WriteIniInt(const wchar_t* sec, const wchar_t* key, int v, const wchar_t* ini) {
    wchar_t b[32];
    if (_snwprintf_s(b, 32, _TRUNCATE, L"%d", v) < 0) return;
#pragma warning(suppress:6031)
    WritePrivateProfileStringW(sec, key, b, ini);
}
static void WriteIniDbl(const wchar_t* sec, const wchar_t* key, double v, const wchar_t* ini) {
    wchar_t b[48];
    if (_snwprintf_s(b, 48, _TRUNCATE, L"%.6g", v) < 0) return;
#pragma warning(suppress:6031)
    WritePrivateProfileStringW(sec, key, b, ini);
}

static float AudioGain() { return (float)g_currentVolume / 1000.0f; }

static void ClearFrames(vector<Image*>& frames) {
    for (auto* img : frames) { if (img) delete img; }
    frames.clear();
}

static bool LoadWav(const wchar_t* path, XSound& s) {
    s.Reset();
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct { char riff[4]; DWORD size; char wave[4]; } h0;
    DWORD rd;
    if (!ReadFile(h, &h0, sizeof h0, &rd, nullptr) || rd != sizeof h0 ||
        memcmp(h0.riff, "RIFF", 4) || memcmp(h0.wave, "WAVE", 4)) {
        CloseHandle(h); return false;
    }
    LARGE_INTEGER fs; GetFileSizeEx(h, &fs);
    DWORD pos = sizeof h0; bool gotfmt = false, gotdata = false;
    while (pos + 8 <= (DWORD)fs.QuadPart) {
        char cid[4]; DWORD cs;
        SetFilePointer(h, pos, nullptr, FILE_BEGIN);
        if (!ReadFile(h, cid, 4, &rd, nullptr) || !ReadFile(h, &cs, 4, &rd, nullptr)) break;
        // ★ 声明长度可能超过文件实际剩余字节（损坏/被截断的 wav）：
        //   不裁剪的话 resize(cs) 会一次申请巨量内存，pos 还可能回绕成死循环
        {
            DWORD avail = 0;
            if ((DWORD)fs.QuadPart > pos + 8) avail = (DWORD)fs.QuadPart - (pos + 8);
            if (cs > avail) cs = avail;
        }
        if (!memcmp(cid, "fmt ", 4)) {
            SetFilePointer(h, pos + 8, nullptr, FILE_BEGIN);
            WAVEFORMATEX tmp{};
            DWORD tr = min(cs, (DWORD)sizeof(WAVEFORMATEX));
            ReadFile(h, &tmp, tr, &rd, nullptr);
            if (tmp.wFormatTag == 1) { s.wfx = tmp; s.wfx.cbSize = 0; gotfmt = true; }
        } else if (!memcmp(cid, "data", 4)) {
            SetFilePointer(h, pos + 8, nullptr, FILE_BEGIN);
            s.pcm.resize(cs);
            if (cs) ReadFile(h, s.pcm.data(), cs, &rd, nullptr);
            gotdata = true;
        }
        pos += 8 + cs + (cs & 1);
    }
    CloseHandle(h);
    if (!gotfmt || !gotdata || s.pcm.empty() || s.wfx.nChannels == 0) return false;
    s.loaded = true; return true;
}

// 从内存中的 WAV 数据直接解析（用于 TTS / RVC）
static bool LoadWavFromMemory(const std::vector<char>& wavData, XSound& s) {
    s.Reset();
    if (wavData.size() < 44) return false;
    if (memcmp(wavData.data(), "RIFF", 4) != 0 || memcmp(wavData.data() + 8, "WAVE", 4) != 0) return false;
    DWORD pos = 12;
    bool gotfmt = false, gotdata = false;
    while (pos + 8 <= wavData.size()) {
        char cid[4]; DWORD cs;
        memcpy(cid, wavData.data() + pos, 4);
        memcpy(&cs, wavData.data() + pos + 4, 4);
        if (!memcmp(cid, "fmt ", 4)) {
            WAVEFORMATEX tmp{};
            DWORD tr = min(cs, (DWORD)sizeof(WAVEFORMATEX));
            memcpy(&tmp, wavData.data() + pos + 8, tr);
            if (tmp.wFormatTag == 1) { s.wfx = tmp; s.wfx.cbSize = 0; gotfmt = true; }
        } else if (!memcmp(cid, "data", 4)) {
            // ★ 声明长度可能超过实际剩余字节，必须裁剪，避免越界读与超大内存分配
            size_t avail = 0;
            if (wavData.size() > (size_t)pos + 8) avail = wavData.size() - ((size_t)pos + 8);
            DWORD take = (cs < (DWORD)avail) ? cs : (DWORD)avail;
            s.pcm.resize(take);
            if (take) memcpy(s.pcm.data(), wavData.data() + pos + 8, take);
            gotdata = true;
        }
        pos += 8 + cs + (cs & 1);
    }
    if (!gotfmt || !gotdata || s.pcm.empty() || s.wfx.nChannels == 0) return false;
    s.loaded = true; return true;
}

static void WaitForSoundToFinish(XSound& s, DWORD timeoutMs = 2000) {
    if (!s.v || !s.loaded) return;
    DWORD start = GetTickCount();
    for (;;) {
        XAUDIO2_VOICE_STATE state;
        s.v->GetState(&state);
        if (state.BuffersQueued == 0) { Sleep(50); break; }
        if (GetTickCount() - start > timeoutMs) break;
        Sleep(10);
    }
}

static void XSoundPlay(XSound& s, bool loop) {
    if (!g_xa || !s.loaded) return;
    if (!s.v) {
        SfxCallback* cb = &g_sfxCb[g_sfxCbIdx++ % 16];
        cb->s = &s; s.playing = false;
        HRESULT hr = g_xa->CreateSourceVoice(&s.v, &s.wfx, 0,
            XAUDIO2_DEFAULT_FREQ_RATIO, cb, nullptr, nullptr);
        if (FAILED(hr)) { s.v = nullptr; return; }
    }
    XAUDIO2_VOICE_STATE st{};
    s.v->GetState(&st);
    if (st.BuffersQueued > 0 || s.playing) {
        s.v->Stop(0);
        s.v->FlushSourceBuffers();
    }
    for (int i = 0; i < 50; i++) {
        XAUDIO2_VOICE_STATE t{};
        s.v->GetState(&t);
        if (t.BuffersQueued == 0) break;
        Sleep(1);
    }
    XAUDIO2_BUFFER buf2{};
    buf2.pAudioData = s.pcm.data();
    buf2.AudioBytes = (UINT)s.pcm.size();
    buf2.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
    if (FAILED(s.v->SubmitSourceBuffer(&buf2))) return;
    s.playing = true;
    s.v->Start(0, 0);
}

static void XSoundStop(XSound& s) { if (s.v) { s.v->Stop(0); s.v->FlushSourceBuffers(); } }

static bool AudioX_Init() {
    if (FAILED(XAudio2Create(&g_xa, 0))) return false;
    if (FAILED(g_xa->CreateMasteringVoice(&g_master, XAUDIO2_DEFAULT_CHANNELS,
        XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr))) {
        g_xa->Release(); g_xa = nullptr; return false;
    }
    g_master->SetVolume(AudioGain());
    return true;
}

static void AudioX_SetVolume(int vol) { if (g_master) g_master->SetVolume((float)vol / 1000.0f); }

static void AudioX_ShutdownAll() {
    XSoundStop(g_bgm); XSoundStop(g_startup); XSoundStop(g_exit);
    XSoundStop(g_think); XSoundStop(g_peace); XSoundStop(g_tts); XSoundStop(g_rvcCover);
    for (auto& c : g_costume) XSoundStop(c);
    auto dz = [](XSound& s) { if (s.v) { s.v->DestroyVoice(); s.v = nullptr; } };
    dz(g_bgm); dz(g_startup); dz(g_exit); dz(g_think); dz(g_peace); dz(g_tts); dz(g_rvcCover);
    for (auto& c : g_costume) dz(c);
    if (g_master) { g_master->DestroyVoice(); g_master = nullptr; }
    if (g_xa) { g_xa->Release(); g_xa = nullptr; }
}

static void AudioX_Preload() {
    std::wstring d;
    if (!GetExeDir(d)) return;          // 取不到目录就放弃预加载，不带路径去拼会读到错误位置
    auto sf = [&](const wchar_t* fn) -> std::wstring {
        return d + L"\\" + fn;
    };
    LoadWav(sf(L"startup.wav").c_str(), g_startup);
    LoadWav(sf(L"exit.wav").c_str(), g_exit);
    LoadWav(sf(L"think.wav").c_str(), g_think);
    LoadWav(sf(L"peace.wav").c_str(), g_peace);
    for (int i = 0; i < 3; i++) { wchar_t nm[32]; wsprintfW(nm, L"costume%d.wav", i + 1); LoadWav(sf(nm).c_str(), g_costume[i]); }
}

// ---- BGM 扫描 ----
void ScanBGMDirectory() {
    g_bgmFileList.clear(); g_bgmDisplayNameList.clear();
    wchar_t audioDir[MAX_PATH]; GetModuleFileNameW(nullptr, audioDir, MAX_PATH);
    PathRemoveFileSpecW(audioDir); lstrcatW(audioDir, L"\\Audio");
    if (!PathFileExistsW(audioDir)) CreateDirectoryW(audioDir, nullptr);
    wchar_t sp[MAX_PATH]; wsprintfW(sp, L"%s\\*.wav", audioDir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(sp, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                wchar_t fp[MAX_PATH]; wsprintfW(fp, L"%s\\%s", audioDir, fd.cFileName);
                g_bgmFileList.push_back(fp);
                std::wstring n = fd.cFileName;
                if (n.size() > 4 && n.substr(n.size() - 4) == L".wav") n = n.substr(0, n.size() - 4);
                g_bgmDisplayNameList.push_back(n);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_bgmFileList.empty()) {
        wchar_t dp[MAX_PATH]; GetModuleFileNameW(nullptr, dp, MAX_PATH);
        PathRemoveFileSpecW(dp); lstrcatW(dp, L"\\Audio\\BGM.wav");
        g_bgmFileList.push_back(dp); g_bgmDisplayNameList.push_back(L"默认");
    }
}

void StopBGM() { XSoundStop(g_bgm); if (g_bgm.v) { g_bgm.v->DestroyVoice(); g_bgm.v = nullptr; } g_bgm.pcm.clear(); g_bgm.loaded = false; }

void PlayBGMByIndex(int index) {
    if (index < 0 || index >= (int)g_bgmFileList.size()) return;
    StopBGM(); g_currentBGMIndex = index;
    if (LoadWav(g_bgmFileList[index].c_str(), g_bgm)) XSoundPlay(g_bgm, true);
}

void SetBGMVolume(int vol) { g_currentVolume = vol; AudioX_SetVolume(vol); }
void SyncAllSFXVolume() { AudioX_SetVolume(g_currentVolume); }

void PlayPoseSFX(AnimState state) {
    if (state == AnimState::Think) XSoundPlay(g_think, false);
    else if (state == AnimState::Peace) XSoundPlay(g_peace, false);
}

void PlayCostumeChangeSFX() {
    int idx = g_costumeChangeIndex % 3;
    g_costumeChangeIndex = (g_costumeChangeIndex + 1) % 3;
    XSoundPlay(g_costume[idx], false);
}

static void PlayStartupAudio() { XSoundPlay(g_startup, false); }
static void PlayExitAudio() { XSoundPlay(g_exit, false); }

// ================= 配置读写 =================
void SaveConfig() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    wchar_t iniPath[MAX_PATH];
    wsprintfW(iniPath, L"%s\\config.ini", exePath);
    wchar_t buf[32];
    wsprintfW(buf, L"%d", (int)g_currentCostume);
    WritePrivateProfileStringW(L"State", L"Costume", buf, iniPath);
    wsprintfW(buf, L"%d", g_currentVolume);
    WritePrivateProfileStringW(L"State", L"Volume", buf, iniPath);
    wsprintfW(buf, L"%.2f", g_zoom);
    WritePrivateProfileStringW(L"State", L"Zoom", buf, iniPath);
    RECT rc; GetWindowRect(g_hwnd, &rc);
    wsprintfW(buf, L"%d", rc.left);
    WritePrivateProfileStringW(L"State", L"PosX", buf, iniPath);
    wsprintfW(buf, L"%d", rc.top);
    WritePrivateProfileStringW(L"State", L"PosY", buf, iniPath);
    wsprintfW(buf, L"%d", rc.right - rc.left);
    WritePrivateProfileStringW(L"State", L"WinW", buf, iniPath);
    wsprintfW(buf, L"%d", rc.bottom - rc.top);
    WritePrivateProfileStringW(L"State", L"WinH", buf, iniPath);
    wsprintfW(buf, L"%d", g_currentBGMIndex);
    WritePrivateProfileStringW(L"BGM", L"Index", buf, iniPath);
    wsprintfW(buf, L"%d", g_rvcOctave);
    WritePrivateProfileStringW(L"RVC", L"Octave", buf, iniPath);
}

// 解锁判定函数定义在"成就统计系统"区（要用 PetStats），这里先声明给读档逻辑用
static bool UnlockReady(int id);
static int  CostumeUnlock(int idx);
// 好感度是否已满。同样定义在"成就统计系统"区，读档逻辑要用，先声明
static bool AffinityMaxed();

void LoadConfig() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    PathRemoveFileSpecW(exePath);
    wchar_t iniPath[MAX_PATH];
    wsprintfW(iniPath, L"%s\\config.ini", exePath);
    if (!PathFileExistsW(iniPath)) return;
    wchar_t buf[32];
    GetPrivateProfileStringW(L"State", L"Costume", L"0", buf, 32, iniPath);
    {
        int ci = _wtoi(buf);
        if (ci < 0 || ci > 3) ci = 0;
        // ★ 读档时校验：累计好感不够就退回忍者服，避免"换过礼服但好感清零后还穿着"
        int uid = CostumeUnlock(ci);
        if (uid >= 0 && !UnlockReady(uid)) ci = 0;
        g_currentCostume = (Costume)ci;
    }
    GetPrivateProfileStringW(L"State", L"Volume", L"700", buf, 32, iniPath);
    g_currentVolume = _wtoi(buf);
    GetPrivateProfileStringW(L"State", L"Zoom", L"1.0", buf, 32, iniPath);
    g_zoom = (float)_wtof(buf);
    if (g_zoom < 0.5f) g_zoom = 0.5f;
    if (g_zoom > 3.0f) g_zoom = 3.0f;
    GetPrivateProfileStringW(L"State", L"PosX", L"-1", buf, 32, iniPath);
    g_savedPosX = _wtoi(buf);
    GetPrivateProfileStringW(L"State", L"PosY", L"-1", buf, 32, iniPath);
    g_savedPosY = _wtoi(buf);
    g_hasSavedPos = (g_savedPosX >= 0 && g_savedPosY >= 0);
    GetPrivateProfileStringW(L"State", L"WinW", L"", buf, 32, iniPath);
    if (buf[0] != L'\0') {
        g_currentW = _wtoi(buf);
        GetPrivateProfileStringW(L"State", L"WinH", L"", buf, 32, iniPath);
        g_currentH = _wtoi(buf);
    } else {
        g_currentW = (int)(BASE_W * g_zoom);
        g_currentH = (int)(BASE_H * g_zoom);
    }
    // 存档里的窗口尺寸若异常（旧版本写入、手改过 ini）会让窗口远大于纹理，
    // 人物被拉伸放大就会发虚 —— 超出合理范围时回退到 zoom 推算的尺寸。
    if (g_currentW < 40 || g_currentW > (int)(BASE_W * 3.2f)
        || g_currentH < 60 || g_currentH > (int)(BASE_H * 3.2f)) {
        g_currentW = (int)(BASE_W * g_zoom);
        g_currentH = (int)(BASE_H * g_zoom);
    }
    g_currentBGMIndex = GetPrivateProfileIntW(L"BGM", L"Index", 0, iniPath);
    if (g_currentBGMIndex < 0 || g_currentBGMIndex >= (int)g_bgmFileList.size()) g_currentBGMIndex = 0;
    g_rvcOctave = GetPrivateProfileIntW(L"RVC", L"Octave", 0, iniPath);
    if (g_rvcOctave < -1) g_rvcOctave = -1;
    if (g_rvcOctave > 1) g_rvcOctave = 1;
}

// ==================== 成就统计系统 ====================
// 存档：stats.ini（独立于 config.ini，清理配置不会丢失累计数据）
#include <cmath>
#define ACH_COUNT        31
// 养成解锁项并入成就表：bit 24~30 一一对应 UNLOCK_DEF[0..6]
// 这样解锁既有气泡提示，也会在成就面板里留一格，和陪伴时长类成就同等待遇
#define UNLOCK_ACH_BASE  24
#define AFFINITY_DAY_CAP 50     // 每日好感度获取上限

// ---- 好感度上限 ----
// 爱心、等级、解锁三者共用同一条刻度：占上限的百分比。
// 爱心液面 = 累计好感 / 上限；等级 = 同一比例落在哪一档。
// 三者同源，所以不可能出现"心动 Lv.5、面板却写好感 0、爱心是空的"这类自相矛盾的画面。
//
// 上限必须"够得着"：定成 500 时要满额对话 10 天，爱心长期停在两三成，
// 既没有成长感也没有终点感。取 300 与最后一项解锁（开机自启 300）对齐——
// 爱心填满的那一天正好是全功能解锁的日子，养成有明确的通关点；
// 按每日上限 50 算，最快 6 天可达，正常玩一到两周。
//
// 到达上限后停止累计：数字不再往上涨，爱心永久保持全满，
// 否则"心意已满"之后还会看到 300→487 这种没有意义的小数增长。
#define AFFINITY_CAP         300
#define AFFINITY_MAX_LEVEL   6
// 负面心情最多能压掉多少点好感（只影响爱心显示，不影响累计值与解锁）。
// 取 40 是刻意留小的：满心时被骂一次也只掉 13%，不会把爱心打空。
// 而且它只把爱心压到本档下沿为止，不会让等级跟着掉——掉级比掉爱心更打击人。
#define AFFINITY_MOOD_FLOOR  40
// 达到上限的提示只弹一次，借用 unlockAck 的空闲位（解锁项只用 bit 0~6）
#define AFFINITY_MAX_ACK_BIT 7
#define ACH_W            380
#define ACH_H            620

struct PetStats {
    double totalSeconds = 0.0;      // 累计陪伴秒数
    int    chatChars = 0;           // 累计聊天字数（只统计主人发送）
    int    coverCount = 0;          // 累计翻唱成功次数
    // 累计获得的好感度，只增不减：它是等级、爱心、解锁三者唯一的来源。
    // 不再单独维护"液面"和"等级"两个变量——那两个数和累计值会各自漂移，
    // 出现过"心动 Lv.5 但爱心全空"的自相矛盾画面。
    int    affinityTotal = 0;
    int    affinityMood = 0;        // 心情修正，负数：被敷衍/惹到时下压爱心显示
    wchar_t firstMeet[16] = { 0 };  // 首次相遇 YYYY-MM-DD
    wchar_t lastDay[16] = { 0 };    // 最近活跃日期
    int    streakDays = 0;          // 连续陪伴天数
    int    totalDays = 0;           // 累计活跃天数
    int    todayAffinity = 0;       // 今日已获得好感度
    int    nightBoot = 0;           // 曾在深夜（0~5点）陪伴
    int    mornBoot = 0;            // 曾在清晨（5~8点）陪伴
    unsigned long long unlocked = 0;// 成就位图
    unsigned int unlockAck = 0;     // 已弹过提示的解锁项位图（只影响是否再弹一次）
};
static PetStats g_stat;

struct AchDef { int bit; const wchar_t* icon; const wchar_t* name; const wchar_t* desc; };
static const AchDef ACH_DEF[ACH_COUNT] = {
    {  0, L"✿", L"初次见面",   L"和茉子相遇的第一天" },
    {  1, L"♫", L"相伴一小时", L"累计陪伴 1 小时" },
    {  2, L"♫", L"午后常客",   L"累计陪伴 10 小时" },
    {  3, L"♫", L"形影不离",   L"累计陪伴 50 小时" },
    {  4, L"♫", L"百分百陪伴", L"累计陪伴 100 小时" },
    {  5, L"✎", L"开口说话",   L"累计聊天 100 字" },
    {  6, L"✎", L"无话不谈",   L"累计聊天 1000 字" },
    {  7, L"✎", L"倾诉万千",   L"累计聊天 10000 字" },
    {  8, L"♪", L"初次开嗓",   L"完成 1 次翻唱" },
    {  9, L"♪", L"小有名气",   L"累计翻唱 10 首" },
    { 10, L"♪", L"茉子歌姬",   L"累计翻唱 50 首" },
    { 11, L"♥", L"心动初萌",   L"心动等级达到 Lv.2" },
    { 12, L"♥", L"心意相通",   L"心动等级达到 Lv.3" },
    { 13, L"♥", L"此生不渝",   L"心动等级达到 Lv.5" },
    { 14, L"☾", L"相识一周",   L"相遇满 7 天" },
    { 15, L"☾", L"相识一月",   L"相遇满 30 天" },
    { 16, L"☾", L"百日纪念",   L"相遇满 100 天" },
    { 17, L"☾", L"一周年",     L"相遇满 365 天" },
    { 18, L"✦", L"三日之约",   L"连续陪伴 3 天" },
    { 19, L"✦", L"七日之约",   L"连续陪伴 7 天" },
    { 20, L"✦", L"习惯成自然", L"连续陪伴 30 天" },
    { 21, L"★", L"深夜相伴",   L"在凌晨 0~5 点陪伴过她" },
    { 22, L"☀", L"晨间问候",   L"在清晨 5~8 点陪伴过她" },
    { 23, L"❖", L"岁月留痕",   L"累计活跃 50 天" },
    // ---- 以下 7 格 = 好感度解锁养成项（图标/名称与 UNLOCK_DEF 保持一致）----
    { 24, L"🎵", L"背景音乐",   L"她愿意为你放歌" },
    { 25, L"👘", L"日常服",     L"她换下了忍者装" },
    { 26, L"🎤", L"翻唱",       L"她愿意开口唱歌" },
    { 27, L"🎒", L"校服",       L"和你一起上学" },
    { 28, L"👗", L"礼服",       L"只为你穿的那一件" },
    { 29, L"🎚", L"翻唱变调",   L"她开始配合你" },
    { 30, L"🚀", L"开机自启",   L"每天第一个见你" },
};

// ==================== 养成：好感度解锁功能 ====================
// 判定用 affinityTotal（累计获得的好感度，只增不减）。
// 选累计值而不是当前液面，是为了"解锁了就不会再锁回去"——
// 一次冷场让已解锁的功能消失，对养成体验是纯负反馈。
enum UnlockID {
    UNLOCK_BGM = 0,        // 背景音乐
    UNLOCK_COSTUME_DAILY,  // 日常服
    UNLOCK_RVC,            // 翻唱
    UNLOCK_COSTUME_SCHOOL, // 校服
    UNLOCK_COSTUME_DRESS,  // 礼服
    UNLOCK_OCTAVE,         // 翻唱变调档位
    UNLOCK_AUTOSTART,      // 开机自启
    UNLOCK_COUNT
};

struct UnlockDef { const wchar_t* icon; const wchar_t* name; const wchar_t* desc; int need; };
static const UnlockDef UNLOCK_DEF[UNLOCK_COUNT] = {
    { L"🎵", L"背景音乐",   L"茉子愿意为你放歌了",      15  },
    { L"👘", L"日常服",     L"她换下了忍者装",          40  },
    { L"🎤", L"翻唱",       L"她愿意为你开口唱歌",      80  },
    { L"🎒", L"校服",       L"和你一起上学的样子",      110 },
    { L"👗", L"礼服",       L"只为你穿的那一件",        160 },
    { L"🎚", L"翻唱变调",   L"她开始配合你的音域",      220 },
    { L"🚀", L"开机自启",   L"她想每天第一个见到你",    300 },
};

static bool UnlockReady(int id) {
    if (id < 0 || id >= UNLOCK_COUNT) return false;
#if DEV_UNLOCK_ALL
    return true;                       // 开发人员模式：无条件开放
#else
    return g_stat.affinityTotal >= UNLOCK_DEF[id].need;
#endif
}
// 下一个待解锁项：用于菜单提示"还差多少"
static int UnlockNext(int* remainOut) {
    for (int i = 0; i < UNLOCK_COUNT; i++) {
        if (!UnlockReady(i)) { if (remainOut) *remainOut = UNLOCK_DEF[i].need - g_stat.affinityTotal; return i; }
    }
    return -1;
}
static void ToastShow(const wchar_t* icon, const wchar_t* title, const wchar_t* desc);  // 定义靠后

// 结算解锁：只对"新达成且没弹过提示"的项弹一次气泡
static void UnlockCheck() {
#if DEV_UNLOCK_ALL
    return;                            // 开发人员模式：全开，不再弹解锁气泡
#else
    for (int i = 0; i < UNLOCK_COUNT; i++) {
        if (!UnlockReady(i)) continue;
        unsigned int mask = (1u << i);
        if (g_stat.unlockAck & mask) continue;
        g_stat.unlockAck |= mask;
        wchar_t desc[80];
        swprintf(desc, 80, L"%s · 累计好感 %d", UNLOCK_DEF[i].desc, UNLOCK_DEF[i].need);
        ToastShow(UNLOCK_DEF[i].icon, UNLOCK_DEF[i].name, desc);
        break;                       // 一次只弹一个，避免连解锁时刷屏
    }
#endif
}

// 好感度触顶：爱心画满、等级封顶，弹一次纪念气泡
static void AffinityMaxCheck() {
#if DEV_UNLOCK_ALL
    return;
#else
    if (!AffinityMaxed()) return;
    unsigned int mask = (1u << AFFINITY_MAX_ACK_BIT);
    if (g_stat.unlockAck & mask) return;
    g_stat.unlockAck |= mask;
    ToastShow(L"♥", L"心意已满", L"茉子的心已经完全属于你了");
#endif
}

// 服装索引 → 需要的解锁项；0（忍者服）永远可用
static int CostumeUnlock(int idx) {
    switch (idx) {
    case 1: return UNLOCK_COSTUME_DAILY;
    case 2: return UNLOCK_COSTUME_SCHOOL;
    case 3: return UNLOCK_COSTUME_DRESS;
    default: return -1;
    }
}

// 取 stats.ini 全路径：用 GetExeDir 检查返回值，并用安全版格式化（消除 C6031）
static bool StatsIniPath(std::wstring& out) {
    std::wstring dir;
    if (!GetExeDir(dir)) return false;
    out = dir + L"\\stats.ini";
    return true;
}

static void TodayStr(wchar_t* out) {
    SYSTEMTIME st; GetLocalTime(&st);
    wsprintfW(out, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
}

static bool WstrToSystemTime(const wchar_t* s, SYSTEMTIME& st) {
    if (!s || wcslen(s) < 10) return false;
    int y = 0, m = 0, d = 0;
    if (swscanf_s(s, L"%d-%d-%d", &y, &m, &d) != 3) return false;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    ZeroMemory(&st, sizeof(st));
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)d;
    return true;
}

// 两个 YYYY-MM-DD 之间相差的天数（a 早于 b 时为正）
static int DaysBetween(const wchar_t* a, const wchar_t* b) {
    SYSTEMTIME sa, sb;
    if (!WstrToSystemTime(a, sa) || !WstrToSystemTime(b, sb)) return 0;
    FILETIME fa, fb;
    SystemTimeToFileTime(&sa, &fa);
    SystemTimeToFileTime(&sb, &fb);
    ULARGE_INTEGER ua, ub;
    ua.LowPart = fa.dwLowDateTime;  ua.HighPart = fa.dwHighDateTime;
    ub.LowPart = fb.dwLowDateTime;  ub.HighPart = fb.dwHighDateTime;
    if (ub.QuadPart < ua.QuadPart) return 0;
    return (int)((ub.QuadPart - ua.QuadPart) / (10ULL * 1000 * 1000 * 60 * 60 * 24));
}

static int DaysSinceFirstMeet() {
    if (g_stat.firstMeet[0] == 0) return 0;
    wchar_t today[16]; TodayStr(today);
    return DaysBetween(g_stat.firstMeet, today);
}

static void StatsLoad() {
    std::wstring ini;
    if (!StatsIniPath(ini)) return;
    wchar_t b[64];
    GetPrivateProfileStringW(L"Stats", L"TotalSeconds", L"0", b, 64, ini.c_str());
    g_stat.totalSeconds = _wtof(b);
    g_stat.chatChars   = GetPrivateProfileIntW(L"Stats", L"ChatChars", 0, ini.c_str());
    g_stat.coverCount  = GetPrivateProfileIntW(L"Stats", L"CoverCount", 0, ini.c_str());
    // 旧存档里的 Affinity（液面）和 AffinityLevel 已不再读取：
    // 二者现在都由 AffinityTotal 推导，读进来反而会和累计值对不上。
    // 累计值本身一直和液面同步累加，所以不读也不会丢进度。
    g_stat.affinityTotal = GetPrivateProfileIntW(L"Stats", L"AffinityTotal", 0, ini.c_str());
    g_stat.affinityMood  = GetPrivateProfileIntW(L"Stats", L"AffinityMood", 0, ini.c_str());
    GetPrivateProfileStringW(L"Stats", L"FirstMeet", L"", b, 64, ini.c_str());
    if (b[0]) wcsncpy_s(g_stat.firstMeet, 16, b, 15);
    GetPrivateProfileStringW(L"Stats", L"LastDay", L"", b, 64, ini.c_str());
    if (b[0]) wcsncpy_s(g_stat.lastDay, 16, b, 15);
    g_stat.streakDays    = GetPrivateProfileIntW(L"Stats", L"StreakDays", 0, ini.c_str());
    g_stat.totalDays     = GetPrivateProfileIntW(L"Stats", L"TotalDays", 0, ini.c_str());
    g_stat.todayAffinity = GetPrivateProfileIntW(L"Stats", L"TodayAffinity", 0, ini.c_str());
    g_stat.nightBoot     = GetPrivateProfileIntW(L"Stats", L"NightBoot", 0, ini.c_str());
    g_stat.mornBoot      = GetPrivateProfileIntW(L"Stats", L"MornBoot", 0, ini.c_str());
    GetPrivateProfileStringW(L"Stats", L"Unlocked", L"0", b, 64, ini.c_str());
    g_stat.unlocked = _wcstoui64(b, nullptr, 16);
    g_stat.unlockAck = (unsigned int)GetPrivateProfileIntW(L"Stats", L"UnlockAck", 0, ini.c_str());
    if (g_stat.affinityTotal < 0) g_stat.affinityTotal = 0;
    // 上限调小过（500 → 300）。老存档里超过上限的累计值收敛到满值：
    // 不截断的话爱心会画爆、面板也会显示"好感 480 / 300"这种怪数字。
    if (g_stat.affinityTotal > AFFINITY_CAP) g_stat.affinityTotal = AFFINITY_CAP;
    if (g_stat.todayAffinity < 0) g_stat.todayAffinity = 0;
    if (g_stat.todayAffinity > AFFINITY_DAY_CAP) g_stat.todayAffinity = AFFINITY_DAY_CAP;
    if (g_stat.affinityMood > 0) g_stat.affinityMood = 0;
    if (g_stat.affinityMood < -AFFINITY_MOOD_FLOOR) g_stat.affinityMood = -AFFINITY_MOOD_FLOOR;
}

static void StatsSave() {
    std::wstring ini;
    if (!StatsIniPath(ini)) return;
    wchar_t b[64];
    swprintf(b, 64, L"%.1f", g_stat.totalSeconds);
    WriteIniDbl(L"Stats", L"TotalSeconds", g_stat.totalSeconds, ini.c_str());
    WriteIniInt(L"Stats", L"ChatChars", g_stat.chatChars, ini.c_str());
    wsprintfW(b, L"%d", g_stat.coverCount);           WritePrivateProfileStringW(L"Stats", L"CoverCount", b, ini.c_str());
    WriteIniInt(L"Stats", L"AffinityMood", g_stat.affinityMood, ini.c_str());
    wsprintfW(b, L"%d", g_stat.affinityTotal);        WritePrivateProfileStringW(L"Stats", L"AffinityTotal", b, ini.c_str());
    WriteIniStr(L"Stats", L"FirstMeet", g_stat.firstMeet, ini.c_str());
    WriteIniStr(L"Stats", L"LastDay", g_stat.lastDay, ini.c_str());
    wsprintfW(b, L"%d", g_stat.streakDays);           WritePrivateProfileStringW(L"Stats", L"StreakDays", b, ini.c_str());
    WriteIniInt(L"Stats", L"TotalDays", g_stat.totalDays, ini.c_str());
    wsprintfW(b, L"%d", g_stat.todayAffinity);        WritePrivateProfileStringW(L"Stats", L"TodayAffinity", b, ini.c_str());
    WriteIniInt(L"Stats", L"NightBoot", g_stat.nightBoot, ini.c_str());
    WriteIniInt(L"Stats", L"MornBoot", g_stat.mornBoot, ini.c_str());
    swprintf(b, 64, L"%llx", g_stat.unlocked);
    WriteIniStr(L"Stats", L"Unlocked", b, ini.c_str());
    WriteIniInt(L"Stats", L"UnlockAck", (int)g_stat.unlockAck, ini.c_str());
}

// 跨天处理：重置每日好感度、更新连续天数与累计天数
static void StatsCheckDay() {
    wchar_t today[16]; TodayStr(today);
    if (g_stat.firstMeet[0] == 0) {
        wcsncpy_s(g_stat.firstMeet, 16, today, 15);
        g_stat.totalDays = 1;
        g_stat.streakDays = 1;
    } else if (wcscmp(g_stat.lastDay, today) != 0) {
        int gap = DaysBetween(g_stat.lastDay, today);
        if (gap == 1) g_stat.streakDays += 1;
        else if (gap > 1) g_stat.streakDays = 1;
        g_stat.totalDays += 1;
        g_stat.todayAffinity = 0;
    }
    wcsncpy_s(g_stat.lastDay, 16, today, 15);
    SYSTEMTIME st; GetLocalTime(&st);
    if (st.wHour < 5) g_stat.nightBoot = 1;
    if (st.wHour >= 5 && st.wHour < 8) g_stat.mornBoot = 1;
}

// ---- 好感度的派生量：等级、显示值、是否已满 ----
// 全部由 affinityTotal 单一来源推导，不各自维护变量，杜绝互相矛盾。

// 心动等级：按"占上限的比例"分档，和爱心用的是同一条刻度。
// Lv.1 = 0~19%，Lv.2 = 20~39% …… Lv.5 = 80~99%，Lv.MAX = 满格。
// 这样等级与爱心永远落在同一区间，不可能出现"Lv.5 却几乎是空心"。
// 末档单独留给满值：只有真正填满才是 Lv.MAX，99% 仍显示 Lv.5。
static int AffinityLevel() {
    if (g_stat.affinityTotal >= AFFINITY_CAP) return AFFINITY_MAX_LEVEL;
    int lv = g_stat.affinityTotal * (AFFINITY_MAX_LEVEL - 1) / AFFINITY_CAP + 1;
    if (lv < 1) lv = 1;
    if (lv > AFFINITY_MAX_LEVEL - 1) lv = AFFINITY_MAX_LEVEL - 1;
    return lv;
}
// 本档下沿：负面心情最多把爱心压到这里为止。
// 有了这条下限，"Lv.N"对应的爱心区间就是固定的，被骂一次也不会掉进上一档。
static int AffinityLevelFloor() {
    if (g_stat.affinityTotal >= AFFINITY_CAP) return AFFINITY_CAP;   // 满心之后不再回落
    return (AffinityLevel() - 1) * AFFINITY_CAP / (AFFINITY_MAX_LEVEL - 1);
}
// 爱心里实际画多少：累计值加心情修正，夹在 [本档下沿, 上限]
static int AffinityShown() {
    int v = g_stat.affinityTotal + g_stat.affinityMood;
    int lo = AffinityLevelFloor();
    if (v < lo) v = lo;
    if (v > AFFINITY_CAP) v = AFFINITY_CAP;
    if (v < 0) v = 0;
    return v;
}
// 是否已经把心填满了（用累计值判定，心情不好不算退步）
static bool AffinityMaxed() { return g_stat.affinityTotal >= AFFINITY_CAP; }

// 增加好感度（受每日上限约束）。返回值=是否真的加进去了
static bool StatsAddAffinity(int n) {
    if (n <= 0) return false;
    // 已经填满就停止累计：数字停在满值，爱心永久全满。
    // 否则"心意已满"之后还会看到 300 → 487 这种没有意义的小数增长。
    if (AffinityMaxed()) return false;
    int room = AFFINITY_DAY_CAP - g_stat.todayAffinity;
    if (room <= 0) return false;
    if (n > room) n = room;
    // 别越过上限：最后一次只补到满，多出来的部分丢掉
    if (g_stat.affinityTotal + n > AFFINITY_CAP) n = AFFINITY_CAP - g_stat.affinityTotal;
    if (n <= 0) return false;
    g_stat.todayAffinity += n;
    g_stat.affinityTotal += n;
    // 正分先抵消负面心情，抵消完剩下的才是真实的亲密度增长
    if (g_stat.affinityMood < 0) {
        g_stat.affinityMood += n;
        if (g_stat.affinityMood > 0) g_stat.affinityMood = 0;
    }
    return true;
}

// 对话好感度：由模型返回的增减值驱动，是养成的决定性指标。
// 负分只压心情（影响爱心显示），不动 affinityTotal ——
// 累计值只增，解锁永不回退，爱心也不会被扣空。
// 返回 true 表示本轮确实"获得"了好感（用于区分"没加分"和"今日已达上限"）。
static bool StatsAddAffinityChat(int delta) {
    if (delta == 0) return false;
    if (delta < 0) {
        g_stat.affinityMood += delta;
        if (g_stat.affinityMood < -AFFINITY_MOOD_FLOOR) g_stat.affinityMood = -AFFINITY_MOOD_FLOOR;
        return false;
    }
    return StatsAddAffinity(delta);
}

static void FmtDuration(double sec, wchar_t* out, size_t n) {
    int h = (int)(sec / 3600.0);
    int m = (int)((sec - h * 3600.0) / 60.0);
    if (h > 0) swprintf(out, n, L"%d 小时 %d 分", h, m);
    else swprintf(out, n, L"%d 分钟", m);
}


// ---- 心形液体绘制 ----
static void BuildHeartPath(GraphicsPath& path, float cx, float cy, float w, float h) {
    const int N = 96;
    std::vector<PointF> pts(N);   // 放堆上：96 个 PointF 约 768B，避免 /analyze C6262 栈用量告警
    for (int i = 0; i < N; i++) {
        double t = 6.28318530717958 * i / N;
        double x = 16.0 * pow(sin(t), 3.0);
        double y = 13.0 * cos(t) - 5.0 * cos(2 * t) - 2.0 * cos(3 * t) - cos(4 * t);
        pts[i].X = cx + (float)(x * w / 32.0);
        pts[i].Y = cy - (float)((y + 2.5) * h / 30.0);
    }
    path.AddPolygon(pts.data(), N);
}

// pct: 0~1 液面高度；phase: 波动相位（弧度）
static void DrawHeartLiquid(Graphics& g, float cx, float cy, float w, float h, float pct, float phase) {
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;
    GraphicsPath heart;
    BuildHeartPath(heart, cx, cy, w, h);
    RectF bb; heart.GetBounds(&bb);

    SolidBrush empty(Color(255, 252, 244, 248));
    g.FillPath(&empty, &heart);

    float surfaceY = bb.Y + bb.Height * (1.0f - pct);
    const int N = 48;
    std::vector<PointF> wave(N + 3);   // 同上，避免栈上大数组
    for (int i = 0; i <= N; i++) {
        float x = bb.X + bb.Width * (float)i / N;
        float y = surfaceY + sinf(6.2831853f * 1.5f * i / N + phase) * (bb.Height * 0.013f);
        wave[i] = PointF(x, y);
    }
    wave[N + 1] = PointF(bb.GetRight() + 4, bb.GetBottom() + 4);
    wave[N + 2] = PointF(bb.X - 4, bb.GetBottom() + 4);
    GraphicsPath liquid;
    liquid.AddPolygon(wave.data(), N + 3);

    g.SetClip(&heart);
    LinearGradientBrush br(PointF(bb.X, surfaceY), PointF(bb.X, bb.GetBottom()),
        Color(255, 255, 138, 186), Color(255, 236, 72, 132));
    g.FillPath(&br, &liquid);
    // 液面高光带
    if (pct > 0.001f && pct < 0.999f) {
        Pen surf(Color(120, 255, 255, 255), 1.6f);
        for (int i = 0; i < N; i++) g.DrawLine(&surf, wave[i], wave[i + 1]);
    }
    g.ResetClip();

    // 填满是养成的终点，画面上要能一眼看出来：描边转金色并加一圈柔光
    bool full = (pct >= 0.999f);
    if (full) {
        Pen glow(Color(90, 255, 200, 90), 8.0f);
        g.DrawPath(&glow, &heart);
    }
    Pen outline(full ? Color(255, 255, 208, 96) : Color(200, 226, 74, 122),
                full ? 3.0f : 2.2f);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.DrawPath(&outline, &heart);

    SolidBrush hl(Color(150, 255, 255, 255));
    g.FillEllipse(&hl, cx - w * 0.24f, cy - h * 0.30f, w * 0.15f, h * 0.11f);
}

// ---- 成就解锁气泡（独立分层窗口） ----
#define TOAST_W 330
#define TOAST_H 78
#define ID_ACH_PANEL 5001

static HWND g_hToast = nullptr;
static int  g_toastBit = -1;
// ★ 通用气泡内容：成就只是其中一种，养成解锁也复用同一个气泡窗口
static std::wstring g_toastIcon, g_toastTitle, g_toastDesc;
static int  g_toastAlpha = 0;
static int  g_toastPhase = 0;     // 0 淡入 1 停留 2 淡出
static int  g_toastTick = 0;
static int  g_toastQueue[ACH_COUNT];
static int  g_toastQHead = 0, g_toastQTail = 0;
static HDC     g_toastDC = nullptr;
static HBITMAP g_toastBmp = nullptr, g_toastOld = nullptr;

static void ToastEnqueue(int bit) {
    if (g_toastQTail < ACH_COUNT) g_toastQueue[g_toastQTail++] = bit;
}

static void ToastPaint() {
    if (!g_hToast || g_toastBit < 0) return;
    if (!g_toastDC) {
        HDC sc = GetDC(nullptr);
        g_toastDC = CreateCompatibleDC(sc);
        g_toastBmp = CreateCompatibleBitmap(sc, TOAST_W, TOAST_H);
        ReleaseDC(nullptr, sc);
        g_toastOld = (HBITMAP)SelectObject(g_toastDC, g_toastBmp);
    }
    Graphics g(g_toastDC);
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // 用通用字段而不是 ACH_DEF：解锁提示不属于成就表
    const std::wstring icon = g_toastIcon.empty() ? std::wstring(L"\u2730") : g_toastIcon;
    const std::wstring tname = g_toastTitle;
    const std::wstring tdesc = g_toastDesc;
    int pad = 5, r = 12;
    GraphicsPath bg;
    bg.AddArc((float)pad, (float)pad, (float)(2 * r), (float)(2 * r), 180, 90);
    bg.AddArc((float)(TOAST_W - pad - 2 * r), (float)pad, (float)(2 * r), (float)(2 * r), 270, 90);
    bg.AddArc((float)(TOAST_W - pad - 2 * r), (float)(TOAST_H - pad - 2 * r), (float)(2 * r), (float)(2 * r), 0, 90);
    bg.AddArc((float)pad, (float)(TOAST_H - pad - 2 * r), (float)(2 * r), (float)(2 * r), 90, 90);
    bg.CloseFigure();
    SolidBrush bgb(Color(232, 38, 32, 48));
    g.FillPath(&bgb, &bg);
    Pen border(Color(255, 236, 190, 120), 1.8f);
    g.DrawPath(&border, &bg);

    Font fTitle(L"Microsoft YaHei", 14, FontStyleBold);
    Font fDesc(L"Microsoft YaHei", 10);
    Font fIcon(L"Microsoft YaHei", 22, FontStyleBold);
    SolidBrush gold(Color(255, 255, 208, 96));
    SolidBrush white(Color(255, 245, 242, 250));
    SolidBrush gray(Color(220, 198, 190, 205));
    StringFormat sf; sf.SetAlignment(StringAlignmentNear); sf.SetLineAlignment(StringAlignmentCenter);
    RectF iconR((float)pad + 12, (float)pad + 8, 40, (float)(TOAST_H - 2 * pad - 16));
    g.DrawString(icon.c_str(), -1, &fIcon, iconR, &sf, &gold);
    RectF tR((float)pad + 58, (float)pad + 14, (float)(TOAST_W - 100), 22);
    g.DrawString(tname.c_str(), -1, &fTitle, tR, &sf, &white);
    wchar_t line[80]; swprintf(line, 80, L"%s", tdesc.c_str());
    RectF dR((float)pad + 58, (float)pad + 38, (float)(TOAST_W - 80), 20);
    g.DrawString(line, -1, &fDesc, dR, &sf, &gray);

    POINT ptSrc = { 0, 0 };
    RECT rc; GetWinRect(g_hToast, rc);
    POINT ptWin = { rc.left, rc.top };
    SIZE sz = { TOAST_W, TOAST_H };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)g_toastAlpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hToast, nullptr, &ptWin, &sz, g_toastDC, &ptSrc, RGB(0, 0, 0), &bf, ULW_ALPHA);
}

static void ToastDestroy() {
    if (g_hToast) { DestroyWindow(g_hToast); g_hToast = nullptr; }
    if (g_toastDC) {
        SelectObject(g_toastDC, g_toastOld);
        DeleteObject(g_toastBmp); DeleteDC(g_toastDC);
        g_toastDC = nullptr; g_toastBmp = nullptr;
    }
    g_toastBit = -1; g_toastAlpha = 0; g_toastPhase = 0; g_toastTick = 0;
}

// 通用气泡：任意 icon/标题/描述都能弹
static void ToastShow(const wchar_t* icon, const wchar_t* title, const wchar_t* desc) {
    if (g_hToast) ToastDestroy();
    g_toastIcon  = icon  ? icon  : L"";
    g_toastTitle = title ? title : L"";
    g_toastDesc  = desc  ? desc  : L"";
    // g_toastBit 兼作"气泡是否在展示"的哨兵，必须 >= 0，否则 ToastTick 会直接返回；
    // 内容一律读 g_toastIcon/Title/Desc，不再碰 ACH_DEF。
    g_toastBit = 0;
    g_toastAlpha = 0; g_toastPhase = 0; g_toastTick = 0;
    g_toastQHead = g_toastQTail = 0;       // 解锁提示优先，清掉排队中的成就
    g_hToast = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        L"MakotoToast", L"", WS_POPUP, 0, 0, TOAST_W, TOAST_H, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!g_hToast) { g_toastBit = -1; return; }
    RECT rc; GetWinRect(g_hwnd, rc);
    int x = GetSystemMetrics(SM_CXSCREEN) - TOAST_W - 20;
    int y = rc.top - TOAST_H - 12;
    if (y < 10) y = rc.bottom + 12;
    SetWindowPos(g_hToast, HWND_TOPMOST, x, y, TOAST_W, TOAST_H, SWP_NOACTIVATE);
    ShowWindow(g_hToast, SW_SHOWNA);
    ToastPaint();
}

static void ToastStart(int bit) {
    if (g_hToast) ToastDestroy();
    int uid = bit - UNLOCK_ACH_BASE;
    if (bit >= UNLOCK_ACH_BASE && uid < UNLOCK_COUNT) {
        // 养成解锁项：标题改"功能解锁"，描述带达成所需累计好感
        g_toastIcon  = UNLOCK_DEF[uid].icon;
        g_toastTitle = L"功能解锁";
        wchar_t d[96];
        swprintf(d, 96, L"%s · 累计好感 %d", UNLOCK_DEF[uid].desc, UNLOCK_DEF[uid].need);
        g_toastDesc  = d;
    } else {
        g_toastIcon  = ACH_DEF[bit].icon;
        g_toastTitle = L"成就解锁";
        g_toastDesc  = std::wstring(ACH_DEF[bit].name) + L" · " + ACH_DEF[bit].desc;
    }
    g_toastBit = bit;
    g_toastAlpha = 0; g_toastPhase = 0; g_toastTick = 0;
    g_hToast = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        L"MakotoToast", L"", WS_POPUP, 0, 0, TOAST_W, TOAST_H, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!g_hToast) { g_toastBit = -1; return; }
    RECT rc; GetWinRect(g_hwnd, rc);
    int x = GetSystemMetrics(SM_CXSCREEN) - TOAST_W - 20;
    int y = rc.top - TOAST_H - 12;
    if (y < 10) y = rc.bottom + 12;
    SetWindowPos(g_hToast, HWND_TOPMOST, x, y, TOAST_W, TOAST_H, SWP_NOACTIVATE);
    ShowWindow(g_hToast, SW_SHOWNA);
    ToastPaint();
}

// 由主窗口 WM_TIMER 每帧推进（30fps）
static void ToastTick() {
    if (g_toastBit < 0) {
        if (g_toastQHead < g_toastQTail) ToastStart(g_toastQueue[g_toastQHead++]);
        return;
    }
    g_toastTick++;
    if (g_toastPhase == 0) {
        g_toastAlpha += 255 / 8;
        if (g_toastAlpha >= 255) { g_toastAlpha = 255; g_toastPhase = 1; g_toastTick = 0; }
        ToastPaint();
    } else if (g_toastPhase == 1) {
        if (g_toastTick > FPS * 3) { g_toastPhase = 2; g_toastTick = 0; }
    } else {
        g_toastAlpha -= 255 / 8;
        if (g_toastAlpha <= 0) { ToastDestroy(); return; }
        ToastPaint();
    }
}


// ---- 成就判定 ----
static bool AchCond(int bit) {
    switch (bit) {
    case 0:  return true;
    case 1:  return g_stat.totalSeconds >= 3600.0;
    case 2:  return g_stat.totalSeconds >= 36000.0;
    case 3:  return g_stat.totalSeconds >= 180000.0;
    case 4:  return g_stat.totalSeconds >= 360000.0;
    case 5:  return g_stat.chatChars >= 100;
    case 6:  return g_stat.chatChars >= 1000;
    case 7:  return g_stat.chatChars >= 10000;
    case 8:  return g_stat.coverCount >= 1;
    case 9:  return g_stat.coverCount >= 10;
    case 10: return g_stat.coverCount >= 50;
    // 心动等级按占上限的比例分档（上限 300）：Lv.2 = 60，Lv.3 = 120，Lv.5 = 240
    case 11: return AffinityLevel() >= 2;
    case 12: return AffinityLevel() >= 3;
    case 13: return AffinityLevel() >= 5;
    case 14: return DaysSinceFirstMeet() >= 7;
    case 15: return DaysSinceFirstMeet() >= 30;
    case 16: return DaysSinceFirstMeet() >= 100;
    case 17: return DaysSinceFirstMeet() >= 365;
    case 18: return g_stat.streakDays >= 3;
    case 19: return g_stat.streakDays >= 7;
    case 20: return g_stat.streakDays >= 30;
    case 21: return g_stat.nightBoot != 0;
    case 22: return g_stat.mornBoot != 0;
    case 23: return g_stat.totalDays >= 50;
    }
    // 养成解锁项：按真实累计好感判定（不走 UnlockReady，
    // 这样 DEV_UNLOCK_ALL 开功能的同时，面板进度仍如实反映养成）
    if (bit >= UNLOCK_ACH_BASE && bit < ACH_COUNT) {
        int id = bit - UNLOCK_ACH_BASE;
        if (id >= 0 && id < UNLOCK_COUNT) return g_stat.affinityTotal >= UNLOCK_DEF[id].need;
    }
    return false;
}

static void AchCheck() {
    bool any = false;
    for (int i = 0; i < ACH_COUNT; i++) {
        unsigned long long mask = (1ULL << i);
        if (g_stat.unlocked & mask) continue;
        if (AchCond(i)) {
            g_stat.unlocked |= mask;
            // 养成解锁项的气泡交给 UnlockCheck（文案带"累计好感 N"，信息更全），
            // 这里只点亮成就格，避免同一个解锁弹两个气泡
            if (i < UNLOCK_ACH_BASE) ToastEnqueue(i);
            any = true;
        }
    }
    if (any) StatsSave();
}

// ---- 成就面板 ----
static HWND  g_hAchPanel = nullptr;
static int   g_achScroll = 0;
static float g_achWave = 0.0f;

static void FillRoundRect(Graphics& g, const RectF& r, float rad, Color c) {
    GraphicsPath p; float d = rad * 2.0f;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
    SolidBrush b(c); g.FillPath(&b, &p);
}

// 面板右侧就是窗口边界，长文案按原字号画会被裁掉半句：
// 先量一次，超宽就逐级降字号，保证整句完整显示（最低 8pt，再小就看不清了）。
static void DrawFitString(Graphics& g, const wchar_t* text, const RectF& r,
    float emSize, int style, const StringFormat* sf, Brush* br)
{
    if (r.Width <= 1.0f) return;
    FontFamily fam(L"Microsoft YaHei");
    float s = emSize;
    for (;;) {
        Font f(&fam, s, style, UnitPoint);
        RectF need;
        g.MeasureString(text, -1, &f, PointF(0, 0), &need);
        if (need.Width <= r.Width || s <= 8.0f) {
            g.DrawString(text, -1, &f, r, sf, br);
            return;
        }
        s -= 0.5f;
    }
}

static void AchPanelPaint(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    SolidBrush bk(Color(255, 28, 24, 40));
    g.FillRectangle(&bk, 0, 0, rc.right, rc.bottom);

    Font fName(L"Microsoft YaHei", 11, FontStyleBold);
    Font fSmall(L"Microsoft YaHei", 9);
    Font fBig(L"Microsoft YaHei", 18, FontStyleBold);
    Font fDesc(L"Microsoft YaHei", 8);
    SolidBrush white(Color(255, 240, 238, 245));
    SolidBrush gray(Color(200, 150, 145, 160));
    SolidBrush gold(Color(255, 255, 205, 105));
    SolidBrush pink(Color(255, 255, 150, 190));
    StringFormat sfC; sfC.SetAlignment(StringAlignmentCenter); sfC.SetLineAlignment(StringAlignmentCenter);
    StringFormat sfL; sfL.SetAlignment(StringAlignmentNear);   sfL.SetLineAlignment(StringAlignmentCenter);

    RectF titleR(0, 6, (float)rc.right, 30);
    g.DrawString(L"♥ 茉子的回忆手册", -1, &fBig, titleR, &sfC, &pink);

    // 爱心画的是总进度（累计好感 / 上限），不是"本级液面"：
    // 前者只会越画越满，后者每升一级就清零，会出现等级很高却空心的画面。
    float pct = (float)AffinityShown() / (float)AFFINITY_CAP;
    DrawHeartLiquid(g, 92.0f, 118.0f, 132.0f, 124.0f, pct, g_achWave);

    wchar_t lv[32];
    if (AffinityMaxed()) swprintf(lv, 32, L"Lv.MAX");
    else                 swprintf(lv, 32, L"Lv.%d", AffinityLevel());
    RectF lvR(180, 74, 170, 34);
    g.DrawString(lv, -1, &fBig, lvR, &sfL, &gold);
    wchar_t af[48];
    // 括号里用"显示值 - 累计值"而不是心情原值：爱心有本档下沿保护，
    // 心情 -40 时实际可能只掉了 10 点，直接印 -40 会和左边的数字对不上。
    int drop = AffinityShown() - g_stat.affinityTotal;
    if (AffinityMaxed())    swprintf(af, 48, L"好感度 %d/%d · 心意已满", AFFINITY_CAP, AFFINITY_CAP);
    else if (drop < 0)      swprintf(af, 48, L"好感度 %d/%d（心情 %d）", AffinityShown(), AFFINITY_CAP, drop);
    else                    swprintf(af, 48, L"好感度 %d/%d", AffinityShown(), AFFINITY_CAP);
    // 右边界贴着客户区算出来，不再写死 190：窗口边框厚度随系统主题变化，
    // 写死会让这句在部分机器上顶到边被裁掉。
    RectF afR(180, 110, (float)rc.right - 188.0f, 22);
    DrawFitString(g, af, afR, 11, FontStyleBold, &sfL, &white);
    // 满心之后今日计数不再增长（累计已停止），再显示"今日 +30 / 50"会让人以为还没到头，
    // 换成通关文案——上限 300 正好是最后一项解锁的门槛，两件事同一天达成。
    wchar_t tg[40];
    if (AffinityMaxed()) swprintf(tg, 40, L"全部功能已解锁");
    else                 swprintf(tg, 40, L"今日 +%d / %d", g_stat.todayAffinity, AFFINITY_DAY_CAP);
    RectF tgR(180, 134, (float)rc.right - 188.0f, 20);
    DrawFitString(g, tg, tgR, 9, FontStyleRegular, &sfL, &gray);
    wchar_t mt[48]; swprintf(mt, 48, L"相遇 %d 天 · 连续 %d 天", DaysSinceFirstMeet(), g_stat.streakDays);
    RectF mtR(180, 156, (float)rc.right - 188.0f, 20);
    DrawFitString(g, mt, mtR, 9, FontStyleRegular, &sfL, &gray);

    // 四格数据
    wchar_t dur[48]; FmtDuration(g_stat.totalSeconds, dur, 48);
    const wchar_t* labels[4] = { L"陪伴时长", L"聊天字数", L"翻唱曲目", L"活跃天数" };
    wchar_t vals[4][48];
    wcsncpy_s(vals[0], 48, dur, 47);
    swprintf(vals[1], 48, L"%d 字", g_stat.chatChars);
    swprintf(vals[2], 48, L"%d 首", g_stat.coverCount);
    swprintf(vals[3], 48, L"%d 天", g_stat.totalDays);
    for (int i = 0; i < 4; i++) {
        float cx = 14.0f + (i % 2) * 180.0f;
        float cy = 208.0f + (i / 2) * 54.0f;
        RectF card(cx, cy, 168.0f, 46.0f);
        FillRoundRect(g, card, 10, Color(255, 44, 38, 58));
        RectF lr(cx + 10, cy + 4, 148, 18);
        g.DrawString(labels[i], -1, &fSmall, lr, &sfL, &gray);
        RectF vr(cx + 10, cy + 22, 148, 20);
        // 陪伴时长会变成"2 天 3 小时 45 分钟"这种长串，同样走自适应
        DrawFitString(g, vals[i], vr, 11, FontStyleBold, &sfL, &white);
    }

    // 成就列表（两列，可滚动）
    int viewTop = 322, viewBot = rc.bottom - 10;
    int viewH = viewBot - viewTop;
    // 前段成就 + 一行分组标题 + 后段养成解锁
    int rows = (UNLOCK_ACH_BASE + 1) / 2 + 1 + (ACH_COUNT - UNLOCK_ACH_BASE + 1) / 2;
    int rowH = 40;
    int contentH = rows * rowH + 8;
    int maxScroll = contentH - viewH; if (maxScroll < 0) maxScroll = 0;
    if (g_achScroll < 0) g_achScroll = 0;
    if (g_achScroll > maxScroll) g_achScroll = maxScroll;

    RectF listR(6, (float)viewTop - 6, (float)rc.right - 12, (float)viewH);
    FillRoundRect(g, listR, 10, Color(255, 34, 30, 48));

    g.SetClip(RectF(8, (float)viewTop, (float)rc.right - 16, (float)viewH - 4));
    // 行号：成就占前 12 行，第 13 行放分组标题，解锁项从其后开始
    auto rowOf = [](int i) -> int {
        if (i < UNLOCK_ACH_BASE) return i / 2;
        return (UNLOCK_ACH_BASE + 1) / 2 + 1 + (i - UNLOCK_ACH_BASE) / 2;
    };
    for (int i = 0; i < ACH_COUNT; i++) {
        int col = i % 2, row = rowOf(i);
        float ix = 14.0f + col * 180.0f;
        float iy = (float)viewTop + row * rowH - (float)g_achScroll;
        bool got = (g_stat.unlocked & (1ULL << i)) != 0;
        RectF item(ix, iy, 168.0f, 36.0f);
        FillRoundRect(g, item, 8, got ? Color(255, 62, 46, 74) : Color(255, 42, 38, 54));
        const Brush* ib = got ? (const Brush*)&gold : (const Brush*)&gray;
        const Brush* nb = got ? (const Brush*)&white : (const Brush*)&gray;
        RectF ic(ix + 6, iy + 2, 26, 32);
        g.DrawString(ACH_DEF[i].icon, -1, &fName, ic, &sfC, ib);
        RectF nr(ix + 34, iy + 3, 128, 17);
        g.DrawString(ACH_DEF[i].name, -1, &fSmall, nr, &sfL, nb);
        RectF dr(ix + 34, iy + 19, 128, 15);
        // 养成解锁项未达成时，把静态说明换成"还差 N 好感"，让进度一目了然
        if (i >= UNLOCK_ACH_BASE && !got) {
            int id = i - UNLOCK_ACH_BASE;
            wchar_t nd[64];
#if DEV_UNLOCK_ALL
            swprintf(nd, 64, L"好感 %d / %d · DEV 已开放", g_stat.affinityTotal, UNLOCK_DEF[id].need);
#else
            swprintf(nd, 64, L"好感 %d / %d", g_stat.affinityTotal, UNLOCK_DEF[id].need);
#endif
            g.DrawString(nd, -1, &fDesc, dr, &sfL, &pink);
        } else {
            g.DrawString(ACH_DEF[i].desc, -1, &fDesc, dr, &sfL, &gray);
        }
    }
    // 分组标题
    {
        float ty = (float)viewTop + ((UNLOCK_ACH_BASE + 1) / 2) * rowH - (float)g_achScroll;
        RectF tr(14.0f, ty + 6, (float)rc.right - 28, 22);
        g.DrawString(L"♥ 好感度解锁", -1, &fSmall, tr, &sfL, &pink);
    }
    g.ResetClip();

    int unlockedCount = 0;
    for (int i = 0; i < ACH_COUNT; i++) if (g_stat.unlocked & (1ULL << i)) unlockedCount++;
    wchar_t prog[48]; swprintf(prog, 48, L"成就 %d / %d", unlockedCount, ACH_COUNT);
    RectF pr(0, (float)viewTop - 26, (float)rc.right, 20);
    g.DrawString(prog, -1, &fSmall, pr, &sfC, &gray);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK AchPanelProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 40, nullptr);
        return 0;
    case WM_TIMER:
        g_achWave += 0.12f;
        if (g_achWave > 1000.0f) g_achWave -= 1000.0f;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        g_achScroll -= (d > 0 ? 40 : -40);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_PAINT:
        AchPanelPaint(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        g_hAchPanel = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void OpenAchPanel() {
    if (g_hAchPanel && IsWindow(g_hAchPanel)) { SetForegroundWindow(g_hAchPanel); return; }
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = AchPanelProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MakotoAchPanel";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    int x = (GetSystemMetrics(SM_CXSCREEN) - ACH_W) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - ACH_H) / 2;
    g_hAchPanel = CreateWindowExW(WS_EX_TOPMOST, L"MakotoAchPanel", L"🏆 成就与回忆",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_MINIMIZEBOX,
        x, y, ACH_W, ACH_H, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// 气泡窗口过程（空实现即可，不需要交互）
static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProcW(hwnd, msg, wp, lp);
}



bool IsAutoStartEnabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;
    wchar_t val[MAX_PATH] = { 0 };
    DWORD type = 0, size = sizeof(val);
    bool ok = (RegQueryValueExW(hKey, L"MakotoPet", nullptr, &type, (LPBYTE)val, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return ok;
}

void SetAutoStart(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    if (enable) {
        std::wstring exePath;
        if (GetExeDir(exePath))
            RegSetValueExW(hKey, L"MakotoPet", 0, REG_SZ, (const BYTE*)exePath.c_str(),
                           (DWORD)(exePath.size() + 1) * sizeof(wchar_t));
    } else {
        RegDeleteValueW(hKey, L"MakotoPet");
    }
    RegCloseKey(hKey);
}

// ================= 托盘 =================
NOTIFYICONDATAW g_nid = { 0 };

void TrayAdd() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    HICON hIcon = (HICON)LoadImageW(nullptr, L"icon.ico", IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    bool iconFromFile = (hIcon != nullptr);
    if (!hIcon) hIcon = LoadIconW(nullptr, IDI_APPLICATION);   // 系统共享图标，不要 Destroy
    nid.hIcon = hIcon;
    lstrcpynW(nid.szTip, L"常陆茉子桌宠", ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);
    if (iconFromFile && hIcon) DestroyIcon(hIcon);              // ★ 托盘已复制一份，原图可释放
}

void TrayRemove() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void TraySetTip(const wchar_t* tip) {
    if (!g_hwnd) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ================= 工具 =================
bool NatLess(const std::wstring& a, const std::wstring& b) {
    return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
}

// 磁盘 .pac 母版高度：始终按 zoom 上限 3.0 存，保证放大时够清晰
#ifndef PAC_MASTER_H
#define PAC_MASTER_H (int)(BASE_H * 3.0f)
#endif

// 内存纹理尺寸按当前缩放档位走：zoom 1.0 时只要 200x300，
// 不再无条件常驻 600x900。整套内存从 216MB 降到 24MB。
// 档位以【实际显示高度】为基准并向上取，保证纹理 >= 显示尺寸。
// 旧版以 g_zoom 为基准且向下取档（zoom 1.7 -> 1.5），纹理比落点小就会被
// 放大绘制，这正是"启动后人物发虚"的根因；而且纹理尺寸与 ini 里保存的
// 窗口尺寸脱节时（WinH 与 Zoom 不一致）偏差会被进一步放大。
static float ZoomTierScale() {
    float z = (float)g_currentH / (float)BASE_H;
    if (!(z > 0.2f)) z = 1.0f;
    if (z > 3.0f) z = 3.0f;
    if (z <= 1.0f)  return 1.0f;
    if (z <= 1.25f) return 1.25f;
    if (z <= 1.5f)  return 1.5f;
    if (z <= 2.0f)  return 2.0f;
    if (z <= 2.5f)  return 2.5f;
    return 3.0f;
}
// 纹理高度 = 显示高度，纹理宽度只做宽松上限（不约束比例）。
// 这样纹理尺寸 >= CalcDrawRectFit 算出的落点尺寸，绘制时只缩小不放大，
// 不再有逐帧拉伸 —— 切换动作/服装时才可能做到零顿挫。
static int TexMaxW() { return (int)(BASE_H * ZoomTierScale() * 4); }
static int TexMaxH() { return (int)(BASE_H * ZoomTierScale()); }

static float g_texTier[4][4] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
static unsigned long g_zoomLastMs = 0;

// 解码后的像素缓存到磁盘，第二次起直接读盘，单帧 300ms -> 约 2ms
#ifndef PAC_ENABLE
#define PAC_ENABLE 1
#endif

static std::wstring PacPathFor(const std::wstring& png) {
    std::wstring r = png;
    const std::wstring tag = L"Animations\\";
    size_t p = r.find(tag);
    if (p != std::wstring::npos) r.replace(p, tag.size(), L"AnimCache\\");
    else r = L"AnimCache\\" + r;
    size_t q = r.rfind(L'.');
    if (q != std::wstring::npos && q > r.rfind(L'\\')) r = r.substr(0, q);
    return r + L".pac";
}

static bool PacRead(const std::wstring& pac, int& w, int& h, std::vector<BYTE>& px) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, pac.c_str(), L"rb") != 0 || !f) return false;
    char mg[4] = {}; int iw = 0, ih = 0;
    bool ok = fread(mg, 1, 4, f) == 4 && memcmp(mg, "PAC2", 4) == 0
        && fread(&iw, 1, 4, f) == 4 && fread(&ih, 1, 4, f) == 4;
    if (ok && iw > 0 && ih > 0 && iw <= 8192 && ih <= 8192) {
        px.resize((size_t)iw * ih * 4);
        ok = fread(px.data(), 1, px.size(), f) == px.size();
    } else ok = false;
    fclose(f);
    if (!ok) { px.clear(); return false; }
    w = iw; h = ih; return true;
}

static void PacWrite(const std::wstring& pac, int w, int h, const BYTE* px, size_t n) {
    size_t sp = pac.rfind(L'\\');
    if (sp != std::wstring::npos) {
        std::wstring dir = pac.substr(0, sp);
        for (size_t i = 0; i < dir.size(); ++i)
            if (dir[i] == L'\\' && i > 2) {
                std::wstring sub = dir.substr(0, i);
                if (GetFileAttributesW(sub.c_str()) == INVALID_FILE_ATTRIBUTES)
                    CreateDirectoryW(sub.c_str(), nullptr);
            }
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, pac.c_str(), L"wb") != 0 || !f) return;
    fwrite("PAC2", 1, 4, f);
    fwrite(&w, 1, 4, f);
    fwrite(&h, 1, 4, f);
    fwrite(px, 1, n, f);
    fclose(f);
}

static Bitmap* MakeBitmap(int w, int h, const std::vector<BYTE>& px) {
    if (w <= 0 || h <= 0 || px.size() < (size_t)w * h * 4) return nullptr;
    Bitmap* b = new Bitmap(w, h, PixelFormat32bppARGB);
    if (!b || b->GetLastStatus() != Ok) { if (b) delete b; return nullptr; }
    BitmapData bd{}; Rect rc(0, 0, w, h);
    if (b->LockBits(&rc, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok) { delete b; return nullptr; }
    for (int y = 0; y < h; ++y)
        memcpy((BYTE*)bd.Scan0 + (size_t)y * bd.Stride, px.data() + (size_t)y * w * 4, (size_t)w * 4);
    b->UnlockBits(&bd);
    return b;
}

static bool ExtractPixels(Bitmap* b, int& w, int& h, std::vector<BYTE>& px) {
    if (!b || b->GetLastStatus() != Ok) return false;
    w = (int)b->GetWidth(); h = (int)b->GetHeight();
    if (w <= 0 || h <= 0) return false;
    px.resize((size_t)w * h * 4);
    BitmapData bd{}; Rect rc(0, 0, w, h);
    if (b->LockBits(&rc, ImageLockModeRead, PixelFormat32bppARGB, &bd) != Ok) return false;
    for (int y = 0; y < h; ++y)
        memcpy(px.data() + (size_t)y * w * 4, (BYTE*)bd.Scan0 + (size_t)y * bd.Stride, (size_t)w * 4);
    b->UnlockBits(&bd);
    return true;
}

static bool PacFresh(const std::wstring& pac, const std::wstring& png) {
    WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
    if (!GetFileAttributesExW(pac.c_str(), GetFileExInfoStandard, &a)) return false;
    if (!GetFileAttributesExW(png.c_str(), GetFileExInfoStandard, &b)) return false;
    LONG64 x = ((LONG64)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    LONG64 y = ((LONG64)b.ftLastWriteTime.dwHighDateTime << 32) | b.ftLastWriteTime.dwLowDateTime;
    return x >= y;
}

static Bitmap* Downscale(Bitmap* src, int maxW, int maxH) {
    if (!src || src->GetLastStatus() != Ok) return nullptr;
    int sw = (int)src->GetWidth(), sh = (int)src->GetHeight();
    if (sw <= 0 || sh <= 0) return nullptr;
    int dw = sw, dh = sh;
    if (maxW > 0 && maxH > 0 && (sw > maxW || sh > maxH)) {
        float s = (float)maxW / (float)sw;
        float t = (float)maxH / (float)sh;
        if (t < s) s = t;
        dw = (int)(sw * s); dh = (int)(sh * s);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    } else {
        return nullptr;
    }
    Bitmap* b = new Bitmap(dw, dh, PixelFormat32bppARGB);
    if (!b || b->GetLastStatus() != Ok) { delete b; return nullptr; }
    Graphics g(b);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.DrawImage(src, 0, 0, dw, dh);
    return b;
}

Image* SafeLoadImage(const std::wstring& path, int maxW = 0, int maxH = 0) {
    if (!PathFileExistsW(path.c_str())) return nullptr;
#if PAC_ENABLE
    {
        std::wstring pac = PacPathFor(path);
        int pw = 0, ph = 0; std::vector<BYTE> px;
        if (PacFresh(pac, path) && PacRead(pac, pw, ph, px)) {
            if (Bitmap* mb = MakeBitmap(pw, ph, px)) {
                if (maxW > 0 && maxH > 0 && (pw > maxW || ph > maxH)) {
                    if (Bitmap* r = Downscale(mb, maxW, maxH)) { delete mb; return r; }
                }
                return mb;
            }
        }
    }
#endif
    Bitmap* img = Bitmap::FromFile(path.c_str());
    if (!img || img->GetLastStatus() != Ok) {
        if (img) delete img;
        return nullptr;
    }
    // PNG 只解码一次：先按母版尺寸缩放并写 .pac，之后所有档位都从母版派生
    Bitmap* master = nullptr;
#if PAC_ENABLE
    {
        int mw = (int)(PAC_MASTER_H * 2);
        int sw = (int)img->GetWidth(), sh = (int)img->GetHeight();
        if (sw > mw || sh > PAC_MASTER_H) master = Downscale(img, mw, PAC_MASTER_H);
    }
#endif
    Bitmap* base = master ? master : img;
#if PAC_ENABLE
    { int ow = 0, oh = 0; std::vector<BYTE> opx;
      if (ExtractPixels(base, ow, oh, opx)) PacWrite(PacPathFor(path), ow, oh, opx.data(), opx.size()); }
#endif
    if (maxW > 0 && maxH > 0
        && ((int)base->GetWidth() > maxW || (int)base->GetHeight() > maxH)) {
        if (Bitmap* r = Downscale(base, maxW, maxH)) {
            if (master) delete master;
            delete img;
            return r;
        }
    }
    if (master) { delete img; return master; }
    return img;
}

static std::vector<std::wstring> ScanAnimFiles(Costume costume, AnimState state) {
    std::vector<std::wstring> out;
    static const wchar_t* cN[4] = { L"Ninja", L"Daily", L"School", L"Dress" };
    static const wchar_t* sN[4] = { L"Idle", L"Run", L"Think", L"Peace" };
    int ci = (int)costume, si = (int)state;
    if (ci < 0 || ci > 3) ci = 0;
    if (si < 0 || si > 3) si = 0;
    std::wstring dir = std::wstring(L"Animations\\") + cN[ci] + L"\\" + sN[si] + L"\\";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"*.png").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    std::vector<std::wstring> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), NatLess);
    out.reserve(names.size());
    for (auto& n : names) out.push_back(dir + n);
    return out;
}

static std::recursive_mutex     g_frameMtx;
static std::recursive_mutex     g_renderMtx;
static volatile bool            g_renderPrewarmReq = false;
static HANDLE                   g_hLoadThread = nullptr;
static volatile bool            g_loadQuit = false;

std::vector<Image*> LoadFrames(Costume costume, AnimState state) {
    std::vector<Image*> out;
    int ci = (int)costume, si = (int)state;
    if (ci < 0 || ci > 3) ci = 0;
    if (si < 0 || si > 3) si = 0;
    std::vector<std::wstring> paths = ScanAnimFiles(costume, state);
    if (paths.empty()) return out;
    out.reserve(paths.size());
    for (size_t i = 0; i < paths.size(); ++i) {
        if (g_loadQuit && i > 0) break;
        if (Image* im = SafeLoadImage(paths[i], TexMaxW(), TexMaxH())) out.push_back(im);
    }
    return out;
}

static std::vector<Image*>& GetFramesCached(Costume costume, AnimState state);   // 定义在下方（帧缓存）

void SwitchAnim(Costume costume, AnimState state) {
    if (state != g_currentState) PlayPoseSFX(state);
    if (costume == g_currentCostume) {
        g_currentFrameIndex = 0;
        g_currentState = state;
        g_currentCostume = costume;
        // ★ 取自帧缓存：缓存独占这些 Image，这里绝不能 ClearFrames(g_currentFrames)
        g_currentFrames = GetFramesCached(costume, state);
        g_idleActionTimer = 0;
        g_isRandomAction = false;
        g_renderPrewarmReq = true;
        return;
    }
    PlayCostumeChangeSFX();
    StatsAddAffinity(1);                    // 换装 +1 好感（对话才是主要来源）
    g_pendingCostume = costume;
    g_pendingState = state;
    g_pendingFrames = GetFramesCached(costume, state);
    g_renderPrewarmReq = true;
    g_bTransitioning = true;
    g_transitionTimer = 0;
}

// ==================================================================
//  ★ 帧缓存（性能核心 1）：避免每次切换动作都重新解码整套 PNG
//  原来切换服装 / 开始拖拽都要重新：扫描目录 → 解码 8~10 张 PNG，
//  统一走一次就要几百毫秒，这就是"切换动作、开始拖拽时卡顿"的根因。
//  素材运行期不会变，所以每套动画只解码一次，之后常驻复用。
// ==================================================================
static std::vector<Image*> g_frameCache[4][4];
static bool                g_frameCached[4][4] = {};
static int                 g_cacheSetCount = 0;
static unsigned long       g_cacheStamp[4][4] = {};
static unsigned long       g_cacheTick = 0;

// 16 套（4 服装 × 4 动作）全部常驻，切换时不再触发任何解码。
// 纹理按显示尺寸生成，单帧约 240KB(zoom 1.0)，全套 100 帧约 24MB，
// 16 套合计约 380MB —— 相比"每套 600x900 常驻"的 3GB 已大幅下降。
#ifndef FRAME_CACHE_MAX
#define FRAME_CACHE_MAX 16
#endif

static void FrameCacheEvictIfNeeded(int keepC1, int keepS1, int keepC2, int keepS2) {
    std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
    while (g_cacheSetCount >= FRAME_CACHE_MAX) {
        int bc = -1, bs = -1;
        unsigned long best = 0;
        for (int c = 0; c < 4; ++c)
            for (int s2 = 0; s2 < 4; ++s2) {
                if (!g_frameCached[c][s2]) continue;
                if ((c == keepC1 && s2 == keepS1) || (c == keepC2 && s2 == keepS2)) continue;
                if (bc < 0 || g_cacheStamp[c][s2] < best) { best = g_cacheStamp[c][s2]; bc = c; bs = s2; }
            }
        if (bc < 0) break;                       // 只剩在用的两套，无法再淘汰
        ClearFrames(g_frameCache[bc][bs]);
        g_frameCached[bc][bs] = false;
        g_texTier[bc][bs] = -1;
        --g_cacheSetCount;
    }
}

// ★ 缓存里的帧由缓存独占：外部只可读，禁止 delete
static std::vector<Image*>& GetFramesCached(Costume costume, AnimState state) {
    int ci = (int)costume, si = (int)state;
    if (ci < 0 || ci > 3) ci = 0;
    if (si < 0 || si > 3) si = 0;
    std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
    if (!g_frameCached[ci][si]) {
        int pc = -1, ps = -1;                    // 过渡中待切换的那套也要保护
        if (g_bTransitioning) { pc = (int)g_pendingCostume; ps = (int)g_pendingState; }
        FrameCacheEvictIfNeeded((int)g_currentCostume, (int)g_currentState, pc, ps);
        g_frameCache[ci][si] = LoadFrames((Costume)ci, (AnimState)si);
        g_frameCached[ci][si] = true;
        g_texTier[ci][si] = ZoomTierScale();
        ++g_cacheSetCount;
    }
    g_cacheStamp[ci][si] = ++g_cacheTick;
    return g_frameCache[ci][si];
}

static void ClearFrameCache() {
    std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
    for (int c = 0; c < 4; ++c)
        for (int s2 = 0; s2 < 4; ++s2) {
            ClearFrames(g_frameCache[c][s2]);
            g_frameCached[c][s2] = false;
            g_cacheStamp[c][s2] = 0;
            g_texTier[c][s2] = -1;
        }
    g_cacheSetCount = 0;
}

static void RenderCachePrewarm(int w, int h, const std::vector<Image*>& frames, int ci, int si);
static void RenderCacheInvalidate();
static void RenderCacheInvalidate(int ci, int si);

static std::vector<Image*> g_pendingDelete;
static volatile bool g_preloadDone = false;
static volatile int  g_preloadProgress = 0;   // 0..16，启动时托盘提示用

// 装载一套：解码全部帧 + 预构建本套的渲染缓存
static void LoadSet(int c, int s) {
    std::vector<Image*> nf = LoadFrames((Costume)c, (AnimState)s);
    std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
    if (g_frameCached[c][s]) ClearFrames(g_frameCache[c][s]);
    g_frameCache[c][s] = nf;
    g_frameCached[c][s] = true;
    g_texTier[c][s] = ZoomTierScale();
    RenderCacheInvalidate(c, s);
    RenderCachePrewarm(g_currentW, g_currentH, g_frameCache[c][s], c, s);
}

// 启动全量预加载：16 套全部解码并预构建，之后切换动作/服装不再有任何解码
static void PreloadAll() {
    for (int c = 0; c < 4; ++c)
        for (int s = 0; s < 4; ++s) {
            if (g_loadQuit) return;
            if (!ScanAnimFiles((Costume)c, (AnimState)s).empty()) LoadSet(c, s);
            ++g_preloadProgress;
            wchar_t tip[128];
            wsprintfW(tip, L"常陆茉子桌宠 · 加载素材 %d/16", g_preloadProgress);
            TraySetTip(tip);
        }
}

// 缩放档位变化后重建已缓存的套（旧帧交给主线程下一帧回收）
static void TexRebuildIfNeeded() {
    float want = ZoomTierScale();
    bool need = false;
    for (int c = 0; c < 4; ++c)
        for (int s = 0; s < 4; ++s)
            if (g_frameCached[c][s] && g_texTier[c][s] >= 0 && g_texTier[c][s] != want) need = true;
    if (!need) return;
    if (GetTickCount() - g_zoomLastMs < 500) return;
    for (int c = 0; c < 4; ++c)
        for (int s = 0; s < 4; ++s) {
            if (g_loadQuit) return;
            if (!g_frameCached[c][s]) continue;
            std::vector<Image*> nf = LoadFrames((Costume)c, (AnimState)s);
            if (nf.empty()) { g_texTier[c][s] = want; continue; }
            {
                std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
                std::vector<Image*> old = g_frameCache[c][s];
                g_frameCache[c][s] = nf;
                g_pendingDelete.insert(g_pendingDelete.end(), old.begin(), old.end());
            }
            g_texTier[c][s] = want;
            RenderCacheInvalidate(c, s);
            {
                std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
                RenderCachePrewarm(g_currentW, g_currentH, g_frameCache[c][s], c, s);
            }
        }
}

// 兜底：磁盘 .pac 是旧档位、或 ini 窗口尺寸与 zoom 不一致时，
// 纹理会小于落点尺寸而被拉伸。加载完成后统一校验并按需重建，
// 这样"启动发虚、切换服装后才清晰"不会再出现。
static void TexEnsureAll() {
    int want = TexMaxH();
    for (int c = 0; c < 4; ++c)
        for (int s = 0; s < 4; ++s) {
            if (g_loadQuit) return;
            if (!g_frameCached[c][s] || g_frameCache[c][s].empty()) continue;
            int h = 0;
            if (g_frameCache[c][s][0] && g_frameCache[c][s][0]->GetLastStatus() == Ok)
                h = (int)g_frameCache[c][s][0]->GetHeight();
            if (h >= want) continue;
            LoadSet(c, s);
        }
}

static unsigned __stdcall AnimLoadProc(void*) {
    PreloadAll();
    TexEnsureAll();
    g_preloadDone = true;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_PRELOAD_DONE, 0, 0);
    for (;;) {
        if (g_loadQuit) break;
        if (g_renderPrewarmReq) {
            g_renderPrewarmReq = false;
            std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
            int cc = (int)g_currentCostume, ss = (int)g_currentState;
            if (g_frameCached[cc][ss])
                RenderCachePrewarm(g_currentW, g_currentH, g_frameCache[cc][ss], cc, ss);
        }
        TexRebuildIfNeeded();
        Sleep(120);
    }
    return 0;
}

static void AnimLoadStart() {
    if (!g_hLoadThread)
        g_hLoadThread = (HANDLE)_beginthreadex(nullptr, 0, AnimLoadProc, nullptr, 0, nullptr);
}

static void AnimLoadStop() {
    if (!g_hLoadThread) return;
    g_loadQuit = true;
    WaitForSingleObject(g_hLoadThread, 3000);
    CloseHandle(g_hLoadThread);
    g_hLoadThread = nullptr;
}

// ==================================================================
//  ★ 立绘缩放：画布贴合（沿用你原本的公式，保证四套服装人物一样大）
//  drawH 固定为窗口高，宽度按原图比例推算 —— 不同服装因此天然等高。
//  不要改成"按角色包围盒缩放"：包围盒靠像素扫描得出，绿边残留、悬浮碎片、
//  呆毛、动作幅度都会让它忽大忽小，反而造成四套人物大小不一致。
// ==================================================================
static bool CalcDrawRectFit(int w, int h, int imgW, int imgH, int& drawX, int& drawY, int& drawW, int& drawH) {
    if (w <= 0 || h <= 0 || imgW <= 0 || imgH <= 0) return false;
    drawH = (int)(h * CHAR_RATIO);
    if (drawH <= 0) return false;
    drawW = (int)(drawH * (float)imgW / (float)imgH);
    if (drawW <= 0) return false;
    drawX = (w - drawW) / 2;
    drawY = h - drawH;
    return true;
}

// ==================================================================
//  ★ 预缩放渲染缓存（性能核心 2）
//  原来每秒 30 帧，每帧都要把 800~1100px 高的原图交给 GDI+ 缩放到窗口尺寸。
//  缩放是纯 CPU 重采样，占了绘制耗时的绝大部分，而且每帧重复做同一件事。
//  这里预先缩放好存起来，之后每帧只剩一次 1:1 拷贝。
//  构建是【逐帧懒加载】：帧序号每帧前进一格，所以最多每帧新建 1 张，
//  不会像"切换时一次性缩放整套"那样造成明显顿挫。
// ==================================================================
struct RenderCache {
    int w = 0, h = 0;
    const void* src = nullptr;
    std::vector<Bitmap*> bits;
    std::vector<int>     px, py;
    std::vector<char>    ready;
    void Clear() {
        for (size_t i = 0; i < bits.size(); ++i) if (bits[i]) delete bits[i];
        bits.clear(); px.clear(); py.clear(); ready.clear();
        w = 0; h = 0; src = nullptr;
    }
};
// ★ 每套动画一份：切换动作/服装时直接命中已有缓存，不再清空重建
static RenderCache g_render[4][4];

static RenderCache& RC(int ci, int si) {
    if (ci < 0 || ci > 3) ci = 0;
    if (si < 0 || si > 3) si = 0;
    return g_render[ci][si];
}

static void RenderCacheReset(int w, int h, const std::vector<Image*>& frames, int ci, int si) {
    RenderCache& r = RC(ci, si);
    // ★ 不能用 frames.data() 做身份判定：g_currentFrames 是 vector 拷贝，
    //   指针每次切换都不同，会导致整套缓存被误清空（这正是切换顿挫的来源）。
    //   缓存已按 (ci,si) 分开存放，尺寸与帧数一致即可复用；
    //   内容真正失效时由 RenderCacheInvalidate(ci,si) 显式处理。
    if (r.w != w || r.h != h || r.bits.size() != frames.size()) {
        r.Clear();
        r.w = w; r.h = h;
        r.bits.assign(frames.size(), nullptr);
        r.px.assign(frames.size(), 0);
        r.py.assign(frames.size(), 0);
        r.ready.assign(frames.size(), 0);
    }
}

// 第 idx 帧：命中缓存直接返回；未命中则构建一张（纹理已是显示尺寸，1:1 拷贝）
static Bitmap* RenderCacheGet(int idx, const std::vector<Image*>& frames, int ci, int si) {
    if (idx < 0 || idx >= (int)frames.size()) return nullptr;
    RenderCache& r = RC(ci, si);
    if (r.ready[idx]) return r.bits[idx];
    r.ready[idx] = 1;                       // 失败也标记，避免每帧重试
    Image* img = frames[idx];
    if (!img || img->GetLastStatus() != Ok) return nullptr;
    int imgW = (int)img->GetWidth(), imgH = (int)img->GetHeight();
    int dx, dy, dw, dh;
    if (!CalcDrawRectFit(r.w, r.h, imgW, imgH, dx, dy, dw, dh)) return nullptr;
    Bitmap* b = new Bitmap(dw, dh, PixelFormat32bppARGB);
    if (!b || b->GetLastStatus() != Ok) { delete b; return nullptr; }
    Graphics gg(b);
    gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    gg.SetSmoothingMode(SmoothingModeHighQuality);
    gg.SetPixelOffsetMode(PixelOffsetModeHalf);
    gg.DrawImage(img, 0, 0, dw, dh);
    r.bits[idx] = b; r.px[idx] = dx; r.py[idx] = dy;
    return b;
}

// 预先构建整套，主线程绘制时直接命中
static void RenderCachePrewarm(int w, int h, const std::vector<Image*>& frames, int ci, int si) {
    if (frames.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(g_renderMtx);
    RenderCacheReset(w, h, frames, ci, si);
    for (int i = 0; i < (int)frames.size(); ++i) RenderCacheGet(i, frames, ci, si);
}

static void RenderCacheInvalidate() {
    std::lock_guard<std::recursive_mutex> lk(g_renderMtx);
    for (int c = 0; c < 4; ++c) for (int s2 = 0; s2 < 4; ++s2) g_render[c][s2].Clear();
}
static void RenderCacheInvalidate(int ci, int si) {
    std::lock_guard<std::recursive_mutex> lk(g_renderMtx);
    RC(ci, si).Clear();
}

// ================= 绘制 =================
// 启动加载画面：人物位置先显示托盘图标，下面配一条进度条
static void DrawLoading(Graphics& g, int w, int h) {
    const int total = 16;
    int done = g_preloadProgress;
    if (done < 0) done = 0;
    if (done > total) done = total;

    float bob = sinf((float)g_loadTick * 0.16f) * (float)h * 0.02f;

    bool drew = false;
    if (g_pLoadingImg && g_pLoadingImg->GetLastStatus() == Ok) {
        int iw = (int)g_pLoadingImg->GetWidth(), ih = (int)g_pLoadingImg->GetHeight();
        if (iw > 0 && ih > 0) {
            int side = (int)(min(w, h) * 0.45f);
            if (side > 96) side = 96;
            if (side < 8) side = 8;
            float sc = (float)side / (float)(iw > ih ? iw : ih);
            int dw = (int)(iw * sc), dh = (int)(ih * sc);
            int dx = (w - dw) / 2;
            int dy = (int)((float)(h - dh) / 2.0f + bob);
            g.DrawImage(g_pLoadingImg, dx, dy, dw, dh);
            drew = true;
        }
    }
    if (!drew) {   // 没有 icon.ico 时退化为呼吸圆点
        int r = (int)(min(w, h) * 0.12f);
        if (r < 6) r = 6;
        int cx = w / 2, cy = (int)((float)h / 2.0f + bob);
        SolidBrush dot(Color(255, 120, 200, 255));
        g.FillEllipse(&dot, cx - r, cy - r, r * 2, r * 2);
    }

    int barH = 6;
    int barW = (int)(w * 0.72f);
    int bx = (w - barW) / 2;
    int by = h - barH - 14;
    SolidBrush track(Color(190, 70, 70, 70));
    g.FillRectangle(&track, bx, by, barW, barH);
    int pw = (int)((float)barW * (float)done / (float)total);
    if (pw > 0) {
        SolidBrush fill(Color(255, 120, 200, 255));
        g.FillRectangle(&fill, bx, by, pw, barH);
    }

    wchar_t buf[40];
    wsprintfW(buf, L"加载素材 %d", (int)((float)done * 100.0f / (float)total));
    wcscat(buf, L"%");
    Font font(L"Microsoft YaHei UI", 9.0f, FontStyleBold);
    SolidBrush txt(Color(255, 255, 255, 255));
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    RectF lay((float)bx, (float)(by - 20), (float)barW, 16.0f);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    g.DrawString(buf, lstrlenW(buf), &font, lay, &sf, &txt);
}

void UpdateLayered() {
    if (!g_hwnd) return;
    int w = g_currentW, h = g_currentH;
    if (w <= 0 || h <= 0) return;

    // 尺寸变化时重建离屏画布与 Graphics（Graphics 复用，不再每帧构造/析构）
    if (!g_memDC || g_cachedW != w || g_cachedH != h) {
        if (g_gfx) { delete g_gfx; g_gfx = nullptr; }
        if (g_memDC) {
            SelectObject(g_memDC, g_hOldBmp);
            DeleteObject(g_hMemBmp);
            DeleteDC(g_memDC);
            g_memDC = nullptr; g_hMemBmp = nullptr; g_hOldBmp = nullptr;
        }
        HDC screenDC = GetDC(nullptr);
        if (!screenDC) return;
        g_memDC = CreateCompatibleDC(screenDC);
        g_hMemBmp = CreateCompatibleBitmap(screenDC, w, h);
        ReleaseDC(nullptr, screenDC);
        if (!g_memDC || !g_hMemBmp) return;
        g_hOldBmp = (HBITMAP)SelectObject(g_memDC, g_hMemBmp);
        g_cachedW = w; g_cachedH = h;
        g_gfx = new Graphics(g_memDC);
        if (g_gfx && g_gfx->GetLastStatus() != Ok) { delete g_gfx; g_gfx = nullptr; }
        if (g_gfx) {
            g_gfx->SetSmoothingMode(SmoothingModeAntiAlias);
            g_gfx->SetInterpolationMode(InterpolationModeHighQualityBicubic);
            g_gfx->SetPixelOffsetMode(PixelOffsetModeHalf);
        }
        { std::lock_guard<std::recursive_mutex> lk(g_renderMtx);
          RenderCacheInvalidate(); }   // 尺寸变了，预缩放结果作废
    }
    if (!g_gfx) return;
    Graphics& g = *g_gfx;

    g.Clear(Color(0, 0, 0, 0));
    if (!g_preloadDone) {
        DrawLoading(g, w, h);
    } else if (g_bTransitioning && g_pTransitionImg) {
        int halfDur = TRANSITION_DURATION / 2;
        int alpha = 255;
        if (g_transitionTimer <= halfDur) alpha = (g_transitionTimer * 255) / (halfDur > 0 ? halfDur : 1);
        else {
            int t = g_transitionTimer - halfDur;
            int remain = TRANSITION_DURATION - halfDur;
            alpha = 255 - (t * 255) / (remain > 0 ? remain : 1);
        }
        if (alpha < 0) alpha = 0; if (alpha > 255) alpha = 255;
        if (g_transitionTimer > halfDur && !g_pendingFrames.empty() && g_pendingFrames[0]) {
            Image* pending = g_pendingFrames[0];
            if (pending->GetLastStatus() == Ok) {
                int imgW = (int)pending->GetWidth(), imgH = (int)pending->GetHeight();
                int drawX, drawY, drawW, drawH;
                if (CalcDrawRectFit(w, h, imgW, imgH, drawX, drawY, drawW, drawH))
                    g.DrawImage(pending, drawX, drawY, drawW, drawH);
            }
        }
        ColorMatrix cm = {
            1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, alpha / 255.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, 1.0f
        };
        ImageAttributes imgAttr;
        imgAttr.SetColorMatrix(&cm);
        if (g_pTransitionImg->GetLastStatus() == Ok) {
            int imgW = (int)g_pTransitionImg->GetWidth(), imgH = (int)g_pTransitionImg->GetHeight();
            int drawX, drawY, drawW, drawH;
            if (CalcDrawRectFit(w, h, imgW, imgH, drawX, drawY, drawW, drawH))
                g.DrawImage(g_pTransitionImg, Rect(drawX, drawY, drawW, drawH), 0, 0, imgW, imgH, UnitPixel, &imgAttr);
        }
    } else if (!g_currentFrames.empty()) {
        std::lock_guard<std::recursive_mutex> lk(g_renderMtx);
        int cc = (int)g_currentCostume, ss = (int)g_currentState;
        RenderCacheReset(w, h, g_currentFrames, cc, ss);
        int n = (int)g_currentFrames.size();
        int idx = g_currentFrameIndex % n;
        if (idx < 0) idx += n;
        if (Bitmap* b = RenderCacheGet(idx, g_currentFrames, cc, ss)) {
            RenderCache& r = RC(cc, ss);
            int px = (idx < (int)r.px.size()) ? r.px[idx] : 0;
            int py = (idx < (int)r.py.size()) ? r.py[idx] : 0;
            g.DrawImage(b, px, py);                 // 1:1 拷贝，不再每帧重采样
        }
    }

    // ===== RVC 转换状态栏（画在窗口顶部）=====
    {
        std::lock_guard<std::recursive_mutex> lk(g_rvcMtx);
        if (g_rvcStage != RvcStage::Idle) {
            std::wstring title = g_rvcSourceName.empty() ? L"" : (g_rvcSourceName + L" · ");
            std::wstring text = title + g_rvcStatusText;
            SolidBrush barBg(Color(180, 0, 0, 0));
            g.FillRectangle(&barBg, 0, 0, w, 20);
            Color dotColor;
            switch (g_rvcStage) {
            case RvcStage::Done:    dotColor = Color(255, 80, 200, 120); break;
            case RvcStage::Failed:  dotColor = Color(255, 220, 60, 60);  break;
            default:
                dotColor = ((GetTickCount() / 300) % 2) ? Color(255, 120, 180, 255)
                                                         : Color(255, 255, 255, 255);
                break;
            }
            SolidBrush dot(dotColor);
            g.FillEllipse(&dot, 6, 7, 6, 6);
            Font font(L"Microsoft YaHei UI", 9.0f, FontStyleBold);
            SolidBrush txt(Color(255, 255, 255, 255));
            g.SetTextRenderingHint(TextRenderingHintAntiAlias);
            RectF lay(18.0f, 2.0f, (float)(w - 22), 18.0f);
            StringFormat sf; sf.SetAlignment(StringAlignmentNear); sf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(text.c_str(), (int)text.size(), &font, lay, &sf, &txt);

            if (g_rvcProgress >= 0 &&
                g_rvcStage != RvcStage::Done && g_rvcStage != RvcStage::Failed) {
                int pct = g_rvcProgress;
                if (pct > 100) pct = 100;
                SolidBrush track(Color(190, 70, 70, 70));
                g.FillRectangle(&track, 0, 20, w, 4);
                SolidBrush fill(Color(255, 120, 200, 255));
                int pw = (int)((float)w * (float)pct / 100.0f);
                if (pw > 0) g.FillRectangle(&fill, 0, 20, pw, 4);
            }
        }
    }

    POINT ptSrc = { 0, 0 };
    POINT ptWin;
    if (g_winPosKnown) {
        ptWin.x = g_winX; ptWin.y = g_winY;
    } else {
        RECT rcWin;
        if (!GetWinRect(g_hwnd, rcWin)) return;
        g_winX = rcWin.left; g_winY = rcWin.top; g_winPosKnown = true;
        ptWin.x = g_winX;    ptWin.y = g_winY;
    }
    SIZE size = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, nullptr, &ptWin, &size, g_memDC, &ptSrc, RGB(0, 0, 0), &bf, ULW_COLORKEY);
    g_lastPaintMs = GetTickCount64();
}

// ==================================================================
//  ★ 合成节流（性能核心 3）
//  UpdateLayeredWindow 是分层窗口最贵的一步：每次都要让系统重新合成整块区域。
//  鼠标移动消息在高刷鼠标下可达数百 Hz，若每次都贴屏，主线程会被合成占满 ——
//  这就是"拖动人物时很卡"的根因。
//  这里把额外触发的合成限制到约 60fps，超出的移动只更新逻辑位置，
//  下一次定时重绘（30fps）自然会跟上，视觉上完全连续。
// ==================================================================
// 间隔取 17ms：略大于定时器周期的一半(33/2=16.5)，这样额外合成只会落在
// 两次定时器之间，与 30fps 定时器错开形成稳定的 60fps 上限，不会叠加成 90fps。
#ifndef REPAINT_MIN_MS
#define REPAINT_MIN_MS 17
#endif
static void RequestPaint(bool force = false) {
    ULONGLONG now = GetTickCount64();
    if (force || g_lastPaintMs == 0 || (now - g_lastPaintMs) >= REPAINT_MIN_MS) {
        g_paintPending = false;
        UpdateLayered();                 // 内部会刷新 g_lastPaintMs
    } else {
        g_paintPending = true;           // 交由下一次定时重绘补上
    }
}

// 隐藏桌宠时停掉定时器：不然后台仍在每帧解码 + 合成，白占 CPU
static void SetPetVisible(HWND hwnd, bool show) {
    ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE);
    if (show && !g_timerOn) {
        SetTimer(hwnd, 1, FRAME_INTERVAL_MS, nullptr);
        g_timerOn = true;
        g_lastTickMs = GetTickCount64();
    } else if (!show && g_timerOn) {
        KillTimer(hwnd, 1);
        g_timerOn = false;
    }
}

// 拖拽落点限制在光标所在显示器内：否则能拖出屏幕，只能靠改 config.ini 救回来
static void ClampToMonitor(POINT pt, int w, int h, int& outX, int& outY) {
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (hm && GetMonitorInfoW(hm, &mi)) {
        int L = mi.rcWork.left, T = mi.rcWork.top;
        int R = mi.rcWork.right - w,  B = mi.rcWork.bottom - h;
        if (R < L) R = L;
        if (B < T) B = T;
        if (outX < L) outX = L; if (outX > R) outX = R;
        if (outY < T) outY = T; if (outY > B) outY = B;
    } else {
        int sw = GetSystemMetrics(SM_CXSCREEN) - w;
        int sh = GetSystemMetrics(SM_CYSCREEN) - h;
        if (outX < 0) outX = 0; if (outX > sw) outX = sw;
        if (outY < 0) outY = 0; if (outY > sh) outY = sh;
    }
}

// ==================== LLM 对话系统 ====================
#define WM_APPEND_TEXT      (WM_USER + 100)
#define WM_APPEND_CHAR      (WM_USER + 102)
#define WM_STREAM_DONE      (WM_USER + 101)
#define WM_REMOVE_THINKING  (WM_USER + 103)

static const int TYPEWRITER_DELAY_MS = 30;
static bool g_lastJsonTruncated = false;   // 本次回复是否被 max_tokens 截断
static HWND g_hChatWnd = nullptr;
static HWND g_hHistoryEdit = nullptr;
static HWND g_hInputEdit = nullptr;
static HWND g_hSendBtn = nullptr;
static bool g_isChatting = false;
static bool g_richEditLoaded = false;

#include <string>
std::wstring g_ttsRefAudioPath; // 全局变量，用来存运行时的绝对路径
// ★ 参考文本原值里用了全角逗号「，」，TTS 断句会怪，现在统一用顿号。
//   1) "よろし" 少了 "く"，正确是 "よろしく"；
//   2) 全角逗号 "，" 在日语里应写作 "、"。
//   参考文本与实际参考音频内容不一致会明显拖累音色克隆效果。
//   如果你的参考音频说的不是这句，请在 llm.ini 的 [TTS] TtsRefText 里改成完全一致的内容。
static const wchar_t* TTS_REF_TEXT = L"常陸茉子です、よろしくお願いします";

// ==================== TTS 配置（[TTS] 段） ====================
// 端口/路径随 GPT-SoVITS 版本而变，写死容易"换个整合包就哑了"，全部可配。
static std::wstring GetIniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* defVal) {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return defVal;
    std::wstring iniPath = exeDir + L"\\llm.ini";
    std::vector<wchar_t> buf(1024, L'\0');
    GetPrivateProfileStringW(sec, key, defVal, buf.data(), (DWORD)buf.size(), iniPath.c_str());
    return std::wstring(buf.data());
}
static int GetIniInt(const wchar_t* sec, const wchar_t* key, int defVal, int lo, int hi) {
    std::wstring v = GetIniStr(sec, key, L"");
    if (v.empty()) return defVal;
    wchar_t* end = nullptr;
    long d = wcstol(v.c_str(), &end, 10);
    if (end == v.c_str()) return defVal;
    if (d < lo) return lo;
    if (d > (long)hi) return hi;
    return (int)d;
}
// 浮点版（TTS 的 top_p / temperature / speed_factor 等需要小数）
static double GetIniDbl(const wchar_t* sec, const wchar_t* key, double defVal, double lo, double hi) {
    std::wstring v = GetIniStr(sec, key, L"");
    if (v.empty()) return defVal;
    wchar_t* end = nullptr;
    double d = wcstod(v.c_str(), &end);
    if (end == v.c_str()) return defVal;
    if (d < lo) return lo;
    if (d > hi) return hi;
    return d;
}
static void TtsLog(const std::string& tag, const std::string& text) {
    std::string s = "[TTS] " + tag + ": " + text;
    if (s.size() > 4000) s = s.substr(0, 4000) + "...(truncated)";
    // ★ 这里的内容是要朗读的回复正文，所以统一走调试开关，默认不输出
    PetDbgA(s.c_str());
    PetDbgA("\n");
}

// ★ 原写法先按"含终止符"算出 len，却只分配 len-1 再要求它写 len 字节，
//   WideCharToMultiByte 在缓冲不足时返回 0 且内容未定义 —— 文本可能被写空。
//   正确做法：cchWideChar 传 wstr.size()（不含终止符），输入输出长度精确一致。
static std::string WideStringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int wlen = (int)wstr.size();
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string str((size_t)len, '\0');
    int written = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, &str[0], len, nullptr, nullptr);
    if (written <= 0) return "";
    str.resize((size_t)written);
    return str;
}

static std::wstring Utf8ToWideString(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring wstr((size_t)len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len - 1);
    return wstr;
}

static std::string EscapeJsonA(std::string str) {
    size_t pos = 0;
    while ((pos = str.find('\\', pos)) != std::string::npos) { str.replace(pos, 1, "\\\\"); pos += 2; }
    pos = 0;
    while ((pos = str.find('"', pos)) != std::string::npos) { str.replace(pos, 1, "\\\""); pos += 2; }
    pos = 0;
    while ((pos = str.find('\n', pos)) != std::string::npos) { str.replace(pos, 1, "\\n"); pos += 2; }
    pos = 0;
    while ((pos = str.find('\r', pos)) != std::string::npos) { str.replace(pos, 1, "\\r"); pos += 2; }
    return str;
}

static std::wstring EscapeJsonW(std::wstring str) {
    size_t pos = 0;
    while ((pos = str.find(L'\\', pos)) != std::wstring::npos) { str.replace(pos, 1, L"\\\\"); pos += 2; }
    pos = 0;
    while ((pos = str.find(L'"', pos)) != std::wstring::npos) { str.replace(pos, 1, L"\\\""); pos += 2; }
    pos = 0;
    while ((pos = str.find(L'\n', pos)) != std::wstring::npos) { str.replace(pos, 1, L"\\n"); pos += 2; }
    pos = 0;
    while ((pos = str.find(L'\r', pos)) != std::wstring::npos) { str.replace(pos, 1, L"\\r"); pos += 2; }
    return str;
}

// 完整解码 JSON 字符串转义：只处理 \n 和 \" 的话，
// 遇到 \u4e2d 这类 unicode 转义会原样留在正文里，长文本里出现就会变成 "\u4e2d..." 乱码
static std::string DecodeJsonEscapes(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    auto hex4 = [&](size_t at, unsigned& cp) -> bool {
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            char c = value[at + k];
            unsigned d;
            if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (unsigned)(c - 'A' + 10);
            else return false;
            cp = cp * 16 + d;
        }
        return true;
        };
    for (size_t i = 0; i < value.size();) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            char n = value[i + 1];
            if (n == 'u' && i + 6 <= value.size()) {
                unsigned cp = 0;
                if (hex4(i + 2, cp)) {
                    if (cp < 0x80) {
                        out += (char)cp;
                    }
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    i += 6;
                    continue;
                }
            }
            switch (n) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            default: out += n; break;
            }
            i += 2;
            continue;
        }
        out += value[i];
        i++;
    }
    return out;
}

static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";
    pos += searchKey.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.length() || json[pos] != '"') return "";
    pos++;
    // ★ 判断转义要看清反斜杠本身是否被转义，遇到 "\\" 这种连续反斜杠容易误判；
    //   这里改成真正跳过转义序列
    size_t endPos = pos;
    bool closed = false;
    while (endPos < json.length()) {
        if (json[endPos] == '\\') { endPos += 2; continue; }
        if (json[endPos] == '"') { closed = true; break; }
        endPos++;
    }
    // ★ 长文本被 max_tokens 截断时收不到闭合引号，原逻辑直接返回空串，
    //   整段故事就白生成了。这里保留已经生成的部分。
    if (!closed) g_lastJsonTruncated = true;
    std::string value = closed ? json.substr(pos, endPos - pos) : json.substr(pos);
    return DecodeJsonEscapes(value);
}

// 取 JSON 里的整数（如 affection）。模型可能写成 3、"3"、3.0，甚至字段缺失，
// 这里全部容错；越界或缺字段一律返回默认值，绝不因为评分字段毁掉整轮对话。
static int ExtractJsonInt(const std::string& json, const std::string& key, int defVal, int lo, int hi) {
    std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return defVal;
    pos += searchKey.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    bool quoted = (pos < json.length() && json[pos] == '"');
    if (quoted) pos++;
    bool neg = false;
    if (pos < json.length() && (json[pos] == '-' || json[pos] == '+')) { neg = (json[pos] == '-'); pos++; }
    if (pos >= json.length() || json[pos] < '0' || json[pos] > '9') return defVal;
    long long v = 0;
    while (pos < json.length() && json[pos] >= '0' && json[pos] <= '9') {
        v = v * 10 + (json[pos] - '0');
        if (v > 1000000) { v = 1000000; break; }   // 防止异常大的数溢出
        pos++;
    }
    if (neg) v = -v;
    int out = (int)v;
    if (out < lo) out = lo;
    if (out > hi) out = hi;
    return out;
}

static std::string ExtractAssistantContent(const std::string& body) {
    size_t p = body.find("\"content\"");
    if (p == std::string::npos) return "";
    p = body.find(':', p);
    if (p == std::string::npos) return "";
    p++;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p >= body.size() || body[p] != '"') return "";
    p++;
    std::string out;
    while (p < body.size()) {
        char c = body[p];
        if (c == '\\' && p + 1 < body.size()) {
            char n = body[p + 1];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'r') out += '\r';
            else if (n == '"') out += '"';
            else if (n == '\\') out += '\\';
            else if (n == '/') out += '/';
            else if (n == 'u' && p + 5 < body.size()) { out.append(body, p, 6); p += 6; continue; }
            else out += n;
            p += 2; continue;
        }
        if (c == '"') break;
        out += c; p++;
    }
    return out;
}

static std::string CleanToJsonObject(const std::string& text) {
    std::string s = text;
    auto stripFence = [&](const std::string& open) {
        size_t pos = s.find(open);
        if (pos != std::string::npos) {
            s = s.substr(pos + open.size());
            size_t end = s.find("```");
            if (end != std::string::npos) s = s.substr(0, end);
        }
        };
    stripFence("```json"); stripFence("```JSON"); stripFence("```");
    size_t a = s.find('{'), b = s.rfind('}');
    if (a != std::string::npos && b != std::string::npos && b > a) s = s.substr(a, b - a + 1);
    return s;
}

// ==================== TTS 端点自动探测 ====================
// GPT-SoVITS 是 FastAPI 服务，自带 /openapi.json。
// 不同整合包的推理路径并不一样：/tts、/v2/tts、/v1/tts、/api/tts、/run/predict…
// 写死其中一个就会"换个整合包就 404 哑掉"。
// 所以先问服务端"你有哪些端点"，再挑名字里带 tts 的那个。
// WinHttpGetText 定义在本文件靠后（WinHTTP 工具区），这里先声明，否则编译不过
static std::string WinHttpGetText(HINTERNET hConnect, const wchar_t* path);

// ★ 本地服务（127.0.0.1 / localhost）必须绕过系统代理。
//   WinHTTP 的默认代理来自系统设置，装了代理/加速器的机器上会把 localhost 的请求也转发出去，
//   典型症状就是"服务端日志里一个请求都没有，程序却报连不上"。
static bool IsLocalHostName(const std::wstring& h) {
    if (h.empty()) return false;
    std::wstring s;
    s.reserve(h.size());
    for (wchar_t c : h) s += (wchar_t)tolower((int)c);
    return s == L"127.0.0.1" || s == L"localhost" || s == L"::1" || s == L"[::1]";
}
static HINTERNET WinHttpOpenAuto(const std::wstring& host) {
    return WinHttpOpen(L"MakotoPet/1.0",
        IsLocalHostName(host) ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        NULL, NULL, 0);
}

static std::vector<std::wstring> TtsDiscoverPaths(const std::wstring& host, int port) {
    std::vector<std::wstring> out;

    HINTERNET hSession = WinHttpOpenAuto(host);
    if (!hSession) return out;
    WinHttpSetTimeouts(hSession, 3000, 3000, 4000, 4000);
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return out; }
    std::string spec = WinHttpGetText(hConnect, L"/openapi.json");
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (spec.empty()) { TtsLog("probe", "GET /openapi.json 取不到（服务没起来 / 不是 FastAPI 服务）"); return out; }

    // 定位 "paths" 的 { ... } 块（按括号匹配，跳过字符串里的括号）
    size_t pk = spec.find("\"paths\"");
    if (pk == std::string::npos) { TtsLog("probe", "openapi.json 里没有 paths 段"); return out; }
    size_t ob = spec.find('{', pk);
    if (ob == std::string::npos) return out;
    int depth = 0; size_t cb = std::string::npos; bool inStr = false;
    for (size_t i = ob; i < spec.size(); ++i) {
        char c = spec[i];
        if (inStr) { if (c == '\\') { ++i; continue; } if (c == '"') inStr = false; continue; }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') { --depth; if (depth == 0) { cb = i; break; } }
    }
    if (cb == std::string::npos) cb = spec.size();
    std::string block = spec.substr(ob, cb - ob);

    // 抓所有 "/xxx" 形式的键
    std::vector<std::string> all;
    for (size_t i = 0; i + 1 < block.size(); ++i) {
        if (block[i] == '"' && block[i + 1] == '/') {
            size_t e = block.find('"', i + 2);
            if (e == std::string::npos) break;
            all.push_back(block.substr(i + 1, e - i - 1));
            i = e;
        }
    }

    std::string diag;
    for (const auto& pa : all) {
        std::string low = pa;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        diag += pa + " ";
        // 只要推理端点，排除 /set_refer_audio、/set_gpt_weights 这类配置端点
        if (low.find("tts") != std::string::npos && low.find("set_") == std::string::npos) {
            int n = MultiByteToWideChar(CP_UTF8, 0, pa.c_str(), (int)pa.size(), nullptr, 0);
            if (n > 0) {
                std::wstring wp((size_t)n, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, pa.c_str(), (int)pa.size(), &wp[0], n);
                out.push_back(wp);
            }
        }
    }
    if (!diag.empty()) TtsLog("probe", "服务端端点: " + diag);
    if (!out.empty()) {
        std::string pick;
        for (const auto& w : out) pick += WideStringToUtf8(w) + " ";
        TtsLog("probe", "命中 tts 端点: " + pick);
    } else {
        TtsLog("probe", "端点里没找到带 tts 的路径（服务可能是 webui 而非 api）");
    }
    return out;
}

// 发一次 TTS 请求；返回 true 表示拿到了 wav 数据。
// 把状态码一起带回来，方便上层判断是"接口路径不对"还是"服务端报错"。
static bool TtsPostOnce(const std::wstring& host, int port, const std::wstring& path,
    const std::string& body, int timeoutMs,
    std::vector<char>& outWav, int& outStatus, std::string* errBodyOut = nullptr) {
    outStatus = 0;
    outWav.clear();
    if (errBodyOut) errBodyOut->clear();
    HINTERNET hSession = WinHttpOpenAuto(host);
    if (!hSession) { TtsLog("error", "WinHttpOpen failed"); return false; }
    // ★ 不设超时的话，服务没起来/处理卡住会一直挂在这，UI 也跟着僵住
    WinHttpSetTimeouts(hSession, 10000, 10000, 30000, (int)timeoutMs);
    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) { TtsLog("error", "WinHttpConnect failed"); WinHttpCloseHandle(hSession); return false; }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        NULL, NULL, NULL, 0);
    if (!hRequest) { TtsLog("error", "WinHttpOpenRequest failed"); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json; charset=utf-8", -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Accept: audio/wav,*/*", -1, WINHTTP_ADDREQ_FLAG_ADD);
    BOOL sent = WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)body.c_str(),
        (DWORD)body.length(), (DWORD)body.length(), 0);
    if (!sent) {
        TtsLog("error", "WinHttpSendRequest failed (服务未启动或端口不对?)");
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        TtsLog("error", "WinHttpReceiveResponse failed");
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return false;
    }
    DWORD code = 0, cs = sizeof(code);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &cs, WINHTTP_NO_HEADER_INDEX))
        outStatus = (int)code;
    std::vector<char> data;
    DWORD dwSize = 0, dwDownloaded = 0;
    for (;;) {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
        std::vector<char> buf(dwSize);
        if (!WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded) || dwDownloaded == 0) break;
        data.insert(data.end(), buf.begin(), buf.begin() + dwDownloaded);
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    if (data.empty()) { TtsLog("warn", "empty response, http=" + std::to_string(outStatus)); return false; }
    bool isWav = data.size() > 12 && !memcmp(data.data(), "RIFF", 4) && !memcmp(data.data() + 8, "WAVE", 4);
    if (!isWav) {
        // 不是 wav：多半是 404/500 的错误页或 JSON。把内容打出来，一眼就能看出原因。
        std::string head(data.begin(), data.begin() + (std::min)((size_t)512, data.size()));
        if (errBodyOut) *errBodyOut = head;
        TtsLog("error", "http=" + std::to_string(outStatus) + " path=" + WideStringToUtf8(path) + " body=" + head);
        return false;
    }
    outWav.swap(data);
    return true;
}

static bool CallTTSService(const std::wstring& jaText, std::wstring* errOut) {
    if (errOut) errOut->clear();
    if (jaText.empty()) return false;

    std::wstring host      = GetIniStr(L"TTS", L"TtsHost", L"127.0.0.1");
    int          port      = GetIniInt(L"TTS", L"TtsPort", 9880, 1, 65535);
    int          timeoutMs = GetIniInt(L"TTS", L"TtsTimeoutMs", 45000, 1000, 600000);
    std::wstring cfgPath   = GetIniStr(L"TTS", L"TtsPath", L"");
    std::wstring refAudio  = GetIniStr(L"TTS", L"TtsRefAudio", L"");
    if (refAudio.empty()) refAudio = g_ttsRefAudioPath;
    std::wstring refText   = GetIniStr(L"TTS", L"TtsRefText", TTS_REF_TEXT);
    std::wstring textLang  = GetIniStr(L"TTS", L"TtsTextLang", L"ja");
    std::wstring promptLang= GetIniStr(L"TTS", L"TtsPromptLang", L"ja");
    int          split     = GetIniInt(L"TTS", L"TtsSplit", 1, 0, 1);

    // ★ 参考音频缺失时服务端返回的是 JSON 错误而不是 wav，界面上只表现为"没声音"。
    //   这里先自检，把真正原因说出来。
    if (refAudio.empty()) {
        TtsLog("error", "未配置参考音频（llm.ini [TTS] TtsRefAudio，或直接拖音频到桌宠）");
        if (errOut) *errOut = L"未配置参考音频";
        return false;
    }
    if (!PathFileExistsW(refAudio.c_str())) {
        std::string e = "参考音频不存在: " + WideStringToUtf8(refAudio)
            + " （在 llm.ini 的 [TTS] TtsRefAudio 填绝对路径）";
        TtsLog("error", e);
        if (errOut) *errOut = L"参考音频不存在";
        return false;
    }

    // ==================== v2 与 v2Pro 的真正差异（重点） ====================
    // ★ 端点【没有变】：v2 和 v2Pro 整合包用的都是 POST /tts。
    //   不要因为版本号就去找 /v2pro/tts、/v2/tts 之类的路径 —— 那些都会 404。
    //   真正不一样的是参数要求：
    //   1) v2Pro / v2ProPlus 用的是 v3 那一套 GPT（s1v3.ckpt）。这类模型里
    //      prompt_text 是【必填】，v1/v2 才是可选。所以下面无条件发送 prompt_text，
    //      并且 v3 系下为空时直接提前报错，不留到服务端才失败。
    //   2) 各版本的采样默认值不同，这里全部显式写出，不依赖服务端默认值。
    //   ★ 关键修复：不再发送 "split_text"。它根本不是 api_v2.py 的字段。
    //     FastAPI 默认会忽略多余字段，但部分整合包（v2pro 分支就有）把模型配成了
    //     extra="forbid"，多一个字段就返回 422 —— 表现就是"没有任何语音"，
    //     而服务端日志里只有一个看不懂的 422，很难往参数上想。
    std::wstring version = GetIniStr(L"TTS", L"TtsVersion", L"v2Pro");
    bool v3class = (version == L"v2Pro" || version == L"v2ProPlus"
                 || version == L"v3"    || version == L"v4");
    if (v3class && refText.empty()) {
        TtsLog("error", "TtsVersion=" + WideStringToUtf8(version)
            + " 要求 prompt_text 必填（llm.ini [TTS] TtsRefText 填参考音频说的原话）");
        if (errOut) *errOut = L"v2Pro 需要 TtsRefText（参考音频的文本）";
        return false;
    }
    // ★ 参考音频路径含中文/日文等字符时，部分整合包读取会失败（GPT-SoVITS 官方也提示过）
    for (wchar_t c : refAudio) {
        if (c > 127) {
            TtsLog("warn", "参考音频路径含非 ASCII 字符，部分整合包会读不出来："
                + WideStringToUtf8(refAudio));
            break;
        }
    }

    auto numStr = [](double v) -> std::string {
        char b[32];
        if (_snprintf_s(b, 32, _TRUNCATE, "%g", v) < 0) return std::string("0");
        return std::string(b);
    };
    // 自定义追加字段：[TTS] TtsExtraJson 里写形如 "sample_steps":8 的片段（不带逗号/花括号）
    std::string extra = WideStringToUtf8(GetIniStr(L"TTS", L"TtsExtraJson", L""));
    while (!extra.empty() && (unsigned char)extra.front() <= ' ') extra.erase(extra.begin());
    while (!extra.empty() && (unsigned char)extra.back() <= ' ') extra.pop_back();
    if (!extra.empty()) {
        if (extra.front() == '{') extra.erase(extra.begin());
        if (!extra.empty() && extra.back() == '}') extra.pop_back();
        if (!extra.empty()) extra = "," + extra;
    }

    std::string splitMethod = split ? "cut5" : "cut0";
    std::string jsonBody = "{"
        "\"text\": \"" + EscapeJsonA(WideStringToUtf8(jaText)) + "\","
        "\"text_lang\": \"" + EscapeJsonA(WideStringToUtf8(textLang)) + "\","
        "\"ref_audio_path\": \"" + EscapeJsonA(WideStringToUtf8(refAudio)) + "\","
        "\"aux_ref_audio_paths\": [],"
        "\"prompt_text\": \"" + EscapeJsonA(WideStringToUtf8(refText)) + "\","
        "\"prompt_lang\": \"" + EscapeJsonA(WideStringToUtf8(promptLang)) + "\","
        "\"top_k\": " + std::to_string(GetIniInt(L"TTS", L"TtsTopK", 15, 1, 100)) + ","
        "\"top_p\": " + numStr(GetIniDbl(L"TTS", L"TtsTopP", 1.0, 0.0, 1.0)) + ","
        "\"temperature\": " + numStr(GetIniDbl(L"TTS", L"TtsTemperature", 1.0, 0.0, 2.0)) + ","
        "\"text_split_method\": \"" + splitMethod + "\","
        "\"batch_size\": 1,"
        "\"batch_threshold\": 0.75,"
        "\"split_bucket\": true,"
        "\"speed_factor\": " + numStr(GetIniDbl(L"TTS", L"TtsSpeed", 1.0, 0.5, 2.0)) + ","
        "\"fragment_interval\": 0.3,"
        "\"seed\": -1,"
        "\"media_type\": \"wav\","
        "\"streaming_mode\": false,"
        "\"parallel_infer\": true,"
        "\"repetition_penalty\": " + numStr(GetIniDbl(L"TTS", L"TtsRepetitionPenalty", 1.35, 1.0, 2.0)) + ","
        "\"sample_steps\": " + std::to_string(GetIniInt(L"TTS", L"TtsSampleSteps", 32, 1, 200)) + ","
        "\"super_sampling\": false"
        + extra +
        "}";
    TtsLog("req", "version=" + WideStringToUtf8(version)
        + " ref=" + WideStringToUtf8(refAudio)
        + " lang=" + WideStringToUtf8(textLang)
        + " chars=" + std::to_string(jaText.size()));
    // 把请求体打出来（截断），可以直接复制去 curl 复现，比猜快得多
    {
        std::string dbg = jsonBody;
        if (dbg.size() > 900) dbg = dbg.substr(0, 900) + "...(truncated)";
        TtsLog("body", dbg);
    }

    // ★ GPT-SoVITS 的端点随版本/整合包变：/tts、/v2/tts、/v1/tts、/、/api/tts、/run/predict…
    //   写死一个必然"换个包就 404"。策略：手动配置 > 自动探测 > 兜底全试。
    // ★ 探测结果按 host:port 缓存：每次对话都去拉一遍 openapi.json 纯属浪费，
    //   而且服务刚启动时还容易撞上超时。
    static std::wstring            s_probeKey;
    static std::vector<std::wstring> s_probePaths;
    std::wstring probeKey = host + L":" + std::to_wstring(port);

    std::vector<std::wstring> paths;
    auto pushPath = [&](const std::wstring& pp) {
        if (pp.empty()) return;
        if (std::find(paths.begin(), paths.end(), pp) == paths.end()) paths.push_back(pp);
    };
    if (!cfgPath.empty()) {
        pushPath(cfgPath);
    } else {
        if (probeKey != s_probeKey) {
            s_probePaths = TtsDiscoverPaths(host, port);
            s_probeKey = probeKey;
        }
        for (const auto& pp : s_probePaths) pushPath(pp);
    }
    // ★ 兜底候选里【不再放 "/"】：GPT-SoVITS 从来没有 POST / 这个端点，
    //   放进去只会让真正出错时，原因被伪装成最后一个路径的 404
    //   （你服务端日志里那行 "POST / 404" 就是这么来的，它掩盖了 /tts 的真实报错）。
    const wchar_t* fallbacks[] = { L"/tts", L"/v2/tts", L"/v1/tts", L"/api/tts", L"/run/predict" };
    for (const wchar_t* f : fallbacks) pushPath(f);

    std::vector<char> wav;
    int status = 0;
    std::wstring usedPath;
    std::string tried;
    std::string fatalBody;          // 服务端返回的错误原文（用于定位真因）
    int         fatalStatus = 0;
    std::wstring fatalPath;
    int         lastStatus = 0;
    for (const auto& pth : paths) {
        wav.clear(); status = 0;
        std::string eb;
        if (TtsPostOnce(host, port, pth, jsonBody, timeoutMs, wav, status, &eb)) { usedPath = pth; break; }
        lastStatus = status;
        tried += WideStringToUtf8(pth) + "=>" + std::to_string(status) + " ";
        // ★ 关键：只有"这个路径不存在"才继续换下一个路径试。
        //   400 / 422 / 500 说明【路径是对的】，是参数或服务端内部出错 —— 再换路径毫无意义，
        //   却会把真正的报错掩盖成最后那个路径的 404。
        bool pathMissing = (status == 0 || status == 404 || status == 405 || status == 501);
        if (!pathMissing) {
            fatalStatus = status; fatalPath = pth; fatalBody = eb;
            break;
        }
    }
    if (wav.empty()) TtsLog("fail", "已尝试路径: " + tried);
    // ★ 所有候选路径都不存在（全是 404）时 fatalStatus 还是 0，
    //   那就拿最后一个状态码来报，否则会被误读成"服务没启动"。
    if (wav.empty() && fatalStatus == 0) {
        fatalStatus = lastStatus;
        fatalPath = paths.empty() ? std::wstring() : paths.back();
    }

    if (wav.empty()) {
        // 把服务端说的话原样带出来（GPT-SoVITS 返回 {"message":"..."}）
        std::string msg = ExtractJsonString(fatalBody, "message");
        if (msg.empty()) msg = fatalBody.substr(0, (std::min)((size_t)200, fatalBody.size()));

        std::wstring e;
        if (fatalStatus != 0) {
            e = L"HTTP " + std::to_wstring(fatalStatus);
            if (!msg.empty()) {
                int n = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), (int)msg.size(), nullptr, 0);
                if (n > 0) {
                    std::wstring wm((size_t)n, L'\0');
                    MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), (int)msg.size(), &wm[0], n);
                    e += L"（" + wm + L"）";
                }
            }
        } else {
            e = L"无法连接（服务未启动或端口不对）";
        }
        TtsLog("fail", "host=" + WideStringToUtf8(host) + " port=" + std::to_string(port)
            + " path=" + WideStringToUtf8(fatalPath) + " -> " + WideStringToUtf8(e));
        if (errOut) *errOut = e;
        return false;
    }

    // 直接用 XAudio2 从内存播放，不再写临时文件
    if (!LoadWavFromMemory(wav, g_tts)) {
        TtsLog("error", "wav 解析失败 bytes=" + std::to_string(wav.size()));
        if (errOut) *errOut = L"返回的音频无法解析";
        return false;
    }
    XSoundPlay(g_tts, false);
    TtsLog("ok", "path=" + WideStringToUtf8(usedPath) + " bytes=" + std::to_string(wav.size()));
    return true;
}

// 兼容旧键名 [LLM] TTSMaxChars，新的统一放 [TTS]
static int GetTtsMaxChars() {
    std::wstring v = GetIniStr(L"LLM", L"TTSMaxChars", L"");
    if (!v.empty()) {
        wchar_t* e = nullptr;
        long d = wcstol(v.c_str(), &e, 10);
        if (e != v.c_str() && d >= 0) return (int)d;
    }
    return GetIniInt(L"TTS", L"TtsMaxChars", 600, 0, 100000);
}

static std::wstring GetLLMConfig(const wchar_t* key) {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return L"";
    std::wstring iniPath = exeDir + L"\\llm.ini";   // 用 wstring 拼路径，避免 MAX_PATH 栈数组
    std::vector<wchar_t> buf(512, L'\0');
    GetPrivateProfileStringW(L"LLM", key, L"", buf.data(), (DWORD)buf.size(), iniPath.c_str());
    return std::wstring(buf.data());
}

// 读数值型配置：ini 里没写就用 defVal，超出 [lo,hi] 就钳制
static double GetLLMConfigDouble(const wchar_t* key, double defVal, double lo, double hi) {
    std::wstring v = GetLLMConfig(key);
    if (v.empty()) return defVal;
    wchar_t* end = nullptr;
    double d = wcstod(v.c_str(), &end);
    if (end == v.c_str()) return defVal;          // 完全不是数字
    if (!(d > lo)) return lo;                     // 顺带挡住 NaN
    if (d > hi) return hi;
    return d;
}

static int GetLLMConfigInt(const wchar_t* key, int defVal, int lo, int hi) {
    std::wstring v = GetLLMConfig(key);
    if (v.empty()) return defVal;
    wchar_t* end = nullptr;
    long d = wcstol(v.c_str(), &end, 10);
    if (end == v.c_str()) return defVal;
    if (d < lo) return lo;
    if (d > (long)hi) return hi;
    return (int)d;
}

// ==================== 长文本模式 ====================
// 主人想要"一整段内容"（故事、讲解、教程）时，用默认的 3000 token 上限
// 会被生生截断，故事讲一半就没了。这里识别出来后单独放宽上限并追加指令。
static const wchar_t* LONG_KEYWORDS[] = {
    L"故事", L"童话", L"睡前", L"讲一个", L"讲个", L"讲讲", L"编一个", L"编个",
    L"写一篇", L"写个", L"写一段", L"长篇", L"长一点", L"详细", L"展开讲", L"展开",
    L"具体说说", L"说说看", L"介绍一下", L"讲解", L"教程", L"教学", L"教我",
    L"原理", L"完整", L"续写", L"接下去", L"然后呢", L"举例", L"列举",
    L"分析一下", L"为什么", L"来历", L"传说", L"背景设定", L"世界观"
};

static bool IsLongFormRequest(const std::wstring& text) {
    if (text.empty()) return false;
    if (GetLLMConfigInt(L"LongMode", 1, 0, 1) == 0) return false;
    for (const wchar_t* k : LONG_KEYWORDS)
        if (text.find(k) != std::wstring::npos) return true;
    return text.size() >= 80;      // 输入本身就一大段，大概率也要长回答
}

// 长文本模式下追加的临时指令（不写进 prompt.txt，只在本次请求里生效）
static const wchar_t* LONG_INSTRUCT =
L"【本次回复要求 - 长篇模式】\n"
L"主人这次要的是一整段完整的内容，不是一句话。请务必做到：\n"
L"1. zh 必须是完整成篇的长文：有开头、有发展、有高潮、有结尾。严禁中途停下，严禁写成提纲或要点列表。\n"
L"2. 篇幅：中文 700 到 1500 字。宁可写长，也不要草草收尾。\n"
L"3. 用具体细节撑起来：场景、动作、对话、心理、比喻都要有。不要写\"总之\"\"以上就是\"这类空洞总结句充数。\n"
L"4. 全程保持茉子的口吻和性格，长文里也要有俏皮、有情绪起伏。\n"
L"5. ja 是同一段内容的日语版；如果长度不够分配，优先保证 zh 完整。\n"
L"6. 仍然只输出一个 JSON 对象。";

// ==================== 打字机自适应 ====================
// 按字数动态决定打字速度：固定 30ms/字的话，1500 字的长文要打 45 秒，观感像卡死。
// 改成按"目标总时长"反推每字延时和每批字符数。
struct TypewriterPlan { int delayMs; int charsPerTick; };

static TypewriterPlan PlanTypewriter(size_t len) {
    int targetMs = GetLLMConfigInt(L"TypewriterTargetMs", 6000, 500, 120000);
    int minDelay = GetLLMConfigInt(L"TypewriterMinDelayMs", 2, 0, 500);
    int maxDelay = GetLLMConfigInt(L"TypewriterMaxDelayMs", TYPEWRITER_DELAY_MS, 1, 500);
    if (len == 0) return { maxDelay, 1 };
    double perChar = (double)targetMs / (double)len;
    int delay = (int)floor(perChar);
    if (delay < minDelay) delay = minDelay;
    if (delay > maxDelay) delay = maxDelay;
    int batch = 1;
    if ((double)len * (double)delay > (double)targetMs * 1.2) {
        batch = (int)ceil((double)len * (double)delay / (double)targetMs);
        if (batch < 1) batch = 1;
        if (batch > 64) batch = 64;
    }
    return { delay, batch };
}

static const char* DEFAULT_PROMPT =
"你是常陆茉子，一个住在主人电脑桌面上的桌宠少女。\n"
"【性格】活泼开朗、好奇心强、有点粘人、爱撒娇，情绪真实外露，会开心会惊讶也会小小抱怨。\n"
"【口吻】像跟很亲近的人聊天，自然口语化。禁止客服腔、禁止官方辞令、禁止空洞客套。\n"
"\n"
"【回复要求 - 最重要】\n"
"1. 默认每次回复至少 2 到 4 句话，把话说完整说具体，不要一两句就结束。\n"
"2. 【篇幅自适应 - 关键】篇幅要跟着主人的需求走，不要一律说短：\n"
"   - 主人让你讲故事、写一段、详细说、讲解、介绍、教他什么 → 必须写完整成篇的长文，中文 700 到 1500 字，\n"
"     有开头、有发展、有结尾，细节充分，严禁中途停下或写成提纲。\n"
"   - 主人明确说\"简短\"\"一句话\"\"随便说两句\" → 才可以短。\n"
"   - 其余日常闲聊 → 2 到 5 句，有内容有情绪即可。\n"
"3. 严禁空洞短回复，例如\"好的呢\"\"我明白了\"\"有什么可以帮你的吗\"\"嗯嗯\"这类没有信息量的话。\n"
"4. 主动延伸话题：追问主人在做什么、评论主人刚说的话、分享自己的感受和联想、提个小建议或撒个娇。\n"
"5. 不要使用Markdown、不要列表、不要用括号描写动作，就像真的在说话。\n"
"6. 每句话都要有实际内容或真实情绪，不要重复自己说过的话。\n"
"\n"
"【输出格式】只返回一个纯JSON对象，不要任何解释、不要Markdown代码块、不要```符号。\n"
"格式严格为：{\"zh\":\"中文回复\",\"ja\":\"意思相同的日语回复，用常陆茉子口吻\"}\n"
"规则：zh和ja都不能为空；ja要与zh同样饱满有感情，不要缩短；\n"
"内部双引号必须转义为\\\"，换行用\\n；如果用户说的不是中文，也先按中文理解再生成zh和ja。";

static std::wstring LoadPrompt() {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return Utf8ToWideString(DEFAULT_PROMPT);
    std::wstring promptPath = exeDir + L"\\prompt.txt";
    if (!PathFileExistsW(promptPath.c_str())) return Utf8ToWideString(DEFAULT_PROMPT);
    FILE* f = nullptr;
    _wfopen_s(&f, promptPath.c_str(), L"r, ccs=UTF-8");
    if (!f) return Utf8ToWideString(DEFAULT_PROMPT);
    std::vector<wchar_t> content(16384, L'\0');     // 堆分配，避免大栈占用
#pragma warning(suppress:6031)                      // 读取失败时 content 全 0，下面有兜底
    size_t nread = fread(content.data(), sizeof(wchar_t), 16383, f);
    fclose(f);
    std::wstring promptW(content.data(), nread);
    return promptW.empty() ? Utf8ToWideString(DEFAULT_PROMPT) : promptW;
}

static std::wstring LoadChatHistory() {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return L"";
    std::wstring historyPath = exeDir + L"\\chat_history.txt";
    if (!PathFileExistsW(historyPath.c_str())) return L"";
    FILE* f = nullptr;
    _wfopen_s(&f, historyPath.c_str(), L"r, ccs=UTF-8");
    if (!f) return L"";
    std::vector<wchar_t> content(32768, L'\0');     // 原为 64KB 栈数组，改堆分配
#pragma warning(suppress:6031)
    size_t nread = fread(content.data(), sizeof(wchar_t), 32767, f);
    fclose(f);
    std::wstring history(content.data(), nread);
    size_t pos = 0;
    while ((pos = history.find(L"\n", pos)) != std::wstring::npos) {
        if (pos == 0 || history[pos - 1] != L'\r') { history.replace(pos, 1, L"\r\n"); pos += 2; }
        else pos += 1;
    }
    return history;
}

static void SaveChatHistory(const std::wstring& userInput, const std::wstring& reply) {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return;
    std::wstring historyPath = exeDir + L"\\chat_history.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, historyPath.c_str(), L"a, ccs=UTF-8");
    if (f) {
        std::wstring record = L"主人：" + userInput + L"\r\n\r\n" + L"茉子：" + reply + L"\r\n\r\n";
#pragma warning(suppress:6031)                      // 写失败不影响主流程
        fwrite(record.c_str(), sizeof(wchar_t), record.length(), f);
        fclose(f);
    }
}

struct LLMThreadParam { HWND hwnd; std::wstring input; };

static unsigned __stdcall LLMThreadProc(void* param) {
    if (!param) return 0;                       // 防御空指针：静默返回，避免解引用崩溃
    LLMThreadParam* p = (LLMThreadParam*)param;
    HWND hwnd = p->hwnd;
    std::wstring userInput = p->input;
    delete p;

    std::wstring apiUrl = GetLLMConfig(L"ApiUrl");
    std::wstring apiKey = GetLLMConfig(L"ApiKey");
    std::wstring model = GetLLMConfig(L"Model");

    if (apiUrl.empty() || apiKey.empty()) {
        auto* msg = new std::wstring(L"请在 llm.ini 中配置 ApiUrl 和 ApiKey");
        if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)msg)) delete msg;
        PostMessageW(hwnd, WM_STREAM_DONE, 0, 0);
        return 0;
    }

    URL_COMPONENTS urlComp = { sizeof(urlComp) };
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    std::wstring urlBuf = apiUrl;                 // 放堆上，避免函数栈占用过大
    if (urlBuf.empty()) { PostMessageW(hwnd, WM_STREAM_DONE, 0, 0); return 0; }
    if (!WinHttpCrackUrl(&urlBuf[0], (DWORD)urlBuf.size(), 0, &urlComp)) {
        auto* msg = new std::wstring(L"ApiUrl 格式错误，请检查 llm.ini");
        if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)msg)) delete msg;
        PostMessageW(hwnd, WM_STREAM_DONE, 0, 0);
        return 0;
    }

    wchar_t hostName[256] = { 0 };
    if (urlComp.lpszHostName && urlComp.dwHostNameLength > 0)
        wcsncpy_s(hostName, urlComp.lpszHostName, min(urlComp.dwHostNameLength, 255u));
    wchar_t urlPath[256] = { 0 };
    if (urlComp.lpszUrlPath && urlComp.dwUrlPathLength > 0)
        wcsncpy_s(urlPath, urlComp.lpszUrlPath, min(urlComp.dwUrlPathLength, 255u));

    std::wstring prompt = LoadPrompt();
    std::wstring history = LoadChatHistory();
    size_t histMax = (size_t)GetLLMConfigInt(L"HistoryMaxChars", 4000, 500, 60000);
    if (history.size() > histMax) history = history.substr(history.size() - histMax);
    // ★ 历史放进独立的 system 消息，而不是塞成一条 assistant 消息。
    //   不能把整段历史原样发回：模型会把"主人：xx / 茉子：xx"当成自己以前说过的话，
    //   于是它开始模仿历史里的纯文本格式，不再输出要求的 JSON，回复也变得又短又乱。
    std::wstring contextStr = L"";
    if (!history.empty()) {
        contextStr = L",{\"role\":\"system\",\"content\":\""
            + EscapeJsonW(L"【以下是你和主人过去的对话记录，只用来了解上下文与你们的相处氛围，"
                L"不要模仿它的书写格式，不要照抄里面的句子，你这次的回复仍必须严格输出规定的JSON】\n")
            + EscapeJsonW(history) + L"\"}";
    }

    // ★ 采样参数：从 llm.ini 读，没写就用默认值（原代码是硬编码的，改 ini 根本无效）
    double temperature = GetLLMConfigDouble(L"Temperature", 0.9, 0.0, 2.0);
    double topP = GetLLMConfigDouble(L"TopP", 0.95, 0.0, 1.0);
    double presencePen = GetLLMConfigDouble(L"PresencePenalty", 0.6, -2.0, 2.0);
    double freqPen = GetLLMConfigDouble(L"FrequencyPenalty", 0.3, -2.0, 2.0);
    int    maxTokens = GetLLMConfigInt(L"MaxTokens", 3000, 256, 16384);
    int    enableThink = GetLLMConfigInt(L"EnableThinking", 0, 0, 1);

    // ★ 长文本模式：放宽上限 + 追加一次性指令
    bool longForm = IsLongFormRequest(userInput);
    if (longForm) {
        int longTok = GetLLMConfigInt(L"LongMaxTokens", 8000, 256, 32768);
        if (longTok > maxTokens) maxTokens = longTok;
        prompt = prompt + L"\n\n" + LONG_INSTRUCT;
        PetDbgW(L"[LLM] 长文本模式：已放宽 max_tokens 并追加长篇指令\n");
    }
    g_lastJsonTruncated = false;

    wchar_t numBuf[64];
    auto numToW = [&](double d) -> std::wstring {
        swprintf_s(numBuf, L"%.3f", d);
        std::wstring r(numBuf);
        while (r.size() > 1 && r.back() == L'0') r.pop_back();   // 去掉多余尾零
        if (!r.empty() && r.back() == L'.') r.pop_back();
        return r;
        };

    std::wstring jsonBodyW = L"{\"model\":\"" + model +
        L"\",\"messages\":[{\"role\":\"system\",\"content\":\"" + EscapeJsonW(prompt) +
        L"\"}" + contextStr + L",{\"role\":\"user\",\"content\":\"" + EscapeJsonW(userInput) +
        L"\"}],\"stream\":false,\"max_tokens\":" + std::to_wstring(maxTokens) +
        L",\"temperature\":" + numToW(temperature) +
        L",\"top_p\":" + numToW(topP) +
        L",\"presence_penalty\":" + numToW(presencePen) +
        L",\"frequency_penalty\":" + numToW(freqPen) +
        L",\"response_format\":{\"type\":\"json_object\"},\"chat_template_kwargs\":{\"enable_thinking\":"
        + (enableThink ? L"true" : L"false") + L"}}";


    std::string jsonBodyUtf8 = WideStringToUtf8(jsonBodyW);

    HINTERNET hSession = WinHttpOpen(L"MakotoPet/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        auto* msg = new std::wstring(L"网络初始化失败（WinHttpOpen）");
        if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)msg)) delete msg;
        PostMessageW(hwnd, WM_STREAM_DONE, 0, 0);
        return 0;
    }
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        auto* msg = new std::wstring(L"无法连接服务器，请检查 ApiUrl");
        if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)msg)) delete msg;
        PostMessageW(hwnd, WM_STREAM_DONE, 0, 0);
        return 0;
    }
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", urlPath, NULL, NULL, NULL,
        urlComp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        auto* msg = new std::wstring(L"创建请求失败（WinHttpOpenRequest）");
        if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)msg)) delete msg;
        PostMessageW(hwnd, WM_STREAM_DONE, 0, 0);
        return 0;
    }

    std::wstring authHeader = L"Authorization: Bearer " + apiKey;
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json; charset=utf-8", -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)jsonBodyUtf8.c_str(), (DWORD)jsonBodyUtf8.length(), (DWORD)jsonBodyUtf8.length(), 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        auto* msg = new std::wstring(L"发送请求失败（WinHttpSendRequest）");
        if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)msg)) delete msg;
        PostMessageW(hwnd, WM_STREAM_DONE, 0, 0);
        return 0;
    }
    WinHttpReceiveResponse(hRequest, NULL);

    std::string fullResponse;
    DWORD dwSize = 0, dwDownloaded = 0;
    for (;;) {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
        std::vector<char> buffer(dwSize);
        if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded) || dwDownloaded == 0) break;
        fullResponse.append(buffer.data(), dwDownloaded);
    }
    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);

    if (fullResponse.find("\"error\"") != std::string::npos) {
        // ★ 完整响应里包含对话正文，默认不输出，排查时才开 PET_ENABLE_DEBUG_LOG
        PetDbgA("[LLM] error response begin\n");
        PetDbgA(fullResponse.c_str());
        PetDbgA("\n[LLM] error response end\n");
    }

    std::string assistantRaw = ExtractAssistantContent(fullResponse);
    PetDbgA("[LLM] raw content begin\n");
    PetDbgA(assistantRaw.c_str());
    PetDbgA("\n[LLM] raw content end\n");

    std::string jsonObj = CleanToJsonObject(assistantRaw);
    std::string zhUtf8 = ExtractJsonString(jsonObj, "zh");
    std::string jaUtf8 = ExtractJsonString(jsonObj, "ja");

    // ★ 好感度由模型这一轮给出的增减值决定（缺字段时退回 +1，保证仍有推进）。
    //   结算放在打字机跑完、历史已存档之后，界面上的顺序才不会串。
    int affDelta = ExtractJsonInt(jsonObj, "affection", 1, -3, 5);
    {
        // ★ 排查用：VS 输出窗口搜 [AFF] 就能看到模型这一轮到底给了几分
        char dbg[192];
        _snprintf_s(dbg, sizeof dbg, _TRUNCATE,
            "[AFF] delta=%d  today=%d/%d  total=%d  mood=%d  shown=%d/%d  zhLen=%d\n",
            affDelta, g_stat.todayAffinity, AFFINITY_DAY_CAP,
            g_stat.affinityTotal, g_stat.affinityMood, AffinityShown(), AFFINITY_CAP,
            (int)zhUtf8.size());
        PetDbgA(dbg);
    }

    if (zhUtf8.empty() && !assistantRaw.empty() && assistantRaw.find('{') == std::string::npos)
        zhUtf8 = assistantRaw;
    if (zhUtf8.empty())
        zhUtf8 = "（解析回复失败，开启 PET_ENABLE_DEBUG_LOG 可在 VS 输出窗口看到原始 content）";

    std::wstring zhReply = Utf8ToWideString(zhUtf8);
    std::wstring jaReply = Utf8ToWideString(jaUtf8);

    if (g_lastJsonTruncated && !zhReply.empty())
        zhReply += L"\n（…这次回复达到长度上限被截断了，想看完整版请把 llm.ini 的 LongMaxTokens 调大）";

    PostMessageW(hwnd, WM_REMOVE_THINKING, 0, 0);

    // ★ 打字机自适应：长回复自动加速并批量送出，1500 字不再要等 45 秒
    TypewriterPlan tp = PlanTypewriter(zhReply.size());
    for (size_t i = 0; i < zhReply.size();) {
        if (!IsWindow(hwnd)) break;
        size_t n = (size_t)tp.charsPerTick;
        if (i + n > zhReply.size()) n = zhReply.size() - i;
        auto* ch = new std::wstring(zhReply.substr(i, n));
        if (!PostMessageW(hwnd, WM_APPEND_CHAR, 0, (LPARAM)ch)) delete ch;
        i += n;
        Sleep((DWORD)tp.delayMs);
    }

    auto* replyMsg = new std::wstring(zhReply);
    if (!PostMessageW(hwnd, WM_STREAM_DONE, 0, (LPARAM)replyMsg)) delete replyMsg;
    // ★ WM_APP_AFFINITY 的处理函数在【主窗口 WndProc】里，聊天窗口 ChatWndProc 没有这个 case。
    //   必须发给主窗口：聊天窗口不处理这条消息，会被 DefWindowProc 丢弃，
    //   结果是好感度永远不累加、解锁提示也不会弹出。
    HWND affHwnd = (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd : hwnd;
    PostMessageW(affHwnd, WM_APP_AFFINITY, (WPARAM)(affDelta + AFFINITY_MSG_BIAS), 0);

    // ★ TTS 放在"聊天结束"之后再跑：就算合成慢或卡住，也不会把界面卡在"正在思考"。
    //   超长文本默认跳过（合成要等很久），上限默认 600 字，可在 [TTS] TtsMaxChars 调整。
    int ttsMax = GetTtsMaxChars();
    if (!jaReply.empty() && IsWindow(hwnd)) {
        if ((int)jaReply.size() > ttsMax) {
            TtsLog("skip", "日语 " + std::to_string(jaReply.size())
                + " 字 > TtsMaxChars=" + std::to_string(ttsMax));
        } else {
            std::wstring err;
            if (!CallTTSService(jaReply, &err) && !err.empty()
                && GetIniInt(L"TTS", L"TtsShowError", 1, 0, 1)) {
                auto* em = new std::wstring(L"（语音合成失败：" + err + L"，详情见 VS 输出窗口 [TTS]）");
                if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)em)) delete em;
            }
        }
    }

    return 0;
}

static LRESULT CALLBACK ChatWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hHistoryEdit = CreateWindowExW(0, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
            10, 10, 460, 240, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_hInputEdit = CreateWindowExW(0, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN,
            10, 260, 350, 80, hwnd, (HMENU)102, GetModuleHandleW(nullptr), nullptr);
        g_hSendBtn = CreateWindowExW(0, L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            370, 260, 100, 35, hwnd, (HMENU)103, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"退出",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            370, 305, 100, 35, hwnd, (HMENU)104, GetModuleHandleW(nullptr), nullptr);
        HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_DONTCARE, L"Segoe UI Emoji");
        SendMessageW(g_hHistoryEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(g_hInputEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        std::wstring history = LoadChatHistory();
        SetWindowTextW(g_hHistoryEdit, history.empty() ? L"（暂无对话历史，开始和茉子聊天吧~）\r\n\r\n" : history.c_str());
        SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
        break;
    }
    case WM_SIZE: {
        if (g_hHistoryEdit && g_hInputEdit && g_hSendBtn) {
            int cx = LOWORD(lParam), cy = HIWORD(lParam);
            int historyH = cy - 270;
            if (historyH < 50) historyH = 50;
            MoveWindow(g_hHistoryEdit, 10, 10, cx - 20, historyH, TRUE);
            MoveWindow(g_hInputEdit, 10, cy - 260, cx - 140, 80, TRUE);
            MoveWindow(g_hSendBtn, cx - 120, cy - 260, 100, 35, TRUE);
            HWND hExitBtn = GetDlgItem(hwnd, 104);
            if (hExitBtn) MoveWindow(hExitBtn, cx - 120, cy - 215, 100, 35, TRUE);
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 400; mmi->ptMinTrackSize.y = 350;
        break;
    }
    case WM_APPEND_TEXT: {
        std::wstring* text = (std::wstring*)lParam;
        if (text && g_hHistoryEdit) {
            DWORD end = GetWindowTextLengthW(g_hHistoryEdit);
            SendMessageW(g_hHistoryEdit, EM_SETSEL, (WPARAM)end, (LPARAM)end);
            SendMessageW(g_hHistoryEdit, EM_REPLACESEL, TRUE, (LPARAM)text->c_str());
            SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
        }
        delete text; break;
    }
    case WM_REMOVE_THINKING:
        if (g_hHistoryEdit) {
            int len = GetWindowTextLengthW(g_hHistoryEdit);
            std::wstring currentText(len, L'\0');
            GetWindowTextW(g_hHistoryEdit, &currentText[0], len + 1);
            size_t pos = currentText.rfind(L"茉子：");
            if (pos != std::wstring::npos) {
                currentText.erase(pos + 3);
                SetWindowTextW(g_hHistoryEdit, currentText.c_str());
            }
        }
        break;
    case WM_APPEND_CHAR: {
        std::wstring* str = (std::wstring*)lParam;
        if (str && g_hHistoryEdit) {
            DWORD end = GetWindowTextLengthW(g_hHistoryEdit);
            SendMessageW(g_hHistoryEdit, EM_SETSEL, (WPARAM)end, (LPARAM)end);
            SendMessageW(g_hHistoryEdit, EM_REPLACESEL, TRUE, (LPARAM)str->c_str());
            SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
        }
        delete str; break;
    }
    case WM_STREAM_DONE: {
        std::wstring* fullReply = (std::wstring*)lParam;
        if (fullReply) {
            int len = GetWindowTextLengthW(g_hHistoryEdit);
            std::wstring currentText(len, L'\0');
            GetWindowTextW(g_hHistoryEdit, &currentText[0], len + 1);
            size_t thinkPos = currentText.rfind(L"思考中...");
            if (thinkPos != std::wstring::npos) {
                currentText.erase(thinkPos - 3, currentText.length() - (thinkPos - 3));
                currentText += L"（未收到回复）";
                SetWindowTextW(g_hHistoryEdit, currentText.c_str());
            }
            size_t pos = currentText.rfind(L"主人：");
            if (pos != std::wstring::npos) {
                size_t endPos = currentText.find(L"\r\n\r\n茉子：", pos);
                if (endPos != std::wstring::npos) {
                    std::wstring userInput = currentText.substr(pos + 3, endPos - pos - 3);
                    SaveChatHistory(userInput, *fullReply);
                }
            }
            delete fullReply;
        }
        g_isChatting = false;
        if (IsWindow(g_hSendBtn)) EnableWindow(g_hSendBtn, TRUE);
        if (IsWindow(g_hInputEdit)) SetFocus(g_hInputEdit);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 103 && !g_isChatting) {
            std::vector<wchar_t> buf(512, L'\0');
            GetWindowTextW(g_hInputEdit, buf.data(), (int)buf.size());
            if (buf[0] == 0) break;
            g_isChatting = true;
            g_stat.chatChars += (int)wcslen(buf.data());   // ★ 累计聊天字数（只统计主人发送）
            // ★ 好感度不再在这里固定 +1：改由模型回复里的 affection 字段决定，
            //   在收到回复后通过 WM_APP_AFFINITY 结算（见 LLMThreadProc）
            AchCheck();
            EnableWindow(g_hSendBtn, FALSE);
            SetWindowTextW(g_hInputEdit, L"");
            // ★ 原写法 std::wstring currentText(len) 后要求 Win32 写入 len+1 个 wchar_t，
            //   最后一个 '\0' 落在缓冲区外 —— 属于越界写，会破坏堆。改用显式 len+1 的缓冲。
            int len = GetWindowTextLengthW(g_hHistoryEdit);
            std::vector<wchar_t> histBuf((size_t)len + 1, L'\0');
            GetWindowTextW(g_hHistoryEdit, histBuf.data(), len + 1);
            std::wstring currentText(histBuf.data());
            std::wstring userText(buf.data());          // vector 不能隐式转 wstring，必须取 .data()
            currentText += L"主人：" + userText + L"\r\n\r\n茉子：思考中...";
            SetWindowTextW(g_hHistoryEdit, currentText.c_str());
            SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
            LLMThreadParam* param = new LLMThreadParam();
            param->hwnd = hwnd; param->input = userText;
            _beginthreadex(nullptr, 0, LLMThreadProc, param, 0, nullptr);
        }
        else if (LOWORD(wParam) == 104) {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    case WM_APP_AFFINITY:
        // 兜底：这条消息由主窗口处理。万一被发到聊天窗口，转发过去而不是丢掉。
        if (g_hwnd && IsWindow(g_hwnd)) SendMessageW(g_hwnd, WM_APP_AFFINITY, wParam, lParam);
        return 0;
    case WM_CLOSE:
        g_hChatWnd = nullptr; g_isChatting = false;
        DestroyWindow(hwnd); break;
    case WM_DESTROY:
        g_hChatWnd = nullptr; g_isChatting = false;
        break;
    default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void OpenChatDialog() {
    if (g_hChatWnd && IsWindow(g_hChatWnd)) { SetForegroundWindow(g_hChatWnd); return; }
    if (!g_richEditLoaded) { LoadLibraryW(L"Msftedit.dll"); g_richEditLoaded = true; }
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = ChatWndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MakotoChatWndClass"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.cbSize = sizeof(WNDCLASSEXW);
    RegisterClassExW(&wc);
    RECT ownerRect; GetWinRect(g_hwnd, ownerRect);
    int x = ownerRect.left + (ownerRect.right - ownerRect.left) / 2 - 240;
    int y = ownerRect.top + (ownerRect.bottom - ownerRect.top) / 2 - 260;
    g_hChatWnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"MakotoChatWndClass", L"💬 和茉子聊天",
        WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE | WS_THICKFRAME | WS_MAXIMIZEBOX,
        x, y, 490, 520, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
}
// ================= RVC 翻唱系统 =================
// ============================================================
//  RVC 翻唱模块（改造版）
//  流程：POST /convert (separate=1) → 后台自动「人声伴奏分离」→「翻唱人声」
//        → 桌宠同时拿到 翻唱人声 + 伴奏 → 内存混音 → 播放
//  全程异步轮询，不阻塞 UI；分离跑在 bridge 子进程里，显存不打架
// ============================================================

// 服务地址 / 端口 / 模型名：填 RVC 服务所在机器与浏览器里看到的模型名。
// 这三个是编译期常量，没有单独的配置文件，改完重新编译即可。
static const wchar_t* RVC_MODEL = L"mako.pth";
static const wchar_t* RVC_HOST  = L"127.0.0.1";
static const int     RVC_PORT  = 5050;

// ---------- 阶段枚举与状态文字见文件顶部「RVC 转换状态显示」唯一定义 ----------

// ---------- JSON 字段提取 ----------
static std::string rvc_extract_file(const std::string& json) {
    auto p = json.find("\"file\"");
    if (p == std::string::npos) return "";
    p = json.find("\"", p + 6);
    if (p == std::string::npos) return "";
    auto e = json.find("\"", p + 1);
    if (e == std::string::npos) return "";
    return json.substr(p + 1, e - p - 1);
}

static std::string rvc_extract_string(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":";
    auto p = json.find(searchKey);
    if (p == std::string::npos) return "";
    p += searchKey.length();
    while (p < json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= json.length()) return "";
    if (json[p] == '"') {
        p++;
        auto e = json.find('"', p);
        if (e == std::string::npos) return "";
        return json.substr(p, e - p);
    } else if (json[p] == 't' || json[p] == 'f') {
        return (json[p] == 't') ? "true" : "false";
    }
    return "";
}

// 提取数值型字段（如 "progress": 45），失败返回 -1
static int rvc_extract_int(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":";
    auto p = json.find(searchKey);
    if (p == std::string::npos) return -1;
    p += searchKey.length();
    while (p < json.length() && (json[p] == ' ' || json[p] == '\t')) p++;
    if (p >= json.length()) return -1;
    if (json[p] == '"') {
        p++;
        auto e = json.find('"', p);
        if (e == std::string::npos) return -1;
        return atoi(json.substr(p, e - p).c_str());
    }
    bool neg = false;
    if (json[p] == '-') { neg = true; p++; }
    if (p >= json.length() || !isdigit((unsigned char)json[p])) return -1;
    int v = 0;
    while (p < json.length() && isdigit((unsigned char)json[p])) { v = v * 10 + (json[p] - '0'); p++; }
    return neg ? -v : v;
}

// ---------- WinHTTP 工具 ----------
static bool WinHttpDownload(HINTERNET hConnect, const wchar_t* path, std::vector<char>& out) {
    out.clear();
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, 0);
    if (!hReq) return false;
    if (!WinHttpSendRequest(hReq, NULL, 0, NULL, 0, 0, 0)) { WinHttpCloseHandle(hReq); return false; }
    if (!WinHttpReceiveResponse(hReq, NULL)) { WinHttpCloseHandle(hReq); return false; }
    DWORD dwSize = 0, dwDownloaded = 0;
    while (WinHttpQueryDataAvailable(hReq, &dwSize) && dwSize > 0) {
        std::vector<char> buf(dwSize);
        if (!WinHttpReadData(hReq, buf.data(), dwSize, &dwDownloaded) || dwDownloaded == 0) break;
        out.insert(out.end(), buf.begin(), buf.begin() + dwDownloaded);
    }
    WinHttpCloseHandle(hReq);
    return !out.empty();
}

static std::string WinHttpGetText(HINTERNET hConnect, const wchar_t* path) {
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, NULL, NULL, NULL, 0);
    if (!hReq) return "";
    if (!WinHttpSendRequest(hReq, NULL, 0, NULL, 0, 0, 0)) { WinHttpCloseHandle(hReq); return ""; }
    if (!WinHttpReceiveResponse(hReq, NULL)) { WinHttpCloseHandle(hReq); return ""; }
    std::string resp;
    DWORD dwSize = 0, dwDownloaded = 0;
    while (WinHttpQueryDataAvailable(hReq, &dwSize) && dwSize > 0) {
        std::vector<char> buf(dwSize);
        if (!WinHttpReadData(hReq, buf.data(), dwSize, &dwDownloaded) || dwDownloaded == 0) break;
        resp.append(buf.data(), dwDownloaded);
    }
    WinHttpCloseHandle(hReq);
    return resp;
}

// 检查 RVC 服务是否在线（HEAD /models）
static bool RvcCheckService() {
    HINTERNET hSess = WinHttpOpenAuto(RVC_HOST);   // ★ RVC 也是本地服务，同样要绕过系统代理
    if (!hSess) return false;
    HINTERNET hConn = WinHttpConnect(hSess, RVC_HOST, RVC_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return false; }
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"HEAD", L"/models", NULL, NULL, NULL, 0);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false; }
    bool ok = WinHttpSendRequest(hReq, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(hReq, NULL);
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);
    return ok;
}

// ---------- WAV PCM 混音（翻唱人声 + 伴奏叠加） ----------
// 支持 16bit/24bit/32bit PCM，自动归一化防溢出
struct WavData {
    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;
    int formatTag = 0;        // 1=PCM, 3=IEEE_FLOAT, 0xFFFE=EXTENSIBLE
    int numSamples = 0;       // 单声道样本数
    std::vector<int32_t> samples;  // 统一转成 32bit 交错存储
};

static bool ParseWav(const std::vector<char>& wav, WavData& out) {
    out = WavData();
    if (wav.size() < 44) return false;
    if (memcmp(wav.data(), "RIFF", 4) != 0 || memcmp(wav.data() + 8, "WAVE", 4) != 0) return false;

    DWORD pos = 12;
    int byteRate = 0, blockAlign = 0;
    std::vector<uint8_t> rawPcm;
    while (pos + 8 <= wav.size()) {
        char cid[4]; DWORD cs;
        memcpy(cid, wav.data() + pos, 4);
        memcpy(&cs, wav.data() + pos + 4, 4);
        if (!memcmp(cid, "fmt ", 4)) {
            WAVEFORMATEX tmp{};
            DWORD tr = (DWORD)min((size_t)cs, sizeof(WAVEFORMATEX));
            memcpy(&tmp, wav.data() + pos + 8, tr);
            out.sampleRate = tmp.nSamplesPerSec;
            out.channels = tmp.nChannels;
            out.bitsPerSample = tmp.wBitsPerSample;
            out.formatTag = tmp.wFormatTag;
            blockAlign = tmp.nBlockAlign;
        } else if (!memcmp(cid, "data", 4)) {
            // ★ data 声明长度可能超过实际字节数，必须裁剪，否则越界读导致垃圾/崩溃
            size_t avail = 0;
            if (wav.size() > (size_t)pos + 8) avail = wav.size() - ((size_t)pos + 8);
            DWORD take = (cs < (DWORD)avail) ? cs : (DWORD)avail;
            if (take > 0) {
                rawPcm.assign((const uint8_t*)(wav.data() + pos + 8),
                              (const uint8_t*)(wav.data() + pos + 8) + take);
            }
        }
        pos += 8 + cs + (cs & 1);
    }
    if (out.sampleRate == 0 || out.channels == 0 || out.bitsPerSample == 0 || rawPcm.empty()) return false;

    int bytesPerSample = out.bitsPerSample / 8;
    if (blockAlign == 0) blockAlign = bytesPerSample * out.channels;
    int frameSize = blockAlign;
    if (bytesPerSample <= 0 || frameSize <= 0) return false;   // 防止除零
    int numFrames = (int)(rawPcm.size() / frameSize);
    out.numSamples = numFrames;
    out.samples.resize((size_t)numFrames * out.channels);

    auto getSample = [&](const uint8_t* p, int bps) -> int32_t {
        int32_t v = 0;
        if (bps == 2) {
            v = (int32_t)*(const int16_t*)p;
        } else if (bps == 3) {
            int32_t s = p[0] | (p[1] << 8) | (p[2] << 16);
            if (s & 0x800000) s |= (int32_t)~0xffffff;
            v = s;
        } else if (bps == 4) {
            // ★ PyMSS 分离出的伴奏是 32 位浮点 WAV(WAVE_FORMAT_IEEE_FLOAT)。
            //   若按 int32 直接解释浮点位模式，会读出天文数字 → 混音后变成刺耳杂音。
            if (out.formatTag != 1) {
                float fv = 0.0f;
                memcpy(&fv, p, 4);
                if (!(fv == fv)) fv = 0.0f;                 // NaN 保护
                if (fv >  1.0f) fv =  1.0f;
                if (fv < -1.0f) fv = -1.0f;
                v = (int32_t)(fv * 32767.0f);
            } else {
                v = *(const int32_t*)p;
            }
        } else if (bps == 1) {
            v = (int32_t)(p[0] - 128) << 8;  // 8bit 无符号 → 有符号
        }
        return v;
    };

    for (int f = 0; f < numFrames; f++) {
        const uint8_t* fp = rawPcm.data() + f * frameSize;
        for (int c = 0; c < out.channels; c++) {
            out.samples[(size_t)f * out.channels + c] = getSample(fp + c * bytesPerSample, bytesPerSample);
        }
    }
    return true;
}

// 线性重采样：把单声道样本序列从 srcRate 变换到 dstRate
static std::vector<int32_t> ResampleMono(const std::vector<int32_t>& src, int srcRate, int dstRate) {
    if (src.empty() || srcRate <= 0 || dstRate <= 0 || srcRate == dstRate) return src;
    double ratio = (double)srcRate / (double)dstRate;
    std::vector<int32_t> out((size_t)((double)src.size() / ratio) + 1);
    for (size_t i = 0; i < out.size(); i++) {
        double idx = (double)i * ratio;
        size_t i0 = (size_t)idx;
        // ★ 升采样时 i0 可能等于 src.size()，直接取下标会触发
        //   "vector subscript out of range" 断言，必须钳制到最后一个有效下标
        if (i0 >= src.size()) i0 = src.size() - 1;
        size_t i1 = (i0 + 1 < src.size()) ? (i0 + 1) : i0;
        double f = idx - (double)i0;
        if (f < 0.0) f = 0.0; else if (f > 1.0) f = 1.0;
        out[i] = (int32_t)((double)src[i0] * (1.0 - f) + (double)src[i1] * f);
    }
    return out;
}

// 在内存中直接构造 16bit 单声道 WAV（去掉临时文件，避免权限与残留问题）
static bool BuildWav16Mono(const std::vector<int32_t>& mono, int sampleRate, std::vector<char>& out) {
    if (sampleRate <= 0 || mono.empty()) return false;
    DWORD dataSize = (DWORD)(mono.size() * 2);
    std::vector<uint8_t> f;
    f.reserve(44 + dataSize);
    auto w4  = [&](const char* t) { f.insert(f.end(), t, t + 4); };
    auto w32 = [&](DWORD v) { f.insert(f.end(), (uint8_t*)&v, (uint8_t*)&v + 4); };
    auto w16 = [&](WORD v) { f.insert(f.end(), (uint8_t*)&v, (uint8_t*)&v + 2); };
    w4("RIFF"); w32(36 + dataSize); w4("WAVE");
    w4("fmt "); w32(16); w16(1); w16(1);
    w32((DWORD)sampleRate); w32((DWORD)sampleRate * 2); w16(2); w16(16);
    w4("data"); w32(dataSize);
    for (size_t i = 0; i < mono.size(); i++) {
        int32_t v = mono[i];
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        int16_t sv = (int16_t)v;
        f.insert(f.end(), (uint8_t*)&sv, (uint8_t*)&sv + 2);
    }
    out.assign((const char*)f.data(), (const char*)f.data() + f.size());
    return true;
}

// 把两段 WAV（翻唱人声 + 伴奏）混音成一段单声道 WAV，输出为 wav 字节流
static bool MixTwoWavs(const std::vector<char>& vocalWav, const std::vector<char>& insWav,
                       std::vector<char>& outMixed) {
    outMixed.clear();
    WavData vd, id;
    if (!ParseWav(vocalWav, vd)) {
        // 只有伴奏时，直接返回伴奏
        if (ParseWav(insWav, id)) {
            outMixed.assign(insWav.begin(), insWav.end());
            return true;
        }
        return false;
    }
    if (!ParseWav(insWav, id)) {
        // 没有伴奏，直接返回人声
        outMixed.assign(vocalWav.begin(), vocalWav.end());
        return true;
    }

    // 统一到较高采样率：采样率不同时做线性重采样，避免人声与伴奏错位
    int outRate = max(vd.sampleRate, id.sampleRate);

    // 先把整段都降成单声道
    auto toMonoAll = [](const WavData& w) -> std::vector<int32_t> {
        std::vector<int32_t> mono(w.numSamples);
        for (int f = 0; f < w.numSamples; f++) {
            int64_t s = 0;
            for (int c = 0; c < w.channels; c++) s += w.samples[(size_t)f * w.channels + c];
            mono[f] = (int32_t)(s / w.channels);
        }
        return mono;
    };

    std::vector<int32_t> vMono = toMonoAll(vd);
    std::vector<int32_t> iMono = toMonoAll(id);
    if (vd.sampleRate != outRate) vMono = ResampleMono(vMono, vd.sampleRate, outRate);
    if (id.sampleRate != outRate) iMono = ResampleMono(iMono, id.sampleRate, outRate);

    int frames = min((int)vMono.size(), (int)iMono.size());
    if (frames <= 0) return false;
    vMono.resize(frames);
    iMono.resize(frames);

    // 找峰值做归一化，防止叠加溢出
    int64_t peakV = 1, peakI = 1;
    for (int f = 0; f < frames; f++) {
        int64_t av = abs((int64_t)vMono[f]), ai = abs((int64_t)iMono[f]);
        if (av > peakV) peakV = av;
        if (ai > peakI) peakI = ai;
    }
    // 目标：混音后峰值不超过 30000，给两段各留 headroom
    const int64_t target = 30000;
    double scaleV = (double)target / (double)peakV;
    double scaleI = (double)target / (double)peakI;
    // 两段各占 0.7 / 0.5 权重，保证人声清晰
    double wV = 0.7 * scaleV;
    double wI = 0.5 * scaleI;
    // 防止其中一个完全静音导致权重失衡
    if (peakV <= 1) wV = 0.0;
    if (peakI <= 1) wI = 0.0;

    std::vector<int32_t> mixed(frames);
    for (int f = 0; f < frames; f++) {
        int64_t mv = (int64_t)((double)vMono[f] * wV);
        int64_t mi = (int64_t)((double)iMono[f] * wI);
        int64_t s = mv + mi;
        if (s >  32767) s = 32767;
        if (s < -32768) s = -32768;
        mixed[f] = (int32_t)s;
    }

    // 直接在内存构造 16bit 单声道 WAV
    return BuildWav16Mono(mixed, outRate, outMixed);
}

// ---------- 核心：上传 → 分离 → 翻唱 → 下载双文件 ----------
// outWav 返回「混音后」的最终音频（人声+伴奏叠加）
static bool RvcConvert(const std::wstring& wavPath, std::vector<char>& outWav) {
    // 1. 读入音频
    std::wstring path = wavPath;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER fs; GetFileSizeEx(h, &fs);
    std::vector<char> fileData((size_t)fs.QuadPart);
    if (fs.QuadPart > 0) {
        DWORD rd = 0;
        if (!ReadFile(h, fileData.data(), (DWORD)fs.QuadPart, &rd, nullptr) || rd != (DWORD)fs.QuadPart) {
            CloseHandle(h); return false;   // 读取被截断，直接失败而不是上传残数据
        }
    }
    CloseHandle(h);

    // 2. 构造 multipart/form-data（新增 separate=1）
    std::string boundary = "----MakotoPetRVC" + std::to_string(GetTickCount());
    auto make_field = [&](const std::string& name, const std::string& value) -> std::string {
        return "--" + boundary + "\r\n"
            + "Content-Disposition: form-data; name=\"" + name + "\"\r\n\r\n"
            + value + "\r\n";
    };
    std::string body;
    body += make_field("model",      WideStringToUtf8(RVC_MODEL));
    body += make_field("f0method",   "rmvpe");      
    body += make_field("pitch",      "0");
    // ★ 变调改由用户手动指定（托盘菜单「翻唱变调」），不再走 bridge 的自动判定：
    //   自动判定依赖源音高估计，男声源被误判成女声时就不会抬，结果发哑。
    //   -1 = 降 RVC_OCTAVE_SEMI 半音，0 = 原调，+1 = 升 RVC_OCTAVE_SEMI 半音
    //   （男声源配女声模型通常选 +1）。
    body += make_field("auto_pitch", "0");
    body += make_field("octave", std::to_string(g_rvcOctave));
    // ★ index_rate 不要给 1.0：检索权重越高，源音高偏离模型音域时越容易被
    //   最近邻拉到低八度样本上（表现就是高音掉八度）。
    //   同性转换可用 0.7 保音色；跨性别/大跨度由 bridge 自动再降到 0.3。
    body += make_field("index_rate", "0.5");
    body += make_field("protect",    "0.33");
    body += make_field("separate",   "1");       // ★ 告诉 bridge 先分离人声伴奏

    std::string fileName = WideStringToUtf8(std::wstring(PathFindFileNameW(path.c_str())));
    if (fileName.empty()) fileName = "audio.wav";
    std::string fileHead = "--" + boundary + "\r\n";
    fileHead += "Content-Disposition: form-data; name=\"audio\"; filename=\"" + fileName + "\"\r\n";
    fileHead += "Content-Type: audio/wav\r\n\r\n";
    std::string endBoundary = "\r\n--" + boundary + "--\r\n";

    std::vector<char> payload;
    payload.insert(payload.end(), body.begin(), body.end());
    payload.insert(payload.end(), fileHead.begin(), fileHead.end());
    payload.insert(payload.end(), fileData.begin(), fileData.end());
    payload.insert(payload.end(), endBoundary.begin(), endBoundary.end());

    // 3. 建立连接
    HINTERNET hSess = WinHttpOpenAuto(RVC_HOST);   // ★ RVC 也是本地服务，同样要绕过系统代理
    if (!hSess) return false;
    HINTERNET hConn = WinHttpConnect(hSess, RVC_HOST, RVC_PORT, 0);
    if (!hConn) { WinHttpCloseHandle(hSess); return false; }

    // 3a. POST /convert（异步 bridge 立即返回 task_id）
    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", L"/convert", NULL, NULL, NULL, 0);
    if (!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false; }

    std::wstring ct = L"Content-Type: multipart/form-data; boundary=" + Utf8ToWideString(boundary);
    WinHttpAddRequestHeaders(hReq, ct.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(hReq, NULL, 0, (LPVOID)payload.data(), (DWORD)payload.size(), (DWORD)payload.size(), 0)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false;
    }
    WinHttpReceiveResponse(hReq, NULL);

    std::string resp;
    DWORD dwSize = 0, dwDownloaded = 0;
    while (WinHttpQueryDataAvailable(hReq, &dwSize) && dwSize > 0) {
        std::vector<char> buf(dwSize);
        if (!WinHttpReadData(hReq, buf.data(), dwSize, &dwDownloaded) || dwDownloaded == 0) break;
        resp.append(buf.data(), dwDownloaded);
    }
    WinHttpCloseHandle(hReq);

    // 3b. 解析 task_id
    std::string taskId = rvc_extract_string(resp, "task_id");
    if (taskId.empty()) {
        // 兼容同步返回
        std::string file = rvc_extract_file(resp);
        if (!file.empty()) {
            std::wstring resultPath = L"/result/" + Utf8ToWideString(file);
            bool ok = WinHttpDownload(hConn, resultPath.c_str(), outWav);
            WinHttpCloseHandle(hConn);
            WinHttpCloseHandle(hSess);
            return ok;
        }
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return false;
    }

    // 3c. 轮询 /status/<task_id>，识别 separating / converting 两阶段
    std::wstring statusPath = L"/status/" + Utf8ToWideString(taskId);
    std::string resultFile, instrumentalFile;
    bool done = false;
    // 轮询退避：起步 800ms 逐步放宽到 5s；总上限 30 分钟（长歌分离+翻唱可能很久）
    const DWORD kPollTimeoutMs = 30 * 60 * 1000;
    const DWORD startTick = GetTickCount();
    int pollIntervalMs = 800;

    for (;;) {
        std::string statusResp = WinHttpGetText(hConn, statusPath.c_str());
        if (!statusResp.empty()) {
            std::string status = rvc_extract_string(statusResp, "status");
            std::string stage  = rvc_extract_string(statusResp, "stage");
            // 动态更新状态栏
            if (stage == "separating")       RvcSetStage(RvcStage::Separating);
            else if (stage == "converting")  RvcSetStage(RvcStage::Inferring);
            else if (stage == "queued")      RvcSetStage(RvcStage::Uploading);

            // 拉取进度百分比（bridge 返回 0~100）
            RvcSetProgress(rvc_extract_int(statusResp, "progress"));

            if (status == "done") {
                resultFile = rvc_extract_string(statusResp, "file");
                instrumentalFile = rvc_extract_string(statusResp, "instrumental");
                done = true;
                break;
            } else if (status == "error") {
                break;
            }
        }
        if (GetTickCount() - startTick > kPollTimeoutMs) break;   // 超时保护
        Sleep(pollIntervalMs);
        if (pollIntervalMs < 5000) pollIntervalMs += 400;         // 退避，减少无效请求
    }

    if (!done || resultFile.empty()) {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return false;
    }

    // 4. 下载翻唱人声
    std::vector<char> vocalWav;
    std::wstring vocalPath = L"/result/" + Utf8ToWideString(resultFile);
    if (!WinHttpDownload(hConn, vocalPath.c_str(), vocalWav)) {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return false;
    }

    // 5. 下载伴奏
    std::vector<char> insWav;
    if (!instrumentalFile.empty()) {
        std::wstring insPath = L"/result/" + Utf8ToWideString(instrumentalFile);
        if (!WinHttpDownload(hConn, insPath.c_str(), insWav)) {
            insWav.clear();  // 伴奏下载失败不致命，只播放人声
        }
    }

    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);

    // 6. 混音：翻唱人声 + 伴奏叠加
    if (!insWav.empty()) {
        if (!MixTwoWavs(vocalWav, insWav, outWav)) {
            outWav = vocalWav;  // 混音失败则退回纯人声
        }
    } else {
        outWav = vocalWav;
    }
    return !outWav.empty();
}

// ---------- 后台线程 ----------
static unsigned __stdcall RvcWorker(void* arg) {
    std::wstring* path = (std::wstring*)arg;
    std::wstring src = *path;
    delete path;

    RvcBusyGuard busyGuard;   // 析构时自动清除 busy，任何 return 路径都不漏

    RvcSetStage(RvcStage::Checking);
    if (!RvcCheckService()) {
        RvcSetStage(RvcStage::Failed);
        return 0;
    }

    RvcSetProgress(-1);
    std::vector<char> mixedWav;
    if (!RvcConvert(src, mixedWav)) {
        RvcSetStage(RvcStage::Failed);
        return 0;
    }

    // 成功：加载混音结果并播放
    LoadWavFromMemory(mixedWav, g_rvcCover);
    XSoundPlay(g_rvcCover, false);
    RvcSetStage(RvcStage::Done);
    g_stat.coverCount += 1;         // ★ 只有成功才计数
    StatsAddAffinity(3);            // 翻唱 +3 好感（对话才是主要来源）
    AchCheck();
    StatsSave();

    // 存盘便于试听
    std::wstring outPath = src + L".rvc_mixed.wav";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, outPath.c_str(), L"wb") == 0 && fp) {
        fwrite(mixedWav.data(), 1, mixedWav.size(), fp);
        fclose(fp);
    }

    RvcSetProgress(-1);
    return 0;
}

// 对外接口①：弹出"打开文件"对话框，让用户选一首歌来翻唱
static void RvcPickAndConvert(HWND owner) {
    if (g_rvcBusy) return;   // 正在转换，忽略
    wchar_t file[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"音频文件\0*.wav;*.mp3;*.flac;*.ogg;*.aac\0"
                      L"WAV 文件\0*.wav\0"
                      L"MP3 文件\0*.mp3\0"
                      L"所有文件\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;   // 用户取消

    std::wstring src = file;
    std::wstring name = PathFindFileNameW(src.c_str());
    // 去掉扩展名，留文件名显示
    std::wstring stem = name;
    size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);

    g_rvcBusy = true;
    {
        std::lock_guard<std::recursive_mutex> lk(g_rvcMtx);
        g_rvcSourceName = stem;
        g_rvcStage = RvcStage::Checking;
        g_rvcStatusText = L"正在连接 RVC 服务…";
        g_rvcStatusTimer = 0;
    }
    SwitchAnim(g_currentCostume, AnimState::Think);   // 思考动作 = 工作中

    // 起后台线程，把路径拷一份传进去
    std::wstring* arg = new std::wstring(src);
    HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, RvcWorker, arg, 0, nullptr);
    if (!hThread) {                       // 线程创建失败必须回滚，否则功能永久锁死
        delete arg;
        g_rvcBusy.store(false);
        RvcSetStage(RvcStage::Failed);
        return;
    }
    if (g_hRvcThread) CloseHandle(g_hRvcThread);
    g_hRvcThread = hThread;
}

// 对外接口②：直接对指定路径做翻唱（供扩展/调试用，菜单不再暴露）
static void RvcPerform(const std::wstring& srcWav) {
    if (g_rvcBusy) return;
    g_rvcBusy = true;
    std::wstring* arg = new std::wstring(srcWav);
    HANDLE hThread = (HANDLE)_beginthreadex(nullptr, 0, RvcWorker, arg, 0, nullptr);
    if (!hThread) {
        delete arg;
        g_rvcBusy.store(false);
        RvcSetStage(RvcStage::Failed);
        return;
    }
    if (g_hRvcThread) CloseHandle(g_hRvcThread);
    g_hRvcThread = hThread;
}

// ================= 右键菜单 =================
void ShowContextMenu(HWND hwnd) {
    static bool s_opening = false;
    if (s_opening) return;      // 防重入：菜单已弹出时不再嵌套弹一次
    s_opening = true;
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    HMENU outfits = CreatePopupMenu();
    HMENU volumeMenu = CreatePopupMenu();
    HMENU bgmMenu = CreatePopupMenu();
    HMENU octMenu = CreatePopupMenu();

    // ★ 养成总览：放在菜单最上面，随时能看到进度
    wchar_t headLine[64];
    int remain = 0;
    int nextId = UnlockNext(&remain);
#if DEV_UNLOCK_ALL
    swprintf(headLine, 64, L"🔧 DEV 全功能模式 · 累计好感 %d", g_stat.affinityTotal);
#else
    if (AffinityMaxed()) swprintf(headLine, 64, L"♥ 心动 Lv.MAX · 好感 %d / %d", AFFINITY_CAP, AFFINITY_CAP);
    else                 swprintf(headLine, 64, L"♥ 心动 Lv.%d · 好感 %d / %d", AffinityLevel(), AffinityShown(), AFFINITY_CAP);
#endif
    AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, headLine);
    if (nextId >= 0) {
        wchar_t nextLine[96];
        swprintf(nextLine, 96, L"　下一个解锁：%s（还差 %d）", UNLOCK_DEF[nextId].name, remain);
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, nextLine);
    } else {
#if DEV_UNLOCK_ALL
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"　🔧 好感度门槛已停用");
#else
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"　全部功能已解锁 ♥");
#endif
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 未解锁项：灰掉但在标题里写清"要多少好感"，比直接消失更能驱动养成
    auto lockName = [](int id, const wchar_t* base) -> std::wstring {
        if (UnlockReady(id)) return std::wstring(base);
        wchar_t b[96];
        swprintf(b, 96, L"%s（🔒 好感 %d 解锁）", base, UNLOCK_DEF[id].need);
        return std::wstring(b);
    };

    AppendMenuW(menu, MF_STRING, ID_CHAT, L"💬 对话");
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, IsWindowVisible(hwnd) ? L"隐藏桌宠" : L"显示桌宠");
    AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED)
        | (UnlockReady(UNLOCK_AUTOSTART) ? 0 : MF_DISABLED | MF_GRAYED),
        ID_TRAY_AUTOSTART, lockName(UNLOCK_AUTOSTART, L"开机自启").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | MF_POPUP, (UINT_PTR)outfits, L"切换服装");
    AppendMenuW(outfits, MF_STRING | (g_currentCostume == Costume::Ninja  ? MF_CHECKED : 0), 1, L"忍者服");
    AppendMenuW(outfits, MF_STRING | (g_currentCostume == Costume::Daily  ? MF_CHECKED : 0)
        | (UnlockReady(UNLOCK_COSTUME_DAILY) ? 0 : MF_DISABLED | MF_GRAYED),
        2, lockName(UNLOCK_COSTUME_DAILY, L"日常服").c_str());
    AppendMenuW(outfits, MF_STRING | (g_currentCostume == Costume::School ? MF_CHECKED : 0)
        | (UnlockReady(UNLOCK_COSTUME_SCHOOL) ? 0 : MF_DISABLED | MF_GRAYED),
        3, lockName(UNLOCK_COSTUME_SCHOOL, L"校服").c_str());
    AppendMenuW(outfits, MF_STRING | (g_currentCostume == Costume::Dress  ? MF_CHECKED : 0)
        | (UnlockReady(UNLOCK_COSTUME_DRESS) ? 0 : MF_DISABLED | MF_GRAYED),
        4, lockName(UNLOCK_COSTUME_DRESS, L"礼服").c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | MF_POPUP, (UINT_PTR)volumeMenu, L"音量");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 0 ? MF_CHECKED : 0), ID_VOLUME_MUTE, L"静音");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 300 ? MF_CHECKED : 0), ID_VOLUME_LOW, L"低");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 700 ? MF_CHECKED : 0), ID_VOLUME_MID, L"中");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 1000 ? MF_CHECKED : 0), ID_VOLUME_HIGH, L"高");

    bool bgmOk = UnlockReady(UNLOCK_BGM);
    AppendMenuW(menu, MF_STRING | MF_POPUP | (bgmOk ? 0 : MF_DISABLED | MF_GRAYED),
        (UINT_PTR)bgmMenu, lockName(UNLOCK_BGM, L"🎵 背景音乐").c_str());
    for (int i = 0; i < (int)g_bgmDisplayNameList.size(); i++) {
        AppendMenuW(bgmMenu, MF_STRING | (i == g_currentBGMIndex ? MF_CHECKED : 0),
            ID_BGM_SELECT_START + i, g_bgmDisplayNameList[i].c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    bool rvcOk = UnlockReady(UNLOCK_RVC);
    AppendMenuW(menu, MF_STRING | (rvcOk && !g_rvcBusy ? 0 : MF_DISABLED | MF_GRAYED),
        ID_RVC_PICK, lockName(UNLOCK_RVC, L"🎤 选择歌曲翻唱…").c_str());
    wchar_t octTitle[64];
    int octSemi = g_rvcOctave * RVC_OCTAVE_SEMI;
    wsprintfW(octTitle, L"🎤 翻唱变调", octSemi);
    bool octOk = rvcOk && UnlockReady(UNLOCK_OCTAVE);
    AppendMenuW(menu, MF_STRING | MF_POPUP | (octOk ? 0 : MF_DISABLED | MF_GRAYED),
        (UINT_PTR)octMenu, octOk ? octTitle : lockName(UNLOCK_OCTAVE, L"🎤 翻唱变调").c_str());
    wchar_t octDown[64], octUp[64];
    wsprintfW(octDown, L"降调 ", -RVC_OCTAVE_SEMI);
    wsprintfW(octUp,   L"升调 ",  RVC_OCTAVE_SEMI);
    AppendMenuW(octMenu, MF_STRING | (g_rvcOctave == -1 ? MF_CHECKED : 0), ID_RVC_OCT_DOWN, octDown);
    AppendMenuW(octMenu, MF_STRING | (g_rvcOctave ==  0 ? MF_CHECKED : 0), ID_RVC_OCT_ZERO, L"原调 ");
    AppendMenuW(octMenu, MF_STRING | (g_rvcOctave ==  1 ? MF_CHECKED : 0), ID_RVC_OCT_UP,   octUp);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING, ID_ACH_PANEL, L"🏆 成就");
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");

    // ★ 不先 SetForegroundWindow，点完菜单可能残留在屏幕上不消失
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    // 灰掉的菜单项正常点不到，但键盘/快捷键仍可能送进来，这里做最后一道兜底
    auto guardUnlock = [&](int id) -> bool {
        if (UnlockReady(id)) return true;
        wchar_t d[96];
        swprintf(d, 96, L"%s · 需累计好感 %d，还差 %d",
            UNLOCK_DEF[id].desc, UNLOCK_DEF[id].need,
            UNLOCK_DEF[id].need - g_stat.affinityTotal);
        ToastShow(L"🔒", UNLOCK_DEF[id].name, d);
        return false;
    };

    switch (cmd) {
    case ID_TRAY_SHOW: SetPetVisible(hwnd, !IsWindowVisible(hwnd)); break;
    case ID_TRAY_AUTOSTART: if (guardUnlock(UNLOCK_AUTOSTART)) SetAutoStart(!IsAutoStartEnabled()); break;
    case ID_VOLUME_MUTE: g_currentVolume = 0; SetBGMVolume(0); SyncAllSFXVolume(); break;
    case ID_VOLUME_LOW: g_currentVolume = 300; SetBGMVolume(300); SyncAllSFXVolume(); break;
    case ID_VOLUME_MID: g_currentVolume = 700; SetBGMVolume(700); SyncAllSFXVolume(); break;
    case ID_VOLUME_HIGH: g_currentVolume = 1000; SetBGMVolume(1000); SyncAllSFXVolume(); break;
    case ID_CHAT: OpenChatDialog(); break;
    case ID_ACH_PANEL: OpenAchPanel(); break;
    case ID_TRAY_EXIT: DestroyWindow(hwnd); break;
    case ID_RVC_PICK:   // 🎤 弹出文件框选歌翻唱
        if (guardUnlock(UNLOCK_RVC)) RvcPickAndConvert(hwnd);
        break;
    case ID_RVC_OCT_DOWN: if (guardUnlock(UNLOCK_OCTAVE)) { g_rvcOctave = -1; SaveConfig(); } break;
    case ID_RVC_OCT_ZERO: if (guardUnlock(UNLOCK_OCTAVE)) { g_rvcOctave =  0; SaveConfig(); } break;
    case ID_RVC_OCT_UP:   if (guardUnlock(UNLOCK_OCTAVE)) { g_rvcOctave =  1; SaveConfig(); } break;
    default:
        if (cmd >= ID_BGM_SELECT_START && cmd < ID_BGM_SELECT_START + (int)g_bgmDisplayNameList.size()) {
            if (guardUnlock(UNLOCK_BGM)) PlayBGMByIndex(cmd - ID_BGM_SELECT_START);
        }
        else if (cmd >= 1 && cmd <= 4) {
            int uid = CostumeUnlock(cmd - 1);
            if (uid < 0 || guardUnlock(uid)) SwitchAnim(static_cast<Costume>(cmd - 1), AnimState::Idle);
        }
        break;
    }
    s_opening = false;
}

// ================= 窗口过程 =================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SHOWWINDOW:
        if (wp == TRUE && g_preloadDone && !g_bStartupPlayed) {
            g_bStartupPlayed = true;
            PlayStartupAudio();
        }
        g_lastTickMs = GetTickCount64();     // 隐藏/恢复后重新开始计陪伴时长
        break;
    case WM_MOUSEWHEEL: {
        if (!g_preloadDone) return 0;            // 加载期不缩放，避免触发重建
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta > 0) g_zoom = min(g_zoom + 0.1f, 3.0f);
        else g_zoom = max(g_zoom - 0.1f, 0.5f);
        int newW = (int)(BASE_W * g_zoom), newH = (int)(BASE_H * g_zoom);
        RECT rc; GetWinRect(g_hwnd, rc);
        int nx = rc.left, ny = rc.bottom - newH;      // 以底边为锚缩放
        SetWindowPos(g_hwnd, HWND_TOPMOST, nx, ny, newW, newH, SWP_NOACTIVATE);
        g_currentW = newW; g_currentH = newH;
        g_winX = nx; g_winY = ny; g_winPosKnown = true;   // ★ 同步逻辑位置
        g_zoomLastMs = GetTickCount();
        UpdateLayered();
        return 0;
    }
    case WM_TIMER:
        // RVC 状态栏倒计时：完成/失败显示 4 秒后自动消失
        if (g_rvcStatusTimer > 0) {
            g_rvcStatusTimer--;
            if (g_rvcStatusTimer == 0) {
                std::lock_guard<std::recursive_mutex> lk(g_rvcMtx);
                g_rvcStage = RvcStage::Idle;
                g_rvcStatusText.clear();
                g_rvcSourceName.clear();
                // 状态结束，切回空闲动画
                SwitchAnim(g_currentCostume, AnimState::Idle);
            }
        }
        if (wp == 1) {
            if (!g_preloadDone) {          // 加载期只推进加载动画，不碰帧数据
                ++g_loadTick;
                UpdateLayered();
                return 0;
            }
            g_paintPending = false;

            // ★ 陪伴时长：用真实时钟差值，不用"帧数 × 33ms"
            //   系统休眠时定时器会停摆，醒来会算出几小时的差值 ——
            //   所以单帧超过 2 个周期只按一帧计，避免把睡觉时间算成陪伴。
            {
                ULONGLONG nowMs = GetTickCount64();
                if (g_lastTickMs == 0) g_lastTickMs = nowMs;
                double dt = (double)(nowMs - g_lastTickMs) / 1000.0;
                g_lastTickMs = nowMs;
                double cap = (double)FRAME_INTERVAL_MS / 1000.0 * 2.0;
                if (dt < 0.0) dt = 0.0;
                if (dt > cap) dt = cap;
                g_stat.totalSeconds += dt;
                g_hourAcc += dt;
                while (g_hourAcc >= 3600.0) { g_hourAcc -= 3600.0; StatsAddAffinity(1); }

                if (g_lastStatsSaveMs == 0) g_lastStatsSaveMs = nowMs;
                if (nowMs - g_lastStatsSaveMs >= 60000) { g_lastStatsSaveMs = nowMs; StatsSave(); }
                if (nowMs - g_lastAchCheckMs >= 3000)   { g_lastAchCheckMs = nowMs; AchCheck(); UnlockCheck(); AffinityMaxCheck(); }
            }
            ToastTick();                 // 成就解锁气泡推进（各自独立的分层窗口）

            if (g_bTransitioning) {
                g_transitionTimer++;
                UpdateLayered();
                if (g_transitionTimer > TRANSITION_DURATION) {
                    g_bTransitioning = false;
                    // ★ 帧由缓存独占：这里只交接引用，绝不能 ClearFrames（否则二次释放）
                    g_currentFrames = std::move(g_pendingFrames);
                    g_currentCostume = g_pendingCostume;
                    g_currentState = g_pendingState;
                    g_currentFrameIndex = 0;
                    g_idleActionTimer = 0; g_isRandomAction = false;
                    g_renderPrewarmReq = true;
                }
                return 0;
            }
            {
                std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
                int cc = (int)g_currentCostume, ss = (int)g_currentState;
                if (cc >= 0 && cc <= 3 && ss >= 0 && ss <= 3 && g_frameCached[cc][ss])
                    g_currentFrames = g_frameCache[cc][ss];
                if (!g_pendingDelete.empty()) ClearFrames(g_pendingDelete);
            }
            if (!g_currentFrames.empty()) {
                g_currentFrameIndex = (g_currentFrameIndex + 1) % (int)g_currentFrames.size();
                UpdateLayered();
            }
            if (!g_bDragging) {
                if (g_currentState == AnimState::Idle && !g_isRandomAction) {
                    g_idleActionTimer++;
                    if (g_idleActionTimer > 360) {
                        SwitchAnim(g_currentCostume, rand() % 2 == 0 ? AnimState::Think : AnimState::Peace);
                        g_isRandomAction = true; g_idleActionTimer = 0;
                    }
                } else if (g_isRandomAction) {
                    g_idleActionTimer++;
                    if (g_idleActionTimer > 120) SwitchAnim(g_currentCostume, AnimState::Idle);
                }
            } else g_idleActionTimer = 0;
        }
        return 0;
    case WM_APP_AFFINITY: {
        int delta = (int)wp - AFFINITY_MSG_BIAS;
        bool applied = StatsAddAffinityChat(delta);
        if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) {
            wchar_t line[80];
            if (delta > 0 && applied)
                swprintf(line, 80, L"\r\n\r\n（茉子对你的好感 +%d）", delta);
            else if (delta > 0 && AffinityMaxed())
                // 满心之后不再累计，别把它报成"今日已达上限"——那会让人以为明天还能涨
                swprintf(line, 80, L"\r\n\r\n（茉子的心意已经满了）");
            else if (delta > 0)
                swprintf(line, 80, L"\r\n\r\n（茉子对你的好感 +%d，但今日已达上限 %d，明天再来陪她吧）", delta, AFFINITY_DAY_CAP);
            else if (delta < 0)
                swprintf(line, 80, L"\r\n\r\n（茉子对你的好感 %d）", delta);
            else
                swprintf(line, 80, L"\r\n\r\n（茉子的心情没有变化）");
            DWORD end = GetWindowTextLengthW(g_hHistoryEdit);
            SendMessageW(g_hHistoryEdit, EM_SETSEL, (WPARAM)end, (LPARAM)end);
            SendMessageW(g_hHistoryEdit, EM_REPLACESEL, TRUE, (LPARAM)line);
            SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
        }
        UnlockCheck();
        AffinityMaxCheck();
        AchCheck();
        StatsSave();
        RequestPaint(true);
        if (g_hAchPanel && IsWindow(g_hAchPanel)) InvalidateRect(g_hAchPanel, nullptr, FALSE);
        return 0;
    }
    case WM_APP_PRELOAD_DONE:
        SwitchAnim(g_currentCostume, AnimState::Idle);
        // ★ 一套素材都没解出来：多半是没放 Animations。
        //   不提示的话用户只会看到一个空白窗口，完全不知道出了什么事。
        {
            bool anyFrame = false;
            for (int c = 0; c < 4 && !anyFrame; ++c)
                for (int s = 0; s < 4 && !anyFrame; ++s)
                    if (g_frameCached[c][s] && !g_frameCache[c][s].empty()) anyFrame = true;
            if (!anyFrame)
                ToastShow(L"📂", L"没有找到序列帧素材",
                          L"请把图片放到 exe 同级的 Animations\\服装\\动作\\ 下，目录结构见 README。");
        }
        if (!IsWindowVisible(hwnd)) { ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); }
        if (!g_timerOn) { g_timerOn = true; SetTimer(hwnd, 1, FRAME_INTERVAL_MS, nullptr); }
        if (!g_bStartupPlayed) { g_bStartupPlayed = true; PlayStartupAudio(); }
        g_lastTickMs = GetTickCount64();
        TraySetTip(L"常陆茉子桌宠");
        if (UnlockReady(UNLOCK_BGM)) PlayBGMByIndex(g_currentBGMIndex);   // 未解锁则安静启动
        UnlockCheck();               // 补弹：老存档已有高好感但从未提示过的情况
        AffinityMaxCheck();
        RequestPaint(true);
        return 0;
    case WM_TRAY:
        if (wp == 1 && lp == WM_RBUTTONUP) ShowContextMenu(hwnd);
        break;
    case WM_LBUTTONDOWN:
        if (!g_preloadDone) return 0;            // 加载期不接受交互
        g_bDragging = false;
        GetCursorPos(&g_ptLButtonDown);
        { RECT rc; GetWindowRect(hwnd, &rc); g_dragWindowStart.x = rc.left; g_dragWindowStart.y = rc.top; }
        SetCapture(hwnd); return 0;
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd && !g_bDragging) {
            POINT pt; GetCursorPos(&pt);
            if (abs(pt.x - g_ptLButtonDown.x) > GetSystemMetrics(SM_CXDRAG) || abs(pt.y - g_ptLButtonDown.y) > GetSystemMetrics(SM_CYDRAG)) {
                g_bDragging = true; SwitchAnim(g_currentCostume, AnimState::Run);
            }
        } else if (g_bDragging) {
            POINT pt; GetCursorPos(&pt);
            int nx = g_dragWindowStart.x + (pt.x - g_ptLButtonDown.x);
            int ny = g_dragWindowStart.y + (pt.y - g_ptLButtonDown.y);
            ClampToMonitor(pt, g_currentW, g_currentH, nx, ny);   // ★ 拖不出屏幕
            // ★ 不再调用 SetWindowPos：原写法每次移动都要"移动窗口 + 贴屏"两次合成，
            //   高刷鼠标下主线程被合成占满就是拖动卡顿的根因。这里只更新逻辑位置，
            //   由 UpdateLayeredWindow 一次性把位置和内容都带过去。
            g_winX = nx; g_winY = ny; g_winPosKnown = true;
            RequestPaint();
        }
        return 0;
    case WM_LBUTTONUP:
        if (!g_preloadDone) return 0;            // 加载期不接受交互
        if (GetCapture() == hwnd) ReleaseCapture();
        if (g_bDragging) { g_bDragging = false; SwitchAnim(g_currentCostume, AnimState::Idle); }
        else {
            switch (g_currentState) {
            case AnimState::Idle: SwitchAnim(g_currentCostume, AnimState::Think); break;
            case AnimState::Think: SwitchAnim(g_currentCostume, AnimState::Peace); break;
            default: SwitchAnim(g_currentCostume, AnimState::Idle); break;
            }
        }
        return 0;
    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS* pwp = (WINDOWPOS*)lp;
        if (pwp && !(pwp->flags & SWP_NOMOVE)) { g_winX = pwp->x; g_winY = pwp->y; g_winPosKnown = true; }
        break;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd && g_bDragging) { g_bDragging = false; SwitchAnim(g_currentCostume, AnimState::Idle); }
        return 0;
    // ★ 右键菜单必须在 WM_RBUTTONUP 里自己弹。
    //   分层窗口上如果这里 return 0，DefWindowProc 就不会被调用，
    //   WM_CONTEXTMENU 也就永远不会产生 —— 只依赖它会表现为"右键完全没反应"。
    case WM_RBUTTONUP:
        ShowContextMenu(hwnd);
        return 0;
    // 仍保留：覆盖系统直接发出的情况（键盘菜单键 / Shift+F10）。
    // 不会重复：上面的 WM_RBUTTONUP 已 return，不会再派生 WM_CONTEXTMENU，
    // 且 ShowContextMenu 内部有防重入。
    case WM_CONTEXTMENU:
        ShowContextMenu(hwnd);
        return 0;
    case WM_DESTROY: {
        SaveConfig(); KillTimer(hwnd, 1); StopBGM();
        if (g_exit.loaded) { PlayExitAudio(); WaitForSoundToFinish(g_exit, 2000); }
        TrayRemove();
        if (g_gfx) { delete g_gfx; g_gfx = nullptr; }
        if (g_memDC) { SelectObject(g_memDC, g_hOldBmp); DeleteObject(g_hMemBmp); DeleteDC(g_memDC); }
        g_memDC = nullptr; g_hMemBmp = nullptr; g_hOldBmp = nullptr;
        // ★ 帧由缓存独占：这里只断开引用，绝不能 ClearFrames（否则二次释放）
        g_currentFrames.clear(); g_pendingFrames.clear();
        RenderCacheInvalidate();   // ★ 必须在 GdiplusShutdown 之前释放 GDI+ 对象
        AnimLoadStop();            // 先停解码线程，再动缓存
        ClearFrameCache();
        StatsSave();                 // 累计数据落盘
        ToastDestroy();
        if (g_pTransitionImg) { delete g_pTransitionImg; g_pTransitionImg = nullptr; }
        if (g_pLoadingImg) { delete g_pLoadingImg; g_pLoadingImg = nullptr; }
        // 等待 RVC 后台线程结束，避免它还在回调 XAudio2 / 改全局状态时被强杀
        if (g_hRvcThread) {
            WaitForSingleObject(g_hRvcThread, 5000);
            CloseHandle(g_hRvcThread);
            g_hRvcThread = nullptr;
        }
        AudioX_ShutdownAll();
        if (g_gdiToken) GdiplusShutdown(g_gdiToken);
        PostQuitMessage(0); return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ================= 入口 =================
int WINAPI WinMain(__in HINSTANCE hInst, __in_opt HINSTANCE, __in LPSTR, __in int) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { MessageBoxW(nullptr, L"COM 初始化失败！", L"错误", MB_OK); return 0; }
    std::wstring exePath;
    if (!GetExeDir(exePath)) { MessageBoxW(nullptr, L"无法定位程序目录！", L"错误", MB_OK); return 0; }
    SetCurrentDirectoryW(exePath.c_str());
    // 把相对路径 "reference\mako_ref.wav" 拼成绝对路径
    g_ttsRefAudioPath = exePath + L"\\reference\\mako_ref.wav";
    srand(GetTickCount());
    ScanBGMDirectory(); LoadConfig();
    StatsLoad(); StatsCheckDay(); AchCheck();      // ★ 成就系统初始化
    GdiplusStartupInput gi{};
    gi.GdiplusVersion = 1;
    GdiplusStartup(&g_gdiToken, &gi, nullptr);
    AudioX_Init(); AudioX_SetVolume(g_currentVolume); AudioX_Preload();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)(UINT_PTR)IDC_ARROW);
    wc.lpszClassName = L"MakotoPet";
    if (PathFileExistsW(L"transition.png")) g_pTransitionImg = Image::FromFile(L"transition.png");
    if (PathFileExistsW(L"icon.ico")) g_pLoadingImg = Image::FromFile(L"icon.ico");
    if (g_pLoadingImg && g_pLoadingImg->GetLastStatus() != Ok) { delete g_pLoadingImg; g_pLoadingImg = nullptr; }
    RegisterClassExW(&wc);
    {   // 成就解锁气泡窗口类
        WNDCLASSEXW twc = { sizeof(WNDCLASSEXW) };
        twc.lpfnWndProc = ToastProc;
        twc.hInstance = hInst;
        twc.lpszClassName = L"MakotoToast";
        RegisterClassExW(&twc);
    }
    int initX = (GetSystemMetrics(SM_CXSCREEN) - g_currentW) / 2;
    int initY = GetSystemMetrics(SM_CYSCREEN) - g_currentH - 50;
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"MakotoPet", L"常陆茉子", WS_POPUP, initX, initY, g_currentW, g_currentH, nullptr, nullptr, hInst, nullptr);
    if (g_hasSavedPos) SetWindowPos(g_hwnd, HWND_TOPMOST, g_savedPosX, g_savedPosY, g_currentW, g_currentH, SWP_NOACTIVATE);
    // ★ 人物位置先显示托盘图标 + 进度条，等 16 套素材全部就绪后再换成人物
    { RECT rc0; if (GetWinRect(g_hwnd, rc0)) { g_winX = rc0.left; g_winY = rc0.top; g_winPosKnown = true; } }
    g_lastTickMs = GetTickCount64();
    TrayAdd();
    g_timerOn = true;
    SetTimer(g_hwnd, 1, FRAME_INTERVAL_MS, nullptr);
    ShowWindow(g_hwnd, SW_SHOWNA);          // 不抢焦点，仅显示加载画面
    UpdateLayered();
    AnimLoadStart();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return 0;
}
