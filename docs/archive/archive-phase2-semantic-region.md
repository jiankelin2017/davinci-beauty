# Phase 2 报告：语义区域接入（v1.5）

> **已废弃（2026-11-08）**。同上：探针对象为已移除的闭源 SDK。
> 目标：把 v1.4 的「颜色启发式判皮肤」换成 **fuai 模型级语义分割**，让磨皮和美白
> 真正只在皮肤上生效。
> 结论：**已完成并验收通过**。非皮肤区被改动的比例从 **40%~77.5%** 降到
> **0.00%~0.19%**，同时掩码内美白仍是满档（+24.35 / 理论上限 25.5）。

---

## 一、结论摘要

| # | 结论 | 证据 |
|---|---|---|
| 1 | 语义区域已接通，`fuai.dll` 可与 CNamaSDK 同进程共用 | 5 张真实素材全部 PASS（`probe_phase2_region_gate.py`） |
| 2 | **非皮肤区改动从 40~77.5% 降到 0.00~0.19%** | 见第四节表格 |
| 3 | 掩码内美白满档，未被削弱 | 掩码内平均差 **24.35**，理论上限 25.5 |
| 4 | 磨皮也一并被关进皮肤内（v1.4 做不到） | `blur=60` 掩码外改动 >=3 占比 **0.00%** |
| 5 | 掩码取法（RGBA/fmt=3 vs RGB/fmt=1）结果一致 | 最大差 **0.0075**，覆盖度同为 27.0% |
| 6 | 不需要 29 MB 的 fuai 副本，共用同一模块即可 | 见第三节第 2 条 |
| 7 | 拿不到掩码时优雅降级到 v1.4 行为 | `NAMA_REGION_OFF=1` 时 mock_host 仍 19/19 PASS |
| 8 | 回归无损 | mock_host **19/19 PASS**（区域开、区域关各一遍） |
| 9 | **修掉一个「整进程静默消失」的致命 bug** | `int extra[4]` 栈越界 → `0xC0000409`，见第三节第 1 条 |
| 10 | 修掉一个「无皮肤画面整帧被磨」的语义漏洞 | A7300560：掩码全 0 时原来退回全帧磨皮（53.94%），现在 0.00% |

---

## 二、改了哪些东西

### 2.1 新增

| 文件 | 作用 |
|---|---|
| `01_ofx插件源码/src/nama_region.h` | `NamaRegion` 接口：加载 fuai、跑推理、给出 192×256 皮肤概率图 |
| `01_ofx插件源码/src/nama_region.cpp` | fuai 驱动实现（14 个导出、SEH 包装、bundle 查找、降频） |
| `03_测试夹具/probe_region_contract.py` | 隔离验证 fuai 调用契约（不含 CNamaSDK），4 种参数组合 |
| `03_测试夹具/probe_phase2_region_gate.py` | **验收脚本**：三种使用姿势下量掩码内/外的改动 |
| `03_测试夹具/make_probe_frame.py` | 生成 `probe_in.raw`（见 2.3） |

### 2.2 修改

| 文件 | 改动 |
|---|---|
| `nama_engine.cpp` | 新增 `buildMaskMap()`（双线性下标/权重表）、`applyRegionEffects()`（一趟同时做「按掩码收回 SDK 效果」+「按掩码美白」）、`ensureRegion()` 与 6 个 `region*()` 访问器；`process()` 接入区域路径并保留回退；`release()` 清区域状态 |
| `nama_engine.h` | 区域状态成员与访问器声明 |
| `build.bat` | 编译列表加入 `nama_region.cpp` |

### 2.3 夹具变化（重要）

`mock_host.py` 原来在找不到 `probe_in.raw` 时会现场合成一张纹理帧。**v1.5 起这条退路失效**：
合成帧里没有皮肤，语义模型判定「整帧无皮肤」，插件按契约什么都不做，于是
8bit / float / B5 全部 FAIL —— 那不是回归，是夹具不再有代表性。

现在必须先生成测试帧：

```
python make_probe_frame.py <一张有真人脸的照片>
```

`_load_or_make_frame` 已加显式警告，避免下次有人踩同一个坑。

---

## 三、两个必须记住的坑

### 3.1 `int extra[4]` → 整进程静默消失（0xC0000409）

**现象**：接上区域后，宿主里第一次 render 就整进程没了。退出码 `0xC0000409`，
**没有 WER 事件、没有 Application Error、连 VEH 都收不到异常**。SEH 完全兜不住。

**定位过程**（记下来，下次能省两小时）：

1. 加逐调用追踪（`NAMA_REGION_TRACE=1`），发现死在
   `FUAI_FaceProcessorGetSkinSegResult`，它前面的 `Process2Result` / `GetNum` 都正常。
2. 在**不含 CNamaSDK** 的独立进程里跑同一段调用序列（`probe_region_contract.py`），
   4 种参数组合全部正常 → 排除「契约写错」。
3. 怀疑共享状态：`CNamaSDK.dll` 的 PE 导入表里确实有 `fuai.dll`。于是复制一份改名
   `fuai_region.dll` 做模块隔离 —— **崩得一模一样** → 排除「共享 fuai 全局状态」。
4. 怀疑 GL 上下文（fuai 日志里有 `initialGLExtentions`），把推理挪到上下文仍 current 的
   位置 —— **还是崩** → 排除 GL。
5. 用 PowerShell 取原始退出码，拿到 `0xC0000409`（STATUS_STACK_BUFFER_OVERRUN）。
   这类终止走 `__fastfail`，**绕过 VEH 也绕过 SEH**，正好解释「什么都没打出来」。
6. 对照 Python 侧：`extract_face_assets.py` 里是 `extra = (ctypes.c_int * 4)()` ——
   **堆**对象；我们是栈数组。fuai 实际往里写 **6 个 int**（用哨兵量出来的，
   640×394 与 1920×1080 都是 6，与分辨率无关），栈上越界 8 字节正好踩坏 /GS cookie。

**修复**：`int extra[256]`。修完共享 `fuai.dll` 完全正常，所以第 3 步那份 29 MB
副本也一并删掉了。

**教训**：Python 里「能跑」不代表契约对 —— 堆越界是静默的，栈越界是致命的。

### 3.2 掩码全 0 ≠ 掩码不可信

最初写了个「覆盖度 < 0.1% 就当没有掩码」的兜底，想法是「避免磨皮突然失效」。
实测素材 A7300560（无脸、无人像）时它把整帧磨了 **53.94%** —— 因为退回的
v1.4 老路用的是 SDK 自己的颜色掩码，而那个掩码认为 54% 的像素是皮肤。

**正确语义**：掩码全 0 是模型在明确回答「这一帧没有皮肤」，此时**什么都不做**
才是对的。现在拿全 0 掩码走一遍 `applyRegionEffects`：`m=0` 的像素逐字节还原成
输入，结果就是整帧不动。

只保留一个例外：**SDK 检出了人脸、fuai 却说没皮肤** —— 两边判断冲突，更像 fuai
偶发失败，此时退回颜色启发式，至少不会让人脸突然没效果。

---

## 四、验收数据

素材 640×394，`skin_detect=1`，掩码由 fuai 现算（RGBA 取法，与插件一致）。
「掩码外」= 概率 < 0.1，「掩码内」= 概率 >= 0.5。

| 素材 | faces | 掩码覆盖 | A 只美白：掩码内平均差 | A 掩码外改动>=3 | B 只磨皮：掩码外改动>=3 | C 两者同开：掩码外改动>=3 | 判定 |
|---|---|---|---|---|---|---|---|
| 微信图片…（主素材） | 1 | 27.0% | **24.35** | **0.09%** | **0.00%** | 0.07% | PASS |
| A7300538 | 1 | 21.4% | **23.63** | **0.19%** | **0.00%** | 0.16% | PASS |
| A7300544 | 0 | 0.6% | 0（按设计不提亮） | 0.00% | 0.00% | 0.00% | PASS |
| A7300552 | 0 | 11.3% | 0（按设计不提亮） | 0.00% | 0.00% | 0.00% | PASS |
| A7300560 | 0 | 0.0% | 0（按设计不提亮） | 0.00% | **0.00%**（修前 53.94%） | 0.00% | PASS |

对照 Phase 1 的 v1.4 数据（同一批素材、面板美白 100）：
**非皮肤区被提亮 40.0% / 77.5% / 69.0%，平均 +11.4 ~ +18.1 / 255。**

---

## 五、成本

| 项 | 实测 |
|---|---|
| 初始化（读 23 MB bundle + 建 tflite） | **594~600 ms**，放在 `createInstance` 预热，与 SDK 的 1.7 s 重叠 |
| 单次推理（检测+关键点+皮肤分割） | **15.6 ms** @1080×1920，与分辨率基本无关 |
| 降频（每 3 帧推理一次） | 摊销 **约 5.2 ms/帧** |
| 被删掉的 v1.4 CPU 颜色掩码 | **−4.5 ms/帧** |
| **净增** | **约 +0.7 ms/帧** |

降频系数在 `nama_engine.cpp` 的 `kRegionEvery`（当前 3）。快速摇镜 / 主体突然入画
会滞后最多 2 帧；要更稳就改小（N=2 → 7.8 ms/帧）。

---

## 六、运行时开关

| 环境变量 | 作用 |
|---|---|
| `NAMA_REGION_OFF=1` | 强制退回 v1.4 颜色启发式，用于同一段素材做 A/B |
| `NAMA_REGION_BUNDLE=<路径>` | 显式指定明文 bundle（默认在 fuai.dll 同目录找） |
| `NAMA_REGION_TRACE=1` | 逐调用追踪 + 安装 VEH + 哨兵量 extra 写入量 |
| `NAMA_REGION_SELFTEST=1` | 预热后立刻用合成图跑一次推理，切分故障区间 |

明文 bundle 的查找顺序（`NamaRegion::findBundle`）：
`NAMA_REGION_BUNDLE` → `ai_face_processor_pc_plain.bundle` → `model\…_plain.bundle`
→ `ai_face_processor_pc.bundle` → `model\ai_face_processor_pc.bundle`。

> 随安装包分发的 `model\ai_face_processor_pc.bundle` 是**加密件**，`PK` 校验会失败，
> 日志会明确提示「bundle 不是明文 Zip」。这是预期行为，插件降级、不崩。

---

## 七、复现命令

```powershell
Set-Location '...\03_测试夹具'
$env:PYTHONIOENCODING='utf-8'
$env:NAMA_OFX_DLL='...\01_ofx插件源码\out\DaVinciBeauty.ofx'
$env:NAMA_ENGINE_DIR='C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64'
$py='python.exe'

# 0) 生成测试帧（v1.5 必需）
& $py make_probe_frame.py "$env:USERPROFILE\Pictures\微信图片_2026-10-02_201126_548.jpg"

# 1) 回归（区域开 / 区域关各一遍，都应 19/19 PASS）
& $py mock_host.py
$env:NAMA_REGION_OFF='1'; & $py mock_host.py; Remove-Item Env:NAMA_REGION_OFF

# 2) 区域验收（掩码外不得被改）
& $py probe_phase2_region_gate.py "$env:USERPROFILE\Pictures\微信图片_2026-10-02_201126_548.jpg"

# 3) fuai 契约隔离验证（不含 CNamaSDK）
& $py probe_region_contract.py "$env:USERPROFILE\Pictures\微信图片_2026-10-02_201126_548.jpg" --variant B
```

---

## 八、还没做的（等决策）

1. ~~版本号 `NAMA_BUILD_MINOR` 还是 4，没改成 1.5。~~ **已改**：`NAMA_BUILD_MINOR=51`、
   `NAMA_BUILD_STR="1.51"`，`NamaVersion()` 返回 `0x00010033`。预热日志新增 `build=%s`，
   否则日志里只有 SDK 版本（8.10.0），分不出装的是 1.4 还是 1.51。
2. ~~`ofx.log` 里的区域状态~~ **已加**：
   - 预热时一行 `createInstance 语义区域 region=on/off segfaces= cov= infer= n= <失败原因>`
     —— 出问题时第一个问题永远是「区域到底有没有生效」，这行就是答案。
   - `render` 的 verbose 行末尾加 `region=on/off segn=N`。`segn` 是累计推理次数，
     它增长得比帧号慢正好印证降频在起作用（实测 #1→#4 之间从 1 涨到 2）。
3. ~~明文 bundle 的落地~~：维持手工拷到安装目录（用户确认不分发安装包）。
4. `ofx_plugin.cpp` 里三处已被 Phase 0 证伪的注释（`:542/:545/:550`）还没改。
5. **磨皮质感重新标定**：用户决定**暂时维持不动**，等打包时再评估 ——
   非皮肤区被压到逐字节不动后，同样的 `blur` 值观感会比 v1.4 弱一些
   （因为以前有一部分「磨感」来自被误磨的背景）。
