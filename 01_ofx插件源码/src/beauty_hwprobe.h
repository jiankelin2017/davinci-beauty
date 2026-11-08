#pragma once
// beauty_hwprobe.h —— 硬件能力探测与渲染路径自动定档（P1）
//
// == 为什么需要这个模块 ====================================================
// P1 的 D3D11 搬运基准实测表明，4K 帧的瓶颈**不是 GPU 算力，而是 PCIe 上传**：
// 本机（RTX 5060 Laptop + Intel 核显，笔记本 x8 通道）实测上传仅 9.3 GB/s，
// 133MB 的 RGBA32F 一帧要 14.3ms；而同样一帧的 ROI（人脸包围盒 1280x1440）
// 上传只要 2.34ms。两者相差 6 倍。
//
// 因此「检测到独显就一定走 GPU」是错误策略。正确做法是按**本机真实带宽 +
// 当帧分辨率**共同判定，因为不同机器差异极大：
//   - 台式机 PCIe 4.0 x16 实测可达 20+ GB/s
//   - 笔记本双显卡常见 x8 通道，实测 9 GB/s 左右
//   - 纯核显显存小（本机 Intel 仅 0.12GB 专用），4K 会直接分配失败
//   - Resolve 自身可能正占用独显做 CUDA 运算，实际可用带宽更低
//
// 本模块做三件事：
//   1) 静态探测：CPU 物理/逻辑核、内存、GPU 型号/显存/feature level/各项能力标志
//   2) 微型校准：实测本机上传/下载带宽（~60ms，仅进程内首次）
//   3) 定档选路：给出是否走 GPU、CPU 路径线程数、以及走 GPU 的最小像素阈值
//
// == 覆盖方式（3.3.0 起改为三级：环境变量 > 配置文件 > 自动）================
//
//   配置文件 C:\ProgramData\DaVinciBeauty\beauty.ini（首次运行自动生成模板）
//     path        = auto | gpu | cpu     走哪条渲染路
//     gpu_pipe    = auto | on | off      GPU 管道：auto=首帧自检一次后关闭
//     gpu_minpix  = 数字                 走 GPU 的最小像素数
//
//   环境变量同名覆盖（BEAUTY_PATH / BEAUTY_GPU_PIPE / BEAUTY_GPU_MINPIX）
//   仍保留，优先级最高，方便临时排查而不必改文件。
//
// == 为什么从「只用环境变量」改成配置文件 ==================================
// 2026-11-06 实测教训：用 launch.bat 设环境变量再 start Resolve，用户双击
// 桌面图标启动时变量根本继承不到，于是「管道一行日志都没有」被误判成管道
// 坏了，白跑一个来回。开关必须落在磁盘上，不能挂在「从哪个图标启动」上。
//
// == 线程安全 ==============================================================
// beautyHwProbe() 内部用 std::once_flag 保证只探测一次；返回的是进程级常量引用，
// 多实例并发读安全。beautyHwCalibrate() 可重复调用，但只在首次真正测量。
// ===========================================================================

enum BeautyPath {
    BEAUTY_PATH_AUTO  = 0,
    BEAUTY_PATH_GPU   = 1,
    BEAUTY_PATH_CPU   = 2,
};

enum BeautyGpuTier {
    BEAUTY_GPU_NONE = 0,   // 无可用 GPU（无 D3D11 / 无 CS / 只有 Basic Render Driver）
    BEAUTY_GPU_LOW  = 1,   // 核显，或可用显存 < 2GB
    BEAUTY_GPU_MID  = 2,   // 独显，可用显存 2~6GB
    BEAUTY_GPU_HIGH = 3,   // 独显，可用显存 >= 6GB
};

struct BeautyHwInfo {
    // ---- CPU ----
    int  cpuLogical  = 0;
    int  cpuPhysical = 0;
    int  cpuMaxMhz   = 0;
    unsigned long long memTotalMB = 0;

    // ---- GPU（选中的那一张）----
    int  gpuAdapterCount = 0;      // 枚举到的适配器总数（含核显/软件）
    int  gpuTier         = BEAUTY_GPU_NONE;
    char gpuName[128]    = {0};
    unsigned int  gpuVendorId  = 0;
    unsigned long long gpuVramMB = 0;
    int  gpuFeatureLevel = 0;      // 0xB100 = FL11_1
    bool gpuIsDiscrete   = false;
    bool gpuCs50Ok       = false;  // CS 5.0 可创建
    bool gpuTypedUav16F  = false;  // 16F 能否做 typed UAV **读**（同款卡的已知坑）
    bool gpuTypedUav32F  = false;
    bool gpuStagingOk    = false;  // STAGING + CPU_ACCESS_READ 可用

    // ---- 运行时校准（微基准实测）----
    bool   calibrated       = false;
    double uploadGBs        = 0.0;  // host→VRAM，**已应用下限兜底**，选路用这个
    double uploadGBsRaw     = 0.0;  // 未兜底的原始实测值，日志用（排查抖动）
    bool   uploadFloored    = false;// true = 实测低于 kBwFloor 被抬到下限
    double downloadGBs      = 0.0;  // 实测 VRAM→host
    double gpuCopyMs4k      = 0.0;  // 4K 单趟 load/store 的 GPU 时间，用于估算算力

    // ---- 结论 ----
    int  recommendedPath    = BEAUTY_PATH_CPU;
    int  recommendedThreads = 1;
    unsigned long long gpuMinPixels = 0;  // 低于此像素数走 CPU 更快
    char verdict[320]       = {0};        // 人类可读结论，写日志用
};

// 探测一次（进程内缓存）。forceRefresh 仅用于离线工具重复探测。
const BeautyHwInfo& beautyHwProbe(bool forceRefresh = false);

// 跑微型带宽校准（首次约 60ms，之后命中缓存直接返回 true）。
// 失败（无 GPU）返回 false，此时 calibrated 保持 false。
bool beautyHwCalibrate();

// 把探测结果格式化成多行文本（写日志用）。返回实际写入长度。
int beautyHwDescribe(char* out, int cap);

// 决定当前帧走哪条路。userSetting 取 BEAUTY_PATH_*。
// 注意：本函数只做「能力与尺寸」判定，不含运行时自适应；后者在 P2 由
// 实测帧时间反馈来修正（见 PROGRESS 的 P2 条目）。
BeautyPath beautyChoosePath(int w, int h, int userSetting);

// CPU 路径应使用的线程数（已按物理核、内存、GPU 占用折中）。
int beautyHwCpuThreads();

// 供离线工具与本机基准复用：格式化一行摘要（单行，便于 grep）。
void beautyHwOneLine(char* out, int cap);

// ---------------------------------------------------------------------------
// 配置文件读取（3.3.0）
//
// 文件：C:\ProgramData\DaVinciBeauty\beauty.ini，形如
//     path = auto
//     gpu_pipe = auto
//     gpu_minpix = 0
//
// 规则：
//   · 首次访问时整体读入并缓存；进程内只读一次（改文件需重启宿主生效）
//   · 文件不存在 → 自动写出带注释的模板，返回值全部取 def
//   · 语法：`key = value`，`#` 或 `;` 起注释，键名不分大小写，两侧空白忽略
//   · 读取永远不失败：任何 I/O 或解析问题都退化为 def
// ---------------------------------------------------------------------------
const char* beautyCfgStr(const char* key, const char* def);
long long   beautyCfgInt(const char* key, long long def);
// 配置文件的完整路径（日志用）。
const char* beautyCfgPath();
