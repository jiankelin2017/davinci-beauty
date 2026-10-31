# DaVinci Beauty 源码备份

- 备份时间：2026-10-14 22:36（CST）
- 最后修订：**2026-10-31（v3.0.8：工程验收 + 性能优化 —— ① 编译链路改为 PowerShell 驱动：
  旧 `build.bat` 把引擎 include 路径以 GBK 字节写死在批处理里，控制台代码页一变就报
  「打不开 onnxruntime_c_api.h」，现在 build.bat 只是调用 build.ps1 的薄壳，工具链与
  中文路径都由 PowerShell 以 UTF-16 处理；链接加 /Brepro，同源码重编逐字节相同；② install.bat 的「是否管理员」判据从
  `net session` 改为「能否创建目标目录」，受限管理员令牌下不再误报；③ 8bit 且无行填充
  时引擎直接读写宿主图像，省掉两次全帧拷贝，1080p 实测 24.5→20.5 ms（-16%）；
  ④ mock host 新增「14 个滑块逐个独奏」用例（23 项），补上立体/亮眼/去眼袋/去法令纹/
  祛斑/红润从未单独测过的空白；⑤ 清掉 v2.x NamaSDK 遗留的 20 个死成员与死方法声明，
  修掉 `faceScore_` 的 double→float 截断告警与 `version()` 的可变静态串悬空风险。
  v3.0.7：加强磨皮强度并放大四组档位差异 —— 磨皮引入「强度总增益」dst=src+(dst−src)×1.8（满档人脸区平均改动 1.01→1.82 灰阶、峰值 13→23），高斯档半径 ×2、弱磨皮 0.5→0.30、清晰/精细回填增益 4.0/5.5 + 中频 0.45/0.75、轻磨皮 0.55→0.40；v3.0.6：修「磨皮算法 / 磨皮模式」两个下拉框的面板侧缺陷 —— 参数读取里的 SDK 时代重映射把「双边滤波」和「高斯」对调，且高斯档下回填强度恒 0 使磨皮模式四档失效；v3.0.5：修美形三项的 y 方向 —— 瘦脸/下巴此前把「视觉下方」算到了额头发际线，只有径向各向同性的大眼不受影响；并把「磨皮算法」三档与「磨皮模式」四档真正接线；v3.0.4：修正帧行序 —— OFX/Resolve 给的是 bottom-up 帧，检测此前按 top-down 读，脸是上下颠倒的，真机 1080×1920 帧实测 score 0.2519/0 脸 → 0.9333/1 脸；v3.0.3：修掉三个「假故障」——离线自测夹具把文件路径当图像、异步检测在按需渲染宿主里永久透传、参数句柄失败静默给 0；v3.0.2 修正人脸检测预处理为 raw 0-255；v3.0.0 彻底移除 FaceUnity NamaSDK，改用 OBS 自研纯 CPU 美颜引擎 + ONNX 人脸检测，支持 GPU 自动检测）**
- 备份内容：**源码、文档、脚本**。`out/`、`obj/`、`bundle/`、`__pycache__` 是本地
  构建产物，不计入清单；`FILELIST.txt` 只列源码/文档，每个文件带 SHA256
- 文件数：**140 个，合计约 3.25 MB**（含测试输入帧 `probe_in.raw` / `input_face.png`；
  03/04/05 历史目录与探针输出不计入。以 `FILELIST.txt` 头部为准；由
  `gen_filelist.py` 生成，改完源码重跑一次即可）
- 本地回滚副本（`src/_backup_before_*`）**不**计入清单，只作修复前留档

## 目录形态（2026-10-31 整合后）

现在**只有一个目录** `D:\插件\DaVinciBeauty_整合_20261031\`，工作区与交付包合一：

- **工作区形态**（本目录）：主线源码 + 文档 + 工具 + 引擎资产，外加 `03/04/05` 历史目录、
  `_dbgtest`、构建产物（`out/ obj/ bundle/`）与 `.git`（442 个跟踪文件）。
- **交付包形态**：`04_验收工具\package.py` 用 `--name` 生成一个**独立目录**，只含主线
  （`01_ofx插件源码 / 03_测试夹具 / 04_验收工具 / 02_引擎资产 / 05_安装包` + 文档），
  **不含** `03/04/05`、`_dbgtest`、构建产物与 `.git`。
- 本目录根的 `MANIFEST.txt`、`安装说明.txt`、`stage_engine.bat`、`DaVinciBeauty_Setup_v3.0.8.exe`
  是 v3.0.8 那次交付的留档。`MANIFEST.txt` 只覆盖**交付子集**（140 个文件），
  不覆盖 `03/04/05`、`.git`、构建产物；**全仓校验看 `FILELIST.txt` 与 `git status`**。
- `package.py` 已加防呆：目标目录 == 本仓库时直接拒绝，避免把 `.git`（148 MB）哈希进清单，
  或 `--force` 把仓库删掉重来。

## 当前状态（2026-10-31，v3.0.8）

**v3.0.0 已把引擎从闭源 FaceUnity NamaSDK 整体替换为 OBS 自研的纯 CPU 美颜引擎
（CoreEngine，对应 OBS 3.1.39），人脸检测用自带 ONNX YuNet 模型，并在引擎内自动检测
GPU（仅 Windows DirectML）：把 `onnxruntime.dll` 换成 `onnxruntime-directml` 构建版，
人脸推理就自动走 GPU，否则自动退回 CPU。公开接口不变，宿主（ofx_plugin.cpp）零改动。**

**v3.0.8 是交付前的工程收口版：画面算法零改动（18/18 逐字节不变），修好构建链路
（中文路径 + 代码页导致的编译失败）、装机权限判据、每帧两次多余的 8.3MB 拷贝
（1080p 24.5→20.5 ms），并把 14 个滑块全部纳入单项接线测试。**
变更细节见 `01_ofx插件源码/README.md` 的「v3.0.8 变更」与 `PROGRESS.md`。

演进主线：v2.0 把**引擎层整体替换为 OBS 美颜插件 3.0.19**（宿主层保持 v1.51）；
之后 v2.2.0 做性能 3.2×、v2.2.3 整理面板与默认档、v2.3.0 再压内存流量、v2.3.1 统一更名
DaVinci Beauty；v3.0.0 进一步**脱离 NamaSDK**，改用完全自研、可自由分发的引擎。
决策依据、标定数据、踩坑记录见 `01_ofx插件源码/PROGRESS.md`（精简版），
完整历史流水账在 `_archive/PROGRESS_ARCHIVE_至v2.3.0.md`。

| 项 | 状态 |
|---|---|
| 编译 | 通过（`build.ps1`，`build.bat` 为薄壳；产物 252928 B，md5 `40df78a2dc7bce61ed6f8f5e027114fe`） |
| 装机 | **已装**（`C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle`），行为等价校验通过 |
| 无宿主回归 | **23/23 PASS**（含 14 个滑块逐个独奏） |
| 逐字节回归 | **18/18**（`04_验收工具/baseline.py`） |
| 一键验收 | **25 PASS / 0 FAIL / 0 WARN**（`04_验收工具/acceptance.py`） |
| 性能 | 1080p 8bit 单帧 24.5→20.5 ms（直通优化，A/B 各 5 轮取最小值） |
| 整合包 | **本目录即整合包**：`D:\插件\DaVinciBeauty_整合_20261031\`（v3.0.8，工作区 + 历史 + 交付留档；对外副本用 `package.py` 另生成） |
| 安装包 | **已构建** `DaVinciBeauty_Setup_v3.0.8.exe`（4.6 MB，含卸载器；`05_安装包/build_nsis.py` 可重建） |
| Resolve 内实测 | **已做**：空项目 1080p 播放稳定流畅 |
| 仓库体积 | 384 MB（含 `.git` 148 MB、旧安装包 v2.3.1 105 MB、`05_解密模型数据` 62 MB、构建产物 25 MB；删掉这些可回到 ~45 MB） |

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
| `02_引擎资产/` | 自研引擎运行时资产：ONNX Runtime（CPU 构建版 `onnxruntime.dll`）+ 人脸检测模型 `yunet.onnx`（含 `stage.bat`） |
| `04_验收工具/` | 一键验收、逐字节回归基线、性能 A/B 测量 |
| `_archive/03_脚本版Python源码/` | 早期 Python 脚本版（已不再维护，留档） |
| `_archive/04_脚本版Lua源码/` | 早期 Lua 脚本版（已移出菜单，留档） |
| `05_安装包/` | NSIS 安装包构建脚本 |
| `FILELIST.txt` | 文件清单（SHA256 + 大小） |
| `gen_filelist.py` | 清单生成器 |
| `_archive/CODE_REVIEW.md` | v1.3 时期的代码审查记录（B1–B9 / G1–G4） |

## 目录说明

### 01_ofx插件源码 —— OFX 插件的 C++ 源码（当前主线）

| 路径 | 说明 |
|---|---|
| `src/ofx_plugin.cpp` | OFX 协议实现主体（describe / describeInContext / render / IsIdentity / GetClipPreferences） |
| `src/nama_engine.cpp` / `.h` | 自研引擎适配层：把 OFX 宿主请求转给 OBS CoreEngine（纯 CPU 美颜 + ONNX 人脸检测），公开接口与旧 NamaSDK 版完全一致 |
| `sdk/include/` | OFX 官方头文件（第三方，编译必需） |
| `examples/` | 参考用 OFX 示例（不参与编译） |
| `build.bat` | 编译脚本（MSVC，输出 `out\DaVinciBeauty.ofx`） |
| `install.bat` | 打包 + 安装到 `%COMMONPROGRAMFILES%\OFX\Plugins` |
| `PROGRESS.md` | **精简版**：当前状态、设计决策、宿主行为坑、GT 数据、版本索引 |
| `_archive/PROGRESS_ARCHIVE_至v2.3.0.md` | 完整历史流水账（v1.4 → v2.3.0），需要追查过程时再翻 |
| `README.md` | 插件设计、参数、性能、自测说明 |

**重新编译**：双击 `build.bat`，再跑 `install.bat`。
编译环境是 MSVC（沙箱拦截 `reg.exe`，`vcvars64.bat` 用不了，需手动设 `INCLUDE`/`LIB`/`PATH`）。
本机验证版本：MSVC 14.51.36231 + Windows SDK 10.0.26100.0。

### 03_测试夹具 —— 无宿主回归测试

来自 `...\beauty_probe\`（只收录 .py / .lua / .json / .txt；raw 与 mp4 素材未收录）

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
- `_archive/CODE_REVIEW.md` —— v1.3 时期的代码审查记录（B1–B9 / G1–G4）
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
| 自研引擎运行时资产 | `02_引擎资产\`（`onnxruntime.dll` 约 11MB、`yunet.onnx` 约 0.2MB） | 已是源码仓库的一部分，可随仓库分发；如需启用 GPU 把它换成 `onnxruntime-directml` 构建版 |
| 已安装的插件 | `C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\` | 由 `install.bat` / 安装包生成，可重建 |
| 编译产物 | `01_ofx插件源码\obj\`、`out\`、`bundle\` | 可重建（`bundle\` 是 `install.bat` 的暂存副本） |
| 运行日志与任务队列 | `C:\ProgramData\DaVinciBeauty\logs\`、`jobs\`、`live\` | 运行时数据 |

## 恢复方法

1. 把 `01_ofx插件源码` 放回任意目录，确保引擎资源可用
   （仓库内 `02_引擎资产\`，或 `bundle\...\Contents\Win64`，或已安装的 bundle）；
2. 运行 `build.bat` 编译，再运行 `install.bat` 安装；
3. 启动 DaVinci Resolve，效果库 → OpenFX → DaVinci Beauty → 「DaVinci Beauty 美颜」。

> 注意：改了插件之后**必须重启 Resolve**。
> OFX 缓存 `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`
> 按「mtime + size」判断插件是否失效。
