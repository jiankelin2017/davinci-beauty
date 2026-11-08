# DaVinciBeauty 开发文档

> 适用代码版本：1.3.5.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：未登记 ｜ 替代文档：原根目录 `开发文档.md`
> 配套的「日常 5 步工作流 / 编码约束」见 `docs/guides/guide-dev-workflow.md`。本文件偏**架构、关键技术事实、调试与排错**。
> 插件 ID 不变：`com.nama.davinci.beauty`。文中提到「早期版本」的地方都是 2026-11-06 之前的事实，其 3.x 旧编号已退役，不再参与版本判定。

---

## 0. 一句话定位

达芬奇 Resolve 的 OFX 美颜插件。引擎替换那次起彻底脱离旧版闭源商业 SDK，改为**自研纯 CPU 美颜引擎**（移植自上一代宿主 3.1.x）+ **ONNX 人脸检测**。人脸推理支持 GPU 自动加速，无 GPU 时自动退回 CPU，效果不变。

---

## 1. 架构与调用链

```
OFX 宿主层   ofx_plugin.cpp            取帧 / 色彩转换(blitFrame) / 写回 / 日志
   ↓
引擎适配层   nama_engine.cpp            NamaSettings → CoreEngine 参数，转调
   ↓
桥接层       core_glue.h / beauty_engine_glue.cpp   C ABI 桥
   ↓
自研引擎     beauty_core.cpp
           + beauty_smooth / beauty_region / beauty_guide
             beauty_warp / beauty_freq / beauty_pool   磨皮/美白/美型/肤色掩码
人脸检测     beauty_face.cpp + beauty_onnx.cpp        ONNX 人脸检测推理
```

渲染一帧（`ofx_plugin.cpp` 的 render）：

1. `blitFrame` 把宿主帧（float 或 byte）统一转成连续 **RGBA8** 缓冲 `pin`。float→byte 即 `*255`，**不做伽马解码**（DaVinci YRGB + Rec.709 下正确）。
2. `engine.process(pin, ...)` → `coreBeautyProcess` → `buildMaskInto` → `FaceDetector::detect`（人脸检测模型）。
3. 解码出人脸框 + 5 关键点 → 肤色椭圆掩码 + 五官保护 → 磨皮/美白/美型**只作用于脸内** → 写回 `pout`。
4. 再次 `blitFrame` 把 `pout` 写回宿主目标深度。
5. **降级规则**（早期文档写错过，这里按实测口径）：
   - **从未拿到过掩码**（`everHadMask_ == false`）→ `process()` 走**整帧透传**，画面**一个字节都不改**；
   - **拿到过掩码、后来丢了脸** → 沿用旧掩码最多 `kMaskHold` 次，之后退回**全帧磨皮**。

   早期文档把两种情况混写成"检测挂了 = 全局磨皮"，导致排错方向跑偏：实际最常见的是**透传**（看起来"插件完全没效果"），而不是全局磨皮。判别方法见 §6。

---

## 2. ⚠️ 人脸检测铁律【最高优先级，已踩坑】

模型文件：`02_引擎资产/yunet.onnx`（公开人脸检测模型 2023mar 版；input `[1,3,640,640]`，可用 `setInputSize` 调小提速，向下取整到 32 的倍数）。

**预处理必须喂【原始 0–255】，绝对不能做归一化。** 本地用模型实跑（真实人脸图）结果：

| 预处理 | 检出数 | bestScore |
|---|---|---|
| **raw 0–255（RGB 或 BGR）** | **8** | **≈0.92 ✅** |
| `(x−127.5)/128` 归一化 | 0 ❌ | ≈0.07 |
| `x/255` 归一化 | 0 ❌ | ≈0.05 |
| 模拟 scene-linear 变暗后 raw | 8 ✅ | ≈0.91 |

要点：

- **通道序不是问题**：RGB / BGR 两种 raw 都能检出（对检出无影响），默认按源缓冲原顺序喂入即可，**无需交换**。
- 这是「清晰正脸却全局磨皮」的**真正根因**：早期两版 `detect` 都做了归一化，后来已改回 raw。
- 置信度阈值 `conf=0.6`（`beauty_core.cpp` 调用 `detect(..., 0.6f, 0.3f, 4)`）。
- 调试开关 `BEAUTY_YUNET_PREPROC`（0..5，见 `beauty_face.cpp::load`）：
  - `0` = raw 0–255（**默认 / 修复档**）
  - `1` = 显式 BGR raw（仅当宿主缓冲是 RGBA 且模型偏好 BGR 时）
  - `2/3` = `(x−127.5)/128` RGB / BGR（旧档，**正常不应使用**）
  - `4/5` = `x/255` RGB / BGR（旧档，**正常不应使用**）

> 教训：模型预处理**以模型实跑为准**，不要凭"通用视觉库的训练惯例"猜。本 `yunet.onnx` 与常见的人脸检测封装默认归一化**正好相反**——这也是当时误诊的根。

---

## 2-b. ⚠️ 三个"假故障"——当时定位【必读，都曾把排错带进沟里】

改回 raw 之后仍然出现"清晰正脸、画面毫无效果、日志 `faces=0 score=0.00`"。用 dump 把**引擎实际送进模型的缓冲**落盘后，真因有三条，**没有一条是检测算法本身的问题**：

### (1) `NamaSelfTest` 把「文件路径」当成了「图像数据」→ 离线自测全部无效

`nama_engine.cpp` 的 `NamaSelfTest(engineDir, rawRgbaIn, rawRgbaOut, ...)` 参数名是"raw 内存缓冲"，但探针脚本 `selftest_ofx.py` 传的是**文件路径**。v2.0 移植到 CoreEngine 时把原来"读文件→处理→写文件"的实现改成了"直接把入参当指针用"，于是：

- 喂进检测的是文件路径字符串（如 `C:\...\frame.png`）；
- 检测必然 0 脸，`bestScore` 只有 0.1x（那是随机字节喂出来的值）；
- 输出写进 Python 的 `bytes` 对象 → **段错误**。

**因此那期间所有"引擎 0 检出"的离线结论全部作废**。修复后同一张 `probe_in.raw`：

| | 修复前（喂路径字符串） | 修复后（真读文件） | Python 参考值 |
|---|---|---|---|
| faces | 0 | **1** | 1 |
| bestScore | 0.1727 | **0.9214** | 0.9214 |
| 输入均值 | 31.1 | **40.9** | 41.0 |

**教训**：`NamaSelfTest` 的 `rawRgbaIn/Out` 语义是**文件路径**（UTF-8，内部用 `_wfopen`）。改它的签名或实现时，先跑 `03_测试夹具\selftest_ofx.py` 确认 `msg=faces=1 best=0.9xxx`。

### (2) 异步检测在「按需渲染」的宿主里永远等不到掩码 → 永久透传

`asyncOn_` 默认开：`updateMask` 只提交给 worker，当帧返回 `false`。连续 30fps 的实时宿主无所谓，但 **Resolve 只在播放头移动时渲染**，单帧导出更是只渲染一帧 —— worker 的结果永远赶不回来，`everHadMask_` 恒为 `false`，`process()` 就一直走"首帧透传"分支。

修复：`updateMask` 的异步分支里，**`everHadMask_ == false` 时先同步做一次检测**（仅首帧，多花约 15ms），拿到掩码后再交给异步。这样任何宿主**第一帧就有掩码**。

### (3) 参数句柄取不到时静默给 0 → 引擎走"逐字节透传"

`readParams` 原写法是 `double v = 0.0; if (d->p[i]) paramGetValue(...)`。句柄为空或取值失败时 `v` 保持 0 → 面板全 0 → `process()` 命中"面板全是 0：逐字节透传"早退分支。

**日志特征**：`sdk=1.0ms`（1080p 上真跑一次检测 ≥15ms，1ms 只够一次 memcpy）、`faces=0`、`score=0.00`。

修复：回落到 `kParams[i].dflt` 声明默认值；`createInstance` 里句柄取不到会打 `WARN N/M 个参数句柄取不到：<名字>`。

---

## 2-c. ⚠️ 帧行序是 bottom-up —— 当时定位【Resolve 侧 faces=0 的真因】

**OFX 规范规定 `kOfxImagePropData` 指向图像的左下角**（`ofxImageEffect.h`：*"a pointer to memory that is the lower left hand corner of an image"*）。Resolve 严格照做：插件拿到的缓冲**第 0 行是画面最下面一行**。而 PIL / numpy / 所有离线夹具都按 top-down 存帧，于是"离线 0.92 通过、真机 0.25 全灭"。

### 证据（同一帧、同一模型、只改读行方向）

真机 dump：`<dump目录>\src.raw`（1080×1920 RGBA，从 Resolve render 里原样落盘）

| 读行方向 | bestScore | faces | 画面改动 |
|---|---|---|---|
| 按 top-down 读（修复前） | **0.2519** | 0 | 0.00%（整帧透传） |
| 按 bottom-up 读（修复后） | **0.9333** | **1** | 1.56%（只动人脸） |

`0.2519` 与 Resolve 日志里那串 `score=0.25` **完全一致**，可反向确认真机帧确实是 bottom-up。同一张图导出成 JPG（top-down）检测 0.9411 —— 说明模型和预处理都没问题，**只是行序反了**。

### 实现：零成本翻转，只改读行方向

`letterboxRgb()` 本来就要做一次缩放重采样，所以直接把读指针换成 `sh-1-y` 即可，**不需要整帧翻转**（省一次 1080p 的 memcpy），插值权重仍然属于"视觉行"：

```cpp
const int r0 = flipY ? (sh - 1 - y0) : y0;
const int r1 = flipY ? (sh - 1 - y1) : y1;
```

检测出来的框/关键点是**视觉坐标**，必须再映射回缓冲行号，下游（掩码绘制、几何美形）才不用改：

```cpp
f.y          = flipY ? (h - vy - f.h) : vy;   // 框：y' = h - y - 高度
f.kps[p*2+1] = flipY ? (h - vy2)     : vy2;   // 点：y' = h - y
```

要点：

- **掩码、磨皮、美白、几何变形全部按缓冲行号落笔**，所以只要"检测时反读 + 结果映射回来"这一处改对，下游一行都不用动。
- 对称类处理（盒式低通、颜色变换）本来就与行序无关。
- 默认 `imgBottomUp_ = true`（`beauty_core.cpp` 成员初值），因为生产路径只有 OFX 宿主。
- 离线工具喂 top-down 的 raw 时：`NamaSelfTest` 内部**强制声明 top-down**，也可以用 `BEAUTY_IMG_BOTTOMUP=1` 让它按 bottom-up 读真机帧。
- `BEAUTY_DEBUG_DETECT` 日志里的 `flip=0/1` 就是当前生效的行序，排错时先看它。

> 教训：**"离线通过、真机全灭"优先怀疑帧的行序/通道序/步长这三件事**，而不是模型。夹具替插件掩盖方向问题的代价，是三轮排查全部追在检测算法上。

---

## 3. 构建

- `01_ofx插件源码\build.bat`：**薄壳**，只做 `powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 %*`。
  真正的编译逻辑在 `build.ps1`：用 vswhere 找 Visual Studio → 取最新 MSVC 与 Windows SDK →
  编译 `src\*.cpp`，参数 `/O2 /EHsc /MD /W3 /utf-8 /arch:AVX2 /std:c++17` → `out\DaVinciBeauty.ofx`。
  `build.ps1` 支持 `-Clean` / `-NoInstall`。
- **为什么不是纯 .bat**：批处理里的非 ASCII 字面量（如 `..\02_引擎资产\...`）由 cmd 按当前 OEM
  代码页解码，936/65001 切换就会把路径变乱码，cl 报 `cannot open include file`；`%~dp0` 动态
  展开不受影响，所以「只有中文字面量出问题」。PowerShell 的 argv 是 UTF-16，且 `build.ps1`
  自身写成纯 ASCII，任何代码页下都能解析。`build.bat` 也保持纯 ASCII + CRLF。
- 依赖 onnxruntime 头：`02_引擎资产\onnxruntime\include`（脚本按 `07_*` 前缀自动定位，改名不用改脚本）。
- **可在本机实跑验证**：`cmd /c "build.bat < nul"`（用 `nul` 喂 stdin 避免旧版末尾 `pause` 挂起）。通过会打印 `[OK] out\DaVinciBeauty.ofx 252928 bytes md5=...`。
- ⚠️ 任何源码 / 构建改动**必须实跑 build.bat** 验证——静态分析曾漏掉符号级错误（`face_.provider()` 不存在、`nama_engine.cpp` 成员名写错 `lastFaces_`）。

---

## 4. 部署

- `01_ofx插件源码\install.bat`（**管理员 cmd** 内运行，勿双击）：拷 `onnxruntime.dll` + `yunet.onnx` + `DaVinciBeauty.ofx` 到
  `C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle`，并自动清理旧引擎的残留。
- 装前**必须完全退出 DaVinci Resolve**（否则 DLL 被占用、改名 `.locked-<pid>` 留垃圾，攒几次上百 MB）。
- install.bat 曾因 cmd 解析期雷区（通配符里的 `..`、`...`、中文 UTF-8 BOM）闪退；**现版本纯 ASCII + 无 `..`/`...`/括号 + CRLF + 无 BOM**。运行方式：管理员 cmd → `cd` → `install.bat`。

---

## 5. 运行时环境变量

| 变量 | 取值 | 作用 |
|---|---|---|
| `BEAUTY_GPU` | `0` | 强制 CPU |
| | `1` | 强制 GPU（不可用则报错） |
| | 其它 / 不设置 | **自动**（有 GPU 优先，缺则退回 CPU） |
| `BEAUTY_YUNET_PREPROC` | `0..5` | 人脸检测预处理档，见 §2 |
| `BEAUTY_ASYNC_DETECT` | `0` / `1`(默认) | 关 / 开异步检测。**离线单帧自测必须设 `0`**（否则首帧拿不到掩码；首帧已自动同步兜底） |
| `BEAUTY_MASK_EVERY` | 整数，默认 `0` | 异步模式下每隔几帧重算一次掩码（0 = 每帧） |
| `BEAUTY_IMG_BOTTOMUP` | `0` / `1` | 源帧行序。**不设置 = 引擎默认 bottom-up（OFX/Resolve）**；离线工具直接喂 top-down 的 raw 时设 `0`（`NamaSelfTest` 内部已强制 top-down，只有要回归真机帧时才需设 `1`） |
| `BEAUTY_DEBUG_DETECT` | `1` | 把每帧检测结果追加到 `BEAUTY_DEBUG_LOG`（默认 `C:\ProgramData\DaVinciBeauty\logs\detect_debug.log`），含 `ncand / faces / best / 输入 min-max-mean` |
| `BEAUTY_DEBUG_DUMP` | 目录（**必须纯 ASCII**） | 把第一帧真实尺寸的 `src.raw` / `lb.raw` / `in.f32` / 12 个输出张量落盘，用于"输入错还是推理错"的二分定位 |
| `NAMA_OFX_LOG` | 路径 | 覆盖日志路径，默认 `C:\ProgramData\DaVinciBeauty\logs\ofx.log` |

> 启用 GPU = 把 `02_引擎资产\onnxruntime\onnxruntime.dll` 换成带 GPU 支持的构建版（API 20 / 1.20.x 匹配）。

---

## 6. 日志与验收

- 日志：`C:\ProgramData\DaVinciBeauty\logs\ofx.log`（设 `NAMA_OFX_LOG` 可改）。
- 关键行：
  - `onLoad 成功 ... build=1.3.5.0`
  - `createInstance 预热引擎 成功 ... ver=1.4.0 / CPU`（或 `/ GPU`）—— 证明自研引擎 + 模型真正加载运行
  - `createInstance 语义区域 region=on` —— 人脸模型加载成功（`region=off` = 模型没加载，去查 `yunet.onnx` / `onnxruntime.dll`）
  - `面板参数 磨皮/清晰/美白/... = 50/0/30/...`（**参数变化时打一行**）—— 全 0 就是参数没进来
  - render 行：`faces=N score=%.2f ... sdk=%.1fms` —— **人脸检测是否打起来的铁证**

### 一眼判别三件事

| 日志特征 | 含义 |
|---|---|
| `sdk≈1ms` + `faces=0` + `score=0.00` + `面板参数` 全 0 | 参数没进来 → 引擎逐字节透传（查 §2-b(3)） |
| `sdk≈1ms` + `faces=0` + `面板参数` 非 0 | 掩码没到 → 走了透传分支（首帧已用同步兜底，若仍出现查 `region=`） |
| `sdk≥15ms` + `faces=0` + `score` 是小正数（0.0x~0.5） | 检测真跑了但没到 `conf=0.6`。**若 score≈0.25 且画面明明有正脸 → 先查 `flip=`**（§2-c） |
| `sdk≥15ms` + `faces>0` + `score>0.6` | **正常**，效果只作用在脸内 |

- 验收：清晰正脸挂效果，render 行应出 `faces>0 / score>0.6`。离线验收用 `03_测试夹具\selftest_ofx.py`，`msg=faces=1 best=0.9xxx` 为通过。

### 6-b. 测试现状（算法数值标定）

| 套件 | 位置 | 现状 |
|---|---|---|
| OFX 契约（mock host） | `03_测试夹具/mock_host.py` | **20/20 PASS** |
| 逐字节基线 | `04_验收工具/baseline.py check` | **18/18 一致** |
| 一键验收 | `04_验收工具/acceptance.py` | 见下 |

**跑夹具必须指定引擎目录与 Python**（PATH 里的 `python` 是 Microsoft Store 占位符）：

```bat
set NAMA_OFX_DLL=%CD%\..\01_ofx插件源码\out\DaVinciBeauty.ofx
set NAMA_ENGINE_DIR=%CD%\..\01_ofx插件源码\out
set PYTHONUTF8=1
py -3.12 mock_host.py
```

`out\` 下必须有 `onnxruntime.dll` + `yunet.onnx`（引擎按 `.ofx` 同目录找模型），否则 `region=off`、全部用例退化。

三条夹具侧的坑（都曾造成假 FAIL，均已修）：

1. **帧必须 bottom-up**（`_to_bottom_up()`）—— 与真实宿主同序，否则检测看到倒立的脸。
2. **判据要「与源帧比」，不要「两次渲染互比」**——引擎有跨帧状态（肤色中心 EMA、掩码缓存、细节层 EMA），两次渲染之间掩码可能不同，互比会量出「四角差 36 / 人脸区 107」这种离谱数字。
3. **合成素材（纯色/阶梯图）要用独立实例渲染**——复用主实例时，上一个用例留下的肤色 EMA 会让语义区域判「无皮肤 → 不处理」，整帧变暗（通道序用例曾因此整帧变黑）。

`acceptance.py` 里的「mock host 契约」用的是**已装机**的插件（`INSTALLED` 优先），所以装完新版才会转绿；装机件与 `out\` 字节不同属正常（PE 时间戳），不是缺陷。

### 6-c. 「磨皮算法」与「磨皮模式」的接线

这两个下拉框**曾经都是空接线**：值经 `beauty_engine_glue.cpp` 塞进 `BeautySettings`，但 `beauty_core.cpp::process()` 从不读取，三档/四档输出逐字节相同（当时 `baseline.py` 里 `mode1/mode2/mode3` 与 `blur62_wh30` 同一个 md5，就是最直接的证据）。

**磨皮算法（`blur_type`，面板顺序见 `kBlurTypeChoices`）**

| 档 | 实现 | 依据 |
|---|---|---|
| 0 双边滤波（默认） | 现状 `surface3Rgb` 保边路径，逐字节不变 | 基线 15 条 md5 全部不变，`alg0` == `blur62_wh30` |
| 1 高斯 | `bilateralRgbaRoi`，`sigmaColor` 放到 255 | 值域权重恒 1 → 双边退化成纯空间高斯，边缘一起糊（实测距源帧 +46%，眼镜框明显发虚） |
| 2 弱磨皮 | 同一套 `surface3Rgb`，`I *= 0.5` | 与档位曲线正交，只整体压低一档（实测距源帧 −42%） |

**磨皮模式（`smooth_mode`）** 控制「磨皮削掉的细节按什么策略还回去」：

| 档 | 实现 | 实测（距源帧） |
|---|---|---|
| 0 人像磨皮 | 不回填，逐字节不变 | 基准 1.031 |
| 1 清晰磨皮 | 基准回填 `fine ×2.2`、`mid 0.18·I` | +5.7%（回填过冲 → 更锐） |
| 2 精细磨皮 | `fine ×3.2`、`mid 0.40·I` | +9.5% |
| 3 轻磨皮 | 不回填，`I *= 0.55` | −37.8%（明显更接近原图） |

两个实现要点（改之前先读）：

1. **模式 1/2 必须自带回填**，不能复用 `wantBlurRefill` —— 那条路要求「锐化=0 且清晰=0」，而且只在 band 分支里求值。用户拖「磨皮模式」时锐化通常是 0，不另开一条的话这个下拉框依旧是死的。
2. **倍率必须远大于 1，且两档都带中频**。`blurRefill=0.06` 是给 band 路径标定的，单靠细频只回填 5% 的细频带，落在 8bit 上四舍五入后**一格都不动**（第一版 `mode1` 增益 1.0×，实测与默认档 md5 完全相同）。真正能推动像素的是中频。

⚠️ 同时把 `NamaSettings::blur_type` 的默认值从 `1` 改成 `0`：旧默认与面板默认（0）不一致，只是过去从不被读所以没暴露；一旦接线，自测路径会静默跑成「高斯」档，把 15 条基线全部打掉。

### 6-d. 两条链路必须分别验：自测钩子 ≠ 面板参数 ⚠️

把上面两个下拉框接好之后，**真机拖控件仍然不对**，又修了一版才完。原因是本项目有两条
互不覆盖的参数通路：

| 链路 | 入口 | 覆盖它的测试 | 验的是什么 |
|---|---|---|---|
| 自测 | `NamaSelfTest` + `NAMA_ST_*` 环境变量 | `baseline.py`（18 条逐字节） | 引擎**接没接**这个参数 |
| 面板 | `ofx_plugin.cpp::readParams()` → `coreBeautyProcess` | `mock_host.py`（`paramSetValue`） | 参数有没有**正确送到**引擎 |

`baseline.py` 直接调引擎，**绕过了 `readParams()`**，所以 `readParams` 里的错误它一律看不见。
当时就是在 `readParams` 里留着 SDK 时代的 `map[3] = {1,0,2}`（面板序 ↔ SDK 序互换），
把「双边滤波」和「高斯」对调了，而 18 条基线全绿。

规矩：**新增或改动任何面板参数，两边都要有用例**。面板侧一律用 `mock_host.py` 的
`paramSetValue`（和真人拖控件等价），并断言**方向**而不只是「有变化」—— 档位对调时
「有变化」依然成立，只有方向能抓住。参考 `mock_host.py` 的「下拉框接线」用例：
算法三档断言「高斯最糊、弱磨皮最轻」；模式四档断言 **9×9 中频保留率
`0 人像 < 1 清晰 < 2 精细`** 且 `3 轻磨皮` 距源帧明显更小（口径见 §6-e）。

另外注意 **mock host 默认驱动的是已装机版**（`NAMA_OFX_DLL` 不设时读 bundle），
改完代码没装机就跑去跑夹具，量到的是旧二进制的行为 —— 当时调试正是这样先被骗了一次
（以为是修复没生效，其实测的是旧二进制）。验证新产物要显式设 `NAMA_OFX_DLL` 指向 `out\`。

### 6-e. 磨皮「强度」和「档位差异」是两个旋钮 ⚠️

那次的触发是用户一句「磨皮算法三档 / 磨皮模式四档，无明显变化」。排查顺序很重要：

1. **先证伪接线**。真机 `ofx.log` 里 `blurType` 0/1/2 与 `smoothMode` 0–3 全都出现过
   （时间线 `alg=0 → alg=1 → mode=1`），说明参数确实送到引擎 —— 不是接线问题。
2. **再量化幅度**。真机帧 1080×1920、面板磨皮 100：人脸区与源帧平均绝对差 **0.955 灰阶**、
   峰值 13、改动 >3 仅 14.2%。这个量级在 1:1 下确实看不出来。
3. **然后判断天花板在哪**。surface3 的合成式是 `res = c + (even−c)·I`，满档 `I=0.976`、
   `e=0.95`，**两条曲线都顶死了** —— 想更强只能动合成式本身。

#### 强度总增益：为什么是「事后乘」而不是「让 I 突破 1」

数学上等价（`I>1` 就是 `(even−c)` 乘个大于 1 的系数），但**只有 surface3 这一条路有 I**：
高斯档靠外层 `blendWithMask` 的 `t`、legacy 盒式靠 `mixRgba` 的 `t`、25 tap 与降采样又各有
各的。所以新增 `beautycore::amplifySmooth(src, dst, w, h, gain)` 统一在「磨皮写完 dst 之后、
清晰/美白/细节回填之前」乘一次：

```
dst' = src + (dst − src) · gain
```

位置不能挪：放后面会把清晰和美白一起放大（那是另一个旋钮）；放前面 dst 还是原图、没有差值
可放大。`gain == 1` 时函数内部直接 return —— **逐字节等于改前那一版**，基线据此判回归。

#### 两条硬边界

- **混合类路径不许外推**。高斯/legacy 的强度就是 `tBlend`，`tBlend > 1` 的语义是「比完全
  平滑还平滑」，落到模糊图上等于反向补偿边缘 → 硬边 + 光晕。实测高斯档外推到 1.51 时人脸区
  最大改动 **87 灰阶**。所以这几条路只把增益乘进 `tBlend` 并**封顶 1.0**，`amplifySmooth`
  只给 surface3 的重建路径用。封顶后最大改动降到 59。
- **gain 有安全上限 ≈ 2.0–2.2**。`(dst−src)` 里混着**振幅门控结构**（`gw` 逐像素变化），
  放大过头会把它放大到压过磨皮本身。判据是「高频保留率 tex」：gain 1.0→0.942、
  1.6→0.950、2.0→0.957、**2.4→0.998**。tex 回到 1.0 就不是「磨得更狠」而是「平了但发毛」。
  取 **1.8**（res 1.82 / maxd 23 / tex 0.953）。现场用 `BEAUTY_SMOOTH_GAIN`（0.25–3.0）调。

#### 档位差异：为什么不能靠调 I

「清晰 / 精细」的语义是**把磨掉的高频按更大增益还回去**，走的是 `dp.fine` / `dp.mid` 那条
正交的腿。动 I 会把「磨皮」和「磨皮模式」两个旋钮绑死。所以那次只加大回填增益
（`fine 2.2→4.0 / 3.2→5.5`，`mid 0.18→0.45 / 0.40→0.75`）。

**这带来一个夹具断言的坑**：回填越多，画面越接近原图，「距源帧」反而**变小**。此前那版的
断言 `_mod[2] > _mod[0]*1.02`（精细应更远离源帧）在增益提到 5.5 之后必然失败 —— 而画面
完全正确。改量「9×9 中频高通保留率」`|G − box9(G)|`：人像 8.397 < 清晰 8.781 < 精细 9.069，
方向单调。**3×3 细频不够用**：回填主体是中频（`dp.mid`），细频只有 `0.06·I·4.0`，落在 8bit
上常常四舍五入为 0，四档之间只差 0.5%，断言拉不开。

> 教训与 §6-d 同源：**断言要对着语义写，不能对着「有没有变化」写**。而且当参数之间是
> 「越多越接近原图」这种反向关系时，单调性方向要跟着翻。

---

## 7. 版本历史

版本演进不写在这里。**用户能看出来的变化**看 `docs/CHANGELOG.md`；**当时的完整过程**（报错、逐帧数据、被推翻的结论）看 `docs/history/history-progress-1x.md`。
本节原先维护过一张按旧编号排列的对照表，2026-11-08 文档治理时移除 —— 它与 CHANGELOG 重复，且旧编号已退役，留着只会让人以为有两套版本序列。


版本号唯一出处：`nama_engine.h` 的 `NAMA_BUILD_MAJOR/MINOR/PATCH/STR`（面板显示、二进制内嵌、安装包文件名均派生自此）。

---

## 8. 已知坑 / 教训

1. **人脸检测模型 预处理必须 raw 0–255**（§2）—— 已证伪"归一化 / 通道序"假说，是本工程头号 footgun。
2. **OFX 帧是 bottom-up**（§2-c）—— 检测要反读行、框要映射回缓冲行号。`score≈0.25` 且画面有正脸，先怀疑行序。
3. **凡是「上/下」有语义的几何量，都必须显式声明行序**—— 检测修好行序之后，下游的 `warpFace()` 仍然按 top-down 用 `f.y`，于是下巴压到了发际线。这类 bug 的伪装性极强：径向各向同性的运算（大眼）完全不受影响，看上去像「只有某一项坏了」。凡是拿 `f.y` 推「下面/上面」的地方，一律传 `flipY` 并用 `sgn`，不要靠注释约定。
4. **排错第一步是 dump，不是猜**（§2-b）—— 把真正送进模型的 `in.f32` 落盘、交给 Python 用同一个 onnx 重跑，能一次性切开"输入错 / 推理错 / 解码错"。早期三版都在没有这一步的情况下猜错方向。
5. **离线自测本身也会骗人**（§2-b(1)）—— 夹具报 `PASS` 不代表引擎正常：`selftest_ofx.py` 的 `PASS` 只要求"画面有改动"，输出文件是上一次残留的旧文件也能过。判据要看 `msg=faces=N best=X`。
6. **夹具的行序/通道序必须与真机一致**（§2-c）—— mock host 以前按 top-down 造帧，等于替插件掩盖了方向 bug，一路 PASS。现在 `mock_host.py` 用 `_to_bottom_up()` 统一翻转，新加用例别漏。
7. `faces=0` 有两种完全不同的后果：**没拿到过掩码 → 逐字节透传**（看起来插件没装），**拿到过又丢了 → 全帧磨皮**。日志 `sdk=` 耗时是最快的判别依据。
8. 宿主若送 scene-linear（Color Managed 项目），`blitFrame` 的 `*255` 会把画面压暗；本项目按非线性编码像素处理，线性输入会漏检（日志会 WARN）。**注意该 WARN 只看中位灰阶（<55 就报警），暗调素材会误报**，最终以 `faces/score` 为准。
9. **判据指标选错会得出反向结论**（踩过一次）—— 用全帧「5 点高通」量磨皮强度时，结果是非单调的（磨皮 62 反而比 0 高），因为背景把统计稀释了。改用「按满档改动脚印自动圈出人脸区，再量该区内与原图的平均距离」，才得到单调的 0.315 / 0.317 / 1.028 / 2.176。**先验证指标本身单调，再拿它调参。**
10. **「不明显」先量幅度，再怀疑接线**—— 用户报「磨皮算法/磨皮模式无明显变化」时，日志已经证明参数送达，真正的数字是「满档只改 0.955 灰阶」。接线 bug 与幅度不足的表现一模一样，但修法完全不同；先量化再动手，能少改一版。
11. **放大磨皮必须分清「外推」和「加宽核」**—— `dst = src + (dst−src)·gain` 在 gain>2 之后放大的是振幅门控结构而不是磨皮本身（高频保留率 tex 从 0.957 反弹到 0.998，观感「平了但发毛」）；而混合类路径（高斯/legacy）外推到 tBlend>1 等于反向补偿边缘，直接出光晕（最大改动 87 灰阶）。安全做法：surface3 用 gain ≤1.8，混合类只乘进权重并封顶 1.0。
12. **「回填越多越接近原图」——反向单调**—— 「清晰/精细」加大回填后，距源帧反而变小。断言这类档位不能沿用「离原图更远」的口径，要换成「中频保留率递增」，且窗口用 9×9 而不是 3×3（回填主体是中频）。
7. install.bat 的 cmd 解析期雷区（`..` / `...` / 中文 BOM），现已规避但勿再引入。
8. 任何构建改动实跑 `build.bat` 验证（`< /dev/null`）；沙箱可跑，别因一次"禁调 cmd"报错就放弃。
9. `BEAUTY_DEBUG_DUMP` 的目录**必须是纯 ASCII 路径**（引擎内部用 `fopen`，非 ASCII 会静默写不出来）。
