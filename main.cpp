// ============================================================================
//  MakotoPet —— 一个跑在 Windows 桌面上的桌宠：常陆茉子
//  Copyright (c) 2025 MakotoPet contributors
//  Licensed under the MIT License.  See LICENSE / NOTICE in the repository root.
//
//  这是一个个人练手项目，开源的目的是记录"这个东西是怎么做出来的"。
//  整个客户端只有这一个 .cpp，除可选的 Live2D 运行时外只用 Windows 系统组件：
//
//    · 显示   分层透明窗口；默认由 p2l_runtime.dll 实时渲染 live2d\ 下
//             的 .p2lrt 模型（4 套服装运行时热切换），
//             把 USE_LIVE2D 改成 0 则回退到序列帧动画 + 磁盘纹理缓存
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
#include <richedit.h>   // ：RichEdit 的 CHARFORMAT2W/CHARRANGE/EM_*/CFM_* 真实定义
#include <mmsystem.h>
#include <gdiplus.h>
#include <shlwapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <queue>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>
#include <xaudio2.h>
#include <cstring>
#include <winhttp.h>
#include <objidl.h>     // ★附件：IStream / CreateStreamOnHGlobal（GDI+ 内存编码 JPEG 用）
#include <process.h>
#include <fstream>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <utility>
// ★扫描件 OCR：走系统自带的 WinRT OCR（Windows.Media.Ocr），零额外下载。
//   这些 WinRT 头会和 windows.h 的 min/max 宏打架，而本文件下面大量用 min()/max() 宏，
//   所以只在包含这几行的期间临时取消宏，包完立刻恢复。
#pragma push_macro("min")
#pragma push_macro("max")
#undef min
#undef max
#include <roapi.h>
#include <winstring.h>
#include <activation.h>
#include <wrl/client.h>
#include <windows.foundation.h>
#include <windows.storage.h>
#include <windows.storage.streams.h>
#include <windows.graphics.imaging.h>
#include <windows.media.ocr.h>
#pragma pop_macro("max")
#pragma pop_macro("min")
#pragma comment(lib, "windowsapp.lib")   // RoGetActivationFactory / WindowsCreateString
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "winhttp.lib")

using namespace std;
using namespace Gdiplus;

// 全局前置声明：
//   L2dClothTug 等在文件前部就要用配置读取函数，
//   而这些函数的定义在文件后半部(GetLLMConfig* 约 4277 行、宽字符转换约 3634 行)。
//   C++ 要求"先声明后使用"，这里统一前置声明。
//   MultiByteToWideChar 来自 <windows.h>(第 28 行已包含)，无需额外声明。
static std::wstring GetLLMConfig(const wchar_t* key);
static double GetLLMConfigDouble(const wchar_t* key, double defVal, double lo, double hi);
static int GetLLMConfigInt(const wchar_t* key, int defVal, int lo, int hi);
static int GetIniInt(const wchar_t* sec, const wchar_t* key, int defVal, int lo, int hi);
static void SetIniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* val);
static std::string WideStringToUtf8(const std::wstring& wstr);
static std::wstring Utf8ToWideString(const std::string& str);
static void MarkChatGeomDirty();
static WNDPROC g_oldInputProc;
static LRESULT CALLBACK InputSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);


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
// 半身模式：角色仍是完整立绘，但窗口沉到屏幕底部、下半截探出屏幕外，
// 像普通 VTuber 那样只露出上半身。不用菜单按钮：往下拖让下半身探出屏幕即可，
// 往上拖回来恢复全身；状态直接由窗口位置推导，并随窗口坐标一起存档。
static bool g_halfBody = false;
#define HALF_BODY_VISIBLE     0.60f   // 半身时露在屏幕内的身高比例（其余沉到屏幕下）
#define HALF_BODY_FULL_LIMIT  0.20f   // 探出超过这个比例就算半身（也是拖动时允许的最大"普通"探出量）
#define FPS 30
#define FRAME_INTERVAL_MS (1000 / FPS)

#define WM_TRAY (WM_USER + 1)
#define WM_APP_PRELOAD_DONE (WM_APP + 1)   // 素材全量加载完成，可以显示人物了
#define WM_APP_AFFINITY     (WM_APP + 2)   // 对话好感度增减（wParam = delta + 16，便于传负数）
#define WM_APP_RVC_DONE     (WM_APP + 3)   // 翻唱成功，回主线程结算统计
#define WM_APP_EMOTION      (WM_APP + 4)   // 情绪联动：wParam = PetEmotion 枚举值，主线程驱动动作/视线/音效/心情提示
#define WM_APP_PROACTIVE    (WM_APP + 5)   // 主动搭话：lParam = new std::wstring（要说的话），主线程弹气泡
#define WM_APP_DIRECTOR     (WM_APP + 6)   // 导演指令：lParam = new L2dDirectorCmd（情绪/强度/动作/视线），主线程用完 delete
#define WM_APP_TOUCH_REPLY  (WM_APP + 7)   // ★触摸互动：LLM 对"主人摸了某部位"的回应，lParam = new std::wstring，主线程弹气泡
#define WM_APP_ATTACH_READY (WM_APP + 8)   // ★附件：后台解析完成，主线程回填占位附件（无 lParam，结果在全局队列）
// 导演视线模式（放在 USE_LIVE2D 块外，供 ParseDirectorGaze 在非 Live2D 编译单元也能用）
enum L2dGazeMode { L2D_GAZE_FOLLOW = 0, L2D_GAZE_AT_USER = 1, L2D_GAZE_AWAY = 2, L2D_GAZE_DOWN = 3 };
#define AFFINITY_MSG_BIAS   16
#define ID_TRAY_SHOW 100
#define ID_TRAY_AUTOSTART 101
#define ID_TRAY_EXIT 102

#define ID_VOLUME_MUTE  110
#define ID_VOLUME_LOW   111
#define ID_VOLUME_MID   112
#define ID_VOLUME_HIGH  113

#define ID_RENDER_FRAMES 120        // 🖼 渲染方式：序列帧
#define ID_RENDER_LIVE2D 121        // 🖼 渲染方式：Live2D 模型
#define ID_TTS_TOGGLE    130        // 🔊 语音朗读开关（GPT-SoVITS）

#define ID_BGM_SELECT_START 2000
#define ID_CHAT 3000
#define ID_SETTINGS 3001          // ⚙ 设置面板入口（右键菜单）
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

// ★ 渲染方式（右键菜单可切换）：序列帧 / Live2D 模型。
//   两套素材彼此独立，启动时各扫一次，缺哪套就把菜单里对应项灰掉，不给切。
//   g_wantMode 是存档里选的那一项，g_renderMode 是这次实际跑起来的（可能因缺素材回落）。
enum RenderMode { RM_FRAMES = 0, RM_LIVE2D = 1 };
static RenderMode g_renderMode      = RM_FRAMES;
static RenderMode g_wantMode        = RM_FRAMES;
static bool       g_hasFrameAssets  = false;   // Animations\ 下至少有一套序列帧
static bool       g_hasLive2DAssets = false;   // live2d\ 下有模型，且 p2l_runtime.dll 在
static bool       g_modeFallbackNotice = false; // 启动时因缺素材回落，加载完提示一次

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
ULONGLONG g_ptLButtonDownTick = 0;      // ★按下时刻，用来区分"点击"和"按住拖"

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

// ==================================================================
//  ★ Live2D 渲染桥接（psd2live / p2l_runtime.dll）
//
//  .p2lrt 是运行时模型：每帧由运行时的软件光栅化器算出一张 RGBA8 位图，
//  再交给已有的分层窗口贴到桌面。好感度、成就、对话、托盘等业务逻辑
//  完全不感知渲染方式的差异。
//
//  这里用 LoadLibraryW 动态加载，不链接 .lib、不改 VS 项目属性：
//  p2l_runtime.dll 放 exe 同级，4 套 .p2lrt 放其 live2d\ 子目录。
//  缺文件时回退到序列帧渲染；换装时加载失败则保留当前这套，不白屏。
//  把 USE_LIVE2D 改成 0 重新编译，即可完整回到旧的序列帧实现。
// ==================================================================
#ifndef USE_LIVE2D
#define USE_LIVE2D 1
#endif

#include <cstdint>

#if USE_LIVE2D
#define P2L_ABI_VERSION_MAJOR 1
#define P2L_ABI_VERSION_MINOR 4
#define P2L_ABI_COMPATIBLE(v) (((v) >> 16) == P2L_ABI_VERSION_MAJOR && ((v) & 0xffffu) >= P2L_ABI_VERSION_MINOR)
#define P2L_LOAD_VERIFY_CRC   1u
#define P2L_RENDER_STRAIGHT   1u
// 运行时内置的程序化行为位。眨眼与呼吸默认开，视线跟随默认关。
#define P2L_BEHAVIOR_BLINK    1u
#define P2L_BEHAVIOR_BREATH   2u
#define P2L_BEHAVIOR_LOOK     4u

struct P2lRig;   // 不透明句柄，C ABI 里只传指针

struct P2lApi {
    HMODULE dll = nullptr;
    uint32_t    (*abi_version)(void) = nullptr;
    void*       (*model_load)(const uint8_t*, size_t, uint32_t, char*, size_t) = nullptr;
    void*       (*rig_create)(const void*, char*, size_t) = nullptr;
    void        (*model_free)(void*) = nullptr;
    void        (*rig_free)(P2lRig*) = nullptr;
    const char* (*rig_failure)(const P2lRig*) = nullptr;
    void        (*canvas)(const P2lRig*, float*, float*) = nullptr;
    uint32_t    (*clip_count)(const P2lRig*) = nullptr;
    void        (*play)(P2lRig*, int32_t) = nullptr;
    void        (*physics_stabilize)(P2lRig*) = nullptr;
    // ★触摸物理反馈：给指定物理组注入冲量（返回是否命中）。旧 DLL 没有此导出时为 nullptr，自动降级。
    bool        (*physics_impulse)(P2lRig*, const char*, float, float) = nullptr;
    // ★局部布料抓取：直接位移裙摆/裤子网格顶点并弹性回弹（返回是否命中网格）。旧 DLL 无此导出则降级。
    bool        (*cloth_tug)(P2lRig*, const char*, float, float, float, float, float, float, float, float, float) = nullptr;
    void        (*cloth_clear)(P2lRig*) = nullptr;
    void        (*update)(P2lRig*, float) = nullptr;
    bool        (*render)(P2lRig*, uint8_t*, uint32_t, uint32_t, const float*, uint32_t) = nullptr;
    void        (*behaviors)(P2lRig*, uint32_t) = nullptr;   // 可选：旧运行时没有也不影响渲染
    void        (*look_at)(P2lRig*, float, float) = nullptr;
    // ★ 表情 API（可选）：旧 DLL 没有这些导出时为 nullptr，表情功能自动降级，
    //   其余渲染/动作完全不受影响。ABI 1.4 起提供。
    uint32_t    (*expression_count)(const P2lRig*) = nullptr;
    const char* (*expression_id)(const P2lRig*, uint32_t) = nullptr;
    void        (*expression)(P2lRig*, int32_t) = nullptr;
    // ★ 多模型表情贴图 API（可选）：加载模型后告诉 DLL 用哪套烘焙表情，
    //   切换服装时表情贴图随之切换。旧 DLL 没有这些导出时为 nullptr，自动降级。
    void        (*face_atlas_set_model)(P2lRig*, const char*) = nullptr;
    // ★ school8 表情系统（可选）：启用后 p2l_expression 的 0..7 对应 8 张原作差分表情，
    //   分层眼球仍跟随光标。旧 DLL 无此导出时降级到旧 7 槽表情。
    void        (*school8_enable)(bool) = nullptr;
};

static P2lApi               g_p2l;
static P2lRig*              g_l2dRig = nullptr;

// ★★ 32-rig 架构 ★★
// 新模型结构：live2d/{ninja|daily|school|dress}/{base,smile,...}.p2lrt，
// 每个表情是独立烘焙 rig。换装时把该套 8 个表情一次性预加载缓存，
// 情绪触发时只切换 g_l2dRig 指针 —— 表情 100% 还原立绘，且切换零延迟。
// 旧的单文件结构（live2d/{costume}.p2lrt + DLL 内嵌 atlas 表情）仍兼容：
// 这时只有 base 一个 rig，表情继续走 g_p2l.expression() 参数化切换。
struct L2dExprRig {
    P2lRig* rig = nullptr;
    bool    ok  = false;
};
#define L2D_EXPR_COUNT 8
static L2dExprRig  g_l2dExprRigs[L2D_EXPR_COUNT];   // 当前服装的 8 个表情 rig
static int         g_l2dCurExpr = 0;                // 当前表情槽位（0=base）
// ★ 最近一次播放的动作片段槽位。-1 = 还没播过。
//   8 个表情是 8 个独立 rig，动作片段状态不互相共享；切 rig 后必须在
//   新 rig 上重放一次，否则"先播动作、再切表情"会把本轮动作丢掉
//   （表现就是表情变了、身体却停在待机）。
static int         g_l2dLastClipSlot = -1;
static bool        g_l2dMultiRig = false;           // true=新结构(8独立rig) false=旧结构(单rig+atlas)
// 与 live2d/{服装}/ 下的文件名严格一致
static const char* const kL2dExprFile[L2D_EXPR_COUNT] = {
    "base", "smile", "smile_sulky", "panic",
    "shy_blush", "embarrassed", "thinking2", "wonder"
};
// ★ 运行时 DLL 是否已成功加载且 ABI 兼容。与「当前服装有没有模型」分开记：
//   DLL 就绪但某套服装没模型时，保持 DLL 常驻，换到有模型的服装仍能用模型；
//   这就把旧的「全局开关式降级」改成了「按服装粒度自动降级」。
static bool                 g_l2dLibReady = false;
static float                g_l2dCW = 0, g_l2dCH = 0;
static std::vector<uint8_t> g_l2dRgba;      // p2l_render 的输出，复用避免每帧分配
static bool                 g_l2dFrameDirty = true;
static ULONGLONG            g_l2dLastMs = 0;
// ★ 表情显示时长：到点自动淡出回中性脸，避免一个表情永久挂脸上。
static ULONGLONG            g_l2dExprUntilMs = 0;   // 0 = 当前无表情
#define L2D_EXPR_HOLD_MS 4200


// ★ 当前模型是否启用 school8（8 张原作差分表情）。校服物理模型 = true。
//   为 true 时情绪直接映射到 0..7 槽位号；为 false 时走旧的 EXPR 字符串查表（兼容旧模型）。
static bool                 g_school8Active = false;

// ---------- 统一人物大小 ----------
// 4 套 .p2lrt 的画布尺寸和留白都不同。按 canvas 等比缩放会让换装时人物忽大忽小，
// 所以改为按「实际不透明内容的包围盒」归一：所有服装的人物高度都映射到窗口
// 高度的同一比例，宽度按各自原画比例，不拉伸。
struct L2dContent {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // canvas 坐标系
    bool  ok = false;
};
static L2dContent g_l2dContent;

// 人物内容占窗口的比例。留 6% 给头发物理 / 裙摆摆动，免得动作一出就被裁掉。
#define L2D_FIT_W 0.94f
#define L2D_FIT_H 0.94f

static void Live2DApplyWindowSize();   // 定义在窗口工具区（要用 g_winX / g_winY）
// ★ 保证序列帧后台预加载线程已启动。定义在加载区，换装降级为序列帧时调用，
//   否则若启动时因为「当前服装有模型」跳过了 AnimLoadStart，之后降级到无模型
//   服装时帧缓存会是空的、画面全黑。此函数幂等，重复调用无副作用。
static void AnimLoadStart();

// canvas -> image 变换。校准失败时退回整块画布等比居中，不影响可用性。
static void Live2DFrameTransform(int w, int h, float cw, float ch,
                                 const L2dContent& c,
                                 float& scale, float& tx, float& ty) {
    if (c.ok && c.w > 0.0f && c.h > 0.0f && cw > 0.0f && ch > 0.0f) {
        float sx = (float)w * L2D_FIT_W / c.w;
        float sy = (float)h * L2D_FIT_H / c.h;
        scale = (sx < sy) ? sx : sy;
        tx = (float)w * 0.5f - (c.x + c.w * 0.5f) * scale;
        ty = (float)h * 0.5f - (c.y + c.h * 0.5f) * scale;
    } else {
        float sx = (float)w / cw, sy = (float)h / ch;
        scale = (sx < sy) ? sx : sy;
        tx = ((float)w - cw * scale) * 0.5f;
        ty = ((float)h - ch * scale) * 0.5f;
    }
}

// 渲染一张小图，量出人物实际不透明内容的包围盒。只在载入 / 换装时跑一次。
static bool Live2DCalibrate(P2lRig* rig, float cw, float ch, L2dContent& out) {
    out = L2dContent{};
    if (!rig || !g_p2l.render || !(cw > 0.0f) || !(ch > 0.0f)) return false;
    float s = 512.0f / ((cw > ch) ? cw : ch);
    int pw = (int)(cw * s + 0.5f), ph = (int)(ch * s + 0.5f);
    if (pw < 8 || ph < 8 || (size_t)pw * (size_t)ph > 4u * 1024u * 1024u) return false;
    std::vector<uint8_t> buf((size_t)pw * (size_t)ph * 4u, 0);
    float xf[6] = { s, 0.0f, 0.0f, s, 0.0f, 0.0f };   // canvas -> 校准缓冲
    if (!g_p2l.render(rig, buf.data(), (uint32_t)pw, (uint32_t)ph, xf, P2L_RENDER_STRAIGHT))
        return false;

    // 行列累计阈值：滤掉抗锯齿留下的零星半透明噪点，避免包围盒被撑大。
    std::vector<int> rowHit(ph, 0), colHit(pw, 0);
    for (int y = 0; y < ph; ++y) {
        const uint8_t* p = buf.data() + (size_t)y * (size_t)pw * 4u;
        for (int x = 0; x < pw; ++x, p += 4) {
            if (p[3] >= 24) { ++rowHit[y]; ++colHit[x]; }
        }
    }
    int rThr = (int)(pw * 0.004f); if (rThr < 2) rThr = 2;
    int cThr = (int)(ph * 0.004f); if (cThr < 2) cThr = 2;
    int minY = -1, maxY = -1, minX = -1, maxX = -1;
    for (int y = 0; y < ph; ++y) if (rowHit[y] >= rThr) { if (minY < 0) minY = y; maxY = y; }
    for (int x = 0; x < pw; ++x) if (colHit[x] >= cThr) { if (minX < 0) minX = x; maxX = x; }
    if (minY < 0 || minX < 0) return false;
    out.x = (float)minX / s; out.y = (float)minY / s;
    out.w = (float)(maxX - minX + 1) / s; out.h = (float)(maxY - minY + 1) / s;
    if (!(out.w >= cw * 0.02f) || !(out.h >= ch * 0.02f)) { out = L2dContent{}; return false; }
    out.ok = true;
    return true;
}
#endif

static uint8_t* g_dibBits = nullptr;        // 32bpp 离屏位图的像素（仅 Live2D 路径使用）

// 鼠标命中：alpha 低于此值的像素视为「空白」，让鼠标穿透到桌面。
// 只在完全透明处生效，人物身上照常拖动 / 右键。
// 少数系统对分层窗口的 HTTRANSPARENT 处理不同，若发现空白区点不到桌面，
// 把 L2D_PIXEL_HITTEST 改成 0 即可回到「整个窗口都命中」的原行为。
#define L2D_HIT_ALPHA_MIN 8
#define L2D_PIXEL_HITTEST 1

// 窗口位置工具定义在窗口工具区，桥接段要提前用，这里先声明。
static void ClampToMonitor(POINT pt, int w, int h, int& outX, int& outY);
static int  MonitorWorkBottom(POINT pt);
static void RefreshHalfBodyFromPlacement(int top, int h, POINT anchor);
static void RefreshHalfBodyFromWindow();
static void SnapHalfBodyOnDragEnd(POINT cursor, int& outY);

static bool Live2DReady() {
#if USE_LIVE2D
    return g_l2dRig != nullptr;
#else
    return false;
#endif
}

#if USE_LIVE2D
static void Live2DWarn(const wchar_t* what, const char* detail = nullptr) {
    std::wstring s = L"Live2D 模型未能加载：";
    s += what;
    if (detail && *detail) {
        wchar_t buf[512];
        if (MultiByteToWideChar(CP_UTF8, 0, detail, -1, buf, 512) > 0) { s += L"\n\n"; s += buf; }
    }
    s += L"\n\n已回退到序列帧渲染。若要用 Live2D，请把 p2l_runtime.dll 放在 exe 同级，\n并把 ninja / daily / school / dress 的 *_physics.p2lrt（或 .p2lrt）放进同级的 live2d 文件夹。";
    MessageBoxW(nullptr, s.c_str(), L"MakotoPet", MB_OK | MB_ICONINFORMATION);
}

// ★ 只释放当前 rig，保留运行时 DLL 与函数指针。
//   换到「没有模型」的服装时用它把渲染切回序列帧，这样后面再换回有模型的
//   服装还能立即重新加载，无需重开程序。与 Live2DShutdown 的区别就在是否
//   卸载 DLL：前者是「按服装降级」，后者才是「整个运行时退出」。


// 统一的 rig 释放入口
// 所有权规则必须唯一且清晰，否则切换服装会崩在堆损坏上：
//   · 新结构(g_l2dMultiRig)：8 个 rig 的拥有者是 g_l2dExprRigs 中 ok=true 的槽位；
//     ok=false 的槽位只是指向 base 的共享别名，不单独释放；
//     g_l2dRig 也只是一个借用指针（始终指向 8 个之一），绝不单独释放。
//   · 旧结构：唯一的 rig 由 g_l2dRig 拥有，直接释放。
// 所有"换装/降级/求值失败/退出"路径都必须走这里，禁止再手工 rig_free。
static void ReleaseAllRigs() {
    if (g_l2dMultiRig) {
        for (int i = 0; i < L2D_EXPR_COUNT; ++i) {
            if (g_l2dExprRigs[i].ok && g_l2dExprRigs[i].rig && g_p2l.rig_free)
                g_p2l.rig_free(g_l2dExprRigs[i].rig);
            g_l2dExprRigs[i].rig = nullptr;
            g_l2dExprRigs[i].ok  = false;
        }
    } else {
        if (g_l2dRig && g_p2l.rig_free) g_p2l.rig_free(g_l2dRig);
    }
    g_l2dRig = nullptr;
    g_l2dCurExpr = 0;
    g_l2dMultiRig = false;
}

static void Live2DReleaseRig() {
    ReleaseAllRigs();   // 统一释放：新结构按 ok 去重、g_l2dRig 是别名不单独 free
    g_l2dFrameDirty = true;
    g_l2dLastMs = 0;
    g_l2dRgba.clear();
    g_l2dContent = L2dContent{};
    g_l2dExprUntilMs = 0;
}

static void Live2DShutdown() {
    Live2DReleaseRig();
    if (g_p2l.dll) { FreeLibrary(g_p2l.dll); g_p2l.dll = nullptr; }
    g_p2l = P2lApi{};
    g_l2dLibReady = false;
}

// 4 套服装与 live2d\ 下的模型一一对应，顺序必须与 Costume 枚举一致。
static const char* const kL2dModelName[] = { "ninja", "daily", "school", "dress" };
static const wchar_t* const kL2dModelLabel[] = {
    L"live2d\\ninja.p2lrt", L"live2d\\daily.p2lrt",
    L"live2d\\school.p2lrt", L"live2d\\dress.p2lrt"
};
static std::wstring g_l2dLastError;    // 最近一次加载失败原因，换装失败时给气泡用

// 载入一套服装的 .p2lrt。只有新模型成功出第一帧，才替换当前 rig。
//   warnOnFail   初始化失败弹说明框；运行时换装不弹，交给托盘气泡
//   allowLegacy  仅启动时允许回退到旧的单模型文件名；换装时不回退，
//                否则点了「校服」却冒出别的模型，比不换更让人困惑
// 读取一个 .p2lrt 文件到字节数组（返回大小，失败返回 0）。
static size_t ReadP2lrt(const char* path, std::vector<uint8_t>& bytes) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return 0;
    std::streamoff n = f.tellg();
    if (n <= 0 || n > 64 * 1024 * 1024) return 0;   // 硬上限，挡住畸形文件
    f.seekg(0, std::ios::beg);
    bytes.resize((size_t)n);
    if (!f.read((char*)bytes.data(), n)) { bytes.clear(); return 0; }
    return (size_t)n;
}

// 把一个 rig 初始化到可用状态（atlas / 动作 / 物理 / 行为）。每个表情 rig 都要跑一遍。
static void InitOneRig(P2lRig* rig, int idx) {
    if (!rig) return;
    // ★ 新结构每个表情是独立烘焙 rig，脸已画死在贴图里，不再需要 DLL 内嵌 atlas 换脸。
    //   但仍告诉 DLL 当前服装，防止旧 DLL 的 school8/atlas 逻辑误触发（并把 atlas 关掉）。
    if (g_p2l.face_atlas_set_model) g_p2l.face_atlas_set_model(rig, kL2dModelName[idx]);
    if (g_p2l.clip_count(rig) > 0) g_p2l.play(rig, 0);   // 播放第一个动作（待机）
    g_p2l.physics_stabilize(rig);                        // 头发/裙摆先落到静止位
    // 行为层是 per-rig 的：每个表情 rig 都要重开，否则眨眼/视线跟随静默失效。
    if (g_p2l.behaviors) g_p2l.behaviors(rig, P2L_BEHAVIOR_BLINK | P2L_BEHAVIOR_BREATH | P2L_BEHAVIOR_LOOK);
}

static bool Live2DOpenRig(Costume c, bool warnOnFail, bool allowLegacy) {
    int idx = (int)c;
    if (idx < 0 || idx > 3) idx = 0;
    g_l2dLastError.clear();

    auto fail = [&](const wchar_t* what, const char* detail = nullptr) -> bool {
        g_l2dLastError = what;
        if (warnOnFail) Live2DWarn(what, detail);
        return false;
    };

    // WinMain 已把当前目录设为 exe 所在目录，相对路径即 exe 同级。
    std::string pn = kL2dModelName[idx];

    // ========== 新结构：live2d/{ninja|daily|school|dress}/{表情}.p2lrt ==========
    // 探测目录里的 base.p2lrt；存在则按 8 表情独立 rig 整套预加载。
    std::string basePath = std::string("live2d/") + pn + "/base.p2lrt";
    {
        std::vector<uint8_t> probe;
        bool hasDir = ReadP2lrt(basePath.c_str(), probe) > 0;
        if (hasDir) {
            L2dExprRig loaded[L2D_EXPR_COUNT];
            int okCount = 0;
            char err[256] = { 0 };
            for (int e = 0; e < L2D_EXPR_COUNT; ++e) {
                std::string path = std::string("live2d/") + pn + "/" + kL2dExprFile[e] + ".p2lrt";
                std::vector<uint8_t> bytes;
                if (!ReadP2lrt(path.c_str(), bytes)) {
                    // 单个表情缺失不算致命：缺哪个就用 base 顶上，保证切换不卡住。
                    PetDbgA("[MakotoPet] missing expr rig\n");
                    continue;
                }
                err[0] = 0;
                void* model = g_p2l.model_load(bytes.data(), bytes.size(), P2L_LOAD_VERIFY_CRC, err, sizeof err);
                if (!model) continue;
                P2lRig* rig = (P2lRig*)g_p2l.rig_create(model, err, sizeof err);
                g_p2l.model_free(model);   // rig 持有模型，立即释放
                if (!rig) continue;
                InitOneRig(rig, idx);
                loaded[e].rig = rig;
                loaded[e].ok = true;
                ++okCount;
            }
            if (okCount == 0 || !loaded[0].ok) {
                // base 都没加载成功 → 整套装失败，释放已加载的
                for (int e = 0; e < L2D_EXPR_COUNT; ++e)
                    if (loaded[e].rig && g_p2l.rig_free) g_p2l.rig_free(loaded[e].rig);
                return fail(L"表情模型加载失败", err);
            }
            // 缺失的表情用 base 克隆指针共享（释放时按 ok 标记去重）——这里直接指向 base rig，
            // 但为避免 double free，ok=false 的槽位在释放循环里跳过，只有 base 槽负责释放。
            for (int e = 0; e < L2D_EXPR_COUNT; ++e) {
                if (!loaded[e].ok) { loaded[e].rig = loaded[0].rig; loaded[e].ok = false; }
            }

            P2lRig* baseRig = loaded[0].rig;
            float cw = 0.0f, ch = 0.0f;
            g_p2l.canvas(baseRig, &cw, &ch);
            if (!(cw > 0.0f) || !(ch > 0.0f)) {
                for (int e = 0; e < L2D_EXPR_COUNT; ++e)
                    if (loaded[e].ok && g_p2l.rig_free) g_p2l.rig_free(loaded[e].rig);
                return fail(L"模型画布尺寸无效");
            }
            // 校准用 base（中性脸），所有表情共用同一 content/变换，切换时不跳动。
            L2dContent content;
            if (!Live2DCalibrate(baseRig, cw, ch, content)) content = L2dContent{};

            // 首帧：先让 base 出第一帧，成功才替换，避免闪黑。
            const int pw = (g_currentW > 0) ? g_currentW : BASE_W;
            const int ph = (g_currentH > 0) ? g_currentH : BASE_H;
            std::vector<uint8_t> first((size_t)pw * (size_t)ph * 4, 0);
            float scale = 0.0f, tx = 0.0f, ty = 0.0f;
            Live2DFrameTransform(pw, ph, cw, ch, content, scale, tx, ty);
            float xf[6] = { scale, 0.0f, 0.0f, scale, tx, ty };
            bool drawn = g_p2l.render(baseRig, first.data(), (uint32_t)pw, (uint32_t)ph, xf, P2L_RENDER_STRAIGHT)
                         && (g_p2l.rig_failure(baseRig) == nullptr);
            if (!drawn) {
                for (int e = 0; e < L2D_EXPR_COUNT; ++e)
                    if (loaded[e].ok && g_p2l.rig_free) g_p2l.rig_free(loaded[e].rig);
                return fail(L"新模型首帧渲染失败，已保留当前服装");
            }

            // 走到这里整套已验证可用，才动全局状态：先释放旧的一套（统一入口，杜绝 double free）。
            ReleaseAllRigs();
            for (int e = 0; e < L2D_EXPR_COUNT; ++e) g_l2dExprRigs[e] = loaded[e];

            g_l2dRig = baseRig;
            g_l2dCurExpr = 0;
            g_l2dMultiRig = true;
            g_l2dCW = cw; g_l2dCH = ch;
            g_l2dContent = content;
            g_l2dRgba.swap(first);
            g_l2dFrameDirty = false;
            g_l2dLastMs = 0;
            g_l2dExprUntilMs = 0;
            // 新结构不再用 school8 的 atlas 表情机制，关闭避免干扰。
            g_school8Active = false;
            if (g_p2l.school8_enable) g_p2l.school8_enable(false);

            PetDbgA("[MakotoPet] multi-rig structure loaded\n");
            Live2DApplyWindowSize();
            return true;
        }
    }

    // ========== 旧结构：live2d/{pn}[_physics].p2lrt 单文件 + DLL atlas 表情 ==========
    std::string p0 = std::string("live2d/") + pn + "_physics.p2lrt";
    std::string p1 = std::string("live2d/") + pn + ".p2lrt";
    std::string p2 = std::string(pn) + "_physics.p2lrt";   // 也接受直接放在 exe 同级
    std::string p3 = std::string(pn) + ".p2lrt";
    std::vector<const char*> paths;
    paths.push_back(p0.c_str());                 // ★ 物理模型优先
    paths.push_back(p1.c_str());
    paths.push_back(p2.c_str());
    paths.push_back(p3.c_str());
    if (allowLegacy) { paths.push_back("mako.p2lrt"); paths.push_back("tml.p2lrt"); }

    std::vector<uint8_t> bytes;
    for (const char* p : paths) {
        if (ReadP2lrt(p, bytes) > 0) break;
        bytes.clear();
    }
    if (bytes.empty()) {
        g_l2dLastError = std::wstring(L"找不到模型 ") + kL2dModelLabel[idx];
        if (warnOnFail) Live2DWarn(g_l2dLastError.c_str());
        return false;
    }

    char err[256] = { 0 };
    void* model = g_p2l.model_load(bytes.data(), bytes.size(), P2L_LOAD_VERIFY_CRC, err, sizeof err);
    if (!model) return fail(L"模型解析失败", err);
    P2lRig* rig = (P2lRig*)g_p2l.rig_create(model, err, sizeof err);
    g_p2l.model_free(model);          // rig 持有模型，这里可以立即释放
    if (!rig) return fail(L"模型无法求值", err);

    // ★ 告诉定制 DLL 当前服装，随后 expression 会切到该服装的烘焙表情贴图。
    if (g_p2l.face_atlas_set_model)
        g_p2l.face_atlas_set_model(rig, kL2dModelName[idx]);

    // 旧单文件模型走 school8 / EXPR 表情机制。
    g_school8Active = (idx == (int)Costume::School) && (g_p2l.school8_enable != nullptr);
    if (g_p2l.school8_enable)
        g_p2l.school8_enable(g_school8Active);

    float cw = 0.0f, ch = 0.0f;
    g_p2l.canvas(rig, &cw, &ch);
    if (!(cw > 0.0f) || !(ch > 0.0f)) { g_p2l.rig_free(rig); return fail(L"模型画布尺寸无效"); }
    if (g_p2l.clip_count(rig) > 0) g_p2l.play(rig, 0);
    g_p2l.physics_stabilize(rig);
    if (g_p2l.behaviors)
        g_p2l.behaviors(rig, P2L_BEHAVIOR_BLINK | P2L_BEHAVIOR_BREATH | P2L_BEHAVIOR_LOOK);

    L2dContent content;
    if (!Live2DCalibrate(rig, cw, ch, content)) content = L2dContent{};

    const int pw = (g_currentW > 0) ? g_currentW : BASE_W;
    const int ph = (g_currentH > 0) ? g_currentH : BASE_H;
    std::vector<uint8_t> first((size_t)pw * (size_t)ph * 4, 0);
    float scale = 0.0f, tx = 0.0f, ty = 0.0f;
    Live2DFrameTransform(pw, ph, cw, ch, content, scale, tx, ty);
    float xf[6] = { scale, 0.0f, 0.0f, scale, tx, ty };
    bool drawn = g_p2l.render(rig, first.data(), (uint32_t)pw, (uint32_t)ph, xf, P2L_RENDER_STRAIGHT)
                 && (g_p2l.rig_failure(rig) == nullptr);
    if (!drawn) { g_p2l.rig_free(rig); return fail(L"新模型首帧渲染失败，已保留当前服装"); }

    ReleaseAllRigs();   // 统一释放（也可能刚从新结构服装换过来）
    g_l2dRig = rig;
    g_l2dCurExpr = 0;
    g_l2dMultiRig = false;
    g_l2dCW = cw; g_l2dCH = ch;
    g_l2dContent = content;
    g_l2dRgba.swap(first);
    g_l2dFrameDirty = false;
    g_l2dLastMs = 0;
    g_l2dExprUntilMs = 0;
    Live2DApplyWindowSize();
    return true;
}

static bool Live2DInit() {
    g_p2l.dll = LoadLibraryW(L"p2l_runtime.dll");
    // ★ 静默降级：没有运行时 DLL 是「纯序列帧部署」的正常情形，不弹框打断用户。
    if (!g_p2l.dll) { PetDbgA("[MakotoPet] no p2l_runtime.dll, use frame animation\n"); return false; }

    bool ok = true;
#define P2L_GET(field) \
    g_p2l.field = reinterpret_cast<decltype(g_p2l.field)>(GetProcAddress(g_p2l.dll, "p2l_" #field)); \
    if (!g_p2l.field) ok = false;
#define P2L_GET_OPT(field) \
    g_p2l.field = reinterpret_cast<decltype(g_p2l.field)>(GetProcAddress(g_p2l.dll, "p2l_" #field));
    P2L_GET(abi_version) P2L_GET(model_load) P2L_GET(rig_create) P2L_GET(model_free)
    P2L_GET(rig_free)    P2L_GET(rig_failure) P2L_GET(canvas)     P2L_GET(clip_count)
    P2L_GET(play)        P2L_GET(physics_stabilize) P2L_GET(update) P2L_GET(render)
    // 视线跟随是增量玩法：缺这两个导出时只是不跟随，不让整个 Live2D 回退。
    P2L_GET_OPT(behaviors) P2L_GET_OPT(look_at)
    // ★触摸物理反馈：可选导出，缺了只是没有裙摆冲量，不影响其余功能。
    P2L_GET_OPT(physics_impulse)
    // ★局部布料抓取：可选导出，缺了退回旧的物理冲量方案。
    P2L_GET_OPT(cloth_tug) P2L_GET_OPT(cloth_clear)
    // ★ 表情 API（可选）：缺任一导出时 g_p2l.expression 置空，表情功能降级为无，
    //   不影响其余功能。要求的三个必须同时存在才启用。
    P2L_GET_OPT(expression_count) P2L_GET_OPT(expression_id) P2L_GET_OPT(expression)
    if (!(g_p2l.expression_count && g_p2l.expression_id && g_p2l.expression))
        g_p2l.expression = nullptr;   // 三缺一就整体关闭，避免半套调用
    // ★ 多模型表情贴图：缺导出时不报错，只是表情贴图不随服装切换（降级为 ninja 贴图）。
    P2L_GET_OPT(face_atlas_set_model)
    // ★ school8 表情系统：缺导出时降级到旧表情贴图。
    P2L_GET_OPT(school8_enable)
#undef P2L_GET_OPT
#undef P2L_GET
    // 接口缺失 / ABI 不兼容：运行时本身不可用，才真正卸载并走序列帧。
    // 这里仍不弹框（用户要的是「自动降级」），只写调试日志。
    if (!ok) { PetDbgA("[MakotoPet] p2l_runtime.dll too old, use frame animation\n"); Live2DShutdown(); return false; }

    uint32_t abi = g_p2l.abi_version();
    if (!P2L_ABI_COMPATIBLE(abi)) {
        char dbg[160];
        sprintf_s(dbg, sizeof dbg, "[MakotoPet] runtime ABI %u.%u incompatible\n", abi >> 16, abi & 0xffffu);
        PetDbgA(dbg);
        Live2DShutdown();
        return false;
    }

    // ★ DLL 与 ABI 都 OK：运行时已就绪。这个标志与「当前服装有没有模型」无关——
    //   即使当前服装没有模型、本函数随后返回 false，DLL 也保持加载，
    //   这样换到有模型的服装时能立刻加载，实现按服装粒度的自动升级/降级。
    g_l2dLibReady = true;

    // LoadConfig() 在 WinMain 里先于本函数执行，这里直接用存档中的服装，
    // 重开程序自然回到上次穿的那套，不需要额外的配置项。
    // warnOnFail=false：当前服装没模型时静默回退序列帧，绝不弹框。
    // 注意失败时**不再** Live2DShutdown()——保留 DLL，供换装时复用。
    if (!Live2DOpenRig(g_currentCostume, false, true)) {
        PetDbgA("[MakotoPet] current costume has no model, use frame animation\n");
        return false;
    }
    return true;
}

// ★ 表情 rig 切换函数前置声明（定义在情感联动区，Live2DAdvance 的到期回退需要）。
static void L2dSwitchExprRig(int slot);

// 推进一次逻辑帧。只在 30fps 定时器里调用，拖拽引起的额外重绘不重复推进，
// 否则鼠标动得越快、模型动作越慢。
static void Live2DAdvance() {
    if (!g_l2dRig) return;
    ULONGLONG now = GetTickCount64();
    float dt = (float)FRAME_INTERVAL_MS / 1000.0f;
    if (g_l2dLastMs) {
        dt = (float)(now - g_l2dLastMs) / 1000.0f;
        float cap = (float)FRAME_INTERVAL_MS / 1000.0f * 2.0f;   // 休眠醒来不补算
        if (dt < 0.0f) dt = 0.0f;
        if (dt > cap)  dt = cap;
    }
    g_l2dLastMs = now;
    // ★ 表情到期自动回中性脸
    if (g_l2dExprUntilMs && now >= g_l2dExprUntilMs) {
        if (g_l2dMultiRig) {
            L2dSwitchExprRig(0);             // 新结构：切回 base rig
        } else if (g_p2l.expression) {
            g_p2l.expression(g_l2dRig, -1);  // 旧结构：淡出参数化表情
        }
        g_l2dExprUntilMs = 0;
    }
    // ★ 待机物理微动改由 p2l_behaviors 的 idle sway（ParamAngleX/ParamBodyAngleX 周期摆动）
    //   驱动摆锤链被动响应，不再注入外部风力——这是参考模型（psd2live 37 组物理链）的做法：
    //   摆锤是被动件，靠输入参数变化驱动，硬注入风只会让发/裙摆动方向和节奏失真。
    g_p2l.update(g_l2dRig, dt);
    if (const char* f = g_p2l.rig_failure(g_l2dRig)) {
        // 运行时内部 panic 不会展开到宿主，只会让句柄进入失效态；这里止损。
        Live2DWarn(L"运行时求值失败，已停止 Live2D 渲染", f);
        ReleaseAllRigs();   // 新结构下 g_l2dRig 是别名，必须整套统一释放，避免残留悬空指针
        // ★ 求值失败会切回序列帧；若启动时跳过了解码，这里补启动兜底。
        //   AnimLoadStart 幂等：后台加载完成后，主定时器每帧会自动同步帧缓存。
        AnimLoadStart();
        g_currentFrameIndex = 0;
    }
    g_l2dFrameDirty = true;
}

// 导演视线覆盖：LLM 可指定「看向主人/移开/低头」一小段时间，覆盖鼠标跟随。
// 到期自动恢复跟随鼠标，避免表情切走后视线一直僵在某个方向。
// （L2dGazeMode 枚举定义在 USE_LIVE2D 块外，见文件上方消息定义区）
static int         g_l2dGazeMode     = L2D_GAZE_FOLLOW;
static ULONGLONG   g_l2dGazeUntilMs  = 0;
static void L2dSetGazeOverride(int mode, int holdMs) {
    if (mode < 0) { g_l2dGazeMode = L2D_GAZE_FOLLOW; g_l2dGazeUntilMs = 0; return; }
    if (mode > L2D_GAZE_DOWN) mode = L2D_GAZE_FOLLOW;
    g_l2dGazeMode = mode;
    g_l2dGazeUntilMs = GetTickCount64() + (ULONGLONG)(holdMs > 0 ? holdMs : 2500);
}

// 让视线朝鼠标所在的方向偏。p2l_look_at 各轴取值 -1..1，且 y 轴向上，
// 屏幕 y 向下，因此要取反。量程取窗口自身的 1.5 倍：鼠标离角色约一个身位
// 就到极限，靠近时的移动也能看出眼珠在动；超出一律钳制，副屏/负坐标不越界。
static void Live2DLookAtCursor(const RECT& rc) {
    if (!g_l2dRig || !g_p2l.look_at) return;
    POINT pt = { 0, 0 };
    if (!GetCursorPos(&pt)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    float cx = (float)(rc.left + rc.right) * 0.5f;
    float cy = (float)(rc.top + rc.bottom) * 0.5f;
    float x = ((float)pt.x - cx) / ((float)w * 1.5f);
    float y = -((float)pt.y - cy) / ((float)h * 1.5f);
    if (x > 1.0f) x = 1.0f; else if (x < -1.0f) x = -1.0f;
    if (y > 1.0f) y = 1.0f; else if (y < -1.0f) y = -1.0f;
    // ★ 导演视线覆盖：未到期则用固定视线，到期自动恢复跟随鼠标
    ULONGLONG nowGaze = GetTickCount64();
    if (g_l2dGazeMode != L2D_GAZE_FOLLOW && nowGaze >= g_l2dGazeUntilMs)
        g_l2dGazeMode = L2D_GAZE_FOLLOW;
    if (g_l2dGazeMode != L2D_GAZE_FOLLOW) {
        switch (g_l2dGazeMode) {
        case L2D_GAZE_AT_USER: x = -0.55f; y = 0.40f; break;  // 看主人（偏下、稍侧）
        case L2D_GAZE_AWAY:    x = 0.85f;  y = 0.20f; break;   // 移开视线（傲娇）
        case L2D_GAZE_DOWN:    x = 0.0f;   y = -0.75f; break;  // 低头（害羞）
        }
    }
    g_p2l.look_at(g_l2dRig, x, y);
}

// ★ 八部位命中：判断客户区坐标落在人物哪个部位。
// 运行时未导出命中测试接口，用不透明内容包围盒 + 比例窗口做几何近似。
// 不同模型比例偏差大时调下面几个分界数即可，无需动运行时。
// 纵向 v: 0=顶 1=底 -> 头 0~0.14 / 脸 0.14~0.30 / 胸 0.30~0.46 /
// 腹 0.46~0.56 / 裙 0.56~0.76 / 腿 0.76~0.88 / 脚 0.88~1.0；手按"横向两侧"判定，独立于纵向。
#define L2D_HEAD_V1 0.14f    // 头部纵向终点（头发/头顶）
#define L2D_FACE_V1  0.30f
#define L2D_CHEST_V1 0.46f
#define L2D_BELLY_V1 0.56f    // 腹 0.46~0.56（下半让给裙摆带）
#define L2D_SKIRT_V0 0.56f    // 裙摆带上沿：起算独立的"扯裙子"部位
#define L2D_SKIRT_V1 0.76f    // 裙摆带下沿（腿上部）
#define L2D_LEG_V1   0.88f
#define L2D_HAND_V0  0.26f    // 手/手臂大致从胸部高度往下
#define L2D_HAND_V1  0.78f    // 到腿部之前
#define L2D_HAND_U_EDGE 0.24f // 横向偏离中心超过此值算"手/手臂"

// ★ 八部位：头 / 脸 / 手 / 胸 / 腹 / 裙 / 腿 / 脚
//   裙(L2D_PART_SKIRT)与摸头等同级：有自己的 LLM 描述，点击时注入裙摆物理冲量。
//   BODY 作为兼容别名保留 (=CHEST)，防止旧引用悬空；新逻辑一律用具体部位。
enum L2dBodyPart {
    L2D_PART_NONE = 0,
    L2D_PART_HEAD, L2D_PART_FACE, L2D_PART_HAND,
    L2D_PART_CHEST, L2D_PART_BELLY, L2D_PART_SKIRT, L2D_PART_LEG, L2D_PART_FOOT,
    L2D_PART_BODY = L2D_PART_CHEST
};

// ★ 部位 -> 自然语言描述：注入 LLM 的"主人刚刚做了什么"。
//   直接给模型这句话，让它自己决定用什么表情/台词回应，而不是写死气泡文案。
static const wchar_t* L2dTouchPhrase(L2dBodyPart part) {
    switch (part) {
        case L2D_PART_HEAD:  return L"主人摸了摸你的头";
        case L2D_PART_FACE:  return L"主人轻轻戳了戳你的脸颊";
        case L2D_PART_HAND:  return L"主人握住了你的手";
        case L2D_PART_CHEST: return L"主人把手放在了你的胸口";
        case L2D_PART_BELLY: return L"主人摸了摸你的肚子";
        case L2D_PART_SKIRT: return L"主人轻轻扯了扯你的裙子";
        case L2D_PART_LEG:   return L"主人碰了碰你的腿";
        case L2D_PART_FOOT:  return L"主人碰了碰你的脚";
        default:             return L"主人碰了碰你";
    }
}

// ★ 触摸旁白（第三人称，给对话界面 / 历史用）：
//   与上面的 L2dTouchPhrase 分开——那句是第一/二人称、直接喂给模型；
//   这句是给玩家看的旁白，主语用"你"、宾语用"茉子"，读起来才顺。
static const wchar_t* L2dTouchNarration(L2dBodyPart part) {
    switch (part) {
        case L2D_PART_HEAD:  return L"你摸了摸茉子的头";
        case L2D_PART_FACE:  return L"你轻轻戳了戳茉子的脸颊";
        case L2D_PART_HAND:  return L"你握住了茉子的手";
        case L2D_PART_CHEST: return L"你把手放在了茉子的胸口";
        case L2D_PART_BELLY: return L"你摸了摸茉子的肚子";
        case L2D_PART_SKIRT: return L"你轻轻扯了扯茉子的裙子";
        case L2D_PART_LEG:   return L"你碰了碰茉子的腿";
        case L2D_PART_FOOT:  return L"你碰了碰茉子的脚";
        default:             return L"你碰了碰茉子";
    }
}

// ★ 各服装的实际纵向分界（归一化 v，0=人物顶 1=脚底）。
//   数值来自 output28/<服装>/base/render/still.rgba 的 alpha 轮廓实测：
//   制服/私服2 的蓬蓬裙最宽处（横向铺到 u≈0~1）在 v≈0.45~0.55，
//   旧版把"裙"硬写成 0.56~0.76，其实那是腿的位置，点击自然对不上。
struct L2dBodyBands {
    float head, face, chest, belly, skirt1, leg;   // belly 同时是裙带上沿
    bool  hasSkirt;
};
static L2dBodyBands BandsForCostume(Costume c) {
    switch (c) {
    case Costume::School:  // 校服(制服)：蓬蓬裙
        return { 0.15f, 0.23f, 0.37f, 0.44f, 0.580f, 0.88f, true };
    case Costume::Dress:   // 私服2：蓬蓬裙
        return { 0.15f, 0.23f, 0.37f, 0.44f, 0.580f, 0.88f, true };
    case Costume::Ninja:   // 忍者服：贴身短裙，裙带更短（下沿约 0.55）
        return { 0.15f, 0.23f, 0.37f, 0.45f, 0.550f, 0.88f, true };
    case Costume::Daily:   // 私服1：裤子，无裙
        return { 0.15f, 0.23f, 0.37f, 0.50f, 0.580f, 0.88f, false };
    }
    return { 0.15f, 0.23f, 0.37f, 0.44f, 0.575f, 0.88f, true };
}

// 采样当前渲染帧 (px,py) 的颜色；该点几乎透明或无帧数据时返回 false。
//   g_l2dRgba 是 p2l_render 输出的窗口尺寸 RGBA（top-down），客户区坐标可直接索引。
static bool L2dSamplePixel(int px, int py, int& r, int& g, int& b) {
    int w = g_currentW, h = g_currentH;
    if (w <= 0 || h <= 0) return false;
    if (px < 0 || py < 0 || px >= w || py >= h) return false;
    size_t idx = ((size_t)py * (size_t)w + (size_t)px) * 4;
    if (g_l2dRgba.size() < idx + 4) return false;
    const uint8_t* p = &g_l2dRgba[idx];
    if (p[3] < 24) return false;               // 透明处 = 没点到人物
    r = p[0]; g = p[1]; b = p[2];
    return true;
}
// 肤色判定（与建模时抠手用的阈值同源）：用于把"露出的手"和"裙摆布料"区分开。
static bool L2dIsSkin(int r, int g, int b) {
    return r > 150 && g > 108 && b > 88 &&
           (r - b) > 16 && (r - g) > 6 && (r - g) < 90;
}

static L2dBodyPart Live2DHitPart(int px, int py) {
    if (!g_l2dRig || !g_l2dContent.ok) return L2D_PART_NONE;
    // 客户区像素 -> canvas 坐标：逆用渲染变换 (image = canvas*scale + t)
    float scale = 0.0f, tx = 0.0f, ty = 0.0f;
    Live2DFrameTransform(g_currentW, g_currentH, g_l2dCW, g_l2dCH,
                         g_l2dContent, scale, tx, ty);
    if (scale <= 0.0001f) return L2D_PART_NONE;
    float cx = ((float)px - tx) / scale;
    float cy = ((float)py - ty) / scale;
    // canvas -> 内容包围盒归一化坐标 u/v
    float u = (cx - g_l2dContent.x) / g_l2dContent.w;
    float v = (cy - g_l2dContent.y) / g_l2dContent.h;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return L2D_PART_NONE;

    // 像素级命中：有渲染帧时，点到透明处直接算未命中（比矩形包围盒更贴合身体轮廓）。
    int pr = 0, pg = 0, pb = 0;
    bool haveFrame = ((size_t)g_currentW * (size_t)g_currentH * 4 > 0) &&
                     (g_l2dRgba.size() >= (size_t)g_currentW * (size_t)g_currentH * 4);
    bool solid = L2dSamplePixel(px, py, pr, pg, pb);
    if (haveFrame && !solid) return L2D_PART_NONE;

    L2dBodyBands bd = BandsForCostume(g_currentCostume);

    // 手：身体中段、靠两侧、且该处是"肤色"才算手。
    //   用肤色而非固定 u 边界——蓬蓬裙横向铺到 u=0，按 u<0.24 会把裙摆误判成手。
    if (v >= 0.24f && v <= 0.66f && (u < 0.30f || u > 0.70f) && L2dIsSkin(pr, pg, pb))
        return L2D_PART_HAND;

    // 纵向按各服装实测轮廓分带
    if (v <= bd.head)  return L2D_PART_HEAD;
    if (v <= bd.face)  return L2D_PART_FACE;
    if (v <= bd.chest) return L2D_PART_CHEST;
    if (v <= bd.belly) return L2D_PART_BELLY;
    if (bd.hasSkirt && v <= bd.skirt1) return L2D_PART_SKIRT;
    if (v <= bd.leg)   return L2D_PART_LEG;
    return L2D_PART_FOOT;
}

// ★触摸物理反馈：扯裙子 —— 直接位移布料网格顶点，拉出去再弹性回弹。
//   ★2026-11-25 调整：触发条件收紧为只有 L2D_PART_SKIRT（扯裙子判定）。
//   为什么不用模型自带的物理链：Skirt/Trousers (preset) 那组输出被钳在 ±30，
//   实测最多只能把整件衣服挪动二三十像素（约人物宽度的 8%），贴身短裙和短裤更是几乎看不出，
//   而且它是"整件平移"，不是"被手抓住一角"。这里改成对网格顶点做局部剪切：
//   腰线固定、下摆被拉向点击那一侧并轻微上提，再叠阻尼振荡（先拉出、过冲、回稳）。
//   振幅以画布像素为单位，等于美术尺寸，四套衣服按各自衣长分别标定。
//   强度用 llm.ini 的 TouchTugScale（默认 1.0，0~3）整体缩放；TouchSkirtEnable=0 关闭。
//   旧 DLL 没有 p2l_cloth_tug 时自动退回原来的物理组冲量。
static void L2dClothTug(int px, int py, L2dBodyPart part) {
#if USE_LIVE2D
    if (!Live2DReady()) return;
    // ★只有"扯裙子"(L2D_PART_SKIRT)判定才触发布料扯动。
    //   腹/腿/脚虽同在下半身，但分别是摸肚子/碰腿/碰脚，不该把裙子一起拽走；
    //   旧条件把这四个部位都放进来，才导致点下半身任意处都会扯裙。
    if (part != L2D_PART_SKIRT) return;
    if (!GetLLMConfigInt(L"TouchSkirtEnable", 1, 0, 1)) return;

    // 客户区像素 -> 画布像素（与 Live2DHitPart 同一套变换）。布料模型在画布坐标里工作。
    float scale = 0.0f, tx = 0.0f, ty = 0.0f;
    Live2DFrameTransform(g_currentW, g_currentH, g_l2dCW, g_l2dCH, g_l2dContent, scale, tx, ty);
    if (scale <= 0.0001f) return;
    float cx = ((float)px - tx) / scale;
    float cy = ((float)py - ty) / scale;
    float u = (cx - g_l2dContent.x) / g_l2dContent.w;
    float dir = (u - 0.5f) * 2.0f;                   // -1(最左) .. +1(最右)
    if (dir < -1.0f) dir = -1.0f;
    if (dir >  1.0f) dir =  1.0f;
    // 正中间不好判断往哪边扯，按靠近的一侧给一个最小方向，保证点中间也有反应。
    if (dir > -0.25f && dir < 0.25f) dir = (u < 0.5f) ? -0.35f : 0.35f;

    // 各服装实测标定（画布 1024 高、人物高约 1020）：
    //   amp    下摆峰值位移(px)   radius 影响半径(px)
    //   lift   上提比例(0~1)      freq 摆动频率(Hz)   damp 阻尼(1/s)
    float amp, radius, lift, freq, damp;
    switch (g_currentCostume) {
    case Costume::School:   // 制服：蓬蓬裙，布量大、惯性大，摆幅最大、衰减最慢
    case Costume::Dress:    // 私服2：同款蓬蓬裙
        amp = 115.0f; radius = 310.0f; lift = 0.80f; freq = 2.2f; damp = 3.2f;
        break;
    case Costume::Ninja:    // 忍者服：贴身超短裙，只能小幅甩动，回弹快
        amp = 65.0f;  radius = 200.0f; lift = 0.60f; freq = 3.0f; damp = 5.5f;
        break;
    default:                // 私服1：短裤，腰胯贴着身体，只在下摆外侧拖一点
        amp = 58.0f;  radius = 135.0f; lift = 0.30f; freq = 3.4f; damp = 6.5f;
        break;
    }
    double k = GetLLMConfigDouble(L"TouchTugScale", 1.0, 0.0, 3.0);
    amp = (float)(amp * k);
    if (amp < 0.5f) return;

    // 四套模型的裙/裤都在 bottomwear 网格上（忍者服是短裙，私服1是短裤）。
    if (g_p2l.cloth_tug) {
        if (g_p2l.cloth_tug(g_l2dRig, "bottomwear,skirt", cx, cy, dir, -0.12f,
                            amp, radius, lift, freq, damp))
            return;
    }
    // 降级：旧 DLL 只支持物理组冲量（幅度明显小，但不至于没反应）。
    if (g_p2l.physics_impulse)
        g_p2l.physics_impulse(g_l2dRig, "skirt", dir * 0.8f, -0.2f);
#else
    (void)px; (void)py; (void)part;
#endif
}

// 把一帧 RGBA 写进 32bpp DIB。p2l_render 输出 rows top first 的直通 RGBA，
// DIB 用负 biHeight 同样是 top-down，两边行序一致，只需交换 R/B。
static void Live2DBlit(int w, int h) {
    if (!g_dibBits || !g_l2dRig) return;
    const size_t pixels = (size_t)w * (size_t)h;
    if (g_l2dFrameDirty) {
        g_l2dRgba.assign(pixels * 4, 0);
        float scale = 0.0f, tx = 0.0f, ty = 0.0f;
        Live2DFrameTransform(w, h, g_l2dCW, g_l2dCH, g_l2dContent, scale, tx, ty);
        float xf[6] = { scale, 0.0f, 0.0f, scale, tx, ty };
        if (!g_p2l.render(g_l2dRig, g_l2dRgba.data(), (uint32_t)w, (uint32_t)h, xf, P2L_RENDER_STRAIGHT))
            g_l2dRgba.assign(pixels * 4, 0);
        g_l2dFrameDirty = false;
    }
    if (g_l2dRgba.size() < pixels * 4) return;
    const uint8_t* s = g_l2dRgba.data();
    uint8_t* d = g_dibBits;
    for (size_t i = 0; i < pixels; ++i, s += 4, d += 4) {
        d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];   // RGBA -> BGRA
    }
}

// ULW_ALPHA 要求源位图的颜色已按 alpha 预乘。整块画布统一在这里转换，
// 这样 GDI+ 画上去的状态栏也用同一套直通语义，不必分两处处理。
static void Live2DPremultiply(int w, int h) {
    if (!g_dibBits) return;
    uint8_t* p = g_dibBits;
    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; ++i, p += 4) {
        unsigned a = p[3];
        p[0] = (uint8_t)((unsigned)p[0] * a / 255u);
        p[1] = (uint8_t)((unsigned)p[1] * a / 255u);
        p[2] = (uint8_t)((unsigned)p[2] * a / 255u);
    }
}
#endif

// ------------------------------------------------------------------
//  情感三联动：情绪标签 -> 动作片段 / TTS 语气 / 好感
//  P2L 运行时没有表情通道，所以"表情"这一维用动作片段 + 视线 + 音效
//  共同表达。模型返回 emotion 字符串，这里统一解析成枚举，再驱动三层反馈。
//  定义在 #if 外：序列帧模式也要解析（用于心情提示与主动搭话）。
// ------------------------------------------------------------------
// ★ 8 个情绪 = 8 个表情槽，一一对应（枚举值就是槽位号，别再插中间值）。
//   槽位顺序与 kL2dExprFile / live2d/{服装}/ 下的文件名严格一致：
//     0 base(平静) 1 smile(开心) 2 smile_sulky(坏笑) 3 panic(惊讶)
//     4 shy_blush(害羞) 5 embarrassed(难过) 6 thinking2(思考) 7 wonder(疑惑)
//   prompt.txt 里的取值必须与这 8 个一一对应；改这里就要同步改 prompt。
// ★ 枚举名 = 表情槽文件名（去掉下划线后首字母大写），枚举值 = 槽位号。
//   prompt.txt 里必须原样使用下面的槽位名，不要再用"开心/生气"这类自造情绪名。
enum class PetEmotion {
    Base = 0,      // 0 base        平静
    Smile,         // 1 smile       微笑
    SmileSulky,    // 2 smile_sulky 坏笑
    Panic,         // 3 panic       惊慌
    ShyBlush,      // 4 shy_blush   害羞脸红
    Embarrassed,   // 5 embarrassed 尴尬抿嘴
    Thinking,      // 6 thinking2   思考
    Wonder         // 7 wonder      疑惑
};

// 导演指令：LLM 一轮回复里除 emotion 外可附带的多模态演出参数。
// 由后台 LLM 线程 new 出来，经 WM_APP_DIRECTOR 投递主线程，主线程用完 delete。
// 所有字段都有"缺省=无"语义，模型没输出时完全退回旧行为（只按 emotion 演出）。
struct L2dDirectorCmd {
    PetEmotion emotion   = PetEmotion::Base;
    int        moodDelta = 0;     // 心情微调（-1/0/1）
    float      intensity = 0.0f;  // 情绪强度 0..1；<=0 表示模型没给，忽略
    int        action    = -1;    // 动作槽覆盖：-1=不覆盖，0=待机,1=思考,2=开心
    int        gaze      = -1;    // 视线覆盖：-1=不覆盖，见 L2dGazeMode
    int        gazeHoldMs = 2500; // 视线保持时长
    int        holdMs     = 0;    // 表情保持时长：0=用默认 L2D_EXPR_HOLD_MS；说话时按打字时长延长
};

// ==================================================================
//  玩法：Live2D 动作片段调度（轮播 + 好感解锁）
//
//  p2l_runtime 只导出 clip_count / play，不导出片段名。4 套 .p2lrt 的
//  CLIP 段均为 3 个片段，约定槽位 0=待机、1=点头/思考、2=摇摆/开心。
//  若你的模型片段顺序不同，改这里的槽位常量即可；运行时还会用 clip_count
//  再兜底一次，片段不足时安全回退到待机，绝不越界。
// ==================================================================
#if USE_LIVE2D
enum L2dSlot { L2D_IDLE = 0, L2D_THINK = 1, L2D_PEACE = 2 };
#define L2D_SLOT_COUNT 3
// ★ 情绪枚举必须与 8 个表情槽一一对应：数量或顺序对不上就直接编译失败，
//   避免"prompt 加了新情绪、表情表没加"这种只在运行时才发现的错配。
static_assert((int)PetEmotion::Wonder + 1 == L2D_EXPR_COUNT,
              "PetEmotion 必须正好 8 个，且顺序与 kL2dExprFile 一致（枚举值即表情槽位号）");

// 槽位好感门槛：待机/思考恒可用，"开心"需累计好感达标才进入待机轮播。
// 与 UNLOCK_DEF 第一档（背景音乐 15）错开，让养成节奏有层次。
#define L2D_PEACE_NEED 30

// 部位点击（摸头）：连击窗口与彩蛋阈值
static const ULONGLONG PAT_COMBO_WINDOW_MS = 2500;   // 两次摸头算连击的最大间隔
static const ULONGLONG PAT_REWARD_COOLDOWN = 4000;   // 彩蛋奖励冷却，防连点刷好感
static const int       PAT_COMBO_NEED       = 5;      // 连击几次触发彩蛋
static ULONGLONG g_lastPatMs    = 0;
static ULONGLONG g_lastPatEggMs = 0;
static int       g_patCombo     = 0;

// 播放一个片段。slot 越界一律回退待机，模型片段不足时安全降级。
static void L2dPlayClip(int slot) {
    if (!g_l2dRig || !g_p2l.play || !g_p2l.clip_count) return;
    int n = (int)g_p2l.clip_count(g_l2dRig);
    if (n <= 0) return;
    if (slot < 0 || slot >= n) slot = L2D_IDLE;
    g_l2dLastClipSlot = slot;          // 记住当前动作，切表情 rig 后据此重放
    g_p2l.play(g_l2dRig, slot);
    g_l2dFrameDirty = true;
}

// 情绪 -> 动作槽位。只有 3 个片段可用（0待机/1点头思考/2摇摆开心），
// 按语义就近归类；表情由 8 个独立 rig 承担，动作只做辅助。
// 依赖 L2D_* 槽位常量，故留在 #if USE_LIVE2D 块内；序列帧模式不使用本映射。
static int L2dSlotForEmotion(PetEmotion e) {
    switch (e) {
    case PetEmotion::Smile: return L2D_PEACE;   // 开心 -> 摇摆
    case PetEmotion::Panic:
    case PetEmotion::ShyBlush:
    case PetEmotion::Embarrassed:
    case PetEmotion::Thinking:
    case PetEmotion::Wonder: return L2D_THINK;   // 静态情绪 -> 点头/思考
    // 坏笑不摇摆开心：用待机站姿 + 坏笑表情表达，比让坏笑的身体做开心动作自然。
    case PetEmotion::SmileSulky: return L2D_IDLE;
    default:                    return L2D_IDLE;    // 平静 -> 待机
    }
}

// ★ 8 张原作差分的表情槽位（与运行时 school8 的 atlas 槽位严格对齐）：
//   0 base 中性 / 1 smile 大笑 / 2 smile_sulky 坏笑 / 3 panic 惊慌
//   4 shy_blush 害羞脸红 / 5 embarrassed 尴尬 / 6 thinking2 思考 / 7 wonder 疑惑
enum School8Slot { S8_BASE=0, S8_SMILE=1, S8_SULKY=2, S8_PANIC=3,
                   S8_SHYPOS=4, S8_EMBARR=5, S8_THINK=6, S8_WONDER=7 };

// ★ 情绪 -> 8 差分槽位（school8 模式直接用槽位号，不查 EXPR 表——物理模型无 EXPR chunk）。
//   PetEmotion 的枚举值已经是 0..7，与 School8Slot 一一对应，直接取序号即可。
static int L2dEmotionSlot8(PetEmotion e) {
    int s = (int)e;
    return (s >= S8_BASE && s <= S8_WONDER) ? s : S8_BASE;
}

// ★ 旧模型（有 EXPR chunk）的情绪 -> 表情 id。旧 DLL/旧模型沿用的 7 槽英文 id。
//   旧 7 槽没有「思考 / 疑惑」，返回 nullptr 时 L2dSetExpression 会淡出成中性脸。
static const char* L2dEmotionExprId(PetEmotion e) {
    switch (e) {
    case PetEmotion::Smile: return "smile";
    case PetEmotion::SmileSulky: return "angry";
    case PetEmotion::Panic: return "surprised";
    case PetEmotion::ShyBlush: return "shy";
    case PetEmotion::Embarrassed: return "sad";
    default:                    return nullptr;   // 平静/思考/疑惑：保持中性脸
    }
}

// ★ 设置表情：按 id 名在当前 rig 的表情表里查找索引，找到则淡入、其余淡出。
//   -1 表示淡出全部（中性脸）。不缓存索引——换装后 rig 变了，现查更安全。
//   旧 DLL 没有表情 API 时本函数静默返回，功能优雅降级。
// ★ 新结构：情绪 -> 8 表情槽位。枚举值就是槽位号，不再有手工映射表。
static int L2dExprSlotForEmotion(PetEmotion e) {
    int s = (int)e;
    return (s >= 0 && s < L2D_EXPR_COUNT) ? s : 0;
}

// ★ 把当前显示的 rig 切到指定表情槽位（仅新结构）。
//   所有表情共用 base 校准出的 g_l2dContent / g_l2dCW / g_l2dCH，切换时不跳位。
static void L2dSwitchExprRig(int slot) {
    if (slot < 0 || slot >= L2D_EXPR_COUNT) return;
    P2lRig* target = g_l2dExprRigs[slot].rig;
    if (!target || target == g_l2dRig) { g_l2dCurExpr = slot; return; }
    g_l2dRig = target;
    g_l2dCurExpr = slot;
    // 目标 rig 可能是刚从冻结状态唤醒的：稳一下物理、确保行为层开着。
    g_p2l.physics_stabilize(target);
    if (g_p2l.behaviors) g_p2l.behaviors(target, P2L_BEHAVIOR_BLINK | P2L_BEHAVIOR_BREATH | P2L_BEHAVIOR_LOOK);
    // ★ 在"新" rig 上重放本轮动作。动作片段是 rig 私有的，不重放的话
    //   L2dPlayClip -> L2dSwitchExprRig 这条常见顺序会让动作停在旧 rig 上，
    //   玩家看到的就是"表情对了但身体没动"。回中性脸时同样重放，避免动作被打断。
    if (g_l2dLastClipSlot > L2D_IDLE) L2dPlayClip(g_l2dLastClipSlot);
    g_l2dFrameDirty = true;
}

static void L2dSetExpression(PetEmotion e) {
    if (!g_l2dRig) return;

    // ===== 新结构：直接切 rig 指针，表情即独立烘焙模型 =====
    if (g_l2dMultiRig) {
        int slot = L2dExprSlotForEmotion(e);
        L2dSwitchExprRig(slot);
        if (slot != 0) g_l2dExprUntilMs = GetTickCount64() + L2D_EXPR_HOLD_MS;
        else           g_l2dExprUntilMs = 0;   // base 中性不需要回退
        return;
    }

    // ===== 旧结构：DLL 内嵌 atlas / EXPR 参数化表情 =====
    if (!g_p2l.expression) return;
    int32_t idx = -1;                       // 默认淡出所有表情

    if (g_school8Active) {
        int slot = L2dEmotionSlot8(e);
        idx = (slot == S8_BASE) ? -1 : slot;
    } else {
        const char* want = L2dEmotionExprId(e);
        if (want && g_p2l.expression_id && g_p2l.expression_count) {
            uint32_t n = g_p2l.expression_count(g_l2dRig);
            for (uint32_t i = 0; i < n; ++i) {
                const char* id = g_p2l.expression_id(g_l2dRig, i);
                if (id && strcmp(id, want) == 0) { idx = (int32_t)i; break; }
            }
        }
    }
    g_p2l.expression(g_l2dRig, idx);        // 找到则淡入，没找到/中性则淡出
    if (idx >= 0) g_l2dExprUntilMs = GetTickCount64() + L2D_EXPR_HOLD_MS;
    else          g_l2dExprUntilMs = 0;     // 中性脸不需要回退
    g_l2dFrameDirty = true;
}
#endif   // USE_LIVE2D

// 情绪 -> TTS 语气（语速 / 音调浮动系数，乘在 ini 基准值上）。
// GPT-SoVITS 的 speed_factor 0.5~2.0；这里给温和偏移，避免过度变声。
// 放在 #if 外：序列帧模式的 TTS 也要随情绪变调。
struct EmotionTone { double speed; double temperature; };
static EmotionTone ToneForEmotion(PetEmotion e) {
    switch (e) {
    case PetEmotion::Smile: return { 1.10, 1.10 };
    case PetEmotion::SmileSulky: return { 1.06, 0.95 };
    case PetEmotion::Panic: return { 1.16, 1.20 };
    case PetEmotion::ShyBlush: return { 0.94, 1.05 };
    case PetEmotion::Embarrassed: return { 0.88, 0.85 };
    case PetEmotion::Thinking: return { 0.96, 0.92 };
    case PetEmotion::Wonder: return { 1.02, 1.02 };
    default:                    return { 1.00, 1.00 };   // 平静
    }
}

// 情绪解析放在 #if 外：序列帧模式也要解析（用于好感修正与主动搭话），
// 只是不驱动 Live2D 动作。模型可能输出中文/英文/带空格，这里统一小写后匹配。
//
// ★★ prompt 取值 = 表情槽文件名，完全一致（改一处必须改三处）★★
//    槽 0 base        平静       槽 4 shy_blush   害羞脸红
//    槽 1 smile       微笑       槽 5 embarrassed 尴尬抿嘴
//    槽 2 smile_sulky 坏笑   槽 6 thinking2   思考
//    槽 3 panic       惊慌       槽 7 wonder      疑惑
//   下面先认"槽位原名"（长名优先，避免 smile_sulky 被 smile 抢先命中），
//   再认旧情绪词/中文作兜底，保证老 prompt 或模型自由发挥也不会解析失败。
static PetEmotion ParseEmotion(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c == ' ' || c == '"' || c == '\'' || c == '\r' || c == '\n') continue;
        s += (char)tolower((unsigned char)c);
    }
    auto has = [&s](const char* k) { return s.find(k) != std::string::npos; };

    // 1) 槽位原名：长名必须先判，否则 "smile_sulky" 会被 "smile" 截断。
    if (has("smile_sulky")) return PetEmotion::SmileSulky;
    if (has("shy_blush"))   return PetEmotion::ShyBlush;
    if (has("thinking2"))   return PetEmotion::Thinking;
    if (has("embarrassed")) return PetEmotion::Embarrassed;
    if (has("panic"))       return PetEmotion::Panic;
    if (has("wonder"))      return PetEmotion::Wonder;
    if (has("smile"))       return PetEmotion::Smile;
    if (has("base"))        return PetEmotion::Base;

    // 2) 旧情绪词 / 中文兜底（prompt 已不再使用这些写法）。
    if (has("angry") || has("生气") || has("愤怒") || has("恼") || has("坏笑") || has("撇嘴")) return PetEmotion::SmileSulky;
    if (has("shy") || has("害羞") || has("羞") || has("脸红")) return PetEmotion::ShyBlush;
    if (has("think") || has("思考") || has("沉思") || has("琢磨")) return PetEmotion::Thinking;
    if (has("sad") || has("难过") || has("伤心") || has("委屈") || has("低落") ||
        has("消沉") || has("gloom") || has("阴沉") || has("沮丧") || has("郁闷") ||
        has("阴郁") || has("尴尬")) return PetEmotion::Embarrassed;
    if (has("surpris") || has("惊讶") || has("惊") || has("慌")) return PetEmotion::Panic;
    if (has("puzzl") || has("confus") || has("curious") || has("疑惑") ||
        has("困惑") || has("好奇") || has("迷茫")) return PetEmotion::Wonder;
    if (has("happy") || has("开心") || has("高兴") || has("joy") || has("love") ||
        has("喜爱") || has("喜欢") || has("亲昵") || has("excited") || has("激动") ||
        has("兴奋") || has("欣喜")) return PetEmotion::Smile;
    if (has("neutral") || has("平静") || has("normal")) return PetEmotion::Base;
    return PetEmotion::Base;
}

// 槽位 -> 中文标签（气泡标题用；prompt 侧只用槽位原名，这里给人看用中文）
static const wchar_t* EmotionLabel(PetEmotion e) {
    switch (e) {
    case PetEmotion::Smile: return L"微笑";
    case PetEmotion::SmileSulky: return L"坏笑";
    case PetEmotion::Panic: return L"惊慌";
    case PetEmotion::ShyBlush: return L"害羞";
    case PetEmotion::Embarrassed: return L"抿嘴";
    case PetEmotion::Thinking: return L"思考";
    case PetEmotion::Wonder: return L"疑惑";
    default:                      return L"平静";
    }
}

// 情绪 -> 心情微调（叠加到 affinityMood，负值只压心情不动累计值）。
// 让"情绪"这一层在爱心显示上也能被看见，形成动作/语气/数值三重反馈。
static int EmotionMoodDelta(PetEmotion e) {
    switch (e) {
    case PetEmotion::Smile: return 1;
    case PetEmotion::Embarrassed:
    case PetEmotion::SmileSulky: return -1;
    default:                    return 0;
    }
}

// ------------------------------------------------------------------
//  主动搭话：定时让茉子主动说一句，结合时段与当前状态。
//  配置写在 llm.ini 的 [LLM] 段：
//    ProactiveEnable     默认 1
//    ProactiveIntervalSec 默认 180（两次主动搭话的最小间隔）
//    ProactiveIdleSec     默认 60 （多久没有用户输入后才允许主动搭话）
//    ProactiveEmotionAffinity 默认 1（情绪是否叠加心情微调）
// ------------------------------------------------------------------
static ULONGLONG g_lastProactiveMs   = 0;   // 上次主动搭话"成功出气泡"的时刻
static ULONGLONG g_proactiveStartMs  = 0;   // 本次主动搭话线程启动时刻（卡死兜底用）
static ULONGLONG g_lastUserInputMs   = 0;   // 上次用户输入时刻（避免打断用户）
static bool      g_proactiveRunning  = false; // 后台线程互斥，防重复触发

// ★触摸互动：LLM 回应互斥 + 节流。触摸会真实发起网络请求，必须防连点堆积。
static volatile bool g_touchRunning  = false; // 触摸 LLM 请求进行中
static ULONGLONG g_lastTouchAskMs    = 0;     // 上次发起触摸请求的时刻（节流用）

// 当前时段描述，注入主动搭话的 prompt，让台词呼应真实时间。
static const wchar_t* TimeOfDayLabel() {
    SYSTEMTIME st; GetLocalTime(&st);
    int h = st.wHour;
    if (h >= 5  && h < 9)  return L"清晨";
    if (h >= 9  && h < 12) return L"上午";
    if (h >= 12 && h < 14) return L"中午";
    if (h >= 14 && h < 18) return L"下午";
    if (h >= 18 && h < 22) return L"晚上";
    return L"深夜";
}

static bool g_bStartupPlayed = false;
const float CHAR_RATIO = 1.0f;

vector<wstring> g_bgmFileList;
vector<wstring> g_bgmDisplayNameList;
int g_currentBGMIndex = 0;
// 翻唱变调档位：-1 / 0 / +1（男声源配女声模型通常选 +1），存 config.ini 的 [RVC] Octave
// 实际半音数 = 档位 x RVC_OCTAVE_SEMI
int g_rvcOctave = 0;

// ★ 语音朗读总开关。必须声明在 LoadConfig 之前（那里要读 [TTS] TtsEnable），
//   具体实现（切句/队列）在后面的 TTS 模块里。
static std::atomic<bool> g_ttsVoiceOn{ true };

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

struct SfxCallback;   // 完整定义在下面；XSound 只保存它的指针

struct XSound {
    WAVEFORMATEX wfx{};
    std::vector<BYTE> pcm;
    IXAudio2SourceVoice* v = nullptr;
    SfxCallback* myCb = nullptr;   // 本对象固定占用的回调槽，寿命内不更换
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
static std::atomic<int> g_sfxCbIdx{ 0 };

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
// 集中提供"带检查"的包装：
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
        // cs 来自文件头，畸形文件可能给到 0xFFFFFFxx；直接相加会让 DWORD
        // 回绕成很小的值，循环条件重新成立 → 死循环。三处 WAV 解析都要防。
        unsigned long long step = 8ull + cs + (cs & 1u);
        unsigned long long next = (unsigned long long)pos + step;
        if (next > 0xFFFFFFFFull) break;
        pos = (DWORD)next;
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
            // ★安全：fmt 块声明长度可能大于文件实际剩余字节（截断/损坏文件）。
            //   只保证 pos+8 在界内还不够，memcpy 会按 cs 读出去 → 越界读。
            size_t availFmt = wavData.size() - ((size_t)pos + 8);
            DWORD tr = (DWORD)min((size_t)cs, sizeof(WAVEFORMATEX));
            if ((size_t)tr > availFmt) tr = (DWORD)availFmt;
            if (tr) memcpy(&tmp, wavData.data() + pos + 8, tr);
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
        // cs 来自文件头，畸形文件可能给到 0xFFFFFFxx；直接相加会让 DWORD
        // 回绕成很小的值，循环条件重新成立 → 死循环。三处 WAV 解析都要防。
        unsigned long long step = 8ull + cs + (cs & 1u);
        unsigned long long next = (unsigned long long)pos + step;
        if (next > 0xFFFFFFFFull) break;
        pos = (DWORD)next;
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
        // 每个声音对象固定领一个回调槽，寿命内不换：轮询复用会让旧声音的
        // OnBufferEnd 改写新声音的 playing 标志，播放状态会永久卡死。
        if (!s.myCb) s.myCb = &g_sfxCb[g_sfxCbIdx.fetch_add(1) % 16];
        s.myCb->s = &s; s.playing = false;
        HRESULT hr = g_xa->CreateSourceVoice(&s.v, &s.wfx, 0,
            XAUDIO2_DEFAULT_FREQ_RATIO, s.myCb, nullptr, nullptr);
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
// ★ 窗口几何 / 换装存档（序列帧与 Live2D 共用）
//   两条渲染路径都要在退出时保存服装、窗口位置和大小；
//   这里补齐三处，保证 Live2D 路径也能记住：
//     1) 只在 WM_DESTROY 落盘一次 —— 用任务管理器结束进程就什么都没写下去；
//     2) WritePrivateProfileString 只写内存缓存，进程被强杀时缓存直接丢弃，
//        必须显式 flush 才真的落盘；
//     3) Live2D 的窗口尺寸是按模型宽高比重算的，而模型加载发生在"应用存档位置"
//        之后，重算会把刚读出来的尺寸/坐标顶掉（见 Live2DApplyWindowSize /
//        RestoreSavedGeometry）。
static bool      g_geomDirty    = false;   // 位置/尺寸变过、还没落盘
static ULONGLONG g_geomDirtyMs  = 0;
static int       g_restoreWinH  = 0;       // 存档里的窗口高度（0 = 没有存档）

static void MarkGeomDirty() { g_geomDirty = true; g_geomDirtyMs = GetTickCount64(); }

static bool ConfigIniPath(std::wstring& out) {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return false;
    PathRemoveFileSpecW(exePath);
    out = std::wstring(exePath) + L"\\config.ini";
    return true;
}

// 只写几何：拖动、滚轮缩放、换装都会走到这里，写入量很小
static void SaveWindowState() {
    std::wstring ini;
    if (!ConfigIniPath(ini)) return;
    int x = 0, y = 0, h = 0;
    RECT rc{};
    if (g_hwnd && IsWindow(g_hwnd) && GetWindowRect(g_hwnd, &rc)
        && rc.right > rc.left && rc.bottom > rc.top) {
        x = rc.left; y = rc.top; h = rc.bottom - rc.top;
    } else if (g_winPosKnown && g_currentH > 0) {
        // 窗口已销毁（或还没建）：退回逻辑坐标，绝不把 0,0 写进存档
        x = g_winX; y = g_winY; h = g_currentH;
    } else {
        return;
    }
    // 宽度按 2:3 基准反推，而不是存 Live2D 那个"贴着人物收紧"的窄宽度：
    // 高度才是用户调出来的缩放，Live2D 每次加载都会按模型比例重算宽度，
    // 但序列帧路径直接吃这个宽度——存窄宽度会让序列帧人物被横向压扁。
    int wStore = (int)(BASE_W * ((float)h / (float)BASE_H) + 0.5f);
    if (wStore < 40) wStore = 40;
    wchar_t buf[32];
    wsprintfW(buf, L"%d", x); WritePrivateProfileStringW(L"State", L"PosX", buf, ini.c_str());
    wsprintfW(buf, L"%d", y); WritePrivateProfileStringW(L"State", L"PosY", buf, ini.c_str());
    wsprintfW(buf, L"%d", wStore); WritePrivateProfileStringW(L"State", L"WinW", buf, ini.c_str());
    wsprintfW(buf, L"%d", h); WritePrivateProfileStringW(L"State", L"WinH", buf, ini.c_str());
    // 窗口高度就是缩放尺度的真身：写回 Zoom，两条渲染路径才会拿到同一个尺度
    float z = (float)h / (float)BASE_H;
    if (z < 0.5f) z = 0.5f;
    if (z > 3.0f) z = 3.0f;
    wsprintfW(buf, L"%.2f", z);
    WritePrivateProfileStringW(L"State", L"Zoom", buf, ini.c_str());
    g_geomDirty = false;
}

// ★ 显式 flush 配置缓存：不调用它，写入只停在内存里，
//   进程被强杀/崩溃重启后读到的还是旧值 —— 表现就是"退出时的状态没记住"。
static void FlushConfigCache() {
    std::wstring ini;
    if (ConfigIniPath(ini)) WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
}

void SaveConfig() {
    std::wstring ini;
    if (!ConfigIniPath(ini)) return;
    wchar_t buf[32];
    wsprintfW(buf, L"%d", (int)g_currentCostume);
    WritePrivateProfileStringW(L"State", L"Costume", buf, ini.c_str());
    wsprintfW(buf, L"%d", g_currentVolume);
    WritePrivateProfileStringW(L"State", L"Volume", buf, ini.c_str());
    wsprintfW(buf, L"%d", g_currentBGMIndex);
    WritePrivateProfileStringW(L"BGM", L"Index", buf, ini.c_str());
    wsprintfW(buf, L"%d", g_rvcOctave);
    WritePrivateProfileStringW(L"RVC", L"Octave", buf, ini.c_str());
    wsprintfW(buf, L"%d", (int)g_renderMode);
    WritePrivateProfileStringW(L"State", L"RenderMode", buf, ini.c_str());
    SaveWindowState();          // 位置 / 尺寸 / 缩放
    FlushConfigCache();         // 立刻落盘，强杀进程也不丢
}

// 解锁判定函数定义在"成就统计系统"区（要用 PetStats），这里先声明给读档逻辑用
static bool UnlockReady(int id);
static int  CostumeUnlock(int idx);
// 好感度是否已满。同样定义在"成就统计系统"区，读档逻辑要用，先声明
static bool AffinityMaxed();

void LoadConfig() {
    std::wstring iniPathW;
    if (!ConfigIniPath(iniPathW)) return;
    const wchar_t* iniPath = iniPathW.c_str();
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
    // 上次选的渲染方式：这里只记"想用哪个"，实际用哪个由素材扫描结果决定（见 WinMain）。
    // ★ 老存档没有这一项时默认 Live2D —— 沿用"有模型就用模型"的旧行为，
    //   不能让升级后的第一次启动突然退回序列帧。
    {
        wchar_t rm[16];
        GetPrivateProfileStringW(L"State", L"RenderMode", L"", rm, 16, iniPath);
        g_wantMode = (rm[0] == L'\0') ? RM_LIVE2D
                                      : ((_wtoi(rm) != 0) ? RM_LIVE2D : RM_FRAMES);
    }
    GetPrivateProfileStringW(L"State", L"Zoom", L"1.0", buf, 32, iniPath);
    g_zoom = (float)_wtof(buf);
    if (g_zoom < 0.5f) g_zoom = 0.5f;
    if (g_zoom > 3.0f) g_zoom = 3.0f;
    GetPrivateProfileStringW(L"State", L"PosX", L"-1", buf, 32, iniPath);
    g_savedPosX = _wtoi(buf);
    GetPrivateProfileStringW(L"State", L"PosY", L"-1", buf, 32, iniPath);
    g_savedPosY = _wtoi(buf);
    // 放宽拖动边界后窗口可以部分探出屏幕边缘，位置可能是负数。
    // 仍把 (-1,-1) 当作旧版「没有存过位置」的哨兵。
    g_hasSavedPos = (g_savedPosX > -10000 && g_savedPosY > -10000
                     && !(g_savedPosX == -1 && g_savedPosY == -1));
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
    // ★ Live2D 专用：把存档高度记下来，模型加载时以它为准。
    //   Live2DApplyWindowSize() 一律按 g_zoom 重算高度，
    //   读档得到的 WinH 当场被顶掉 —— 这是"Live2D 记不住窗口大小"的直接原因。
    g_restoreWinH = g_currentH;
    // 窗口高度才是缩放的唯一真身：用反推值覆盖 [State] Zoom，
    // 避免"高度是 1.4 倍、缩放却还是 1.0"这种两条路径尺度不一致。
    {
        float z = (float)g_currentH / (float)BASE_H;
        if (z < 0.5f) z = 0.5f;
        if (z > 3.0f) z = 3.0f;
        g_zoom = z;
    }
    g_currentBGMIndex = GetPrivateProfileIntW(L"BGM", L"Index", 0, iniPath);
    if (g_currentBGMIndex < 0 || g_currentBGMIndex >= (int)g_bgmFileList.size()) g_currentBGMIndex = 0;
    g_rvcOctave = GetPrivateProfileIntW(L"RVC", L"Octave", 0, iniPath);
    if (g_rvcOctave < -1) g_rvcOctave = -1;
    if (g_rvcOctave > 1) g_rvcOctave = 1;
    // ★ 语音朗读开关：llm.ini 的 [TTS] TtsEnable，默认开。
    //   以前这里读的是 config.ini，而菜单/设置面板写的是 llm.ini ——
    //   两个文件各存一份，重启后开关总是回到默认值。
    g_ttsVoiceOn.store(GetIniInt(L"TTS", L"TtsEnable", 1, 0, 1) != 0);
    // 半身状态不再存开关：由窗口最后停留的位置推导，WinMain 在尺寸定下来后刷新。
    g_halfBody = false;
    {
        char dbg[192];
        sprintf_s(dbg, sizeof dbg, "[State] costume=%d pos=%d,%d size=%dx%d zoom=%.2f\n",
                  (int)g_currentCostume, g_savedPosX, g_savedPosY, g_currentW, g_currentH, g_zoom);
        PetDbgA(dbg);
    }
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
// 统计数据的写入方不止主线程（RVC 后台线程也会结算好感并落盘）。
// 用同一把递归锁包住所有写入与存档，避免并发改写把 stats.ini 写成半截。
static std::recursive_mutex g_statMtx;

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

// 取 stats.ini 全路径：检查 GetExeDir 返回值，用安全版格式化
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
    std::lock_guard<std::recursive_mutex> lk(g_statMtx);
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
    std::lock_guard<std::recursive_mutex> lk(g_statMtx);
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
    std::lock_guard<std::recursive_mutex> lk(g_statMtx);
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
    std::lock_guard<std::recursive_mutex> lk(g_statMtx);
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

#if USE_LIVE2D
// 动作槽位是否已解锁：DEV 模式全开，正式版按累计好感门槛。
// 待机(0)/思考(1)是基础动作恒可用；开心(2)要攒够好感才进待机轮播池。
static bool L2dSlotUnlocked(int slot) {
    if (slot <= L2D_THINK) return true;
#if DEV_UNLOCK_ALL
    return true;
#else
    return g_stat.affinityTotal >= L2D_PEACE_NEED;
#endif
}

// AnimState -> 片段槽位。Run（拖拽）沿用思考槽：让拖动时人物也有反应，
// 松手回待机；不新增槽位是因为模型只有 3 个片段，硬映射会越界。
static int L2dSlotForState(AnimState s) {
    switch (s) {
    case AnimState::Think: return L2D_THINK;
    case AnimState::Peace: return L2D_PEACE;
    case AnimState::Run:   return L2D_THINK;
    default:               return L2D_IDLE;
    }
}

// 播放一个 AnimState 对应的片段，并按好感解锁状态兜底：
// 未解锁的"开心"不播，退回当前片段，避免低好感就看到解锁后的动作。
static void L2dPlayState(AnimState s) {
    int slot = L2dSlotForState(s);
    if (!L2dSlotUnlocked(slot)) return;
    L2dPlayClip(slot);
}
#endif

// 对话好感度：由模型返回的增减值驱动，是养成的决定性指标。
// 负分只压心情（影响爱心显示），不动 affinityTotal ——
// 累计值只增，解锁永不回退，爱心也不会被扣空。
// 返回 true 表示本轮确实"获得"了好感（用于区分"没加分"和"今日已达上限"）。
static bool StatsAddAffinityChat(int delta) {
    std::lock_guard<std::recursive_mutex> lk(g_statMtx);
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
// 主动搭话气泡"可点击展开对话"：g_toastClickable 为真时
//   气泡窗口去掉 WS_EX_TRANSPARENT（可点），点一下就打开聊天窗定位到该条。
static bool g_toastClickable = false;
// ★ 通用气泡内容：成就只是其中一种，养成解锁也复用同一个气泡窗口
static std::wstring g_toastIcon, g_toastTitle, g_toastDesc;
static int  g_toastAlpha = 0;
static int  g_toastPhase = 0;     // 0 淡入 1 停留 2 淡出
static int  g_toastTick = 0;
static int  g_toastQueue[ACH_COUNT];
static int  g_toastQHead = 0, g_toastQTail = 0;
static HDC     g_toastDC = nullptr;
static HBITMAP g_toastBmp = nullptr, g_toastOld = nullptr;

// ==================================================================
// 通用气泡队列：成就只是其中一类，错误/主动搭话（高优先级）
//   与养成解锁（低优先级）可排队逐条展示，不再互相盖掉。
//   每个气泡项可以是"直接文本"(icon/title/desc) 或"成就/解锁"(bit)。
//   高优先级插到队首，低优先级排队尾；ToastTick 空闲时按序出队。
// ==================================================================
struct ToastItem {
    int     bit = -1;          // >=0 表示成就/解锁项（走 ToastStart(bit)），-1 表示直接文本
    wchar_t icon[24]  = { 0 };
    wchar_t title[96] = { 0 };
    wchar_t desc[160] = { 0 };
    bool    clickable = false; // 可点击展开聊天窗（主动搭话气泡）
};
static std::deque<ToastItem> g_toastItems;   // 统一队列（高优先级 front）

// 前置声明（定义在下方 ToastStart / ToastShowDirect）
static void ToastStart(int bit);
static void ToastShowDirect(const wchar_t* icon, const wchar_t* title, const wchar_t* desc, bool clickable = false);

// 通用气泡入队：prio==0 高优先级（错误/主动搭话）插队首，1 低优先级（成就/解锁）排队尾
static void ToastEnqueueMsg(const wchar_t* icon, const wchar_t* title, const wchar_t* desc, int prio, bool clickable = false) {
    ToastItem m;
    m.bit = -1;
    m.clickable = clickable;
    wcsncpy_s(m.icon, icon ? icon : L"", _TRUNCATE);
    wcsncpy_s(m.title, title ? title : L"", _TRUNCATE);
    wcsncpy_s(m.desc, desc ? desc : L"", _TRUNCATE);
    if (prio == 0) g_toastItems.push_front(m);
    else           g_toastItems.push_back(m);
    if (g_toastItems.size() > 8) g_toastItems.pop_back();   // 限长，防堆积
}
// 成就/解锁入队（低优先级）：bit 形式，展示时由 ToastStart(bit) 填内容
static void ToastEnqueueBit(int bit, int prio) {
    ToastItem m; m.bit = bit;
    if (prio == 0) g_toastItems.push_front(m);
    else           g_toastItems.push_back(m);
    if (g_toastItems.size() > 8) g_toastItems.pop_back();
}

// 兼容旧调用：成就位入队（低优先级）
static void ToastEnqueue(int bit) {
    ToastEnqueueBit(bit, 1);
}

// 出队一项展示：bit>=0 走 ToastStart(bit)，否则直接文本（可带 U7 clickable）
static bool ToastItemPop() {
    if (g_toastItems.empty()) return false;
    ToastItem it = g_toastItems.front();
    g_toastItems.pop_front();
    if (it.bit >= 0) ToastStart(it.bit);
    else ToastShowDirect(it.icon, it.title, it.desc, it.clickable);
    return true;
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
    // ★安全：swprintf 溢出时返回 -1 且不保证以 NUL 结尾，后续 DrawString(-1) 会越界读。
    //   用 _TRUNCATE 版本，任何长度都保证终止。
    wchar_t line[80];
    _snwprintf_s(line, 80, _TRUNCATE, L"%s", tdesc.c_str());
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
    g_toastClickable = false;
}

// 直接显示一条通用气泡（立即建窗 + 绘制）。拆出来供队列出队时调用。
// clickable=true 时去掉 WS_EX_TRANSPARENT，气泡可点（点击展开聊天窗）。
static void ToastShowDirect(const wchar_t* icon, const wchar_t* title, const wchar_t* desc, bool clickable) {
    if (g_hToast) ToastDestroy();
    g_toastIcon  = icon  ? icon  : L"";
    g_toastTitle = title ? title : L"";
    g_toastDesc  = desc  ? desc  : L"";
    g_toastClickable = clickable;
    // g_toastBit 兼作"气泡是否在展示"的哨兵，必须 >= 0，否则 ToastTick 会直接返回；
    // 内容一律读 g_toastIcon/Title/Desc，不再碰 ACH_DEF。
    g_toastBit = 0;
    g_toastAlpha = 0; g_toastPhase = 0; g_toastTick = 0;
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    if (!clickable) exStyle |= WS_EX_TRANSPARENT;
    g_hToast = CreateWindowExW(exStyle,
        L"MakotoToast", L"", WS_POPUP, 0, 0, TOAST_W, TOAST_H, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    // 建窗失败要一并清掉可点标志，避免残留的 g_toastClickable
    //   让下一次普通气泡误触"点击展开"逻辑。
    if (!g_hToast) { g_toastBit = -1; g_toastClickable = false; return; }
    RECT rc; GetWinRect(g_hwnd, rc);
    int x = GetSystemMetrics(SM_CXSCREEN) - TOAST_W - 20;
    int y = rc.top - TOAST_H - 12;
    if (y < 10) y = rc.bottom + 12;
    SetWindowPos(g_hToast, HWND_TOPMOST, x, y, TOAST_W, TOAST_H, SWP_NOACTIVATE);
    ShowWindow(g_hToast, SW_SHOWNA);
    ToastPaint();
}

// 通用气泡：入队（默认低优先级），由 ToastTick 逐条展示，不再互相盖掉。
// 高优先级（错误/主动搭话）请直接调 ToastEnqueueMsg(..., prio=0)。
static void ToastShow(const wchar_t* icon, const wchar_t* title, const wchar_t* desc) {
    ToastEnqueueMsg(icon, title, desc, 1);
}
// 高优先级版：插队首。若此刻没有气泡在展示（g_toastBit<0）则立即弹，
// 否则排队首等当前气泡淡出后无缝衔接。
static void ToastShowHi(const wchar_t* icon, const wchar_t* title, const wchar_t* desc) {
    ToastEnqueueMsg(icon, title, desc, 0);
    if (g_toastBit < 0) ToastItemPop();   // 空闲则立即出队展示
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
// 气泡队列：空闲时按序出队（高优先级 front 先展示），逐条不再互相盖掉。
// 一条气泡的存在时长（毫秒）：淡入 8 帧 + 停留 3 秒 + 淡出 8 帧。
// 表情保持时长按它来算，气泡还没消失脸就先回中性会很出戏。
static int ToastVisibleMs() {
    return (8 + FPS * 3 + 8) * FRAME_INTERVAL_MS;
}

static void ToastTick() {
    if (g_toastBit < 0) {
        if (!g_toastItems.empty()) { ToastItemPop(); return; }
        // 兼容旧的成就位队列（保留路径）
        if (g_toastQHead < g_toastQTail) { ToastStart(g_toastQueue[g_toastQHead++]); return; }
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
// 前置声明（定义在后）：点击展开 -> 打开聊天窗
#define WM_LOCATE_PROACTIVE (WM_USER + 104)   // 点击主动搭话气泡：滚到该条历史
static void OpenChatDialog();
static HWND g_hChatWnd = nullptr;   // 定义上移（原在 LLM 对话区），供气泡点击定位使用

static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN) {
        // 点击"茉子·xxx"的主动搭话气泡 -> 展开聊天窗并滚到该条历史
        if (g_toastClickable) {
            g_toastClickable = false;
            ToastDestroy();
            OpenChatDialog();
            // OpenChatDialog 同步创建窗口并载入历史；随后异步定位，
            //   确保历史文本已就绪再滚动（PostMessage 排在当前消息之后）。
            if (g_hChatWnd && IsWindow(g_hChatWnd))
                PostMessageW(g_hChatWnd, WM_LOCATE_PROACTIVE, 0, 0);
            return 0;
        }
        return 0;
    }
    if (msg == WM_SETCURSOR) {
        if (g_toastClickable) SetCursor(LoadCursorW(nullptr, IDC_HAND));
    }
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

// ==================================================================
//  ★ 素材扫描：启动时（以及每次打开右键菜单时）各扫一次两套素材。
//  只做"存在性"判断，不解码、不加载 —— 16 次 FindFirstFile + 几个 PathFileExists，
//  放在菜单弹出前做也没有感知延迟。
// ==================================================================
static bool DetectFrameAssets() {
    for (int c = 0; c < 4; ++c)
        for (int s = 0; s < 4; ++s)
            if (!ScanAnimFiles((Costume)c, (AnimState)s).empty()) return true;
    return false;
}

static bool DetectLive2DAssets() {
#if USE_LIVE2D
    // 运行时 DLL 缺了，模型再多也跑不起来
    if (!PathFileExistsW(L"p2l_runtime.dll")) return false;
    for (int i = 0; i < 4; ++i) {
        std::string pn = kL2dModelName[i];
        // 新结构（8 表情独立 rig） / 旧结构（单文件，物理版优先）
        std::string pNew = "live2d/" + pn + "/base.p2lrt";
        std::string pPhy = "live2d/" + pn + "_physics.p2lrt";
        std::string pOld = "live2d/" + pn + ".p2lrt";
        if (PathFileExistsA(pNew.c_str()) || PathFileExistsA(pPhy.c_str())
            || PathFileExistsA(pOld.c_str())) return true;
    }
    // 兜底：旧版直接放 exe 同级的单模型
    return PathFileExistsA("mako.p2lrt") || PathFileExistsA("tml.p2lrt");
#else
    return false;
#endif
}

// 刷新两个标志位（工作目录已由 WinMain 设为 exe 所在目录）
static void DetectAssets() {
    g_hasFrameAssets  = DetectFrameAssets();
    g_hasLive2DAssets = DetectLive2DAssets();
    PetDbgA(g_hasFrameAssets ? "[Assets] frames ok\n" : "[Assets] no frames\n");
    PetDbgA(g_hasLive2DAssets ? "[Assets] live2d ok\n" : "[Assets] no live2d\n");
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
#if USE_LIVE2D
    // ★ 按服装粒度的自动升级 / 降级：换装时只要运行时 DLL 就绪，就先尝试为
    //   目标服装加载模型。条件用 g_l2dLibReady 而不是 Live2DReady()，这样
    //   从「无模型的序列帧服装」换到「有模型的服装」也能自动升级回 Live2D。
    //   ★ 但必须尊重用户选定的渲染方式：菜单里选了「序列帧」之后，
    //     换装不能再偷偷把模型加载回来（DLL 仍常驻，所以还要看 g_renderMode）。
    if (g_l2dLibReady && g_renderMode == RM_LIVE2D && costume != g_currentCostume) {
        if (Live2DOpenRig(costume, false, false)) {
            // 模型可用：Live2D 同步热切换（没有换图淡入淡出，避免 g_currentCostume
            // 卡住导致菜单勾选不变、存档不更新、好感度反复 +1）。
            g_currentCostume = costume;
            g_currentState = state;
            g_currentFrameIndex = 0;
            g_idleActionTimer = 0;
            g_isRandomAction = false;
            g_bTransitioning = false;
            g_pendingCostume = costume;
            g_renderPrewarmReq = true;
            PlayCostumeChangeSFX();
            StatsAddAffinity(1);
            SaveConfig();                     // 立刻记住，强杀进程也不丢
            return;
        }
        // 目标服装没有可用模型 → 降级为序列帧。必须先释放当前 rig，让
        // Live2DReady() 变 false，否则 WM_TIMER 里的 g_bTransitioning 交接
        // 分支会被挡住、画面卡在旧服装；同时丢弃残留的过渡态。
        if (g_l2dRig) Live2DReleaseRig();
        g_bTransitioning = false;
        // 若启动时因「当前服装有模型」跳过了序列帧预加载，这里补启动，
        // 否则降级后帧缓存为空会全黑。AnimLoadStart 幂等，重复调用无副作用。
        AnimLoadStart();
        // 继续落到下面，按普通序列帧换装流程切换到目标服装。
    }
#endif
    if (costume == g_currentCostume) {
        g_currentFrameIndex = 0;
        g_currentState = state;
        g_currentCostume = costume;
#if USE_LIVE2D
        // Live2D 下状态切换同时驱动片段播放；序列帧路径继续用帧缓存，两条路互不干扰。
        if (Live2DReady()) L2dPlayState(state);
#endif
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
//  素材运行期不变，每套动画只解码一次并常驻复用；
//  否则每次切换服装 / 开始拖拽都要重新扫描目录、解码 8~10 张 PNG，造成明显卡顿。
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
    // 16 套全部常驻是设计目标：等于上限时不要淘汰，否则任何一套重建都会
    // 把其余 14 套清掉，切换动作又回到边播边解码。
    while (g_cacheSetCount > FRAME_CACHE_MAX) {
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
static bool          g_bootDone    = false;   // 启动收尾（BGM/启动音）只做一次
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
    if (g_hLoadThread) return;
    g_loadQuit = false;      // ★ 从 Live2D 切回序列帧时复用同一入口，得先把停止标志清掉
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
//  GDI+ 缩放是纯 CPU 重采样，占绘制耗时的大头。
//  预先缩放到窗口尺寸并缓存，之后每帧只剩一次 1:1 拷贝。
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
            g_dibBits = nullptr;
        }
        HDC screenDC = GetDC(nullptr);
        if (!screenDC) return;
        g_memDC = CreateCompatibleDC(screenDC);
#if USE_LIVE2D
        // 32bpp top-down DIB：拿到可直写的像素指针，行序与 p2l_render 一致。
        // ULW_ALPHA 要求位图自带 alpha 通道，CreateCompatibleBitmap 做不到。
        if (g_memDC) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;          // 负高度 = top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            g_hMemBmp = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            g_dibBits = (uint8_t*)bits;
        }
#else
        g_hMemBmp = CreateCompatibleBitmap(screenDC, w, h);
#endif
        ReleaseDC(nullptr, screenDC);
        if (!g_memDC || !g_hMemBmp) return;
#if USE_LIVE2D
        if (!g_dibBits) return;
        g_l2dFrameDirty = true;      // 输出尺寸变了，缓存帧作废
        g_l2dRgba.clear();
#endif
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
#if USE_LIVE2D
    if (Live2DReady() && g_preloadDone) {
        // 直接写 DIB 像素，不经 GDI+ 缩放：网格是实时变形的，任意尺寸都不糊。
        Live2DBlit(w, h);
    } else
#endif
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
#if USE_LIVE2D
    if (Live2DReady()) {
        Live2DPremultiply(w, h);     // 直通 alpha -> 预乘，ULW_ALPHA 的硬性要求
        UpdateLayeredWindow(g_hwnd, nullptr, &ptWin, &size, g_memDC, &ptSrc, 0, &bf, ULW_ALPHA);
    } else
#endif
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

// 光标所在显示器的工作区底边（任务栏上沿）；多屏时跟着当前屏幕走。
static int MonitorWorkBottom(POINT pt) {
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (hm && GetMonitorInfoW(hm, &mi)) return mi.rcWork.bottom;
    return GetSystemMetrics(SM_CYSCREEN);
}

// 拖拽落点限制在光标所在显示器内：左右/顶部仍只留 20% 探出余量；
// 底部一路允许沉到「只露 HALF_BODY_VISIBLE 比例」——这就是纯拖拽的半身手势，
// 不需要任何按钮。往上拖则自然回到全身区域。
static void ClampToMonitor(POINT pt, int w, int h, int& outX, int& outY) {
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    int overW = (int)(w * 0.20f), overH = (int)(h * 0.20f);
    int overBottom = (int)(h * (1.0f - HALF_BODY_VISIBLE)) + 4;
    if (hm && GetMonitorInfoW(hm, &mi)) {
        int L = mi.rcWork.left - overW, T = mi.rcWork.top - overH;
        int R = mi.rcWork.right - w + overW, B = mi.rcWork.bottom - h + overBottom;
        if (R < L) R = L;
        if (B < T) B = T;
        if (outX < L) outX = L; if (outX > R) outX = R;
        if (outY < T) outY = T; if (outY > B) outY = B;
    } else {
        int sw = GetSystemMetrics(SM_CXSCREEN) - w + overW;
        int sh = GetSystemMetrics(SM_CYSCREEN) - h + overBottom;
        if (outX < -overW) outX = -overW; if (outX > sw) outX = sw;
        if (outY < -overH) outY = -overH; if (outY > sh) outY = sh;
    }
}

// 半身状态完全由窗口位置推导：底边探出工作区超过普通上限就算半身。
static bool IsHalfBodyPlacement(int top, int h, POINT anchor) {
    if (h <= 0) return false;
    return (top + h - MonitorWorkBottom(anchor)) > (int)(h * HALF_BODY_FULL_LIMIT);
}

static void RefreshHalfBodyFromPlacement(int top, int h, POINT anchor) {
    bool want = IsHalfBodyPlacement(top, h, anchor);
    if (want != g_halfBody) g_halfBody = want;   // 位置才是唯一真相，不写额外开关
}

static void RefreshHalfBodyFromWindow() {
    RECT rc{};
    if (!g_hwnd || !IsWindow(g_hwnd) || !GetWindowRect(g_hwnd, &rc)) return;
    int h = rc.bottom - rc.top;
    if (h <= 0) h = g_currentH;
    POINT anchor{ (rc.left + rc.right) / 2, rc.bottom };
    RefreshHalfBodyFromPlacement(rc.top, h, anchor);
}

// 松手时若已经越过半身阈值但还没沉到底，就一次性吸附到标准半身位置；
// 这样玩家只要"往下拖一把"，不必把鼠标精确拖到屏幕最底边。
static void SnapHalfBodyOnDragEnd(POINT cursor, int& outY) {
    int h = g_currentH;
    if (h <= 0) return;
    int workBottom = MonitorWorkBottom(cursor);
    int over = outY + h - workBottom;
    int fullLimit = (int)(h * HALF_BODY_FULL_LIMIT);
    int maxOver = (int)(h * (1.0f - HALF_BODY_VISIBLE)) + 4;
    if (over > fullLimit && over < maxOver) outY = workBottom + maxOver - h;
}

#if USE_LIVE2D
// 按当前服装的人物宽高比重算窗口尺寸：固定比例（2:3）会裁掉全身立绘，
// 约 1:3.5，等比缩进去后左右空出一大片透明矩形 —— 既挡桌面点击，又让
// ClampToMonitor 拿这个大矩形去限制拖动范围。这里让窗口贴着人物收紧。
static void Live2DApplyWindowSize() {
    if (!g_l2dRig || !g_hwnd) return;
    float aspect = (g_l2dContent.ok && g_l2dContent.h > 0.0f)
        ? g_l2dContent.w / g_l2dContent.h : (float)BASE_W / (float)BASE_H;
    if (aspect < 0.12f) aspect = 0.12f;
    if (aspect > 0.75f) aspect = 0.75f;
    // ★ 高度优先用存档里的 WinH（g_restoreWinH），只取一次；
    //   没有存档时才按 g_zoom 推算；一律用 g_zoom 会导致重开程序
    //   就把上次的窗口高度丢掉了。
    int winH = (g_restoreWinH > 0) ? g_restoreWinH : (int)(BASE_H * g_zoom);
    g_restoreWinH = 0;
    // FIT_W == FIT_H 时窗口宽高比就是人物宽高比；1.06 是左右各留 3% 的摆动余量。
    int winW = (int)(winH * aspect * (L2D_FIT_H / L2D_FIT_W) * 1.06f);
    if (winW < 64) winW = 64;
    if (winW > (int)(BASE_W * 3.0f)) winW = (int)(BASE_W * 3.0f);
    if (winH < 90) winH = 90;
    if (winH > (int)(BASE_H * 3.0f)) winH = (int)(BASE_H * 3.0f);
    // 把实际高度同步回缩放尺度，滚轮缩放才能从当前大小继续加减
    {
        float z = (float)winH / (float)BASE_H;
        if (z < 0.5f) z = 0.5f;
        if (z > 3.0f) z = 3.0f;
        g_zoom = z;
    }

    RECT rc;
    if (!GetWindowRect(g_hwnd, &rc)) return;
    int cx = (rc.left + rc.right) / 2;
    int bottom = rc.bottom;
    int nx = cx - winW / 2, ny = bottom - winH;   // 以中心 + 底边为锚
    POINT anchor = { cx, bottom };
    ClampToMonitor(anchor, winW, winH, nx, ny);
    SetWindowPos(g_hwnd, HWND_TOPMOST, nx, ny, winW, winH, SWP_NOACTIVATE);
    g_currentW = winW; g_currentH = winH;
    g_winX = nx; g_winY = ny; g_winPosKnown = true;
    g_l2dFrameDirty = true;    // 输出尺寸变了，缓存帧作废
    RefreshHalfBodyFromPlacement(ny, winH, anchor);   // 尺寸变了，按新位置重算半身状态
}

// ★ 模型加载完才知道窗口的真实尺寸，这里把存档里的坐标与尺寸精确贴一次。
//   否则 Live2DApplyWindowSize 的"底边锚定"重算会在高度变化时把窗口位置拽偏，
//   用户看到的就是"位置没记住"。
static void RestoreSavedGeometry() {
    if (!g_hwnd || !IsWindow(g_hwnd)) return;
    if (!g_hasSavedPos) return;
    int w = g_currentW, h = g_currentH;
    if (w < 64) w = 64;
    if (h < 90) h = 90;
    int nx = g_savedPosX, ny = g_savedPosY;
    POINT anchor = { nx + w / 2, ny + h / 2 };
    ClampToMonitor(anchor, w, h, nx, ny);   // 换过显示器 / 改过分辨率也不会跑到屏幕外
    SetWindowPos(g_hwnd, HWND_TOPMOST, nx, ny, w, h, SWP_NOACTIVATE);
    g_winX = nx; g_winY = ny; g_winPosKnown = true;
    g_l2dFrameDirty = true;
}
#endif

// ==================================================================
//  ★ 渲染方式切换（右键菜单：序列帧 / Live2D）
//  两条路各自的"进入 / 退出"动作都收口在这里，菜单里只负责发指令。
// ==================================================================
// 序列帧窗口是固定 2:3（BASE_W : BASE_H × zoom）；Live2D 会把它收窄到人物宽度。
// 所以从模型切回序列帧时必须把宽度还原，否则人物会被窗口左右裁掉一截。
static void ApplyFrameWindowSize() {
    int w = (int)(BASE_W * g_zoom), h = (int)(BASE_H * g_zoom);
    if (w < 64) w = 64;
    if (h < 90) h = 90;
    RECT rc{};
    if (g_hwnd && IsWindow(g_hwnd) && GetWindowRect(g_hwnd, &rc)) {
        int cx = (rc.left + rc.right) / 2, bottom = rc.bottom;
        int nx = cx - w / 2, ny = bottom - h;      // 与 Live2D 相同的"中心 + 底边"锚点
        POINT anchor = { cx, bottom };
        ClampToMonitor(anchor, w, h, nx, ny);
        SetWindowPos(g_hwnd, HWND_TOPMOST, nx, ny, w, h, SWP_NOACTIVATE);
        g_winX = nx; g_winY = ny; g_winPosKnown = true;
    }
    g_currentW = w; g_currentH = h;
    RenderCacheInvalidate();      // 输出尺寸变了，预缩放缓存全部作废
    MarkGeomDirty();
    RefreshHalfBodyFromWindow();   // 尺寸变了，按新位置重算半身状态
}

// 返回是否切换成功；失败时保持原来的渲染方式，界面和存档都不动。
static bool SetRenderMode(RenderMode m) {
    // "目标已达成"就不折腾。Live2D 还要看 rig 在不在：某套服装没有模型时会被
    // 自动降级成序列帧，此时 g_renderMode 仍是 Live2D，但画面是帧——必须允许重试。
    if (m == RM_FRAMES  && g_renderMode == RM_FRAMES  && !Live2DReady()) return true;
    if (m == RM_LIVE2D  && g_renderMode == RM_LIVE2D  &&  Live2DReady()) return true;

    if (m == RM_LIVE2D) {
#if USE_LIVE2D
        if (!g_hasLive2DAssets) {
            ToastShow(L"🖼", L"没有 Live2D 素材",
                L"把 p2l_runtime.dll 放到 exe 同级、模型放进 live2d\\ 文件夹后再试。");
            return false;
        }
        if (!g_l2dLibReady) {
            Live2DInit();                 // 第一次切到模型：装载运行时 DLL（内部也会试开当前服装）
            if (!g_l2dLibReady) {
                ToastShow(L"🖼", L"Live2D 运行时不可用",
                    L"p2l_runtime.dll 加载失败或版本过旧，详见 DebugView 里的 [MakotoPet]。");
                return false;
            }
        }
        if (!Live2DReady() && !Live2DOpenRig(g_currentCostume, false, true)) {
            ToastShow(L"🖼", L"这套服装没有可用模型",
                g_l2dLastError.empty() ? L"live2d\\ 下找不到对应模型的 .p2lrt。" : g_l2dLastError.c_str());
            return false;
        }
        g_bTransitioning = false;
        g_renderMode = RM_LIVE2D;
        AnimLoadStop();               // 停掉后台预解码；已解出的帧保留着，切回来秒开
        Live2DApplyWindowSize();      // 宽度收窄到人物比例
        L2dPlayState(g_currentState);
        g_preloadDone = true;
        SaveConfig();
        RequestPaint(true);
        return true;
#else
        ToastShow(L"🖼", L"这个版本没有编译 Live2D", L"请用 USE_LIVE2D=1 重新编译后再试。");
        return false;
#endif
    }

    // → 序列帧
    bool haveFrames = g_hasFrameAssets;
    if (!haveFrames) {
        // 素材目录被移走、但这一套已经解码在内存里：仍然允许切过去
        std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
        int ci = (int)g_currentCostume, si = (int)g_currentState;
        if (ci >= 0 && ci < 4 && si >= 0 && si < 4)
            haveFrames = g_frameCached[ci][si] && !g_frameCache[ci][si].empty();
    }
    if (!haveFrames) {
        ToastShow(L"🖼", L"没有序列帧素材",
            L"把 PNG 放进 Animations\\服装\\动作\\ 后再试（目录结构见 README）。");
        return false;
    }
#if USE_LIVE2D
    if (g_l2dRig) Live2DReleaseRig();   // 只放 rig，DLL 留着，切回去不用重新加载
#endif
    g_bTransitioning = false;
    g_renderMode = RM_FRAMES;
    ApplyFrameWindowSize();
    AnimLoadStart();                    // 幂等：线程还在就直接复用
    SwitchAnim(g_currentCostume, AnimState::Idle);   // 缓存里没有就当场解码这一套
    g_preloadDone = true;
    SaveConfig();
    RequestPaint(true);
    return true;
}

// ==================== LLM 对话系统 ====================
#define WM_APPEND_TEXT      (WM_USER + 100)
#define WM_APPEND_CHAR      (WM_USER + 102)
#define WM_STREAM_DONE      (WM_USER + 101)
#define WM_REMOVE_THINKING  (WM_USER + 103)
#define WM_APP_ASR_DONE     (WM_USER + 105)   // ★语音：whisper 转写完成，lParam = new std::wstring

static const int TYPEWRITER_DELAY_MS = 30;
static bool g_lastJsonTruncated = false;   // 本次回复是否被 max_tokens 截断
static HWND g_hHistoryEdit = nullptr;
static HWND g_hInputEdit = nullptr;
static HWND g_hSendBtn = nullptr;
static HWND g_hStatusLabel = nullptr;      // 底部状态栏（"茉子正在思考…"动态省略号）
static HWND g_hAttachStrip = nullptr;      // ★附件条子窗口（有附件才显示）
static bool g_isChatting = false;
static bool g_richEditLoaded = false;
static volatile bool g_typewriting = false; // 打字机进行中（点击/任意键跳过）
static std::atomic<bool> g_typewriterSkip{ false }; // 跳过信号：LLM 线程下次循环检查后立即灌全文
static int g_chatTheme = 0;                // ★主题缓存（0浅/1深）：上色函数直接读它，避免每段查 ini

// ★危险操作二次确认（清空对话 / 清空长期记忆）：
//   第一次点击只"上膛"并把按钮改成确认态，5 秒内再点一次才真正删除。
//   记忆、对话记录属于不可恢复数据，误点一下全丢对玩家来说代价太大。
static int       g_confirmAction = 0;      // 0=无 1=清空对话 2=清空记忆
static ULONGLONG g_confirmUntil  = 0;      // 上膛失效时刻（GetTickCount64 毫秒）
#define CONFIRM_WINDOW_MS 5000

// 聊天窗顶栏控件 ID
#define ID_BTN_CLEAR    105
#define ID_BTN_MEMORY   106
#define ID_BTN_LONGFORM 107
#define ID_BTN_THEME    108        // ★主题切换（浅/深）按钮
#define ID_BTN_ATTACH   109        // ★附件：📎 选文件
#define ID_BTN_MIC      110        // ★附件：🎤 语音输入
#define ID_ATTACH_STRIP 4001       // ★附件条子窗口
#define ATTACH_STRIP_H  56
#define ID_TIMER_STATUS 901        // 状态栏动画定时器

// 前置声明（定义在 ChatWndProc 之后）
static void SubmitChat(HWND hwnd);
static void ClearChatHistory();
static void ClearMemoryFile();     // ★危险操作：清空 memory.txt（调用前必须二次确认）
static void OpenMemoryFile();
static void ArmDangerousAction(HWND hwnd, int action);   // ★上膛：按钮切到确认态
static void SetLLMConfigInt(const wchar_t* key, int val, int lo, int hi);
static void SetUIConfigInt(const wchar_t* key, int val, int lo, int hi);   // ★主题/打字机开关写回 [UI]
static void SetButtonText(HWND hwnd, int id, const wchar_t* text);
static void ShowTypewriterSkipHint();
static void OpenSettingsDialog();   // ★设置面板：覆盖所有玩家可配参数
static void ToggleVoiceRecord(HWND hwnd);   // ★附件/语音：🎤 开始/结束录音（实现在附件对话区）
static void VoiceAbortRecording();          // 关窗/退出时强制停止录音

#include <string>
std::wstring g_ttsRefAudioPath; // 全局变量，用来存运行时的绝对路径
// ★ 参考文本原值里用了全角逗号「，」，TTS 断句会怪，现在统一用顿号。
//   1) "よろし" 少了 "く"，正确是 "よろしく"；
//   2) 全角逗号 "，" 在日语里应写作 "、"。
//   参考文本与实际参考音频内容不一致会明显拖累音色克隆效果。
//   如果你的参考音频说的不是这句，请在 llm.ini 的 [TTS] TtsRefText 里改成完全一致的内容。
static const wchar_t* TTS_REF_TEXT = L"常陸茉子です、よろしくお願いします";

// ==================================================================
//  llm.ini 读写引擎：UTF-8 + 内存缓存
// ------------------------------------------------------------------
//  为什么不再用 Get/WritePrivateProfileStringW：
//   1) 它们按系统 ANSI 码页解析文件。llm.ini 是 UTF-8（中文注释 + 中日文值），
//      系统 API 读到的是乱码；写回时还会把整份文件按 ANSI 重新编码，把中文值改坏
//      （TtsRefText 就是这样变成一长串替换字符的）。
//   2) 每次读取都要重新解析整个文件，而一轮对话会读几十项配置。
//   3) 单值上限 32KB，TtsExtraJson 这类长值有被静默截断的风险。
//  这里的做法：整文件按 UTF-8 解码（识别 BOM；个别非法行按系统码页兜底，
//  能救回旧版写进去的 GBK 值）→ 解析成 (段, 键) 索引 → 缓存，
//  只有文件时间戳/大小变了才重新解析；写回按行精确替换，
//  注释、顺序、行尾风格原样保留，并先写临时文件再原子替换，断电不会写坏原文件。
// ==================================================================
struct IniFileCache {
    std::wstring path;
    ULONGLONG    stamp = 0;      // 最后写入时间（FILETIME 合并成 64 位）
    ULONGLONG    size = 0;
    bool         valid = false;
    bool         hasBom = false;
    bool         crlf = true;
    std::wstring text;                                  // 全文（UTF-16）
    std::map<std::wstring, std::wstring> kv;            // "SECTION\x1fKEY" -> 值
};
static std::mutex g_iniMtx;                             // LLM 线程也会读配置
static std::vector<IniFileCache> g_iniCache;

static std::wstring IniUpper(std::wstring s) {
    for (wchar_t& c : s) if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);   // 键名只有 ASCII
    return s;
}
static std::wstring IniTrim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) --b;
    return s.substr(a, b - a);
}

static bool IniReadRaw(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > 8ll * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() ||
        (ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == (DWORD)out.size());
    CloseHandle(h);
    return ok;
}

static bool IniUtf8ToWide(const char* p, int n, std::wstring& out) {
    out.clear();
    if (n <= 0) return true;
    int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, nullptr, 0);
    if (need <= 0) return false;
    out.resize((size_t)need);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, n, &out[0], need) == need;
}

// 整份按 UTF-8 解；失败说明混进了非 UTF-8 行（旧版写下的 GBK 值），逐行兜底。
static std::wstring IniDecode(const std::string& raw, bool& hasBom) {
    hasBom = false;
    const char* p = raw.data();
    size_t n = raw.size();
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        hasBom = true;
        p += 3;
        n -= 3;
    }
    std::wstring all;
    if (IniUtf8ToWide(p, (int)n, all)) return all;
    std::wstring out;
    size_t i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && p[j] != '\n') ++j;
        std::wstring line;
        if (!IniUtf8ToWide(p + i, (int)(j - i), line)) {
            int need = MultiByteToWideChar(CP_ACP, 0, p + i, (int)(j - i), nullptr, 0);
            if (need > 0) {
                line.assign((size_t)need, L'\0');
                MultiByteToWideChar(CP_ACP, 0, p + i, (int)(j - i), &line[0], need);
            }
        }
        out += line;
        if (j >= n) break;
        out += L'\n';
        i = j + 1;
    }
    return out;
}

static std::string IniEncode(const std::wstring& text, bool withBom) {
    std::string out;
    if (!text.empty()) {
        int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            out.resize((size_t)need);
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], need, nullptr, nullptr);
        }
    }
    if (withBom) out.insert(0, "\xEF\xBB\xBF", 3);   // UTF-8 BOM：原文件有就保留
    return out;
}

static void IniParse(IniFileCache& c) {
    c.kv.clear();
    std::wstring sec;
    size_t i = 0;
    while (i < c.text.size()) {
        size_t j = c.text.find(L'\n', i);
        if (j == std::wstring::npos) j = c.text.size();
        std::wstring line = c.text.substr(i, j - i);
        i = (j < c.text.size()) ? j + 1 : c.text.size();
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = IniTrim(line);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;   // 注释整行才算，值里的分号不动
        if (t[0] == L'[') {
            size_t e = t.find(L']');
            if (e != std::wstring::npos) sec = IniUpper(IniTrim(t.substr(1, e - 1)));
            continue;
        }
        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = IniUpper(IniTrim(t.substr(0, eq)));
        std::wstring val = IniTrim(t.substr(eq + 1));
        if (val.size() >= 2 && val.front() == L'"' && val.back() == L'"')
            val = val.substr(1, val.size() - 2);                 // 和系统 API 一致：去掉成对引号
        std::wstring full = sec + L'\x1f' + key;
        if (c.kv.find(full) == c.kv.end()) c.kv[full] = val;      // 首次出现优先
    }
}

// 取（必要时重新解析）缓存项，调用方必须持有 g_iniMtx
static IniFileCache* IniAcquire(const std::wstring& path) {
    ULONGLONG stamp = 0, size = 0;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        stamp = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
        size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    IniFileCache* c = nullptr;
    for (auto& it : g_iniCache)
        if (_wcsicmp(it.path.c_str(), path.c_str()) == 0) { c = &it; break; }
    if (!c) { g_iniCache.push_back(IniFileCache()); c = &g_iniCache.back(); c->path = path; }
    if (c->valid && c->stamp == stamp && c->size == size) return c;    // 文件没动，命中缓存
    c->valid = true;
    c->stamp = stamp;
    c->size = size;
    std::string raw;
    if (!IniReadRaw(path, raw)) {                                     // 文件不存在/读不到
        c->text.clear();
        c->kv.clear();
        c->hasBom = false;
        return c;
    }
    c->text = IniDecode(raw, c->hasBom);
    c->crlf = (c->text.find(L"\r\n") != std::wstring::npos);
    IniParse(*c);
    return c;
}

static bool IniGetValue(const std::wstring& path, const wchar_t* sec, const wchar_t* key, std::wstring& out) {
    if (!sec || !key) return false;
    std::lock_guard<std::mutex> lk(g_iniMtx);
    IniFileCache* c = IniAcquire(path);
    auto it = c->kv.find(IniUpper(sec) + L'\x1f' + IniUpper(key));
    if (it == c->kv.end()) return false;
    out = it->second;
    return true;
}

static bool IniWriteRaw(const std::wstring& path, const std::string& bytes) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = bytes.empty() ||
        (WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == (DWORD)bytes.size());
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

static bool IniSetValue(const std::wstring& path, const wchar_t* secW, const wchar_t* keyW, const std::wstring& val) {
    if (!secW || !keyW) return false;
    const std::wstring sec = IniUpper(IniTrim(secW));
    const std::wstring key = IniUpper(IniTrim(keyW));
    std::lock_guard<std::mutex> lk(g_iniMtx);
    IniFileCache* c = IniAcquire(path);

    const bool tailNl = !c->text.empty() && c->text.back() == L'\n';
    std::vector<std::wstring> lines;
    for (size_t i = 0; i < c->text.size();) {
        size_t j = c->text.find(L'\n', i);
        if (j == std::wstring::npos) { lines.push_back(c->text.substr(i)); break; }
        std::wstring line = c->text.substr(i, j - i);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        i = j + 1;
    }

    int secLine = -1, secEnd = (int)lines.size();
    for (int i = 0; i < (int)lines.size(); ++i) {
        std::wstring t = IniTrim(lines[i]);
        if (t.size() < 2 || t[0] != L'[') continue;
        size_t e = t.find(L']');
        if (e == std::wstring::npos) continue;
        if (secLine < 0) {
            if (IniUpper(IniTrim(t.substr(1, e - 1))) == sec) secLine = i;
        } else {
            secEnd = i;                                  // 下一个段头 = 本段结束
            break;
        }
    }

    std::wstring keyOut = keyW;                          // 新插入时沿用调用方写的键名
    int keyLine = -1;
    for (int i = secLine + 1; i < secEnd && secLine >= 0; ++i) {
        std::wstring t = IniTrim(lines[i]);
        if (t.empty() || t[0] == L';' || t[0] == L'#') continue;
        size_t eq = t.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (IniUpper(IniTrim(t.substr(0, eq))) == key) { keyLine = i; break; }
    }
    if (keyLine >= 0) {
        // 保留文件里原有的键名大小写，避免每次保存都把键名改成大写
        std::wstring raw = lines[keyLine];
        size_t eq = raw.find(L'=');
        keyOut = IniTrim(raw.substr(0, eq));
        lines[keyLine] = keyOut + L"=" + val;
    } else if (secLine >= 0) {
        lines.insert(lines.begin() + secEnd, keyOut + L"=" + val);
    } else {
        if (!lines.empty() && !IniTrim(lines.back()).empty()) lines.push_back(L"");
        lines.push_back(L"[" + std::wstring(secW) + L"]");
        lines.push_back(keyOut + L"=" + val);
    }

    std::wstring text;
    for (size_t i = 0; i < lines.size(); ++i) {
        text += lines[i];
        if (i + 1 < lines.size() || tailNl) text += c->crlf ? L"\r\n" : L"\n";
    }
    if (!IniWriteRaw(path, IniEncode(text, c->hasBom))) return false;

    // 写成功：同步刷新缓存（时间戳重新读，避免和外部编辑器的写入打架）
    c->text = text;
    IniParse(*c);
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        c->stamp = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
        c->size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    }
    c->valid = true;
    return true;
}

// ==================== TTS 配置（[TTS] 段） ====================
// 端口/路径随 GPT-SoVITS 版本而变，写死容易"换个整合包就哑了"，全部可配。
// 读配置统一走 IniGetValue：UTF-8 解析 + 缓存，不再受系统 ANSI 码页影响。
static std::wstring ReadIniStringDyn(const wchar_t* sec, const wchar_t* key,
                                     const wchar_t* defVal, const std::wstring& iniPath) {
    std::wstring v;
    // 空值同样回落默认值：和老实现（GetPrivateProfileString 返回 0 字符即视为未配置）
    // 保持一致，避免 TtsRefText 被写成空行后 TTS 直接哑掉。
    if (IniGetValue(iniPath, sec, key, v) && !v.empty()) return v;
    return defVal ? defVal : L"";
}
static std::wstring GetIniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* defVal) {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return defVal;
    std::wstring iniPath = exeDir + L"\\llm.ini";
    return ReadIniStringDyn(sec, key, defVal, iniPath);
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

// 取 JSON 里的浮点（如 intensity 0..1）。支持 0.8 / "0.8" / 1，容错同 ExtractJsonInt。
static float ExtractJsonFloat(const std::string& json, const std::string& key, float defVal, float lo, float hi) {
    std::string searchKey = "\"" + key + "\":";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return defVal;
    pos += searchKey.length();
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    bool quoted = (pos < json.length() && json[pos] == '"');
    if (quoted) pos++;
    bool neg = false;
    if (pos < json.length() && (json[pos] == '-' || json[pos] == '+')) { neg = (json[pos] == '-'); pos++; }
    if (pos >= json.length() || json[pos] < '0' || json[pos] > '9') return defVal;   // 只接受以数字开头
    double v = 0.0;
    while (pos < json.length() && json[pos] >= '0' && json[pos] <= '9') { v = v * 10 + (json[pos] - '0'); pos++; }
    if (pos < json.length() && json[pos] == '.') {
        pos++; double frac = 0.1;
        while (pos < json.length() && json[pos] >= '0' && json[pos] <= '9') { v += (json[pos] - '0') * frac; frac *= 0.1; pos++; }
    }
    if (neg) v = -v;
    float out = (float)v;
    if (out < lo) out = lo;
    if (out > hi) out = hi;
    return out;
}

// ==================================================================
// JSON 字段归一化：模型偶尔把字段名写成中文/大小写不一致/别名
// （zh / 中文 / reply / content / text …），逐个 try 容易漏，故统一归一化。
// 这里做"一次归一化"：把 JSON 对象的 key 统一映射到约定字段再取。
// ==================================================================
// 带别名的字符串提取：按 aliases 顺序找第一个非空值。
static std::string ExtractJsonStringAny(const std::string& json, const std::vector<std::string>& aliases) {
    for (const auto& a : aliases) {
        std::string v = ExtractJsonString(json, a);
        if (!v.empty()) return v;
    }
    return "";
}
// 好感度等"数字字段"走整数提取，不用 ExtractJsonStringAny。
//   ExtractJsonString 只认带引号的字符串，模型正常输出 "affection": 2（数字字面量）时
//   会取到空串，导致 affDelta 永远停在默认 1、评分形同虚设。
//   这里按别名表逐个尝试 ExtractJsonInt（它兼容 2 / "2" / 2.0 / 负数），
//   用 found 标记区分"字段给了 0"和"字段缺失"两种情况。
static int ExtractJsonIntAny(const std::string& json, const std::vector<std::string>& aliases,
                             int defVal, int lo, int hi, bool* found = nullptr) {
    if (found) *found = false;
    for (const auto& a : aliases) {
        // 先看字段在不在（带引号的 key 后面跟冒号），在才调用，避免把缺失字段误判成 0。
        if (json.find("\"" + a + "\":") == std::string::npos &&
            json.find("\"" + a + "\" :") == std::string::npos) continue;
        int v = ExtractJsonInt(json, a, defVal, lo, hi);
        if (found) *found = true;
        return v;
    }
    return defVal;
}
// 字段归一化映射表（一次查全，避免散落各处）
static std::vector<std::string> AliasZh() {
    return { "zh", "中文", "cn", "chinese", "reply", "content", "text",
             "回答", "回复", "中文回复" };
}
static std::vector<std::string> AliasJa() {
    return { "ja", "日文", "日语", "jp", "japanese", "日语回复" };
}
static std::vector<std::string> AliasEmotion() {
    return { "emotion", "情绪", "心情", "mood" };
}
static std::vector<std::string> AliasAffection() {
    return { "affection", "好感", "好感度" };
}
static std::vector<std::string> AliasMemory() {
    return { "memory", "记忆" };
}

// 把 LLM 的 action 字符串归类到动作槽：返回 -1 表示"没给/不覆盖"。
// 只认约定词，避免模型自由发挥导致越界；与 L2dSlotForEmotion 的槽位定义一致。
// 放在 #if 外，序列帧模式也能编译通过（不调用即可）。
static int ParseDirectorAction(const std::string& raw) {
    std::string s;
    for (char c : raw) s += (char)tolower((unsigned char)c);
    if (s.find("peace") != std::string::npos || s.find("happy") != std::string::npos ||
        s.find("laugh") != std::string::npos || s.find("开心") != std::string::npos ||
        s.find("笑") != std::string::npos) return 2;    // 开心摇摆
    if (s.find("think") != std::string::npos || s.find("nod") != std::string::npos ||
        s.find("思考") != std::string::npos || s.find("点头") != std::string::npos) return 1;  // 思考/点头
    if (s.find("idle") != std::string::npos || s.find("stand") != std::string::npos ||
        s.find("待机") != std::string::npos) return 0;   // 待机
    return -1;
}

// 视线字符串 -> L2dGazeMode；返回 -1 表示不覆盖。
static int ParseDirectorGaze(const std::string& raw) {
    std::string s;
    for (char c : raw) s += (char)tolower((unsigned char)c);
    if (s.find("away") != std::string::npos || s.find("aside") != std::string::npos ||
        s.find("移开") != std::string::npos || s.find("别处") != std::string::npos ||
        s.find("傲娇") != std::string::npos) return L2D_GAZE_AWAY;
    if (s.find("down") != std::string::npos || s.find("shy") != std::string::npos ||
        s.find("低头") != std::string::npos || s.find("害羞") != std::string::npos) return L2D_GAZE_DOWN;
    if (s.find("user") != std::string::npos || s.find("owner") != std::string::npos ||
        s.find("主人") != std::string::npos || s.find("看我") != std::string::npos ||
        s.find("straight") != std::string::npos) return L2D_GAZE_AT_USER;
    return -1;
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
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json; charset=utf-8", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Accept: audio/wav,*/*", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
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
    // ★安全：TTS 音频设 64MB 上限 + 4MB 分块，防止服务端异常时无限增长
    const size_t kMaxTtsBytes = 64u * 1024u * 1024u;
    std::vector<char> data;
    DWORD dwSize = 0, dwDownloaded = 0;
    for (;;) {
        dwSize = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
        if (data.size() >= kMaxTtsBytes) break;
        if (dwSize > 4u * 1024u * 1024u) dwSize = 4u * 1024u * 1024u;
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

// ★ 前置声明：TtsSynth 等函数要先用到这几个配置读取函数，
//   但它们的定义在下面（GetLLMConfig 在 3722 行附近）。
// 前置声明保证后续函数可见。
static std::wstring GetLLMConfig(const wchar_t* key);
static double GetLLMConfigDouble(const wchar_t* key, double defVal, double lo, double hi);
static int GetLLMConfigInt(const wchar_t* key, int defVal, int lo, int hi);

// ★ 只负责"合成"：返回一整个 WAV 的字节，不碰播放。
//   拆出来是为了分段流式——先合第一句就能开口，而不是等整段 600 字都合成完。
// emo: 情绪语气。默认 Neutral 时对 ini 基准值无影响，旧调用点行为不变。
static bool TtsSynth(const std::wstring& jaText, std::vector<char>* outWav,
                     std::wstring* errOut, PetEmotion emo = PetEmotion::Base) {
    if (errOut) errOut->clear();
    if (outWav) outWav->clear();
    if (jaText.empty()) return false;

    // 情绪只微调语速/语调，乘在 ini 基准上；用户可在 [LLM] EmotionTone=0 关闭。
    EmotionTone tone = ToneForEmotion(emo);
    bool useTone = GetLLMConfigInt(L"EmotionTone", 1, 0, 1) != 0;
    double speedScale = useTone ? tone.speed : 1.0;
    double tempScale  = useTone ? tone.temperature : 1.0;

    std::wstring host      = GetIniStr(L"TTS", L"TtsHost", L"127.0.0.1");
    int          port      = GetIniInt(L"TTS", L"TtsPort", 9880, 1, 65535);
    int          timeoutMs = GetIniInt(L"TTS", L"TtsTimeoutMs", 45000, 1000, 600000);
    std::wstring cfgPath   = GetIniStr(L"TTS", L"TtsPath", L"");
    std::wstring refAudio  = GetIniStr(L"TTS", L"TtsRefAudio", L"");
    if (refAudio.empty()) refAudio = g_ttsRefAudioPath;
    std::wstring refText   = GetIniStr(L"TTS", L"TtsRefText", TTS_REF_TEXT);
    // 旧版 ini 是 UTF-8/GBK 混排，某些编辑器重存后会把日文变成 U+FFFD（"锟斤拷"）。
    // 这种文本送进 GPT-SoVITS 只会得到错误音色，直接回退到内置参考文本。
    if (refText.find(L'\uFFFD') != std::wstring::npos
        || refText.find(L"锟斤拷") != std::wstring::npos) {
        TtsLog("warn", "TtsRefText 编码损坏，已回退内置参考文本");
        refText = TTS_REF_TEXT;
    }
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
    //   不发送 "split_text"：它不是 api_v2.py 的字段。
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
        "\"temperature\": " + numStr(min(2.0, max(0.0, GetIniDbl(L"TTS", L"TtsTemperature", 1.0, 0.0, 2.0) * tempScale))) + ","
        "\"text_split_method\": \"" + splitMethod + "\","
        "\"batch_size\": 1,"
        "\"batch_threshold\": 0.75,"
        "\"split_bucket\": true,"
        "\"speed_factor\": " + numStr(min(2.0, max(0.5, GetIniDbl(L"TTS", L"TtsSpeed", 1.0, 0.5, 2.0) * speedScale))) + ","
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
    static std::recursive_mutex    s_probeMtx;   // 探测缓存可能被两个线程同时访问
    std::wstring probeKey = host + L":" + std::to_wstring(port);

    std::vector<std::wstring> paths;
    auto pushPath = [&](const std::wstring& pp) {
        if (pp.empty()) return;
        if (std::find(paths.begin(), paths.end(), pp) == paths.end()) paths.push_back(pp);
    };
    if (!cfgPath.empty()) {
        pushPath(cfgPath);
    } else {
        {
            std::lock_guard<std::recursive_mutex> lk(s_probeMtx);
            if (probeKey != s_probeKey) {
                s_probePaths = TtsDiscoverPaths(host, port);
                s_probeKey = probeKey;
            }
            for (const auto& pp : s_probePaths) pushPath(pp);
        }
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

    TtsLog("ok", "path=" + WideStringToUtf8(usedPath) + " bytes=" + std::to_string(wav.size()));
    if (outWav) outWav->swap(wav);
    return true;
}


// ==================================================================
//  ★ 分段流式语音：边合成边播，而不是"整段合完才出声"
//
//  整段（最长 600 字）一次合成要 8~20 秒，期间模型无声、嘴也不动，
//  体感就是"说完台词半天才响"，因此改为边合成边播。
//
//  现在的链路：
//     日语台词 → 按标点切句 → 后台线程逐句合成 → 主线程逐句播放
//  第一句合完（通常 1~2 秒）就开口，后面的边播边合。
//  语音朗读与文字显示同步进行。
// ==================================================================

struct TtsJob {
    std::wstring      text;
    std::vector<char> wav;
    int               state = 0;    // 0=待合成  1=合成中  2=就绪  -1=失败
};

static std::vector<TtsJob>     g_ttsJobs;
static size_t                  g_ttsPlayIdx = 0;
static std::recursive_mutex    g_ttsJobMtx;
// ★ 专门保护 g_tts 这个 XSound：主线程在换段时会 Reset（销毁 voice），
//   LLM 线程打断时会 Stop。两边不加锁会撞在同一个 voice 上，偶尔崩在
//   CreateSourceVoice / DestroyVoice 里。加锁顺序统一为先 job 后 voice。
static std::recursive_mutex    g_ttsVoiceMtx;
static std::atomic<uint32_t>   g_ttsGen{ 0 };      // 批次号：换话题后丢弃旧批次的结果
static std::atomic<bool>       g_ttsAbort{ false }; // 本轮语音已作废
static std::atomic<bool>       g_ttsQuit{ false };  // 进程退出，工作线程收尾
static std::atomic<int>        g_ttsFailCount{ 0 };
static bool                    g_ttsErrNotified = false;   // 每轮只提示一次，不刷屏
static std::wstring            g_ttsLastErr;
static PetEmotion              g_ttsEmo = PetEmotion::Base;
static HANDLE                  g_hTtsEvent = nullptr;   // 有新任务时 Set
static HANDLE                  g_hTtsWorker = nullptr;

// 去掉不该念出来的东西：动作描写、markdown 符号、括号注释。
// 不处理会听到"抬手摸了摸你的头发"这种旁白被朗读，非常出戏。
static std::wstring TtsStripMarks(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    int depth = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'（' || c == L'(') { ++depth; continue; }
        if (c == L'）' || c == L')') { if (depth > 0) --depth; continue; }
        if (depth > 0) continue;
        // 括号里的动作描写已剔除；剩下的修饰符直接丢
        if (c == L'*' || c == L'_' || c == L'`' || c == L'#' || c == L'~') continue;
        if (c == L'「' || c == L'」' || c == L'『' || c == L'』') continue;
        out += c;
    }
    // 收敛连续空白：GPT-SoVITS 对多余空格会当成停顿，容易拖出怪节奏
    std::wstring compact;
    compact.reserve(out.size());
    bool lastSpace = false;
    for (wchar_t c : out) {
        bool sp = (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n');
        if (sp) { if (!lastSpace) compact += L' '; lastSpace = true; }
        else { compact += c; lastSpace = false; }
    }
    size_t b = compact.find_first_not_of(L" ");
    if (b == std::wstring::npos) return L"";
    size_t e = compact.find_last_not_of(L" ");
    return compact.substr(b, e - b + 1);
}

// 按标点切句。硬标点必断，软标点在够长时才断，兜底按长度硬切。
// 切得太碎会让 TTS 每句都重新起调，听起来一顿一顿；太长又回到"等半天"。
static std::vector<std::wstring> TtsSplitText(const std::wstring& in, int maxLen) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&]() {
        // 至少要有实际内容，纯标点段会让服务端报错或产生怪音
        bool hasWord = false;
        for (wchar_t c : cur) {
            if (iswalnum(c) || (c >= 0x3040 && c != L'　')) { hasWord = true; break; }
        }
        if (hasWord) {
            size_t b = cur.find_first_not_of(L" ");
            if (b != std::wstring::npos) out.push_back(cur.substr(b));
        }
        cur.clear();
    };
    for (wchar_t c : in) {
        cur += c;
        bool hard = (c == L'。' || c == L'！' || c == L'？' || c == L'!' || c == L'?'
                  || c == L'\n' || c == L'；' || c == L';');
        bool soft = (c == L'、' || c == L'，' || c == L',' || c == L'…' || c == L'　');
        if (hard) { flush(); }
        else if (soft && (int)cur.size() * 10 >= maxLen * 6) { flush(); }
        else if ((int)cur.size() >= maxLen) { flush(); }
    }
    flush();
    return out;
}

// 后台合成线程：常驻，用事件唤醒，不反复创建销毁。
static unsigned __stdcall TtsWorkerProc(void*) {
    for (;;) {
        WaitForSingleObject(g_hTtsEvent, 300);
        if (g_ttsQuit.load()) break;
        for (;;) {
            if (g_ttsAbort.load() || g_ttsQuit.load()) break;
            int idx = -1;
            uint32_t gen = 0;
            std::wstring text;
            {
                std::lock_guard<std::recursive_mutex> lk(g_ttsJobMtx);
                if (g_ttsAbort.load()) break;
                for (size_t i = 0; i < g_ttsJobs.size(); ++i) {
                    if (g_ttsJobs[i].state == 0) { g_ttsJobs[i].state = 1; idx = (int)i; break; }
                }
                if (idx < 0) break;                       // 本批次已全部投递
                text = g_ttsJobs[idx].text;
                gen  = g_ttsGen.load();
            }
            std::vector<char> wav;
            std::wstring err;
            bool ok = TtsSynth(text, &wav, &err, g_ttsEmo);
            if (g_ttsQuit.load()) break;
            // ★ 合成期间用户可能已经发了新消息：批次号变了就整段丢弃，
            //   否则会把上一轮的语音插进新对话里，听起来像抢话。
            if (g_ttsAbort.load() || gen != g_ttsGen.load()) break;
            {
                std::lock_guard<std::recursive_mutex> lk(g_ttsJobMtx);
                if (gen != g_ttsGen.load()) break;
                if ((size_t)idx < g_ttsJobs.size()) {
                    g_ttsJobs[idx].state = ok ? 2 : -1;
                    if (ok) g_ttsJobs[idx].wav.swap(wav);
                }
            }
            if (!ok) {
                g_ttsFailCount.fetch_add(1);
                if (!err.empty()) g_ttsLastErr = err;
            }
        }
    }
    return 0;
}

static void TtsWorkerEnsure() {
    if (g_hTtsWorker) return;
    if (!g_hTtsEvent) g_hTtsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_hTtsEvent) return;
    unsigned tid = 0;
    g_hTtsWorker = (HANDLE)_beginthreadex(nullptr, 0, TtsWorkerProc, nullptr, 0, &tid);
}

// 立刻停止当前语音并作废本批次（发新消息、切服装、退出时调用）。
static void TtsStopAll() {
    g_ttsAbort.store(true);
    g_ttsGen.fetch_add(1);
    {
        std::lock_guard<std::recursive_mutex> lk(g_ttsJobMtx);
        g_ttsJobs.clear();
        g_ttsPlayIdx = 0;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(g_ttsVoiceMtx);
        if (g_tts.v) XSoundStop(g_tts);
        g_tts.playing = false;
    }
}

// 朗读一段日语台词：切句 → 投递 → 后台合成。已有语音会被打断（不叠音）。
static void TtsSpeak(const std::wstring& jpText, PetEmotion emo) {
    TtsStopAll();
    if (!g_ttsVoiceOn.load()) return;
    std::wstring clean = TtsStripMarks(jpText);
    if (clean.empty()) return;

    int maxLen = GetIniInt(L"TTS", L"TtsSegMaxChars", 60, 12, 300);
    std::vector<std::wstring> segs = TtsSplitText(clean, maxLen);
    if (segs.empty()) return;

    g_ttsEmo = emo;
    g_ttsFailCount.store(0);
    g_ttsErrNotified = false;
    g_ttsLastErr.clear();
    {
        std::lock_guard<std::recursive_mutex> lk(g_ttsJobMtx);
        g_ttsJobs.clear();
        for (auto& t : segs) {
            TtsJob j;
            j.text = std::move(t);
            g_ttsJobs.push_back(std::move(j));
        }
        g_ttsPlayIdx = 0;
    }
    g_ttsAbort.store(false);
    g_ttsGen.fetch_add(1);          // 新批次：让上一批还在飞的合成结果作废
    TtsWorkerEnsure();
    if (g_hTtsEvent) SetEvent(g_hTtsEvent);
    TtsLog("speak", "segs=" + std::to_string(segs.size()));
}

// 主循环每帧调用：上一句放完就接下一句。
static void TtsPump() {
    if (g_ttsAbort.load()) return;
    if (g_tts.playing) return;                 // 还在响，别打断
    std::vector<char> wav;
    bool allDone = false;
    {
        std::lock_guard<std::recursive_mutex> lk(g_ttsJobMtx);
        if (g_ttsJobs.empty()) return;
        // 跳掉合成失败的句子：宁可少一句，也不要卡在这儿让整段语音停住
        while (g_ttsPlayIdx < g_ttsJobs.size() && g_ttsJobs[g_ttsPlayIdx].state == -1)
            ++g_ttsPlayIdx;
        if (g_ttsPlayIdx >= g_ttsJobs.size()) { allDone = true; }
        else if (g_ttsJobs[g_ttsPlayIdx].state == 2) {
            wav.swap(g_ttsJobs[g_ttsPlayIdx].wav);
            ++g_ttsPlayIdx;
        }
        // 还没合完（state==0/1）：这一帧什么都不做，等后台线程
    }
    // ★ 这一轮全部处理完且存在失败句子：在聊天窗口里说明一次。
    //   不然用户只会觉得"没声音"，而真正的原因（参考音频缺失 / 端口不对）藏在日志里。
    if (allDone && !g_ttsErrNotified && g_ttsFailCount.load() > 0) {
        g_ttsErrNotified = true;
        if (g_hChatWnd && IsWindow(g_hChatWnd)
            && GetIniInt(L"TTS", L"TtsShowError", 1, 0, 1)) {
            std::wstring msg = L"\r\n（语音合成失败："
                + (g_ttsLastErr.empty() ? std::wstring(L"服务无响应") : g_ttsLastErr)
                + L"，详见 DebugView 的 [TTS]）\r\n";
            auto* em = new std::wstring(msg);
            if (!PostMessageW(g_hChatWnd, WM_APPEND_TEXT, 0, (LPARAM)em)) delete em;
        }
    }
    if (wav.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(g_ttsVoiceMtx);
    if (LoadWavFromMemory(wav, g_tts)) XSoundPlay(g_tts, false);
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
    return ReadIniStringDyn(L"LLM", key, L"", iniPath);
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

// UI 配置（llm.ini 的 [UI] 段）：字号/字体/主题等。
// 缺省沿用旧值，不影响未配置 [UI] 的用户。
static std::wstring GetUIConfigStr(const wchar_t* key, const wchar_t* defVal) {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return defVal ? defVal : L"";
    std::wstring iniPath = exeDir + L"\\llm.ini";
    return ReadIniStringDyn(L"UI", key, defVal, iniPath);
}
static int GetUIConfigInt(const wchar_t* key, int defVal, int lo, int hi) {
    std::wstring v = GetUIConfigStr(key, L"");
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
// 会被生生截断，故事讲一半就没了。M6 起改为"强指令 + 弱词特征"双重判定，
// 强/弱关键词表直接内联在 IsLongFormRequest 里，避免维护两份常量。

// 长文触发双重判定：
//   1) 手动开关（llm.ini [LLM] LongFormManual：0=短文 1=长文 2=自动，默认 2）
//      - 显式设为 1 -> 直接进长文；设为 0 -> 强制短文（除非命中强指令）；
//   2) 自动模式按"显式长指令 或 (强关键词 + 输入特征)"判定：
//      - 命中任一"强指令"（讲一个故事/写一篇/详细展开/完整/续写/长篇）-> 长文；
//      - 命中"弱词"（为什么/介绍一下/原理）需搭配输入特征（>=20 字 或 含"讲/写/详"）才放宽；
//      - 纯"为什么"这类弱提问不再误触发 8000 token 长文。
//   这样减少误触发、响应更快，同时保留"主人要长篇"时的完整输出。
static bool IsLongFormRequest(const std::wstring& text) {
    if (text.empty()) return false;
    int manual = GetLLMConfigInt(L"LongFormManual", 2, 0, 2);   // 0 短文 / 1 长文 / 2 自动
    if (manual == 1) return true;                                 // 手动强开
    if (GetLLMConfigInt(L"LongMode", 1, 0, 1) == 0) return false;
    if (manual == 0) {
        // 手动短文：仅当输入命中"强指令"才放宽，否则一律短文
        static const wchar_t* strong[] = { L"讲一个", L"讲个", L"写一篇", L"写个", L"写一段",
            L"长篇", L"详细", L"完整", L"续写", L"接下去", L"展开讲" };
        for (const wchar_t* k : strong)
            if (text.find(k) != std::wstring::npos) return true;
        return text.size() >= 120;    // 手动短文模式下，超大段输入才放宽
    }
    // 自动模式（默认）：强指令直接命中
    static const wchar_t* strong[] = { L"讲一个", L"讲个", L"讲讲", L"编一个", L"编个",
        L"写一篇", L"写个", L"写一段", L"长篇", L"长一点", L"详细", L"展开讲", L"展开",
        L"续写", L"接下去", L"完整", L"睡前", L"童话", L"教程", L"教学", L"教我",
        L"列举", L"分析一下", L"来历", L"传说", L"背景设定", L"世界观", L"介绍一下", L"原理" };
    for (const wchar_t* k : strong)
        if (text.find(k) != std::wstring::npos) return true;
    // 弱词需搭配输入特征（>=20 字 或 含"讲/写/详"），避免"为什么"误触发
    static const wchar_t* weak[] = { L"为什么", L"说说看", L"具体说说", L"举例", L"然后呢" };
    bool hasWeak = false;
    for (const wchar_t* k : weak)
        if (text.find(k) != std::wstring::npos) hasWeak = true;
    if (hasWeak) {
        bool feature = (text.size() >= 20) ||
            (text.find(L"讲") != std::wstring::npos || text.find(L"写") != std::wstring::npos ||
             text.find(L"详") != std::wstring::npos);
        if (feature) return true;
    }
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

// 导演字段补充指令：只扩展输出的"演出字段"，不改变人设/世界观/口吻/语言风格。
// 由 llm.ini [LLM] DirectorEnable=1 控制（默认开）；设为 0 则完全不追加，
// 模型仍按你自己的 prompt.txt 输出 zh/ja/emotion，行为与改造前完全一致。
// 所有新字段都标为"可选"，模型不输出也能正常对话（代码端有缺省回退）。
static const wchar_t* DIRECTOR_INSTRUCT =
L"\n\n【演出字段（可选，仅在自然贴切时附带）】\n"
L"在保持原有 JSON 字段不变的前提下，你可以额外附带以下字段来让演出更贴合你说的话；"
L"如果觉得没必要，全部省略即可，不要为了填字段而牺牲说话的自然：\n"
L"- \"intensity\": 0到1的小数，表示这句话的情绪强度（平静0.2、一般0.5、很激动0.9）。\n"
L"- \"action\": 从 idle(待机)/think(点头思考)/peace(开心摇摆) 里选一个最贴合当前情绪的，可不填。\n"
L"- \"gaze\": 从 at_user(看着主人)/away(移开视线)/down(害羞低头) 里选一个，可不填。\n"
L"- \"memory\": 如果这次对话里你得知了关于主人的、值得长期记住的事实（比如称呼、喜好、约定、纪念日），"
L"用一句简短的话写在这里；没有就省略。不要记无关紧要的闲聊。\n"
L"这些字段只影响动作和视线演出，不改变你说话的性格与口吻。";

// ==================== 打字机自适应 ====================
// 按字数动态决定打字速度：固定 30ms/字的话，1500 字的长文要打 45 秒，观感像卡死。
// 改成按"目标总时长"反推每字延时和每批字符数。
struct TypewriterPlan { int delayMs; int charsPerTick; };

static TypewriterPlan PlanTypewriter(size_t len) {
    int targetMs = GetLLMConfigInt(L"TypewriterTargetMs", 6000, 500, 120000);
    int minDelay = GetLLMConfigInt(L"TypewriterMinDelayMs", 2, 0, 500);
    int maxDelay = GetLLMConfigInt(L"TypewriterMaxDelayMs", TYPEWRITER_DELAY_MS, 1, 500);
    // [UI] TypewriterSkipMode：2=加速模式，把目标时长砍半（整体更快显示）
    int skipMode = GetUIConfigInt(L"TypewriterSkipMode", 1, 0, 2);
    if (skipMode == 2) targetMs /= 2;
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
"格式严格为：{\"zh\":\"中文回复\",\"ja\":\"意思相同的日语回复，用常陆茉子口吻\",\"emotion\":\"情绪\"}\n"
"规则：zh和ja都不能为空；ja要与zh同样饱满有感情，不要缩短；\n"
"emotion 必须原样填写下面 8 个表情槽名之一（含下划线和数字，不要自造词、不要写中文）："
"base(平静)/smile(微笑)/smile_sulky(坏笑)/panic(惊慌)/shy_blush(害羞脸红)/embarrassed(尴尬抿嘴)/thinking2(思考)/wonder(疑惑)。\n"
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

// ==================================================================
// 启动时缓存 prompt + 记忆，避免每条请求全量读盘；
//   历史文件加滚动归档（超 N KB 轮转），防止无限增长。
// ==================================================================
// 历史文件写锁：聊天/触摸两条 LLM 线程都往 chat_history.txt 追加，
//   加一把锁避免多线程 append 交错写坏文件。
static std::recursive_mutex g_historyFileMtx;

static std::wstring g_promptCache;                   // prompt 启动缓存
static std::mutex   g_promptCacheMtx;
static bool         g_promptCacheLoaded = false;

// 取缓存的 prompt：首次（或缓存失效）读盘，之后直接返回。
static std::wstring GetCachedPrompt() {
    std::lock_guard<std::mutex> lk(g_promptCacheMtx);
    if (!g_promptCacheLoaded) {
        g_promptCache = LoadPrompt();
        g_promptCacheLoaded = true;
    }
    return g_promptCache;
}

// 缓存失效（比如 prompt.txt 被外部改过、或用户清缓存）时调用。
static void InvalidatePromptCache() {
    std::lock_guard<std::mutex> lk(g_promptCacheMtx);
    g_promptCache.clear();
    g_promptCacheLoaded = false;
}

// 记忆缓存：内存里持有一份，写入时同步（见 AppendMemoryLine 末尾）。
static std::vector<std::wstring> g_memoryCache;
static std::mutex                 g_memoryCacheMtx;
static bool                       g_memoryCacheLoaded = false;

// 前置声明（定义在下方长期记忆区）：缓存冷启动时要读盘
static std::vector<std::wstring> LoadMemoryLines();

static std::vector<std::wstring> GetMemoryCache() {
    std::lock_guard<std::mutex> lk(g_memoryCacheMtx);
    if (!g_memoryCacheLoaded) {
        g_memoryCache = LoadMemoryLines();
        g_memoryCacheLoaded = true;
    }
    return g_memoryCache;
}

// 历史滚动归档：chat_history.txt 超 N KB 时，把前半归档到 chat_history.old.txt，
// 主文件只保留最近 1/2，避免无限增长。默认 512KB 触发（HistoryArchiveKB 可配）。
static void ArchiveHistoryIfLarge() {
    int maxKb = GetLLMConfigInt(L"HistoryArchiveKB", 512, 0, 8192);   // 0=不归档
    if (maxKb <= 0) return;
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return;
    std::wstring hist = exeDir + L"\\chat_history.txt";
    std::wstring arch = exeDir + L"\\chat_history.old.txt";
    FILE* f = nullptr;
    _wfopen_s(&f, hist.c_str(), L"rb");
    if (!f) return;
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    long long threshold = (long long)maxKb * 1024;
    if (sz > threshold) {
        fclose(f);
        std::lock_guard<std::recursive_mutex> lk(g_historyFileMtx);
        // 读全文
        std::wstring exeDir2;
        GetExeDir(exeDir2);
        FILE* rf = nullptr;
        _wfopen_s(&rf, hist.c_str(), L"rb");
        if (rf) {
            std::string bytes;
            char b[8192]; size_t n;
            while ((n = fread(b, 1, sizeof b, rf)) > 0) bytes.append(b, n);
            fclose(rf);
            // 前半归档（追加到 .old），后半保留
            size_t half = bytes.size() / 2;
            FILE* af = nullptr;
            _wfopen_s(&af, arch.c_str(), L"ab");
            if (af) {
                if (half > 0) fwrite(bytes.data(), 1, half, af);
                fclose(af);
            }
            FILE* wf = nullptr;
            _wfopen_s(&wf, hist.c_str(), L"wb");
            if (wf) {
                const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
                fwrite(bom, 1, 3, wf);
                if (half < bytes.size()) fwrite(bytes.data() + half, 1, bytes.size() - half, wf);
                fclose(wf);
            }
            PetDbgW(L"[M8] 历史已滚动归档\n");
        }
    } else {
        fclose(f);
    }
}

// ==================================================================
//  ★ 对话文本换行规范化
//  RichEdit 内部只把 CR 当段落符：裸 LF 会被原样塞进去，显示时不换行。
//  模型回复来自 JSON，里面是 "\n"；本地拼接历来手写 "\r\n"；两条流水线
//  混在一起，就出现"有的地方换行了、有的地方粘成一行"——也就是换行时灵时不灵。
//  这里把所有来源统一成 CRLF：追加、落盘、读档三条路走同一套规则。
// ==================================================================
// 统一 CRLF，并把连续空行压到最多一空行（模型常在结尾甩 3~4 个换行）
static std::wstring NormalizeCrlf(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 16);
    int run = 0;                                  // 连续换行计数（一个 CRLF 算一次）
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'\r' || c == L'\n') {
            if (c == L'\r' && i + 1 < in.size() && in[i + 1] == L'\n') ++i;
            if (++run <= 2) out += L"\r\n";
            continue;
        }
        run = 0;
        out += c;
    }
    return out;
}

static bool IsBlankChar(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0x3000;
}

// 去掉首尾空白（含换行）：避免出现"主人：\r\n\r\n\r\n\r\n茉子："这种空块
static std::wstring TrimBlock(const std::wstring& in) {
    size_t b = 0, e = in.size();
    while (b < e && IsBlankChar(in[b])) ++b;
    while (e > b && IsBlankChar(in[e - 1])) --e;
    return in.substr(b, e - b);
}

// 流式分片可能把一个 CRLF 切成 "\r" 和 "\n" 两块，
// 不做记账就会被补成两个换行（多出一个空行）。
static bool g_streamCr = false;
static int  g_nlRun    = 0;      // 连续换行计数（跨分片累计，和内存里的文本保持同步）

// ★ 追加的唯一入口：规范化换行 + 光标移到末尾 + 滚到底。
//   所有追加操作走这一处入口，统一换行处理与 EM_REPLACESEL 逻辑，
//   避免多处各自实现导致行为不一致。
//   连续空行限制为一个：模型经常在段末甩 3~4 个 \n，原样写进去就是一大片空白。
static void AppendHistoryText(const std::wstring& text) {
    if (text.empty()) return;
    if (!g_hHistoryEdit || !IsWindow(g_hHistoryEdit)) return;
    std::wstring norm;
    norm.reserve(text.size() + 16);
    for (wchar_t c : text) {
        if (c == L'\r') { if (++g_nlRun <= 2) norm += L"\r\n"; g_streamCr = true; }
        else if (c == L'\n') {
            if (g_streamCr) g_streamCr = false;                 // 已被上一块的 \r 消费
            else if (++g_nlRun <= 2) norm += L"\r\n";
        } else { g_streamCr = false; g_nlRun = 0; norm += c; }
    }
    if (norm.empty()) return;
    DWORD end = GetWindowTextLengthW(g_hHistoryEdit);
    SendMessageW(g_hHistoryEdit, EM_SETSEL, (WPARAM)end, (LPARAM)end);
    SendMessageW(g_hHistoryEdit, EM_REPLACESEL, TRUE, (LPARAM)norm.c_str());
    SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
}

// 只读控件尾部几个字符，不整段取文本（历史可能上百 KB）。
// 这里不走 richedit.h：本文件只动态加载 Msftedit.dll，从没包含过该头文件，
// 直接按其 ABI 声明区间结构 + 消息号（EM_GETTEXTRANGE = WM_USER + 75）即可。
struct PetCharRange { LONG cpMin; LONG cpMax; };          // == CHARRANGE
struct PetTextRange { PetCharRange chrg; LPWSTR lpstrText; };
#ifndef EM_GETTEXTRANGE
#define EM_GETTEXTRANGE (WM_USER + 75)
#endif

static std::wstring HistoryTail(int maxChars) {
    if (!g_hHistoryEdit || !IsWindow(g_hHistoryEdit)) return L"";
    long len = (long)GetWindowTextLengthW(g_hHistoryEdit);
    if (len <= 0) return L"";
    if (maxChars < 4) maxChars = 4;
    long from = (len > maxChars) ? len - maxChars : 0;
    wchar_t buf[64] = { 0 };
    PetTextRange tr{};
    tr.chrg.cpMin = from; tr.chrg.cpMax = len;
    tr.lpstrText = buf;
    SendMessageW(g_hHistoryEdit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    return std::wstring(buf);
}

// 数尾部有几个"段落分隔"。RichEdit 内部用单个 CR 表示换行，EM_GETTEXTRANGE 读回来也是 CR，
// 只有 GetWindowText 才还原成 CRLF —— 所以这里两种写法都要认。
static int CountTrailingBreaks(const std::wstring& s) {
    int n = 0; size_t i = s.size();
    while (i > 0) {
        wchar_t c = s[i - 1];
        if (c == L'\n') { --i; continue; }            // CRLF 里的 LF 不单独算一次换行
        if (c == L'\r') { --i; ++n; continue; }
        break;
    }
    return n;
}

// ★ 保证"上一段话"和"下一段话"之间隔着一个空行。
//   统一维护段间空行，避免各处手写前缀导致空行数量不一致。
static void EnsureHistoryBlankLine() {
    if (!g_hHistoryEdit || !IsWindow(g_hHistoryEdit)) return;
    std::wstring tail = HistoryTail(16);
    if (tail.empty()) return;
    size_t n = tail.size();                       // 尾部空格不算内容
    while (n > 0 && (tail[n - 1] == L' ' || tail[n - 1] == L'\t')) --n;
    tail.resize(n);
    if (CountTrailingBreaks(tail) >= 2) return;   // 已经是空行，别再补
    // 上一段（尤其是模型回复）可能以换行结尾，此时 g_nlRun 还停在
    //   1 或 2，AppendHistoryText 的去重逻辑会把分隔符整段丢掉，于是"茉子：…主人：…"
    //   粘成一行。这里先清零记账，保证分隔符一定写进去。
    g_nlRun = 0; g_streamCr = false;
    AppendHistoryText(CountTrailingBreaks(tail) == 1 ? L"\r\n" : L"\r\n\r\n");
}

// ==================================================================
// 聊天记录角色/情绪上色：RichEdit 按块前缀设色，长对话里可扫读。
//   主人=蓝、茉子=朱红、旁白(（…）)=灰斜体、好感提示=绿。
//   用 EM_SETCHARFORMAT + SCF_SELECTION 只给"刚追加的块"上色，不全量重排。
//   CHARFORMAT2W/CHARRANGE/CFM_*/SCF_* 均来自 <richedit.h>（见文件头 include），
//   不再手写结构体——手写布局一旦和 SDK 不一致，RichEdit 会读错字段导致上色失效。
//   注：EM_SETCHARFORMAT / EM_GETTEXTRANGE / SCF_SELECTION 在 richedit.h 已有定义，
//   下面这些 #ifndef 仅为兼容旧 SDK 保留兜底，正常不会生效。
// ==================================================================
#ifndef EM_SETCHARFORMAT
#define EM_SETCHARFORMAT (WM_USER + 68)
#endif
#ifndef SCF_SELECTION
#define SCF_SELECTION 1
#endif

// 说话人判定：除了"主人："这种标准前缀，也要认"（主动搭话）茉子："这种带标注的写法
// （标注不会太长，限定在前 12 个字符内，避免正文里提到"茉子："被误判）。
static bool BlockStartsWithSpeaker(const std::wstring& block, const wchar_t* tag) {
    size_t pos = block.find(tag);
    return pos != std::wstring::npos && pos <= 12;
}

// 按块前缀取颜色与效果（-1 表示不上色，沿用默认）。
// ★配色约定（玩家指定）：主人=蓝、茉子=绿、旁白(（…）)=灰斜体、好感/心情提示=琥珀。
//   颜色随主题取两套（深底用更亮的色保证对比度），由 g_chatTheme 缓存决定。
static void ColorForPrefix(const std::wstring& block, LONG& color, DWORD& effects) {
    effects = 0;
    const bool dark = (g_chatTheme == 1);
    if (BlockStartsWithSpeaker(block, L"主人：")) {
        color = dark ? RGB(115, 175, 255) : RGB(0, 102, 204);     // 蓝（主人）
        return;
    }
    if (BlockStartsWithSpeaker(block, L"茉子：")) {
        color = dark ? RGB(110, 220, 140) : RGB(21, 128, 61);     // 绿（茉子）
        return;
    }
    if (!block.empty() && block[0] == L'（') {
        if (block.find(L"好感") != std::wstring::npos ||
            block.find(L"茉子的心意") != std::wstring::npos ||
            block.find(L"茉子的心情") != std::wstring::npos) {
            // 琥珀色：与茉子的绿区分，暖色便于一眼识别
            color = dark ? RGB(240, 190, 90) : RGB(176, 108, 0);
        } else {
            color = dark ? RGB(180, 180, 190) : RGB(120, 120, 120); // 灰（旁白）
            effects = CFE_ITALIC;
        }
        return;
    }
    color = -1;                        // 其它：不上色
}

// 给 [cpStart, cpEnd) 区间上色
static void ColorizeRange(HWND edit, long cpStart, long cpEnd, LONG color, DWORD effects) {
    if (!edit || !IsWindow(edit) || color < 0 || cpEnd <= cpStart) return;
    SendMessageW(edit, EM_SETSEL, (WPARAM)cpStart, (LPARAM)cpEnd);
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    // 只改颜色 + 斜体效果；不碰 CFM_SIZE/CFM_FACE，避免把字号/字体重置掉。
    cf.dwMask = CFM_COLOR | CFM_ITALIC;
    cf.dwEffects = effects;              // 非斜体块传 0，显式清掉斜体位
    cf.crTextColor = color;
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

// 把 [cpStart,cpEnd) 这段"实际写进控件的文本"读回来，
//   按段落边界切开，每段各自识别前缀设色。
//   ★★ 关键坑：RichEdit 的 cp 空间里，一个段落分隔占【2 格】（内部存 1 个 CR，
//   但 GetWindowTextLengthW 按 CRLF 算长度），而 EM_GETTEXTRANGE 读回来的字符串里
//   只有 1 个 CR。所以"raw 下标 == cp 下标"是错的，直接相加会让颜色随换行数量往左漂，
//   长对话里越往后错得越多、末尾干脆不上色。这里先建一张 raw→cp 映射表再上色。
static void ColorizeSpan(HWND edit, long cpStart, long cpEnd) {
    if (!edit || !IsWindow(edit) || cpEnd <= cpStart) return;
    long spanCp = cpEnd - cpStart;
    std::wstring text((size_t)spanCp + 1, L'\0');
    PetTextRange tr{};
    tr.chrg.cpMin = cpStart; tr.chrg.cpMax = cpEnd;
    tr.lpstrText = &text[0];
    SendMessageW(edit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    text.resize(wcslen(text.c_str()));   // EM_GETTEXTRANGE 以 NUL 结尾

    // raw 下标 → cp 之间的换算表（CR 占 2 格；紧跟其后的 LF 不额外占格）
    std::vector<long> cpOf(text.size() + 1, cpStart);
    long cp = cpStart;
    for (size_t k = 0; k < text.size(); ++k) {
        cpOf[k] = cp;
        wchar_t c = text[k];
        if (c == L'\r') cp += 2;
        else if (c == L'\n') { if (k == 0 || text[k - 1] != L'\r') cp += 2; }
        else cp += 1;
    }
    cpOf[text.size()] = cp;

    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == L'\r' || text[i] == L'\n')) ++i;  // 跳过空行
        if (i >= text.size()) break;
        size_t segEnd = i;
        while (segEnd < text.size() && text[segEnd] != L'\r' && text[segEnd] != L'\n') ++segEnd;
        LONG color; DWORD effects;
        ColorForPrefix(text.substr(i, segEnd - i), color, effects);
        ColorizeRange(edit, cpOf[i], cpOf[segEnd], color, effects);
        i = segEnd;
    }
    // 上色靠"选中再设格式"，收尾时把选区收回到末尾，
    // 否则最后一段会一直处于选中态（看起来像被高亮）。
    long endCp = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, (WPARAM)endCp, (LPARAM)endCp);
}

// 开新的一段（玩家/茉子说的每一句都算一段）
static void AppendChatView(const std::wstring& block) {
    if (block.empty()) return;
    EnsureHistoryBlankLine();
    long startCp = (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) ? GetWindowTextLengthW(g_hHistoryEdit) : 0;
    AppendHistoryText(block);
    // 新增块按段落逐段上色（而非整块一个色）
    if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) {
        long endCp = GetWindowTextLengthW(g_hHistoryEdit);
        ColorizeSpan(g_hHistoryEdit, startCp, endCp);
    }
}

// 载入历史后整体重上色：与 AppendChatView 共用 ColorizeSpan，逻辑一致。
static void ColorizeFullHistory() {
    if (!g_hHistoryEdit || !IsWindow(g_hHistoryEdit)) return;
    int len = GetWindowTextLengthW(g_hHistoryEdit);
    if (len <= 0) return;
    ColorizeSpan(g_hHistoryEdit, 0, (long)len);
}

// ==================================================================
//  深色主题：RichEdit 不发送 WM_CTLCOLOREDIT，它自己绘制背景，
//  必须用 EM_SETBKGNDCOLOR 设底色、EM_SETCHARFORMAT(SCF_ALL) 设文字色，
//  否则历史框/输入框保持白底而文字被上成浅色，主题看起来没生效。
// ==================================================================
static const COLORREF kChatDarkBg = RGB(30, 30, 40);
static const COLORREF kChatDarkFg = RGB(230, 230, 235);

// 设置整个 RichEdit 的默认文字色（未被角色色覆盖的部分，如占位提示语）
static void SetRichEditDefaultColor(HWND edit, COLORREF color) {
    if (!edit || !IsWindow(edit)) return;
    CHARFORMAT2W cf{};
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.dwEffects = 0;
    cf.crTextColor = color;
    // SCF_ALL 改已有文本；SCF_DEFAULT 同时设默认格式，保证空控件里新输入的字也是这个颜色
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
}

// 把当前主题应用到聊天窗全部控件（建窗 / 切换主题 / 保存设置 时统一调用）
static void ApplyChatTheme(HWND hwnd) {
    const bool dark = (g_chatTheme == 1);
    const COLORREF bg = dark ? kChatDarkBg : GetSysColor(COLOR_WINDOW);
    const COLORREF fg = dark ? kChatDarkFg : GetSysColor(COLOR_WINDOWTEXT);
    // 1) RichEdit 背景：唯一真正生效的途径
    if (g_hHistoryEdit && IsWindow(g_hHistoryEdit))
        SendMessageW(g_hHistoryEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)bg);
    if (g_hInputEdit && IsWindow(g_hInputEdit))
        SendMessageW(g_hInputEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)bg);
    // 2) 先铺默认文字色，再按角色重上色（角色色是浅色系，深底才看得清）
    SetRichEditDefaultColor(g_hHistoryEdit, fg);
    SetRichEditDefaultColor(g_hInputEdit, fg);
    ColorizeFullHistory();
    // 3) 整体重绘（窗口边框 / 状态栏 / 按钮）
    if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, TRUE);
    if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) InvalidateRect(g_hHistoryEdit, nullptr, TRUE);
    if (g_hInputEdit && IsWindow(g_hInputEdit)) InvalidateRect(g_hInputEdit, nullptr, TRUE);
    if (g_hStatusLabel && IsWindow(g_hStatusLabel)) InvalidateRect(g_hStatusLabel, nullptr, TRUE);
    if (g_hAttachStrip && IsWindow(g_hAttachStrip)) InvalidateRect(g_hAttachStrip, nullptr, TRUE);
    if (hwnd && IsWindow(hwnd)) {
        const int ids[] = { ID_BTN_CLEAR, ID_BTN_MEMORY, ID_BTN_LONGFORM, ID_BTN_THEME,
            ID_BTN_ATTACH, ID_BTN_MIC, 103, 104 };
        for (int id : ids) {
            HWND b = GetDlgItem(hwnd, id);
            if (b) InvalidateRect(b, nullptr, TRUE);
        }
    }
}

// ★深色主题下的按钮自绘：标准 PUSHBUTTON 由系统绘制，不吃 WM_CTLCOLOR，
//   深底上会留一排浅灰按钮、看起来像"主题没生效"。
//   所以聊天窗按钮统一改成 BS_OWNERDRAW，浅色/深色都用同一套自绘逻辑。
static void DrawChatButton(const DRAWITEMSTRUCT* dis) {
    if (!dis || dis->CtlType != ODT_BUTTON) return;
    const bool dark = (g_chatTheme == 1);
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF bg, fg, border;
    if (dark) {
        bg     = pressed ? RGB(62, 62, 78) : RGB(48, 48, 60);
        fg     = disabled ? RGB(120, 120, 132) : RGB(235, 235, 242);
        border = RGB(84, 84, 100);
    } else {
        bg     = pressed ? RGB(206, 206, 212) : RGB(240, 240, 243);
        fg     = disabled ? RGB(155, 155, 155) : RGB(32, 32, 32);
        border = RGB(188, 188, 194);
    }
    HBRUSH br = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, br);
    DeleteObject(br);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
    HGDIOBJ oldBr  = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBr);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(pen);

    // ★附件/上传按钮：用 GDI 画一个几何加号，不依赖 emoji 字形。
    //   加号大小、粗细、颜色完全可控，深浅主题统一，也不会因字体缺字变成方块。
    if (GetDlgCtrlID(dis->hwndItem) == ID_BTN_ATTACH) {
        RECT arc = dis->rcItem;
        if (pressed) OffsetRect(&arc, 0, 1);
        int cx = (arc.left + arc.right) / 2;
        int cy = (arc.top + arc.bottom) / 2;
        int w  = arc.right - arc.left, h = arc.bottom - arc.top;
        int arm = (w < h ? w : h) * 3 / 10;   // 半臂长：总长约为短边的 60%
        if (arm < 5) arm = 5;
        HPEN plusPen = CreatePen(PS_SOLID, 2, fg);
        HGDIOBJ oldP = SelectObject(dis->hDC, plusPen);
        MoveToEx(dis->hDC, cx - arm, cy, nullptr); LineTo(dis->hDC, cx + arm, cy);
        MoveToEx(dis->hDC, cx, cy - arm, nullptr); LineTo(dis->hDC, cx, cy + arm);
        SelectObject(dis->hDC, oldP);
        DeleteObject(plusPen);
        if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
        return;
    }

    wchar_t text[256] = {};
    GetWindowTextW(dis->hwndItem, text, 255);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, fg);
    HFONT font = (HFONT)SendMessageW(dis->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(dis->hDC, font) : nullptr;
    RECT rc = dis->rcItem;
    if (pressed) OffsetRect(&rc, 0, 1);       // 按下时文字下移 1px，像真按钮
    DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (oldFont) SelectObject(dis->hDC, oldFont);
    if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
}

// 接在当前这段的末尾（流式回复、同一条回复的后半句），收尾补一个空行
static void AppendChatTail(const std::wstring& text) {
    AppendHistoryText(text);
    EnsureHistoryBlankLine();
}

static std::wstring LoadChatHistory() {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return L"";
    std::wstring historyPath = exeDir + L"\\chat_history.txt";
    if (!PathFileExistsW(historyPath.c_str())) return L"";
    // 历史文件会一直追加。只读末尾一段：否则聊天久了发给模型的上下文
    // 永远停在最早那几轮，最近说过的话反而看不见，内存也会无限增长。
    const long long kMaxRead = 128 * 1024;
    FILE* f = nullptr;
    _wfopen_s(&f, historyPath.c_str(), L"rb");
    if (!f) return L"";
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    if (sz <= 0) { fclose(f); return L""; }
    long long start = (sz > kMaxRead) ? (sz - kMaxRead) : 0;
    _fseeki64(f, start, SEEK_SET);
    std::string bytes((size_t)(sz - start), '\0');
#pragma warning(suppress:6031)
    size_t nread = bytes.empty() ? 0 : fread(&bytes[0], 1, bytes.size(), f);
    fclose(f);
    bytes.resize(nread);
    // 截断处可能落在 UTF-8 多字节序列中间，先丢掉开头的续字节
    size_t skip = 0;
    while (skip < bytes.size() &&
           ((unsigned char)bytes[skip] & 0xC0) == 0x80) ++skip;
    if (skip) bytes.erase(0, skip);
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring history((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), &history[0], n);
    // RichEdit 需要 CRLF；顺手把老存档里因文本模式写入多出来的空行也压掉
    return NormalizeCrlf(history);
}

static void SaveChatHistory(const std::wstring& userInput, const std::wstring& reply) {
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return;
    std::wstring historyPath = exeDir + L"\\chat_history.txt";
    std::wstring u = TrimBlock(userInput);
    std::wstring r = TrimBlock(reply);
    if (u.empty() && r.empty()) return;
    std::lock_guard<std::recursive_mutex> lk(g_historyFileMtx);
    // 一段一轮，段内 CRLF、段间空行；换行在这里就规范化好，
    // 下次读档显示才会和当时看到的一模一样。
    std::wstring record = L"主人：" + u + L"\r\n\r\n" + L"茉子：" + r + L"\r\n\r\n";
    std::string utf8 = WideStringToUtf8(record);
    // 用二进制模式写字节：UTF-8 文本模式下 CRT 会再次翻译 \r\n，
    // 读回变成 \r\r\n，导致空行忽多忽少。
    FILE* f = nullptr;
    _wfopen_s(&f, historyPath.c_str(), L"ab");
    if (!f) return;
    _fseeki64(f, 0, SEEK_END);
    if (_ftelli64(f) == 0) {                       // 新文件补 BOM，读档时会剥掉
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        fwrite(bom, 1, sizeof bom, f);
    }
#pragma warning(suppress:6031)                     // 写失败不影响主流程
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
    // 周期性触发历史滚动归档（每 50 轮检查一次，避免频繁 stat）
    {
        static std::atomic<int> s_archiveTick{ 0 };
        if (++s_archiveTick >= 50) { s_archiveTick = 0; ArchiveHistoryIfLarge(); }
    }
}

// 当前触摸的旁白：LLM 回复到达时，用它和回复配对写入 chat_history。
// DoBodyTouch 有 g_touchRunning 互斥，同一时刻只有一个触摸在途，用全局变量即可。
static std::wstring g_touchPendingNarration;
// ★ 本轮的玩家发言：回复到达时直接用它配对写盘。
//   回复到达时直接用它配对写盘；从控件全文 rfind("主人：") 反推，
//   会因模型在回话里喊"主人："而错位。
static std::wstring g_pendingUserInput;

// 会话序号：每次发起聊天请求时 ++，投递到 UI 的消息带上该 id。
//   主线程（聊天窗）只接受等于"当前期望序号"的回复，晚到/作废的旧回复被丢弃，
//   避免"触摸超时兜底复位后旧线程还在跑、晚到的回复写进历史"或"窗口重建后旧流串进来"。
static std::atomic<int> g_chatSessionSeq{ 0 };        // 全局递增的会话序号
static std::atomic<int> g_chatExpectedSession{ 0 };   // 聊天窗当前应显示的会话

// ==================================================================
// 长期记忆：把茉子记住的"关于主人的事实"沉淀到 memory.txt，
// 每次对话注入 prompt，让她记得你说过的话、喜好、称呼、纪念日。
// 与 chat_history（逐轮对话流）不同，memory 是提炼后的稳定事实，条目少而精。
// 设计要点：去重（新事实覆盖同关键词旧条目）、有条数上限（防止无限膨胀）。
// ==================================================================
#define MEMORY_MAX_LINES  60      // 最多 60 条事实，超出丢弃最旧的
#define MEMORY_MAX_CHARS  4000    // 注入 prompt 的记忆文本上限

// 读取全部记忆（UTF-8 -> wstring），返回已规范化的行数组。
static std::vector<std::wstring> LoadMemoryLines() {
    std::vector<std::wstring> lines;
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return lines;
    std::wstring path = exeDir + L"\\memory.txt";
    if (!PathFileExistsW(path.c_str())) return lines;
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return lines;
    std::string bytes;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.append(buf, n);
    fclose(f);
    // 去 BOM
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) bytes.erase(0, 3);
    int nch = MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), nullptr, 0);
    if (nch <= 0) return lines;
    std::wstring all((size_t)nch, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.c_str(), (int)bytes.size(), &all[0], nch);
    size_t start = 0;
    while (start <= all.size()) {
        size_t e = all.find(L'\n', start);
        if (e == std::wstring::npos) e = all.size();
        std::wstring line = all.substr(start, e - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (e == all.size()) break;
        start = e + 1;
    }
    return lines;
}

// ==================================================================
// 长期记忆增强：
//   1) 近似去重：新事实与旧条目互为"长短句包含关系"或"共享 3 字以上关键词"
//      时，视为同一事实，不再写入（避免"喜欢猫"/"很喜欢猫咪"重复占配额）。
//   2) 主题标签替换：带 [主题] 前缀的条目（如 [称呼]），同主题新事实替换旧条目，
//      称呼改了旧的不留。无标签的新事实在与旧条目近似时直接覆盖最接近那条。
//   3) 写入时间戳：每行尾追加 " @<unix秒>"，超期（默认 180 天）低价值条目
//      （无 [高] 标签）自动淘汰，防止记忆越用越满越乱。
//   兼容性：老文件没有标签/时间戳也能读，写入时补上。
// ==================================================================
// 取一条事实的"主题标签"：开头形如 [xxx] 的部分，没有则返回空。
static std::wstring MemTopicTag(const std::wstring& fact) {
    if (fact.empty() || fact[0] != L'[') return L"";
    size_t e = fact.find(L']');
    if (e == std::wstring::npos || e > 8) return L"";
    return fact.substr(1, e - 1);
}
// 是否"高价值"条目（带 [高] 标签）：淘汰时优先保留
static bool MemHighValue(const std::wstring& fact) {
    std::wstring t = MemTopicTag(fact);
    return (t == L"高");
}
// 近似去重判定：去空白/标点后的两个事实，互相包含或共享 3+ 字符公共子串片段。
//   用"字符 n-gram 集合交集"近似衡量语义相似度，简单且够覆盖"猫/猫咪"这类。
static bool MemSimilar(const std::wstring& a, const std::wstring& b) {
    if (a == b) return true;
    // 归一化：小写 + 去空白
    auto norm = [](std::wstring s) {
        for (auto& c : s) if (IsBlankChar(c)) c = L' ';
        while (s.find(L"  ") != std::wstring::npos) s.erase(s.find(L"  "), 1);
        return s;
    };
    std::wstring na = norm(a), nb = norm(b);
    if (na.find(nb) != std::wstring::npos || nb.find(na) != std::wstring::npos) return true;
    // 取 a 的所有 3-gram 集合，统计 b 中命中比例；>= 40% 视为近似
    std::wstring grams;
    for (size_t i = 0; i + 3 <= na.size(); ++i) {
        std::wstring g = na.substr(i, 3);
        if (nb.find(g) != std::wstring::npos) grams += g;
    }
    size_t total = na.size() >= 3 ? na.size() - 2 : 1;
    if (total == 0) return false;
    double ratio = (double)grams.size() / ((double)total * 3.0);
    return ratio >= 0.4;
}

// 写入一条新记忆：近似去重 + 主题替换 + 时间戳，超上限截断最旧（高价值条目保护）。
// 走内存缓存：读缓存 -> 改 -> 写盘 -> 同步回缓存，避免每轮全量读盘。
static void AppendMemoryLine(const std::wstring& fact) {
    if (fact.empty()) return;
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return;
    std::wstring path = exeDir + L"\\memory.txt";
    auto lines = GetMemoryCache();

    // 给新事实加时间戳（若还没有）
    SYSTEMTIME st; GetLocalTime(&st);
    ULARGE_INTEGER ticks; GetSystemTimeAsFileTime((FILETIME*)&ticks);
    unsigned long long unixSec = ticks.QuadPart / 10000000ull - 11644473600ull;
    std::wstring tagged = fact;
    if (tagged.find(L" @") == std::wstring::npos)
        tagged += L" @" + std::to_wstring(unixSec);

    // ① 近似去重 + ② 主题替换：找到"该被替换"的旧条目下标，找不到则 -1
    int replaceIdx = -1;
    std::wstring newTag = MemTopicTag(tagged);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (newTag.empty()) {
            // 无标签新事实：与任何近似旧条目都不再重复写；若近似则覆盖最旧那条
            if (MemSimilar(tagged, lines[i])) { replaceIdx = (int)i; break; }
        } else {
            // 有主题标签：同主题旧条目直接被新事实替换（称呼改了不留旧的）
            if (MemTopicTag(lines[i]) == newTag && !MemHighValue(tagged)) {
                replaceIdx = (int)i; break;
            }
            // 同主题但新事实是"高"价值：保留新的，仍替换旧
            if (MemTopicTag(lines[i]) == newTag) { replaceIdx = (int)i; break; }
        }
    }
    if (replaceIdx >= 0)
        lines[replaceIdx] = tagged;               // 替换
    else
        lines.push_back(tagged);                  // 全新

    // ③ 时间戳淘汰：超期低价值条目移除（高价值条目保护）
    unsigned long long nowSec = unixSec;
    int expireDays = GetLLMConfigInt(L"MemoryExpireDays", 180, 0, 3650);
    if (expireDays > 0) {
        unsigned long long cutoff = nowSec - (unsigned long long)expireDays * 86400ull;
        size_t w = 0;
        for (size_t i = 0; i < lines.size(); ++i) {
            auto& l = lines[i];
            bool keep = true;
            if (!MemHighValue(l)) {
                size_t at = l.rfind(L" @");
                if (at != std::wstring::npos) {
                    unsigned long long ts = 0;
                    try { ts = std::stoull(l.substr(at + 2)); } catch (...) {}
                    if (ts > 0 && ts < cutoff) keep = false;
                }
            }
            if (keep) { lines[w++] = l; }
        }
        lines.resize(w);
    }

    // 超上限截断最旧的（从头部丢），但保护最近 5 条高价值条目
    while ((int)lines.size() > MEMORY_MAX_LINES) {
        int rm = -1;
        for (int i = (int)lines.size() - 1; i >= 0; --i)
            if (!MemHighValue(lines[i]) || (int)i < 5) { rm = i; break; }
        if (rm < 0) rm = 0;
        lines.erase(lines.begin() + rm);
    }

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8");
    if (!f) return;
    for (const auto& l : lines) {
        std::wstring rec = l + L"\r\n";
        fwrite(rec.c_str(), sizeof(wchar_t), rec.length(), f);
    }
    fclose(f);
    // 写盘成功 -> 同步内存缓存，下一轮 BuildMemoryContext 直接读缓存
    {
        std::lock_guard<std::mutex> lk(g_memoryCacheMtx);
        g_memoryCache = lines;
        g_memoryCacheLoaded = true;
    }
}

// 拼接成一段可注入 prompt 的记忆文本（带上限，倒序取最近的）。
// 走内存缓存，AppendMemoryLine 写时同步刷新，避免每轮全量读盘。
static std::wstring BuildMemoryContext() {
    auto lines = GetMemoryCache();
    if (lines.empty()) return L"";
    std::wstring out;
    for (int i = (int)lines.size() - 1; i >= 0; --i) {
        std::wstring piece = L"- " + lines[i] + L"\n";
        if (out.size() + piece.size() > MEMORY_MAX_CHARS) break;
        out += piece;
    }
    return out;
}

// 记忆缓存失效（外部改动 memory.txt / 程序启动后）调用。
static void InvalidateMemoryCache() {
    std::lock_guard<std::mutex> lk(g_memoryCacheMtx);
    g_memoryCache.clear();
    g_memoryCacheLoaded = false;
}

// ==================================================================
//  ★附件中枢（图片 / 文本 / PDF·DOCX·XLSX / 语音转写）
//  ------------------------------------------------------------------
//  设计要点：
//   · 附件只活在"待发送队列"里，发送后立刻清空；base64 绝不写进
//     chat_history.txt（否则历史文件几十 MB、启动变慢还泄露隐私）。
//   · 历史里只留一行占位符，如「[图片 photo.jpg]」。
//   · 无附件时请求体仍是原来的字符串 content，对老网关 100% 兼容。
//   · 图片统一缩到最长边 ImageMaxSide（默认 1280）再转 JPEG q85，
//     一张手机照片从 4MB 压到 200KB 左右，视觉问答完全够用。
// ==================================================================
#define ATTACH_MAX_FILES       8
#define ATTACH_MAX_BYTES       (20u * 1024u * 1024u)   // 单文件硬上限
#define ATTACH_IMG_MAX_BYTES   (8u  * 1024u * 1024u)   // 图片单张上限
#define ATTACH_TEXT_MAX_BYTES  (2u  * 1024u * 1024u)   // 文本类上限
#define ATTACH_KIND_IMAGE 0
#define ATTACH_KIND_TEXT  1
#define ATTACH_KIND_VOICE 2

struct Attachment {
    int          kind = ATTACH_KIND_IMAGE;
    std::wstring path;          // 原始路径（剪贴板图片为空）
    std::wstring name;          // 显示名 / 历史占位符用
    std::wstring text;          // 文本类提取结果 / 语音转写
    std::string  b64;           // 图片 JPEG 的 base64
    std::string  mime;          // image/jpeg
    size_t       bytes = 0;     // 原始字节数
    bool         ok = false;    // 处理成功标记
    bool         loading = false;   // ★后台解析中（PDF/OCR 可能要几十秒），先占位显示
    int          id = 0;            // ★解析任务 id：后台结果按 id 回填，不依赖下标
};

static std::vector<Attachment> g_pendingAttachments;   // 仅主线程访问
static void RefreshAttachStrip();                      // UI 层实现（附件条显隐/重绘）
static void AttachDrainResults();                      // 后台解析结果回填 + 刷新附件条

// ---------------- 基础工具 ----------------

static std::string Base64Encode(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    if (!data || len == 0) return out;
    out.resize(((len + 2) / 3) * 4);
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)data[i] << 16;
        if (i + 1 < len) v |= (unsigned)data[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned)data[i + 2];
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? tbl[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < len) ? tbl[v & 63] : '=';
    }
    return out;
}

// GDI+ 编码器 CLSID 查找（JPEG/PNG 都要先拿到 CLSID 才能 Save 到流）
static bool GetEncoderClsid(const wchar_t* mime, CLSID* out) {
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return false;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* codecs = (ImageCodecInfo*)buf.data();
    if (GetImageEncoders(num, size, codecs) != Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(codecs[i].MimeType, mime) == 0) { *out = codecs[i].Clsid; return true; }
    }
    return false;
}

// 把任意 GDI+ 位图缩放到最长边 maxSide，编码成 JPEG(q85) 的 base64
static bool EncodeBitmapToJpegBase64(Bitmap* src, int maxSide, std::string& outB64) {
    outB64.clear();
    if (!src || src->GetLastStatus() != Ok) return false;
    UINT w = src->GetWidth(), h = src->GetHeight();
    if (w == 0 || h == 0) return false;
    UINT longSide = (w > h) ? w : h;
    double scale = 1.0;
    if (maxSide > 0 && (int)longSide > maxSide) scale = (double)maxSide / (double)longSide;
    int nw = (int)(w * scale + 0.5); if (nw < 1) nw = 1;
    int nh = (int)(h * scale + 0.5); if (nh < 1) nh = 1;

    Bitmap* dst = new Bitmap(nw, nh, PixelFormat24bppRGB);
    if (!dst || dst->GetLastStatus() != Ok) { delete dst; return false; }
    {
        Graphics g(dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        // PNG 的透明区直接转 JPEG 会变黑，先铺白底
        g.Clear(Color(255, 255, 255, 255));
        g.DrawImage(src, Rect(0, 0, nw, nh), 0, 0, (int)w, (int)h, UnitPixel);
    }

    CLSID jpgClsid;
    bool ok = false;
    if (GetEncoderClsid(L"image/jpeg", &jpgClsid)) {
        EncoderParameters ep;
        ep.Count = 1;
        ep.Parameter[0].Guid = EncoderQuality;
        ep.Parameter[0].Type = EncoderParameterValueTypeLong;
        ep.Parameter[0].NumberOfValues = 1;
        LONG quality = 85;
        ep.Parameter[0].Value = &quality;
        IStream* stm = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &stm) == S_OK && stm) {
            if (dst->Save(stm, &jpgClsid, &ep) == Ok) {
                STATSTG ss{};
                if (stm->Stat(&ss, STATFLAG_NONAME) == S_OK && ss.cbSize.QuadPart > 0) {
                    HGLOBAL hg = nullptr;
                    if (GetHGlobalFromStream(stm, &hg) == S_OK && hg) {
                        void* p = GlobalLock(hg);
                        if (p) {
                            outB64 = Base64Encode((const unsigned char*)p, (size_t)ss.cbSize.QuadPart);
                            GlobalUnlock(hg);
                            ok = !outB64.empty();
                        }
                    }
                }
            }
            stm->Release();
        }
    }
    delete dst;
    return ok;
}

// 读取整个文件（超过 maxBytes 只读前面部分）
static bool ReadWholeFileBytes(const std::wstring& path, size_t maxBytes, std::string& out) {
    out.clear();
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }
    if ((size_t)sz > maxBytes) sz = (long long)maxBytes;
    out.resize((size_t)sz);
    size_t rd = (sz > 0) ? fread(&out[0], 1, (size_t)sz, f) : 0;
    fclose(f);
    out.resize(rd);
    return true;
}

// 文本编码探测：UTF-8 BOM / UTF-16LE BOM / 严格 UTF-8 / 回退 ANSI(GBK)
static std::wstring DecodeTextBytes(const std::string& raw) {
    if (raw.empty()) return L"";
    const unsigned char* p = (const unsigned char*)raw.data();
    size_t n = raw.size();
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        int wl = MultiByteToWideChar(CP_UTF8, 0, raw.data() + 3, (int)(n - 3), nullptr, 0);
        std::wstring w((size_t)(wl > 0 ? wl : 0), L'\0');
        if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, raw.data() + 3, (int)(n - 3), &w[0], wl);
        return w;
    }
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        size_t cnt = (n - 2) / 2;
        std::wstring w(cnt, L'\0');
        if (cnt) memcpy(&w[0], raw.data() + 2, cnt * 2);
        return w;
    }
    int wl = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)n, nullptr, 0);
    if (wl <= 0) wl = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)n, nullptr, 0);
    if (wl <= 0) return L"";
    std::wstring w((size_t)wl, L'\0');
    UINT cp = (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(), (int)n, nullptr, 0) > 0)
        ? CP_UTF8 : CP_ACP;
    MultiByteToWideChar(cp, 0, raw.data(), (int)n, &w[0], wl);
    return w;
}

static void ReplaceAllInPlace(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static void AppendUtf8Codepoint(std::string& s, unsigned cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) {
        s += (char)(0xC0 | (cp >> 6));
        s += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += (char)(0xE0 | (cp >> 12));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    } else {
        s += (char)(0xF0 | (cp >> 18));
        s += (char)(0x80 | ((cp >> 12) & 0x3F));
        s += (char)(0x80 | ((cp >> 6) & 0x3F));
        s += (char)(0x80 | (cp & 0x3F));
    }
}

// OOXML → 纯文本：段落/换行标签变 \n，剥掉所有标签，解实体
static std::string XmlToPlainText(const std::string& xml) {
    std::string s = xml;
    const char* breaks[] = { "</w:p>", "</a:p>", "<w:br/>", "<w:br />",
                             "</w:tr>", "</w:tab/>", "</si>" };
    for (const char* b : breaks) ReplaceAllInPlace(s, b, "\n");
    std::string out;
    out.reserve(s.size());
    bool inTag = false;
    for (char c : s) {
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) out += c;
    }
    ReplaceAllInPlace(out, "&lt;", "<");
    ReplaceAllInPlace(out, "&gt;", ">");
    ReplaceAllInPlace(out, "&quot;", "\"");
    ReplaceAllInPlace(out, "&apos;", "'");
    ReplaceAllInPlace(out, "&amp;", "&");     // 必须最后做，避免二次解码
    // 数字实体 &#123; / &#x1F600;
    for (size_t i = 0; i + 3 < out.size();) {
        if (out[i] == '&' && out[i + 1] == '#') {
            size_t semi = out.find(';', i + 2);
            if (semi != std::string::npos && semi - i <= 10) {
                std::string num = out.substr(i + 2, semi - i - 2);
                unsigned cp = 0;
                if (!num.empty() && (num[0] == 'x' || num[0] == 'X'))
                    cp = (unsigned)strtoul(num.c_str() + 1, nullptr, 16);
                else
                    cp = (unsigned)strtoul(num.c_str(), nullptr, 10);
                if (cp > 0) {
                    std::string rep;
                    AppendUtf8Codepoint(rep, cp);
                    out.replace(i, semi - i + 1, rep);
                    i += rep.size();
                    continue;
                }
            }
        }
        ++i;
    }
    // 压掉连续 3 个以上换行
    while (out.find("\n\n\n") != std::string::npos) ReplaceAllInPlace(out, "\n\n\n", "\n\n");
    return out;
}

// 隐藏窗口跑外部程序（tar / pdftotext / whisper），带超时强杀
static bool RunHiddenProcess(const std::wstring& cmdLine, DWORD timeoutMs, DWORD* exitCode) {
    if (exitCode) *exitCode = 1;
    if (cmdLine.empty()) return false;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;                 // CreateProcessW 要求可写缓冲
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    DWORD wr = WaitForSingleObject(pi.hProcess, timeoutMs);
    bool finished = (wr != WAIT_TIMEOUT);
    if (!finished) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    if (exitCode) *exitCode = ec;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished;
}

// 建一个本次专用的临时目录：%TEMP%\MakotoPetAttach\<pid>_<tick>
static std::wstring MakeTempDir() {
    wchar_t tmp[MAX_PATH] = { 0 };
    if (GetTempPathW(MAX_PATH, tmp) == 0) return L"";
    std::wstring base = std::wstring(tmp) + L"MakotoPetAttach";
    CreateDirectoryW(base.c_str(), nullptr);
    // ★同一毫秒内可能连续调用（OCR 转图 + docx 解包），只用 pid+tick 会撞名导致
    //   CreateDirectory 失败、整条解析链报"创建临时目录失败"。加自增序号 + 重试。
    static std::atomic<unsigned> s_seq{ 0 };
    for (int attempt = 0; attempt < 8; ++attempt) {
        unsigned seq = s_seq.fetch_add(1);
        wchar_t sub[96];
        _snwprintf_s(sub, 96, _TRUNCATE, L"\\%lu_%llu_%u",
            (unsigned long)GetCurrentProcessId(), (unsigned long long)GetTickCount64(), seq);
        std::wstring dir = base + sub;
        if (CreateDirectoryW(dir.c_str(), nullptr)) return dir;
    }
    return L"";
}

// 只删自己创建的临时树（绝不碰用户目录）
static void CleanupTempTree(const std::wstring& dir) {
    if (dir.size() < 10) return;                 // 防御：路径过短直接放弃
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CleanupTempTree(p);
            else DeleteFileW(p.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// 在 程序目录 / 程序目录\tools / PATH 里找一个外部工具
static bool FindToolExe(const wchar_t* exeName, std::wstring& outPath) {
    std::wstring dir;
    if (GetExeDir(dir)) {
        std::wstring p = dir + L"\\" + exeName;
        if (PathFileExistsW(p.c_str())) { outPath = p; return true; }
        p = dir + L"\\tools\\" + exeName;
        if (PathFileExistsW(p.c_str())) { outPath = p; return true; }
    }
    wchar_t buf[MAX_PATH] = { 0 };
    if (SearchPathW(nullptr, exeName, nullptr, MAX_PATH, buf, nullptr) > 0) {
        outPath = buf;
        return true;
    }
    return false;
}

static bool ExtractDocxText(const std::wstring& path, std::wstring& out, std::wstring& err) {
    std::wstring tmp = MakeTempDir();
    if (tmp.empty()) { err = L"创建临时目录失败"; return false; }
    wchar_t sys[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    bool ok = false;
    if (!PathFileExistsW(tar.c_str())) {
        err = L"系统缺少 tar.exe（Win10 1803+ 自带），无法解析 docx";
    } else {
        std::wstring cmd = L"\"" + tar + L"\" -x -f \"" + path + L"\" -C \"" + tmp + L"\" word/document.xml";
        DWORD ec = 1;
        if (RunHiddenProcess(cmd, 15000, &ec) && ec == 0) {
            std::string xml;
            std::wstring doc = tmp + L"\\word\\document.xml";
            if (ReadWholeFileBytes(doc, 32u * 1024u * 1024u, xml) && !xml.empty()) {
                out = Utf8ToWideString(XmlToPlainText(xml));
                ok = !out.empty();
                if (!ok) err = L"docx 里没有可用文字（可能只有图片）";
            } else {
                err = L"docx 解包后没有 document.xml";
            }
        } else {
            err = L"docx 解包失败（文件可能损坏或被加密）";
        }
    }
    CleanupTempTree(tmp);
    return ok;
}

static std::vector<std::wstring> ParseSharedStrings(const std::string& xml) {
    std::vector<std::wstring> v;
    size_t pos = 0;
    while ((pos = xml.find("<si>", pos)) != std::string::npos) {
        size_t end = xml.find("</si>", pos);
        if (end == std::string::npos) break;
        std::string inner = xml.substr(pos + 4, end - pos - 4);
        v.push_back(Utf8ToWideString(XmlToPlainText(inner)));
        pos = end + 5;
    }
    return v;
}

// sheet XML → 每行制表符分隔的纯文本（够模型读懂表格内容）
static void ParseSheetXml(const std::string& xml, const std::vector<std::wstring>& shared,
    std::wstring& out, size_t maxChars) {
    size_t pos = 0;
    while ((pos = xml.find("<row", pos)) != std::string::npos) {
        size_t rowEnd = xml.find("</row>", pos);
        if (rowEnd == std::string::npos) break;
        std::string row = xml.substr(pos, rowEnd - pos);
        std::wstring line;
        size_t cp = 0;
        while ((cp = row.find("<c", cp)) != std::string::npos) {
            size_t cEnd = row.find("</c>", cp);
            if (cEnd == std::string::npos) break;
            std::string cell = row.substr(cp, cEnd - cp);
            bool isShared = cell.find("t=\"s\"") != std::string::npos;
            std::wstring val;
            size_t vp = cell.find("<v>");
            if (vp != std::string::npos) {
                size_t ve = cell.find("</v>", vp);
                if (ve != std::string::npos) {
                    std::string num = cell.substr(vp + 3, ve - vp - 3);
                    if (isShared) {
                        int idx = atoi(num.c_str());
                        if (idx >= 0 && idx < (int)shared.size()) val = shared[(size_t)idx];
                    } else {
                        val = Utf8ToWideString(num);
                    }
                }
            } else {
                val = Utf8ToWideString(XmlToPlainText(cell));
            }
            if (!val.empty()) {
                if (!line.empty()) line += L'\t';
                line += val;
            }
            cp = cEnd + 4;
        }
        if (!line.empty()) { out += line; out += L'\n'; }
        if (out.size() > maxChars) break;
        pos = rowEnd + 6;
    }
}

static bool ExtractXlsxText(const std::wstring& path, std::wstring& out, std::wstring& err) {
    std::wstring tmp = MakeTempDir();
    if (tmp.empty()) { err = L"创建临时目录失败"; return false; }
    wchar_t sys[MAX_PATH] = { 0 };
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring tar = std::wstring(sys) + L"\\tar.exe";
    bool ok = false;
    if (!PathFileExistsW(tar.c_str())) {
        err = L"系统缺少 tar.exe（Win10 1803+ 自带），无法解析 xlsx";
    } else {
        // 分两次解：sharedStrings.xml 在纯数字表里可能不存在，
        // 合在一条命令里 tar 会整体报错，连 sheet1 都解不出来。
        std::wstring cmdSs = L"\"" + tar + L"\" -x -f \"" + path + L"\" -C \"" + tmp +
            L"\" xl/sharedStrings.xml";
        std::wstring cmdSheet = L"\"" + tar + L"\" -x -f \"" + path + L"\" -C \"" + tmp +
            L"\" xl/worksheets/sheet1.xml";
        DWORD ec = 1;
        RunHiddenProcess(cmdSs, 15000, &ec);        // 可能没有，忽略退出码
        RunHiddenProcess(cmdSheet, 15000, &ec);
        std::string ssXml, sheetXml;
        ReadWholeFileBytes(tmp + L"\\xl\\sharedStrings.xml", 32u * 1024u * 1024u, ssXml);
        if (!ReadWholeFileBytes(tmp + L"\\xl\\worksheets\\sheet1.xml", 32u * 1024u * 1024u, sheetXml)) {
            err = L"xlsx 里没有 sheet1（多工作表时请另存为 csv）";
        } else {
            std::vector<std::wstring> shared = ParseSharedStrings(ssXml);
            std::wstring body;
            ParseSheetXml(sheetXml, shared, body, 400000);
            if (!body.empty()) {
                out = L"[工作表 sheet1]\n" + body;
                ok = true;
            } else {
                err = L"xlsx 里没有可读的单元格文本";
            }
        }
    }
    CleanupTempTree(tmp);
    return ok;
}

// ==================================================================
//  ★扫描件 OCR：走系统自带的 WinRT OCR（Windows.Media.Ocr），零额外下载。
//   PDF 没有文字层时：pdftoppm 把前 N 页转成 PNG → 逐页 OCR → 拼成文本。
//   注意：WinRT 异步在 MTA 线程完成，UI 线程若是 STA 会被卡死，
//   所以整条 OCR 链路放进独立线程（里面自己 RoInitialize(MTA)）。
// ==================================================================
namespace ocrwin {

using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Storage;
using namespace ABI::Windows::Storage::Streams;
using namespace ABI::Windows::Graphics::Imaging;
using namespace ABI::Windows::Media::Ocr;
using Microsoft::WRL::ComPtr;

static HRESULT WaitAsyncInfo(IAsyncInfo* info, DWORD timeoutMs) {
    if (!info) return E_POINTER;
    ULONGLONG t0 = GetTickCount64();
    for (;;) {
        AsyncStatus st = AsyncStatus::Started;
        HRESULT hr = info->get_Status(&st);
        if (FAILED(hr)) return hr;
        if (st != AsyncStatus::Started)
            return (st == AsyncStatus::Completed) ? S_OK : E_FAIL;
        if (GetTickCount64() - t0 > timeoutMs) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Sleep(10);
    }
}

// WinRT 的 AggregateType 要求 GetResults 收"接口指针"，所以接口类型单独作模板参数
template <typename TAsync, typename TIface>
static HRESULT AwaitOp(IAsyncOperation<TAsync>* op, ComPtr<TIface>& out, DWORD timeoutMs) {
    if (!op) return E_POINTER;
    ComPtr<IAsyncInfo> info;
    HRESULT hr = op->QueryInterface(__uuidof(IAsyncInfo), (void**)info.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = WaitAsyncInfo(info.Get(), timeoutMs);
    if (FAILED(hr)) return hr;
    TIface* raw = nullptr;
    hr = op->GetResults(&raw);
    if (FAILED(hr)) return hr;
    if (!raw) return E_FAIL;
    out.Attach(raw);
    return S_OK;
}

template <typename T>
static HRESULT GetStatics(const wchar_t* cls, REFIID iid, T** out) {
    HSTRING hs = nullptr;
    HRESULT hr = WindowsCreateString(cls, (UINT32)wcslen(cls), &hs);
    if (FAILED(hr)) return hr;
    ComPtr<IActivationFactory> f;
    hr = RoGetActivationFactory(hs, __uuidof(IActivationFactory), (void**)f.GetAddressOf());
    WindowsDeleteString(hs);
    if (FAILED(hr)) return hr;
    return f->QueryInterface(iid, (void**)out);
}

// 识别一张图（PNG/JPG 都行）。调用线程必须已 RoInitialize。
static bool RecognizeFile(const std::wstring& path, std::wstring& out) {
    out.clear();
    ComPtr<IOcrEngineStatics> engStatics;
    if (FAILED(GetStatics(L"Windows.Media.Ocr.OcrEngine", __uuidof(IOcrEngineStatics),
                          (void**)engStatics.GetAddressOf())))
        return false;

    ComPtr<IOcrEngine> engine;
    if (FAILED(engStatics->TryCreateFromUserProfileLanguages(engine.GetAddressOf())) || !engine)
        return false;

    ComPtr<IStorageFileStatics> fileStatics;
    if (FAILED(GetStatics(L"Windows.Storage.StorageFile", __uuidof(IStorageFileStatics),
                          (void**)fileStatics.GetAddressOf())))
        return false;

    ComPtr<IStorageFile> file;
    {
        HSTRING hp = nullptr;
        WindowsCreateString(path.c_str(), (UINT32)path.size(), &hp);
        ComPtr<IAsyncOperation<StorageFile*>> op;
        HRESULT hr = fileStatics->GetFileFromPathAsync(hp, op.GetAddressOf());
        WindowsDeleteString(hp);
        if (FAILED(hr)) return false;
        if (FAILED(AwaitOp(op.Get(), file, 20000)) || !file) return false;
    }

    ComPtr<IRandomAccessStream> stream;
    {
        ComPtr<IAsyncOperation<IRandomAccessStream*>> op;
        if (FAILED(file->OpenAsync(FileAccessMode_Read, op.GetAddressOf()))) return false;
        if (FAILED(AwaitOp(op.Get(), stream, 20000)) || !stream) return false;
    }

    ComPtr<IBitmapDecoderStatics> decStatics;
    if (FAILED(GetStatics(L"Windows.Graphics.Imaging.BitmapDecoder", __uuidof(IBitmapDecoderStatics),
                          (void**)decStatics.GetAddressOf())))
        return false;

    ComPtr<IBitmapDecoder> decoder;
    {
        ComPtr<IAsyncOperation<BitmapDecoder*>> op;
        if (FAILED(decStatics->CreateAsync(stream.Get(), op.GetAddressOf()))) return false;
        if (FAILED(AwaitOp(op.Get(), decoder, 20000)) || !decoder) return false;
    }

    ComPtr<ISoftwareBitmap> bmp;
    {
        ComPtr<IBitmapFrame> frame;
        ComPtr<IAsyncOperation<BitmapFrame*>> op;
        if (FAILED(decoder->GetFrameAsync(0, op.GetAddressOf()))) return false;
        if (FAILED(AwaitOp(op.Get(), frame, 20000)) || !frame) return false;
        ComPtr<IBitmapFrameWithSoftwareBitmap> fwsb;
        if (FAILED(frame.As(&fwsb)) || !fwsb) return false;
        ComPtr<IAsyncOperation<SoftwareBitmap*>> opBmp;
        if (FAILED(fwsb->GetSoftwareBitmapAsync(opBmp.GetAddressOf()))) return false;
        if (FAILED(AwaitOp(opBmp.Get(), bmp, 20000)) || !bmp) return false;
    }

    ComPtr<IOcrResult> result;
    {
        ComPtr<IAsyncOperation<OcrResult*>> op;
        if (FAILED(engine->RecognizeAsync(bmp.Get(), op.GetAddressOf()))) return false;
        if (FAILED(AwaitOp(op.Get(), result, 60000)) || !result) return false;
    }

    HSTRING text = nullptr;
    if (FAILED(result->get_Text(&text)) || !text) return false;
    UINT32 len = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(text, &len);
    out.assign(raw, raw + len);
    WindowsDeleteString(text);

    // OCR 会把每个"词"用空格隔开，中文之间这些空格是多余的，去掉更好读
    std::wstring tidy;
    tidy.reserve(out.size());
    auto isCjk = [](wchar_t c) {
        return (c >= 0x3000 && c <= 0x303F) || (c >= 0x3040 && c <= 0x30FF) ||
               (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
               (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
               (c >= 0xFF00 && c <= 0xFF65);
    };
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == L' ' && !tidy.empty() && i + 1 < out.size() &&
            isCjk(tidy.back()) && isCjk(out[i + 1]))
            continue;
        tidy += out[i];
    }
    out.swap(tidy);
    return !out.empty();
}

} // namespace ocrwin

struct OcrPdfJob {
    std::wstring pdf;
    std::wstring text;
    std::wstring err;
    int maxPages = 3;
    int dpi = 150;
    bool ok = false;
};

static int OcrPageNumOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return 0;
    size_t dash = path.find_last_of(L'-', dot);
    if (dash == std::wstring::npos) return 0;
    return _wtoi(path.substr(dash + 1, dot - dash - 1).c_str());
}

static unsigned __stdcall OcrPdfThreadProc(void* p) {
    OcrPdfJob* job = (OcrPdfJob*)p;
    HRESULT hrInit = RoInitialize(RO_INIT_MULTITHREADED);
    const bool initedHere = SUCCEEDED(hrInit);
    do {
        std::wstring tool;
        if (!FindToolExe(L"pdftoppm.exe", tool)) {
            job->err = L"未找到 pdftoppm.exe（poppler 的 PDF 转图工具）";
            break;
        }
        std::wstring tmp = MakeTempDir();
        if (tmp.empty()) { job->err = L"创建临时目录失败"; break; }
        std::wstring prefix = tmp + L"\\page";
        // ★安全：PDF 路径可能很长（深层目录/长文件名），固定 256 会截断并留下未终止串，
        //   命令拼坏后 OCR 直接失败。改用 _TRUNCATE 保证终止（超长时宁可报错也不越界）。
        wchar_t opt[1024];
        _snwprintf_s(opt, 1024, _TRUNCATE, L" -r %d -png -f 1 -l %d \"%s\" \"%s\"",
                 job->dpi, job->maxPages, job->pdf.c_str(), prefix.c_str());
        DWORD ec = 1;
        bool ran = RunHiddenProcess(L"\"" + tool + L"\"" + opt, 60000, &ec);
        if (!ran) { job->err = L"PDF 转图超时（文件太大或页数太多）"; CleanupTempTree(tmp); break; }

        std::vector<std::wstring> pages;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((tmp + L"\\page*.png").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do { pages.push_back(tmp + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(pages.begin(), pages.end(), [](const std::wstring& a, const std::wstring& b) {
            return OcrPageNumOf(a) < OcrPageNumOf(b);
        });

        std::wstring all;
        for (size_t i = 0; i < pages.size(); ++i) {
            std::wstring t;
            if (ocrwin::RecognizeFile(pages[i], t) && !t.empty()) {
                if (!all.empty()) all += L"\n\n";
                all += t;
            }
        }
        CleanupTempTree(tmp);
        if (all.empty()) { job->err = L"扫描件里没识别出文字（分辨率太低或图片太糊）"; break; }
        job->text = all;
        job->ok = true;
    } while (false);
    if (initedHere) RoUninitialize();
    return 0;
}

// 同步封装：开线程跑 OCR 并等待（OCR 是纯计算，超时强杀不会留副作用）
static bool ExtractScannedPdf(const std::wstring& pdf, int maxPages, int dpi,
                              std::wstring& out, std::wstring& err) {
    OcrPdfJob job;
    job.pdf = pdf;
    job.maxPages = maxPages;
    job.dpi = dpi;
    HANDLE h = (HANDLE)_beginthreadex(nullptr, 0, OcrPdfThreadProc, &job, 0, nullptr);
    if (!h) { err = L"启动 OCR 线程失败"; return false; }
    DWORD w = WaitForSingleObject(h, 120000);
    if (w == WAIT_TIMEOUT) {
        TerminateThread(h, 1);
        CloseHandle(h);
        err = L"OCR 超时：页数太多或机器较慢，可调小「OCR 最多页数」";
        return false;
    }
    CloseHandle(h);
    if (!job.ok) { err = job.err.empty() ? L"OCR 失败" : job.err; return false; }
    out = job.text;
    return true;
}

static bool ExtractPdfText(const std::wstring& path, std::wstring& out, std::wstring& err) {
    std::wstring tool;
    if (!FindToolExe(L"pdftotext.exe", tool)) {
        err = L"未找到 pdftotext.exe：把 poppler 的 pdftotext.exe 放到程序目录即可解析 PDF";
        return false;
    }
    std::wstring tmp = MakeTempDir();
    if (tmp.empty()) { err = L"创建临时目录失败"; return false; }
    std::wstring outTxt = tmp + L"\\out.txt";
    std::wstring cmd = L"\"" + tool + L"\" -enc UTF-8 -layout \"" + path + L"\" \"" + outTxt + L"\"";
    DWORD ec = 1;
    bool ran = RunHiddenProcess(cmd, 30000, &ec);
    std::string bytes;
    bool ok = false;
    if (ran && ec == 0 && ReadWholeFileBytes(outTxt, 16u * 1024u * 1024u, bytes) && !bytes.empty()) {
        out = DecodeTextBytes(bytes);
        ok = !out.empty();
        if (!ok) err = L"PDF 里没有可提取的文字（可能是扫描件）";
    } else if (!ran) {
        err = L"pdftotext 执行超时，PDF 可能过大";
    } else {
        err = L"PDF 解析失败（加密或扫描件），可以试试直接复制文字粘贴";
    }
    CleanupTempTree(tmp);

    // 短文字层兜底：以前"可见字符 < 20 就走 OCR，OCR 失败则整份判失败"，
    // 会把只有一句话的正常 PDF（例如 5 个字的测试页）当成扫描件并丢掉已提取的文字。
    // 现在 OCR 只作增强：pdftotext 已经有文字就绝不丢弃；OCR 更长才替换。
    int visible = 0;
    for (wchar_t c : out) if (!iswspace(c)) ++visible;
    const bool hasText = ok && visible > 0;           // 纯空白不算"有文字"
    if (hasText && visible >= 20) return true;        // 文字层够长，无需额外 OCR
    if (GetIniInt(L"Attach", L"OcrEnable", 1, 0, 1) == 0) {
        if (hasText) return true;                     // OCR 关着也保留已有文字
        err = L"PDF 没有文字层，且「扫描件 OCR」是关闭的";
        return false;
    }
    std::wstring ocrText, ocrErr;
    int pages = GetIniInt(L"Attach", L"OcrMaxPages", 3, 1, 20);
    int dpi = GetIniInt(L"Attach", L"OcrDpi", 150, 72, 400);
    if (ExtractScannedPdf(path, pages, dpi, ocrText, ocrErr)) {
        int ocrVisible = 0;
        for (wchar_t c : ocrText) if (!iswspace(c)) ++ocrVisible;
        if (!hasText || ocrVisible > visible) {
            const wchar_t* label = hasText ? L"（OCR 补充结果，前 " : L"（扫描件 OCR 结果，前 ";
            out = label + std::to_wstring(pages) + L" 页）\n" + ocrText;
        }
        return true;                                  // 无论 OCR 是否更好，已有文字都保留
    }
    if (hasText) return true;                         // OCR 失败也不能丢掉 pdftotext 的文字
    err = L"PDF 没有文字层，OCR 也失败：" + ocrErr;
    return false;
}

// ---------------- 对外入口 ----------------

static std::wstring LowerExt(const std::wstring& path) {
    const wchar_t* dot = wcsrchr(path.c_str(), L'.');
    if (!dot) return L"";
    std::wstring e = dot;
    for (wchar_t& c : e) c = (wchar_t)towlower(c);
    return e;
}

static bool IsImagePath(const std::wstring& path) {
    std::wstring e = LowerExt(path);
    return e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".bmp" ||
        e == L".gif" || e == L".webp" || e == L".tif" || e == L".tiff";
}

static bool IsTextPath(const std::wstring& path) {
    static const wchar_t* kExts[] = {
        L".txt", L".md", L".log", L".json", L".csv", L".tsv", L".ini", L".yaml", L".yml",
        L".xml", L".html", L".htm", L".css", L".js", L".ts", L".jsx", L".tsx", L".py",
        L".cpp", L".c", L".h", L".hpp", L".cs", L".java", L".go", L".rs", L".sql",
        L".bat", L".ps1", L".sh", L".lua", L".kt", L".php", L".rb", L".toml", L".conf"
    };
    std::wstring e = LowerExt(path);
    for (const wchar_t* x : kExts) if (e == x) return true;
    return false;
}

static std::wstring FileNameOf(const std::wstring& path) {
    const wchar_t* base = wcsrchr(path.c_str(), L'\\');
    if (!base) base = wcsrchr(path.c_str(), L'/');
    return base ? std::wstring(base + 1) : path;
}

// 文本落进 JSON 前先清洗：控制字符会让严格的网关直接 400；
// 同时把 CRLF 统一成 LF、Tab 换成 4 空格（JSON 里不能出现裸 Tab）。
static void SanitizeAttachmentText(std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 32);
    for (wchar_t c : s) {
        if (c == L'\r') continue;
        if (c == L'\n') { out += L'\n'; continue; }
        if (c == L'\t') { out += L"    "; continue; }
        if (c < 0x20 || c == 0x7F) continue;
        out += c;
    }
    while (out.find(L"\n\n\n") != std::wstring::npos)
        out.replace(out.find(L"\n\n\n"), 3, L"\n\n");
    s.swap(out);
}

// 慢路径：PDF 要起外部进程 + 扫描件 OCR（最长 120 秒），docx/xlsx 要解 zip。
// 这些必须放到后台线程，否则挂上一个大文件时整个聊天窗会僵住。
static bool IsSlowAttachPath(const std::wstring& path) {
    std::wstring e = LowerExt(path);
    return e == L".pdf" || e == L".docx" || e == L".xlsx";
}

// 真正解析内容：图片编码 / 文本解码 / PDF 文本与 OCR / Office 解包。
// 与 UI 无关（只读配置、只碰临时目录），可以在后台线程调用。
static bool ParseAttachmentContent(Attachment& a, std::wstring& err) {
    const std::wstring path = a.path;
    const size_t fsize = a.bytes;
    std::wstring e = LowerExt(path);
    int maxSide = GetIniInt(L"Attach", L"ImageMaxSide", 1280, 512, 2048);
    int maxChars = GetIniInt(L"Attach", L"TextMaxChars", 12000, 1000, 40000);

    if (IsImagePath(path)) {
        if (fsize > ATTACH_IMG_MAX_BYTES) { err = L"图片超过 8MB，请先压缩"; return false; }
        Bitmap* bmp = Bitmap::FromFile(path.c_str());
        if (!bmp || bmp->GetLastStatus() != Ok) {
            delete bmp;
            err = L"图片无法解码：" + a.name + L"（WebP/TIFF 可能缺系统解码器，请先转成 PNG/JPG）";
            return false;
        }
        bool ok = EncodeBitmapToJpegBase64(bmp, maxSide, a.b64);
        delete bmp;
        if (!ok) { err = L"图片编码失败：" + a.name; return false; }
        a.kind = ATTACH_KIND_IMAGE;
        a.mime = "image/jpeg";
        a.ok = true;
    } else if (IsTextPath(path)) {
        if (fsize > ATTACH_TEXT_MAX_BYTES) { err = L"文本文件超过 2MB，请先拆分"; return false; }
        std::string bytes;
        if (!ReadWholeFileBytes(path, ATTACH_TEXT_MAX_BYTES, bytes)) { err = L"读取失败：" + a.name; return false; }
        std::wstring text = DecodeTextBytes(bytes);
        if (text.empty()) { err = L"文件是空的：" + a.name; return false; }
        bool truncated = false;
        if ((int)text.size() > maxChars) { text.resize((size_t)maxChars); truncated = true; }
        if (truncated) text += L"\n…（文件较长，已截断到 " + std::to_wstring(maxChars) + L" 字）";
        a.kind = ATTACH_KIND_TEXT;
        a.text = text;
        a.ok = true;
    } else if (e == L".pdf") {
        if (fsize > ATTACH_MAX_BYTES) { err = L"PDF 超过 20MB"; return false; }
        std::wstring text, why;
        if (!ExtractPdfText(path, text, why)) { err = why; return false; }
        if ((int)text.size() > maxChars) text.resize((size_t)maxChars);
        a.kind = ATTACH_KIND_TEXT;
        a.text = text;
        a.ok = true;
    } else if (e == L".docx") {
        if (fsize > ATTACH_MAX_BYTES) { err = L"文档超过 20MB"; return false; }
        std::wstring text, why;
        if (!ExtractDocxText(path, text, why)) { err = why; return false; }
        if ((int)text.size() > maxChars) text.resize((size_t)maxChars);
        a.kind = ATTACH_KIND_TEXT;
        a.text = text;
        a.ok = true;
    } else if (e == L".xlsx") {
        if (fsize > ATTACH_MAX_BYTES) { err = L"表格超过 20MB"; return false; }
        std::wstring text, why;
        if (!ExtractXlsxText(path, text, why)) { err = why; return false; }
        if ((int)text.size() > maxChars) text.resize((size_t)maxChars);
        a.kind = ATTACH_KIND_TEXT;
        a.text = text;
        a.ok = true;
    } else {
        err = L"不支持的类型：" + (e.empty() ? L"(无扩展名)" : e) +
            L"（支持 图片 / txt·md·代码 / pdf / docx / xlsx）";
        return false;
    }

    if (a.ok && a.kind != ATTACH_KIND_IMAGE && !a.text.empty())
        SanitizeAttachmentText(a.text);
    return a.ok;
}

// ---------- 后台解析任务：主线程只放占位，结果用 id 回填 ----------
struct AttachParseJob {
    int        id = 0;
    Attachment att;
};
struct AttachParseResult {
    int         id = 0;
    Attachment  att;
    std::wstring err;
};
static std::atomic<int> g_attachIdSeq{ 0 };
static std::mutex g_attachDoneMtx;
static std::vector<AttachParseResult> g_attachDone;   // 后台写，主线程 drain

static unsigned __stdcall AttachParseThreadProc(void* p) {
    AttachParseJob* job = (AttachParseJob*)p;
    AttachParseResult r;
    r.id = job->id;
    r.att = job->att;
    std::wstring err;
    if (!ParseAttachmentContent(r.att, err)) { r.err = err; r.att.ok = false; }
    r.att.loading = false;
    r.att.id = job->id;
    delete job;
    {
        std::lock_guard<std::mutex> lk(g_attachDoneMtx);
        g_attachDone.push_back(std::move(r));
    }
    // 通知主线程回填。两个窗口都不在时结果留在队列里，下次打开聊天窗再 drain。
    HWND target = (g_hChatWnd && IsWindow(g_hChatWnd)) ? g_hChatWnd : g_hwnd;
    if (target && IsWindow(target)) PostMessageW(target, WM_APP_ATTACH_READY, 0, 0);
    return 0;
}

static bool AttachHasLoading() {
    for (const Attachment& a : g_pendingAttachments) if (a.loading) return true;
    return false;
}

// 把本地文件变成待发送附件（按扩展名分派），失败时 err 给玩家看的原因
static bool AddAttachmentFromPath(const std::wstring& path, std::wstring& err) {
    int maxFiles = GetIniInt(L"Attach", L"MaxFiles", 4, 1, ATTACH_MAX_FILES);
    if ((int)g_pendingAttachments.size() >= maxFiles) {
        err = L"单次最多 " + std::to_wstring(maxFiles) + L" 个附件";
        return false;
    }
    if (!PathFileExistsW(path.c_str())) { err = L"文件不存在：" + path; return false; }

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        err = L"读不到文件属性：" + FileNameOf(path);
        return false;
    }
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        err = L"暂不支持文件夹，请选择具体文件";
        return false;
    }
    const ULONGLONG fsize = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    const std::wstring ext = LowerExt(path);
    const bool isImage = IsImagePath(path);
    const bool isText = IsTextPath(path);
    const bool isDoc = IsSlowAttachPath(path);
    if (!isImage && !isText && !isDoc) {
        err = L"不支持的类型：" + (ext.empty() ? L"(无扩展名)" : ext) +
            L"（支持 图片 / txt·md·代码 / pdf / docx / xlsx）";
        return false;
    }
    // 体积先按类型挡掉，别等解析到一半才发现太大
    if (isImage && fsize > ATTACH_IMG_MAX_BYTES) { err = L"图片超过 8MB，请先压缩"; return false; }
    if (isText && fsize > ATTACH_TEXT_MAX_BYTES) { err = L"文本文件超过 2MB，请先拆分"; return false; }
    if (isDoc && fsize > ATTACH_MAX_BYTES) { err = L"文件超过 20MB"; return false; }

    Attachment a;
    a.path = path;
    a.name = FileNameOf(path);
    a.bytes = (size_t)fsize;
    a.kind = isImage ? ATTACH_KIND_IMAGE : ATTACH_KIND_TEXT;

    if (isDoc) {
        // 先占位：玩家马上能看到附件条上的「解析中…」，UI 线程立刻返回
        a.loading = true;
        a.id = ++g_attachIdSeq;
        g_pendingAttachments.push_back(a);
        RefreshAttachStrip();
        AttachParseJob* job = new AttachParseJob();
        job->id = a.id;
        job->att = a;
        uintptr_t th = _beginthreadex(nullptr, 0, AttachParseThreadProc, job, 0, nullptr);
        if (!th) {
            // 线程开不出来（句柄耗尽）：退回同步解析，宁可卡一下也别丢附件
            delete job;
            Attachment& back = g_pendingAttachments.back();
            back.loading = false;
            if (!ParseAttachmentContent(back, err)) {
                g_pendingAttachments.pop_back();
                RefreshAttachStrip();
                return false;
            }
            RefreshAttachStrip();
        }
        PetDbgW((L"[ATTACH] 已加入（后台解析）：" + a.name + L"\n").c_str());
        return true;
    }

    if (!ParseAttachmentContent(a, err)) return false;
    g_pendingAttachments.push_back(a);
    PetDbgW((L"[ATTACH] 已加入：" + a.name + L"\n").c_str());
    return true;
}

// 语音转写结果作为附件（音频本体不发送，模型只吃文字）
static bool AddVoiceAttachment(const std::wstring& wavPath, const std::wstring& transcript,
    size_t bytes, std::wstring& err) {
    int maxFiles = GetIniInt(L"Attach", L"MaxFiles", 4, 1, ATTACH_MAX_FILES);
    if ((int)g_pendingAttachments.size() >= maxFiles) {
        err = L"单次最多 " + std::to_wstring(maxFiles) + L" 个附件";
        return false;
    }
    std::wstring text = transcript;
    if (text.empty()) { err = L"没听清，换个近一点的距离再说一次？"; return false; }
    Attachment a;
    a.kind = ATTACH_KIND_VOICE;
    a.path = wavPath;
    a.name = L"语音";
    a.text = text;
    a.bytes = bytes;
    a.ok = true;
    g_pendingAttachments.push_back(a);
    return true;
}

// 历史里只写占位符，绝不写正文/base64
static std::wstring BuildAttachmentHistoryTag(const std::vector<Attachment>& atts) {
    std::wstring tag;
    for (const Attachment& a : atts) {
        if (!a.ok) continue;
        if (a.kind == ATTACH_KIND_IMAGE)      tag += L"[图片 " + a.name + L"] ";
        else if (a.kind == ATTACH_KIND_VOICE) tag += L"[语音] ";
        else                                  tag += L"[文件 " + a.name + L"] ";
    }
    return tag;
}

// 拼 user 消息的 content：无图 → 字符串（老格式）；有图 → content 数组
static std::wstring BuildUserContentJson(const std::wstring& userText,
    const std::vector<Attachment>& atts) {
    std::wstring textPart = userText;
    std::wstring imageParts;
    int imageCount = 0;
    for (const Attachment& a : atts) {
        if (!a.ok) continue;
        if (a.kind == ATTACH_KIND_IMAGE && !a.b64.empty()) {
            ++imageCount;
            imageParts += L",{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,"
                + Utf8ToWideString(a.b64) + L"\"}}";
        } else if (!a.text.empty()) {
            if (a.kind == ATTACH_KIND_VOICE) {
                textPart += L"\n\n【语音消息转写】\n" + a.text;
            } else {
                textPart += L"\n\n<file name=\"" + a.name + L"\" chars=\"" +
                    std::to_wstring(a.text.size()) + L"\">\n" + a.text + L"\n</file>";
            }
        }
    }
    if (imageCount == 0)
        return L"\"" + EscapeJsonW(textPart) + L"\"";
    // 提醒模型这轮真的有图，避免它把图片当装饰忽略掉
    textPart = L"（随消息附上 " + std::to_wstring(imageCount) +
        L" 张图片，请结合图片内容理解主人的话）\n" + textPart;
    std::wstring out;
    out.reserve(textPart.size() * 4 + imageParts.size() + 64);
    out = L"[{\"type\":\"text\",\"text\":\"" + EscapeJsonW(textPart) + L"\"" + imageParts + L"}]";
    return out;
}

// 纯文本版本的请求体（图片被网关拒绝时自动降级用）
static std::wstring BuildUserContentTextOnly(const std::wstring& userText,
    const std::vector<Attachment>& atts) {
    std::wstring textPart = userText;
    for (const Attachment& a : atts) {
        if (!a.ok) continue;
        if (a.kind == ATTACH_KIND_IMAGE) {
            textPart += L"\n\n[图片 " + a.name + L"：当前模型不支持看图，无法读取内容]";
        } else if (a.kind == ATTACH_KIND_VOICE) {
            textPart += L"\n\n【语音消息转写】\n" + a.text;
        } else if (!a.text.empty()) {
            textPart += L"\n\n<file name=\"" + a.name + L"\">\n" + a.text + L"\n</file>";
        }
    }
    return L"\"" + EscapeJsonW(textPart) + L"\"";
}

// proactive=true 表示这是定时主动搭话：用专门的开场白 prompt、不写历史、
// 结果走气泡而不是聊天窗口打字机；hwnd 仍是主窗口，用于投递回主线程。
struct LLMThreadParam {
    HWND hwnd;
    std::wstring input;
    bool proactive = false;
    // ★触摸互动：true 表示这次请求由"主人摸了某部位"触发。
    //   touchPhrase 是预先生成的自然语言（如"主人摸了摸你的头"），
    //   放在字符串里传进来，避免 LLM 线程（可能在 USE_LIVE2D=0 下编译）依赖部位枚举。
    bool touch = false;
    std::wstring touchPhrase;
    // 会话序号：本次请求的 id。主线程只接受等于"当前期望序号"的回复。
    int session = 0;
    // ★附件：本轮随消息发送的图片/文件/语音转写（主线程填好，线程内只读）
    std::vector<Attachment> attachments;
    std::wstring historyTag;      // 历史占位符前缀，如 "[图片 a.jpg] "
};

// 聊天收尾消息：携带会话序号 + 本轮完整回复。
//   LLM 线程发 WM_STREAM_DONE 时 new 出来，主线程用完 delete。
//   历史落盘已挪到 LLM 线程（见 LLMThreadProc），UI 这里只负责显示与状态复位。
struct StreamDoneMsg {
    int session = 0;
    std::wstring reply;
};

static unsigned __stdcall LLMThreadProc(void* param) {
    if (!param) return 0;                       // 防御空指针：静默返回，避免解引用崩溃
    LLMThreadParam* p = (LLMThreadParam*)param;
    HWND hwnd = p->hwnd;
    std::wstring userInput = p->input;
    bool proactive = p->proactive;
    bool touch = p->touch;
    std::wstring touchPhrase = p->touchPhrase;
    int session = p->session;                   // 本次请求的会话序号
    // ★附件在 delete p 之前先搬出来（move 避免复制几百 KB 的 base64）
    std::vector<Attachment> atts = std::move(p->attachments);
    std::wstring attachTag = p->historyTag;
    delete p;

    // ★触摸互斥标志用 RAII 清理：本函数有 7+ 处提前 return（网络失败等），
    //   任何一条路径忘记手动复位都会让 g_touchRunning 永久为 true、再也摸不动。
    struct TouchGuard {
        volatile bool* flag;
        explicit TouchGuard(volatile bool* f) : flag(f) {}
        ~TouchGuard() { if (flag) *flag = false; }
    } touchGuard(touch ? &g_touchRunning : nullptr);

    // 统一错误上报，保证触摸触发的请求也能正确反馈。
    //   错误路径统一发 WM_APPEND_TEXT/WM_STREAM_DONE 给 hwnd ——
    //   聊天模式 hwnd 是聊天窗口（有这两个 case），没问题；但触摸/主动搭话模式
    //   hwnd 是主窗口，主窗口根本没有这两个 case → 错误被 DefWindowProc 静默丢弃，
    //   界面永远停在占位气泡"（……）"，看起来就像"连不上 LLM"。
    //   这里按模式分流：触摸/搭话的错误发成对应气泡，聊天仍走原消息。
    // hardError 标记"错误已就地展示给用户"，避免后续无谓重试 +
    //   再叠一条通用"解析回复失败"提示（网络失败时两层提示会同时冒出来）。
    bool hardError = false;
    auto reportError = [&](const wchar_t* text) {
        hardError = true;
        if (touch || proactive) {
            HWND mh = (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd : hwnd;
            UINT um = touch ? WM_APP_TOUCH_REPLY : WM_APP_PROACTIVE;
            auto* b = new std::wstring(L"（" + std::wstring(text) + L"）");
            if (!PostMessageW(mh, um, (WPARAM)PetEmotion::Base, (LPARAM)b)) delete b;
        } else {
            // 聊天模式错误要完整收尾，且顺序必须正确——
            //   ① 先删掉"茉子：思考中..."占位（否则稍后的 WM_REMOVE_THINKING 会把
            //      下面这条错误正文一起擦掉，用户只看到"（未收到回复）"）；
            //   ② 再追加错误正文；③ 发带会话号的 StreamDoneMsg 复位按钮/状态。
            //   发 lParam=0 的旧写法会被 if(done) 判空跳过，导致发送按钮永久禁用。
            PostMessageW(hwnd, WM_REMOVE_THINKING, 0, 0);
            auto* m = new std::wstring(text);
            if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)m)) delete m;
            auto* done = new StreamDoneMsg();
            done->session = session;
            done->reply.clear();
            if (!PostMessageW(hwnd, WM_STREAM_DONE, 0, (LPARAM)done)) delete done;
        }
    };

    // 诊断：DebugView 搜 [TOUCH] 可看清每次触摸请求的关键节点
    if (touch) PetDbgW(L"[TOUCH] 线程已启动，收到触摸事件描述\n");

    std::wstring apiUrl = GetLLMConfig(L"ApiUrl");
    std::wstring apiKey = GetLLMConfig(L"ApiKey");
    std::wstring model = GetLLMConfig(L"Model");

    if (apiUrl.empty() || apiKey.empty()) {
        reportError(L"请在 llm.ini 中配置 ApiUrl 和 ApiKey");
        return 0;
    }

    URL_COMPONENTS urlComp = { sizeof(urlComp) };
    urlComp.dwSchemeLength = (DWORD)-1;
    urlComp.dwHostNameLength = (DWORD)-1;
    urlComp.dwUrlPathLength = (DWORD)-1;
    std::wstring urlBuf = apiUrl;                 // 放堆上，避免函数栈占用过大
    if (urlBuf.empty()) { reportError(L"ApiUrl 为空"); return 0; }
    if (!WinHttpCrackUrl(&urlBuf[0], (DWORD)urlBuf.size(), 0, &urlComp)) {
        reportError(L"ApiUrl 格式错误，请检查 llm.ini");
        return 0;
    }

    wchar_t hostName[256] = { 0 };
    if (urlComp.lpszHostName && urlComp.dwHostNameLength > 0)
        wcsncpy_s(hostName, urlComp.lpszHostName, min(urlComp.dwHostNameLength, 255u));
    wchar_t urlPath[256] = { 0 };
    if (urlComp.lpszUrlPath && urlComp.dwUrlPathLength > 0)
        wcsncpy_s(urlPath, urlComp.lpszUrlPath, min(urlComp.dwUrlPathLength, 255u));

    // 用缓存的 prompt（启动时读一次），避免每轮全量读盘。
    std::wstring prompt = GetCachedPrompt();
    std::wstring history = LoadChatHistory();
    size_t histMax = (size_t)GetLLMConfigInt(L"HistoryMaxChars", 4000, 500, 60000);
    if (history.size() > histMax) {
        // 按轮次/段落边界截断：从字符切点向后（往字符串前面）找最近的段落起点
        //   （"\r\n\r\n" 空行），保证不会从半句话/半轮对话中间切掉。
        //   历史文件格式是 "主人：...\r\n\r\n茉子：...\r\n\r\n"，段间必有空行。
        //   若找不到空行（旧格式/单段），退化为原 UTF-16 代理对齐逻辑。
        size_t cut = history.size() - histMax;
        size_t para = history.rfind(L"\r\n\r\n", cut);
        if (para != std::wstring::npos) {
            // 对齐到空行之后（即下一段起点）。空行本身 4 字符，
            // 段落起点 = para + 4；若该点仍是空行开头，再往后推。
            cut = para + 4;
        } else {
            // 兜底：找不到段落边界，保持原 UTF-16 代理对对齐
            if (cut < history.size() && history[cut] >= 0xDC00 && history[cut] <= 0xDFFF) ++cut;
        }
        history = history.substr(cut);
    }
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

    // 采样参数：从 llm.ini 读，没写就用默认值
    double temperature = GetLLMConfigDouble(L"Temperature", 0.9, 0.0, 2.0);
    double topP = GetLLMConfigDouble(L"TopP", 0.95, 0.0, 1.0);
    double presencePen = GetLLMConfigDouble(L"PresencePenalty", 0.6, -2.0, 2.0);
    double freqPen = GetLLMConfigDouble(L"FrequencyPenalty", 0.3, -2.0, 2.0);
    int    maxTokens = GetLLMConfigInt(L"MaxTokens", 3000, 256, 16384);
    int    enableThink = GetLLMConfigInt(L"EnableThinking", 0, 0, 1);

    // ★ 这三个开关决定要不要把非通用字段发给网关。OpenAI 官方、DeepSeek、Kimi、智谱
    //   以及多数中转站都不认 chat_template_kwargs；部分网关也不支持 response_format。
    //   硬编码全发会被不认该字段的网关返回 400、取不到 content。
    int    usePenalty = GetLLMConfigInt(L"UsePenalty", 1, 0, 1);
    int    useFmt = GetLLMConfigInt(L"UseResponseFormat", 1, 0, 1);
    int    useKw = GetLLMConfigInt(L"UseChatTemplateKwargs", 0, 0, 1);

    // 开启 response_format 时，补一段"字段缺失也必须返回完整对象"的硬约束，
    //   降低模型"漏字段/格式跑偏"的概率（配合下方的重试兜底）。
    if (useFmt)
        prompt = prompt + L"\n\n【输出完整性 - 关键】无论任何情况，你都必须返回一个完整的 JSON 对象，"
            L"包含 zh 与 ja 两个字段（都不能为空字符串），可选字段缺失时省略即可，"
            L"但绝不能省略 zh/ja、不能输出 JSON 以外的任何文字或代码块。";

    // ★ 长文本模式：放宽上限 + 追加一次性指令
    bool longForm = IsLongFormRequest(userInput);
    if (longForm) {
        int longTok = GetLLMConfigInt(L"LongMaxTokens", 8000, 256, 32768);
        if (longTok > maxTokens) maxTokens = longTok;
        prompt = prompt + L"\n\n" + LONG_INSTRUCT;
        PetDbgW(L"[LLM] 长文本模式：已放宽 max_tokens 并追加长篇指令\n");
    }
    // 导演指令：默认开启，仅追加"演出字段"说明，不改人设/世界观/口吻。
    //   llm.ini [LLM] DirectorEnable=0 可完全关闭，行为与改造前一致。
    bool directorOn = GetLLMConfigInt(L"DirectorEnable", 1, 0, 1) != 0;
    if (directorOn && !proactive && !touch)
        prompt = prompt + DIRECTOR_INSTRUCT;   // 主动搭话/触摸走各自简短 prompt，不堆演出字段

    // ★触摸互动：把"主人摸了摸你的头"/"主人扯了扯你的裙子"这件事直接讲给模型，让它自己决定表情/台词/动作。
    //   八部位（头/脸/手/胸/腹/裙/腿/脚）各有一句专属描述，经 touchPhrase 动态注入，与人设 prompt 无关。
    //   触摸复用导演协议（emotion/action/gaze 由模型输出），因此在 touch 分支单独补一段
    //   简短的演出提示；保留历史上下文，让她能结合你们的相处氛围自然反应。
    if (touch) {
        if (directorOn)
            prompt = prompt + DIRECTOR_INSTRUCT;
        userInput = std::wstring(L"【触摸互动】") + touchPhrase + L"。"
            + L"请就在此时此地，自然地对这个触碰做出反应：先用一小句描写你的动作或神态（放在中文全角括号里，"
            + L"例如「（脸微微一红，往旁边躲了躲）」），再接上你说的话。全程保持茉子本来的性格和口吻，"
            + L"不要提到『触摸互动』『主人摸了你』这类旁白字眼，仿佛这真的正在发生。"
            + L"只回复这一瞬间的反应，一到两句话即可，不要写成一整段长文。"
            + L"仍然严格按 JSON 格式回复（zh/ja/emotion，可附带演出字段）。";
        // 触摸回复默认给到 1200：思考型模型（如 agnes-2.5-flash 带 thinking）
        // 会先在 reasoning 里吃掉不少 token，400 太小容易出现 content 为空 → "连不上"的假象。
        int touchTok = GetLLMConfigInt(L"TouchMaxTokens", 1200, 256, 4000);
        if (touchTok < maxTokens) maxTokens = touchTok;
        PetDbgW(L"[LLM] 触摸互动模式：已注入触摸事件\n");
    }

    // 主动搭话：替换 user 输入，但不再完全清掉历史上下文。
    //   注入"最近 1-2 轮对话摘要"（截断到 400 字以内）+ 已有的 memory 锚点，
    //   prompt 里明确"可以顺势接上刚才的话题，但不许重复原话"。
    //   这样既不会像全量历史那样把它带成复读，也不会像全清那样机械搭话。
    if (proactive) {
        // 结合时段、当前动作、陪伴时长拼一句自然的开场指令
        std::wstring state;
        if (g_bDragging)                    state = L"你正被主人拖来拖去";
        else if (g_currentState == AnimState::Run)   state = L"你正在跑动";
        else if (g_currentState == AnimState::Think) state = L"你正在发呆思考";
        else                                state = L"你正安静地待在桌面上";
        wchar_t hours[48];
        int hm = (int)(g_stat.totalSeconds / 3600.0);
        swprintf(hours, 48, L"已经陪了主人 %d 小时", hm);
        userInput = std::wstring(L"【主动搭话】现在是") + TimeOfDayLabel() + L"，" + state + L"，"
            + hours + L"。主人有一阵子没理你了。请主动开口说一句话，自然一点、贴合当下的时间和你正在做的事，"
            + L"可以关心主人、撒个娇、分享一个念头或问个小问题；不要提到『主动搭话』或『系统』这类字眼，"
            + L"就像你真的突然想说话一样。仍然严格按 JSON 格式回复（zh/ja/emotion）。";
        // 主动搭话带最近 1-2 轮对话摘要（截断到 400 字以内），而不是全量历史
        //   让茉子可以顺势接上刚才的话题，但 prompt 明确要求不重复原话。
        int summaryMax = GetLLMConfigInt(L"ProactiveSummaryChars", 400, 0, 2000);
        if (summaryMax > 0 && !history.empty()) {
            // 目标是从"末尾往前 summaryMax 字"这段里取完整段落。
            //   原写法 rfind(L"\r\n\r\n", keep) 的起始位置是 400（从头数），
            //   历史一长就落在开头，跟"最近对话"完全无关，且仍会从半句切断。
            const size_t tailStart = (history.size() > (size_t)summaryMax)
                ? history.size() - (size_t)summaryMax : 0;
            size_t pstart = tailStart;
            // 找 tailStart 之前（含）最近的段落边界，从边界后开始取——
            // 这样窗口第一句是完整的一段，不会从半句开头（同 M1 对齐思路）。
            size_t pcut = history.rfind(L"\r\n\r\n", tailStart);
            if (pcut != std::wstring::npos && pcut + 4 < history.size())
                pstart = pcut + 4;
            std::wstring summary = history.substr(pstart);
            if (!summary.empty()) {
                // 单段本身可能仍超长：从这段自己的末尾再截一次 summaryMax。
                if (summary.size() > (size_t)summaryMax)
                    summary = summary.substr(summary.size() - (size_t)summaryMax);
                contextStr += L",{\"role\":\"system\",\"content\":\""
                    + EscapeJsonW(L"【以下是你最近 1-2 轮和主人的对话片段，仅供了解你们刚才聊了什么，"
                        L"你可以顺势接上话题、回应她的关心，但严禁照抄原文里的句子，"
                        L"也不要提到『刚才』『上次』这类字眼——就像你真的突然想起什么一样】\n")
                    + EscapeJsonW(summary) + L"\"}";
            }
        }
        int shortTok = GetLLMConfigInt(L"ProactiveMaxTokens", 300, 128, 2000);
        if (shortTok < maxTokens) maxTokens = shortTok;
        PetDbgW(L"[LLM] 主动搭话模式：已注入最近对话摘要 + memory 锚点\n");
    }

    // 长期记忆注入：放在 proactive 清历史之后，避免被 clear 掉。
    //   作为独立 system 消息，和对话历史并列，不改 prompt 主体/人设/口吻。
    //   config 开关 MemoryEnable 控制（默认开）；主动搭话也带上，方便她"想起"你。
    if (GetLLMConfigInt(L"MemoryEnable", 1, 0, 1)) {
        std::wstring mem = BuildMemoryContext();
        if (!mem.empty()) {
            contextStr += L",{\"role\":\"system\",\"content\":\""
                + EscapeJsonW(L"【以下是你长久记住的关于主人的事，请自然地记在心里、在合适时提起，"
                    L"但不要生硬地一条条复述，更不要提到『记忆』『系统』这些字眼】\n")
                + EscapeJsonW(mem) + L"\"}";
        }
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

    // ★附件：本轮 user content 可能是字符串（无图）或数组（有图），统一由
    //   BuildUserContentJson 拼；有图且网关不认时再准备一份纯文本兜底体。
    bool hasImage = false;
    for (const Attachment& a : atts)
        if (a.ok && a.kind == ATTACH_KIND_IMAGE && !a.b64.empty()) { hasImage = true; break; }
    // 某些网关 response_format 与图片不能共存，可 [Attach] VisionForceJson=0 关掉
    bool visionKeepFmt = GetIniInt(L"Attach", L"VisionForceJson", 1, 0, 1) != 0;
    bool fmtThis = useFmt && (!hasImage || visionKeepFmt);

    const std::wstring userJson = BuildUserContentJson(userInput, atts);
    auto buildBody = [&](const std::wstring& ujson) -> std::wstring {
        std::wstring b = L"{\"model\":\"" + model +
            L"\",\"messages\":[{\"role\":\"system\",\"content\":\"" + EscapeJsonW(prompt) +
            L"\"}" + contextStr + L",{\"role\":\"user\",\"content\":" + ujson +
            L"}],\"stream\":false,\"max_tokens\":" + std::to_wstring(maxTokens) +
            L",\"temperature\":" + numToW(temperature) +
            L",\"top_p\":" + numToW(topP);
        // ★ 非通用字段按开关拼接：关掉就一个字都不发，网关不会再因为不认识而报 400
        if (usePenalty) {
            b += L",\"presence_penalty\":" + numToW(presencePen) +
                L",\"frequency_penalty\":" + numToW(freqPen);
        }
        if (fmtThis) b += L",\"response_format\":{\"type\":\"json_object\"}";
        if (useKw) {
            b += L",\"chat_template_kwargs\":{\"enable_thinking\":";
            b += (enableThink ? L"true" : L"false");
            b += L"}";
        }
        b += L"}";
        return b;
    };
    std::wstring jsonBodyW = buildBody(userJson);
    std::wstring jsonBodyTextOnly;
    if (hasImage) jsonBodyTextOnly = buildBody(BuildUserContentTextOnly(userInput, atts));

    // 重试封装：把"发一次请求"做成可重复的块。
    //   解析失败（取不到 zh）时自动重试一次，重试体里追加更强的格式指令。
    int httpStatus = 0;                 // 最近一次 HTTP 状态（供下方兜底提示）
    auto doOneRequest = [&](const std::wstring& body, int attempt)
        -> std::pair<std::string, std::string> {        // { fullResponse, serverErr }
        std::string bodyA = WideStringToUtf8(body);
        HINTERNET hSession = WinHttpOpen(L"MakotoPet/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
        if (!hSession) { if (attempt == 0) reportError(L"网络初始化失败（WinHttpOpen）"); return { std::string(), std::string() }; }
        HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            if (attempt == 0) reportError(L"无法连接服务器，请检查 ApiUrl");
            return { std::string(), std::string() };
        }
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", urlPath, NULL, NULL, NULL,
            urlComp.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            if (attempt == 0) reportError(L"创建请求失败（WinHttpOpenRequest）");
            return { std::string(), std::string() };
        }
        std::wstring authHeader = L"Authorization: Bearer " + apiKey;
        WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json; charset=utf-8", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        {
            char dbg[320];
            _snprintf_s(dbg, sizeof dbg, _TRUNCATE,
                "[LLM] send#%d: touch=%d proactive=%d model=[%s] max_tokens=%d bodyLen=%u\n",
                attempt, touch ? 1 : 0, proactive ? 1 : 0,
                WideStringToUtf8(model).c_str(), maxTokens, (unsigned)bodyA.length());
            PetDbgA(dbg);
        }
        if (!WinHttpSendRequest(hRequest, NULL, 0, (LPVOID)bodyA.c_str(), (DWORD)bodyA.length(), (DWORD)bodyA.length(), 0)) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            if (attempt == 0) reportError(L"发送请求失败（WinHttpSendRequest）");
            return { std::string(), std::string() };
        }
        if (!WinHttpReceiveResponse(hRequest, NULL)) {
            WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
            if (attempt == 0) reportError(L"接收响应失败（WinHttpReceiveResponse）：多为代理认证失败或网络被拦截");
            return { std::string(), std::string() };
        }
        DWORD code = 0, cs = sizeof(code);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &code, &cs, WINHTTP_NO_HEADER_INDEX))
            httpStatus = (int)code;      // [&] 捕获，写回外层 httpStatus
        // 响应体设 32MB 上限并按 4MB 分块读取，避免异常网关的超大响应吃满内存。
        const size_t kMaxResponse = 32u * 1024u * 1024u;
        std::string fullResponse;
        DWORD dwSize = 0, dwDownloaded = 0;
        for (;;) {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
            if (fullResponse.size() >= kMaxResponse) {
                PetDbgW(L"[LLM] 响应超过 32MB，已停止读取（避免内存耗尽）\n");
                break;
            }
            if (dwSize > 4u * 1024u * 1024u) dwSize = 4u * 1024u * 1024u;
            std::vector<char> buffer(dwSize);
            if (!WinHttpReadData(hRequest, buffer.data(), dwSize, &dwDownloaded) || dwDownloaded == 0) break;
            fullResponse.append(buffer.data(), dwDownloaded);
        }
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        {
            char dbg[192];
            _snprintf_s(dbg, sizeof dbg, _TRUNCATE,
                "[LLM] recv#%d: HTTP=%d respLen=%u\n",
                attempt, httpStatus, (unsigned)fullResponse.size());
            PetDbgA(dbg);
        }
        std::string serverErr;
        if (fullResponse.find("\"error\"") != std::string::npos) {
            PetDbgA("[LLM] error response begin\n");
            PetDbgA(fullResponse.c_str());
            PetDbgA("\n[LLM] error response end\n");
            serverErr = ExtractJsonString(fullResponse, "message");
            if (serverErr.empty()) serverErr = ExtractJsonString(fullResponse, "msg");
            if (serverErr.empty()) serverErr = ExtractJsonString(fullResponse, "error");
            if (serverErr.empty()) serverErr = "服务端返回了 error 字段（完整响应见 VS 输出窗口）";
        }
        return { fullResponse, serverErr };
    };

    std::string serverErr;
    std::string assistantRaw;
    std::string jsonObj;
    std::string zhUtf8, jaUtf8;

    // 主请求 + 解析失败自动重试一次（重试体里追加更强格式指令）
    //   重试时把"必须输出完整 JSON"约束追加到 messages 数组末尾（system 消息），
    //   而不是塞到外层对象里，避免破坏 JSON 结构。
    for (int attempt = 0; attempt <= 1; ++attempt) {
        std::wstring body = jsonBodyW;
        if (attempt == 1) {
            std::wstring stronger =
                L",{\"role\":\"system\",\"content\":\""
                + EscapeJsonW(L"【重试】你上一次的输出无法被程序解析成 JSON。"
                    L"请严格只输出一个 JSON 对象，必须同时包含非空的 zh 与 ja 两个字段，"
                    L"不要任何解释、Markdown、代码块。")
                + L"\"}";
            // 在 messages 数组的闭合 "]" 前插入（找到 "]," 即 messages 数组结束处）
            //   jsonBodyW 形如 {...,"messages":[...],"stream":false,...}
            size_t mEnd = body.find(L"],\"stream\":");
            if (mEnd == std::wstring::npos) mEnd = body.find(L"],");
            if (mEnd != std::wstring::npos)
                body.insert(mEnd, stronger);   // 插到 "]" 之前
        }
        auto pr = doOneRequest(body, attempt);
        // ★附件降级：网关不认 content 数组 / data URI 时（400/415/422），
        //   立刻换纯文本体重发一次，图片降级成"[图片 xxx]"说明，不至于整轮失败。
        if (hasImage && !jsonBodyTextOnly.empty() &&
            (httpStatus == 400 || httpStatus == 415 || httpStatus == 422)) {
            PetDbgW(L"[ATTACH] 多模态请求被拒，自动降级为纯文本重发\n");
            pr = doOneRequest(jsonBodyTextOnly, attempt);
        }
        std::string fullResponse = pr.first;
        serverErr = pr.second;
        assistantRaw = ExtractAssistantContent(fullResponse);
        PetDbgA("[LLM] raw content begin\n");
        PetDbgA(assistantRaw.c_str());
        PetDbgA("\n[LLM] raw content end\n");
        jsonObj = CleanToJsonObject(assistantRaw);
        // 字段归一化：按别名表统一映射（大小写/中英/别名一次取全）
        zhUtf8 = ExtractJsonStringAny(jsonObj, AliasZh());
        jaUtf8 = ExtractJsonStringAny(jsonObj, AliasJa());
        // ★ 模型压根没输出 JSON（直接回了一段话），把这段话当正文用
        if (zhUtf8.empty() && !assistantRaw.empty() && assistantRaw.find('{') == std::string::npos)
            zhUtf8 = assistantRaw;
        // 成功取到正文 -> 跳出重试
        if (!zhUtf8.empty()) break;
        // 只有"确实收到了 HTTP 响应、但解析不出 zh"才值得重试。
        //   网络失败（hardError 已就地报错）或服务端明确报错（serverErr 非空）
        //   时重试只会把同一条提示再叠一遍，直接跳出。
        if (hardError || !serverErr.empty() || fullResponse.empty()) break;
        if (attempt == 0) {
            PetDbgW(L"[M7] HTTP 响应解析不出 zh，追加强格式指令重试一次\n");
            Sleep(300);
        }
    }

    // 网络/解析错误已由 reportError 就地展示（含按钮复位），
    //   不再走下方通用"解析失败"/"接口报错"兜底，避免提示叠加。
    if (hardError) {
        if (proactive) g_proactiveRunning = false;  // 及时释放互斥，避免 90s 超时延迟
        return 0;
    }

    // ★ 好感度由模型这一轮给出的增减值决定（缺字段时退回 +1，保证仍有推进）。
    //   结算放在打字机跑完、历史已存档之后，界面上的顺序才不会串。
    //   按别名表归一化提取（affection/好感/好感度）。
    //   数字字段走 ExtractJsonIntAny（兼容 2/"2"/2.0/负数），
    //   数字字段兼容 2/"2"/2.0/负数，范围 [-3,5]，缺省 1。
    int affDelta = ExtractJsonIntAny(jsonObj, AliasAffection(), 1, -3, 5, nullptr);

    // 情绪解析：模型多返回的 emotion 字段，驱动动作/语气/心情三层反馈。
    //   按别名表归一化（emotion/情绪/心情/mood）。
    PetEmotion emo = ParseEmotion(ExtractJsonStringAny(jsonObj, AliasEmotion()));
    {
        char ebuf[96];
        _snprintf_s(ebuf, sizeof ebuf, _TRUNCATE, "[EMO] emotion=%d  proactive=%d\n",
            (int)emo, proactive ? 1 : 0);
        PetDbgA(ebuf);
    }

    // ★ 主动搭话不刷累计好感：定时说话若给分，挂着就能涨满，会毁掉养成节奏。
    //   但仍保留情绪的心情波动（通过下面的 WM_APP_EMOTION），让互动有温度。
    //   触摸互动同理：好感在 DoBodyTouch 点击瞬间已本地结算，这里归零避免双重加分。
    if (proactive || touch) affDelta = 0;

    // 情绪 -> 心情微调：只叠加 mood（影响爱心显示），不动 affinityTotal。
    // 由主窗口在收到 WM_APP_EMOTION 后按开关决定是否应用，避免后台线程碰共享状态。
    int emoMood = GetLLMConfigInt(L"EmotionAffinity", 1, 0, 1) ? EmotionMoodDelta(emo) : 0;

    // 导演字段解析：全部可选，缺省时不覆盖任何旧行为。
    int  dirAction  = -1;
    int  dirGaze    = -1;
    float dirIntensity = 0.0f;
    std::wstring dirMemory;
    if (directorOn) {
        dirIntensity = ExtractJsonFloat(jsonObj, "intensity", 0.0f, 0.0f, 1.0f);
        dirAction = ParseDirectorAction(ExtractJsonString(jsonObj, "action"));
        dirGaze   = ParseDirectorGaze(ExtractJsonString(jsonObj, "gaze"));
        std::string memUtf8 = ExtractJsonStringAny(jsonObj, AliasMemory());
        if (!memUtf8.empty()) dirMemory = Utf8ToWideString(memUtf8);
    }
    // 记忆落盘：只有非主动搭话、且开关开启时才记，避免定时搭话把记忆刷满。
    if (!proactive && !dirMemory.empty() && GetLLMConfigInt(L"MemoryEnable", 1, 0, 1))
        AppendMemoryLine(dirMemory);

    {
        std::lock_guard<std::recursive_mutex> lk(g_statMtx);
        // ★ 排查用：VS 输出窗口搜 [AFF] 就能看到模型这一轮到底给了几分
        char dbg[192];
        _snprintf_s(dbg, sizeof dbg, _TRUNCATE,
            "[AFF] delta=%d  today=%d/%d  total=%d  mood=%d  shown=%d/%d  zhLen=%d\n",
            affDelta, g_stat.todayAffinity, AFFINITY_DAY_CAP,
            g_stat.affinityTotal, g_stat.affinityMood, AffinityShown(), AFFINITY_CAP,
            (int)zhUtf8.size());
        PetDbgA(dbg);
    }

    // 兜底（zh/ja 已在上方按别名表归一化提取，非 JSON 原文兜底也已在重试循环里处理）。
    //   这里只剩"服务端 error 字段"和"彻底解析失败"两种提示。
    if (zhUtf8.empty() && !serverErr.empty()) {
        zhUtf8 = "（接口报错：" + serverErr + "）";
    }
    if (zhUtf8.empty()) {
        // ★ 失败时把模型返回的前 120 字显示在窗口里，不开日志也能一眼看出是
        //   空内容、被截断，还是格式不对
        std::string preview = assistantRaw.substr(0, 120);
        for (char& c : preview) if (c == '\n' || c == '\r') c = ' ';
        char st[32]; _snprintf_s(st, sizeof st, _TRUNCATE, "HTTP %d", httpStatus);
        zhUtf8 = "（解析回复失败：没取到 zh 字段。" + std::string(httpStatus ? st : "HTTP 状态未知")
            + "。响应前 120 字 → " + preview + "）";
    }

    std::wstring zhReply = Utf8ToWideString(zhUtf8);
    std::wstring jaReply = Utf8ToWideString(jaUtf8);

    if (g_lastJsonTruncated && !zhReply.empty())
        zhReply += L"\n（…这次回复达到长度上限被截断了，想看完整版请把 llm.ini 的 LongMaxTokens 调大）";

    PostMessageW(hwnd, WM_REMOVE_THINKING, 0, 0);

    // 情绪演出提前到"开始呈现回复"时投递：旧代码在打字机结束后才发，
    // 玩家只在身体交互时看得到表情切换。现在先切脸，再说话/打字。
    auto PostSpeechEmotion = [&](int holdMs) {
        HWND emoHwnd = (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd : hwnd;
        auto* cmd = new L2dDirectorCmd();
        cmd->emotion    = emo;
        cmd->moodDelta  = emoMood;
        cmd->intensity  = dirIntensity;
        cmd->action     = directorOn ? dirAction : -1;
        cmd->gaze       = directorOn ? dirGaze   : -1;
        cmd->gazeHoldMs = 1800 + (int)(dirIntensity * 1800.0f);
        cmd->holdMs     = holdMs;
        if (!PostMessageW(emoHwnd, WM_APP_DIRECTOR, 0, (LPARAM)cmd)) delete cmd;
    };

    // 历史落盘挪到 LLM 线程完成（UI 只负责显示，窗口关闭也不丢历史）。
    //   聊天/触摸两条线程都写，SaveChatHistory 内部已加 g_historyFileMtx 串行化。
    if (!proactive && !touch && !userInput.empty()) {
        // ★附件只写占位符（如 "[图片 a.jpg] 帮我看看"），正文/base64 绝不落盘
        SaveChatHistory(attachTag + userInput, zhReply);
    } else if (touch) {
        // 触摸：旁白 + 回复成对写盘（主线程 WM_APP_TOUCH_REPLY 也有一份旧逻辑，
        //   为避免双写，触摸改为只在主线程写一次——见下方，线程内不写）
    } else if (proactive) {
        // 主动搭话也 quietly 写一条历史（带"（主动搭话）"标注），方便回看。
        //   默认开，可在 llm.ini 设 ProactiveSaveHistory=0 关闭。
        if (GetLLMConfigInt(L"ProactiveSaveHistory", 1, 0, 1) && !zhReply.empty())
            SaveChatHistory(L"", L"（主动搭话）茉子：" + zhReply);
    }

    if (touch) {
        // 触摸回复：气泡弹出前先切表情；保持时长跟气泡同寿（以前传 0 = 默认 4.2s，
        // 长回复会"脸先回去、文字还在"，短回复则白白多留一会儿）。
        PostSpeechEmotion(ToastVisibleMs() + 400);
        // 触摸回复呈现统一：走"回复呈现层"——聊天窗打开就进窗口，
        //   未打开时按 TouchReplyMode 决定弹贴角色轻气泡或自动开窗口。
        HWND mainHwnd = (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd : hwnd;
        auto* bubble = new std::wstring(zhReply);
        if (!PostMessageW(mainHwnd, WM_APP_TOUCH_REPLY, (WPARAM)emo, (LPARAM)bubble)) delete bubble;
    } else if (proactive) {
        PostSpeechEmotion(ToastVisibleMs() + 400);   // 主动搭话：表情跟气泡一起收
        // 主动搭话：不走聊天窗口打字机，直接把这句话发到主窗口弹气泡。
        //   用 WM_APP_PROACTIVE，lParam = new std::wstring(要显示的话)，主线程负责释放。
        HWND mainHwnd = (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd : hwnd;
        auto* bubble = new std::wstring(zhReply);
        if (!PostMessageW(mainHwnd, WM_APP_PROACTIVE, (WPARAM)emo, (LPARAM)bubble)) delete bubble;
    } else {
        // ★ 打字机自适应：长回复自动加速并批量送出，1500 字不再要等 45 秒
        TypewriterPlan tp = PlanTypewriter(zhReply.size());
        // 表情覆盖整段打字过程，再留 1.5 秒尾巴给 TTS 起声；避免长回复打到一半脸就回去了。
        int speechMs = 0;
        if (tp.charsPerTick > 0) {
            int ticks = ((int)zhReply.size() + tp.charsPerTick - 1) / tp.charsPerTick;
            speechMs = ticks * tp.delayMs;
        }
        if (speechMs < (int)L2D_EXPR_HOLD_MS) speechMs = (int)L2D_EXPR_HOLD_MS;
        else speechMs += 1500;
        if (speechMs > 120000) speechMs = 120000;
        PostSpeechEmotion(speechMs);   // 打字开始前就切表情

        g_typewriterSkip.store(false);        // 新一轮开始，清跳过信号
        for (size_t i = 0; i < zhReply.size();) {
            if (!IsWindow(hwnd)) break;
            // 跳过信号：UI 收到点击/任意键后 set，这里立刻把剩余全文一次灌入
            if (g_typewriterSkip.load()) {
                auto* rest = new std::wstring(zhReply.substr(i));
                if (!PostMessageW(hwnd, WM_APPEND_TEXT, 0, (LPARAM)rest)) delete rest;
                i = zhReply.size();
                break;
            }
            size_t n = (size_t)tp.charsPerTick;
            if (i + n > zhReply.size()) n = zhReply.size() - i;
            auto* ch = new std::wstring(zhReply.substr(i, n));
            if (!PostMessageW(hwnd, WM_APPEND_CHAR, 0, (LPARAM)ch)) delete ch;
            i += n;
            Sleep((DWORD)tp.delayMs);
        }
        g_typewriting = false;
        // 携带会话序号：UI 只接受等于当前序号的收尾消息，晚到的旧轮回复被丢弃
        auto* done = new StreamDoneMsg();
        done->session = session; done->reply = zhReply;
        if (!PostMessageW(hwnd, WM_STREAM_DONE, 0, (LPARAM)done)) delete done;
    }

    // ★ WM_APP_AFFINITY 的处理函数在【主窗口 WndProc】里，聊天窗口 ChatWndProc 没有这个 case。
    //   必须发给主窗口：聊天窗口不处理这条消息，会被 DefWindowProc 丢弃，
    //   结果是好感度永远不累加、解锁提示也不会弹出。
    //   ★ 主动搭话 affDelta 已归零，这里仍发（0）以触发一次存档/面板刷新也无妨，
    //     但没必要，直接跳过。
    HWND affHwnd = (g_hwnd && IsWindow(g_hwnd)) ? g_hwnd : hwnd;
    if (!proactive)
        PostMessageW(affHwnd, WM_APP_AFFINITY, (WPARAM)(affDelta + AFFINITY_MSG_BIAS), 0);

    // 导演反馈已在各呈现分支的开头通过 PostSpeechEmotion 提前投递：
    // 表情/动作必须跟着"说话"走，而不是等整句打完才切。

    // ★ TTS 放在"聊天结束"之后再跑：就算合成慢或卡住，也不会把界面卡在"正在思考"。
    //   超长文本默认跳过（合成要等很久），上限默认 600 字，可在 [TTS] TtsMaxChars 调整。
    //   把情绪传进 TTS，让语速/语调随心情变化（ToneForEmotion）。
    // ★ 语音走分段流式：这里只负责"投递"，切句与合成都交给后台线程。
    //   好处有两个：一是第一句合完就能开口（不用等整段 600 字），
    //   二是 LLM 线程立刻返回，绝不会把界面卡在"正在思考"。
    //   ttsMax <= 0 表示不限制长度（旧行为是硬跳过，长文就整段哑掉）。
    int ttsMax = GetTtsMaxChars();
    if (!jaReply.empty() && IsWindow(hwnd)) {
        if (ttsMax > 0 && (int)jaReply.size() > ttsMax) {
            TtsLog("skip", "日语 " + std::to_string(jaReply.size())
                + " 字 > TtsMaxChars=" + std::to_string(ttsMax));
        } else {
            TtsSpeak(jaReply, emo);
        }
    }

    // ★ 主动搭话线程收尾：释放互斥标志，无论成功失败都要还原，否则以后再也不搭话。
    if (proactive) g_proactiveRunning = false;

    return 0;
}

// ==================================================================
//  ★附件条 UI + 三入口（拖拽 / Ctrl+V / 📎）
//  ------------------------------------------------------------------
//  附件条只在有待发送附件时出现，占 56px，横向排 chip：
//    [36×36 缩略图 | 文件名 | ✕]
//  点 ✕ 移除，点文件名用系统默认程序打开原件。
//  深色主题跟随 g_chatTheme（背景/边框/文字都取主题色）。
// ==================================================================
static std::vector<Gdiplus::Image*> g_attachThumbs;   // 与 g_pendingAttachments 同序
static std::vector<RECT> g_attachChipRects;
static std::vector<RECT> g_attachCloseRects;
static int g_attachHover = -1;

static void ChatSetStatus(HWND chat, const std::wstring& text) {
    if (g_hStatusLabel && IsWindow(g_hStatusLabel)) {
        SetWindowTextW(g_hStatusLabel, text.c_str());
        InvalidateRect(g_hStatusLabel, nullptr, TRUE);
    }
    (void)chat;
}

// 当前输入区高度：有附件时整体上移，历史区相应变矮
static int AttachBarHeight() {
    return g_pendingAttachments.empty() ? 0 : ATTACH_STRIP_H;
}

static void LayoutChatChildren(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    if (!g_hHistoryEdit || !g_hInputEdit || !g_hSendBtn) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int cx = rc.right, cy = rc.bottom;
    int attachH = AttachBarHeight();
    int inputTop = cy - 260;                       // 输入区顶部（和原来一致）
    int barTop = inputTop - attachH - (attachH ? 6 : 0);
    if (g_hAttachStrip && IsWindow(g_hAttachStrip)) {
        if (attachH) {
            ShowWindow(g_hAttachStrip, SW_SHOW);
            MoveWindow(g_hAttachStrip, 10, barTop, cx - 20, attachH, TRUE);
            InvalidateRect(g_hAttachStrip, nullptr, TRUE);
        } else {
            ShowWindow(g_hAttachStrip, SW_HIDE);
        }
    }
    int historyH = barTop - 44 - 10;
    if (historyH < 50) historyH = 50;
    MoveWindow(g_hHistoryEdit, 10, 44, cx - 20, historyH, TRUE);
    // 左侧挤出 📎 / 🎤 两枚小按钮
    MoveWindow(g_hInputEdit, 56, inputTop, cx - 186, 80, TRUE);
    MoveWindow(g_hSendBtn, cx - 120, inputTop, 100, 35, TRUE);
    HWND hExit = GetDlgItem(hwnd, 104);
    if (hExit) MoveWindow(hExit, cx - 120, inputTop + 45, 100, 35, TRUE);
    HWND hAttach = GetDlgItem(hwnd, ID_BTN_ATTACH);
    HWND hMic    = GetDlgItem(hwnd, ID_BTN_MIC);
    if (hAttach) MoveWindow(hAttach, 10, inputTop, 40, 34, TRUE);
    if (hMic)    MoveWindow(hMic, 10, inputTop + 40, 40, 34, TRUE);
    HWND hClear = GetDlgItem(hwnd, ID_BTN_CLEAR);
    HWND hMem   = GetDlgItem(hwnd, ID_BTN_MEMORY);
    HWND hLF    = GetDlgItem(hwnd, ID_BTN_LONGFORM);
    HWND hTheme = GetDlgItem(hwnd, ID_BTN_THEME);
    if (hClear) MoveWindow(hClear, 10, 10, 96, 30, TRUE);
    if (hMem)   MoveWindow(hMem, 112, 10, 96, 30, TRUE);
    if (hLF)    MoveWindow(hLF, 214, 10, 96, 30, TRUE);
    if (hTheme) MoveWindow(hTheme, 316, 10, 96, 30, TRUE);
    if (g_hStatusLabel)
        MoveWindow(g_hStatusLabel, 10, cy - 34, cx - 20, 24, TRUE);
}

// 40×40 缩略图（懒生成，失败返回 nullptr，画成 📄/🎤 图标）
static Image* MakeThumbImage(const std::wstring& path) {
    if (path.empty()) return nullptr;
    Bitmap* src = Bitmap::FromFile(path.c_str());
    if (!src || src->GetLastStatus() != Ok) { delete src; return nullptr; }
    Bitmap* dst = new Bitmap(72, 72, PixelFormat32bppARGB);
    if (!dst || dst->GetLastStatus() != Ok) { delete dst; delete src; return nullptr; }
    UINT w = src->GetWidth(), h = src->GetHeight();
    if (w && h) {
        double scale = (double)72 / (double)((w > h) ? w : h);
        int nw = (int)(w * scale + 0.5); if (nw < 1) nw = 1;
        int nh = (int)(h * scale + 0.5); if (nh < 1) nh = 1;
        Graphics g(dst);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
        g.Clear(Color(0, 0, 0, 0));
        g.DrawImage(src, Rect((72 - nw) / 2, (72 - nh) / 2, nw, nh),
            0, 0, (int)w, (int)h, UnitPixel);
    }
    delete src;
    return dst;
}

static void ClearAttachThumbs() {
    for (Image* img : g_attachThumbs) delete img;
    g_attachThumbs.clear();
}

static void RebuildAttachThumbs() {
    ClearAttachThumbs();
    g_attachThumbs.reserve(g_pendingAttachments.size());
    for (const Attachment& a : g_pendingAttachments)
        g_attachThumbs.push_back(a.kind == ATTACH_KIND_IMAGE ? MakeThumbImage(a.path) : nullptr);
}

// 附件条重绘 + 显隐（AddAttachmentFromPath 之后必须调一次）
static void RefreshAttachStrip() {
    RebuildAttachThumbs();
    g_attachHover = -1;
    if (g_hAttachStrip && IsWindow(g_hAttachStrip)) InvalidateRect(g_hAttachStrip, nullptr, TRUE);
    if (g_hChatWnd && IsWindow(g_hChatWnd)) LayoutChatChildren(g_hChatWnd);
}

// 后台解析完成回填：按 id 找占位项，成功替换、失败删除并把原因显示在状态栏。
// 玩家在解析期间手动点 ✕ 移除的条目，这里自然找不到，结果直接丢弃。
static void AttachDrainResults() {
    std::vector<AttachParseResult> done;
    {
        std::lock_guard<std::mutex> lk(g_attachDoneMtx);
        done.swap(g_attachDone);
    }
    if (done.empty()) return;
    std::wstring firstErr;
    int okCount = 0;
    for (AttachParseResult& r : done) {
        int idx = -1;
        for (size_t i = 0; i < g_pendingAttachments.size(); ++i)
            if (g_pendingAttachments[i].id == r.id) { idx = (int)i; break; }
        if (idx < 0) continue;
        if (r.att.ok) {
            g_pendingAttachments[idx] = r.att;
            ++okCount;
        } else {
            g_pendingAttachments.erase(g_pendingAttachments.begin() + idx);
            if (firstErr.empty()) firstErr = r.err.empty() ? L"附件解析失败" : r.err;
        }
    }
    RefreshAttachStrip();
    if (g_hChatWnd && IsWindow(g_hChatWnd)) {
        if (!firstErr.empty()) ChatSetStatus(g_hChatWnd, L"⚠ " + firstErr);
        else if (okCount > 0 && !AttachHasLoading())
            ChatSetStatus(g_hChatWnd, L"附件已就绪，按 Enter 发送");
    } else if (!firstErr.empty()) {
        ToastShow(L"⚠", L"附件解析失败", firstErr.c_str());
    }
}

static void RemoveAttachmentAt(int idx) {
    if (idx < 0 || idx >= (int)g_pendingAttachments.size()) return;
    g_pendingAttachments.erase(g_pendingAttachments.begin() + idx);
    RefreshAttachStrip();
    ChatSetStatus(g_hChatWnd, L"已移除附件");
}

static LRESULT CALLBACK AttachStripProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        break;
    case WM_ERASEBKGND:
        return 1;                                  // 全在 WM_PAINT 里画，避免闪烁
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        const bool dark = (g_chatTheme == 1);
        const COLORREF bg     = dark ? kChatDarkBg : GetSysColor(COLOR_WINDOW);
        const COLORREF fg     = dark ? kChatDarkFg : GetSysColor(COLOR_WINDOWTEXT);
        const COLORREF border = dark ? RGB(84, 84, 100) : RGB(200, 200, 206);
        const COLORREF hoverC = dark ? RGB(72, 72, 88) : RGB(226, 232, 242);

        HBRUSH bgBr = CreateSolidBrush(bg);
        FillRect(hdc, &rc, bgBr);
        DeleteObject(bgBr);

        g_attachChipRects.clear();
        g_attachCloseRects.clear();

        const int n = (int)g_pendingAttachments.size();
        const int gap = 6;
        const int fixedW = 6 + 36 + 8 + 8 + 22 + 6;    // 缩略图/边距/✕ 的固定占用
        int avail = rc.right - rc.left - 4;
        int nameW = 120;
        if (n > 0) {
            int total = n * (fixedW + nameW) + (n - 1) * gap;
            if (total > avail) {
                nameW = (avail - (n - 1) * gap) / n - fixedW;
                if (nameW < 44) nameW = 44;
            }
        }
        const int chipW = fixedW + nameW;
        const int chipTop = 8, chipH = 40;

        HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);

        int x = 2;
        for (int i = 0; i < n; ++i) {
            RECT chip = { x, chipTop, x + chipW, chipTop + chipH };
            RECT chipClip = chip;
            if (chipClip.right > rc.right) chipClip.right = rc.right;
            if (chipClip.left < rc.left) chipClip.left = rc.left;
            g_attachChipRects.push_back(chip);
            if (chip.left < rc.right) {
                HBRUSH cb = CreateSolidBrush(i == g_attachHover ? hoverC : bg);
                HPEN pen = CreatePen(PS_SOLID, 1, border);
                HGDIOBJ ob = SelectObject(hdc, cb);
                HGDIOBJ op = SelectObject(hdc, pen);
                RoundRect(hdc, chip.left, chip.top, chip.right, chip.bottom, 10, 10);
                SelectObject(hdc, op);
                SelectObject(hdc, ob);
                DeleteObject(pen);
                DeleteObject(cb);

                RECT thumb = { chip.left + 6, chip.top + 2, chip.left + 42, chip.top + 38 };
                Image* th = (i < (int)g_attachThumbs.size()) ? g_attachThumbs[i] : nullptr;
                if (th) {
                    // Graphics 用完立刻销毁：GDI+ 对象必须早于 EndPaint 释放，
                    // 否则 DC 已归还、GDI+ 还在往里写，属于未定义行为。
                    Graphics tg(hdc);
                    tg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
                    tg.DrawImage(th, Rect(thumb.left, thumb.top, 36, 36));
                } else {
                    const wchar_t* icon = (g_pendingAttachments[i].kind == ATTACH_KIND_VOICE) ? L"🎤"
                        : (g_pendingAttachments[i].kind == ATTACH_KIND_IMAGE) ? L"🖼" : L"📄";
                    DrawTextW(hdc, icon, -1, &thumb, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }

                RECT nameRc = { chip.left + 46, chip.top + 2, chip.right - 26, chip.top + 38 };
                const Attachment& ai = g_pendingAttachments[i];
                std::wstring nm = ai.name;
                if (ai.loading) {
                    // 解析中：名字后面挂状态、整体转灰，玩家一眼能看出"还没好"
                    nm += L" · 解析中…";
                    SetTextColor(hdc, dark ? RGB(150, 150, 165) : RGB(130, 130, 140));
                }
                DrawTextW(hdc, nm.c_str(), -1, &nameRc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SetTextColor(hdc, fg);

                RECT closeRc = { chip.right - 24, chip.top + 2, chip.right - 4, chip.top + 38 };
                g_attachCloseRects.push_back(closeRc);
                SetTextColor(hdc, dark ? RGB(200, 200, 210) : RGB(110, 110, 118));
                DrawTextW(hdc, L"✕", -1, &closeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SetTextColor(hdc, fg);
            } else {
                RECT dummy = { 0, 0, 0, 0 };
                g_attachCloseRects.push_back(dummy);
            }
            x += chipW + gap;
        }
        SelectObject(hdc, oldFont);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int mx = (int)(short)LOWORD(lp), my = (int)(short)HIWORD(lp);
        int hover = -1;
        for (size_t i = 0; i < g_attachChipRects.size(); ++i) {
            const RECT& r = g_attachChipRects[i];
            if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) { hover = (int)i; break; }
        }
        if (hover != g_attachHover) {
            g_attachHover = hover;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        // 让窗口在鼠标移出时收到 WM_MOUSELEAVE，把悬停态清掉
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_attachHover != -1) { g_attachHover = -1; InvalidateRect(hwnd, nullptr, TRUE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_attachHover >= 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_LBUTTONUP: {
        int mx = (int)(short)LOWORD(lp), my = (int)(short)HIWORD(lp);
        for (size_t i = 0; i < g_attachCloseRects.size(); ++i) {
            const RECT& r = g_attachCloseRects[i];
            if (r.right > r.left && mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                RemoveAttachmentAt((int)i);
                return 0;
            }
        }
        // ★安全：rect 列表是上一次 WM_PAINT 建的，附件可能在此之前被删掉而尚未重绘，
        //   此时 rect 数量 > 附件数量，直接下标访问会越界。上限取两者较小值。
        size_t chipN = min(g_attachChipRects.size(), g_pendingAttachments.size());
        for (size_t i = 0; i < chipN; ++i) {
            const RECT& r = g_attachChipRects[i];
            if (mx >= r.left && mx < r.right && my >= r.top && my < r.bottom) {
                const Attachment& a = g_pendingAttachments[i];
                if (!a.path.empty() && PathFileExistsW(a.path.c_str()))
                    ShellExecuteW(nullptr, L"open", a.path.c_str(), L"", nullptr, SW_SHOWNORMAL);
                return 0;
            }
        }
        return 0;
    }
    case WM_DROPFILES: {
        // 拖到附件条上也接受，直接转给聊天窗处理
        if (g_hChatWnd && IsWindow(g_hChatWnd)) {
            PostMessageW(g_hChatWnd, WM_DROPFILES, wp, lp);
            return 0;
        }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_DESTROY:
        if (g_hAttachStrip == hwnd) g_hAttachStrip = nullptr;
        break;
    default: break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------- 三入口用的通用动作 ----------------

// 📎：多选文件对话框
static void PickAttachmentFiles(HWND owner) {
    std::vector<wchar_t> buf(64 * 1024, 0);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter =
        L"支持的文件\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.txt;*.md;*.log;*.json;*.csv;*.ini;"
        L"*.yaml;*.yml;*.xml;*.html;*.htm;*.css;*.js;*.ts;*.py;*.cpp;*.c;*.h;*.hpp;*.cs;*.java;"
        L"*.go;*.rs;*.sql;*.pdf;*.docx;*.xlsx\0"
        L"图片\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp\0"
        L"文档\0*.pdf;*.docx;*.xlsx\0"
        L"文本 / 代码\0*.txt;*.md;*.json;*.csv;*.ini;*.log;*.py;*.cpp;*.h\0"
        L"所有文件\0*.*\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
        OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"选择要发给茉子的图片 / 文件";
    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<std::wstring> paths;
    const wchar_t* p = buf.data();
    std::wstring dir = p;
    p += dir.size() + 1;
    if (*p == 0) {
        paths.push_back(dir);                      // 只选了一个
    } else {
        while (*p) {
            std::wstring f = p;
            p += f.size() + 1;
            paths.push_back(dir + L"\\" + f);
        }
    }
    std::wstring firstErr;
    int added = 0;
    for (const std::wstring& path : paths) {
        std::wstring err;
        if (AddAttachmentFromPath(path, err)) ++added;
        else if (firstErr.empty()) firstErr = err;
    }
    RefreshAttachStrip();
    if (!firstErr.empty()) ChatSetStatus(owner, L"⚠ " + firstErr);
    else if (added > 0)    ChatSetStatus(owner, L"已添加 " + std::to_wstring(added) + L" 个附件，按 Enter 发送");
}

// 剪贴板 DIB → HBITMAP → JPEG base64 附件
static bool AddClipboardImageAttachment(const void* dib, size_t bytes, std::wstring& err) {
    int maxFiles = GetIniInt(L"Attach", L"MaxFiles", 4, 1, ATTACH_MAX_FILES);
    if ((int)g_pendingAttachments.size() >= maxFiles) {
        err = L"单次最多 " + std::to_wstring(maxFiles) + L" 个附件";
        return false;
    }
    if (!dib || bytes < sizeof(BITMAPINFOHEADER)) { err = L"剪贴板图片数据无效"; return false; }
    const BITMAPINFOHEADER* bih = (const BITMAPINFOHEADER*)dib;
    size_t hdrSize = bih->biSize;
    if (hdrSize < sizeof(BITMAPINFOHEADER)) hdrSize = sizeof(BITMAPINFOHEADER);
    size_t palSize = 0;
    if (bih->biBitCount <= 8)
        palSize = (bih->biClrUsed ? bih->biClrUsed : (1u << bih->biBitCount)) * 4;
    if (hdrSize + palSize > bytes) { err = L"剪贴板图片数据不完整"; return false; }

    std::vector<BYTE> bmiBuf(hdrSize + palSize + 16, 0);
    memcpy(bmiBuf.data(), dib, hdrSize + palSize);
    BITMAPINFO* bmi = (BITMAPINFO*)bmiBuf.data();
    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(hdc, bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (hbm && bits)
        memcpy(bits, (const BYTE*)dib + hdrSize + palSize, bytes - hdrSize - palSize);
    ReleaseDC(nullptr, hdc);
    if (!hbm || !bits) { if (hbm) DeleteObject(hbm); err = L"剪贴板图片转换失败"; return false; }

    Bitmap* bmp = Bitmap::FromHBITMAP(hbm, nullptr);
    int maxSide = GetIniInt(L"Attach", L"ImageMaxSide", 1280, 512, 2048);
    Attachment a;
    bool ok = bmp && bmp->GetLastStatus() == Ok &&
        EncodeBitmapToJpegBase64(bmp, maxSide, a.b64);
    if (bmp) delete bmp;
    DeleteObject(hbm);
    if (!ok || a.b64.empty()) { err = L"剪贴板图片编码失败"; return false; }

    a.kind = ATTACH_KIND_IMAGE;
    a.name = L"剪贴板截图";
    a.mime = "image/jpeg";
    a.bytes = bytes;
    a.ok = true;
    g_pendingAttachments.push_back(a);
    return true;
}

// 取拖放/剪贴板里的完整路径：固定 MAX_PATH*2 缓冲会截断超长路径，
// 先问实际长度再分配，避免深层目录/长文件名被静默截断。
static std::wstring DragFilePath(HDROP hd, UINT index) {
    UINT len = DragQueryFileW(hd, index, nullptr, 0);
    if (len == 0) return L"";
    std::wstring path((size_t)len + 1, L'\0');
    UINT got = DragQueryFileW(hd, index, path.data(), len + 1);
    if (got == 0) return L"";
    path.resize(got);
    return path;
}

// Ctrl+V / 右键粘贴：优先按"文件"处理，其次按"图片"处理
static bool AttachFromClipboard(HWND owner, std::wstring& err) {
    if (!IsClipboardFormatAvailable(CF_HDROP) && !IsClipboardFormatAvailable(CF_DIB))
        return false;
    if (!OpenClipboard(owner)) { err = L"打开剪贴板失败"; return false; }
    bool handled = false;
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hd = (HDROP)GetClipboardData(CF_HDROP);
        if (hd) {
            UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                std::wstring path = DragFilePath(hd, i);
                if (path.empty()) continue;
                std::wstring e;
                if (AddAttachmentFromPath(path, e)) handled = true;
                else if (err.empty()) err = e;
            }
        }
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE h = GetClipboardData(CF_DIB);
        if (h) {
            const void* p = GlobalLock(h);
            if (p) {
                SIZE_T sz = GlobalSize(h);
                handled = AddClipboardImageAttachment(p, (size_t)sz, err);
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    if (handled) { RefreshAttachStrip(); ChatSetStatus(owner, L"已从剪贴板添加附件"); }
    return handled;
}

// ==================================================================
//  ★附件缓存：图片复制一份到 attach_cache\，历史里可回看
//  ------------------------------------------------------------------
//  历史文件本身只写占位符（[图片 a.jpg]），原件按天清理；
//  右键 📎 → "打开附件缓存目录" 就能找到聊过的图。
// ==================================================================
static std::wstring AttachCacheDir() {
    std::wstring dir;
    if (!GetExeDir(dir)) return L"";
    std::wstring cache = dir + L"\\attach_cache";
    if (!PathFileExistsW(cache.c_str())) CreateDirectoryW(cache.c_str(), nullptr);
    return cache;
}

static void PruneDirByDays(const std::wstring& dirPath, int days) {
    if (dirPath.empty() || days <= 0) return;
    FILETIME ftNow{};
    GetSystemTimeAsFileTime(&ftNow);
    ULARGE_INTEGER uNow;
    uNow.LowPart = ftNow.dwLowDateTime;
    uNow.HighPart = ftNow.dwHighDateTime;
    const ULONGLONG limit = (ULONGLONG)days * 24ULL * 3600ULL * 10000000ULL;
    std::wstring pattern = dirPath + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER u;
        u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (uNow.QuadPart > u.QuadPart && (uNow.QuadPart - u.QuadPart) > limit)
            DeleteFileW((dirPath + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void OpenAttachCacheDir() {
    std::wstring cache = AttachCacheDir();
    if (cache.empty()) return;
    ShellExecuteW(nullptr, L"open", cache.c_str(), L"", nullptr, SW_SHOWNORMAL);
}

// 发送前把图片存进缓存（只存图片；文本/表格已经进历史占位符，没必要留副本）
static void CacheAttachmentFiles(const std::vector<Attachment>& atts) {
    int days = GetIniInt(L"Attach", L"CacheDays", 30, 0, 365);
    if (days <= 0) return;
    std::wstring cache = AttachCacheDir();
    if (cache.empty()) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int idx = 0;
    for (const Attachment& a : atts) {
        if (!a.ok || a.kind != ATTACH_KIND_IMAGE || a.path.empty()) continue;
        if (!PathFileExistsW(a.path.c_str())) continue;
        wchar_t prefix[80];
        swprintf(prefix, 80, L"\\%04d%02d%02d_%02d%02d%02d_%d_",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ++idx);
        std::wstring dst = cache + prefix + a.name;
        CopyFileW(a.path.c_str(), dst.c_str(), FALSE);
    }
    PruneDirByDays(cache, days);
    // 顺手清掉 %TEMP% 里超过 1 天的录音残留（转写完本来就会删，这里兜底）
    wchar_t tmp[MAX_PATH] = { 0 };
    if (GetTempPathW(MAX_PATH, tmp) > 0)
        PruneDirByDays(std::wstring(tmp) + L"MakotoPetAttach", 1);
}

// ==================================================================
//  ★语音输入：waveIn 录音 + 本地 whisper.cpp 转写
//  ------------------------------------------------------------------
//  为什么不能直接把音频丢给模型：Agnes 只吃文本+图片，没有音频输入，
//  所以必须在本地先把语音转成文字，再把文字当普通消息发出去。
//  转写结果只"填进输入框"，不自动发送——识别错一个字就发出去更糟。
//  loopback 说明：录音固定 16kHz/16bit/单声道，这是 whisper 的最佳输入。
// ==================================================================
#define ID_TIMER_REC 902
#define ASR_SAMPLE_RATE 16000

static HWAVEIN      g_hWaveIn = nullptr;
static WAVEHDR      g_recHdr{};
static std::vector<BYTE> g_recBuf;
static bool         g_recActive = false;
static ULONGLONG    g_recStartMs = 0;
static int          g_recLimitSec = 30;
static volatile LONG g_asrBusy = 0;          // 1 = 有转写线程在跑

// 找一个可用的 whisper 可执行 + 模型（相对路径按程序目录解析）
static bool ResolveWhisper(std::wstring& exe, std::wstring& model, std::wstring& err) {
    std::wstring dir;
    if (!GetExeDir(dir)) dir.clear();
    exe   = GetIniStr(L"ASR", L"WhisperExe",   L"whisper-cli.exe");
    model = GetIniStr(L"ASR", L"WhisperModel", L"ggml-base.bin");
    // 相对路径依次在 程序目录 / 程序目录\tools / 程序目录\tools\whisper 里找，
    // 这样默认值 whisper-cli.exe + ggml-base.bin 也能命中我们随包放的位置
    auto absolutize = [&](std::wstring& p) {
        if (p.empty()) return;
        bool absolute = (p.size() > 1 && p[1] == L':') || p[0] == L'\\';
        if (absolute || dir.empty()) return;
        const wchar_t* subs[3] = { L"", L"\\tools", L"\\tools\\whisper" };
        for (int i = 0; i < 3; ++i) {
            std::wstring cand = dir + subs[i] + L"\\" + p;
            if (PathFileExistsW(cand.c_str())) { p = cand; return; }
        }
    };
    absolutize(exe);
    absolutize(model);
    if (!PathFileExistsW(exe.c_str())) {
        err = L"找不到 " + exe + L"（放到 程序目录\\tools\\whisper\\ 或 [ASR] WhisperExe 写绝对路径）";
        return false;
    }
    if (!PathFileExistsW(model.c_str())) {
        err = L"找不到模型 " + model + L"（放到 程序目录\\tools\\whisper\\ 或 [ASR] WhisperModel 写绝对路径）";
        return false;
    }
    return true;
}

// ==================================================================
//  ★语音识别加速：常驻 whisper-server
//   每说一句都起一次 whisper-cli 要重新加载模型（1~3 秒，small 模型更久）；
//   改成第一次用时把 whisper-server 拉起来常驻内存，之后只发一次 HTTP，
//   首字延迟能降到 0.3 秒内。server 起不来时自动退回 whisper-cli，功能不受影响。
// ==================================================================
static HANDLE    g_asrSrvProc = nullptr;
static int       g_asrSrvPort = 0;
static ULONGLONG g_asrSrvLastUse = 0;

static bool AsrServerAlive() {
    if (!g_asrSrvProc) return false;
    DWORD ec = 0;
    if (!GetExitCodeProcess(g_asrSrvProc, &ec) || ec != STILL_ACTIVE) {
        CloseHandle(g_asrSrvProc);
        g_asrSrvProc = nullptr;
        g_asrSrvPort = 0;
        return false;
    }
    return true;
}

static void AsrServerStop() {
    if (!g_asrSrvProc) return;
    TerminateProcess(g_asrSrvProc, 0);
    WaitForSingleObject(g_asrSrvProc, 3000);
    CloseHandle(g_asrSrvProc);
    g_asrSrvProc = nullptr;
    g_asrSrvPort = 0;
}

// 端口探活：能拿到 HTTP 响应（哪怕是 404）就算起来了
static bool AsrServerPing(int port) {
    HINTERNET s = WinHttpOpenAuto(L"127.0.0.1");
    if (!s) return false;
    WinHttpSetTimeouts(s, 600, 600, 600, 600);
    HINTERNET c = WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)port, 0);
    bool ok = false;
    if (c) {
        HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/", nullptr, nullptr, nullptr, 0);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
                WinHttpReceiveResponse(r, nullptr))
                ok = true;
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

static bool AsrServerStart(const std::wstring& cliExe, const std::wstring& model,
                           int port, const std::wstring& lang, int threads) {
    size_t slash = cliExe.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : cliExe.substr(0, slash + 1);
    std::wstring srv = dir + L"whisper-server.exe";
    if (!PathFileExistsW(srv.c_str())) return false;

    // ★安全：路径过长时 swprintf 会溢出返回 -1 且可能不以 NUL 结尾，
    //   随后 std::wstring cmd = args 会越界读。用 _TRUNCATE 版本兜住。
    wchar_t args[1200];
    _snwprintf_s(args, 1200, _TRUNCATE,
             L"\"%s\" -m \"%s\" -t %d -l %s --host 127.0.0.1 --port %d",
             srv.c_str(), model.c_str(), threads, lang.c_str(), port);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = args;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    g_asrSrvProc = pi.hProcess;

    // 等模型加载完（small 模型首次约 2~4 秒，给 20 秒余量）
    for (int i = 0; i < 100; ++i) {
        if (!AsrServerAlive()) return false;
        if (AsrServerPing(port)) { g_asrSrvPort = port; return true; }
        Sleep(200);
    }
    AsrServerStop();
    return false;
}

// 把 wav 以 multipart 形式 POST 到 /inference，解析返回的 {"text":"..."}
static bool AsrServerInfer(int port, const std::string& wavBytes, std::wstring& out) {
    out.clear();
    if (wavBytes.empty()) return false;
    const char* boundary = "----MakotoPetAsrBoundary7d91";
    std::string body;
    body.reserve(wavBytes.size() + 512);
    body += "--"; body += boundary; body += "\r\n";
    body += "Content-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n";
    body += "Content-Type: audio/wav\r\n\r\n";
    body.append(wavBytes);
    body += "\r\n--"; body += boundary; body += "\r\n";
    body += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";
    body += "--"; body += boundary; body += "--\r\n";

    HINTERNET s = WinHttpOpenAuto(L"127.0.0.1");
    if (!s) return false;
    WinHttpSetTimeouts(s, 5000, 5000, 60000, 60000);
    HINTERNET c = WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)port, 0);
    if (!c) { WinHttpCloseHandle(s); return false; }
    HINTERNET r = WinHttpOpenRequest(c, L"POST", L"/inference", nullptr, nullptr, nullptr, 0);
    if (!r) { WinHttpCloseHandle(c); WinHttpCloseHandle(s); return false; }

    std::wstring hdr = L"Content-Type: multipart/form-data; boundary=";
    hdr += Utf8ToWideString(boundary);
    BOOL sent = WinHttpSendRequest(r, hdr.c_str(), (DWORD)-1,
                                   (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0);
    bool ok = false;
    if (sent && WinHttpReceiveResponse(r, nullptr)) {
        std::string resp;
        char buf[4096];
        DWORD n = 0;
        while (WinHttpReadData(r, buf, sizeof buf, &n) && n > 0) {
            resp.append(buf, n);
            if (resp.size() > 4u * 1024u * 1024u) break;
        }
        if (!resp.empty()) {
            std::string text = ExtractJsonString(resp, "text");
            if (!text.empty()) {
                std::wstring w = Utf8ToWideString(text);
                // 与 whisper-cli 路径保持一致：分句/换行统一压成一行（说话内容塞进输入框）
                std::wstring one;
                one.reserve(w.size());
                // 变量名用 ch：外层已有 HINTERNET c，避免遮蔽
                for (wchar_t ch : w) {
                    if (ch == L'\r' || ch == L'\n') {
                        if (!one.empty() && one.back() != L' ') one += L' ';
                    } else {
                        one += ch;
                    }
                }
                size_t b = one.find_first_not_of(L" \t");
                size_t e = one.find_last_not_of(L" \t");
                out = (b == std::wstring::npos) ? L"" : one.substr(b, e - b + 1);
                ok = !out.empty();
            }
        }
    }
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    if (ok) g_asrSrvLastUse = GetTickCount64();
    return ok;
}

// 16k/16bit/mono PCM 写成标准 WAV（whisper 只吃文件）
static std::wstring WriteTempWav(const BYTE* pcm, DWORD bytes) {
    wchar_t tmp[MAX_PATH] = { 0 };
    if (GetTempPathW(MAX_PATH, tmp) == 0) return L"";
    std::wstring dir = std::wstring(tmp) + L"MakotoPetAttach";
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t name[64];
    swprintf(name, 64, L"\\voice_%llu.wav", (unsigned long long)GetTickCount64());
    std::wstring path = dir + name;

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return L"";
    const DWORD rate = ASR_SAMPLE_RATE;
    const WORD  ch = 1, bits = 16;
    const DWORD byteRate = rate * ch * bits / 8;
    const WORD  blockAlign = (WORD)(ch * bits / 8);
    const DWORD dataLen = bytes;
    const DWORD riffLen = 36 + dataLen;

    BYTE hdr[44] = { 0 };
    memcpy(hdr, "RIFF", 4);
    memcpy(hdr + 4, &riffLen, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    DWORD fmtLen = 16; memcpy(hdr + 16, &fmtLen, 4);
    WORD fmtTag = 1;   memcpy(hdr + 20, &fmtTag, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byteRate, 4);
    memcpy(hdr + 32, &blockAlign, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &dataLen, 4);
    fwrite(hdr, 1, sizeof hdr, f);
    if (bytes) fwrite(pcm, 1, bytes, f);
    fclose(f);
    return path;
}

// 跑子进程并抓 stdout+stderr（whisper-cli 把识别结果打到标准输出）
static bool RunProcessCapture(const std::wstring& cmdLine, DWORD timeoutMs, std::string& out) {
    out.clear();
    if (cmdLine.empty()) return false;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = (hNul != INVALID_HANDLE_VALUE) ? hNul : nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;
    BOOL started = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
    if (!started) { CloseHandle(rd); return false; }

    char buf[4096];
    DWORD n = 0;
    ULONGLONG start = GetTickCount64();
    bool killed = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            if (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) {
                out.append(buf, n);
                continue;
            }
            break;      // 读失败/读到 0：别在循环里空转
        }
        DWORD w = WaitForSingleObject(pi.hProcess, 80);
        if (w == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                if (!ReadFile(rd, buf, sizeof buf, &n, nullptr) || n == 0) break;
                out.append(buf, n);
            }
            break;
        }
        if (GetTickCount64() - start > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            killed = true;
            break;
        }
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(rd);
    return !killed && !out.empty();
}

// whisper 的日志行混在结果里，按行过滤，只留真正的转写文本
static std::wstring CleanWhisperOutput(const std::string& raw) {
    std::wstring all = Utf8ToWideString(raw);
    std::wstring out;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find(L'\n', pos);
        std::wstring line = (nl == std::wstring::npos) ? all.substr(pos) : all.substr(pos, nl - pos);
        pos = (nl == std::wstring::npos) ? all.size() : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        size_t b = line.find_first_not_of(L' ');
        if (b == std::wstring::npos) continue;
        line = line.substr(b);
        if (line.empty()) continue;
        // 过滤 whisper 自身的日志/时间戳
        if (line[0] == L'[') continue;
        if (line.find(L"whisper_") != std::wstring::npos) continue;
        if (line.find(L"ggml_") != std::wstring::npos) continue;
        if (line.find(L"main:") == 0) continue;
        if (line.find(L"system_info") != std::wstring::npos) continue;
        if (line.find(L"error:") == 0 || line.find(L"failed") != std::wstring::npos) continue;
        if (!out.empty()) out += L' ';
        out += line;
    }
    return out;
}

// 后台转写线程：绝不能在 UI 线程里等 whisper（base 模型也要几秒）
struct AsrTask {
    std::wstring wav;
    HWND hwnd = nullptr;
};

static unsigned __stdcall AsrThreadProc(void* p) {
    AsrTask* t = (AsrTask*)p;
    if (!t) return 0;
    std::wstring wav = t->wav;
    HWND hwnd = t->hwnd;
    delete t;

    std::wstring exe, model, err;
    std::wstring result;
    if (!ResolveWhisper(exe, model, err)) {
        result = L"⚠ " + err;
    } else {
        std::wstring lang = GetIniStr(L"ASR", L"Language", L"zh");
        int threads = GetIniInt(L"ASR", L"Threads", 4, 1, 32);
        bool done = false;

        // ① 常驻服务优先：模型只加载一次，之后每句只发一次 HTTP
        if (GetIniInt(L"ASR", L"ServerMode", 1, 0, 1) != 0) {
            int port = GetIniInt(L"ASR", L"ServerPort", 8791, 1024, 65535);
            std::string wavBytes;
            if (ReadWholeFileBytes(wav, 32u * 1024u * 1024u, wavBytes)) {
                bool needStart = !AsrServerAlive() || g_asrSrvPort != port;
                bool started = true;
                if (needStart) {
                    AsrServerStop();
                    started = AsrServerStart(exe, model, port, lang, threads);
                }
                if (started && g_asrSrvPort == port) {
                    std::wstring srvText;
                    if (AsrServerInfer(port, wavBytes, srvText)) {
                        result = srvText;
                        done = true;
                    }
                }
            }
        }

        // ② 服务不可用（没装 whisper-server / 端口被占）：退回一次性调用 whisper-cli
        if (!done) {
            // ★安全：模型/音频路径可能很长，固定缓冲需 _TRUNCATE 防未终止串
            wchar_t tail[1024];
            _snwprintf_s(tail, 1024, _TRUNCATE, L" -m \"%s\" -f \"%s\" -l %s -nt -np -t %d",
                model.c_str(), wav.c_str(), lang.c_str(), threads);
            std::string raw;
            if (RunProcessCapture(L"\"" + exe + L"\"" + tail, 120000, raw)) {
                result = CleanWhisperOutput(raw);
            } else {
                result = L"⚠ 语音识别失败（whisper 没跑起来或超时，DebugView 可看输出）";
            }
        }
    }
    DeleteFileW(wav.c_str());          // 音频用完即删，不留隐私
    InterlockedExchange(&g_asrBusy, 0);

    auto* msg = new std::wstring(result);
    if (!PostMessageW(hwnd, WM_APP_ASR_DONE, 0, (LPARAM)msg)) delete msg;
    return 0;
}

static bool VoiceStartRecording(HWND owner) {
    if (g_recActive) return true;
    if (InterlockedCompareExchange(&g_asrBusy, 0, 0) != 0) {
        ChatSetStatus(owner, L"上一次识别还没结束，稍等一下…");
        return false;
    }
    if (GetIniInt(L"ASR", L"Enable", 0, 0, 1) == 0) {
        ChatSetStatus(owner, L"语音输入未开启：右键桌宠 → ⚙ 设置 → 附件·输入 → 语音输入开关");
        return false;
    }
    int engine = GetIniInt(L"ASR", L"Engine", 1, 0, 2);
    if (engine == 0) {
        ChatSetStatus(owner, L"[ASR] Engine=0 表示关闭；改成 1 使用本地 whisper");
        return false;
    }
    if (engine == 2) {
        ChatSetStatus(owner, L"云端识别暂未开放，请把 [ASR] Engine 改成 1 用本地 whisper");
        return false;
    }
    std::wstring exe, model, err;
    if (!ResolveWhisper(exe, model, err)) { ChatSetStatus(owner, L"⚠ " + err); return false; }
    TtsStopAll();      // 先掐掉茉子的语音，否则麦克风会把她的声音一起录进去

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = ASR_SAMPLE_RATE;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = 0;
    if (waveInOpen(&g_hWaveIn, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_hWaveIn = nullptr;
        ChatSetStatus(owner, L"打不开麦克风（检查 设置→隐私→麦克风 权限）");
        return false;
    }
    g_recLimitSec = GetIniInt(L"ASR", L"MaxSeconds", 30, 5, 120);
    g_recBuf.assign((size_t)wf.nAvgBytesPerSec * (size_t)g_recLimitSec, 0);
    ZeroMemory(&g_recHdr, sizeof(g_recHdr));
    g_recHdr.lpData = (LPSTR)g_recBuf.data();
    g_recHdr.dwBufferLength = (DWORD)g_recBuf.size();
    if (waveInPrepareHeader(g_hWaveIn, &g_recHdr, sizeof(g_recHdr)) != MMSYSERR_NOERROR ||
        waveInAddBuffer(g_hWaveIn, &g_recHdr, sizeof(g_recHdr)) != MMSYSERR_NOERROR ||
        waveInStart(g_hWaveIn) != MMSYSERR_NOERROR) {
        waveInReset(g_hWaveIn);
        waveInClose(g_hWaveIn);
        g_hWaveIn = nullptr;
        ChatSetStatus(owner, L"录音启动失败（麦克风被其它程序占用？）");
        return false;
    }
    g_recActive = true;
    g_recStartMs = GetTickCount64();
    if (owner) {
        SetTimer(owner, ID_TIMER_REC, 200, nullptr);
        SetButtonText(owner, ID_BTN_MIC, L"⏹");
        ChatSetStatus(owner, L"🎤 正在录音… 再点一次结束（最长 " + std::to_wstring(g_recLimitSec) + L" 秒）");
    }
    return true;
}

static void VoiceStopRecording(HWND owner, bool transcribe) {
    if (!g_recActive || !g_hWaveIn) return;
    g_recActive = false;
    waveInStop(g_hWaveIn);
    waveInReset(g_hWaveIn);                       // 让 dwBytesRecorded 变成实际收到的字节数
    DWORD recorded = g_recHdr.dwBytesRecorded;
    // ★安全：dwBytesRecorded 由音频驱动填写，正常不会超过 dwBufferLength，
    //   但异常驱动可能给出越界值；下游按该长度读 g_recBuf，必须钳到实际缓冲大小。
    if (recorded > (DWORD)g_recBuf.size()) recorded = (DWORD)g_recBuf.size();
    waveInUnprepareHeader(g_hWaveIn, &g_recHdr, sizeof(g_recHdr));
    waveInClose(g_hWaveIn);
    g_hWaveIn = nullptr;
    if (owner) {
        KillTimer(owner, ID_TIMER_REC);
        SetButtonText(owner, ID_BTN_MIC, L"🎤");
    }
    if (!transcribe) return;
    // 少于 0.1 秒基本是误触
    if (recorded < (DWORD)(ASR_SAMPLE_RATE * 2 / 10)) {
        ChatSetStatus(owner, L"录音太短，已取消");
        return;
    }
    std::wstring wav = WriteTempWav(g_recBuf.data(), recorded);
    if (wav.empty()) { ChatSetStatus(owner, L"保存录音失败"); return; }
    ChatSetStatus(owner, L"正在识别语音…（本地 whisper，第一次会慢一点）");
    InterlockedExchange(&g_asrBusy, 1);
    AsrTask* t = new AsrTask();
    t->wav = wav;
    t->hwnd = owner;
    unsigned int tid = 0;
    if (!_beginthreadex(nullptr, 0, AsrThreadProc, t, 0, &tid)) {
        delete t;
        InterlockedExchange(&g_asrBusy, 0);
        DeleteFileW(wav.c_str());
        ChatSetStatus(owner, L"启动识别线程失败");
    }
}

static void VoiceAbortRecording() {
    if (g_recActive) VoiceStopRecording(g_hChatWnd, false);
}

static void ToggleVoiceRecord(HWND hwnd) {
    if (g_recActive) VoiceStopRecording(hwnd, true);
    else             VoiceStartRecording(hwnd);
}

static LRESULT CALLBACK ChatWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        // 缓存失效：新建聊天窗口时重新读 prompt.txt，外部改过人设无需重启程序。
        InvalidatePromptCache();
        // ★ 主题必须在建控件之前读：否则按钮标题（🌙/☀️）会和实际主题对不上
        g_chatTheme = GetUIConfigInt(L"ChatTheme", 0, 0, 1);
        g_hHistoryEdit = CreateWindowExW(0, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | WS_VSCROLL | ES_AUTOVSCROLL | ES_READONLY | ES_NOHIDESEL,
            10, 44, 460, 240, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        g_hInputEdit = CreateWindowExW(0, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | WS_VSCROLL | ES_WANTRETURN,
            10, 324, 350, 80, hwnd, (HMENU)102, GetModuleHandleW(nullptr), nullptr);
        g_hSendBtn = CreateWindowExW(0, L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            370, 324, 100, 35, hwnd, (HMENU)103, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"退出",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            370, 369, 100, 35, hwnd, (HMENU)104, GetModuleHandleW(nullptr), nullptr);
        // 顶栏三个小按钮：清空对话 / 记忆 / 长文·短文切换
        CreateWindowExW(0, L"BUTTON", L"🗑 清空对话",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 10, 96, 30, hwnd, (HMENU)ID_BTN_CLEAR, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"🧠 记忆",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            112, 10, 96, 30, hwnd, (HMENU)ID_BTN_MEMORY, GetModuleHandleW(nullptr), nullptr);
        {
            // 三态循环：自动(2) → 短文(0) → 长文(1) → 自动(2)，可回到默认"自动"。
            int lv = GetLLMConfigInt(L"LongFormManual", 2, 0, 2);
            const wchar_t* lbl = (lv == 1) ? L"📜 长文" : (lv == 0) ? L"📄 短文" : L"🔀 自动";
            CreateWindowExW(0, L"BUTTON", lbl,
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                214, 10, 96, 30, hwnd, (HMENU)ID_BTN_LONGFORM, GetModuleHandleW(nullptr), nullptr);
        }
        // 主题切换按钮（🌙 深色 / ☀️ 浅色），点击即时切换并写回 [UI] ChatTheme 持久化
        CreateWindowExW(0, L"BUTTON", g_chatTheme ? L"☀️ 浅色" : L"🌙 深色",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            316, 10, 96, 30, hwnd, (HMENU)ID_BTN_THEME, GetModuleHandleW(nullptr), nullptr);
        // ★附件：＋ 加号选文件 / 🎤 语音输入（两条入口与拖拽、粘贴共用 AppendAttachment）
        //   加号由 DrawChatButton 用 GDI 画，此处文字仅为无障碍名称兜底。
        CreateWindowExW(0, L"BUTTON", L"+",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 324, 40, 34, hwnd, (HMENU)ID_BTN_ATTACH, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"🎤",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            10, 364, 40, 34, hwnd, (HMENU)ID_BTN_MIC, GetModuleHandleW(nullptr), nullptr);
        // ★附件条：自绘的横条，有附件才显示
        {
            WNDCLASSEXW sac = { sizeof(WNDCLASSEXW) };
            sac.lpfnWndProc = AttachStripProc;
            sac.hInstance = GetModuleHandleW(nullptr);
            sac.lpszClassName = L"MakotoAttachStrip";
            sac.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassExW(&sac);      // 已注册会返回 0，忽略即可
            g_hAttachStrip = CreateWindowExW(0, L"MakotoAttachStrip", L"",
                WS_CHILD, 10, 264, 460, ATTACH_STRIP_H,
                hwnd, (HMENU)ID_ATTACH_STRIP, GetModuleHandleW(nullptr), nullptr);
        }
        // ★拖拽入口：整个聊天窗接受文件（子控件区域由系统沿父链找到本窗）
        DragAcceptFiles(hwnd, TRUE);
        // 底部状态栏（"茉子正在思考…"动态省略号 / 打字机跳过提示）
        g_hStatusLabel = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 410, 460, 22, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        // 字号/字体/主题可配（[UI] ChatFontPt / ChatFontFace / ChatTheme），缺省沿用旧值
        {
            int pt = GetUIConfigInt(L"ChatFontPt", 16, 8, 40);
            std::wstring face = GetUIConfigStr(L"ChatFontFace", L"Segoe UI Emoji");
            if (face.empty()) face = L"Segoe UI Emoji";
            HFONT hFont = CreateFontW(pt, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FF_DONTCARE, face.c_str());
            SendMessageW(g_hHistoryEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(g_hInputEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
            if (g_hStatusLabel) SendMessageW(g_hStatusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
            // 自绘按钮共用同一字体，文字风格才统一（DrawChatButton 里取 WM_GETFONT）
            {
                const int btnIds[] = { ID_BTN_CLEAR, ID_BTN_MEMORY, ID_BTN_LONGFORM, ID_BTN_THEME,
                    ID_BTN_ATTACH, ID_BTN_MIC, 103, 104 };
                for (int id : btnIds) {
                    HWND b = GetDlgItem(hwnd, id);
                    if (b) SendMessageW(b, WM_SETFONT, (WPARAM)hFont, TRUE);
                }
            }
            // 主题背景在 ApplyChatTheme 里统一处理（EM_SETBKGNDCOLOR，见下方调用）
        }
        std::wstring history = LoadChatHistory();
        g_streamCr = false; g_nlRun = 0;        // 新的一次会话，换行记账清零
        SetWindowTextW(g_hHistoryEdit, history.empty() ? L"（暂无对话历史，开始和茉子聊天吧~）\r\n\r\n" : history.c_str());
        // ★主题 + 载入历史后按块前缀整体重上色（ApplyChatTheme 内部会调 ColorizeFullHistory）
        ApplyChatTheme(hwnd);
        SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
        // 状态栏动画：每 250ms 刷一次"茉子正在思考…"的动态省略号
        SetTimer(hwnd, ID_TIMER_STATUS, 250, nullptr);
        // 子类化输入框：Enter 发送 / Shift+Enter 换行 / Esc 关窗
        //   ★修正：窗口关闭后重建时 g_oldInputProc 仍非空，原写法会跳过子类化，
        //   导致第二次开聊天窗后 Enter 和 Ctrl+V 附件全部失效。每次新建都必须重挂。
        if (g_hInputEdit)
            g_oldInputProc = (WNDPROC)SetWindowLongPtrW(g_hInputEdit, GWLP_WNDPROC, (LONG_PTR)InputSubclassProc);
        // ★附件：把上次关窗期间后台解析完的结果收掉，并按当前附件重建附件条
        AttachDrainResults();
        RefreshAttachStrip();
        LayoutChatChildren(hwnd);       // 首次布局：📎/🎤 + 附件条位置
        break;
    }
    // ★附件后台解析完成：回填占位附件、刷新附件条（不进聊天状态机）
    case WM_APP_ATTACH_READY:
        AttachDrainResults();
        return 0;
    case WM_SIZE: {
        // 布局集中到 LayoutChatChildren：多了附件条和 📎/🎤 两枚按钮，
        // 尺寸变化时整块重排，避免两处逻辑各改一半。
        LayoutChatChildren(hwnd);
        break;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        // ★加了附件条（56px）之后 350 太矮：历史区会被压到 50px 并盖住附件条，
        //   抬到 430 保证「历史区 + 附件条 + 输入区 + 状态栏」都放得下。
        mmi->ptMinTrackSize.x = 400; mmi->ptMinTrackSize.y = 430;
        break;
    }
    // 聊天窗位置/尺寸变化：静默 0.8s 后落盘（主窗定时器里 MaybeSaveChatGeom 推进）
    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS* pwp = (WINDOWPOS*)lParam;
        if (pwp && !(pwp->flags & (SWP_NOMOVE | SWP_NOSIZE))) MarkChatGeomDirty();
        break;
    }
    // 打字机跳过：聊天窗显示中，点一下（左键）立刻把剩余全文灌入。
    //   受 [UI] TypewriterSkipMode 控制（0=禁用跳过，1/2=允许）。
    case WM_LBUTTONDOWN:
        if (g_typewriting && GetUIConfigInt(L"TypewriterSkipMode", 1, 0, 2) != 0) {
            g_typewriterSkip.store(true);
            if (g_hStatusLabel && IsWindow(g_hStatusLabel))
                SetWindowTextW(g_hStatusLabel, L"");
            return 0;
        }
        break;
    case WM_APPEND_TEXT: {
        std::wstring* text = (std::wstring*)lParam;
        if (text) {
            // 记录追加前的 cp，给新内容上色（茉子粉色）
            long cp = (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) ? GetWindowTextLengthW(g_hHistoryEdit) : 0;
            // 同一轮回复里的补充说明（例如语音合成失败），不额外分段
            AppendHistoryText(*text);
            if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) {
                LONG c; DWORD e; ColorForPrefix(L"茉子：", c, e);   // ★主题感知的朱红
                ColorizeRange(g_hHistoryEdit, cp, GetWindowTextLengthW(g_hHistoryEdit), c, e);
            }
            delete text;
        }
        break;
    }
    case WM_REMOVE_THINKING:
        if (g_hHistoryEdit) {
            int len = GetWindowTextLengthW(g_hHistoryEdit);
            if (len < 0) len = 0;
            // 按 len+1 分配（含 NUL），写完再收回到 len；
            // 只分配 len 却告诉 API 有 len+1 空间是未定义行为。
            std::wstring currentText((size_t)len + 1, L'\0');
            GetWindowTextW(g_hHistoryEdit, &currentText[0], len + 1);
            currentText.resize((size_t)len);
            // 只认紧跟在换行后的标签：用户正文里也可能出现"茉子："三个字，
            // 单纯 rfind(L"茉子：") 会把历史从用户那句话中间截断。
            size_t pos = currentText.rfind(L"茉子：");
            if (pos != std::wstring::npos && pos > 0 &&
                (currentText[pos - 1] == L'\n' || currentText[pos - 1] == L'\r')) {
                currentText.erase(pos + 3);
                SetWindowTextW(g_hHistoryEdit, currentText.c_str());
                // ★SetWindowTextW 会把整篇的字符格式重置成默认色，
                //   不重新上色的话，出错一次之后整段历史就全变黑/全变灰了。
                ApplyChatTheme(hwnd);
            }
        }
        break;
    case WM_APPEND_CHAR: {
        std::wstring* str = (std::wstring*)lParam;
        if (str) {
            // 打字机分片也上色（茉子粉色）：记录追加前后 cp
            long cp = (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) ? GetWindowTextLengthW(g_hHistoryEdit) : 0;
            // 打字机分片：逐块规范化换行（跨块的 \r / \n 由 g_streamCr 记账）
            AppendHistoryText(*str);
            if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) {
                LONG c; DWORD e; ColorForPrefix(L"茉子：", c, e);   // ★主题感知的朱红
                ColorizeRange(g_hHistoryEdit, cp, GetWindowTextLengthW(g_hHistoryEdit), c, e);
            }
            delete str;
        }
        break;
    }
    case WM_LOCATE_PROACTIVE: {
        // 点击主动搭话气泡 -> 滚到最后一条"（主动搭话）茉子："记录并高亮选中。
        if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) {
            int len = GetWindowTextLengthW(g_hHistoryEdit);
            std::wstring text(len + 1, L'\0');
            GetWindowTextW(g_hHistoryEdit, &text[0], len + 1);
            text.resize(len);
            size_t pos = text.rfind(L"（主动搭话）茉子：");
            if (pos == std::wstring::npos) {
                // 找不到标注就退而求其次滚到末尾
                SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
            } else {
                // 让该行滚动到可视区域顶部附近，并选中这段便于扫读。
                // 用 (std::min) 加括号，绕开 windows.h 的 min 宏。
                size_t selEnd = pos + 40; if (selEnd > text.size()) selEnd = text.size();
                SendMessageW(g_hHistoryEdit, EM_SETSEL, (WPARAM)pos, (LPARAM)selEnd);
                SendMessageW(g_hHistoryEdit, EM_SCROLLCARET, 0, 0);
            }
        }
        break;
    }
    case WM_STREAM_DONE: {
        // 会话序号防串台：只接受等于当前期望序号的收尾，晚到的旧轮回复丢弃。
        //   历史落盘已在 LLM 线程完成（M4），这里不再重复 SaveChatHistory。
        StreamDoneMsg* done = (StreamDoneMsg*)lParam;
        if (done) {
            if (done->session == g_chatExpectedSession.load()) {
                int len = GetWindowTextLengthW(g_hHistoryEdit);
                if (len < 0) len = 0;
                // ★安全：同 WM_REMOVE_THINKING，按 len+1 分配后再收缩
                std::wstring currentText((size_t)len + 1, L'\0');
                GetWindowTextW(g_hHistoryEdit, &currentText[0], len + 1);
                currentText.resize((size_t)len);
                size_t thinkPos = currentText.rfind(L"思考中...");
                if (thinkPos != std::wstring::npos && thinkPos >= 3) {
                    currentText.erase(thinkPos - 3, currentText.length() - (thinkPos - 3));
                    currentText += L"（未收到回复）";
                    SetWindowTextW(g_hHistoryEdit, currentText.c_str());
                    ApplyChatTheme(hwnd);      // 重设文本会清掉字符格式，这里补回来
                }
                g_pendingUserInput.clear();
                EnsureHistoryBlankLine();   // 这一轮说完，和下一轮之间留一个空行
                g_isChatting = false;
                g_typewriting = false;      // 打字机结束，清跳过提示
                if (IsWindow(g_hSendBtn)) EnableWindow(g_hSendBtn, TRUE);
                if (IsWindow(g_hInputEdit)) SetFocus(g_hInputEdit);
            } else {
                PetDbgW(L"[M4] 丢弃过期会话的收尾消息\n");
            }
            delete done;
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 103) {
            // 共用提交入口：按钮和回车都走 SubmitChat。
            //   按钮被禁用（正在回复）时 Enter 也不能发，故 !g_isChatting 判断放这里。
            if (!g_isChatting) SubmitChat(hwnd);
        }
        else if (LOWORD(wParam) == 104) {
            SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        else if (LOWORD(wParam) == ID_BTN_ATTACH) {
            // 📎 文件对话框（多选）
            PickAttachmentFiles(hwnd);
        }
        else if (LOWORD(wParam) == ID_BTN_MIC) {
            // 🎤 语音输入：开始/结束录音 → whisper 转写 → 填入输入框
            ToggleVoiceRecord(hwnd);
        }
        else if (LOWORD(wParam) == ID_BTN_CLEAR) {
            // ★危险操作二次确认：第一次点击只上膛（按钮变"⚠ 确认清空？"），
            //   5 秒内再点一次才真正删除 chat_history.txt。
            if (g_confirmAction == 1 && GetTickCount64() <= g_confirmUntil) {
                g_confirmAction = 0;
                SetButtonText(hwnd, ID_BTN_CLEAR, L"🗑 清空对话");
                if (g_hStatusLabel && IsWindow(g_hStatusLabel))
                    SetWindowTextW(g_hStatusLabel, L"");
                ClearChatHistory();
                ToastShow(L"🗑", L"对话已清空", L"chat_history.txt 已删除，可以从头开始聊了。");
            } else {
                ArmDangerousAction(hwnd, 1);
            }
        }
        else if (LOWORD(wParam) == ID_BTN_MEMORY) {
            // ★记忆按钮：常态=打开 memory.txt；若已通过右键菜单上膛"清空记忆"，
            //   这次点击就是确认删除（同样 5 秒内有效）。
            if (g_confirmAction == 2 && GetTickCount64() <= g_confirmUntil) {
                g_confirmAction = 0;
                SetButtonText(hwnd, ID_BTN_MEMORY, L"🧠 记忆");
                if (g_hStatusLabel && IsWindow(g_hStatusLabel))
                    SetWindowTextW(g_hStatusLabel, L"");
                ClearMemoryFile();
                ToastShow(L"🧠", L"记忆已清空", L"memory.txt 已删除，茉子不再记得之前的事。");
            } else {
                OpenMemoryFile();
            }
        }
        else if (LOWORD(wParam) == ID_BTN_LONGFORM) {
            // 三态循环：自动(2)→短文(0)→长文(1)→自动(2)，能回到默认"自动"。
            int v = GetLLMConfigInt(L"LongFormManual", 2, 0, 2);
            int nv = (v == 2) ? 0 : (v == 0) ? 1 : 2;
            SetLLMConfigInt(L"LongFormManual", nv, 0, 2);
            const wchar_t* lbl = (nv == 1) ? L"📜 长文" : (nv == 0) ? L"📄 短文" : L"🔀 自动";
            SetButtonText(hwnd, ID_BTN_LONGFORM, lbl);
        }
        else if (LOWORD(wParam) == ID_BTN_THEME) {
            // ★主题切换：翻转并写回 [UI] ChatTheme 持久化，立即把新主题应用到全部控件。
            g_chatTheme = (g_chatTheme == 1) ? 0 : 1;
            SetUIConfigInt(L"ChatTheme", g_chatTheme, 0, 1);
            SetButtonText(hwnd, ID_BTN_THEME, g_chatTheme ? L"☀️ 浅色" : L"🌙 深色");
            ApplyChatTheme(hwnd);       // 内含 EM_SETBKGNDCOLOR + 默认文字色 + 重上色 + 重绘
            UpdateWindow(hwnd);
        }
        break;
    // ★深色主题：聊天窗按钮全部 BS_OWNERDRAW，统一自绘（浅色/深色两套配色）
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis && dis->CtlType == ODT_BUTTON) {
            DrawChatButton(dis);
            return TRUE;
        }
        break;
    }
    // ★「记忆」按钮右键菜单：打开文件 / 清空长期记忆（清空需二次确认）
    case WM_CONTEXTMENU: {
        HWND src = (HWND)wParam;
        // ★📎 右键：打开附件缓存目录 / 查看支持的类型
        if (src && src == GetDlgItem(hwnd, ID_BTN_ATTACH)) {
            POINT pt;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            if (lParam == (LPARAM)-1) {
                RECT r; GetWindowRect(src, &r);
                pt.x = r.left; pt.y = r.bottom;
            }
            HMENU am = CreatePopupMenu();
            AppendMenuW(am, MF_STRING, 1, L"📂 打开附件缓存目录");
            AppendMenuW(am, MF_STRING, 2, L"📄 支持哪些文件？");
            int acmd = (int)TrackPopupMenu(am, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(am);
            if (acmd == 1) OpenAttachCacheDir();
            else if (acmd == 2) ChatSetStatus(hwnd,
                L"支持：图片 png/jpg/bmp/gif/webp ｜ 文本与代码 txt/md/json/csv/py/cpp… ｜ PDF（需 pdftotext.exe）｜ docx ｜ xlsx");
            return 0;
        }
        if (src && src == GetDlgItem(hwnd, ID_BTN_MEMORY)) {
            POINT pt;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            if (lParam == (LPARAM)-1) {          // 键盘触发：贴在按钮下方
                RECT r; GetWindowRect(src, &r);
                pt.x = r.left; pt.y = r.bottom;
            }
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"📂 打开记忆文件");
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(m, MF_STRING, 2, L"🗑 清空长期记忆…");
            int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd == 1) OpenMemoryFile();
            else if (cmd == 2) ArmDangerousAction(hwnd, 2);
            return 0;
        }
        break;
    }
    case WM_APP_AFFINITY:
        // 兜底：这条消息由主窗口处理。万一被发到聊天窗口，转发过去而不是丢掉。
        // 必须异步：SendMessageW 会阻塞后台 LLM 线程死等主窗口消息循环
        if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_APP_AFFINITY, wParam, lParam);
        return 0;
    // ★深色主题：窗口自身背景（控件之间的空隙）必须自己刷，否则会留一片系统灰白。
    case WM_ERASEBKGND: {
        if (g_chatTheme == 1) {
            HDC hdc = (HDC)wParam;
            RECT rc; GetClientRect(hwnd, &rc);
            static HBRUSH s_winBrush = CreateSolidBrush(kChatDarkBg);
            FillRect(hdc, &rc, s_winBrush);
            return 1;       // 已擦除
        }
        break;              // 浅色：交给系统默认
    }
    // 深色主题：STATIC/EDIT 等标准控件走 WM_CTLCOLOR*（RichEdit 不走这里，
    //   它的背景由 ApplyChatTheme 里的 EM_SETBKGNDCOLOR 负责）。
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        if (g_chatTheme == 1) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, kChatDarkBg);
            SetTextColor(hdc, kChatDarkFg);
            static HBRUSH s_darkBrush = CreateSolidBrush(kChatDarkBg);
            return (LRESULT)s_darkBrush;
        }
        return 0;   // 浅色：交给默认处理
    }
    // 状态栏动画：正在回复时动态"茉子正在思考…"（循环省略号），打字机时显示跳过提示
    case WM_TIMER:
        // ★语音：录音计时 + 到上限自动结束（防误触长录把内存占满）
        if ((UINT)wParam == ID_TIMER_REC) {
            if (!g_recActive) { KillTimer(hwnd, ID_TIMER_REC); break; }
            ULONGLONG el = (GetTickCount64() - g_recStartMs) / 1000;
            if (el >= (ULONGLONG)g_recLimitSec) { VoiceStopRecording(hwnd, true); break; }
            static ULONGLONG s_lastShownSec = (ULONGLONG)-1;
            if (el != s_lastShownSec) {          // 每秒只刷一次，避免 200ms 一次的重绘抖动
                s_lastShownSec = el;
                ChatSetStatus(hwnd, L"🔴 录音中 " + std::to_wstring(el) + L" 秒 / 最长 "
                    + std::to_wstring(g_recLimitSec) + L" 秒（再点 🎤 结束）");
            }
            break;
        }
        if ((UINT)wParam == ID_TIMER_STATUS) {
            // ★常驻 whisper 的空闲回收：默认 10 分钟没用就关掉，别白占几百 MB 内存。
            //   每 30 秒才读一次 ini，避免每帧都碰磁盘。
            {
                static ULONGLONG s_lastAsrIdleCheck = 0;
                ULONGLONG now = GetTickCount64();
                if (g_asrSrvProc && now - s_lastAsrIdleCheck > 30000) {
                    s_lastAsrIdleCheck = now;
                    int idleSec = GetIniInt(L"ASR", L"ServerIdleSec", 600, 60, 86400);
                    if (g_asrSrvLastUse != 0 && now - g_asrSrvLastUse > (ULONGLONG)idleSec * 1000)
                        AsrServerStop();
                }
            }
            // ★危险操作确认态超时：按钮文字还原，避免"⚠ 确认清空？"长期挂着被误点
            if (g_confirmAction != 0 && GetTickCount64() > g_confirmUntil) {
                if (g_confirmAction == 1) SetButtonText(hwnd, ID_BTN_CLEAR, L"🗑 清空对话");
                else if (g_confirmAction == 2) SetButtonText(hwnd, ID_BTN_MEMORY, L"🧠 记忆");
                g_confirmAction = 0;
                if (g_hStatusLabel && IsWindow(g_hStatusLabel))
                    SetWindowTextW(g_hStatusLabel, L"");
            }
            if (g_isChatting && g_hStatusLabel && IsWindow(g_hStatusLabel)) {
                static int s_dots = 0;
                s_dots = (s_dots + 1) % 4;
                std::wstring s = L"茉子正在思考" + std::wstring((size_t)s_dots, L'.');
                if (g_typewriting && GetUIConfigInt(L"TypewriterSkipMode", 1, 0, 2) != 0)
                    s += L"（点击聊天窗任意处立即显示全文）";
                SetWindowTextW(g_hStatusLabel, s.c_str());
            }
        }
        break;
    // ★附件入口①：把图片/文件直接拖进聊天窗（含子控件区域，由系统沿父链派发）
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wParam;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::wstring firstErr;
        int added = 0;
        for (UINT i = 0; i < n; ++i) {
            std::wstring path = DragFilePath(hd, i);
            if (path.empty()) continue;
            std::wstring err;
            if (AddAttachmentFromPath(path, err)) ++added;
            else if (firstErr.empty()) firstErr = err;
        }
        DragFinish(hd);
        RefreshAttachStrip();
        if (!firstErr.empty())      ChatSetStatus(hwnd, L"⚠ " + firstErr);
        else if (added > 0)         ChatSetStatus(hwnd, L"已添加 " + std::to_wstring(added)
                                        + L" 个附件，按 Enter 发送");
        return 0;
    }
    // ★语音转写结果：填进输入框让玩家确认，绝不自动发送（识别错一个字就发出去最坑）
    case WM_APP_ASR_DONE: {
        std::wstring* text = (std::wstring*)lParam;
        if (text) {
            if (!text->empty() && g_hInputEdit && IsWindow(g_hInputEdit)) {
                int len = GetWindowTextLengthW(g_hInputEdit);
                std::wstring cur((size_t)len + 1, L'\0');
                GetWindowTextW(g_hInputEdit, &cur[0], len + 1);
                cur.resize((size_t)len);
                if (!cur.empty()) cur += L" ";
                cur += *text;
                SetWindowTextW(g_hInputEdit, cur.c_str());
                SendMessageW(g_hInputEdit, EM_SETSEL, (WPARAM)cur.size(), (LPARAM)cur.size());
                SetFocus(g_hInputEdit);
                ChatSetStatus(hwnd, L"语音已转成文字，确认无误后按 Enter 发送");
            } else {
                ChatSetStatus(hwnd, L"没听清，靠近麦克风再说一次？");
            }
            delete text;
        }
        return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, ID_TIMER_STATUS);
        DestroyWindow(hwnd); break;
    case WM_DESTROY:
        // ★ 窗口销毁后子控件句柄全部失效：必须一并置空；
        //   否则触摸追加 / 聊天写入会访问已销毁的 g_hHistoryEdit（悬空句柄）。
        g_hChatWnd = nullptr;
        g_hHistoryEdit = nullptr;
        g_hInputEdit = nullptr;
        g_hSendBtn = nullptr;
        g_isChatting = false;
        g_typewriting = false;
        g_typewriterSkip.store(false);
        // ★附件：关窗即丢弃待发送队列，并释放缩略图 GDI+ 对象
        g_hAttachStrip = nullptr;
        ClearAttachThumbs();
        g_pendingAttachments.clear();
        g_oldInputProc = nullptr;   // 旧窗口的子类化信息已失效，下次重建要重新挂
        VoiceAbortRecording();      // 正在录音就立刻停掉，别让 waveIn 回调摸到已销毁窗口
        if (g_hStatusLabel) { g_hStatusLabel = nullptr; }
        break;
    default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================================================================
//  ★ 聊天窗辅助函数（U2 提交 / U6 清空·记忆·长短文 / U3 跳过提示）
// ==================================================================

// 写回 llm.ini 的数值配置（聊天窗"长文/短文"开关用）
static void SetLLMConfigInt(const wchar_t* key, int val, int lo, int hi) {
    if (val < lo) val = lo;
    if (val > hi) val = hi;
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return;
    IniSetValue(exeDir + L"\\llm.ini", L"LLM", key, std::to_wstring(val));
}

// 写回 llm.ini 的 [UI] 段（主题/打字机开关等偏好持久化）
static void SetUIConfigInt(const wchar_t* key, int val, int lo, int hi) {
    if (val < lo) val = lo;
    if (val > hi) val = hi;
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return;
    IniSetValue(exeDir + L"\\llm.ini", L"UI", key, std::to_wstring(val));
}

static void SetButtonText(HWND hwnd, int id, const wchar_t* text) {
    HWND b = GetDlgItem(hwnd, id);
    if (!b) return;
    SetWindowTextW(b, text);
    InvalidateRect(b, nullptr, TRUE);   // 自绘按钮需显式重绘才立刻换字
}

// 触摸气泡：把整段回复截成 1-2 句放进贴角色轻气泡。
//   先按标点（。！？～.!?）切到第 2 个断点，再按 60 字硬顶，保证气泡不刷屏。
static std::wstring TruncateReply(const std::wstring& s) {
    if (s.empty()) return L"";
    // 先把换行/回车换成空格（气泡里不显示换行），再按标点切到
    //   第 2 个断点；若整段没到 2 个断点，则用 60 字硬顶兜底。
    std::wstring flat;
    flat.reserve(s.size());
    for (wchar_t c : s)
        flat += (c == L'\r' || c == L'\n') ? L' ' : c;

    const wchar_t* stops = L"。！？～.!?";
    size_t cut = std::wstring::npos;
    int dots = 0;
    for (size_t i = 0; i < flat.size(); ++i) {
        if (wcschr(stops, flat[i])) {
            ++dots;
            if (dots >= 2) { cut = i + 1; break; }   // 含断点标点本身
        }
    }
    // 没凑够 2 句：按 60 字硬顶；凑够了但那句特别长也仍受 60 字限制。
    const size_t hardMax = 60;
    size_t end = (cut == std::wstring::npos) ? flat.size() : cut;
    if (end > hardMax) end = hardMax;
    std::wstring r = flat.substr(0, end);
    // 硬顶截断且后面还有内容时补省略号，气泡里一眼看出"还有下文"
    if (end < flat.size() && r.size() >= hardMax) {
        if (r.size() > hardMax - 1) r.resize(hardMax - 1);
        r += L"…";
    }
    return r;
}

// 打字机跳过提示：打字时在状态栏/输入框旁显示"点击跳过"小字。
//   TypewriterSkipMode=0 时不提示（跳过已禁用）。
static void ShowTypewriterSkipHint() {
    if (g_hStatusLabel && IsWindow(g_hStatusLabel) &&
        GetUIConfigInt(L"TypewriterSkipMode", 1, 0, 2) != 0)
        SetWindowTextW(g_hStatusLabel, L"（正在显示中… 点击聊天窗任意处立即显示全文）");
}

// ★危险操作"上膛"：按钮切到确认态 + 状态栏说明，5 秒内再点一次才真删。
//   记忆 / 对话记录都是不可恢复数据，误点一下全丢代价太大。
static void ArmDangerousAction(HWND hwnd, int action) {
    g_confirmAction = action;
    g_confirmUntil = GetTickCount64() + CONFIRM_WINDOW_MS;
    if (action == 1) {
        // 按钮只有 96px 宽，确认态用短文案，具体删什么写在状态栏
        if (hwnd) SetButtonText(hwnd, ID_BTN_CLEAR, L"⚠ 确认？");
        if (g_hStatusLabel && IsWindow(g_hStatusLabel))
            SetWindowTextW(g_hStatusLabel,
                L"⚠ 再点一次「清空对话」按钮将永久删除全部对话记录（5 秒内有效，超时自动取消）");
    } else if (action == 2) {
        if (hwnd) SetButtonText(hwnd, ID_BTN_MEMORY, L"⚠ 确认？");
        if (g_hStatusLabel && IsWindow(g_hStatusLabel))
            SetWindowTextW(g_hStatusLabel,
                L"⚠ 再点一次「记忆」按钮将永久删除 memory.txt（5 秒内有效，超时自动取消）");
    }
}

// 清空对话：清 chat_history 文件并刷新窗口
//   注意：调用方必须先经 ArmDangerousAction 二次确认，本函数不再自行询问。
static void ClearChatHistory() {
    std::wstring exeDir;
    if (GetExeDir(exeDir)) {
        std::wstring p = exeDir + L"\\chat_history.txt";
        {
            std::lock_guard<std::recursive_mutex> lk(g_historyFileMtx);
            _wremove(p.c_str());
        }
    }
    if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) {
        SetWindowTextW(g_hHistoryEdit, L"（对话已清空，开始新的对话吧~）\r\n\r\n");
        SendMessageW(g_hHistoryEdit, EM_LINESCROLL, 0, 10000);
        ApplyChatTheme(g_hChatWnd);      // 占位文字也要跟着主题上色
    }
    g_pendingUserInput.clear();
    g_streamCr = false; g_nlRun = 0;
}

// ★危险操作：清空长期记忆（memory.txt）。调用前必须二次确认。
static void ClearMemoryFile() {
    std::wstring exeDir;
    if (GetExeDir(exeDir)) {
        std::wstring p = exeDir + L"\\memory.txt";
        std::lock_guard<std::recursive_mutex> lk(g_historyFileMtx);
        _wremove(p.c_str());
    }
    InvalidateMemoryCache();     // 内部已清 g_memoryCache 并置 loaded=false
}

// 记忆：用系统默认程序打开 memory.txt
static void OpenMemoryFile() {
    // 缓存失效：用户点"记忆"按钮查看/编辑 memory.txt，关闭后下一轮对话
    //   必须读到改过的内容——打开前就把缓存作废，下次 BuildMemoryContext 重新读盘。
    InvalidateMemoryCache();
    std::wstring exeDir;
    if (!GetExeDir(exeDir)) return;
    std::wstring p = exeDir + L"\\memory.txt";
    if (!PathFileExistsW(p.c_str()))
        ShellExecuteW(nullptr, L"open", L"notepad.exe", L"", nullptr, SW_SHOWNORMAL);
    else
        ShellExecuteW(nullptr, L"open", p.c_str(), L"", nullptr, SW_SHOWNORMAL);
}

// ==================================================================
//  输入框键盘：RichEdit 自带 WndProc，按键发给它自身而非父窗口。
//  这里子类化 g_hInputEdit，截获 WM_KEYDOWN：
//    Enter        -> 提交（等价于点"发送"）
//    Shift+Enter -> 换行（交给 RichEdit 默认处理）
//    Esc          -> 关闭聊天窗
// ==================================================================

static LRESULT CALLBACK InputSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && h == g_hInputEdit) {
        UINT vk = (UINT)wp;
        bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (vk == VK_RETURN && !shiftDown && !g_isChatting) {
            // Enter（无 Shift）：提交。需要父窗口句柄投递 WM_COMMAND。
            HWND owner = GetParent(h);
            if (owner) SendMessageW(owner, WM_COMMAND, MAKELPARAM(103, BN_CLICKED), 0);
            return 0;
        }
        if (vk == VK_ESCAPE) {
            HWND owner = GetParent(h);
            if (owner) SendMessageW(owner, WM_CLOSE, 0, 0);
            return 0;
        }
        // ★附件入口②：Ctrl+V 粘贴。剪贴板里是文件（资源管理器复制）或纯位图（截图工具）
        //   时做成附件；同时带文字时优先按普通文本粘贴，避免抢掉用户的文字。
        if (vk == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            bool hasFiles = IsClipboardFormatAvailable(CF_HDROP) != FALSE;
            bool hasDib   = IsClipboardFormatAvailable(CF_DIB) != FALSE;
            bool hasText  = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
            if (hasFiles || (hasDib && !hasText)) {
                HWND owner = GetParent(h);
                if (owner) {
                    std::wstring err;
                    if (!AttachFromClipboard(owner, err) && !err.empty())
                        ChatSetStatus(owner, L"⚠ " + err);
                }
                return 0;
            }
        }
        // Shift+Enter 及所有其它键：交给原 RichEdit 默认处理（Enter 换行）
    }
    if (g_oldInputProc)
        return CallWindowProcW(g_oldInputProc, h, msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

// 共用提交入口：按钮(103) 和 回车(WM_KEYDOWN) 都走这里
static void SubmitChat(HWND hwnd) {
    if (g_isChatting || !g_hInputEdit) return;
    // 先把已经解析完的附件收进来；还有在后台跑的（PDF OCR）就先别发，
    //   否则发出去的是一个空内容占位附件，模型只会回一句"没看到内容"。
    AttachDrainResults();
    if (AttachHasLoading()) {
        ChatSetStatus(hwnd, L"⏳ 附件还在解析中，稍等一下再发送");
        if (g_hInputEdit) SetFocus(g_hInputEdit);
        return;
    }
    // 按控件实际长度分配（上限 64KB）：固定缓冲会静默截断长消息。
    int need = GetWindowTextLengthW(g_hInputEdit);
    if (need < 0) need = 0;
    if (need > 64 * 1024) need = 64 * 1024;
    std::vector<wchar_t> buf((size_t)need + 1, L'\0');
    GetWindowTextW(g_hInputEdit, buf.data(), (int)buf.size());
    buf[buf.size() - 1] = L'\0';
    // 只有附件、没有文字也允许发送（"你看这是什么"的意图由缺省话术补足）
    if (buf[0] == 0 && g_pendingAttachments.empty()) return;
    g_isChatting = true;
    g_lastUserInputMs = GetTickCount64();          // 重置主动搭话静默计时，别打断用户
    g_stat.chatChars += (int)wcslen(buf.data());   // ★ 累计聊天字数（只统计主人发送）
    // ★ 好感度由模型回复里的 affection 字段决定（WM_APP_AFFINITY 结算）
    AchCheck();
    if (g_hSendBtn) EnableWindow(g_hSendBtn, FALSE);
    SetWindowTextW(g_hInputEdit, L"");
    std::wstring userText(buf.data());
    if (userText.empty()) userText = L"看看这个～";
    std::wstring attachTag = BuildAttachmentHistoryTag(g_pendingAttachments);
    g_streamCr = false; g_nlRun = 0;
    TtsStopAll();                                   // 新一轮开始：掐掉上一轮语音
    g_pendingUserInput = attachTag + userText;      // 回复到达时配对（M4 起落盘在线程内）
    AppendChatView(L"主人：" + attachTag + userText + L"\r\n\r\n茉子：思考中...");
    // 开启新会话：递增全局序号并设为期望序号
    int seq = ++g_chatSessionSeq;
    g_chatExpectedSession = seq;
    LLMThreadParam* param = new LLMThreadParam();
    param->hwnd = hwnd; param->input = userText; param->session = seq;
    // ★附件：队列拷给线程（线程只读），主线程立刻清空，附件不会重复发送
    param->attachments = g_pendingAttachments;
    param->historyTag = attachTag;
    CacheAttachmentFiles(param->attachments);   // 图片留一份到 attach_cache\ 供回看
    g_pendingAttachments.clear();
    ClearAttachThumbs();
    g_attachHover = -1;
    if (g_hAttachStrip && IsWindow(g_hAttachStrip)) ShowWindow(g_hAttachStrip, SW_HIDE);
    LayoutChatChildren(hwnd);
    g_typewriting = true;
    ShowTypewriterSkipHint();
    // 线程创建失败时必须复位所有"进行中"标志。
    //   忽略返回值时，一旦 _beginthreadex 失败（句柄/内存耗尽），
    //   g_isChatting 永远停在 true，输入框和发送键从此禁用，表现为"聊天卡死"。
    if (!_beginthreadex(nullptr, 0, LLMThreadProc, param, 0, nullptr)) {
        delete param;
        g_isChatting = false;
        g_typewriting = false;
        g_pendingUserInput.clear();
        if (g_hSendBtn) EnableWindow(g_hSendBtn, TRUE);
        AppendChatView(L"（系统）启动回复线程失败，请重新发送。");
        ChatSetStatus(hwnd, L"⚠ 启动回复线程失败，请重试");
    }
}

// ==================================================================
// 聊天窗位置/大小记忆：复用 config.ini 的 [Chat] 段，记住上次
//   位置、尺寸，以及"上次是否开着"（可选）。缺省时回落到主窗居中。
// ==================================================================
static void SaveChatWindowRect() {
    if (!g_hChatWnd || !IsWindow(g_hChatWnd)) return;
    std::wstring ini;
    if (!ConfigIniPath(ini)) return;
    RECT rc;
    if (!GetWindowRect(g_hChatWnd, &rc)) return;
    wchar_t buf[32];
    wsprintfW(buf, L"%d", rc.left);  WritePrivateProfileStringW(L"Chat", L"X", buf, ini.c_str());
    wsprintfW(buf, L"%d", rc.top);   WritePrivateProfileStringW(L"Chat", L"Y", buf, ini.c_str());
    wsprintfW(buf, L"%d", rc.right - rc.left); WritePrivateProfileStringW(L"Chat", L"W", buf, ini.c_str());
    wsprintfW(buf, L"%d", rc.bottom - rc.top); WritePrivateProfileStringW(L"Chat", L"H", buf, ini.c_str());
}

static void OpenChatDialog() {
    if (g_hChatWnd && IsWindow(g_hChatWnd)) { SetForegroundWindow(g_hChatWnd); return; }
    if (!g_richEditLoaded) { LoadLibraryW(L"Msftedit.dll"); g_richEditLoaded = true; }
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = ChatWndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MakotoChatWndClass"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.cbSize = sizeof(WNDCLASSEXW);
    RegisterClassExW(&wc);
    // 优先用上次记住的位置/尺寸，缺省再按主窗居中
    int x = 0, y = 0, w = 490, h = 520;
    bool restored = false;
    {
        std::wstring ini;
        if (ConfigIniPath(ini)) {
            wchar_t b[32];
            GetPrivateProfileStringW(L"Chat", L"X", L"", b, 32, ini.c_str());
            int rx = _wtoi(b);
            GetPrivateProfileStringW(L"Chat", L"Y", L"", b, 32, ini.c_str());
            int ry = _wtoi(b);
            GetPrivateProfileStringW(L"Chat", L"W", L"", b, 32, ini.c_str());
            int rw = _wtoi(b);
            GetPrivateProfileStringW(L"Chat", L"H", L"", b, 32, ini.c_str());
            int rh = _wtoi(b);
            if (rw >= 400) w = rw;
            if (rh >= 430) h = rh;      // 附件条出现后 350 太矮，低于 430 用默认高度
            bool posOk = (rx < 0 || ry < 0 || rx > 0 || ry > 0);
            if (posOk && (rw > 0 || rh > 0)) { x = rx; y = ry; restored = true; }
        }
    }
    if (!restored) {
        RECT ownerRect; GetWinRect(g_hwnd, ownerRect);
        x = ownerRect.left + (ownerRect.right - ownerRect.left) / 2 - w / 2;
        y = ownerRect.top + (ownerRect.bottom - ownerRect.top) / 2 - h / 2;
    }
    g_hChatWnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"MakotoChatWndClass", L"💬 和茉子聊天",
        WS_POPUPWINDOW | WS_CAPTION | WS_VISIBLE | WS_THICKFRAME | WS_MAXIMIZEBOX,
        x, y, w, h, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// 聊天窗位置/尺寸变过（拖完/调完）静默 0.8s 落盘，复用主窗 MarkGeomDirty 思路。
static ULONGLONG g_chatGeomDirtyMs = 0;
static bool g_chatGeomDirty = false;
static void MarkChatGeomDirty() {
    g_chatGeomDirty = true;
    g_chatGeomDirtyMs = GetTickCount64();
}
static void MaybeSaveChatGeom() {
    if (!g_chatGeomDirty) return;
    if (g_chatGeomDirtyMs != 0 && GetTickCount64() - g_chatGeomDirtyMs < 800) return;
    SaveChatWindowRect();
    g_chatGeomDirty = false;
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
    // ★安全：音频下载设 128MB 上限，分块 8MB，防止异常服务端把内存拖垮
    const size_t kMaxDownload = 128u * 1024u * 1024u;
    DWORD dwSize = 0, dwDownloaded = 0;
    while (WinHttpQueryDataAvailable(hReq, &dwSize) && dwSize > 0) {
        if (out.size() >= kMaxDownload) break;
        if (dwSize > 8u * 1024u * 1024u) dwSize = 8u * 1024u * 1024u;
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
    // ★安全：这里只取 openapi.json / 普通文本，8MB 足够，避免异常响应吃满内存
    const size_t kMaxText = 8u * 1024u * 1024u;
    std::string resp;
    DWORD dwSize = 0, dwDownloaded = 0;
    while (WinHttpQueryDataAvailable(hReq, &dwSize) && dwSize > 0) {
        if (resp.size() >= kMaxText) break;
        if (dwSize > 1024u * 1024u) dwSize = 1024u * 1024u;
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
    int blockAlign = 0;
    std::vector<uint8_t> rawPcm;
    while (pos + 8 <= wav.size()) {
        char cid[4]; DWORD cs;
        memcpy(cid, wav.data() + pos, 4);
        memcpy(&cs, wav.data() + pos + 4, 4);
        if (!memcmp(cid, "fmt ", 4)) {
            WAVEFORMATEX tmp{};
            // ★安全：同 LoadWavFromMemory，fmt 声明长度不可信，按实际剩余字节裁剪
            size_t availFmt = wav.size() - ((size_t)pos + 8);
            DWORD tr = (DWORD)min((size_t)cs, sizeof(WAVEFORMATEX));
            if ((size_t)tr > availFmt) tr = (DWORD)availFmt;
            if (tr) memcpy(&tmp, wav.data() + pos + 8, tr);
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
        // cs 来自文件头，畸形文件可能给到 0xFFFFFFxx；直接相加会让 DWORD
        // 回绕成很小的值，循环条件重新成立 → 死循环。三处 WAV 解析都要防。
        unsigned long long step = 8ull + cs + (cs & 1u);
        unsigned long long next = (unsigned long long)pos + step;
        if (next > 0xFFFFFFFFull) break;
        pos = (DWORD)next;
    }
    if (out.sampleRate == 0 || out.channels == 0 || out.bitsPerSample == 0 || rawPcm.empty()) return false;
    // 外部 WAV 不可信：先卡死格式上限，避免畸形头让 samples 申请到 GB 级内存。
    if (out.sampleRate < 8000 || out.sampleRate > 384000 ||
        out.channels < 1 || out.channels > 8 ||
        !(out.bitsPerSample == 8 || out.bitsPerSample == 16 ||
          out.bitsPerSample == 24 || out.bitsPerSample == 32)) return false;
    if (rawPcm.size() > (size_t)256 * 1024 * 1024) return false;

    int bytesPerSample = out.bitsPerSample / 8;
    if (bytesPerSample <= 0) return false;
    // ★安全：blockAlign 来自文件头，可能比"单帧真实字节数"小（畸形/伪造 WAV）。
    //   若直接当步长，循环会按更小的 stride 走更多帧，最后一帧读样本时越过 rawPcm 末尾。
    //   这里强制不小于 bytesPerSample*channels，保证读取范围一定落在缓冲区内。
    int needFrame = bytesPerSample * out.channels;
    if (blockAlign <= 0 || blockAlign < needFrame) blockAlign = needFrame;
    int frameSize = blockAlign;
    int numFrames = (int)(rawPcm.size() / frameSize);
    size_t sampleCount = (size_t)numFrames * (size_t)out.channels;
    // samples 是 int32_t：8bit 输入会放大 4 倍，必须按结果数组大小设上限。
    if (sampleCount > (size_t)32 * 1024 * 1024) return false;
    out.numSamples = numFrames;
    out.samples.resize(sampleCount);

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
    // ★ 变调未解锁时强制按原调转换。g_rvcOctave 可能来自旧存档或开发模式留下的值，
    //   而菜单此时是灰的、用户改不动，若照发就会变成"没解锁却仍在变调"。
    //   这里只影响本次请求，不清 g_rvcOctave，解锁后用户的选择仍然有效。
    int octToSend = UnlockReady(UNLOCK_OCTAVE) ? g_rvcOctave : 0;
    body += make_field("octave", std::to_string(octToSend));
    // ★ index_rate 不要给 1.0：检索权重越高，源音高偏离模型音域时越容易被
    //   最近邻拉到低八度样本上（表现就是高音掉八度）。
    //   同性转换可用 0.7 保音色；跨性别/大跨度由 bridge 自动再降到 0.3。
    body += make_field("index_rate", "0.5");
    body += make_field("protect",    "0.33");
    body += make_field("separate",   "1");       // ★ 告诉 bridge 先分离人声伴奏

    std::string fileName = WideStringToUtf8(std::wstring(PathFindFileNameW(path.c_str())));
    // multipart 的 filename 用双引号包住，文件名里的 " / CR / LF 会破坏协议边界。
    // RFC 7578 建议直接替换掉这些字符，而不是转义（很多服务端不认 %22）。
    {
        std::string safe;
        for (unsigned char c : fileName) {
            if (c == '"' || c == '\r' || c == '\n') safe += '_';
            else safe += (char)c;
        }
        fileName = safe;
    }
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
    WinHttpAddRequestHeaders(hReq, ct.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(hReq, NULL, 0, (LPVOID)payload.data(), (DWORD)payload.size(), (DWORD)payload.size(), 0)) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false;
    }
    WinHttpReceiveResponse(hReq, NULL);

    // ★安全：RVC 任务状态 JSON，8MB 上限足矣
    const size_t kMaxRvcJson = 8u * 1024u * 1024u;
    std::string resp;
    DWORD dwSize = 0, dwDownloaded = 0;
    while (WinHttpQueryDataAvailable(hReq, &dwSize) && dwSize > 0) {
        if (resp.size() >= kMaxRvcJson) break;
        if (dwSize > 1024u * 1024u) dwSize = 1024u * 1024u;
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
    // 统计、成就气泡与存档统一交回主线程：ToastShow 会创建窗口，
    // 从工作线程调 UI 属于跨线程操作；g_stat 也不该被后台线程直接改写。
    if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_APP_RVC_DONE, 0, 0);

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


// ================= 右键菜单 =================
void ShowContextMenu(HWND hwnd) {
    static bool s_opening = false;
    if (s_opening) return;      // 防重入：菜单已弹出时不再嵌套弹一次
    s_opening = true;
    // 守卫：函数任何路径退出（含提前 return）都能自动复位，
    // 否则一次异常之后右键菜单就再也打不开了。
    struct OpeningGuard { bool* p; ~OpeningGuard() { *p = false; } } openingGuard{ &s_opening };
    (void)openingGuard;   // 消除"已初始化但未引用"告警，析构本身才是目的
    POINT pt; GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    HMENU outfits = CreatePopupMenu();
    HMENU renderMenu = CreatePopupMenu();
    HMENU volumeMenu = CreatePopupMenu();
    HMENU bgmMenu = CreatePopupMenu();
    HMENU octMenu = CreatePopupMenu();
    // ★ 每次弹菜单都重扫一遍素材：用户可能刚把 Animations 或 live2d 拷进来，
    //   不必重开程序就能看到新选项。只查存在性，开销可以忽略。
    DetectAssets();

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
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"⚙ 设置");
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

    // ---- 渲染方式：序列帧 / Live2D ----
    // 装了哪套就放开哪一项；缺素材的那项灰掉并写明缺什么，从菜单层面就不给切。
    {
        wchar_t rTitle[64];
        // 标题写"实际正在用的"：某套服装没模型被自动降级时，这一行会如实变成"序列帧"
        swprintf(rTitle, 64, L"🖼 渲染方式（%s）", Live2DReady() ? L"Live2D" : L"序列帧");
        AppendMenuW(menu, MF_STRING | MF_POPUP, (UINT_PTR)renderMenu, rTitle);

        std::wstring frameItem = g_hasFrameAssets
            ? L"序列帧（Animations）"
            : L"序列帧（未找到 Animations 素材）";
        AppendMenuW(renderMenu, MF_STRING
            | (!Live2DReady() ? MF_CHECKED : 0)
            | (g_hasFrameAssets ? 0 : MF_DISABLED | MF_GRAYED),
            ID_RENDER_FRAMES, frameItem.c_str());

#if USE_LIVE2D
        std::wstring l2dItem = g_hasLive2DAssets
            ? L"Live2D（模型渲染）"
            : L"Live2D（未找到模型或 p2l_runtime.dll）";
#else
        std::wstring l2dItem = L"Live2D（本版本未编译）";
#endif
        AppendMenuW(renderMenu, MF_STRING
            | (Live2DReady() ? MF_CHECKED : 0)
            | (g_hasLive2DAssets ? 0 : MF_DISABLED | MF_GRAYED),
            ID_RENDER_LIVE2D, l2dItem.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | MF_POPUP, (UINT_PTR)volumeMenu, L"音量");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 0 ? MF_CHECKED : 0), ID_VOLUME_MUTE, L"静音");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 300 ? MF_CHECKED : 0), ID_VOLUME_LOW, L"低");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 700 ? MF_CHECKED : 0), ID_VOLUME_MID, L"中");
    AppendMenuW(volumeMenu, MF_STRING | (g_currentVolume == 1000 ? MF_CHECKED : 0), ID_VOLUME_HIGH, L"高");

    // ★ 语音朗读开关：关掉只是不播语音，LLM/表情/动作照常，
    //   想安静看字的时候不用把 GPT-SoVITS 服务关掉。
    {
        bool on = g_ttsVoiceOn.load();
        AppendMenuW(menu, MF_STRING | (on ? MF_CHECKED : 0), ID_TTS_TOGGLE,
            on ? L"🔊 语音朗读（开）" : L"🔇 语音朗读（关）");
    }

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
    // ★ 标题带当前半音数，方便确认这一次翻唱用的是哪一档
    if (octSemi == 0) wsprintfW(octTitle, L"🎤 翻唱变调（原调）");
    else              wsprintfW(octTitle, L"🎤 翻唱变调（当前 %+d 半音）", octSemi);
    bool octOk = rvcOk && UnlockReady(UNLOCK_OCTAVE);
    AppendMenuW(menu, MF_STRING | MF_POPUP | (octOk ? 0 : MF_DISABLED | MF_GRAYED),
        (UINT_PTR)octMenu, octOk ? octTitle : lockName(UNLOCK_OCTAVE, L"🎤 翻唱变调").c_str());
    wchar_t octDown[64], octUp[64];
    wsprintfW(octDown, L"降调 (%d 半音)", -RVC_OCTAVE_SEMI);
    wsprintfW(octUp,   L"升调 (%+d 半音)", RVC_OCTAVE_SEMI);
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
    case ID_SETTINGS: OpenSettingsDialog(); break;
    case ID_ACH_PANEL: OpenAchPanel(); break;
    case ID_TRAY_EXIT: DestroyWindow(hwnd); break;
    // 🖼 渲染方式切换：素材缺失的项已经被灰掉，这里再兜一层，避免快捷键绕进来
    case ID_RENDER_FRAMES: SetRenderMode(RM_FRAMES); break;
    case ID_RENDER_LIVE2D: SetRenderMode(RM_LIVE2D); break;
    case ID_TTS_TOGGLE: {
        bool on = !g_ttsVoiceOn.load();
        g_ttsVoiceOn.store(on);
        if (!on) TtsStopAll();          // 关掉时正在念的那句也立刻停
        // 写回 llm.ini [TTS]：以前这里用 WritePrivateProfileStringW 写 UTF-8 文件，
        // 会把同段的 TtsRefText 一起按 ANSI 重编码成乱码。
        SetIniStr(L"TTS", L"TtsEnable", on ? L"1" : L"0");
        ToastShow(on ? L"🔊" : L"🔇", on ? L"语音朗读已开启" : L"语音朗读已关闭",
                  on ? L"回复会自动念出来。"
                     : L"只显示文字。想再打开：右键 → 语音朗读。");
        break;
    }
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
}

// ==================================================================
//  ★触摸互动（LLM 驱动）：八部位触摸 -> 把"主人摸了某部位"讲给 LLM，
//  由模型自己决定表情/台词/动作/视线；好感仍本地即时结算，养成节奏不受网络影响。
//  - 点击瞬间：好感 + 音效 + 动作 + "……"占位气泡（马上有反馈）
//  - LLM 返回：气泡显示模型反应（含（动作描写）+台词），导演指令同步表情/动作/视线
//  - 关闭 TouchLlmEnable / 网络失败 / 冷却中：退回本地固定反应（离线可用）
// ==================================================================
#if USE_LIVE2D
// 本地降级反馈：不依赖 LLM，按部位给一套固定反应（旧行为保留）。
static void DoBodyTouchLocalFallback(L2dBodyPart part, bool egg, int gain) {
    int  slot = L2D_IDLE;
    PetEmotion emo = PetEmotion::Base;
    int  gaze = -1;
    const wchar_t* icon = L"♥";
    const wchar_t* title = L" touching";
    wchar_t bubble[96] = { 0 };

    if (part == L2D_PART_HEAD) {
        slot = L2D_THINK; emo = egg ? PetEmotion::Smile : PetEmotion::ShyBlush; gaze = L2D_GAZE_DOWN;
        icon = L"♥"; title = egg ? L"害羞了" : L"摸摸头";
        swprintf(bubble, 96, egg ? L"唉，别一直摸了啦……（好感 +%d）"
                                 : L"唔……摸摸头吗？（好感 +%d）", gain);
    } else if (part == L2D_PART_FACE) {
        slot = L2D_PEACE; emo = PetEmotion::ShyBlush; gaze = L2D_GAZE_AWAY;
        icon = L"💕"; title = L"戳戳脸";
        swprintf(bubble, 96, L"呜…！别戳脸啦，会、会害羞的！（好感 +%d）", gain);
    } else if (part == L2D_PART_HAND) {
        slot = L2D_PEACE; emo = PetEmotion::ShyBlush; gaze = L2D_GAZE_DOWN;   // 牵手的反应是害羞，不是开心
        icon = L"💗"; title = L"牵牵手";
        swprintf(bubble, 96, L"牵、牵手？笨、笨蛋……（好感 +%d）", gain);
    } else if (part == L2D_PART_CHEST) {
        slot = L2D_PEACE; emo = PetEmotion::ShyBlush; gaze = L2D_GAZE_DOWN;
        icon = L"💗"; title = L"碰到了";
        swprintf(bubble, 96, L"呀！那、那里……别乱碰！（好感 +%d）", gain);
    } else if (part == L2D_PART_BELLY) {
        slot = L2D_THINK; emo = PetEmotion::Panic; gaze = L2D_GAZE_DOWN;
        icon = L"✿"; title = L"揉揉肚子";
        swprintf(bubble, 96, L"噗……别挠，痒、痒！（好感 +%d）", gain);
    } else if (part == L2D_PART_SKIRT) {
        slot = L2D_PEACE; emo = PetEmotion::ShyBlush; gaze = L2D_GAZE_DOWN;
        icon = L"✿"; title = L"扯裙子";
        swprintf(bubble, 96, L"呀！别、别扯裙子啦……（好感 +%d）", gain);
    } else if (part == L2D_PART_LEG) {
        slot = L2D_THINK; emo = PetEmotion::ShyBlush; gaze = L2D_GAZE_AWAY;
        icon = L"✿"; title = L"碰到了";
        swprintf(bubble, 96, L"腿、腿……吓我一跳！（好感 +%d）", gain);
    } else {  // L2D_PART_FOOT
        slot = L2D_THINK; emo = PetEmotion::Panic; gaze = L2D_GAZE_AWAY;
        icon = L"✿"; title = L"碰到了";
        swprintf(bubble, 96, L"唔……脚痒痒的，别闹。");
    }

    if (slot == L2D_PEACE && !L2dSlotUnlocked(L2D_PEACE)) slot = L2D_THINK;
    L2dPlayClip(slot);
    L2dSetExpression(emo);
    if (gaze >= 0) L2dSetGazeOverride(gaze, 1800);
    (void)icon; (void)title;   // 气泡已停用，图标/标题不再使用（保留分支里的赋值以免大改）

    // ★ 不再弹气泡：旁白 + 本地台词写进对话界面，并进入对话历史。
    std::wstring narr = std::wstring(L"（") + L2dTouchNarration(part) + L"）";
    AppendChatView(narr + L"\r\n\r\n茉子：" + std::wstring(bubble));
    EnsureHistoryBlankLine();      // 这一轮到此结束，给下一轮留出空行
    SaveChatHistory(narr, bubble);
}

static void DoBodyTouch(HWND hwnd, L2dBodyPart part) {
    if (!Live2DReady()) return;
    if (part == L2D_PART_NONE) return;

    ULONGLONG now = GetTickCount64();
    bool egg = false;
    int  gain = 1;

    // 好感规则（本地即时，不交给 LLM）：摸头连击彩蛋照旧；脸/手/胸/腹/裙/腿 +1，脚 +0。
    if (part == L2D_PART_HEAD) {
        bool combo = (g_lastPatMs != 0 && now - g_lastPatMs <= PAT_COMBO_WINDOW_MS);
        g_patCombo = combo ? g_patCombo + 1 : 1;
        g_lastPatMs = now;
        egg = (g_patCombo >= PAT_COMBO_NEED && now - g_lastPatEggMs >= PAT_REWARD_COOLDOWN);
        if (egg) { g_lastPatEggMs = now; g_patCombo = 0; }
        gain = egg ? 2 : 1;
    } else if (part == L2D_PART_FOOT) {
        gain = 0;
    }

    bool applied = false;
    if (gain > 0) {
        applied = StatsAddAffinity(gain);
        if (applied && g_stat.affinityMood < 0) {
            std::lock_guard<std::recursive_mutex> lk(g_statMtx);
            g_stat.affinityMood += 1;
            if (g_stat.affinityMood > 0) g_stat.affinityMood = 0;
        }
    }
    if (applied) {
        UnlockCheck();
        AffinityMaxCheck();
        AchCheck();
        StatsSave();
        RequestPaint(true);
        if (g_hAchPanel && IsWindow(g_hAchPanel)) InvalidateRect(g_hAchPanel, nullptr, FALSE);
    }

    // 即时反馈：音效 + 一个通用动作，让用户点了立刻有反应（LLM 有网络延迟）。
    XSoundPlay(g_peace, false);
    if (egg) XSoundPlay(g_think, false);
    L2dPlayClip(L2D_THINK);

    // ★ 是否走 LLM 驱动？开关默认开；节流冷却中 / 已有请求在跑 -> 走本地降级。
    bool useLlm = GetLLMConfigInt(L"TouchLlmEnable", 1, 0, 1) != 0;
    int  cooldownMs = GetLLMConfigInt(L"TouchCooldownMs", 1200, 0, 10000);
    bool canAsk = useLlm && !g_touchRunning &&
                  (g_lastTouchAskMs == 0 || now - g_lastTouchAskMs >= (ULONGLONG)cooldownMs);

    if (canAsk) {
        g_touchRunning = true;
        g_lastTouchAskMs = now;
        g_lastUserInputMs = now;   // 重置主动搭话静默计时，避免摸着摸着突然插话
        // 触摸回复呈现层统一：TouchReplyMode
        //   0=bubble（贴角色轻气泡，默认） / 1=openChat（自动开聊天窗） / 2=both
        //   兼容旧配置 TouchAutoOpenChat（=1 等价 mode 1）。
        int mode = GetLLMConfigInt(L"TouchReplyMode", 0, 0, 2);
        if (mode == 0 && GetLLMConfigInt(L"TouchAutoOpenChat", 0, 0, 1) == 1)
            mode = 1;              // 老配置迁移
        if ((mode == 1 || mode == 2) && (!g_hChatWnd || !IsWindow(g_hChatWnd)))
            OpenChatDialog();
        // ★ 不再弹气泡：先在对话界面写下旁白（"茉子："后等回复填充），回复到达时再追加。
        std::wstring narr = std::wstring(L"（") + L2dTouchNarration(part) + L"）";
        g_touchPendingNarration = narr;
        LLMThreadParam* param = new LLMThreadParam();
        param->hwnd = hwnd;
        param->input.clear();
        param->touch = true;
        param->touchPhrase = L2dTouchPhrase(part);
        unsigned tid = 0;
        if (!_beginthreadex(nullptr, 0, LLMThreadProc, param, 0, &tid)) {
            delete param;
            g_touchRunning = false;
            g_touchPendingNarration.clear();
            DoBodyTouchLocalFallback(part, egg, gain);  // 建线程失败 -> 本地反馈
        } else {
            // 线程起来后才写占位，避免和本地降级路径重复追加旁白
            AppendChatView(narr + L"\r\n\r\n茉子：");
        }
    } else {
        // 关闭 / 冷却中 / 正在请求：本地固定反馈，保证每次点击都有回应。
        DoBodyTouchLocalFallback(part, egg, gain);
    }
}
#endif

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
#if L2D_PIXEL_HITTEST
    case WM_NCHITTEST: {
        // 分层窗口默认整个矩形都吃鼠标消息，透明区会挡住桌面点击。
        // 这里按离屏位图的 alpha 做命中：完全透明处让鼠标穿透下去。
        int sx = (int)(short)LOWORD(lp), sy = (int)(short)HIWORD(lp);
        RECT rc;
        if (!GetWindowRect(hwnd, &rc)) return HTNOWHERE;
        int x = sx - rc.left, y = sy - rc.top;
        if (x < 0 || y < 0 || x >= g_cachedW || y >= g_cachedH) return HTTRANSPARENT;
        if (g_dibBits) {
            uint8_t a = g_dibBits[((size_t)y * (size_t)g_cachedW + (size_t)x) * 4 + 3];
            if (a < L2D_HIT_ALPHA_MIN) return HTTRANSPARENT;
        }
        return HTCLIENT;
    }
#endif
    case WM_MOUSEWHEEL: {
        if (!g_preloadDone) return 0;            // 加载期不缩放，避免触发重建
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (delta > 0) g_zoom = min(g_zoom + 0.1f, 3.0f);
        else g_zoom = max(g_zoom - 0.1f, 0.5f);
#if USE_LIVE2D
        if (Live2DReady()) {
            // 窗口宽高比跟着人物走，别再固定 2:3 空出大片透明区。
            Live2DApplyWindowSize();   // 内部会按新位置重算半身状态
            g_zoomLastMs = GetTickCount();
            MarkGeomDirty();                       // ★ 缩放后立即记住尺寸
            UpdateLayered();
            return 0;
        }
#endif
        int newW = (int)(BASE_W * g_zoom), newH = (int)(BASE_H * g_zoom);
        RECT rc; GetWinRect(g_hwnd, rc);
        int nx = rc.left, ny = rc.bottom - newH;      // 以底边为锚缩放
        POINT zoomAnchor{ (rc.left + rc.right) / 2, rc.bottom };
        ClampToMonitor(zoomAnchor, newW, newH, nx, ny);   // 缩放也不许越过拖拽边界
        SetWindowPos(g_hwnd, HWND_TOPMOST, nx, ny, newW, newH, SWP_NOACTIVATE);
        g_currentW = newW; g_currentH = newH;
        g_winX = nx; g_winY = ny; g_winPosKnown = true;   // ★ 同步逻辑位置
        RefreshHalfBodyFromPlacement(ny, newH, { nx + newW / 2, ny + newH });
        g_zoomLastMs = GetTickCount();
        MarkGeomDirty();                              // ★ 缩放后立即记住尺寸
        UpdateLayered();
        return 0;
    }
    case WM_TIMER:
        // RVC 状态栏倒计时：完成/失败显示 4 秒后自动消失
        {
            bool stageEnded = false;
            {
                std::lock_guard<std::recursive_mutex> lk(g_rvcMtx);
                if (g_rvcStatusTimer > 0) {
                    --g_rvcStatusTimer;
                    if (g_rvcStatusTimer == 0) {
                        g_rvcStage = RvcStage::Idle;
                        g_rvcStatusText.clear();
                        g_rvcSourceName.clear();
                        stageEnded = true;
                    }
                }
            }
            if (stageEnded) SwitchAnim(g_currentCostume, AnimState::Idle);
        }
        if (wp == 1) {
            {   // 休眠跨零点不会重启程序，只在 WinMain 检查一次跨天是不够的
                static ULONGLONG s_lastDayCheckMs = 0;
                ULONGLONG dayNow = GetTickCount64();
                if (s_lastDayCheckMs == 0 || dayNow - s_lastDayCheckMs >= 60000) {
                    s_lastDayCheckMs = dayNow;
                    StatsCheckDay();
                }
            }
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
                // ★ 窗口位置/尺寸变过：静默 0.8s 就落盘。拖完、缩放完立刻记住，
                //   不必等正常退出——被任务管理器结束也照样保住。
                if (g_geomDirty && nowMs - g_geomDirtyMs >= 800) SaveWindowState();
                // 聊天窗位置/尺寸同样静默 0.8s 落盘
                MaybeSaveChatGeom();
            }

            // ★触摸请求超时兜底：占位气泡"（……）"若长时间没被回复覆盖，
            //   说明这一轮 LLM 请求卡住了。给出明确提示并复位互斥标志，
            //   避免界面永远停在"（……）"、让用户误以为"连不上 LLM"。
            if (g_touchRunning && g_lastTouchAskMs != 0) {
                int touchTimeout = GetLLMConfigInt(L"TouchTimeoutMs", 20000, 5000, 120000);
                ULONGLONG nowT = GetTickCount64();
                if (nowT - g_lastTouchAskMs > (ULONGLONG)touchTimeout) {
                    g_touchRunning = false;
                    // 错误提示高优先级（插队首，立即展示）
                    if (!g_isChatting)
                        ToastShowHi(L"⚠", L"连接超时",
                            L"这次反应没回来，请检查网络或 llm.ini（DebugView 搜 [LLM]/[TOUCH] 看详情）");
                    PetDbgW(L"[TOUCH] 请求超时，已复位互斥标志\n");
                }
            }

            // 主动搭话调度：每隔一段时间，趁用户没在聊天/拖拽，让茉子主动开口。
            //   配置项都在 llm.ini 的 [LLM] 段。
            if (GetLLMConfigInt(L"ProactiveEnable", 1, 0, 1)) {
                ULONGLONG nowP = GetTickCount64();
                // 线程卡死兜底：超过 90 秒还没回主线程，强制解锁，避免以后再也不搭话
                if (g_proactiveRunning && g_proactiveStartMs != 0 &&
                    nowP - g_proactiveStartMs > 90000) {
                    g_proactiveRunning = false;
                }
                if (!g_proactiveRunning && g_preloadDone && !g_isChatting && !g_bDragging) {
                    if (g_lastProactiveMs == 0) g_lastProactiveMs = nowP;
                    int intervalMs = GetLLMConfigInt(L"ProactiveIntervalSec", 180, 15, 86400) * 1000;
                    int idleMs     = GetLLMConfigInt(L"ProactiveIdleSec", 60, 5, 86400) * 1000;
                    bool quietEnough = (g_lastUserInputMs == 0 || nowP - g_lastUserInputMs >= (ULONGLONG)idleMs);
                    if (nowP - g_lastProactiveMs >= (ULONGLONG)intervalMs && quietEnough) {
                        g_proactiveRunning = true;
                        g_proactiveStartMs = nowP;
                        auto* pp = new LLMThreadParam();
                        pp->hwnd = hwnd; pp->input.clear(); pp->proactive = true;
                        unsigned int tid = 0;   // ★ _beginthreadex 第6参数要求 unsigned int*（x64 下 uintptr_t 是64位，会报 C2664）
                        if (!_beginthreadex(nullptr, 0, LLMThreadProc, pp, 0, &tid)) {
                            delete pp;                       // 起线程失败要还原标志并释放参数
                            g_proactiveRunning = false;
                        }
                    }
                }
            }
            ToastTick();                 // 成就解锁气泡推进（各自独立的分层窗口）

            // ★ 分段语音：上一句放完就接下一句（合成在后台线程，这里只做播放切换）
            TtsPump();

            if (g_bTransitioning && !Live2DReady()) {
                // Live2D 没有"换图淡入淡出"这回事，别让过渡分支吞掉每帧重绘。
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
                    SaveConfig();          // ★ 序列帧换装路径也要立刻记住，别只靠退出时那一次
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
#if USE_LIVE2D
            if (Live2DReady()) {
                // 用定时器 + GetCursorPos 而不是 WM_MOUSEMOVE：鼠标在桌面别处时
                // 分层窗口收不到鼠标消息，但角色仍应该看着它。
                RECT rcLook;
                if (GetWindowRect(hwnd, &rcLook)) Live2DLookAtCursor(rcLook);
                Live2DAdvance();     // 逻辑帧只在这里推进，拖拽重绘不重复推进
                UpdateLayered();
            } else
#endif
            if (!g_currentFrames.empty()) {
                g_currentFrameIndex = (g_currentFrameIndex + 1) % (int)g_currentFrames.size();
                UpdateLayered();
            }
            if (!g_bDragging) {
                if (g_currentState == AnimState::Idle && !g_isRandomAction) {
                    g_idleActionTimer++;
                    if (g_idleActionTimer > 360) {
#if USE_LIVE2D
                        if (Live2DReady()) {
                            // 轮播：待机久了随机做点小动作。片段 2（开心）
                            //   受好感解锁，没到门槛就只在"思考"里挑，不会出现
                            //   状态已切、画面却没动的空档。
#if DEV_UNLOCK_ALL
                            bool peaceOk = true;
#else
                            bool peaceOk = g_stat.affinityTotal >= L2D_PEACE_NEED;
#endif
                            AnimState pick = (peaceOk && rand() % 2 == 0)
                                ? AnimState::Peace : AnimState::Think;
                            SwitchAnim(g_currentCostume, pick);
                            L2dPlayState(pick);   // 强制驱动片段（含循环片段的重启）
                        } else
#endif
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
    // 情绪反馈：Live2D 动作片段 + 音效 + 心情微调（三层反馈的动作/数值层）。
    //   wParam = PetEmotion，lParam = 心情增量（-1/0/1）
    case WM_APP_EMOTION: {
        PetEmotion emo = (PetEmotion)(int)wp;
        int moodDelta = (int)(int)lp;
#if USE_LIVE2D
        if (Live2DReady()) {
            int slot = L2dSlotForEmotion(emo);
            if (L2dSlotUnlocked(slot)) L2dPlayClip(slot);   // 未解锁的开心动作不越级播放
            // ★ 表情联动：情绪驱动面部表情淡入淡出（动作 + 表情 + 语气三合一）。
            //   旧 DLL 没有表情 API 时 L2dSetExpression 内部静默跳过，不影响动作。
            L2dSetExpression(emo);
            // 音效：正向情绪用 peace，低落/思考用 think，复用现有素材
            if (emo == PetEmotion::Smile) XSoundPlay(g_peace, false);
            else if (emo == PetEmotion::Embarrassed || emo == PetEmotion::Thinking) XSoundPlay(g_think, false);
        }
#endif
        // 心情微调：只改 affinityMood（影响爱心液面），不动累计值；与对话好感结算互补。
        if (moodDelta != 0) {
            std::lock_guard<std::recursive_mutex> lk(g_statMtx);
            g_stat.affinityMood += moodDelta;
            if (g_stat.affinityMood > 0) g_stat.affinityMood = 0;        // 正心情封顶 0
            if (g_stat.affinityMood < -AFFINITY_MOOD_FLOOR) g_stat.affinityMood = -AFFINITY_MOOD_FLOOR;
            StatsSave();
            RequestPaint(true);
        }
        return 0;
    }
    // 导演指令：一次性演出 动作/表情/视线/音效/心情。
    //   lParam = new L2dDirectorCmd（由 LLM 线程投递），这里用完 delete。
    //   action/gaze/intensity 为可选覆盖；缺省(-1/0)时完全退回 WM_APP_EMOTION 的旧行为。
    case WM_APP_DIRECTOR: {
        L2dDirectorCmd* cmd = (L2dDirectorCmd*)lp;
        if (!cmd) return 0;
        PetEmotion emo = cmd->emotion;
        int moodDelta = cmd->moodDelta;
#if USE_LIVE2D
        if (Live2DReady()) {
            // 动作：导演指定优先，否则按情绪映射
            int slot = (cmd->action >= 0) ? cmd->action : L2dSlotForEmotion(emo);
            if (L2dSlotUnlocked(slot)) L2dPlayClip(slot);
            // 表情：情绪驱动（intensity 目前只留作后续强度演出的扩展位，不影响口吻）
            L2dSetExpression(emo);
            // 说话时把表情保持到整句呈现完，而不是固定 4.2 秒就回中性脸。
            if (cmd->holdMs > 0 && emo != PetEmotion::Base)
                g_l2dExprUntilMs = GetTickCount64() + (ULONGLONG)cmd->holdMs;
            // 视线：导演指定则覆盖，到期自动恢复跟随鼠标（在 Live2DLookAtCursor 里处理）
            if (cmd->gaze >= 0) L2dSetGazeOverride(cmd->gaze, cmd->gazeHoldMs);
            // 音效：正向情绪用 peace，低落/思考用 think
            if (emo == PetEmotion::Smile) XSoundPlay(g_peace, false);
            else if (emo == PetEmotion::Embarrassed || emo == PetEmotion::Thinking) XSoundPlay(g_think, false);
        }
#endif
        if (moodDelta != 0) {
            std::lock_guard<std::recursive_mutex> lk(g_statMtx);
            g_stat.affinityMood += moodDelta;
            if (g_stat.affinityMood > 0) g_stat.affinityMood = 0;
            if (g_stat.affinityMood < -AFFINITY_MOOD_FLOOR) g_stat.affinityMood = -AFFINITY_MOOD_FLOOR;
            StatsSave();
            RequestPaint(true);
        }
        delete cmd;
        return 0;
    }
    // ★触摸互动结果：弹气泡显示模型对这次触碰的反应，lParam = new std::wstring，用完释放。
    //   表情/动作/视线由随后到达的 WM_APP_DIRECTOR 驱动（均来自同一轮 LLM 回复）。
    case WM_APP_TOUCH_REPLY: {
        std::wstring* text = (std::wstring*)lp;
        if (text) {
            // 触摸回复呈现层统一：
            //   1) 聊天窗打开时 -> 进窗口（接在先前"茉子："占位后）并写历史；
            //   2) 未打开且 mode=气泡/都要 -> 贴角色轻气泡显示截断的 1-2 句；
            //   3) 未打开且 mode=开窗口 -> 已自动开窗口，回复已在窗口里。
            //   表情/动作/视线仍由随后到达的 WM_APP_DIRECTOR 驱动（同一轮回复）。
            std::wstring reply = *text;
            if (reply.empty()) reply = L"（未收到回复）";
            bool chatOpen = (g_hChatWnd && IsWindow(g_hChatWnd));
            if (chatOpen) {
                AppendChatTail(reply);          // 接在"茉子："后面，并补上段尾空行
                // 进聊天窗时没有气泡时长可参考：按字数估一段"读完这句话"的时间，
                // 让表情陪着文字一起收，而不是字数多也 4 秒就回中性脸。
                PetEmotion e = (PetEmotion)(int)wp;
                if (e != PetEmotion::Base) {
                    size_t n = reply.size() > 120 ? 120 : reply.size();
                    int ms = 2600 + (int)n * 30;
                    if (ms < (int)L2D_EXPR_HOLD_MS) ms = (int)L2D_EXPR_HOLD_MS;
                    g_l2dExprUntilMs = GetTickCount64() + (ULONGLONG)ms;
                }
            } else {
                int mode = GetLLMConfigInt(L"TouchReplyMode", 0, 0, 2);
                if (mode == 0 || mode == 2)     // 气泡 / 都要
                    // 模型回复属"当前互动"，高优先级插队首
                    ToastShowHi(L"✿", L"茉子", TruncateReply(reply).c_str());
            }
            // 历史落盘：触摸的"旁白 + 回复"成对写盘放在主线程做（只此一处，
            //   避免和 LLM 线程双写）。g_touchPendingNarration 是触摸专用槽。
            if (!g_touchPendingNarration.empty())
                SaveChatHistory(g_touchPendingNarration, reply);
            g_touchPendingNarration.clear();
            delete text;
        }
        g_lastUserInputMs = GetTickCount64();   // 触摸也是一种互动，重置主动搭话静默计时
        return 0;
    }
    // ★附件后台解析完成：聊天窗关着时消息会落到这里，同样回填 + 刷新附件条
    //   （聊天窗下次打开会带出这些附件）
    case WM_APP_ATTACH_READY:
        AttachDrainResults();
        return 0;
    // 主动搭话结果：弹气泡显示，lParam = new std::wstring，用完释放。
    case WM_APP_PROACTIVE: {
        std::wstring* text = (std::wstring*)lp;
        if (text) {
            if (!text->empty()) {
                // 标题带上当前情绪，让"表情"这一维在气泡上也能被看见
                std::wstring title = std::wstring(L"茉子·") + EmotionLabel((PetEmotion)(int)wp);
                // 主动搭话是茉子主动开口，高优先级插队首；可点击展开聊天窗
                ToastEnqueueMsg(L"💬", title.c_str(), text->c_str(), 0, /*clickable=*/true);
                if (g_toastBit < 0) ToastItemPop();
            }
            delete text;
        }
        g_lastProactiveMs = GetTickCount64();
        g_proactiveRunning = false;   // 兜底还原互斥标志
        return 0;
    }
    case WM_APP_AFFINITY: {
        int delta = (int)wp - AFFINITY_MSG_BIAS;
        bool applied = StatsAddAffinityChat(delta);
        if (g_hHistoryEdit && IsWindow(g_hHistoryEdit)) {
            wchar_t line[96];
            if (delta > 0 && applied)
                swprintf(line, 96, L"（茉子对你的好感 +%d）", delta);
            else if (delta > 0 && AffinityMaxed())
                // 满心之后不再累计，别把它报成"今日已达上限"——那会让人以为明天还能涨
                swprintf(line, 96, L"（茉子的心意已经满了）");
            else if (delta > 0)
                swprintf(line, 96, L"（茉子对你的好感 +%d，但今日已达上限 %d，明天再来陪她吧）", delta, AFFINITY_DAY_CAP);
            else if (delta < 0)
                swprintf(line, 96, L"（茉子对你的好感 %d）", delta);
            else
                swprintf(line, 96, L"（茉子的心情没有变化）");
            // 好感提示单独成段：换行交给统一入口，不再手写 "\r\n\r\n" 前缀
            AppendChatView(std::wstring(line));
        }
        UnlockCheck();
        AffinityMaxCheck();
        AchCheck();
        StatsSave();
        RequestPaint(true);
        if (g_hAchPanel && IsWindow(g_hAchPanel)) InvalidateRect(g_hAchPanel, nullptr, FALSE);
        return 0;
    }
    case WM_APP_RVC_DONE: {
        {
            std::lock_guard<std::recursive_mutex> lk(g_statMtx);
            g_stat.coverCount += 1;      // 只有转换成功才计数
            StatsAddAffinity(3);         // 翻唱 +3 好感（对话才是主要来源）
        }
        AchCheck();
        StatsSave();
        if (g_hAchPanel && IsWindow(g_hAchPanel)) InvalidateRect(g_hAchPanel, nullptr, FALSE);
        return 0;
    }
    case WM_APP_PRELOAD_DONE: {
        // ★ 这条消息不只启动时会来：从 Live2D 切回序列帧时，后台加载线程跑完
        //   也会再发一次。开机专属的副作用（启动音、BGM、首帧提示）只做一次，
        //   否则每次切渲染方式都会把 BGM 从头放一遍。
        bool firstBoot = !g_bootDone;
        g_bootDone = true;
#if USE_LIVE2D
        if (!Live2DReady())
#endif
        SwitchAnim(g_currentCostume, AnimState::Idle);
        // ★ 一套素材都没解出来：多半是没放 Animations。
        //   不提示的话用户只会看到一个空白窗口，完全不知道出了什么事。
        {
            bool anyFrame = false;
            for (int c = 0; c < 4 && !anyFrame; ++c)
                for (int s = 0; s < 4 && !anyFrame; ++s)
                    if (g_frameCached[c][s] && !g_frameCache[c][s].empty()) anyFrame = true;
            if (!anyFrame && !Live2DReady())
                ToastShow(L"📂", L"没有找到序列帧素材",
                          L"请把图片放到 exe 同级的 Animations\\服装\\动作\\ 下，目录结构见 README。");
        }
        // ★ 存档里选的是 Live2D，但这次没扫到模型：先按序列帧跑，提醒一次，
        //   补齐素材后可以直接在右键菜单里切过去，不用重开程序。
        if (g_modeFallbackNotice) {
            g_modeFallbackNotice = false;
            ToastShow(L"🖼", L"Live2D 没起来",
                      L"已用序列帧显示。缺素材请补 p2l_runtime.dll 与 live2d\\；素材齐全则看 DebugView 的 [MakotoPet]。修好后可在右键菜单「渲染方式」里切换。");
        }
        if (!IsWindowVisible(hwnd)) { ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd); }
        if (!g_timerOn) { g_timerOn = true; SetTimer(hwnd, 1, FRAME_INTERVAL_MS, nullptr); }
        if (!g_bStartupPlayed) { g_bStartupPlayed = true; PlayStartupAudio(); }
        g_lastTickMs = GetTickCount64();
        TraySetTip(L"常陆茉子桌宠");
        // 未解锁则安静启动；切渲染方式重跑加载线程时不要再重放 BGM
        if (firstBoot && UnlockReady(UNLOCK_BGM)) PlayBGMByIndex(g_currentBGMIndex);
        UnlockCheck();               // 补弹：老存档已有高好感但从未提示过的情况
        AffinityMaxCheck();
        RequestPaint(true);
        return 0;
    }
    case WM_TRAY:
        if (wp == 1 && lp == WM_RBUTTONUP) ShowContextMenu(hwnd);
        break;
    case WM_LBUTTONDOWN:
        if (!g_preloadDone) return 0;            // 加载期不接受交互
        g_bDragging = false;
        g_ptLButtonDownTick = GetTickCount64();  // ★记录按下时刻（区分点击/按住拖）
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
            RefreshHalfBodyFromPlacement(ny, g_currentH, pt);   // 往下拖越界即半身，往上拖回来即全身
            MarkGeomDirty();                                   // ★ 拖完就记住位置
            RequestPaint();
        }
        return 0;
    case WM_LBUTTONUP: {
        if (!g_preloadDone) return 0;            // 加载期不接受交互
        // 区分拖动与点击：
        //   坑点在于 ReleaseCapture() 会【同步】给本窗口发 WM_CAPTURECHANGED，
        //   而那个分支里会把 g_bDragging 清成 false —— 先释放再判断的话，
        //   哪怕刚才拖得很明显，走到这里也会被当成"点击"去摸头摸手。
        //   所以这里先在本地算好"这一下是拖还是点"，再动 capture。
        POINT pt{}; GetCursorPos(&pt);
        const int dx = abs(pt.x - g_ptLButtonDown.x);
        const int dy = abs(pt.y - g_ptLButtonDown.y);
        int slop = (int)GetSystemMetrics(SM_CXDRAG) - 1;
        if (slop < 3) slop = 3;
        const bool windowMoved = g_winPosKnown &&
            (g_winX != g_dragWindowStart.x || g_winY != g_dragWindowStart.y);
        // 按住超过 0.7 秒仍没松手：多半是"按住准备拖"，松手时别奖励一次触摸
        const bool heldTooLong = (g_ptLButtonDownTick != 0) &&
            (GetTickCount64() - g_ptLButtonDownTick > 700);
        const bool wasDragging = g_bDragging;
        const bool treatedAsDrag = wasDragging || windowMoved || dx > slop || dy > slop || heldTooLong;

        if (GetCapture() == hwnd) ReleaseCapture();
        g_bDragging = false;
        g_ptLButtonDownTick = 0;

        if (treatedAsDrag) {
            if (wasDragging || windowMoved) {
                // 往下拖过半身阈值就吸附到标准沉底位置；没过阈值则保留玩家自己摆的位置。
                int snappedY = g_winY;
                SnapHalfBodyOnDragEnd(pt, snappedY);
                if (snappedY != g_winY) {
                    g_winY = snappedY;
                    RequestPaint(true);
                }
                SwitchAnim(g_currentCostume, AnimState::Idle);
            }
            return 0;                            // 拖拽收尾：绝不触发身体交互
        }
#if USE_LIVE2D
        // 按部位分发触摸互动（头/脸/手/身），点不到人物才走状态轮转
        if (Live2DReady()) {
            int cx = (int)(short)LOWORD(lp), cy = (int)(short)HIWORD(lp);
            L2dBodyPart part = Live2DHitPart(cx, cy);
            if (part != L2D_PART_NONE) {
                DoBodyTouch(hwnd, part);
                // ★触摸物理反馈：抓住裙摆/裤腿拉一下再回弹（本地即时，不等 LLM）。
                L2dClothTug(cx, cy, part);
                return 0;
            }
        }
#endif
        switch (g_currentState) {
        case AnimState::Idle: SwitchAnim(g_currentCostume, AnimState::Think); break;
        case AnimState::Think: SwitchAnim(g_currentCostume, AnimState::Peace); break;
        default: SwitchAnim(g_currentCostume, AnimState::Idle); break;
        }
        return 0;
    }
    // ★附件入口③：图片/文件直接拖到桌宠身上 → 自动开聊天窗并挂上附件
    case WM_DROPFILES: {
        HDROP hd = (HDROP)wp;
        UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; ++i) {
            std::wstring p = DragFilePath(hd, i);
            if (!p.empty()) paths.push_back(p);
        }
        DragFinish(hd);
        OpenChatDialog();
        int added = 0;
        std::wstring firstErr;
        for (const std::wstring& p : paths) {
            std::wstring err;
            if (AddAttachmentFromPath(p, err)) ++added;
            else if (firstErr.empty()) firstErr = err;
        }
        RefreshAttachStrip();
        if (g_hChatWnd && IsWindow(g_hChatWnd)) {
            SetForegroundWindow(g_hChatWnd);
            LayoutChatChildren(g_hChatWnd);
            ChatSetStatus(g_hChatWnd, firstErr.empty()
                ? (L"已添加 " + std::to_wstring(added) + L" 个附件，按 Enter 发送")
                : (L"⚠ " + firstErr));
        }
        return 0;
    }
    case WM_WINDOWPOSCHANGED: {
        WINDOWPOS* pwp = (WINDOWPOS*)lp;
        if (pwp && !(pwp->flags & SWP_NOMOVE)) { g_winX = pwp->x; g_winY = pwp->y; g_winPosKnown = true; }
        // 位置/尺寸被外部改过（拖动、贴边、分辨率变化）也记一笔，交给定时器落盘
        if (pwp && !(pwp->flags & (SWP_NOMOVE | SWP_NOSIZE))) MarkGeomDirty();
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
    // ★ 关机 / 注销：Windows 不保证随后还会走 WM_DESTROY，先落盘再放行。
    case WM_ENDSESSION:
        if (wp) { TtsStopAll(); SaveConfig(); }
        return 0;
    case WM_DESTROY: {
        // ★ 常驻 whisper-server 是子进程，主程序退出必须带走，否则会残留在后台吃内存
        AsrServerStop();
        // ★ 先收拾语音：合成线程还在 HTTP 中途时，
        //   不能让它在 XAudio2 / rig 已销毁后继续跑（会崩在后台线程）。
        TtsStopAll();
        g_ttsQuit.store(true);
        if (g_hTtsEvent) SetEvent(g_hTtsEvent);
        if (g_hTtsWorker) {
            WaitForSingleObject(g_hTtsWorker, 3000);   // 超时也不强留：程序即将退出
            CloseHandle(g_hTtsWorker); g_hTtsWorker = nullptr;
        }
        if (g_hTtsEvent) { CloseHandle(g_hTtsEvent); g_hTtsEvent = nullptr; }
        SaveConfig(); KillTimer(hwnd, 1); StopBGM();
#if USE_LIVE2D
        // 桌宠是长驻进程，不能像示例程序那样靠进程退出回收句柄。
        Live2DShutdown();
#endif
        if (g_exit.loaded) { PlayExitAudio(); WaitForSoundToFinish(g_exit, 2000); }
        TrayRemove();
        if (g_gfx) { delete g_gfx; g_gfx = nullptr; }
        if (g_memDC) { SelectObject(g_memDC, g_hOldBmp); DeleteObject(g_hMemBmp); DeleteDC(g_memDC); }
        g_memDC = nullptr; g_hMemBmp = nullptr; g_hOldBmp = nullptr; g_dibBits = nullptr;
        // ★ 帧由缓存独占：这里只断开引用，绝不能 ClearFrames（否则二次释放）
        g_currentFrames.clear(); g_pendingFrames.clear();
        RenderCacheInvalidate();   // ★ 必须在 GdiplusShutdown 之前释放 GDI+ 对象
        AnimLoadStop();            // 先停解码线程，再动缓存
        ClearFrameCache();
        {   // 缩放重建时推迟回收的旧帧，退出前必须释放，且在 GdiplusShutdown 之前
            std::lock_guard<std::recursive_mutex> lk(g_frameMtx);
            ClearFrames(g_pendingDelete);
        }
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

// ==================================================================
//  ★设置面板（数据驱动）：把 llm.ini 里所有"玩家有权手动设置"的参数
//   集中到一个窗口，按类别分页呈现；左侧类别 ListBox + 右侧动态字段控件，
//   支持"保存设置 / 恢复本页默认"，保存即写回 llm.ini 并即时生效。
//   覆盖 [LLM] / [UI] / [TTS] 三段；[State] 的音量/渲染/缩放已有右键菜单，
//   [Chat]/[Stats]/[State] 的位置与统计属于程序自管理，不暴露。
// ==================================================================
enum SetKind { SK_INT, SK_DOUBLE, SK_TEXT, SK_ENUM };
struct SetChoice { int value; const wchar_t* label; };
struct SettingField {
    const wchar_t* section;   // llm.ini 段名
    const wchar_t* key;
    const wchar_t* label;     // 中文说明
    SetKind kind;
    const wchar_t* def;       // 默认值（字符串）
    double lo, hi;            // 数值范围（INT/DOUBLE 校验用）
    const SetChoice* choices; // ENUM 选项，value==-1 结束
};

static const SetChoice CH_ONOFF[]      = { {0,L"关闭"},{1,L"开启"},{-1,nullptr} };
static const SetChoice CH_TOUCHREPLY[] = { {0,L"气泡显示"},{1,L"打开聊天窗"},{2,L"气泡+窗口"},{-1,nullptr} };
static const SetChoice CH_LONGFORM[]   = { {2,L"自动判断"},{0,L"强制短文"},{1,L"强制长文"},{-1,nullptr} };
static const SetChoice CH_THEME[]      = { {0,L"浅色"},{1,L"深色"},{-1,nullptr} };
static const SetChoice CH_SKIPMODE[]   = { {0,L"禁用跳过"},{1,L"点击跳过"},{2,L"点击跳过+加速"},{-1,nullptr} };

// 全部字段（顺序即索引，类别表用 first/count 引用）
static const SettingField SET_FIELDS[] = {
    // —— 连接与模型 ——
    {L"LLM",L"ApiUrl", L"接口地址 ApiUrl",        SK_TEXT,   L"",      0,0,nullptr},
    {L"LLM",L"ApiKey", L"API 密钥 ApiKey",         SK_TEXT,   L"",      0,0,nullptr},
    {L"LLM",L"Model",  L"模型名称 Model",          SK_TEXT,   L"",      0,0,nullptr},
    // —— 采样与生成 ——
    {L"LLM",L"MaxTokens",           L"回复上限 MaxTokens",      SK_INT,    L"3000", 256,16384,nullptr},
    {L"LLM",L"Temperature",         L"随机性 Temperature",      SK_DOUBLE, L"0.9",  0,2,nullptr},
    {L"LLM",L"TopP",                L"核采样 TopP",             SK_DOUBLE, L"0.95", 0,1,nullptr},
    {L"LLM",L"PresencePenalty",     L"话题惩罚 PresencePenalty",SK_DOUBLE, L"0.6", -2,2,nullptr},
    {L"LLM",L"FrequencyPenalty",    L"用词惩罚 FrequencyPenalty",SK_DOUBLE,L"0.3", -2,2,nullptr},
    {L"LLM",L"EnableThinking",      L"模型思考链",              SK_ENUM,   L"0",    0,0,CH_ONOFF},
    {L"LLM",L"UsePenalty",          L"启用惩罚参数",            SK_ENUM,   L"1",    0,0,CH_ONOFF},
    {L"LLM",L"UseResponseFormat",   L"JSON 格式约束",           SK_ENUM,   L"1",    0,0,CH_ONOFF},
    {L"LLM",L"UseChatTemplateKwargs",L"模板参数覆盖",           SK_ENUM,   L"0",    0,0,CH_ONOFF},
    // —— 长文·历史·记忆 ——
    {L"LLM",L"LongMode",        L"长文模式总开关",       SK_ENUM, L"1",   0,0,CH_ONOFF},
    {L"LLM",L"LongMaxTokens",   L"长文上限 LongMaxTokens",SK_INT, L"8000",256,32768,nullptr},
    {L"LLM",L"LongFormManual",  L"长文判定模式",          SK_ENUM, L"2",   0,0,CH_LONGFORM},
    {L"LLM",L"HistoryMaxChars", L"对话历史字数上限",      SK_INT,  L"4000",500,60000,nullptr},
    {L"LLM",L"HistoryArchiveKB",L"历史归档阈值(KB)",      SK_INT,  L"512", 0,8192,nullptr},
    {L"LLM",L"MemoryExpireDays",L"记忆淘汰天数",          SK_INT,  L"180", 0,3650,nullptr},
    {L"LLM",L"MemoryEnable",    L"长期记忆开关",          SK_ENUM, L"1",   0,0,CH_ONOFF},
    // —— 对话机制 ——
    {L"LLM",L"DirectorEnable",      L"导演指令(情绪/动作)",  SK_ENUM, L"1",   0,0,CH_ONOFF},
    {L"LLM",L"EmotionTone",         L"情绪语气联动",        SK_ENUM, L"1",   0,0,CH_ONOFF},
    {L"LLM",L"EmotionAffinity",     L"情绪影响好感",        SK_ENUM, L"1",   0,0,CH_ONOFF},
    {L"LLM",L"ProactiveEnable",     L"主动搭话开关",        SK_ENUM, L"1",   0,0,CH_ONOFF},
    {L"LLM",L"ProactiveSaveHistory",L"主动搭话写入历史",    SK_ENUM, L"1",   0,0,CH_ONOFF},
    {L"LLM",L"ProactiveSummaryChars",L"主动搭话摘要字数",   SK_INT,  L"400", 0,2000,nullptr},
    {L"LLM",L"ProactiveIntervalSec",L"主动搭话间隔(秒)",    SK_INT,  L"180", 15,86400,nullptr},
    {L"LLM",L"ProactiveIdleSec",    L"空闲判定(秒)",        SK_INT,  L"60",  5,86400,nullptr},
    {L"LLM",L"ProactiveMaxTokens",  L"主动搭话字数上限",    SK_INT,  L"300", 128,2000,nullptr},
    // —— 触摸互动 ——
    {L"LLM",L"TouchLlmEnable",   L"触摸由 AI 回复",        SK_ENUM, L"1",    0,0,CH_ONOFF},
    {L"LLM",L"TouchReplyMode",   L"触摸回复呈现方式",      SK_ENUM, L"0",    0,0,CH_TOUCHREPLY},
    {L"LLM",L"TouchAutoOpenChat",L"气泡时自动开聊天窗(旧)",SK_ENUM, L"0",    0,0,CH_ONOFF},
    {L"LLM",L"TouchCooldownMs",  L"触摸冷却(毫秒)",        SK_INT,  L"1200", 0,10000,nullptr},
    {L"LLM",L"TouchTimeoutMs",   L"触摸请求超时(毫秒)",    SK_INT,  L"20000",5000,120000,nullptr},
    {L"LLM",L"TouchMaxTokens",   L"触摸回复字数上限",      SK_INT,  L"1200", 256,4000,nullptr},
    {L"LLM",L"TouchSkirtEnable", L"摸裙摆物理反馈",        SK_ENUM, L"1",    0,0,CH_ONOFF},
    {L"LLM",L"TouchTugScale",    L"裙摆拉扯幅度",          SK_DOUBLE,L"1.0", 0,3,nullptr},
    // —— 打字机·界面 ——
    {L"LLM",L"TypewriterTargetMs",   L"打字机总时长(毫秒)",  SK_INT,  L"6000",500,120000,nullptr},
    {L"LLM",L"TypewriterMinDelayMs", L"每字最小延时(毫秒)",  SK_INT,  L"2",    0,500,nullptr},
    {L"LLM",L"TypewriterMaxDelayMs", L"每字最大延时(毫秒)",  SK_INT,  L"30",   1,500,nullptr},
    {L"UI", L"TypewriterSkipMode",   L"跳过/加速模式",       SK_ENUM, L"1",    0,0,CH_SKIPMODE},
    {L"UI", L"ChatTheme",            L"聊天窗主题",          SK_ENUM, L"0",    0,0,CH_THEME},
    {L"UI", L"ChatFontPt",           L"聊天字号(点)",        SK_INT,  L"16",   8,40,nullptr},
    {L"UI", L"ChatFontFace",         L"聊天字体",            SK_TEXT, L"Segoe UI Emoji",0,0,nullptr},
    // 半身模式不再做设置项：直接往下拖出屏幕即可，状态随窗口坐标保存。
    // —— 语音·基础 ——
    {L"TTS",L"TtsEnable",      L"语音朗读总开关",     SK_ENUM, L"1",      0,0,CH_ONOFF},
    {L"TTS",L"TtsHost",        L"服务地址 TtsHost",   SK_TEXT, L"127.0.0.1",0,0,nullptr},
    {L"TTS",L"TtsPort",        L"服务端口 TtsPort",   SK_INT,  L"9880",   1,65535,nullptr},
    {L"TTS",L"TtsTimeoutMs",   L"合成超时(毫秒)",     SK_INT,  L"45000",  1000,600000,nullptr},
    {L"TTS",L"TtsMaxChars",    L"最多朗读字数",       SK_INT,  L"600",    0,100000,nullptr},
    {L"TTS",L"TtsSegMaxChars", L"分段字数上限",       SK_INT,  L"60",     12,300,nullptr},
    {L"TTS",L"TtsSplit",       L"长文本分段",         SK_ENUM, L"1",      0,0,CH_ONOFF},
    {L"TTS",L"TtsShowError",   L"合成失败时提示",     SK_ENUM, L"1",      0,0,CH_ONOFF},
    // —— 附件·语音输入 ——
    //   只暴露玩家真正需要调的 4+1 项；whisper 路径/模型、PDF 工具路径留 ini，
    //   属于"改错就废"的部署参数，不往 UI 上放。
    {L"Attach",L"ImageMaxSide", L"图片最长边(像素)",   SK_INT,  L"1280", 512,  2048, nullptr},
    {L"Attach",L"TextMaxChars", L"文本附件字数上限",   SK_INT,  L"12000",1000, 40000,nullptr},
    {L"Attach",L"MaxFiles",     L"单次最多附件数",     SK_INT,  L"4",    1,    8,    nullptr},
    {L"Attach",L"CacheDays",    L"图片缓存保留(天)",   SK_INT,  L"30",   0,    365,  nullptr},
    {L"ASR",   L"Enable",       L"语音输入开关",       SK_ENUM, L"0",    0,0,  CH_ONOFF},
    {L"ASR",   L"Threads",      L"识别线程数",         SK_INT,  L"4",    1,    32,   nullptr},
    {L"ASR",   L"MaxSeconds",   L"最长录音(秒)",       SK_INT,  L"30",   5,    120,  nullptr},
    {L"ASR",   L"ServerMode",   L"常驻加速(首字更快)", SK_ENUM, L"1",    0,0,  CH_ONOFF},
    {L"Attach",L"OcrEnable",    L"扫描件 OCR",         SK_ENUM, L"1",    0,0,  CH_ONOFF},
    {L"Attach",L"OcrMaxPages",  L"OCR 最多页数",       SK_INT,  L"3",    1,    20,   nullptr},
    // ★语音高级参数（TtsTextLang / TtsVersion / TtsTopK / TtsRefAudio / TtsExtraJson 等）
    //   不再呈现给玩家：属 GPT-SoVITS 调参项，玩家改错会直接导致语音不可用。
    //   程序仍从 llm.ini 读取它们，需要时手改 ini 即可。
};
static const int SET_FIELD_COUNT = (int)(sizeof(SET_FIELDS)/sizeof(SET_FIELDS[0]));

struct SetCategory { const wchar_t* name; int first; int count; };
static const SetCategory SET_CATS[] = {
    {L"连接与模型",     0, 3},
    {L"采样与生成",     3, 9},
    {L"长文·历史·记忆",12, 7},
    {L"对话机制",      19, 9},
    {L"触摸互动",      28, 8},
    {L"打字机·界面",   36, 7},
    {L"语音·基础",     43, 8},
    {L"附件·输入",     51, 10},
};
static const int SET_CAT_COUNT = (int)(sizeof(SET_CATS)/sizeof(SET_CATS[0]));

static const int SET_ID_LIST      = 22000;
static const int SET_ID_LBL_BASE  = 23000;
static const int SET_ID_CTL_BASE  = 24000;
static const int SET_ID_SAVE      = 25001;
static const int SET_ID_RESET     = 25002;
static const int SET_ID_CLOSE     = 25003;
static const int SET_MAX_ROWS     = 16;   // 单页最多字段数（当前最多 9）
static HWND g_hSettingsWnd = nullptr;
static HWND g_hSetList = nullptr;
static HWND g_hSetLabel[SET_MAX_ROWS] = {};
static HWND g_hSetCtl[SET_MAX_ROWS] = {};
static int  g_setCurCat = 0;
static int  g_setCurCount = 0;

// 写任意段配置（GetIniStr 的对应写函数）。走自写引擎：
// WritePrivateProfileStringW 会按系统 ANSI 码页重写整份 UTF-8 文件，把中文值改坏。
static void SetIniStr(const wchar_t* sec, const wchar_t* key, const wchar_t* val) {
    std::wstring dir;
    if (!GetExeDir(dir)) return;
    IniSetValue(dir + L"\\llm.ini", sec, key, val ? val : L"");
}

// 渲染某个类别的字段控件（先销毁上一页再新建）
static void SetLoadCategory(int cat) {
    if (cat < 0 || cat >= SET_CAT_COUNT) return;
    // 先销毁上一页：数量钳到数组上限，避免历史脏值越界访问 g_hSetLabel/g_hSetCtl
    int prevCount = (g_setCurCount < SET_MAX_ROWS) ? g_setCurCount : SET_MAX_ROWS;
    for (int i = 0; i < prevCount; ++i) {
        if (g_hSetLabel[i]) { DestroyWindow(g_hSetLabel[i]); g_hSetLabel[i] = nullptr; }
        if (g_hSetCtl[i])   { DestroyWindow(g_hSetCtl[i]);   g_hSetCtl[i]   = nullptr; }
    }
    g_setCurCat = cat;
    // ★安全：SET_CATS 的 first/count 与 SET_FIELDS 实际条数是手工维护的，
    //   任何一次加字段漏改都会让 SET_FIELDS[first+i] 越界。这里统一做钳制。
    const int first = SET_CATS[cat].first;
    if (first < 0 || first >= SET_FIELD_COUNT) { g_setCurCount = 0; return; }
    int cnt = SET_CATS[cat].count;
    if (cnt < 0) cnt = 0;
    if (cnt > SET_FIELD_COUNT - first) cnt = SET_FIELD_COUNT - first;
    if (cnt > SET_MAX_ROWS) cnt = SET_MAX_ROWS;
    g_setCurCount = cnt;
    for (int i = 0; i < g_setCurCount; ++i) {
        const SettingField& f = SET_FIELDS[first + i];
        int y = 10 + i * 32;
        g_hSetLabel[i] = CreateWindowExW(0, L"STATIC", f.label,
            WS_CHILD | WS_VISIBLE | SS_LEFT, 182, y + 3, 310, 22,
            g_hSettingsWnd, (HMENU)(INT_PTR)(SET_ID_LBL_BASE + i),
            GetModuleHandleW(nullptr), nullptr);
        std::wstring cur = GetIniStr(f.section, f.key, f.def);
        if (f.kind == SK_ENUM) {
            g_hSetCtl[i] = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
                500, y, 210, 200, g_hSettingsWnd, (HMENU)(INT_PTR)(SET_ID_CTL_BASE + i),
                GetModuleHandleW(nullptr), nullptr);
            int curVal = _wtoi(cur.c_str()), sel = 0;
            for (int k = 0; f.choices && f.choices[k].value != -1; ++k) {
                int idx = (int)SendMessageW(g_hSetCtl[i], CB_ADDSTRING, 0, (LPARAM)f.choices[k].label);
                SendMessageW(g_hSetCtl[i], CB_SETITEMDATA, idx, f.choices[k].value);
                if (f.choices[k].value == curVal) sel = idx;
            }
            SendMessageW(g_hSetCtl[i], CB_SETCURSEL, sel, 0);
        } else {
            g_hSetCtl[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cur.c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                500, y, 210, 24, g_hSettingsWnd, (HMENU)(INT_PTR)(SET_ID_CTL_BASE + i),
                GetModuleHandleW(nullptr), nullptr);
        }
    }
}

// 保存设置后让关键项即时生效（定义在后，先声明供 SetSaveCategory 调用）
static void SetApplyLive();

// 保存当前页字段并即时生效
static void SetSaveCategory() {
    if (g_setCurCat < 0 || g_setCurCat >= SET_CAT_COUNT) return;
    const int first = SET_CATS[g_setCurCat].first;
    if (first < 0 || first >= SET_FIELD_COUNT) return;
    for (int i = 0; i < g_setCurCount; ++i) {
        const SettingField& f = SET_FIELDS[first + i];
        std::wstring v;
        if (f.kind == SK_ENUM) {
            int sel = (int)SendMessageW(g_hSetCtl[i], CB_GETCURSEL, 0, 0);
            int val = (sel >= 0) ? (int)SendMessageW(g_hSetCtl[i], CB_GETITEMDATA, sel, 0) : _wtoi(f.def);
            wchar_t b[16]; wsprintfW(b, L"%d", val); v = b;
        } else {
            wchar_t b[1024]; b[0] = 0;
            GetWindowTextW(g_hSetCtl[i], b, 1024);
            if (f.kind == SK_INT) {
                int n = _wtoi(b);
                if (n < (int)f.lo) n = (int)f.lo;
                if (n > (int)f.hi) n = (int)f.hi;
                wsprintfW(b, L"%d", n);
            } else if (f.kind == SK_DOUBLE) {
                double d = _wtof(b);
                if (d < f.lo) d = f.lo;
                if (d > f.hi) d = f.hi;
                swprintf(b, 1024, L"%.6g", d);
            }
            v = b;
        }
        SetIniStr(f.section, f.key, v.c_str());
        // 兼容旧项：GetTtsMaxChars 优先读 [LLM] TTSMaxChars，清掉它才能让 [TTS] 新值生效
        if (wcscmp(f.key, L"TtsMaxChars") == 0) SetIniStr(L"LLM", L"TTSMaxChars", L"");
    }
    SetApplyLive();   // 缓存失效 + 主题/语音/上色即时生效
    ToastShow(L"⚙", L"设置已保存", L"已写入 llm.ini，相关选项即时生效。");
}

// 保存设置后让关键项即时生效（保存/恢复默认共用）
static void SetApplyLive() {
    InvalidatePromptCache();
    InvalidateMemoryCache();
    g_chatTheme = GetUIConfigInt(L"ChatTheme", 0, 0, 1);
    g_ttsVoiceOn.store(_wtoi(GetIniStr(L"TTS", L"TtsEnable", L"1").c_str()) != 0);
    if (g_hChatWnd && IsWindow(g_hChatWnd)) {
        // ★主题改了要真正应用（EM_SETBKGNDCOLOR + 默认文字色），并同步顶栏按钮文字
        SetButtonText(g_hChatWnd, ID_BTN_THEME, g_chatTheme ? L"☀️ 浅色" : L"🌙 深色");
        ApplyChatTheme(g_hChatWnd);
        UpdateWindow(g_hChatWnd);
    }
}

// 恢复当前页字段到默认值
static void SetResetCategory() {
    if (g_setCurCat < 0 || g_setCurCat >= SET_CAT_COUNT) return;
    const int first = SET_CATS[g_setCurCat].first;
    if (first < 0 || first >= SET_FIELD_COUNT) return;
    for (int i = 0; i < g_setCurCount; ++i) {
        const SettingField& f = SET_FIELDS[first + i];
        SetIniStr(f.section, f.key, f.def);
        if (wcscmp(f.key, L"TtsMaxChars") == 0) SetIniStr(L"LLM", L"TTSMaxChars", L"");
    }
    SetApplyLive();                 // 主题/语音开关等即时生效
    SetLoadCategory(g_setCurCat);   // 重新载入以刷新控件显示
    ToastShow(L"↩", L"已恢复默认", L"本页参数已恢复为默认值并写入 llm.ini。");
}

static LRESULT CALLBACK SettingsWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hSettingsWnd = h;
        g_hSetList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
            10, 10, 160, 400, h, (HMENU)(INT_PTR)SET_ID_LIST, GetModuleHandleW(nullptr), nullptr);
        for (int i = 0; i < SET_CAT_COUNT; ++i)
            SendMessageW(g_hSetList, LB_ADDSTRING, 0, (LPARAM)SET_CATS[i].name);
        SendMessageW(g_hSetList, LB_SETCURSEL, 0, 0);
        CreateWindowExW(0, L"BUTTON", L"保存设置", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 420, 150, 32, h, (HMENU)(INT_PTR)SET_ID_SAVE, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"恢复本页默认", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            172, 420, 150, 32, h, (HMENU)(INT_PTR)SET_ID_RESET, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            560, 420, 150, 32, h, (HMENU)(INT_PTR)SET_ID_CLOSE, GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"STATIC", L"提示：改完点「保存设置」写回 llm.ini；数值自动钳制到合法范围。",
            WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 462, 700, 20, h, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetLoadCategory(0);
        break;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == SET_ID_LIST && code == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_hSetList, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < SET_CAT_COUNT) SetLoadCategory(sel);
        } else if (id == SET_ID_SAVE) SetSaveCategory();
        else if (id == SET_ID_RESET) SetResetCategory();
        else if (id == SET_ID_CLOSE) DestroyWindow(h);
        break;
    }
    case WM_CLOSE: DestroyWindow(h); break;
    case WM_DESTROY:
        g_hSettingsWnd = nullptr; g_hSetList = nullptr;
        break;
    default: return DefWindowProcW(h, m, w, l);
    }
    return 0;
}

static void OpenSettingsDialog() {
    if (g_hSettingsWnd && IsWindow(g_hSettingsWnd)) { SetForegroundWindow(g_hSettingsWnd); return; }
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MakotoSettingsClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"MakotoSettingsClass", L"⚙ 茉子设置",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 740, 520,
        g_hwnd ? g_hwnd : nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// ================= 入口 =================
// ★ 显式 extern "C"：CRT 的 invoke_main 是按【未修饰】的名字找 WinMain 的。
//   如果编译器把它修饰成 ?WinMain@@YAH... 就会报
//   "无法解析的外部符号 WinMain，函数 invoke_main 中引用了该符号"。
//   顺带去掉了 __in / __in_opt 这些 SAL 注解（对入口函数没有意义，
//   而且在缺少 sal.h 的环境里会被当成标识符，反而引入噪音）。
extern "C" int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) { MessageBoxW(nullptr, L"COM 初始化失败！", L"错误", MB_OK); return 0; }
    std::wstring exePath;
    if (!GetExeDir(exePath)) { MessageBoxW(nullptr, L"无法定位程序目录！", L"错误", MB_OK); return 0; }
    SetCurrentDirectoryW(exePath.c_str());
    // 把相对路径 "reference\mako_ref.wav" 拼成绝对路径
    g_ttsRefAudioPath = exePath + L"\\reference\\mako_ref.wav";
    srand(GetTickCount());
    ScanBGMDirectory();
    // 顺序很重要：LoadConfig 要用"累计好感"校验存档中的服装是否已解锁，
    // 因此必须放在 StatsLoad 之后，否则读档时好感恒为 0，服装解锁判断出错。
    StatsLoad();
    LoadConfig();
    StatsCheckDay(); AchCheck();      // ★ 成就系统初始化
    // ★ 启动即扫描两套素材（工作目录已是 exe 同级），据此决定这次跑哪条渲染路径。
    //   规则：优先按存档里的选择；选了但素材不在就回落到另一套；只有一套能跑就用那套。
    DetectAssets();
    bool wantL2D = (g_wantMode == RM_LIVE2D && g_hasLive2DAssets);
    if (!wantL2D && !g_hasFrameAssets && g_hasLive2DAssets) wantL2D = true;   // 只有模型
    if (g_wantMode == RM_LIVE2D && !g_hasLive2DAssets && g_hasFrameAssets)
        g_modeFallbackNotice = true;      // 选了模型却没素材：先按序列帧跑，加载完提示一次
    g_renderMode = RM_FRAMES;             // Live2DInit 成功后才改写，避免菜单显示与画面不一致
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
    DragAcceptFiles(g_hwnd, TRUE);      // ★附件：桌宠本体接受文件拖拽
    // ★ 人物位置先显示托盘图标 + 进度条，等 16 套素材全部就绪后再换成人物
    { RECT rc0; if (GetWinRect(g_hwnd, rc0)) { g_winX = rc0.left; g_winY = rc0.top; g_winPosKnown = true; } }
    g_lastTickMs = GetTickCount64();
    TrayAdd();
    g_timerOn = true;
    SetTimer(g_hwnd, 1, FRAME_INTERVAL_MS, nullptr);
    ShowWindow(g_hwnd, SW_SHOWNA);          // 不抢焦点，仅显示加载画面
    UpdateLayered();
    // 半身状态由窗口位置决定：序列帧尺寸已定，这里先按实际坐标推导；
    // Live2D 尺寸要等模型加载，下面再刷新一次。
    RefreshHalfBodyFromWindow();
#if USE_LIVE2D
    if (wantL2D && Live2DInit()) {
        // 模型已在主线程就绪：跳过 16 套序列帧的全量解码，直接进入人物阶段。
        // 初始化失败时落到 AnimLoadStart，按序列帧跑（缺素材时由下面的提示兜底）。
        g_renderMode = RM_LIVE2D;
        g_preloadDone = true;
        // ★ 模型加载完才知道真实窗口尺寸：把存档里的位置/尺寸精确贴一次，
        //   否则 Live2DApplyWindowSize 的底边锚定重算会把位置拽偏。
        RestoreSavedGeometry();
        RefreshHalfBodyFromWindow();   // Live2D 尺寸定下来后再按位置推导
        UpdateLayered();
        PostMessageW(g_hwnd, WM_APP_PRELOAD_DONE, 0, 0);
    } else {
        g_renderMode = RM_FRAMES;
        if (wantL2D) {
            // 素材在、但模型没起来（DLL 版本过旧 / 文件损坏）：也要提示，别静默变序列帧
            g_modeFallbackNotice = true;
        }
        AnimLoadStart();
    }
#else
    (void)wantL2D;
    AnimLoadStart();
#endif
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    CoUninitialize();
    return 0;
}
