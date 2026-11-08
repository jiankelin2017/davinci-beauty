# DaVinciBeauty 架构与关键事实

> 适用代码版本：1.3.5.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：Jian ｜ 替代文档：原根目录 `开发文档.md`

**这份文档管**：分层结构、改代码会踩的硬约束、日志怎么读、配置项在哪、参数为什么这么定。
**不在这里管**：安装看 `docs/guides/guide-install.md`，出包看 `guide-package.md`，验收看 `guide-acceptance.md`，历史过程看 `docs/history/history-progress-1x.md`。

---

## 0. 摘要

达芬奇 Resolve 的 OFX 美颜插件，效果 ID `com.nama.davinci.beauty`（**不要改**：历史工程按 ID 找效果，改了旧工程里已挂的效果会失效）。引擎为自研纯 CPU 实现，已完全脱离早期闭源商业 SDK；人脸检测走 ONNX，有 DX12 适配器时用 DirectML 加速，没有就退回 CPU，**两条路的画面等价**。

只需要记住五条，其余按需查：

| # | 结论 | 详见 |
|---|---|---|
| 1 | 送进人脸模型的像素必须是**原始 0–255**，任何归一化都会让检出归零 | §2.1 |
| 2 | OFX 帧的**第 0 行是画面最下面一行**；检测要反读行、结果映射回缓冲行号 | §2.2 |
| 3 | Resolve 按需渲染，异步检测等不到掩码；**首帧必须同步兜底** | §2.3 |
| 4 | `faces=0` 有两种后果：从没拿到掩码→**逐字节透传**；拿到过又丢→**全帧磨皮** | §1.3、§6 |
| 5 | 改面板参数要**两条链路各验一次**；`baseline.py` 绕过 `readParams()`，它全绿不代表面板正确 | §7.3 |

---

## 1. 分层与一帧的路径

```
OFX 宿主层   ofx_plugin.cpp            取帧 / 色彩转换(blitFrame) / 写回 / 日志
   ↓
引擎适配层   nama_engine.cpp           NamaSettings → 引擎参数，转调
   ↓
桥接层       core_glue.h / beauty_engine_glue.cpp    C ABI（固定 14 项参数）
   ↓
自研引擎     beauty_core.cpp
           + beauty_smooth / beauty_region / beauty_guide
             beauty_warp / beauty_freq / beauty_pool    磨皮 / 美白 / 美型 / 掩码
人脸检测     beauty_face.cpp + beauty_onnx.cpp          ONNX 推理
GPU 磨皮     gpu_smooth.cpp                             D3D11 Compute（独立通路）
```

### 1.1 渲染一帧做了什么

1. `blitFrame` 把宿主帧（float 或 byte、各种行宽）统一成连续 **RGBA8** 缓冲。float→byte 即 `*255`，**不做伽马解码**（DaVinci YRGB + Rec.709 下正确）。
2. `engine.process(pin,…)` → `coreBeautyProcess` → `buildMaskInto` → `FaceDetector::detect`。
3. 解码人脸框 + 5 关键点 → 肤色椭圆掩码 + 五官保护 → 磨皮/美白/美型**只作用于脸内**。
4. `blitFrame` 把结果写回宿主原始深度与行宽。
5. 面板全 0（且所有 `want*` 开关均为假）时走早退分支：`memcpy(dst, src, need)` 后直接返回，零成本，与旧实现同一条路径（`beauty_core.cpp:1823`）。注意早退条件是"全部关闭"，只开锐化/清晰/美形同样会走完整链路。

### 1.2 两条 GPU 通路（别混）

| 通路 | 控制项 | 加速对象 | 失败表现 |
|---|---|---|---|
| ONNX 推理 EP | `BEAUTY_GPU` | 人脸检测（DirectML） | 退回 CPU 推理，`region=` 仍为 on，仅速度差 |
| GPU 磨皮 | `gpu_smooth`（ini）/ `BEAUTY_GPU_SMOOTH` | 磨皮卷积（D3D11 Compute） | 当场回落 CPU，画面一致，日志会写回落原因 |

两者互不依赖：可以 GPU 磨皮 + CPU 推理。选卡只排除 WARP 软件渲染器，**不按厂商过滤**（AMD / Intel 核显同样会走 GPU 分支）。

### 1.3 降级规则（曾被写错，这里按实测）

| 状态 | 判据 | 行为 | 用户看到 |
|---|---|---|---|
| 从没检到过脸 | `everHadMask_ == false` | `process()` 整帧逐字节透传 | "插件完全没效果"，像没装上 |
| 检到过、当帧丢了 | 有旧掩码但本帧检测失败 | 沿用旧掩码最多 `kMaskHold`（=6，`beauty_core.cpp:4031`）帧，之后退回**全帧磨皮** | 效果溢出到整帧，脸边界消失 |

早期文档把两种情况混写成"检测挂了 = 全局磨皮"，导致排错方向跑偏：**实际最常见的是透传**。判别看 §6。

---

## 2. 人脸检测的四条硬约束

模型：`02_引擎资产/yunet.onnx`（YuNet 2023mar）。导出为动态 shape，实测 320/480/640 均可跑，默认推理边长 `640`；固定 shape 的模型必须用它自己的尺寸，不能自选。

### 2.1 预处理必须喂原始 0–255

**结论：绝对不能归一化。** 本地用模型实跑（真实人脸图）：

| 预处理 | 检出数 | bestScore |
|---|---|---|
| **raw 0–255（RGB 或 BGR）** | **8** | **≈0.92 ✅** |
| `(x−127.5)/128` | 0 ❌ | ≈0.07 |
| `x/255` | 0 ❌ | ≈0.05 |
| 模拟 scene-linear 变暗后 raw | 8 ✅ | ≈0.91 |

- **通道序不是问题**：RGB / BGR 两种 raw 都能检出，按源缓冲原顺序喂即可，**无需交换**。
- 置信度阈值 `conf=0.6`、`iou=0.3`、`topk=4`（`beauty_core.cpp:2890`）。另有 `:2709` 一处 `conf=0.9` 的内部调用（预热/自检路径），阈值不同属正常。
- 调试开关 `BEAUTY_YUNET_PREPROC`（`beauty_face.cpp::load`）：`0`=raw（**默认/唯一正确档**）；`1`=显式 BGR raw；`2/3`=`(x−127.5)/128`；`4/5`=`x/255`。后四档是排查用的旧档，**正常不应使用**。

> 这个模型与常见人脸检测封装的默认归一化**正好相反**，是本工程头号 footgun。

### 2.2 帧行序是 bottom-up

OFX 规范规定 `kOfxImagePropData` 指向图像**左下角**（`ofxImageEffect.h`），Resolve 严格照做：插件拿到的缓冲第 0 行是画面最下面一行。而 PIL / numpy / 离线夹具按 top-down 存帧，于是"离线 0.92 通过、真机 0.25 全灭"。

同一帧、同一模型、**只改读行方向**：

| 读行方向 | bestScore | faces | 画面改动 |
|---|---|---|---|
| top-down（错误） | **0.2519** | 0 | 0.00%（整帧透传） |
| bottom-up（正确） | **0.9333** | **1** | 1.56%（只动人脸） |

**实现是零成本翻转**：`letterboxRgb()` 本来就要重采样，直接把读指针换成 `sh-1-y`，不需要整帧翻转（省一次 1080p memcpy），插值权重仍属于"视觉行"：

```cpp
const int r0 = flipY ? (sh - 1 - y0) : y0;
const int r1 = flipY ? (sh - 1 - y1) : y1;
```

检测结果是**视觉坐标**，必须映射回缓冲行号，下游（掩码绘制、几何美形）才不用改：

```cpp
f.y          = flipY ? (h - vy - f.h) : vy;   // 框：y' = h - y - 高度
f.kps[p*2+1] = flipY ? (h - vy2)     : vy2;   // 关键点：y' = h - y
```

- 掩码、磨皮、美白、几何变形**全部按缓冲行号落笔**，所以只要"检测时反读 + 结果映射回来"这一处改对，下游一行都不用动。
- 默认 `imgBottomUp_ = true`（生产路径只有 OFX 宿主）。
- `BEAUTY_DEBUG_DETECT` 日志里的 `flip=0/1` 是当前生效行序，排错先看它。

> **凡是"上/下"有语义的几何量，都必须显式声明行序。** 检测修好行序之后，`warpFace()` 仍按 top-down 用 `f.y`，于是下巴压到发际线。这类 bug 伪装性极强：径向各向同性的运算（大眼）完全不受影响，看上去像"只有某一项坏了"。拿 `f.y` 推上下的地方一律传 `flipY` 并用 `sgn`，不要靠注释约定。

### 2.3 异步检测在按需渲染宿主里等不到掩码

`asyncOn_` 默认开：`updateMask` 只把任务交给 worker，当帧返回 `false`。连续 30fps 实时播放没问题，但 **Resolve 只在播放头移动时渲染**，单帧导出更是只渲染一帧——worker 结果永远赶不回来，`everHadMask_` 恒为 `false`，`process()` 一直走首帧透传。

**修复**：异步分支里，`everHadMask_ == false` 时**先同步做一次检测**（仅首帧，多花约 15ms），拿到掩码再交给异步。任何宿主第一帧就有掩码。

### 2.4 参数句柄为空必须回落默认值

旧写法 `double v = 0.0; if (d->p[i]) paramGetValue(...)`：句柄为空或取值失败时 `v` 保持 0 → 面板全 0 → 命中"全 0 逐字节透传"早退。

**日志特征**：`sdk=1.0ms`（1080p 跑一次检测 ≥15ms，1ms 只够一次 memcpy）+ `faces=0` + `score=0.00`。
**修复**：回落到 `kParams[i].dflt`；`createInstance` 取不到句柄会打 `WARN N/M 个参数句柄取不到：<名字>`。

---

## 3. 构建

- `01_ofx插件源码/build.bat` 是**薄壳**，只做 `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 %*`。真正逻辑在 `build.ps1`：vswhere 找 VS → 取最新 MSVC 与 Windows SDK → 编译 `src\*.cpp`，参数 `/O2 /EHsc /MD /W3 /utf-8 /arch:AVX2 /std:c++17` → `out\DaVinciBeauty.ofx`。支持 `-Clean` / `-NoInstall`。
- **为什么不用纯 .bat**：批处理里的非 ASCII 字面量由 cmd 按 OEM 代码页（936/65001）解码，切换即乱码，cl 报 `cannot open include file`；`%~dp0` 动态展开不受影响，所以"只有中文字面量出问题"。PowerShell 的 argv 是 UTF-16，`build.ps1` 自身写成纯 ASCII，任何代码页都能解析。`build.bat` 也保持纯 ASCII + CRLF。
- onnxruntime 头：`02_引擎资产\onnxruntime\include`，脚本按 `07_*` 前缀**自动定位**，目录改名不用改脚本。
- 构建**可复现**：`build.ps1` 链接后用 `Reset-PeTimestamp` 把 COFF 与 `IMAGE_DEBUG_DIRECTORY` 两个 `TimeDateStamp` 清零，同源码重编逐字节相同（实测稳定 `5655699…`）。注意 `/Brepro` 在本工具链上**不生效**，可复现靠的是上面那步，别再引用旧说法。
- 自检：`cmd /c "build.bat < nul"`（`nul` 喂 stdin 避免旧版末尾 `pause` 挂起）。通过会打印 `[OK] out\DaVinciBeauty.ofx <bytes> md5=…`。
- ⚠️ 任何源码改动**必须实跑构建**。静态分析曾漏掉符号级错误（`face_.provider()` 不存在、成员名写错 `lastFaces_`）。

## 4. 部署

- `install.bat`：拷 `DaVinciBeauty.ofx` + `onnxruntime.dll` + `yunet.onnx`（有则加 `LICENSE` / `THIRD-PARTY-NOTICES.md`）到 `bundle\DaVinciBeauty.ofx.bundle\Contents\Win64`，再装到 `%COMMONPROGRAMFILES%\OFX\Plugins\…`，并清理旧引擎残留。引擎资产目录按"含 `onnxruntime.dll` + `yunet.onnx`"自动识别，不依赖名字。
- 装前**必须完全退出 Resolve**（否则 DLL 被占用，改名 `.locked-<pid>` 留垃圾，攒几次上百 MB）。
- 运行方式：管理员 cmd → `cd` → `install.bat`，**不要双击**。
- **现状（1.3.5.0）**：`install.bat` 已不是纯 ASCII——有 3 行 UTF-8 中文（`:24` 的 echo、`:51-52` 的 rem）。解析不受影响（多次安装成功），但 936 代码页下那行 echo 会显示乱码。另含 `%~dp0..\LICENSE` 这类**父目录引用**（不是通配符里的 `..` 雷区）。
- ⚠️ **`acceptance.py` 的「批处理编码」检查抓不到这种情况**：它只判断文件能否按 GBK 解码，而 UTF-8 中文的字节恰好也能通过，于是报「17 个 .bat 全部合规」。要真查得加一条「含非 ASCII 且能按 UTF-8 解出 CJK → 违规」。是否改成这样见 `docs/INDEX.md` 待确认项。

## 5. 配置：ini 是第一层，环境变量是第二层

**三级优先级：环境变量 > `beauty.ini` > 代码内默认。** 配置文件在 `%ProgramData%\DaVinciBeauty\beauty.ini`，首次运行自动生成带 BOM 的模板（无 BOM 时记事本显示中文注释为乱码）。改完**需重启 Resolve** 生效。

为什么配置项落在磁盘而不是环境变量：从桌面图标启动 Resolve 时，bat 里 `set` 的变量继承不到，曾因此把"日志里一行管道都没有"误判成管道故障。

### 5.1 `beauty.ini` 的 9 个键

| 键 | 取值 | 作用 |
|---|---|---|
| `path` | `auto`(默认) / `gpu` / `cpu` | 按硬件探测自动定档，或强制走某条路 |
| `gpu_pipe` | `auto`(默认) / `on` / `off` | GPU 渲染管道。`auto`=首帧自检一次，之后零开销；`on`=每帧都过（验证用，多 3~4ms） |
| `gpu_smooth` | `auto`(默认) / `on` / `off` | 磨皮走 D3D11 Compute。`auto` 任何一步失败当场回落 CPU；`off` 行为等同 1.0.8.0 |
| `gpu_smooth_pipeline` | `auto`(默认) / `off` | 取回是否推迟一帧。`auto` 消掉约 18% 帧的 6~14ms 卡顿，代价是 ROI 内晚一帧（ROI 外仍是当前帧）；跳帧/拖播放头/单帧渲染自动退回同步 |
| `gpu_minpix` | 整数，`0`=自动 | 低于此像素数走 CPU（0 按实测带宽推导） |
| `dump_enable` | `0`(默认) / `1` | 掩码有效帧导出 src/dst/mask 三图，1080p 约 14MB。**取证才开** |
| `dump_dir` | 目录，留空=默认 | 导出目录，默认 `%ProgramData%\DaVinciBeauty\dump` |
| `neck_taper` | `0` / `0.30`(默认) | 脖子带下缘收口深度（占带长比例）。`0`=水平截断（1.1.5.0 行为）。A/B 才改 0 |
| `neck_skinend` | `1`(默认) / `0` | 下缘跟随肤色末端（只缩不扩，不会盖到衣领）。`0`=纯几何弧，用于逐字节回归 |

### 5.2 环境变量：常用 9 个，其余是内部调参

| 变量 | 取值 | 作用 |
|---|---|---|
| `BEAUTY_GPU` | `0` / `1` / 不设 | 强制 CPU / 强制 GPU（不可用即报错）/ **自动**。只管 ONNX 推理 EP |
| `BEAUTY_GPU_SMOOTH` | 空或 `0` = off；其它 = on | 覆盖 ini 的 `gpu_smooth`，临时排查用 |
| `BEAUTY_YUNET_PREPROC` | `0..5` | 检测预处理档，见 §2.1（**只有 0 是对的**） |
| `BEAUTY_ASYNC_DETECT` | `0` / `1`(默认) | 关 / 开异步检测。离线单帧自测必须设 `0`（首帧已有同步兜底） |
| `BEAUTY_MASK_EVERY` | 整数，默认 `0` | 异步下每隔几帧重算掩码（0=每帧） |
| `BEAUTY_IMG_BOTTOMUP` | `0` / `1` | 源帧行序。不设=引擎默认 bottom-up；离线喂 top-down raw 时设 `0` |
| `BEAUTY_DEBUG_DETECT` | `1` | 每帧检测结果追加到日志，含 `ncand / faces / best / 输入 min-max-mean / flip` |
| `BEAUTY_DEBUG_DUMP` | **纯 ASCII** 目录 | 落盘第一帧的 `src.raw` / `lb.raw` / `in.f32` / 12 个输出张量，用于"输入错还是推理错"二分 |
| `NAMA_OFX_LOG` | 路径 | 覆盖日志路径，默认 `C:\ProgramData\DaVinciBeauty\logs\ofx.log` |

源码里共 **113 个** `getenv` 名字（其中 109 个是 `BEAUTY_*`），绝大多数是磨皮/区域/GPU 的标定旋钮（`BEAUTY_S3_*`、`BEAUTY_REG_*`、`BEAUTY_FREQ_*` 等），**不属于对外接口**。查全集：

```bash
grep -rho 'getenv("[A-Z_0-9]*")' 01_ofx插件源码/src/*.cpp | sort -u
```

> `BEAUTY_DEBUG_DUMP` 与 `dump_dir` 的路径**必须纯 ASCII**（内部用 `fopen`，非 ASCII 会静默写不出来）。

## 6. 日志判读

日志：`C:\ProgramData\DaVinciBeauty\logs\ofx.log`（`NAMA_OFX_LOG` 可改）。关键行：

| 行 | 含义 |
|---|---|
| `onLoad 成功 … build=1.3.5.0` | 插件加载成功，`build=` 是插件版本 |
| `createInstance 预热引擎 成功 … build=… ver=…` | 引擎初始化。**`ver=` 是 `CoreEngine::version()` 硬编码值（`beauty_core.cpp:1338`），与插件版本不同源，别用它判断版本** |
| `createInstance 语义区域 region=on` | 人脸模型已加载并完成区域推理 |
| `createInstance 语义区域 region=off <原因>` | 模型没加载，**退回颜色启发式**（代码注释里称 "v1.4 颜色启发式"，指旧的颜色判定路径，不是版本号）。不是完全没效果，是效果不再限制在皮肤内 |
| `面板参数 磨皮/清晰/美白/… = …` | 参数变化时打一行；全 0 就是参数没进来 |
| render 行 `faces=N score=%.2f … sdk=%.1fms` | 检测是否真跑起来的铁证 |
| `GPU 磨皮后端已就绪：<型号> [厂商 0x…，显存 x.x GB]` | GPU 磨皮真正生效；**一行都没有 = 压根没进 GPU 分支** |

### 一眼判别

| 日志特征 | 含义 | 去查 |
|---|---|---|
| `sdk≈1ms` + `faces=0` + `score=0.00` + 面板参数全 0 | 参数没进来 → 逐字节透传 | §2.4 |
| `sdk≈1ms` + `faces=0` + 面板参数非 0 | 掩码没到 → 走透传分支 | `region=`、§2.3 |
| `sdk≥15ms` + `faces=0` + `score` 是小正数（0.0x~0.5） | 检测真跑了但没到 `conf=0.6`。**若 score≈0.25 且画面有正脸 → 先查 `flip=`** | §2.2 |
| `sdk≥15ms` + `faces>0` + `score>0.6` | **正常**，效果只作用在脸内 | — |

## 7. 两个下拉框与两条链路

### 7.1 `blur_type`（磨皮算法）

本节这两个下拉框**曾经都是空接线**：值塞进 `BeautySettings` 但 `process()` 从不读，三档输出逐字节相同（当时 `baseline.py` 里 `mode1/mode2/mode3` 与 `blur62_wh30` 是同一个 md5，就是证据；现在四者已各不相同）。

| 档 | 实现 | 实测 |
|---|---|---|
| 0 双边滤波（默认） | `surface3Rgb` 保边路径 | 与改前逐字节相同（基线 `alg0` 与 `blur62_wh30` md5 同为 `3b8d0275…`） |
| 1 高斯 | `bilateralRgbaRoi`，`sigmaColor` 放到 255（值域权重恒 1 → 退化成纯空间高斯） | 距源帧 +46%，眼镜框明显发虚 |
| 2 弱磨皮 | 同一套 `surface3Rgb`，`I *= 0.5` | 距源帧 −42% |

### 7.2 `smooth_mode`（磨皮模式：削掉的细节怎么还回去）

| 档 | 实现 | 实测（距源帧） |
|---|---|---|
| 0 人像磨皮 | 不回填，逐字节不变 | 基准 1.031 |
| 1 清晰磨皮 | `fine ×2.2`、`mid 0.18·I` | +5.7%（回填过冲 → 更锐） |
| 2 精细磨皮 | `fine ×3.2`、`mid 0.40·I` | +9.5% |
| 3 轻磨皮 | 不回填，`I *= 0.55` | −37.8%（更接近原图） |

改这块之前先读两条：

1. **模式 1/2 必须自带回填**，不能复用 `wantBlurRefill`——那条路要求"锐化=0 且清晰=0"，且只在 band 分支求值。用户拖"磨皮模式"时锐化通常是 0，不另开一条这个下拉框依旧是死的。
2. **倍率必须远大于 1，且两档都带中频**。`blurRefill=0.06` 是给 band 路径标定的，单靠细频只回填 5%，落在 8bit 上四舍五入后**一格都不动**（第一版 `mode1` 增益 1.0×，实测与默认档 md5 完全相同）。

⚠️ `NamaSettings::blur_type` 的默认值已从 `1` 改成 `0`：旧默认与面板默认（0）不一致，只是过去从不被读所以没暴露；一旦接线，自测路径会静默跑成"高斯"，把 18 条基线全部打掉。

### 7.3 强度总增益：为什么是"事后乘"

surface3 的合成式是 `res = c + (even−c)·I`，满档 `I=0.976`、`e=0.95`，**两条曲线都顶死**——想更强只能动合成式本身。数学上让 `I>1` 等价，但**只有 surface3 这一条路有 I**：高斯档靠 `blendWithMask` 的 `t`、legacy 盒式靠 `mixRgba` 的 `t`、25 tap 与降采样各有各的。所以统一在"磨皮写完 dst、清晰/美白/细节回填之前"乘一次：

```
dst' = src + (dst − src) · gain      // beautycore::amplifySmooth
```

位置不能挪：放后面会把清晰和美白一起放大（那是另一个旋钮）；放前面 dst 还是原图，没有差值可放大。`gain == 1` 时函数内部直接 return，**逐字节等于改前那一版**，基线据此判回归。

**两条硬边界**：

- **混合类路径不许外推。** 高斯/legacy 的强度就是 `tBlend`，`>1` 的语义是"比完全平滑还平滑"，落到模糊图上等于反向补偿边缘 → 硬边 + 光晕。实测高斯档外推到 1.51 时人脸区最大改动 **87 灰阶**；把增益乘进 `tBlend` 并封顶 1.0 后降到 59。`amplifySmooth` 只给 surface3 的重建路径用。
- **gain 安全上限 ≈2.0–2.2，定档 1.8。** `(dst−src)` 里混着**振幅门控结构**（`gw` 逐像素变化），放大过头会压过磨皮本身。判据是高频保留率 `tex`：1.0→0.942、1.6→0.950、2.0→0.957、**2.4→0.998**（回到 1.0 就不是"磨得更狠"而是"平了但发毛"）。取 1.8（res 1.82 / maxd 23 / tex 0.953）。现场用 `BEAUTY_SMOOTH_GAIN` 调，代码钳位 `0.25–3.0`（`beauty_core.cpp:1621`）。

**档位差异为什么不能靠调 I**：「清晰/精细」的语义是把磨掉的高频按更大增益**还回去**，走 `dp.fine` / `dp.mid` 这条正交的腿；动 I 会把"磨皮"和"磨皮模式"两个旋钮绑死。

### 7.4 两条链路必须分别验

| 链路 | 入口 | 覆盖它的测试 | 验的是什么 |
|---|---|---|---|
| 自测 | `NamaSelfTest` + `NAMA_ST_*` | `baseline.py`（18 条逐字节） | 引擎**接没接**这个参数 |
| 面板 | `readParams()` → `coreBeautyProcess` | `mock_host.py`（`paramSetValue`） | 参数有没有**正确送到**引擎 |

`baseline.py` 直接调引擎、**绕过 `readParams()`**，那里的错误它一律看不见：曾在 `readParams` 里留着 SDK 时代的 `map[3] = {1,0,2}`（面板序 ↔ SDK 序互换），把"双边"和"高斯"对调，而 18 条基线全绿。

**规矩**：新增或改动任何面板参数，两边都要有用例；面板侧一律用 `paramSetValue`（与真人拖控件等价），并断言**方向**而不只是"有变化"——档位对调时"有变化"依然成立，只有方向能抓住。参考 `mock_host.py` 的"下拉框接线"：算法三档断言"高斯最糊、弱磨皮最轻"；模式四档断言 9×9 中频保留率 `0 人像 < 1 清晰 < 2 精细` 且 `3 轻磨皮` 距源帧明显更小。

> **反向单调**：回填越多，画面越接近原图，"距源帧"反而**变小**。旧断言 `_mod[2] > _mod[0]*1.02` 在增益提到 5.5 之后必然失败——而画面完全正确。改量 `|G − box9(G)|`：人像 8.397 < 清晰 8.781 < 精细 9.069，方向单调。**3×3 细频不够用**：回填主体是中频，细频只有 `0.06·I·4.0`，8bit 上常四舍五入为 0，四档只差 0.5%，断言拉不开。

## 8. 测试现状

| 套件 | 位置 | 现状 |
|---|---|---|
| OFX 契约（mock host） | `03_测试夹具/mock_host.py` | **23/23 PASS**（含 22 项宿主契约 + 下拉框接线） |
| 逐字节基线 | `04_验收工具/baseline.py check` | **18/18 一致** |
| 线程池回归 | `04_验收工具/pool_regress.ps1` | **5227 组 PASS**（分块守恒，自带看门狗） |
| 一键验收 | `04_验收工具/acceptance.py` | 28 项，见 `docs/guides/guide-acceptance.md` |

跑夹具**必须指定引擎目录与 Python**（PATH 里的 `python` 是 Microsoft Store 占位符）：

```bat
set NAMA_OFX_DLL=%CD%\..\01_ofx插件源码\out\DaVinciBeauty.ofx
set NAMA_ENGINE_DIR=%CD%\..\01_ofx插件源码\out
set PYTHONUTF8=1
py -3.12 mock_host.py
```

`out\` 下必须有 `onnxruntime.dll` + `yunet.onnx`（引擎按 `.ofx` 同目录找模型），否则 `region=off`、用例全部退化。**临时编译产物也要把这两个文件拷到旁边**，否则 18 例全红是假警报。

三条夹具侧的坑（都曾造成假 FAIL，均已修）：

1. **帧必须 bottom-up**（`_to_bottom_up()`）——与真实宿主同序，否则检测看到倒立的脸。
2. **判据要"与源帧比"，不要"两次渲染互比"**——引擎有跨帧状态（肤色中心 EMA、掩码缓存、细节层 EMA），互比会量出"四角差 36 / 人脸区 107"这种离谱数字。
3. **合成素材（纯色/阶梯图）用独立实例渲染**——复用主实例时，上个用例留下的肤色 EMA 会让语义区域判"无皮肤 → 不处理"，整帧变暗。

`acceptance.py` 的"接口"组驱动的是**已装机**插件（`INSTALLED` 优先），装完新版才转绿；"版本"组的字节比对是**硬判据**（构建可复现，装机件与 `out\` 不一致直接 FAIL，提示先跑 `install.bat`）。

## 9. 排错决策表

| 症状 | 先查什么 | 再去 |
|---|---|---|
| 拖上效果完全没反应 | `面板参数` 是否全 0；`sdk=` 是否 ≈1ms | §2.4 |
| 有正脸但 `faces=0`、`score≈0.25` | `flip=0/1` | §2.2 |
| 有正脸但 `faces=0`、`score≈0.05` | 预处理档 | §2.1 |
| 效果溢出到整帧 | 是否曾检到过脸后丢失 | §1.3 |
| 离线 OK、真机不行 | 行序 / 通道序 / 步长这三件事，**不是模型** | §2.2 |
| 莫名变慢但画面正常 | GPU 磨皮是否已静默停用（有无"后端已就绪"行） | §1.2 |
| 面板拖了没变化 | 先量幅度，再怀疑接线（满档可能只改 0.955 灰阶） | §7.4 |
| 量出来的结果与预期反向 | 指标本身是否单调（背景稀释 / 反向单调） | §7.4 |

**排错第一步是 dump，不是猜**：把真正送进模型的 `in.f32` 落盘、交给 Python 用同一个 onnx 重跑，能一次切开"输入错 / 推理错 / 解码错"。

> 自测本身也会骗人：`selftest_ofx.py` 的 `PASS` 判据只是「`mx>=3` 且改动像素 >1%」，而且它量的是**输出文件**——上一次残留的旧文件同样能过。判据要看 `msg=faces=N best=X`。

## 10. 已作废的说法（别再引用）

| 旧说法 | 现状 | 依据 |
|---|---|---|
| "检测挂了 = 全局磨皮" | 多数是**逐字节透传**；全局磨皮只在"曾有过掩码又丢"时出现 | §1.3 |
| "归一化能提升鲁棒性 / 通道序要交换" | raw 0–255 才对；RGB/BGR 都行，无需交换 | §2.1 |
| "`region=off` 就是坏了" | 是**退回颜色启发式**（旧路径），仍有基础效果，只是不再限制在皮肤内 | §6 |
| "装机件与 `out\` 字节不同属正常（PE 时间戳）" | 时间戳已被清零，构建可复现，字节不一致是**真问题** | §3 |
| "`/Brepro` 让重编逐字节相同" | 本工具链不生效，可复现靠 `Reset-PeTimestamp` | §3 |
| "mock host 20 项" | **23 项** | `mock_host.py` 输出 23 行 |
| "`install.bat` 纯 ASCII、无 `..`" | 有 3 行 UTF-8 中文；含 `%~dp0..\` 父目录引用（非通配符雷区） | §4 |
| "自测 `PASS` 即通过" | `PASS` 只要求"有改动"，要看 `msg=faces=N best=X` | §9 |
| "插件版本 3.0.x" | 旧编号已退役，唯一现行序列是 `1.3.5.0` 四段式 | `docs/CHANGELOG.md` |

## 11. 版本号的唯一出处

`nama_engine.h` 的 `NAMA_BUILD_MAJOR/MINOR/PATCH/REV` 与 `NAMA_BUILD_STR`。面板显示、二进制内嵌串、安装包文件名、验收清单里那几份文档的版本行全部派生自此；改一处而漏其它，`acceptance.py` 的"版本"组与"文档版本号同步"会直接红。

```bash
grep -rn "NAMA_BUILD_STR" 01_ofx插件源码/src/nama_engine.h
```
