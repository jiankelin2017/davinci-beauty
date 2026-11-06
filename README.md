# DaVinci Beauty 源码备份

> **当前构建：1.0.10.0（2026-11-07）** —— 修 1.0.9.0 的一个实装缺陷：那次承诺的「初始化日志打出适配器型号」在生产路径上从未生效，因为播报写在 `gpuSmoothInit()` 里而插件本体不调用它（走 `gpuSmoothRgbBand` 内的懒加载），1.0.9.0 的产物里连该字符串都不存在。本版把播报移到懒加载处并用 `g_initLogged` 保证只打一次。算法与画面未动，18/18 逐字节一致。1.0.9.0 `gpu_smooth` 出厂默认从 `off` 改为 `auto`（真机目视确认通过：1080×1920、磨皮 94、4928 帧零回落，无色块/断层、边缘无糊、亮度无偏）；1.0.8.0 GPU 磨皮把 `mapMs` 拆成「等 GPU」与「读 staging」，并把 ROI 外补齐前移到 Map 之前（实测重叠率 8~18%，未达预期），同批修正台架口径错误，据此推翻 1.0.7.0 的「GPU 比 CPU 慢」结论。1.0.7.0 GPU 磨皮搬运瘦身（掩码改双 R8、读回只拷输出矩形）；1.0.6.0 CPU 磨皮按 ROI 裁剪；1.0.5.0 修掉 BUG4 多实例串台；同批补齐 GPL-2.0-only 许可证与第三方声明。2026-11-06 起版本号改用四段式，下文「v3.0.x」是旧口径的历史记录，两者的对照表见 `01_ofx插件源码/PROGRESS.md`「版本号规则 v2」第 (4) 节。

- 备份时间：2026-10-14 22:36（CST）
- 最后修订：**2026-11-07（1.0.10.0：修 1.0.9.0 的一个实装缺陷——承诺的「初始化日志打出适配器
  型号」在生产路径从未生效。播报原写在 `gpuSmoothInit()`，而插件本体不调用它（走
  `gpuSmoothRgbBand` 内的懒加载），1.0.9.0 的产物里连 `GPU 磨皮后端已就绪` 这个字符串都不存在
  （按字节搜索 0 次，而同文件的 `GPU 磨皮[` 有 3 次）。本版把播报移到懒加载处、用 `g_initLogged`
  保证只打一次，设备重建后允许再播一次；ini 注释同步改成「首次进入写一行，之后首 3 帧与每 60 帧
  写一行」。只改日志与注释，算法、默认值、画面未动，18/18 逐字节一致、台架 5/5。详见
  `PROGRESS.md` 1.0.10.0 节）**
- 上一版（1.0.9.0，2026-11-07，原文保留）：`gpu_smooth` 出厂默认从 `off` 改为 `auto`，只改默认值、
  不改算法，修订位 +1。依据是当天真机目视确认——1080×1920 真实素材、磨皮 94，累计 4928 帧
  零回落、均合计 4.55ms，无色块/断层、发丝与衣领无糊、亮度无偏。同批加适配器型号日志
  （`GPU 磨皮后端已就绪：<厂商型号> [厂商 0x????，显存 x.x GB]`，**该行实际未生效，已被 1.0.10.0 修好**）
  与特性等级显式检查，DX10 老卡提前挡掉而不是等到编译着色器才失败。**改动只对「首次生成 ini」
  的用户生效，存量 ini 里写死的 `off` 不会被静默改**。逐字节 18/18、台架 5/5。
  同日追加：台架补了一组**运动序列判据**（帧间位移，专治「用错帧」这类缺陷——
  静态图对它是瞎的），`gpu_move_bench` 现在同时跑 5 例静态 + 6 帧运动；
  判据灵敏度实测 602 万字节不一致、maxdiff 255。该改动只动台架，插件二进制未变。
  详见 `PROGRESS.md` 1.0.9.0 节 (7)。同日晚再追加 (8)：三段 23.976 fps 真实素材的逐帧
  位移实测（0.42 / 0.72 / 2.53 px），据此把 2c 的「残影会非常明显」修正为
  「慢运动素材亚像素不可辨、仅快节奏素材可能显形」，并写明目视对比测不到 ROI 交界类误差）**
- 上一版（1.0.8.0，2026-11-07，原文保留）：GPU 磨皮计时拆分 + fill 前移。`GpuSmoothTiming`
  新增 `mapWaitMs` / `mapCopyMs`，把「等 GPU 同步点」与「CPU 读 staging」分开计量；
  ROI 外补齐从 Map 之后挪到 `CopySubresourceRegion` 之后、Map 之前，实测与 GPU 的
  重叠率只有 8~18%，远低于预期，数据与复测要求见 `PROGRESS.md` 1.0.8.0 节 (4)。
  同批修正台架口径：`gpu_move_bench` 的 CPU 基准此前多除了一次 `reps`，1.0.7.0 记录的
  CPU 4.18ms 实际是 45.98ms；修正后 GPU 快于 CPU 2.4~4.7 倍，「GPU 比 CPU 慢 2~3 倍」
  的旧结论反转。5 画幅逐字节一致。同批还把 `等GPU/拷` 两个字段接进真机诊断日志，
  并清掉 6 处随发布清单外发的本机绝对路径（详见 `PROGRESS.md` 1.0.8.0 节 (2)(6)）
- 更早（1.0.7.0，2026-11-06，原文保留）：GPU 磨皮搬运瘦身 —— ① 掩码与保护图此前挤在
  同一张 `RGBA8` 里（R=掩码、G=保护强度），HLSL 侧要 `Load` 一个 `uint4` 再拆通道，
  上传侧还要逐像素打包；本版拆成两张 `R8_UINT`（`t1` 掩码、`t2` 保护），上传改成整行
  `memcpy`，省掉打包循环与 4 倍无效带宽；② 读回此前用 `CopyResource` 拷整帧到 staging，
  4K 下这一趟就白搬 33 MB，本版改 `CopySubresourceRegion` + `D3D11_BOX`，只拷输出矩形。
  1080p 大脸 3.41→2.63 ms（-22%），4K 小脸 6.49→5.73 ms（-12%），5 画幅逐字节一致。
  同批 1.0.6.0 让 CPU 磨皮真正按 ROI 裁块（`surface3RgbBand` 的 `roi` 参数此前收了没用），
  提速 1.5~3.4×、画面逐字节不变；1.0.5.0 修 BUG4 多实例串台（`SlotScope` 无锁、
  `slotOf()` 返回悬垂引用）**
  2026-11-03（v3.0.9：修「遮罩比额头还高一大截」—— ① 掩码的脖子通道改锚到
  `yChin = flipY ? y0 : y1`：Resolve 的帧是 bottom-up，旧写法把 0.65 脸高的「脖子」
  整块铺到了发际线以上，实测改动区从 y[21] 收到 y[136]（脸框上沿 146）；② 五官立体的
  8 个椭圆、去黑眼圈的 dy、去法令纹的 dy/rot 全部乘 `sgn = imgBottomUp_ ? -1 : +1`
  （此前沿 y 镜像：黑眼圈落在上眼睑、法令纹落在眉弓）；③ `estimateSkinCenter` 的采样带
  在 bottom-up 下换成缓冲行 0.20~0.60（此前采到额头与头发，肤色中心被拉偏、掩码变薄）；
  ④ 修好本整合包里跑不起来的构建：`build.ps1::Find-EngineDir` 只认 `07_*`，目录已改名
  `02_引擎资产`，改为按 `onnxruntime.dll` 探测。top-down 路径逐字节不变：18/18 基线 md5
  全部一致）**
  v3.0.8：工程验收 + 性能优化 —— ① 编译链路改为 PowerShell 驱动：
  旧 `build.bat` 把引擎 include 路径以 GBK 字节写死在批处理里，控制台代码页一变就报
  「打不开 onnxruntime_c_api.h」，现在 build.bat 只是调用 build.ps1 的薄壳，工具链与
  中文路径都由 PowerShell 以 UTF-16 处理；链接加 /Brepro，同源码重编逐字节相同；② install.bat 的「是否管理员」判据从
  `net session` 改为「能否创建目标目录」，受限管理员令牌下不再误报；③ 8bit 且无行填充
  时引擎直接读写宿主图像，省掉两次全帧拷贝，1080p 实测 24.5→20.5 ms（-16%）；
  ④ mock host 新增「14 个滑块逐个独奏」用例（23 项），补上立体/亮眼/去眼袋/去法令纹/
  祛斑/红润从未单独测过的空白；⑤ 清掉 v2.x 旧版闭源 SDK 遗留的 20 个死成员与死方法声明，
  修掉 `faceScore_` 的 double→float 截断告警与 `version()` 的可变静态串悬空风险。
  v3.0.7：加强磨皮强度并放大四组档位差异 —— 磨皮引入「强度总增益」dst=src+(dst−src)×1.8（满档人脸区平均改动 1.01→1.82 灰阶、峰值 13→23），高斯档半径 ×2、弱磨皮 0.5→0.30、清晰/精细回填增益 4.0/5.5 + 中频 0.45/0.75、轻磨皮 0.55→0.40；v3.0.6：修「磨皮算法 / 磨皮模式」两个下拉框的面板侧缺陷 —— 参数读取里的 SDK 时代重映射把「双边滤波」和「高斯」对调，且高斯档下回填强度恒 0 使磨皮模式四档失效；v3.0.5：修美形三项的 y 方向 —— 瘦脸/下巴此前把「视觉下方」算到了额头发际线，只有径向各向同性的大眼不受影响；并把「磨皮算法」三档与「磨皮模式」四档真正接线；v3.0.4：修正帧行序 —— OFX/Resolve 给的是 bottom-up 帧，检测此前按 top-down 读，脸是上下颠倒的，真机 1080×1920 帧实测 score 0.2519/0 脸 → 0.9333/1 脸；v3.0.3：修掉三个「假故障」——离线自测夹具把文件路径当图像、异步检测在按需渲染宿主里永久透传、参数句柄失败静默给 0；v3.0.2 修正人脸检测预处理为 raw 0-255；v3.0.0 彻底移除旧版闭源商业 SDK，改用自研纯 CPU 美颜引擎 + ONNX 人脸检测，支持 GPU 自动检测）**
- 备份内容：**源码、文档、脚本**。二进制运行时/模型资产、构建产物（`out/`、`obj/`、
  `bundle/`、`__pycache__`）与本地归档都不入库，不计入清单；`FILELIST.txt` 只列
  仓库内的源码/文档/脚本，每个文件带 SHA256
- 文件数：**149 个，合计约 3.34 MB**（含输入源图 `input_face.png`；`probe_in.raw`
  等大体积素材不计入。以 `FILELIST.txt` 头部为准；由 `gen_filelist.py` 生成，
  它只收录 git 已跟踪的文件，改完源码重跑一次即可）
- 本地回滚副本（`src/_backup_before_*`）**不**计入清单，只作修复前留档


## 许可（License）

本项目以 **GNU General Public License version 2（仅第 2 版，GPL-2.0-only）** 发布。

- 许可全文：根目录 [`LICENSE`](LICENSE)
- 随包分发的第三方组件（ONNX Runtime 1.20、YuNet 人脸模型、OpenFX 头文件）的
  版权与许可全文：[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)

两点需要说清楚：

1. **「仅第 2 版」不是笔误**。文件头没有写 "or (at your option) any later version"，
   所以下游不能自行按 GPLv3 使用。要改成「或更高版本」，只需改本段声明，
   `LICENSE` 正文（GPLv2 官方文本）一个字都不用动。
2. **GPL-2.0 与 Apache-2.0 不兼容**，与 MIT、BSD-3-Clause 兼容。现有三个第三方组件
   都是 MIT / BSD-3，所以这个组合是成立的；将来若引入 Apache-2.0 组件，
   整个授权结构必须重做。

## 目录形态（2026-10-31 整合后）

本仓库就是一个完整工作区：源码 + 文档 + 工具都在 `01_ofx插件源码 / 03_测试夹具 /
04_验收工具 / 05_安装包 / 02_引擎资产` 下，根目录放清单与交付说明。

- **仓库里有什么**：源码、测试脚本、验收工具、文档，以及 `02_引擎资产/stage.bat`
  ——「把运行时资产放回 ProgramData」的入口脚本。
- **仓库里没有什么**（本地保留、不入库）：推理运行时 DLL 与人脸检测模型（体积大，
  从交付包或已装机的 bundle 取回）、构建产物（`out/ obj/ bundle/`）、安装包 exe、
  本地归档 `_archive/`。
- 根目录的 `MANIFEST.txt` 是 v3.0.8 交付时由 `package.py` 生成的留档（覆盖含二进制的
  交付子集）；1.0.7.0 的安装包 exe 与 `验收报告.txt` 已重打。这些产物都不入库，
  仓库内的完整性校验看 `FILELIST.txt` —— 它只列已跟踪的源码/文档。
- `package.py` 已加防呆：目标目录 == 本仓库时直接拒绝，避免把 `.git` 哈希进清单，
  或 `--force` 把仓库删掉重来。

## 当前状态（2026-11-07，v1.0.10.0）

**v3.0.0 已把引擎从旧版闭源商业 SDK 整体替换为自研的纯 CPU 美颜引擎
（CoreEngine，对应引擎 3.1.39），人脸检测用自带的 ONNX 模型，并在引擎内自动检测
GPU（仅 Windows）：把 `onnxruntime.dll` 换成带 GPU 支持的构建版，
人脸推理就自动走 GPU，否则自动退回 CPU。公开接口不变，宿主（ofx_plugin.cpp）零改动。**

**1.0.5.0~1.0.10.0 是六批「看不见画面变化」的工程改动，用途都是让它在长时间、
多实例的真实工程里不出事、不空转**：① 1.0.5.0 修 BUG4 —— 多实例（同一条时间线上
挂两个 Beauty）第 0 帧互相串台，根因是 `SlotScope` 的槽位表**无锁**，且 `slotOf()`
把表里的指针取出来就返回、调用方拿着可能已被别的线程清掉的对象继续用；现改为按 slot
持有 `std::recursive_mutex`，诊断输出改 `thread_local`；② 1.0.6.0 让 CPU 磨皮真正按
ROI 裁块 —— `surface3RgbBand` 的 `roi` 参数此前收了但从头到尾没用，整个画幅都在算；
③ 1.0.7.0 砍掉 GPU 磨皮的无效搬运（掩码拆表 + 读回只拷输出矩形）；④ 1.0.8.0 把磨皮的
读回耗时拆成「等 GPU」与「读 staging」两项，并把 ROI 外补齐前移到 Map 之前；
⑤ 1.0.9.0 把 `gpu_smooth` 默认值改成 `auto`，并尝试让初始化日志打出适配器型号；
⑥ 1.0.10.0 把 ⑤ 那行日志真正接通（此前播报点所在的 `gpuSmoothInit()` 在生产路径无人调用）。
六批的逐字节基线都是 18/18 或 5/5 一致。

> **1.0.9.0 改默认值的边界**：只对「首次生成 ini」与「ini 里没写 `gpu_smooth`」的用户生效。
> ini 一旦落盘那一行就固定了，存量用户不会被静默改配置。选卡只排除 WARP 软件渲染器、
> **不按厂商过滤**，AMD 与 Intel 核显同样会走 GPU，而这两家没有实测数据——所以日志里
> 现在带适配器型号与厂商 ID，出问题时先看那一行。**这一行的有效性由 1.0.10.0 保证**：
> 1.0.9.0 里它其实从未打印过，详见 `PROGRESS.md` 1.0.10.0 节。

> **1.0.8.0 更正了 1.0.7.0 的一个错误结论**：此前记录的「CPU 比 GPU 快 2~3 倍」
> 源于台架把 CPU 基准多除了一次 `reps`（`1.18` / `4.18` 实际是 `12.98` / `45.98`）。
> 修正后同一台机重测，GPU 反而快 2.4~4.7 倍，`gpu_smooth=auto` 的默认值无须调整。
> 推导与实测见 `PROGRESS.md` 1.0.8.0 节 (2)(3)。

**v3.0.9 修的是「方向」而不是「强度」：掩码的脖子通道与五官局部五项此前按 top-down
使用 `f.y`，而 Resolve 的帧是 bottom-up —— 脖子上半截铺到了发际线以上（用户看到的
「遮罩比额头还高一大截」），立体/黑眼圈/法令纹则沿 y 镜像（黑眼圈落在上眼睑）。
修后 bottom-up 与 top-down 的逐项改动区 bbox 完全一致。**
变更细节见 `01_ofx插件源码/README.md` 的「v3.0.9 变更」与 `PROGRESS.md`。

演进主线：v2.0 把**引擎层整体替换为自研引擎 3.0.19**（宿主层保持 v1.51）；
之后 v2.2.0 做性能 3.2×、v2.2.3 整理面板与默认档、v2.3.0 再压内存流量、v2.3.1 统一更名
DaVinci Beauty；v3.0.0 进一步**脱离旧版闭源 SDK**，改用完全自研、可自由分发的引擎。
决策依据、标定数据、踩坑记录见 `01_ofx插件源码/PROGRESS.md`（精简版），
完整历史流水账在 `_archive/PROGRESS_ARCHIVE_至v2.3.0.md`（本地留档，不入库）。

| 项 | 状态 |
|---|---|
| 编译 | 通过（`build.ps1`，`build.bat` 为薄壳；产物 324608 B，md5 `a226fea59196fc1582ea5acb007dee42`） |
| 装机 | **已装**（`C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle`），行为等价校验通过 |
| 无宿主回归 | **23/23 PASS**（含 14 个滑块逐个独奏） |
| 逐字节回归 | **18/18**（`04_验收工具/baseline.py`，top-down 路径零改动） |
| 一键验收 | **27 PASS / 0 FAIL / 0 WARN**（`04_验收工具/acceptance.py`，本快照 27 项） |
| 帧行序语义 | **bottom-up 与 top-down 逐项 bbox 一致**（掩码/立体/眼袋/法令纹四项） |
| 性能 | 1080p 8bit 单帧 24.5→20.5 ms（直通优化，A/B 各 5 轮取最小值） |
| 整合包 | **本仓库即整合包**（v1.0.10.0，工作区 + 交付留档；对外副本用 `package.py` 另生成） |
| 安装包 | **已构建** `05_安装包/build/DaVinciBeauty_Setup_v1.0.10.0.exe`（4.6 MB，含卸载器与许可文件；`05_安装包/build_nsis.py` 可重建） |
| 装机件 | **已安装** v1.0.10.0，`DaVinciBeauty.ofx` md5 `a226fea59196fc1582ea5acb007dee42`，与 `out\` 产物一致 |
| Resolve 内实测 | **已做**：① CPU 路径 1080×1920 连续 192 次 render call，`st=0`/`wrote=1`，正常卸载无崩溃；② GPU 路径（`gpu_smooth=auto`）累计 4928 帧、均合计 4.55ms、零回落；③ 目视确认无色块/断层、发丝与衣领无糊、亮度无偏 |
| 仓库体积 | 约 2.5 MB（只有源码/文档/脚本；运行时二进制、模型与本地归档都不入库） |

回滚方式：源码已纳入 git，回滚请用 git 历史
（`git log` / `git checkout <commit> -- 01_ofx插件源码/src/` 等）。
历史上的 `_backup_before_v2.0/` 临时回滚副本已在本次整理中删除（发版确认后按约定清理）。

## 交付物

> **下次继续开发请先看「`开发指南.md`」** —— 里面有改动流程、目录地图、四条硬约束。

| 文件 | 说明 |
|---|---|
| `开发指南.md` | **开发入口**：5 步工作流、目录地图、换机恢复、外部依赖 |
| `01_ofx插件源码/` | OFX 插件 C++ 源码（当前主线） |
| `03_测试夹具/` | 无宿主回归测试与性能基准 |
| `02_引擎资产/` | 引擎运行时资产目录：仓库内只有 `stage.bat`（把 `onnxruntime.dll` + `yunet.onnx` 放回 `ProgramData` 的入口），二进制本身不入库 |
| `04_验收工具/` | 一键验收、逐字节回归基线、性能 A/B 测量 |
| `_archive/03_脚本版Python源码/` | 早期 Python 脚本版（已不再维护；本地留档，不入库） |
| `_archive/04_脚本版Lua源码/` | 早期 Lua 脚本版（已移出菜单；本地留档，不入库） |
| `05_安装包/` | NSIS 安装包构建脚本 |
| `FILELIST.txt` | 文件清单（SHA256 + 大小） |
| `gen_filelist.py` | 清单生成器 |
| `_archive/CODE_REVIEW.md` | v1.3 时期的代码审查记录（B1–B9 / G1–G4；本地留档，不入库） |

## 目录说明

### 01_ofx插件源码 —— OFX 插件的 C++ 源码（当前主线）

| 路径 | 说明 |
|---|---|
| `src/ofx_plugin.cpp` | OFX 协议实现主体（describe / describeInContext / render / IsIdentity / GetClipPreferences） |
| `src/nama_engine.cpp` / `.h` | 自研引擎适配层：把 OFX 宿主请求转给 CoreEngine（纯 CPU 美颜 + ONNX 人脸检测），公开接口与旧版完全一致 |
| `sdk/include/` | OFX 官方头文件（第三方，编译必需） |
| `examples/` | 参考用 OFX 示例（不参与编译） |
| `build.bat` | 编译脚本（MSVC，输出 `out\DaVinciBeauty.ofx`） |
| `install.bat` | 打包 + 安装到 `%COMMONPROGRAMFILES%\OFX\Plugins` |
| `PROGRESS.md` | **精简版**：当前状态、设计决策、宿主行为坑、GT 数据、版本索引 |
| `_archive/PROGRESS_ARCHIVE_至v2.3.0.md` | 完整历史流水账（v1.4 → v2.3.0），需要追查过程时再翻（本地留档，不入库） |
| `README.md` | 插件设计、参数、性能、自测说明 |

**重新编译**：双击 `build.bat`，再跑 `install.bat`。
编译环境是 MSVC（沙箱拦截 `reg.exe`，`vcvars64.bat` 用不了，需手动设 `INCLUDE`/`LIB`/`PATH`）。
本机验证版本：MSVC 14.51.36231 + Windows SDK 10.0.26100.0。

### 03_测试夹具 —— 无宿主回归测试

来自早期的探针工作目录（只收录 .py / .lua / .json / .txt；raw 与 mp4 素材未收录）

- `mock_host.py` —— 模拟最小 OFX 宿主，直接驱动插件跑
  `describe → describeInContext → createInstance → render`，验证协议行为与像素结果。
  共 **19 项**断言，支持 `--bench` 跑性能基准。
- `bench_whiten.py` —— 美白 CPU 掩码成本基准（同一帧只切 `color_level=0/100`）。
- `selftest_ofx.py` —— 不经 OFX 协议，直接调 `NamaSelfTest` 跑
  「加载引擎 → 初始化 → 渲染 → 落盘」（自研引擎，无需 GL 上下文）。
- `probe_nama.py` / `probe_*.py` / `sweep_*.py` / `diag_*.py` —— 各阶段探针与排查脚本（保留过程证据）。

运行基准时需自备原始帧，可通过 `NB_BENCH_RAW`（+`NB_BENCH_W/H`）指定；
也可用 `NAMA_OFX_DLL` 指向尚未安装的插件二进制，用 `NAMA_ENGINE_DIR` 指向引擎资源目录。

### 04_验收工具 —— 发版前的一键验收（2026-10-27 新增）

| 路径 | 说明 |
|---|---|
| `acceptance.py` | **一键验收**：版本一致性 / 三表对齐 / 逐字节回归 / mock host 契约 / bundle 完整性，共 15 项，退出码即结论 |
| `verify.bat` | 双击运行版 |
| `baseline.py` | 逐字节回归基线：生成 / 比对。raw 不入库，只留 md5 清单 |
| `perf_ab.py` | 单 DLL 性能测量，供 A/B 交替比较用 |
| `baseline_manifest.txt` | 黄金基线 md5 清单（15 用例） |
| `input_face.png` | 自带输入源图，仓库自包含 |

```
python acceptance.py                    # 退出码 0 = 通过
python acceptance.py --build --report x.txt   # 发版用
python baseline.py check                # 只跑逐字节回归
```

⚠️ 两个判据上的坑：`NamaVersion()` **不含 PATCH 位**，判版本要靠二进制内嵌字符串；
MSVC 构建**不逐字节可复现**（PE 时间戳），判装机同步要用行为等价而非 md5。

### 03_脚本版Python源码 —— 早期的 Python 脚本版（已不再维护，已归档至 _archive/）

来自 `C:\ProgramData\DaVinciBeauty\`：`nama_worker.py`（Worker）、`nama_live.py`（实时预览）、
`nama_ui.py`（面板）、`DaVinci Beauty_美颜_cli.py`（命令行）、`config.json`、`tests/`。

### 04_脚本版Lua源码 —— 早期 Lua 脚本版（已移出菜单，仅作留档，已归档至 _archive/）

- `DaVinci Beauty 美颜/DaVinci Beauty 美颜.lua` —— Fusion 脚本菜单入口
- `旧备份/` —— 历史版本快照

### 05_安装包 —— 安装包构建（NSIS / Python）

- `davincibeauty.nsi` —— NSIS 安装 / 卸载脚本，由 `build_nsis.py` 调用 makensis 编译
- `build_nsis.py` —— 单文件 exe 安装包构建器（版本号自动从 `src/nama_engine.h` 读取）
- `davincibeauty_setup.py` —— 无 GUI / 脚本化场景的等价安装实现与调试工具
- `README.md` —— 安装器说明

### 根目录

- `FILELIST.txt` —— 本清单（每个文件的 SHA256）
- `gen_filelist.py` —— 清单生成器，改完源码跑 `python gen_filelist.py` 即可重算
- `.gitignore` —— 忽略 `out/`、`obj/`、`bundle/`、`__pycache__/`、`*.pyc`、`*.locked-*`
- `_archive/CODE_REVIEW.md` —— v1.3 时期的代码审查记录（B1–B9 / G1–G4；本地留档，不入库）
- `说明.txt` —— 面向使用者的简短交付说明

## v1.4 验收摘要（2026-10-21）

详见 `01_ofx插件源码/PROGRESS.md` 与 `01_ofx插件源码/README.md`。

- 回归：`mock_host.py` **19/19 PASS**；`selftest_ofx.py` 渲染 / 全 0 透传均 PASS。
- 默认值：首次拖入为「磨皮 20、其余七项 0、双边滤波、只磨皮肤」。
- 性能：1080p 美白掩码 **38.2 ms → 4.5 ms**（8.4 倍）；8bit 端到端仍为 13–16 ms 量级。
- 修复：整数提亮表少除 255（人脸区平均差 67 → 6.2）、每帧 stderr 调试输出、
  中文路径下 `NamaSelfTest` 加载引擎失败、静默卸载退出码 2 → 0。
- 安装：本机构建 `DaVinciBeauty_Setup_v1.4.exe`（历史版本产物，当前版本见上文「交付物」），
  静默安装 / 静默卸载 / 重装全部通过；
  安装后二进制 SHA256 与 `out\DaVinciBeauty.ofx` 一致。

## 未收录的内容（如需完整环境请另行备份）

| 内容 | 位置 | 为什么没收 |
|---|---|---|
| 自研引擎运行时资产 | `02_引擎资产\`（`onnxruntime.dll` 约 11MB、`yunet.onnx` 约 0.2MB） | 二进制运行时资产，体积大，不入库；从交付包或已装机的 bundle 取回即可；如需启用 GPU，换成带 GPU 支持的构建版 |
| 已安装的插件 | `C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\` | 由 `install.bat` / 安装包生成，可重建 |
| 编译产物 | `01_ofx插件源码\obj\`、`out\`、`bundle\` | 可重建（`bundle\` 是 `install.bat` 的暂存副本） |
| 运行日志与任务队列 | `C:\ProgramData\DaVinciBeauty\logs\`、`jobs\`、`live\` | 运行时数据 |

## 恢复方法

1. 把 `01_ofx插件源码` 放回任意目录，并在 `02_引擎资产\` 放好引擎资源
   （`onnxruntime.dll` + `yunet.onnx`，从交付包或已装机的 bundle 取回），
   或直接用 `bundle\...\Contents\Win64`、已安装的 bundle；
2. 运行 `build.bat` 编译，再运行 `install.bat` 安装；
3. 启动 DaVinci Resolve，效果库 → OpenFX → DaVinci Beauty → 「DaVinci Beauty 美颜」。

> 注意：改了插件之后**必须重启 Resolve**。
> OFX 缓存 `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`
> 按「mtime + size」判断插件是否失效。
