# Phase 1 · 区域精准化探测报告

> **已废弃（2026-11-08）**。同上：探针对象为已移除的闭源 SDK。
**日期**：2026-10-24
**目的**：判断「磨皮 / 美白只作用于皮肤」这件事能不能从**颜色判据**升级成**模型级语义分割**，
并给出可落地的路线。范围：区域精准化（A）+ 性能（C）。
**结论一句话**：**SDK 侧所有路都堵死了；唯一能拿到真·皮肤分割的办法是外挂 `fuai.dll`，
代价是 15.1 ms/帧（可摊销），换来「白衣服、木桌、米色墙不再被提亮」。**

---

## 一、结论摘要

| # | 结论 | 证据 |
|---|---|---|
| 1 | `fuLoadAIModelFromPackage` 的 aitype **不能用来加挂皮肤分割** —— 它是**精确枚举，不是位掩码** | 1<<0…1<<31 逐位 + 6 组组合全部无效。硬证据：`aitype=0x100` → 预热 859 ms、`faces=1`；`aitype=0x101` → 预热 313 ms、**`faces=0`**（人脸模型根本没加载）。见 5.5 |
| 2 | **皮肤分割模型其实已经在跑** —— 问题不是「没加载」，而是「美颜道具不用它」 | 初始化日志里有 `fuai/face/face_processor.h:200 Perform SetUseSkinSeg` 与 `:166 Perform SetUseDelSpot`。所以 CNamaSDK 的人脸处理器**每帧都在算皮肤分割**，只是美颜道具的掩码没有消费这个结果 |
| 3 | bundle 的 `enable_skinseg` 开关**在我们的集成路径上无效** | `enable_skinseg=1;color_level=1` 与 `color_level=1` 逐字节相同；且全程**没有任何 `open/close FUAITYPE_...` 日志**，说明 `script.js` 里那段 aitype 切换从未执行 |
| 4 | 美白**没有任何 SDK 侧的区域化路径**，Phase 0 的结论成立且更硬 | `color_level` / `color_level_v2` 在非肤色块上改动 **100.0%** |
| 5 | 现役颜色掩码（Cr/Cb + R>G>B）**误判严重** | 真实素材上「颜色说是皮肤、语义说不是」占全图 **13.6 ~ 47.0 个百分点** |
| 6 | 误判会变成**肉眼可见的画质事故** | 640×394 素材、面板美白 100：非皮肤区 **40%~77.5% 的像素被提亮**，平均 **+11.4 ~ +18.1 / 255**；白色蕾丝上衣被整体提亮到丢失纹理（见 `_phase1_overlay.png`） |
| 7 | 掩码羽化会**外扩** | 颜色掩码**之外**的像素里仍有 20~27 个百分点（占全图）被提亮 —— 3×3 可分离羽化 + 1/4 分辨率双线性放大把边界抹开了 |
| 8 | 磨皮区域同理：SDK 双边档下非皮肤区 **27.5%~47.1%** 被磨 | 与 Phase 0 表 1 一致（BLUE 93.3%），换成真实素材后幅度略低但方向不变 |
| 9 | `delspot_level` / `remove_nasolabial_folds_strength`（v1）会**把美颜整体搞挂** | 开启后从第 2 帧起 FACE Δ = **2.96 → 0.00**，整帧不再有任何改动。**不能直接当新滑块加上去** |
| 10 | 其余 9 个新参数是 no-op | `remove_nasolabial_folds_strength_v2` / `tooth_whiten` / `heavy_denoise` / `blur_scale_ratio` / `blur_offset_scale` / `manual_scale` / `beauty_mask_v=0` / `warp_mask_v=0` / `eye_bright_v2` 全部与基线逐字节相同 |

---

## 二、方法

### 2.1 为什么 Phase 0 的素材不够

Phase 0 用「纯色噪点块」当假肤色（`PALE` / `SKINTONE`）。但那两块**本身就是肤色**，
语义分割网络同样会判成皮肤 —— 那组实验无法区分「颜色判据」和「语义分割」。

要区分两者，只能用**颜色像皮肤、语义上不是皮肤**的素材：白衣服、木桌、米色墙、皮革。

### 2.2 素材筛选（`find_skinlike_material.py`）

对 44 张照片同时算两条判据，取差值最大的：

| 文件 | faces | 颜色命中% | 语义命中% | 差值 |
|---|---|---|---|---|
| 薄饼.jpg | 0 | 65.1 | 0.1 | **65.0** |
| 微信图片_2026-10-02_201126_548.jpg | 1 | 66.3 | 15.5 | **50.8** |
| A7300557.jpg | 2 | 50.5 | 3.3 | 47.2 |

最后选 `微信图片…` 作为主素材（**有人脸 + 大面积肤色非皮肤**），其余作为分布参考。

### 2.3 驱动方式

沿用 Phase 0 的夹具（`probe_whiten_scope.Driver`，模拟最小 OFX 宿主），
口径一致：基线一律是「面板全 0」——此时插件走 IsIdentity 透传，输出就是原始帧，
所以 Δ 直接等于该用例的总改动量。

新增两处改进：

1. **稳定帧数 2 → 6**。`enable_skinseg` / `delspot` 会让 bundle 里的 `script.js`
   改写 aitype 并触发 SDK **重新加载 AI**，重载期间那一两帧是空转的。
   2 帧口径会把「正在重载」误判成「参数无效」（实测第 1 帧 Δ=126.16、第 2 帧起才稳定）。
2. **引擎加了一个默认关闭的诊断开关** `NAMA_AITYPE_EXTRA`（`nama_engine.cpp`），
   把待验证的位 OR 进 aitype。不设该变量时行为与历史完全一致。

---

## 三、探针 A：aitype 位扫描（`probe_phase1_region.py --cases ai`）

`05_解密模型数据` 证明 `ai_face_processor_pc.bundle` 里带着
`skin_small_tiny_self_1205_nama_sparse.tflite`（皮肤分割）与去痘印模型，
CNamaSDK.dll 里也有 `FUAITYPE_FACEPROCESSOR_SKINSEGMENT` / `_DELSPOT` 枚举名。
问题是**枚举数值反查不到**（名字是连续字符串块，没有指针表可回溯），
所以按行为逐位扫。

| 用例 | aitype | FACE Δ | BLUE 改动% | 与基线逐字节相同 |
|---|---|---|---|---|
| `ai_none`（基线） | 0x100 | 2.96 | 93.3 | — |
| `ai_bit00` … `ai_bit07` | 0x100 \| 1<<n | 2.96 | 93.3 | ✅ |
| `ai_bit09` … `ai_bit15` | 0x100 \| 1<<n | 2.96 | 93.3 | ✅ |
| `ai_combo_200_400` 等 3 组 | 0x100 \| 组合 | 2.96 | 93.3 | ✅ |

**全部 18 个用例输出与基线逐字节相同。**
→ `fuLoadAIModelFromPackage` 的 aitype 只认 `FUAITYPE_FACEPROCESSOR`，
OR 进任何其它位都不会让皮肤分割接管磨皮。这条路**堵死**。

> 附带发现：额外位会让初始化耗时从 ~820 ms 掉到 ~300 ms，说明加载被提前短路了，
> 但画面结果不变 —— 也印证了「没有加载到额外模型」。

---

## 四、探针 B：新参数生效矩阵（`--cases param`）

参数名来自 `face_beautification/files/script.js` 与 CNamaSDK 的参数字符串池，
Phase 0 没测过。

| 参数 | 值 | 结果 |
|---|---|---|
| `remove_nasolabial_folds_strength_v2` | 1.0 | no-op（逐字节同基线） |
| `tooth_whiten` | 1.0 | no-op |
| `heavy_denoise` | 1.0 | no-op |
| `blur_scale_ratio` | 0.5 | no-op |
| `blur_offset_scale` | 0.5 | no-op |
| `manual_scale` | 1.0 | no-op |
| `beauty_mask_v` | 0.0 | no-op |
| `warp_mask_v` | 0.0 | no-op |
| `eye_bright_v2` | 1.0 | no-op |
| **`delspot_level` + `use_delspot`** | 1.0 / 0.5 | **整帧失效**：settle 2.96 → 0.00 |
| **`remove_nasolabial_folds_strength`(v1)** | 1.0 | **整帧失效**：settle 2.97 → 0.01 |

`delspot` / `naso(v1)` 的机制是：`script.js` 检测到 `delspot_level>0` 就往 aitype 里加
`FUAITYPE_FACEPROCESSOR_DELSPOT`，触发 AI 重载；重载后道具进入一种「什么都不做」的状态
（render 耗时反而从 5.0 ms 涨到 8.6 ms，说明 SDK 在算，但输出等于输入）。

**工程含义**：这两个参数**不能**当作「去痘印 / 去法令纹」滑块直接暴露给用户 ——
一拖就整条美颜失效。要做这两项功能，必须先搞清楚重载后为什么道具失活。

---

## 五、探针 C：`enable_skinseg` 是否改变区域（`--cases whiten`）

`script.js` 里 `enable_skinseg` 是官方开关，逻辑是：
`color_level>0 且 enable_skinseg>0.5` → `aitype += FUAITYPE_FACEPROCESSOR_SKINSEGMENT`。
这是唯一不依赖「猜枚举数值」的加载路径，所以必须单独验证。

| 用例 | 附加参数 | FACE Δ | BLUE 改动% |
|---|---|---|---|
| `w_off` | —（面板磨皮=1） | 2.55 | 97.7 |
| `w_color` | `color_level=1` | 21.83 | **100.0** |
| `w_seg_color` | `enable_skinseg=1;color_level=1` | 21.83 | **100.0** |
| `w_color_v2` | `color_level_v2=1` | 27.32 | **100.0** |
| `w_seg_v2` | `enable_skinseg=1;color_level_v2=1` | 27.32 | **100.0** |
| `w_seg_on` | `enable_skinseg=1` | 27.32 | 100.0 |
| `w_seg_delspot` | `enable_skinseg=1;delspot_level=1;use_delspot=1` | 27.40 | 100.0 |
| `b_seg` | 面板磨皮=100 + `enable_skinseg=1;color_level=1` | 21.83 | 100.0 |

`w_seg_color` 与 `w_color`、`w_seg_v2` 与 `w_color_v2` **逐字节相同**
→ `enable_skinseg` 对我们的集成路径**没有任何影响**。
（`w_seg_on` 单独开时出现 27.32 的提亮，与 `color_level_v2=1` 完全一致，
说明这个参数在本集成下不是「区域开关」而是触发了别的默认状态 —— 更不能依赖它。）

### 5.4 更正：皮肤分割其实一直在跑，只是没被用作掩码

上面那组「参数无效」的结论**是对的，但原因不是「模型没加载」** —— 这一点初版报告判断错了，
复核日志后更正如下。

初始化日志（`--case ai_none`，未设任何附加参数）：

```
I fuai/face/face_processor.h:95 ] Perform SetUseFaceLandmark.
I fuai/face/face_processor.h:156] Perform SetUseHairSegmenter.
I fuai/face/face_processor.h:162] Perform SetUseHeadSegmenter.
I fuai/face/face_processor.h:172] Perform SetUseLipsOccuSegmenter.
I fuai/face/face_processor.h:200] Perform SetUseSkinSeg.      <-- 皮肤分割被启用
I fuai/face/face_processor.h:166] Perform SetUseDelSpot.      <-- 去痘印也被启用
```

也就是说：**CNamaSDK 的人脸处理器每帧都在算皮肤分割与去痘印**，那部分成本已经含在
11–15 ms 里。真正的断点在**下游**：美颜道具（`face_beautification`）的磨皮/美白着色器
没有消费这个结果，仍然走 `USE_COLOR_MASK_OUT` 那条逐像素颜色判据。

而 `script.js` 里唯一能把两者接起来的开关是「往 aitype 里加
`FUAITYPE_FACEPROCESSOR_SKINSEGMENT`」，它只在 JS 控制器内部生效。
实测全程**没有出现任何 `open/close FUAITYPE_...` 日志**（`_phase1_report.txt` 里 AI 行为 0 条），
说明我们的集成路径（`fuCreateItemFromPackage` + `fuItemSetParamd`）**没有走到 bundle 的 JS 控制器**，
所以 `enable_skinseg` 这层开关对我们**结构性不可用**。

> 这也是为什么 `delspot_level` / `nasolabial(v1)` 会「整帧失效」：它们同样是 JS 侧的设计，
> 直接写原生参数会让道具进到一个没被正确初始化的状态。

**仍未排除的盲点**：`fuRenderItemsMasked` 未试（约半天，Phase 0 记过「函数存在、签名未知、
需要 GL 上下文」）。若它能把 skin mask 输出到纹理，就是零成本方案。

### 5.5 补扫 16–31 位：`aitype` 是**精确枚举，不是位掩码**

初版只扫了 1<<0…1<<15，留下「高位可能才有效」的疑点。补扫 16–31 位 + 6 组组合后：

| 用例 | aitype | 预热耗时 | `faces` | BLUE 改动% |
|---|---|---|---|---|
| `ai_none`（基线） | 0x100 | **859 ms** | **1** | 93.3 |
| `ai_bit00` | 0x101 | **313 ms** | **0** | 93.3 |
| `ai_bit16` … `ai_bit31`（全部） | 0x100 \| 1<<n | 295–343 ms | **0** | 93.3 |
| 6 组组合 | 多位 | 299–326 ms | **0** | 93.3 |

判据不是画面，是 `ofx.log` 里的 `faces=` 与预热耗时：

```
aitype=0x100  createInstance 预热引擎 成功 耗时=859ms   render ... faces=1  sdk=5.0ms
aitype=0x101  createInstance 预热引擎 成功 耗时=313ms   render ... faces=0  sdk=2.2ms
```

**只要 OR 进任何一位，人脸模型就完全不加载**（`faces` 恒 0、预热少掉那 546 ms）。
所以第三个参数是**精确枚举值，不是位掩码** —— 「加挂皮肤分割」这个思路从根上不成立。

> ⚠️ **这也意味着初版扫描的「逐字节相同」是个假阴性**：那些用例里人脸检测根本没跑，
> 而磨皮的肤色掩码是逐像素颜色判据、不依赖人脸检测（Phase 0 表 2），
> 所以画面看起来「磨皮照旧」，从像素差异上完全看不出模型没加载。
> 这类探测必须以 `faces=` 和预热耗时为判据，不能只看像素。

**最终判定**：A1 不可用。不是「没找到正确的位」，而是这条路的设计前提（位掩码）就不成立。

**结论**：SDK 侧目前没有可用路径；要么接受颜色掩码，要么自己拿语义掩码。

---

## 六、探针 D：真实素材上的误伤面积（`probe_phase1_whiten_scope.py`）

面板美白 = 100（磨皮压到 1，只为让管线跑起来），640×394：

| 素材 | faces | 颜色命中 | 语义命中 | 误判面积 | 非皮肤区被提亮 | 平均提亮 | 非皮肤区被磨 |
|---|---|---|---|---|---|---|---|
| A7300538 | 1 | 33.0% | 21.2% | 13.6 pp | 40.0% | +11.4 | 27.5% |
| A7300563 | 1 | 26.7% | 2.1% | 24.8 pp | 52.6% | +13.3 | 33.3% |
| 微信图片… | 1 | 64.3% | 26.9% | **38.6 pp** | **77.5%** | **+16.0** | 47.1% |
| A7300557 | 2 | 49.4% | 3.5% | 47.0 pp | 69.0% | +18.1 | 32.0% |

「误判面积」= 颜色判据说皮肤、语义分割说不是皮肤的面积（占全图）。
「非皮肤区被提亮」= 语义分割判为非皮肤（<0.3）的像素里，实际被改动 ≥3 的比例。

> 注意：`A7300557` 的语义命中只有 3.5% 却检出 2 张脸 —— 说明该图人脸在画面里占比很小，
> 语义掩码的召回本身也有限。所以「误判面积」比「非皮肤区被提亮」更可靠，
> 后者的分母在语义掩码漏检时会偏大。

另外两个结构性发现：

1. **掩码羽化外扩**：颜色掩码**之外**的像素里仍有 20~27 个百分点（占全图）被提亮。
   原因是 `applyWhiten` 在 1/4 分辨率上做 3×3 可分离羽化、再双线性放大 4 倍 ——
   等效边界抹开 12 px，小画幅上就是一大片。
2. **提亮量饱和**：白色蕾丝上衣（原本 230+）被推到接近 255，纹理全部丢失。
   这不是「轻微泛白」，是画质事故。

### 视觉证据

`_phase1_overlay.png`（同一素材四联图）：

| 图块 | 内容 |
|---|---|
| input | 原始帧 |
| whiten 100 | 实际输出 —— **白衣服整体被提亮** |
| actually brightened 83.5% | 红色 = 真正被改动的像素，覆盖整件衣服 + 背景 |
| color heuristic mask 64.3% | 绿色 = 现在的掩码，把整件白衣都算成皮肤 |
| semantic skin mask 26.9% | 蓝色 = 语义分割，只覆盖脸、脖子、手 —— **正确的区域** |

---

## 七、对 Phase 2 的启示

### 7.1 路线判断

| 路线 | 判定 | 依据 |
|---|---|---|
| **A1 · SDK 内建 skinseg** | ❌ **不可用，已穷尽** | ① aitype 是精确枚举，OR 任何位都导致人脸模型不加载（5.5，`faces=0` 硬证据）；② 皮肤分割本来就在跑（5.4），缺的是「道具消费分割结果」的开关；③ 那个开关只在 bundle 的 JS 控制器里，我们的集成路径没走到；④ `enable_skinseg` 实测逐字节无变化 |
| **A1b · `fuRenderItemsMasked` 取 SDK 掩码** | ⏱ 约半天，可选 | 唯一还没试的入口。若它能把 skin mask 输出到纹理就是零成本方案 —— 但签名未知 + 需 GL 上下文，风险高于收益 |
| **A2 · 外挂 fuai 语义分割** | ✅ **可行，唯一确定的路** | 实测独立可跑，皮肤分割只 +2.6 ms（第二节、Phase 0 前测） |
| **A3 · 收紧颜色判据** | ⚠️ 收益有限 | 白衣 / 木桌与皮肤在 CbCr 平面上重叠，收紧只会同时砍掉暗肤色人群的覆盖 |

### 7.2 A2 的真实成本（这是决策的关键）

实测（1080×1920，检出 1 张脸，本机中位数）：

| 开启分支 | 耗时 | 相对底噪 |
|---|---|---|
| 检测 + 75 关键点（关键点关不掉，关了就访问违例） | 12.5 ms | 底噪 |
| **+ 皮肤分割** | **15.1 ms** | **+2.6 ms** |
| + 头部掩码 | 14.5 ms | +2.0 ms |
| + 头发掩码 | 18.0 ms | +5.4 ms |

**关键约束**：耗时与分辨率几乎无关（360×640 与 1080×1920 都是 ~20 ms），
因为模型输入尺寸固定 —— **「降分辨率提速」这条路不存在**，只能靠**降频复用**。

于是有两种用法：

| 用法 | 每帧成本 | 端到端（1080p 8bit） | 评价 |
|---|---|---|---|
| 每帧都跑 | +15.1 ms | 28–31 ms（32–36 fps） | 30fps 时间线够用，但吃掉全部性能余量 |
| **每 3 帧跑一次 + 帧间复用** | **+5.0 ms** | **18–21 ms（48–55 fps）** | 与现在（13–16 ms）基本持平 |

同时可以把现在 4.5 ms 的 CPU 颜色掩码**整段删掉**，净增约 **+0.5 ms**，
换来的是「区域从颜色判据升级成语义分割」—— 这是纯赚。

### 7.3 必须先解决的前置问题

外挂 fuai 需要**明文 bundle**（23 MB）。三条路：

| 方案 | 体积代价 | 风险 |
|---|---|---|
| (a) 随包分发明文 bundle | 安装包 82.7 → ~106 MB | 版权（模型归相芯科技），需确认授权范围 |
| (b) 运行时用 IAT hook 截 CNamaSDK 解密后的 buffer，缓存到 ProgramData | 0 | 脏、依赖 CNamaSDK 内部实现，SDK 升级会失效 |
| (c) 只用 `fuai.dll` 做「肤色簇分析」而不加载完整 bundle | 0 | 不可行 —— 皮肤分割是模型推理，没有 bundle 跑不了 |

### 7.4 建议的 Phase 2 范围

1. **先用一次专项探测排除 A1b**（`fuRenderItemsMasked`）—— 半天，成本极低，
   若成立则整个 A2 都不必做。
2. A1b 不成立 → 走 A2，但先定 bundle 分发方案（(a) 还是 (b)）。
3. 把 `applyWhiten` 的掩码源换成语义掩码，同时**删掉 1/4 分辨率 + 羽化**这套
   （语义掩码本身已经是软的，不需要再羽化；这顺带修掉「羽化外扩」）。
4. 磨皮侧：用语义掩码把 SDK 的输出与原图合成（`out = mix(blur_out, src, mask)`），
   把非皮肤区从「93.3% 被改」压到 **逐字节不动**。
5. `delspot` / `naso(v1)` 暂不暴露为滑块（会整帧失效，见第四节）。

### 7.5 验收标准（Phase 2 用）

- 非皮肤区（语义 <0.3）被提亮比例 **≤ 3%**（现在 40%~77.5%）
- 颜色掩码之外的像素被改动比例 **≤ 1%**（现在 20~27 pp）
- 白色衣物 / 米色墙画面：人脸区外平均漂移 **≤ 0.5 / 255**
- 1080p 8bit 端到端 **≤ 21 ms**
- 现有 `mock_host.py` 19 项全 PASS，新增「非皮肤区不得被提亮」断言

---

## 八、复现命令

```powershell
Set-Location '...\03_测试夹具'
$env:PYTHONIOENCODING='utf-8'
$env:NAMA_OFX_DLL='...\01_ofx插件源码\out\DaVinciBeauty.ofx'
$env:NAMA_ENGINE_DIR='C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64'
$py='python.exe'

# A/B/C 三组：aitype 位扫描 / 新参数矩阵 / enable_skinseg
& $py probe_phase1_region.py "$env:USERPROFILE\Pictures\A7300538.jpg"
& $py probe_phase1_region.py "$env:USERPROFILE\Pictures\A7300538.jpg" --cases whiten,param

# 挑「肤色但不是皮肤」的素材
& $py find_skinlike_material.py "$env:USERPROFILE\Pictures" --top 8

# 真实素材上的误伤面积 + 四联对比图
& $py probe_phase1_whiten_scope.py "$env:USERPROFILE\Pictures\微信图片_2026-10-02_201126_548.jpg"
& $py probe_phase1_overlay.py     "$env:USERPROFILE\Pictures\微信图片_2026-10-02_201126_548.jpg" out.png
```

---

## 九、本轮改动清单

| 文件 | 改动 | 是否影响产品行为 |
|---|---|---|
| `01_ofx插件源码/src/nama_engine.cpp` | ① 扫描期间临时加过 `NAMA_AITYPE_EXTRA` 诊断开关，**扫完已撤除**；② 保留 `fuLoadAIModelFromPackage` 返回值检查（以前被忽略，失败时会静默降级成 `faces=0`）；③ aitype 常量旁补了「这是精确枚举不是位掩码」的注释与实测数据 | **否**（aitype 仍恒为 `0x100`；新增的只是一条失败时的 stderr 警告） |
| `03_测试夹具/probe_phase1_region.py` | 新增：aitype 0–31 位扫描 + 6 组组合 + 新参数矩阵 + skinseg 对照，稳定帧 6 帧，汇总按 `init_ms` 排序 | 否（夹具） |
| `03_测试夹具/probe_phase1_whiten_scope.py` | 新增：真实素材上的误伤面积量化 | 否（夹具） |
| `03_测试夹具/probe_phase1_overlay.py` | 新增：四联对比图 | 否（夹具） |
| `03_测试夹具/find_skinlike_material.py` | 新增：按「颜色 vs 语义」差值挑素材 | 否（夹具） |

> 本轮**未改任何默认值 / 面板文案 / 安装器**，也没有重出安装包。
> `out\DaVinciBeauty.ofx` 已按最终源码重编译（exit=0），`mock_host.py` **19/19 PASS**；
> 安装目录里的那份仍是 v1.4 原样。
