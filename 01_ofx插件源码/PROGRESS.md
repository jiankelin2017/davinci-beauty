# DaVinci Beauty OFX 插件 —— 进展与决策记录

最后更新：2026-10-31（**v3.0.0 开源引擎版**：CPU 全量移植 + 人脸推理 GPU 自动检测；当前 **v3.0.4**：修正帧行序 bottom-up —— Resolve 真机帧此前被当成 top-down 读，人脸上下颠倒导致 `faces=0 score=0.25`）

> 本文件是**精简版**：只留结论、实测数据、踩过的坑。
> v1.4 → v2.3.0 的完整流水账（含探测过程、各版本验证明细）在
> `PROGRESS_ARCHIVE_至v2.3.0.md`，需要追查历史时再翻。

---

## 〇、v3.0.0 变更（2026-10-31，开源引擎 + 人脸推理 GPU 自动检测）

**v3.0.0 把闭源 FaceUnity NamaSDK 彻底移除，引擎层整体替换为 OBS 美颜 3.1.39 的
自研纯 CPU 美颜引擎（CoreEngine），人脸检测用 ONNX YuNet。**

- **CPU 全量移植**：14 项美颜/美型效果（磨皮、清晰、美白、红润、锐化、立体、亮眼、
  黑眼圈、法令纹、祛斑痘、瘦脸、大眼、下巴、全局锐化）全部在 CPU 上逐字节还原；
  自研引擎不再依赖任何闭源 DLL。
- **人脸推理 GPU 自动检测（DirectML）**：引擎启动探测 ONNX Runtime 的 DirectML 执行提供器。
  把 `07_引擎资产/onnxruntime/onnxruntime.dll` 换成 `onnxruntime-directml` 构建版，
  人脸检测即自动走 GPU；否则自动退回 CPU。靠"换 DLL"启用，零编译耦合。
  运行时可用环境变量 `BEAUTY_GPU` 覆盖（0=强制 CPU，1=强制 GPU，其它=自动）。
- **产物**：`installer\DaVinciBeauty_Setup_v3.0.4.exe`（单文件安装包）。
- **验收口径不变**：版本 / 参数三表 / 回归 / 接口 / 装机 / 工程 六组，宿主 `ofx_plugin.cpp` 零改动。

## 〇-b、v3.0.2 变更（2026-10-31，人脸检测预处理修正）

- **修复「清晰正脸却全局磨皮」**：`beauty_face.cpp::detect()` 的输入预处理从归一化（`(x-127.5)/128` / `x/255`）改回**原始 0-255**，按源缓冲原顺序喂入。
- 根因：本地用 `onnxruntime` 对 `07_引擎资产/yunet.onnx` 实跑验证——归一化会让检测全部归零（bestScore 0.92→0.05），raw 0-255 才正常；通道序（RGB/BGR）对检出无影响（已证伪早期「通道序错配」假说）。
- `BEAUTY_YUNET_PREPROC` 取值范围扩到 0..5（默认 0=raw）；`nama_engine.h` 版本号 `3.0.1 → 3.0.2`（修订位 +1）。
- 详见 `开发文档.md` §2。

## 〇-c、v3.0.3 变更（2026-10-31，三个「假故障」——都不是检测算法的问题）

v3.0.2 之后仍然「清晰正脸、画面毫无效果、日志 `faces=0 score=0.00`」。加 `BEAUTY_DEBUG_DUMP`
把**真正送进模型的缓冲**落盘后，查出三条独立原因：

1. **`NamaSelfTest` 把「文件路径」当成了「图像数据」**（`nama_engine.cpp`）。
   v2.0 移植 CoreEngine 时把原来「读文件→处理→写文件」改成「直接把入参当指针」，
   而探针脚本传的是路径。于是喂进检测的是字符串 `C:\Users\...`（dump 首字节
   `43 3A 5C 55 73 65 72 73`），必然 0 脸；输出又写进 Python 的 `bytes` 对象 → 段错误。
   **v3.0.1 / v3.0.2 期间所有离线「引擎 0 检出」结论全部作废。**
   修复后同一张 `probe_in.raw`：`faces=1 best=0.9214`（Python 参考值 0.9214，完全一致）。
2. **异步检测在「按需渲染」宿主里永久透传**（`beauty_core.cpp::updateMask`）。
   `asyncOn_` 默认开，Resolve 只在播放头移动时渲染，worker 结果永远赶不回来 →
   `everHadMask_` 恒 false → `process()` 一直走「首帧透传」，一个字节都不改。
   修复：`everHadMask_ == false` 时**首帧先同步检测一次**（多约 15ms，仅首帧）。
3. **参数句柄取不到时静默给 0**（`ofx_plugin.cpp::readParams`）。
   面板全 0 → 引擎命中「逐字节透传」早退分支，日志特征 `sdk≈1ms / faces=0 / score=0.00`。
   修复：回落到 `kParams[i].dflt`；`createInstance` 打 `WARN N/M 个参数句柄取不到`。

配套新增（都默认关闭、零稳态开销）：`面板参数 ... = 50/0/30/...` 变更日志、
`BEAUTY_DEBUG_DETECT`（每帧检测明细）、`BEAUTY_DEBUG_DUMP`（落盘 `src/lb/in.f32/12 个输出`）。

离线复验（`selftest_ofx.py`，blur=70 whiten=15）：640×394 / 1080×1920 / 720×1280 / 1920×1080
四种分辨率全部 `faces=1 best=0.92`，异步默认档同样首帧即通过。
`nama_engine.h` 版本号 `3.0.2 → 3.0.3`（修订位 +1）。详见 `开发文档.md` §2-b。

## 〇-d、v3.0.4 变更（2026-10-31，帧行序 bottom-up —— Resolve 侧 faces=0 的真因）

v3.0.3 修完三个「假故障」后，Resolve 里 `sdk` 耗时已经正常（≥15ms，检测真跑了），
但日志仍是 `faces=0 score=0.25`。把真机帧原样 dump 出来（`C:\beauty_dump\src.raw`，
1080×1920 RGBA）离线复跑，才定位到**帧行序**：

| 读行方向 | bestScore | faces |
|---|---|---|
| top-down（v3.0.3 及以前） | **0.2519** | 0 |
| bottom-up（v3.0.4） | **0.9333** | **1** |

`0.2519` 与 Resolve 日志里的 `score=0.25` 完全一致 → 确认真机帧是 bottom-up。
**OFX 规范规定 `kOfxImagePropData` 指向图像左下角**，Resolve 严格照做；而 PIL/numpy 与
所有离线夹具都按 top-down 存帧，于是「离线 0.92 通过、真机 0.25 全灭」。

实现（零成本，不整帧翻转）：

- `letterboxRgb()` 本就要缩放重采样，直接把读行换成 `sh-1-y`，插值权重不变。
- 检测出的框/关键点是**视觉坐标**，再映射回缓冲行号：
  `f.y = h - vy - f.h`、`kps.y = h - vy`。掩码/磨皮/美白/几何变形全按缓冲行号落笔，下游一行不改。
- 引擎默认 `imgBottomUp_ = true`；`NamaSelfTest` 内部强制声明 top-down（夹具喂的是 top-down 文件），
  可用 `BEAUTY_IMG_BOTTOMUP=1` 回归真机帧。

配套修正（都是「让测试说真话」，不是放水）：

- `mock_host.py` 新增 `_to_bottom_up()`，夹具帧序与真实宿主一致（此前夹具替插件掩盖了方向 bug，一路 PASS）。
- `NamaSelfTest` 恢复 `NAMA_ST_*` 自测钩子：此前 15 个基线用例里有 8 个实际输出同一个 md5，
  只覆盖 4 种参数组合，参数接线断了也测不出来。
- `NamaSelfTest` 强制关异步：同一输入连跑两次会因 worker 调度得到不同 md5，基线不可复现。
- `06_验收工具/baseline_manifest.txt` 按新引擎重新标定（原清单是 v2.3.1 NamaSDK 时代的）。

离线复验：真机 1080×1920 帧 `faces=1 best=0.9333`、画面改动 1.56% 且只落在人脸框内；
640×394 探针 `faces=1 best=0.9214`。`nama_engine.h` 版本号 `3.0.3 → 3.0.4`（修订位 +1）。
详见 `开发文档.md` §2-c。

## 一、当前状态

**v3.0.0 已发布：开源引擎（CPU 全量）+ 人脸推理 GPU 自动检测（DirectML 优先、CPU 兜底）。**
自 v2.3.1 起像素输出逐字节不变；v3.0.0 进一步彻底脱离闭源 SDK。

- 产物 md5 `e3d649563f6d1e24ff2ddde7f9b0e803`（编译版 = 装机版，字节一致）
- 逐字节回归 15/15，mock host OFX 契约 19 项全过
- 实机确认：空项目 1080p 播放稳定流畅

**仍然未解决 / 不做**：

- 4K 实时预览跑不动（需 Render Cache）。已做过 A+B+C + 内存流量优化，
  剩余大项是必需的全帧遍历（格式转换、美型第二遍）。
- 无脸帧约 8% 像素仍会被改（掩码误判无兜底，OBS 侧遗留 P0）。
- 降分辨率换速度已被用户否决；GPU shader 化未做。

## 二、交付位置

| 内容 | 路径 |
|---|---|
| 已安装插件 | `C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\` |
| 源码 | `src\ofx_plugin.cpp`、`src\nama_engine.{h,cpp}` |
| 编译 / 安装 | `build.bat`、`install.bat` |
| 使用说明 | `README.md` |
| **一键验收** | `..\06_验收工具\acceptance.py`（或双击 `verify.bat`） |
| 逐字节回归 | `..\06_验收工具\baseline.py` |
| 性能 A/B 测量 | `..\06_验收工具\perf_ab.py` |
| 模拟宿主测试 | `..\02_测试夹具beauty_probe\mock_host.py` |
| 插件自带日志 | `C:\ProgramData\DaVinciBeauty\logs\ofx.log` |

## 三、不可回退的设计决策 ★

改这些之前先读完本节，每条背后都有实测代价。

| 决策 | 原因 |
|---|---|
| **`sharpen`（锐化）默认 0 且 `secret` 隐藏** | 磨皮 > 0 时锐化对输出贡献**恒为 0**（字节级确证 0/252160）。暴露出来用户会以为插件坏了。要锐利感用「清晰」。 |
| **全局滤镜永久关闭**（`filter_name="origin"`、`filter_level` 恒 0） | 旧行为是「八项全 0 时为 0，否则 0.4」，会让磨皮一开就整帧提亮。0.4 是原 OBS 插件的产品选择，不该继承。 |
| **支持 float**（降到 8bit 处理再升回） | 有损，但保证 Color 页面能加载。 |
| **线程局部缓冲不用 `thread_local std::vector`** | 带析构函数的线程局部对象，宿主 `FreeLibrary` 后若有线程退出，DLL 内析构函数指针会悬空。改用 `__declspec(thread)` 裸指针，指向刻意不释放的分配。 |
| **不做 Rust 版** | 本机无 Rust 工具链；OFX 是 C ABI，Rust 不解决本项目真正的难点。 |
| **Lua 脚本版已移出菜单** | 代码保留但不再维护，只留 OFX 一条路线。 |
| **默认磨皮 50 / 美白 30 / 红润 30**（v2.2.3 用户决策） | 覆盖 GT 结论，见第五节。回退只改 `kParams[].dflt` 与 `NamaSettings` 的 `d[]` 三个数，**引擎曲线未动**。 |

## 四、宿主行为（Resolve）的坑 ★★

这几条是装机截图反复试出来的，**mock host 层面全都验证不出来**。

1. **OFX 面板不渲染 Page，也不渲染 Group。** v2.2.0（Page）→ v2.2.1（Group 挂 Main 页下）→ v2.2.2（Group 留 root 层级）三版实测**全部平铺**。
   ⇒ **面板布局只能靠 `paramDefine` 的定义顺序**。v2.2.3 起用「沉底」：美型三项挪到列表最末。
2. **UI 归属类改动必须装机 + 重启 Resolve + 截图确认**，没有捷径。
3. **换插件后必须重启 Resolve**。Resolve 把 OFX 扫描结果缓存在
   `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`，
   以 `mtime + size` 判失效。走缓存时日志不再打印 `OFX: loading ...`。
   （`install.bat` 已自动删这个缓存。）
4. **换参数顺序是三个数组的事**：`nama_engine.h` 的 `P_*` 枚举、
   `ofx_plugin.cpp` 的 `kParams[]`、`nama_engine.cpp` 的 `keys[]`（面板值→SDK key 下标表）。
   三者必须逐一对应。v2.2.3 漏改 `keys[]`，后果是「拖 A 滑块、下发成 B 参数」，
   **编译期完全不报错**，是逐字节回归的 `shape100` 用例 md5 变了才抓出来。
   ⇒ 现在 `acceptance.py` 显式校验这三张表。

## 五、默认参数的依据（GT 数据）

磨皮默认值是**唯一有同帧 GT 支撑**的配置调整（v2.0.3）。口径：16×16 块 · 3×3 高通 std
· ROI = 我方引擎改动到的块（**已膨胀 2 次**，占全幅 17.3%）· 取第 25 百分位。

| 磨皮档位 | 皮肤区噪声地板 | 相对输入 | 备注 |
|---|---|---|---|
| 0（输入原帧） | 0.4996 | 100.0% | 第三方 blur=0 的真实相机帧 |
| 50 | 0.4300 | 86.1% | **v2.2.3 起的默认** |
| 62 | 0.4090 | 81.9% | v2.0.3 依据 GT 插值定的档 |
| 75 | 0.3830 | 76.7% | |
| 100 | 0.3427 | 68.6% | |
| 第三方 blur=80 | — | **81.4%** | 对标锚点 |

⚠️ **膨胀这一步不能省**：不膨胀时 ROI 只占 11.2%，同帧同档会从 81.9% 变成 78.5%，
是口径差异造成的假差。

⚠️ v2.2.3 的「50 / 30 / 30」是**用户侧观感决策，覆盖了这个 GT 结论**，不是新测的数据。

## 六、版本演进索引

| 版本 | 关键改动 | 产物 md5 |
|---|---|---|
| v2.0 | 引擎层整体回移 OBS 3.0.19 | — |
| v2.0.2 | 「清晰」独立成增益源（此前必须先把锐化推起来才有反应） | — |
| v2.0.3 | 默认磨皮 50 → 62（GT 支撑） | `0c2cb867…` |
| v2.1.2 | 三项美型装机（瘦脸 / 大眼 / 下巴），两遍渲染 | — |
| **v2.2.0** | **性能 3.2×**（热点是锐化不是磨皮）；美型拆页 | — |
| v2.2.1 / 2.2.2 | 美型分组两版尝试，**均被 Resolve 证伪** | `470d7c96…` / `3af4d552…` |
| v2.2.3 | 面板沉底 + 版本号入面板 + 默认改档；修 `keys[]` 串参数 bug | `7869dd1c…` |
| **v2.3.0** | 锐化中间结果改行缓存（内存流量优化） | **`e3d64956…`** |
| **v2.3.1** | **项目改名 DaVinci Beauty**（UI 文案/下拉去括号 + 全仓路径）；修 install.bat 漏拷资产 bug | `4323fb24…` |

## 六点五、v2.3.1 改名详情

### 改了什么

| 层 | 改动 |
|---|---|
| 界面文案 | 特效库分类 / 条目 / 挂片后标题统一为 **DaVinci Beauty**；磨皮模式四档、磨皮算法三档去掉括号后缀 |
| 文件名 | `NamaBeauty.ofx` → `DaVinciBeauty.ofx`；bundle 目录同名跟随 |
| 路径 | 引擎目录 `C:\ProgramData\NamaBeauty\engine` → `C:\ProgramData\DaVinciBeauty\engine`（日志路径同理） |
| 工程 | 根目录 `NamaBeauty_v1.51` → **`DaVinciBeauty`**；installer 脚本 `namabeauty.*` → `davincibeauty.*` |

**插件 ID `com.nama.davinci.beauty` 刻意保持不变** —— Resolve 按 ID 找效果，
改了会让所有历史工程里已挂的效果失效。

### 顺带挖出一个既有 bug（与改名无关，但改名才把它暴露出来）

`install.bat` 的拷贝清单从来**没包含 `ai_face_processor_pc_plain.bundle`**（23.3 MB，
语义掩码必需），旧 bundle 里那一份是 10/24 手动放进去的。所以第一次全新建 bundle 时它没被拷走，
验收「引擎资产齐全」立刻 FAIL，而且 `NamaSelfTest` 直接**崩溃**（rc=0xC0000005）——
回归比对拿不到任何用例输出。

已在 install.bat 补三处（stage 拷贝 / `:inst` 安装 / 校验清单）。装机脚本现在能自证齐全，
不再依赖「上一次手工补过」。

### 迁移

旧 bundle 没有删除，移到了 `D:\插件\_改名迁移备份_20261027\NamaBeauty.ofx.bundle`（138 MB），
确认新版在 Resolve 里没问题后可自行删除。OFX 目录下现在只有新 bundle 一个，
不会出现两份重复条目。

## 六点六、工程整合（v2.3.1 同日，用户要求「测试优化 / 验收 / 整包 / 清理」）

### 清理：351MB → 70MB

| 组 | 内容 | 体积 |
|---|---|---|
| `locked` | `_locked残留备份_20261027`（原件早已装机覆盖） | 139.3 MB |
| `build` | `bundle/`（install.bat 每次重生）+ `obj/` + 旧名 DLL 与 v2.1.2 备份 | 138.3 MB |
| `probe` | 探针输出 `probe_out.raw` / `_calib_out.raw` / `_np_out.raw` / 报告 txt | 3.0 MB |
| `external` | 改名前的旧 bundle 回滚点 | 137.4 MB |

删除前先跑了 dry-run（14 项 / 418.3MB），逐项核实后执行；`KEEP` 清单保护了
`probe_in.raw`（自测输入）与 `out\DaVinciBeauty.ofx`（当前产物），均完好。

### 整合包

`D:\插件\DaVinciBeauty_v2.3.1_整合包\`（123 文件 / 143MB）+ 同名 zip（109MB）。
含：源码 / 测试夹具 / 验收工具 / installer / **引擎资产** + `安装说明.txt`
+ `stage_engine.bat` + `MANIFEST.txt`（sha256）。03/04/05 历史遗留目录不进包。

换机器三步：`stage_engine.bat` → `01_ofx插件源码\install.bat` → `06_验收工具\verify.bat`。

### 测试优化：新增「工程」组（5 项）

最有价值的一条是 **install.bat 拷贝清单 ⊇ 装机必备资产**。v2.3.1 那次缺资产的崩溃
（rc=0xC0000005）静态判就能抓住，不必等到装机后才发现。这条检查做过反向验证：
把 `ai_face_processor_pc_plain.bundle` 那行从 install.bat 删掉，验收立刻 FAIL。

另外四条：UI 旧名残留、面板文案统一新名、下拉项无括号后缀、文档版本号同步。

写检查时踩到两个自己的假阳性，处理方式记一下：

- 参数级 label（变量 `pp`）也是 `kOfxPropLabel`，直接按类型匹配会把「磨皮」
  当成产品名未同步。**必须按变量名区分插件级（`props`）与参数级（`pp`）**。
- 下拉项查括号时若 regex 跨多个字符串配对，会把注释里的旧叫法也算进来。
  先 `strip_comments`，再按「不含换行」逐字符串提取。

## 六点七、NSIS 安装包（v2.3.1 同日，用户要求「打包 exe 带卸载」）

### 打包方式选型（本机工具链实测）

| 方案 | 本机可用性 | 卸载 | 代价 |
|---|---|---|---|
| **NSIS** | ✅ 已装 Unicode 3.x | 标准级 | **≈0，脚本已存在** |
| WiX v4 → MSI | ⚠️ 需联网 `dotnet tool install wix` | 标准级 | 全部逻辑重写成 XML |
| Inno Setup | ❌ 未装 | 标准级 | 需下载 + 重写脚本 |
| 7-Zip SFX | ❌ 7-Zip 未装 | 需自己写 | — |
| iexpress | ⚠️ 存在 | 弱 | 历史验证调不通 |

> 旧文档里「dotnet 只装了运行时、没有 SDK」**已过时**：本机现在有
> dotnet SDK 8.0.426，WiX 从不可行变成了「可行但要联网」。本次仍选 NSIS——
> 六项功能（控制面板卸载项 / 静默卸载 / 覆盖安装自动卸旧版 / 清理 / 提权 / 版本注入）
> 已全部实现，换方案等于重写。

### 同一个 bug 藏在三份清单里 ★★

`ai_face_processor_pc_plain.bundle`（23.3 MB，语义掩码必需）在**三处同时缺失**：
`install.bat` 的 copy 行、`build_nsis.py` 的 `PAYLOAD_PLAN`、`davincibeauty.nsi`
的 `File` 指令。缺它不是报错，是 `NamaSelfTest` 直接崩（rc=0xC0000005）。

**最有力的证据**：补齐后安装包体积 **82.7 MB → 105.0 MB**，差的正好是这个模型 ——
说明之前打出来的包从一开始就是残缺的，只是那条路径从没被验收覆盖过。

对策：三处一起静态校验（`acceptance.py` 工程组），并做了反向验证——
删掉任意一处，检查立刻 FAIL。以后改动资产清单必须同步三处。

### 顺手修的漂移

- `build_nsis.py` 的 `--version` 默认写死 `"1.4"` → 改为从
  `src\nama_engine.h` 的 `NAMA_BUILD_*` 宏自动读取，版本号回到单一来源。
- 删掉死代码 `collect_python()`（README 里对应的 `--no-python` 参数早已不存在）。
- `installer\README.md` 长期停在 v1.4 → 已修正，并**纳入文档版本号检查**
  （原先只查 4 份，漏了它，所以漂移才没人发现）。
- 检查文案里 `"4 份齐"` 是硬编码，加了第 5 份后仍显示 4 → 改成动态计数。

### UAC 的硬约束（自动化做不到）

安装包是 `requireAdministrator` manifest。在本会话里三种提权尝试全部失败：
PowerShell 工具被沙箱策略拦、Bash 调 powershell 被拦、直接 execve 返回
`Permission denied`（Windows 对 requireAdministrator 的标准行为）。

**结论：安装/卸载必须由人手动触发 UAC。** 已备 `installer\验证沙箱安装.bat`
（隔离 APPID `DaVinciBeauty.SandboxTest`，不碰正式装机），双击即可跑完
「安装 → 校验 → 卸载 → 校验」。

## 六点八、批处理编码：一个「编辑器里完全看不出」的坑 ★★

用户双击 `验证沙箱安装.bat` 后满屏报「`'xx'` 不是内部或外部命令」，
`rem` 注释行被当成命令执行。文件本身内容完全正确——问题全在编码/换行。

### 根因（三个独立因素，任缺一不可）

| 因素 | 机制 | 实测证据 |
|---|---|---|
| **UTF-8 中文** | cmd 按系统 ANSI(GBK) 解析 bat，UTF-8 三字节序列错位，吃掉行边界 | 最小复现：`echo [OK] 第一步` 被拆成 `'[OK]'` + `'二步'` 两条命令 |
| **`chcp 65001`** | 中途切代码页，后续行字节偏移全部错位 | `rem 中文注释…` 与下一行 `title …` 被拼成一条命令执行 |
| **裸 LF 换行** | 批处理只认 CRLF | 我自己生成的干跑副本 CRLF=0/裸LF=149，整份文件全乱 |

### 修复

全部 `.bat` 统一为 **GBK + CRLF + 无 `chcp`**（Windows 批处理的原生组合）。
涉及 `build.bat`、`install.bat`、`verify.bat`、`stage.bat`、`验证沙箱安装.bat`
及 `D:\插件` 根目录的快捷入口。转换后实测：

- `build.bat` 编译成功，中文提示正常，导出符号三件套不变
- `install.bat` 装机 13 项全 ok
- `验证沙箱安装.bat` 干跑返回 0，四段流程走通、中文完整

### 已固化为验收项

`acceptance.py` 工程组新增「批处理编码 GBK+CRLF+无chcp」，扫全仓 `.bat`。
反向验证过：放一个 UTF-8+chcp+LF 的样例进去立刻 FAIL，删掉恢复 PASS。

### 一个自食其果的插曲

第一轮诊断时我以为是编码问题，转了 GBK 却仍然崩——后来发现是**我生成的测试文件
用了裸 LF**（Python 文本模式默认把 `\r\n` 归一成 `\n`）。教训：
**处理 bat 文件必须二进制读写**，`open(p,'w',newline='')` 也不够，
要用 `open(p,'wb')` 写 `\r\n` 字节，否则 universal newlines 会在读的时候就吃掉 `\r`。

## 六点九、沙箱包与正式包同名，把正式安装刷了一遍 ★★

### 发生了什么

为了赶进度，我把正式包**直接覆盖到 `installer\DaVinciBeauty_Setup_v2.3.1.exe`**（历史归档）
（原来那里是 `--sandbox` 构建的沙箱包，两者同名同路径）。之后用户双击
`验证沙箱安装.bat`，发生的事是：

1. 脚本按路径自适应找到 `installer\DaVinciBeauty_Setup_v2.3.1.exe`（历史归档） —— 那是**正式包**
2. 静默安装 → 正式包按自己的 APPID 装进了 `DaVinciBeauty.ofx.bundle`（**真的装成功了**）
3. 脚本检查 `DaVinciBeautySandbox.ofx.bundle\Contents\Win64\*` → 全部不存在 → **9 项报「缺」**
4. 卸载阶段找 `Sandbox\Uninstall_*.exe` → 不存在 → 「找不到卸载器，跳过」
5. 最后检查「正式 bundle 完好未被波及」→ 显示 OK（其实刚被刷成了新版）

用户看到「9 项未通过」，以为是安装包坏了；实际上**装得完全正常**，
注册表卸载项 `DaVinciBeauty.BeautyOFX` v2.3.1 已在位，验收 24 PASS。

### 根因

**两类产物共用同一个文件名和路径，只能靠"谁最后构建"来区分身份。**
正式构建覆盖沙箱构建之后，验证脚本手里的包已经不是它以为的那个了。

### 修复

| 改动 | 作用 |
|---|---|
| `build_nsis.py --sandbox` 输出 `<name>_sandbox.exe` | 身份写进文件名，不再互相覆盖 |
| 验证脚本只匹配 `DaVinciBeauty_Setup_v*_sandbox.exe` | 拿不到沙箱包就明确报错，绝不装正式包 |
| 找不到包时 `FAILS=-1`，判定显示「未执行验证」 | 原来 `FAILS=0` 会显示「全部通过」，同样误导 |
| 安装包名空值时显示「(未找到沙箱包)」 | 原来是空字符串，看着像 bug |

### 教训

**误报比真错误更贵。** 一次错误的「9 项未通过」让用户以为安装包是坏的，
而实际装得好好的——排查成本远高于"根本没执行"。所以验证脚本在
**拿不到目标产物时，必须明确说"没验证"，而不是默默跑完给出一个看起来有意义的结论。**

**改共享产物前，先查还有谁在消费它。** 我覆盖 `installer\*.exe` 时没意识到
验证脚本还在同一目录按同名找沙箱包——两个组件对同一个路径有相反的预期，
冲突是必然的。

## 七、v2.3.0 详情

### 性能：带宽受限，不是算力受限

先证伪了一条路：锐化纵向 7-tap 的 c7 循环改 AVX2，4K 只快 **0.6 ms（0.7%）**。
4K 锐化每帧约 300MB 流量 ÷ 10GB/s ≈ 30 ms，与实测 31.4 ms 吻合 ⇒ **瓶颈是内存带宽，SIMD 无效**。

真正有效的是**减少流量**：取消锐化独立阶段1，`tmp`/`tmp7` 不再整帧落地，
改为每线程环形行缓存（t7 8 槽 / t3 4 槽，槽号 = 行号 % kSlots），按需计算。
计算式逐行照抄原实现 ⇒ **输出逐字节不变**。

| | 4K 全开单帧 |
|---|---|
| v2.2.3 | 78.6 ms |
| **v2.3.0** | **72.9 ms（−7.3%）** |

低于事前 −12~18% 的估计：行缓存约 400KB 未必全命中 L2，且线程块边界会重复算 7 行。

**v2.2.0 的 A+B+C**（多线程 + AVX2 SIMD + 合并格式转换）是更大的一次：
4K 全开 205 → 64.1 ms，锐化 153 → 31 ms。

### 参考数据点

- 720p 全开单帧 process **18.1 ms**（sdk 4.3 ms）
- 640×394 单帧 **33.2 ms**（sdk 19.6 ms，init 1560 ms 一次性）
- 1080×1920：8bit 13–16 ms / float32 18–20 ms

### 关于「1080p 掉到 16.6 帧」

**不是本插件的问题** —— 旧项目里其他片段还挂着别的 OFX 效果，换空项目单挂本插件即流畅。

误导性在于「1080p 和 4K 都是约 16fps」这个**与分辨率无关**的特征。真实机制：
每挂一个 CPU 型 OFX 插件就多一次显存↔主存同步往返，且插件在节点链上串行。
**插件数是加法，分辨率只是每项里的乘法因子** —— 固定延迟压倒分辨率项后两者就趋同。

⇒ 排查分水岭：**新建空项目 → 只挂待测效果 → 看 fps**。

## 八、验收方式

```
cd ..\06_验收工具
python acceptance.py                     # 退出码 0 = 通过
python acceptance.py --build --report x.txt   # 发版用：先编译再验
python baseline.py check                 # 只跑逐字节回归
python baseline.py update                # 有意改了输出后重新标定
```

覆盖 5 组 15 项：版本一致性、三表对齐、逐字节回归、mock host 契约、
bundle 完整性。详见 `06_验收工具\README.md`。

**两个判据上的坑**（都踩过）：

- 判「装的哪个版本」**不能靠 `NamaVersion()`** —— 它只编码 `(MAJOR<<16)|MINOR`，
  **PATCH 位被有意丢弃**以保持 ABI。只能靠二进制内嵌字符串 `"2.3.0"`。
- 判「装机 == 编译产物」**不能靠二进制 md5** —— MSVC 链接会在 PE 头写时间戳，
  同一份源码零改动重编，md5 就从 `50fea442` 变 `e3d64956`。
  改用**行为等价**：装机 DLL 跑同一组黄金用例，输出必须与编译 DLL 一致。

## 九、已知瑕疵（未修，不影响使用）

- `NamaSelfTest` 的错误消息里中文路径会乱码（UTF-8 被按本地码页输出），
  只在初始化失败时才会看到。
- SDK 运行时往 stderr 打 `face_dde.cc:202] data check fail`，是 NamaSDK 内部日志，
  不影响结果，过滤即可。
- **装机前必须先退出 Resolve**，否则被占用的 DLL 会被改名成 `.locked-<pid>` 留存。
  这是此前装机目录堆积 139 MB 垃圾的原因（已清理）。
  `install.bat` 优先用 `C:\ProgramData\DaVinciBeauty\engine` 的引擎，该目录不存在时
  fallback 到已安装 bundle 重新 stage。

## 十、待办

- **git 只有 1 次提交，v2.3.1 全部源码改动未提交**（用户选择暂不提交）。
  `src\nama_engine.cpp.bak_2_2_3` 是 v2.2.3 源码的唯一副本，**提交快照前不要删**。
- `_locked残留备份_20261027\`（139.3 MB，已删文件的备份）与
  `_改名迁移备份_20261027\`（138 MB，改名前的旧 bundle）确认无需回滚后可自行删除。
- 可选：Resolve 内 30 帧连续播放稳定性长测（未做）。
- 可选：美型三项拖拽转头不闪（v2.1.2 遗留）。
