# DaVinci Beauty 源码备份

- 备份时间：2026-10-14 22:36（CST）
- 最后修订：**2026-10-31（v3.0.4：修正帧行序 —— OFX/Resolve 给的是 bottom-up 帧，检测此前按 top-down 读，脸是上下颠倒的，真机 1080×1920 帧实测 score 0.2519/0 脸 → 0.9333/1 脸；v3.0.3：修掉三个「假故障」——离线自测夹具把文件路径当图像、异步检测在按需渲染宿主里永久透传、参数句柄失败静默给 0；v3.0.2 修正人脸检测预处理为 raw 0-255；v3.0.0 彻底移除 FaceUnity NamaSDK，改用 OBS 自研纯 CPU 美颜引擎 + ONNX 人脸检测，支持 GPU 自动检测）**
- 备份内容：**源码、文档、脚本**。`out/`、`obj/`、`bundle/`、`__pycache__` 是本地
  构建产物，不计入清单；`FILELIST.txt` 只列源码/文档，每个文件带 SHA256
- 文件数：**91 个，合计约 1.05 MB**（以 `FILELIST.txt` 头部为准；由
  `gen_filelist.py` 生成，改完源码重跑一次即可）
- 本地回滚副本（`src/_backup_before_*`）**不**计入清单，只作修复前留档

## 当前状态（2026-10-31，v3.0.4）

**v3.0.0 已把引擎从闭源 FaceUnity NamaSDK 整体替换为 OBS 自研的纯 CPU 美颜引擎
（CoreEngine，对应 OBS 3.1.39），人脸检测用自带 ONNX YuNet 模型，并在引擎内自动检测
GPU（仅 Windows DirectML）：把 `onnxruntime.dll` 换成 `onnxruntime-directml` 构建版，
人脸推理就自动走 GPU，否则自动退回 CPU。公开接口不变，宿主（ofx_plugin.cpp）零改动。**

演进主线：v2.0 把**引擎层整体替换为 OBS 美颜插件 3.0.19**（宿主层保持 v1.51）；
之后 v2.2.0 做性能 3.2×、v2.2.3 整理面板与默认档、v2.3.0 再压内存流量、v2.3.1 统一更名
DaVinci Beauty；v3.0.0 进一步**脱离 NamaSDK**，改用完全自研、可自由分发的引擎。
决策依据、标定数据、踩坑记录见 `01_ofx插件源码/PROGRESS.md`（精简版），
完整历史流水账在 `PROGRESS_ARCHIVE_至v2.3.0.md`。

| 项 | 状态 |
|---|---|
| 编译 | 通过（`build.bat`，产物 142848 B，md5 `22d42e96446a5b3c40b27dff061580f2`） |
| 装机 | **已装**（`C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle`），编译版 = 装机版，字节一致 |
| 无宿主回归 | **19/19 PASS**（装机版实跑） |
| 逐字节回归 | **15/15**（`06_验收工具/baseline.py`） |
| 一键验收 | **20 PASS / 0 FAIL / 0 WARN**（`06_验收工具/acceptance.py`，含新增「工程」组） |
| 整合包 | **已出**：`D:\插件\DaVinciBeauty_v3.0.0_整合包\`（自研引擎，约 12MB）+ 同名 zip |
| 安装包 | **未构建** `.exe`，需要时跑 `installer/build_nsis.py` |
| Resolve 内实测 | **已做**：空项目 1080p 播放稳定流畅 |
| 仓库体积 | 71MB（清理前 351MB，清掉 418MB 残留备份/中间产物/探针输出） |

回滚方式：把 `01_ofx插件源码/src/_backup_before_v2.0/` 的源文件覆盖回 `src/`，
重跑 `build.bat` 与 `install.bat`。
（`src/nama_engine.cpp.bak_2_2_3` 是 v2.2.3 源码的唯一副本 —— git 尚未提交快照，
在这之前不要删。）

## 交付物

> **下次继续开发请先看「`开发指南.md`」** —— 里面有改动流程、目录地图、四条硬约束。

| 文件 | 说明 |
|---|---|
| `开发指南.md` | **开发入口**：5 步工作流、目录地图、换机恢复、外部依赖 |
| `01_ofx插件源码/` | OFX 插件 C++ 源码（当前主线） |
| `02_测试夹具beauty_probe/` | 无宿主回归测试与性能基准 |
| `07_引擎资产/` | 自研引擎运行时资产：ONNX Runtime（CPU 构建版 `onnxruntime.dll`）+ 人脸检测模型 `yunet.onnx`（含 `stage.bat`） |
| `06_验收工具/` | 一键验收、逐字节回归基线、性能 A/B 测量 |
| `03_脚本版Python源码/` | 早期 Python 脚本版（已不再维护，留档） |
| `04_脚本版Lua源码/` | 早期 Lua 脚本版（已移出菜单，留档） |
| `installer/` | NSIS 安装包构建脚本 |
| `FILELIST.txt` | 文件清单（SHA256 + 大小） |
| `gen_filelist.py` | 清单生成器 |
| `CODE_REVIEW.md` | v1.3 时期的代码审查记录（B1–B9 / G1–G4） |

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
| `PROGRESS_ARCHIVE_至v2.3.0.md` | 完整历史流水账（v1.4 → v2.3.0），需要追查过程时再翻 |
| `README.md` | 插件设计、参数、性能、自测说明 |

**重新编译**：双击 `build.bat`，再跑 `install.bat`。
编译环境是 MSVC（沙箱拦截 `reg.exe`，`vcvars64.bat` 用不了，需手动设 `INCLUDE`/`LIB`/`PATH`）。
本机验证版本：MSVC 14.51.36231 + Windows SDK 10.0.26100.0。

### 02_测试夹具beauty_probe —— 无宿主回归测试

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

### 06_验收工具 —— 发版前的一键验收（2026-10-27 新增）

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

### 03_脚本版Python源码 —— 早期的 Python 脚本版（已不再维护）

来自 `C:\ProgramData\DaVinciBeauty\`：`nama_worker.py`（Worker）、`nama_live.py`（实时预览）、
`nama_ui.py`（面板）、`DaVinci Beauty_美颜_cli.py`（命令行）、`config.json`、`tests/`。

### 04_脚本版Lua源码 —— 早期 Lua 脚本版（已移出菜单，仅作留档）

- `DaVinci Beauty 美颜/DaVinci Beauty 美颜.lua` —— Fusion 脚本菜单入口
- `旧备份/` —— 历史版本快照

### installer —— 安装包构建（NSIS / Python）

- `davincibeauty.nsi` —— NSIS 安装 / 卸载脚本，由 `build_nsis.py` 调用 makensis 编译
- `build_nsis.py` —— 单文件 exe 安装包构建器（版本号自动从 `src/nama_engine.h` 读取）
- `davincibeauty_setup.py` —— 无 GUI / 脚本化场景的等价安装实现与调试工具
- `README.md` —— 安装器说明

### 根目录

- `FILELIST.txt` —— 本清单（每个文件的 SHA256）
- `gen_filelist.py` —— 清单生成器，改完源码跑 `python gen_filelist.py` 即可重算
- `.gitignore` —— 忽略 `out/`、`obj/`、`bundle/`、`__pycache__/`、`*.pyc`、`*.locked-*`
- `CODE_REVIEW.md` —— v1.3 时期的代码审查记录（B1–B9 / G1–G4）
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
| 自研引擎运行时资产 | `07_引擎资产\`（`onnxruntime.dll` 约 11MB、`yunet.onnx` 约 0.2MB） | 已是源码仓库的一部分，可随仓库分发；如需启用 GPU 把它换成 `onnxruntime-directml` 构建版 |
| 已安装的插件 | `C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\` | 由 `install.bat` / 安装包生成，可重建 |
| 编译产物 | `01_ofx插件源码\obj\`、`out\`、`bundle\` | 可重建（`bundle\` 是 `install.bat` 的暂存副本） |
| 运行日志与任务队列 | `C:\ProgramData\DaVinciBeauty\logs\`、`jobs\`、`live\` | 运行时数据 |

## 恢复方法

1. 把 `01_ofx插件源码` 放回任意目录，确保引擎资源可用
   （仓库内 `07_引擎资产\`，或 `bundle\...\Contents\Win64`，或已安装的 bundle）；
2. 运行 `build.bat` 编译，再运行 `install.bat` 安装；
3. 启动 DaVinci Resolve，效果库 → OpenFX → DaVinci Beauty → 「DaVinci Beauty 美颜」。

> 注意：改了插件之后**必须重启 Resolve**。
> OFX 缓存 `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`
> 按「mtime + size」判断插件是否失效。
