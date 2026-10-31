# NamaBeauty 代码审查报告

审查对象：`NamaBeauty_源码备份_20261014_2236`
审查时间：2026-10-14
审查范围：67 个文件（OFX C++ 插件 3 个核心文件、构建/安装脚本、官方 OFX 头文件、Python 脚本版、Lua 脚本版、无宿主测试夹具）
证据来源：源码 + 官方 `ofxImageEffect.h` 规范 + 真实运行日志 `C:\ProgramData\DaVinciBeauty\logs\ofx.log`

---

## 零、结论先行

整体质量**明显高于一般个人插件项目**：错误处理（SEH 兜底）、资源生命周期（GL 上下文还原、线程局部缓冲刻意不析构）、性能意识（AVX2、零堆分配、参数缓存）都做得扎实，且大部分设计决策在源码注释里都有可追溯的出处。

本次审查发现 **3 个真 BUG**（会导致错误行为，非风格问题）+ **5 个健壮性/合规缺陷** + **4 个工程卫生问题**。

其中**最严重的一个**是真实的：日志显示 Resolve 会以 `scale=0.15`（161×80）的代理分辨率调用我们的 render，而插件对人脸检测的可行性没有任何判断，白白付出 17.7 ms 却拿到 `faces=0`。而**测试夹具结构性地测不出这个问题**（它永远只发 `scale=[1.0,1.0]`）。

下面的 BUG 按严重程度降序排列。

---

## 一、BUG 清单

### 🔴 B1 [严重] 代理/降分辨率渲染未适配 —— 每帧白付 SDK 耗时

**证据**（真实 Resolve 会话，`ofx.log` 第 143、151 行）：

```
22:34:31.900 [T32596] render #3 time=2884.000 161x80 scale=0.15 in=float out=float rowBytes=2576/2576 faces=0 取图=0.0ms 转in=0.0ms sdk=17.7ms 转out=0.0ms 合计=17.8ms
22:34:33.384 [T32596] render #7 time=0.000 504x284 scale=0.47 in=float out=float rowBytes=8064/8064 faces=0 取图=0.0ms 转in=0.3ms sdk=13.7ms 转out=0.2ms 合计=14.1ms
```

对比同会话的全尺寸帧：`1080x1920 scale=1.00 ... sdk=13.3ms faces=1`。

**问题**：161×80 的帧只有 12880 个像素，是全尺寸（2073600）的 0.6%，但 SDK 仍要花 **17.7 ms**（比全尺寸还慢）。原因有二：

1. **人脸检测在小图上必然失败**（`faces=0`），但 SDK 仍会跑完整的检测流程 —— 付出了代价拿不到结果。
2. 这些帧多半来自**缩略图/代理预览/调色台小监看**，用户根本看不出美颜效果，属于纯浪费。

**源码位置**：`ofx_plugin.cpp` 第 554–557 行。

```cpp
// 渲染缩放：宿主在代理/半分辨率播放时会小于 1，仅用于诊断
double scale[2] = {1.0, 1.0};
if (inArgs)
    gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRenderScale, 2, scale);
```

注释自己写明了「宿主在代理/半分辨率播放时会小于 1」，但**只用于日志诊断，没有做任何适配**。

**同类未被利用的信号**：规范还为缩略图渲染提供了专门的 `kOfxImageEffectPropThumbnailRender`（`ofxImageEffect.h` 第 1415–1426 行），文档明确建议插件可以「turning itself off in `kOfxImageEffectActionIsIdentity`」。插件完全没有读这个属性。

**正确做法**（本报告 B1 修复已实现）：
- 在 `isIdentity` 里读 `kOfxImageEffectPropThumbnailRender` / `RenderScale`，缩略图或极小缩放时直接返回 `OK` 要求透传，宿主连 render 都不会调；
- 在 `renderMain` 里对缩放低于阈值的帧走快速透传路径，跳过 SDK。

---

### 🔴 B2 [严重] `SupportsMultipleClipDepths=0` 与「声明支持 Byte+Float 两种深度」自相矛盾

**证据**：`ofx_plugin.cpp` 第 314–320 行

```cpp
gPropHost->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
gPropHost->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat);
...
gPropHost->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
```

**规范原文**（`ofxImageEffect.h` 第 803–816 行）：

> If a host indicates that it can support multiple pixels depths, then it will allow the plugin to explicitly set **the output clip's pixel depth in the `kOfxImageEffectActionGetClipPreferences` action**.
> - `0` - in which case the host or plugin **does not support clips of multiple pixel depths**
> - `1` - which means a host or plugin is able to deal with clips of multiple pixel depths

**矛盾点**：插件声明了「我能吃两种深度」，却又声明「我不支持多深度 clip」。这两条在语义上是打架的 —— 「支持多深度」的准确含义是「输入与输出 clip 可以各自是不同深度」，而插件确实是这么工作的（日志实测 `in=float out=float`、`in=byte out=byte`，且 `blitFrame` 完整支持 float↔byte 互相转换）。

**后果**：把这条路堵死后，插件**永远无法在 `GetClipPreferences` 里指定输出深度** —— 这正是 PROGRESS.md 里自认「没做」的那件事的直接技术原因。留 `=0` 等于主动放弃了一次「byte 输入可以省掉来回两趟格式转换」的优化机会。

**另外注意一个官方头文件的坑**：宏名 `kOfxImageEffectPropSupportsMultipleClipDepths` 对应的实际字符串是 `"OfxImageEffectPropMultipleClipDepths"`（少了 `Supports`，第 816 行），这是 OFX 官方历史遗留的命名不一致，不是本项目的错 —— 但值得在代码里留一行注释说明，免得后人「顺手改成对的」反而改错。

**正确做法**：把 `SupportsMultipleClipDepths` 改为 `1`，并实现 `GetClipPreferences` 让输出深度跟随输入（见 B3）。

---

### 🔴 B3 [严重] 未实现 `kOfxImageEffectActionGetClipPreferences`

**证据**：`pluginMain` 的动作分发（`ofx_plugin.cpp` 第 765–787 行）里没有这个 action，落到最后的 `return kOfxStatReplyDefault`。

PROGRESS.md 第 112–113 行自认：

> **未实现的动作**：`kOfxImageEffectActionGetClipPreferences`（让输出深度跟随输入，可再省一次转换）。因为没有真实宿主可验证，宁可不做也别做错。

**这不是「不做也不错」，而是「不做就错」**：规范（`ofxImageEffect.h` 第 315–325 行）明确要求该 action 应在**实例创建后调用一次、每次输入 clip 变化时、以及被 `ClipPreferencesSlaveParam` 指名的参数变化时**被宿主调用。不实现它，宿主只能按自己的默认策略决定输出深度 —— 而 Resolve 的默认策略可能与输入不一致，导致**多一次无谓的 float↔byte 往返**（日志实测单程 3.5 ms，来回就是 7 ms）。

**规范给出的 outArgs 属性清单**（第 329–360 行）包括：`OfxImageClipPropComponents_<clip>`、`OfxImageClipPropDepth_<clip>`、`OfxImageClipPropPAR_<clip>`、`kOfxImageEffectPropFrameRate`、`kOfxImageClipPropFieldOrder`、`kOfxImageEffectPropPreMultiplication`、`kOfxImageClipPropContinuousSamples`、`kOfxImageEffectFrameVarying`。

**⚠️ 实现时必须注意一个隐蔽的规范陷阱**：文档对 `OfxImageClipPropDepth_` 的描述是「a set of char* X 1 properties, one for each of the **input clips currently attached and the output clip**」—— 也就是说属性名要拼成 `OfxImageClipPropDepth_Source` / `OfxImageClipPropDepth_Output`。而 `ofxImageEffect.h` 第 381–386 行明确列出这些是「special props all have the clip name postpended after "_"」。

**但头文件里没有 `#define` 宏**（只有注释描述），所以必须手写字符串拼接 —— 这是本报告唯一需要手写 magic string 的地方，必须在注释里标明出处。

**⚠️ 第二个陷阱**：`kOfxImageEffectFrameVarying` 必须设为 `true`。我们的引擎内部维护 `frameId_++`，且人脸跟踪带时序状态 —— 输出的第 N 帧依赖于之前处理过的帧。若声明为 `false`，宿主可能复用缓存的输出而跳过 render。

**正确做法**（本报告已实现）：实现该 action，让输出深度/组件/PAR 跟随输入，并置 `FrameVarying=true`。

---

### 🟠 B4 [中] `createInstance` 预热在 4.6–5.4 秒区间，且**不在实例锁内**

**证据**（`ofx.log` 多次会话）：

```
21:58:00.206 createInstance 预热引擎 成功 耗时=5360ms
22:05:54.545 createInstance 预热引擎 成功 耗时=1141ms
22:25:59.612 createInstance 预热引擎 成功 耗时=4656ms
22:27:20.033 createInstance 预热引擎 成功 耗时=4797ms
22:34:31.545 createInstance 预热引擎 成功 耗时=1266ms
```

**问题一：耗时波动 1.1s～5.4s（近 5 倍）**。README / PROGRESS 都按「约 2.5 秒」描述，实测最坏 5.4 秒。这不是 bug，但**用户感知的「拖上去卡住 5 秒」和文档承诺的 2.5 秒不符**，应修正文档或加入进度提示。

**问题二（真问题）**：`createInstance` 里这段逻辑**没有持有 `NamaEngine` 的互斥锁**：

```cpp
NamaEngine& engine = NamaEngine::inst();
if (!engine.ready() && !engine.poisoned()) {
    bool ok = engine.ensureReady(err);   // ensureReady 内部才加锁
```

`ready()` / `poisoned()` 是无锁读取，`ensureReady` 才加锁。多实例并发创建时（第 46–47 行日志可见 `T09536` 一次性连开两个实例），两个线程可能同时通过 `!ready()` 判断，然后一个在 `ensureReady` 里阻塞等锁 —— 语义上没错（`ensureReady` 有 `if (ready_) return true;` 复检），但**外面这次重复的 `engine.ready()` 读取在逻辑上是 TOCTOU**。更重要的是：`describeInContext` 之后 Resolve 可能**新建实例但不预热**（复用已就绪引擎），这时如果引擎因前述原因失败，用户完全看不到报错。

**修正建议**：把「是否需要预热」的判断挪进 `ensureReady`，或外层用同样的锁包住判断+调用；并把预热失败通过 `gMsgHost->message` 报给用户（目前只在日志里记一行）。

---

### 🟠 B5 [中] 引擎单例 vs. 插件声明多实例 —— 参数串扰已被规避但**未根治**

**证据**（`ofx_plugin.cpp` 第 305 行）：

```cpp
gPropHost->propSetString(props, kOfxImageEffectPluginPropSingleInstance, 0, "false");
```

而 `NamaEngine::inst()` 是进程内唯一单例，内部只有**一个** `item_` 和**一份** `applied_` 参数缓存。

**当前靠什么兜住**：同文件第 322–333 行声明了 `kOfxImageEffectPluginRenderThreadSafety = Unsafe`，注释解释得很清楚：

> 我们的引擎是**进程内唯一**的：一个 SDK 道具（item_）+ 一份参数缓存（applied_）。两个实例同时渲染时，A 把参数写进 item_、B 又写一遍，随后 A 会拿着 B 的参数出图。

**这个缓解是有效的** —— `Unsafe` 让宿主串行化所有实例的 render，`process()` 内又有 `CRITICAL_SECTION`。**但仍有残留窗口**：

`NamaEngine::process()` 的临界区覆盖了「下发参数 → render」，这是对的。然而**参数缓存 `applied_` 是全局单例的一份**，不是 per-instance 的。两个实例 A（磨皮 30）和 B（磨皮 90）交替渲染时：

1. A render → `changed=true` → 下发 30 → `applied_=A`
2. B render → `changed=true` → 下发 90 → `applied_=B`
3. A render → `changed=true`（`applied_` 是 B）→ 重新下发 30

**结果正确，但每帧都要重新下发 10 次 setter**，因为 `applied_` 在实例间被反复冲掉。这就是注释里说的「性能上零损失」其实**不完全成立**的地方 —— 单实例时 ziran2 缓存有效，多实例交替时缓存 100% 失效。

**根治方案**：把 `item_` 和 `applied_` 做成 per-instance（实例数据里存一份），或者干脆老实声明 `SingleInstance = "true"`。

---

### 🟠 B6 [中] 参数页 `kOfxParamPropPageChild` 在参数定义**之后**才追加 —— 顺序可能不生效

**证据**（`ofx_plugin.cpp` 第 421–427 行）：

```cpp
// ---- 参数页：决定 Inspector 里的排列顺序 ----
if (gParamHost->paramDefine(paramSet, kOfxParamTypePage, "Main", &pp) == kOfxStatOK) {
    for (int i = 0; i < P_COUNT; i++)
        gPropHost->propSetString(pp, kOfxParamPropPageChild, i, kParams[i].name);
    ...
```

**问题**：参数页在**所有参数定义完之后**才被 define。`kOfxParamPropPageChild` 的语义是「这些参数归入该页」，但规范没有保证「后置归类」一定生效 —— 部分宿主（含 Resolve）要求参数页在**被归类的参数之前**定义，或者在参数定义时就用 `kOfxParamPropParent` 指定父级。

**建议**：把 `Main` 页的 `paramDefine` 挪到**第一个效果滑块之前**，并改用 `kOfxParamPropParent` 显式指定每个参数的父级（更可靠，且不依赖顺序）。

**风险说明**：这一条**需要在真实 Resolve 界面上验证**才能确认是否真的不生效 —— 从日志无法判断（`describeInContext` 只记了 `params=10(+page)`）。所以列为「建议修改 + 需实测确认」，而不是「确定的 bug」。

---

### 🟡 B7 [低] `f32_to_u8_row` 的 AVX2 路径对 NaN 的处理与注释声称的**不一致**

**证据**（`ofx_plugin.cpp` 第 184–212 行）：

```cpp
// maxps/minps 对 NaN 的行为是「返回第二操作数」，所以 NaN 会被夹成 0，
// 与标量分支的结果一致。
...
v = _mm256_max_ps(v, lo);   // lo = 0.0
v = _mm256_min_ps(v, hi);   // hi = 255.0
```

**注释说**：NaN 会被夹成 0。
**实际**：`_mm256_max_ps(v, lo)` 中 v=NaN 时，x86 的 `MAXPS` 语义是「若任一操作数为 NaN，返回**第二个**操作数」，所以返回 `lo = 0.0` ✅。但接着 `_mm256_min_ps(0.0, 255.0)` = 0.0 ✅。**AVX2 路径结论正确**。

而**标量分支**：

```cpp
float v = src[i] * 255.0f + 0.5f;
int iv = (int)v;                     // NaN → (int)NaN 是 UB，MSVC 下通常得 INT_MIN
dst[i] = (unsigned char)(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
```

`INT_MIN < 0` 成立 → 结果是 `0`。**两条路径恰好都得到 0**，所以注释的结论对，但**推理过程有瑕疵**（标量分支依赖「MSVC 把 NaN 转 int 得 INT_MIN」这一未定义行为）。GCC/Clang 或未来的 MSVC 优化可能给出不同结果。

**建议**：标量分支改用 `v != v` 显式判 NaN，或直接用 `_mm256` 的同款语义写：
```cpp
if (!(v >= 0.0f)) v = 0.0f;      // 同时挡住 NaN 与负数
else if (v > 255.0f) v = 255.0f;
```

**另外**：`_mm256_cvttps_epi32` 是截断取整。`v = 254.9999` 加 0.5 后是 255.4999 → 截断得 255 ✅。但 `v = 255.0` 加 0.5 → 255.5 → 已被 `minps` 夹到 255.0 → 加 0.5 在夹**之前**做完了 ✅（顺序是 mul → add → max → min → cvt，正确）。**这条没问题**。

---

### 🟡 B8 [低] 全 0 透传路径**不检查深度是否一致**，`blitFrame` 有个隐含假设

**证据**（`ofx_plugin.cpp` 第 229–260 行）：`blitFrame` 中 `sf == df` 分支：

```cpp
if (sf == df) {
    if (srcRowBytes == rowBytesNeed && dstRowBytes == dstRowBytesNeed) { memcpy(...); return; }
    for (int y = 0; y < h; y++)
        memcpy((unsigned char*)dstPtr + y*dstRowBytes,
               (const unsigned char*)srcPtr + y*srcRowBytes,
               rowBytesNeed);   // ← 永远拷 rowBytesNeed（= 源格式的宽度）
```

**问题**：`sf == df` 时 `rowBytesNeed == dstRowBytesNeed`，所以拷贝宽度是对的 ✅。**这条实际上没问题**，但代码可以写得更直白（用 `dstRowBytesNeed` 表达意图）。

**真正值得警惕的是调用点**（第 640 行）：

```cpp
blitFrame(srcDepth, srcPtr, srcRowBytes, dstDepth, dstPtr, dstRowBytes, w, h);
```

这里 `w` 取的是 `min(sw, dw)` 的**像素数**，而 `blitFrame` 内部按 `sf` 决定每像素字节数。如果宿主给出「源是 float、目标也是 float，但两者宽度不同」（B1 场景下缩略图渲染就可能出现），`w` 取较小值会导致**只写了目标图的一部分，剩余部分保留未初始化内存** —— 表现为**画面边缘出现花屏/残影**。

**建议**：当 `sw != dw || sh != dh` 时，目前只打了一条 WARN 日志，但**继续用 min 尺寸处理**。应改为：要么按目标尺寸完整填充（超出源的部分补 0 或复制边缘），要么直接把这种帧当异常拒绝渲染。至少日志级别应提升，并在 README 里记录这个已知风险。

---

### 🟡 B9 [低] 引擎失败只报一次，且 `warned` 是**函数级 static**（跨实例共享）

**证据**（`ofx_plugin.cpp` 第 675–689 行）：

```cpp
static bool warned = false;
if (!warned) {
    warned = true;
    ...
    gMsgHost->message(instance, kOfxMessageError, "", "%s", ...);
}
```

**问题**：`warned` 是静态局部变量，**跨所有实例共享且永不重置**。第一个实例触发警告后，后续所有实例（哪怕在不同片段上）都不会再报错。用户如果先在一个片段上遇到失败、之后在新片段上又失败，界面上不会有任何提示。

**建议**：把 `warned` 挪进 `InstanceData`（per-instance），或改成「每 N 次失败报一次」的计数策略。

---

## 二、工程卫生问题

### 🧹 G1 `t.h` 是误入库文件（已删除）

`01_ofx插件源码/t.h` 原始内容是一份 **GitClone.com 的 404 网页 HTML**（含完整 `<html><head><style>` 与 JS），显然是把浏览器保存的网页误存成了 `.h`。文件本身 9459 字节，与 C++ 头文件毫无关系。

**已处理**：先确认**全项目没有任何文件 `#include "t.h"`**，然后直接删除。`build.bat` 也不引用它，删除后干净重编译零警告，证明无影响。

### 🧹 G2 `examples/` 里放了三份官方示例（非本项目产物）

`examples/basic.cpp`(37KB)、`examples/invert.cpp`(8.9KB)、`examples/ofxUtilities.H`(9.7KB) 是 OpenFX SDK 自带的官方示例，**不参与编译**（`build.bat` 只编 `src/` 下两个文件）。

**建议**：要么删掉（仓库里已有 `sdk/include/`，示例只是参考），要么移进 `sdk/examples/` 并保留官方 LICENSE 说明，避免与自有代码混淆、也避免许可证问题。

### 🧹 G3 `selftest_ofx.py` 里硬编码了一条**已失效的绝对路径**

```python
ROOT = r"C:\Users\Administrator\WorkBuddy\2026-10-14-13-17-08\beauty_probe"
```

这条路径指向一个**旧工作区**（`WorkBuddy` 而非 `WorkBuddy AI`，且带时间戳目录），当前环境下必然不存在，脚本一跑就 `FileNotFoundError`。

**建议**：改成 `os.path.dirname(os.path.abspath(__file__))`。

### 🧹 G4 版本号散落在 4 个地方，靠手工同步

- `ofx_plugin.cpp`：`ofxLog("...build=1.3")`（出现在 `describeMain` 与 `onLoad` 两处，共 2 个字符串）
- `nama_engine.cpp`：`NamaVersion()` 返回 `(1<<16)|3`
- `nama_engine.cpp`：`NamaSelfTest` 的消息里不带版本
- 日志里还能看到历史 build 1.2 的记录

**建议**：抽一个 `#define NAMA_BUILD "1.3"` 放在共同头文件里，日志与 `NamaVersion()` 都从它派生，避免下次改版本漏掉一处。

---

## 三、已确认**良好**的设计（不要改回去）

以下几点经交叉验证是正确的，且注释里给了出处，值得保留：

| 项 | 结论 | 依据 |
|---|---|---|
| `RenderThreadSafety = Unsafe` | ✅ 正确 | `ofxImageEffect.h` 第 769–790 行三档语义；引擎单例确实不能并行 |
| 全 0 短路 + `IsIdentity` 返回 `kOfxStatOK` 并设 `kOfxPropName`/`kOfxPropTime` | ✅ 完全符合规范 | 第 389–448 行 |
| `time` 原样透传给 `clipGetImage` | ✅ 修复正确 | 第 549–551 行引用规范原文 |
| src/dst bounds 分别取交集 | ✅ 正确 | 修复了「用 srcRect 写 dstPtr」的老 bug |
| `__declspec(thread)` 存裸指针而非 `thread_local std::vector` | ✅ 正确且必要 | 宿主 `FreeLibrary` 后线程退出会调用悬空析构函数指针 |
| SEH 包装函数体内无析构对象 | ✅ 正确 | MSVC C2712 约束 |
| 参数未变跳过 setter | ✅ 正确（见 B5 的多实例例外） | — |
| 4MB 日志上限、抽样记录 | ✅ 合理 | — |
| `install.bat` 逐文件替换、锁文件改名 | ✅ 正确 | 避免 rmdir 半空安装导致静默透传 |

---

## 四、整改优先级建议

| 优先级 | 项 | 动作 | 风险 | 需实测确认 |
|---|---|---|---|---|
| P0 | B1 代理渲染适配 | 已修复（见下） | 低 | 建议 |
| P0 | B2 `SupportsMultipleClipDepths` | 已修复 | 低 | 是 |
| P0 | B3 `GetClipPreferences` | 已实现 | **中**（规范陷阱多） | **是（必须）** |
| P1 | B4 预热锁 + 上报 | 已修复（部分） | 低 | 否 |
| P1 | B5 参数串扰 | 文档化 + 保持 Unsafe | — | 多实例时验 |
| P2 | B6 参数页顺序 | 改用 `kOfxParamPropParent` | 低 | **是** |
| P2 | B7 NaN 标量分支 | 已修复 | 低 | 否 |
| P2 | B8 bounds 不一致 | 提升日志 + 文档化 | 低 | 是 |
| P3 | B9 `warned` 移入 InstanceData | 已修复 | 低 | 否 |
| P3 | G1–G4 工程卫生 | 见下 | 低 | 否 |

---

## 五、关于「测试夹具结构性盲区」的提醒

`mock_host.py` 是一份很扎实的夹具（协议回归 + 全 0 逐字节透传 + IsIdentity + 连续帧时间），但它有一个**结构性盲区**：

```python
# mock_host.py 第 661、715 行
inargs.set("OfxImageEffectPropRenderScale", 0, [1.0, 1.0])
```

**永远只发 `[1.0, 1.0]`**，也**从不设置** `kOfxImageEffectPropThumbnailRender`、不模拟缩小渲染窗口、不模拟 src/dst bounds 不一致、不调用 `GetClipPreferences`。

这意味着 B1、B3、B6、B8 这四项**在夹具里永远测不出来** —— 它们全是「真实宿主行为差异」类问题。建议给夹具补三个用例：

1. `scale=[0.5, 0.5]` + 缩小 render window → 断言插件走了快速透传；
2. `ThumbnailRender=true` → 断言 IsIdentity 返回 OK；
3. src/dst bounds 故意不一致 → 断言不越界写。

---

## 六、本次已执行的代码修改

### 改动文件

| 文件 | 修改内容 |
|---|---|
| `01_ofx插件源码/t.h` | **已删除**（误入库的网页 HTML，全项目无引用） |
| `01_ofx插件源码/src/nama_engine.h` | 新增 `NAMA_BUILD_MAJOR/MINOR/STR` 统一版本号（G4） |
| `01_ofx插件源码/src/nama_engine.cpp` | `NamaVersion()` 改用统一版本宏（G4） |
| `01_ofx插件源码/src/ofx_plugin.cpp` | B1 / B2 / B3 / B4 / B6 / B7 / B9 修复 |
| `02_测试夹具beauty_probe/mock_host.py` | 补代理/缩略图/ClipPrefs 三个用例；修硬编码路径；clip 上补设深度 |
| `02_测试夹具beauty_probe/probe_in.raw` | 从旧工作区复制进来，让夹具自包含 |

原始文件已备份到 `01_ofx插件源码/src/_backup_before_fix/`。

### 验证结果

**编译**：`build.bat` 干净重编译，`/W3` 下**零警告零错误**，导出符号齐全
（`DaVinciBeauty.ofx` 64512 字节，`NamaSelfTest` / `OfxGetNumberOfPlugins` / `OfxGetPlugin`）。

**协议回归**：8 项全 PASS（新增 3 项，原 5 项无回归）。

```
8bit 渲染  : PASS      全0 透传   : PASS      代理透传   : PASS
连续 3 帧  : PASS      IsIdentity : PASS      缩略图恒等 : PASS
float 渲染 : PASS      ClipPrefs  : PASS      总判定     : PASS
```

**修复效果实测**（`scale=0.15` 的 80×49 代理帧，夹具实测）：

| 指标 | 修复前 | 修复后 |
|---|---|---|
| 单帧耗时 | 15.9 ms | **0.6 ms** |
| 与源逐字节差 | 134 | **0** |

**`GetClipPreferences` 实测应答**：

```
GetClipPreferences -> 0  Depth_Output=OfxBitDepthByte FrameVarying=1
```

输出深度正确跟随输入（byte），`FrameVarying=true` 已设置。

**安装**：`install.bat` 12 个文件全部替换成功，逐项校验通过。

### ⚠️ 两点必须知道的事

**1. B3 仍需在真实 Resolve 里确认。** 夹具只能证明「动作被应答、属性被设置」，证明不了
「Resolve 真的按我们说的做了」。`GetClipPreferences` 涉及两个规范陷阱（属性名手写拼接、
`FrameVarying` 必须为 true），**在真实 Resolve 里验证通过之前不要删 backout 路径**：

- 恢复 B6 旧行为：把 `Main` 页的 `paramDefine` 挪回参数之后，删掉 3 处 `kOfxParamPropParent`；
- 恢复 B2：`SupportsMultipleClipDepths` 改回 `0`；
- 关掉 B3：注释掉 `pluginMain` 里分发 `GetClipPreferences` 的那两行。

**2. 安装目录里有 3 个 `.locked-*` 残留文件删不掉。** 因为 **DaVinci Resolve 当前正在运行**
（PID 22236），它映射着旧的 `CNamaSDK.dll` / `fuai.dll` / `DaVinciBeauty.ofx`。
这正是 `install.bat` 改名策略的预期行为 —— **关掉 Resolve 之后手动删掉它们即可**：

```
C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\*.locked-*
```

（同理，B1/B3 的改动要生效，**必须重启 Resolve** —— 它按 `mtime + size` 缓存插件扫描结果。）

