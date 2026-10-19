# NamaBeauty 源码备份

- 备份时间：2026-10-14 22:36（CST）
- 最后修订：2026-10-17（按审查报告落地 P0–P4 修复）
- 备份内容：**纯源码与文档**，不含任何二进制（DLL / .bundle / .raw / .mp4 一律未收录）
- 文件数：71　合计：约 923 KB
- 校验：见同目录 `FILELIST.txt`（含每个文件的 SHA256）；该文件由 `gen_filelist.py` 生成，改动源码后重跑一次即可同步
- 本地回滚副本（`src/_backup_before_*`）**不**计入清单，只作修复前留档

## 目录说明

### 01_ofx插件源码 —— OFX 插件的 C++ 源码（当前主线）

来自 `C:\Users\Administrator\WorkBuddy\2026-10-14-13-17-08\ofx\`

| 路径 | 说明 |
|---|---|
| `src/ofx_plugin.cpp` | OFX 协议实现主体（describe / describeInContext / render / IsIdentity） |
| `src/nama_engine.cpp` / `.h` | NamaSDK 封装：引擎初始化、参数下发、像素转换、日志 |
| `sdk/include/` | OFX 官方头文件（第三方，编译必需） |
| `examples/` | 参考用 OFX 示例 |
| `build.bat` | 编译脚本（MSVC，输出 `out\NamaBeauty.ofx`） |
| `install.bat` | 打包 + 安装到 `%COMMONPROGRAMFILES%\OFX\Plugins` |
| `PROGRESS.md` | 开发进度与踩坑记录（含 2026-10-14 定格 bug 完整复盘） |
| `README.md` | 插件设计说明 |

**重新编译**：双击 `build.bat`，再跑 `install.bat`。
`install.bat`会在`C:\ProgramData\NamaBeauty\engine`找引擎资源，
编译环境是 MSVC（沙箱拦截 `reg.exe`，`vcvars64.bat` 用不了，需手动设 `INCLUDE`/`LIB`/`PATH`）。

### 02_测试夹具beauty_probe —— 无宿主回归测试

来自 `...\beauty_probe\`（只收录 .py / .lua / .json，raw 与 mp4 测试素材未收录）

- `mock_host.py` —— 模拟最小 OFX 宿主，直接驱动插件跑
  `describe → describeInContext → createInstance → render`，并在不开 Resolve 的前提下
  验证「时间透传」「全 0 透传」「IsIdentity」「连续多帧」等协议行为。支持 `--bench` 跑性能基准。
- `selftest_ofx.py` / `selftest_ui.py` / `probe_nama.py` / `test_harness.py` —— 各阶段自测脚本
- `diag_*.py` —— 诊断 Resolve 安装路径 / 注册表 / 引擎加载的排查脚本

运行基准测试时需自备 1080x1920 RGBA 原始帧，可通过环境变量`NB_BENCH_RAW`进行指定；
也可使用`NAMA_OFX_DLL`指向尚未安装的插件二进制文件。

### 03_脚本版Python源码 —— 早期的 Python 脚本版（已不再维护）

来自 `C:\ProgramData\NamaBeauty\`

- `nama_worker.py` —— 引擎 Worker（面板参数 → SDK 换算、逐帧处理、SRT/媒体池导出）
- `nama_live.py` —— 常驻实时预览服务（PPM 握手目录方案）
- `nama_ui.py` —— 面板 UI
- `NamaBeauty_美颜_cli.py` —— 命令行版
- `config.json` —— 默认参数
- `tests/` —— 该阶段的实验与自测脚本

### 04_脚本版Lua源码 —— 早期 Lua 脚本版（已移出菜单，仅作留档）

- `NamaBeauty 美颜/NamaBeauty 美颜.lua` —— Fusion 脚本菜单入口
- `旧备份/` —— 历史版本快照

### installer —— 安装包构建（NSIS / Python）

- `namabeauty.nsi` —— NSIS 安装 / 卸载脚本，由 `build_nsis.py` 调用 makensis 编译
- `build_nsis.py` —— 单文件 exe 安装包构建器（`python build_nsis.py --version 1.3`）
- `namabeauty_setup.py` —— 无 GUI / 脚本化场景的等价安装实现与调试工具
- `README.md` —— 安装器说明

### 根目录

- `FILELIST.txt` —— 本清单（每个文件的 SHA256）
- `gen_filelist.py` —— 清单生成器，改完源码跑 `python gen_filelist.py` 即可重算
- `CODE_REVIEW.md` —— 早期代码审查记录（B1–B9 / G1–G4）

## 未收录的内容（如需完整环境请另行备份）

| 内容 | 位置 | 为什么没收 |
|---|---|---|
| NamaSDK 引擎与模型 | `C:\ProgramData\NamaBeauty\engine\`（`CNamaSDK.dll` 19MB、`fuai.dll` 29MB、`model/*.bundle` 64MB） | 是官方二进制资源不是源码，且体积大 |
| 已安装的插件 | `C:\Program Files\Common Files\OFX\Plugins\NamaBeauty.ofx.bundle\` | 由 `install.bat` 生成，可重建 |
| 编译产物 | `ofx\obj\`、`ofx\out\`、`ofx\bundle\` | 可重建 |
| 运行日志与任务队列 | `C:\ProgramData\NamaBeauty\logs\`、`jobs\`、`live\` | 运行时数据 |

## 恢复方法

1. 把 `01_ofx插件源码` 放回任意目录，确保 `C:\ProgramData\NamaBeauty\engine\` 存在；
2. 运行 `build.bat` 编译，再运行 `install.bat` 安装；
3. 启动 DaVinci Resolve，效果库 → OpenFX → NamaBeauty → 「NamaBeauty 美颜」。

> 注意：改了插件之后**必须重启 Resolve**。
> OFX 缓存 `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`
> 按「mtime + size」判断插件是否失效。

﻿NamaBeauty 美颜插件 v1.3 —— 交付包
生成时间：2026-10-18

【本文件夹内容】
  NamaBeauty_Setup_v1.3.exe   安装程序（双击运行，会请求管理员权限）
                              卸载：控制面板 ->「应用和功能」->「NamaBeauty 美颜」
                              或  NamaBeauty_Setup_v1.3.exe --uninstall
                              或  C:\Program Files\Common Files\OFX\Plugins\NamaBeauty.ofx.bundle\Uninstall_NamaBeauty.exe
  源码\                       修复后的 v1.3 完整源码（纯源码与文档，不含二进制）
                              清单与校验：源码\FILELIST.txt（每个文件的 SHA256）
                              说明：      源码\README.md

【安装前】
  请先完全退出 DaVinci Resolve；安装后重启它才能识别新插件。

【使用】
效果库 -> OpenFX -> NamaBeauty -> 将「NamaBeauty 美颜」拖到片段上。
  提示：磨皮不为 0 时锐化不生效；八项全 0 时画面完全不变。

【版本 / 本次变更】
  插件上报版本 1.3（NamaVersion = 0x10003）。
  源码已完成《NamaBeauty_v1.3_审查报告》P0–P4 全部修复。
  安装器本次重新出包，改动：
    - 新增 --uninstall 参数支持（等价于调用自带卸载器）
    - 静默安装 /S 不再弹框卡死；失败时返回非 0 退出码
    - 卸载更彻底：连带删掉空的 bundle 目录

【静默用法（可选）】
  安装：NamaBeauty_Setup_v1.3.exe /S
  卸载：NamaBeauty_Setup_v1.3.exe --uninstall /S
