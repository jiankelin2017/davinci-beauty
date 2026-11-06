# DaVinci Beauty 一键安装包

达芬奇 DaVinci Resolve 的 DaVinci Beauty 美颜 OpenFX 插件 —— 一键安装 / 一键卸载。

> 当前构建版本：**1.0.8.0**（2026-11-07）。安装包内含 `LICENSE` 与 `THIRD-PARTY-NOTICES.md`。下文出现的 v3.0.9 等是旧口径（三段式）历史记录，四段式对照表见 `01_ofx插件源码/PROGRESS.md`。

## 交付物

| 文件 | 说明 |
|---|---|
| `build\DaVinciBeauty_Setup_v<版本>.exe` | **单文件安装包**。双击即装，内置插件 + 自研美颜/AI 引擎（推理运行时 + 人脸检测模型，约 12 MB），不再依赖闭源 SDK。v1.0.8.0 已构建（4.6 MB，含卸载器与 `LICENSE` / `THIRD-PARTY-NOTICES.md`），可用 `build_nsis.py` 重建 |
| `build\DaVinciBeauty_Setup_v<版本>_sandbox.exe` | 沙箱包，仅 `--sandbox` 构建时产出。装到 `DaVinciBeautySandbox.ofx.bundle`，不影响正式安装 |
| `davincibeauty.nsi` | NSIS 安装脚本（安装/卸载逻辑都在这里） |
| `build_nsis.py` | 构建器：收集资源 + 调 makensis 编译出 exe |
| `验证沙箱安装.bat` | 双击跑一遍「安装 → 校验 → 卸载 → 校验」，**只认 `_sandbox.exe`**，不碰正式安装 |
| `davincibeauty_setup.py` | 等价的 Python 实现，供无 GUI 场景/调试使用 |

> **沙箱包与正式包必须不同名。** 曾经两者同名同路径，正式构建覆盖沙箱构建后，
> 验证脚本把正式包当沙箱包装了一遍，把正式安装刷掉，还误报 9 项「缺文件」。
> 现在沙箱包一律带 `_sandbox` 后缀，验证脚本也只匹配该后缀——拿不到就明确
> 报「未执行验证」，绝不替你装正式包。

## 快速开始

```
:: 直接出正式包（日常就用这条）
python build_nsis.py
→ build\DaVinciBeauty_Setup_v<版本>.exe（版本取自 src\nama_engine.h，当前 1.0.8.0；四段式，与 NAMA_BUILD_STR 同源）；若要随包分发，再手动复制一份到仓库根目录同名文件

:: 改过安装脚本、想先试一遍又不想弄脏正式安装时，才需要沙箱
python build_nsis.py --sandbox
→ build\DaVinciBeauty_Setup_v<版本>_sandbox.exe（带 _sandbox 后缀，与正式包并存）
然后双击「验证沙箱安装.bat」
```

**找不到 `验证沙箱安装.bat`？** Windows 默认隐藏 `.bat` 扩展名，列表里显示为
「验证沙箱安装」（没有后缀）；中文名按拼音排序还会掉到列表最底部。三个办法：

| 位置 | 说明 |
|---|---|
| `..\01_验证沙箱安装.bat` | **上级目录快捷入口**（`01_` 前缀，排最前），双击即可，内部转调本目录真身 |
| `DaVinciBeauty\05_安装包\验证沙箱安装.bat` | 真身（改逻辑只改这个） |
| 资源管理器地址栏粘贴 `..\05_安装包` | 直接进目录找 |

脚本会自动找安装包（同目录 / 上级 / 再上级 / `build\`），放哪都能跑。
运行过程弹两次 UAC，点「是」。第一阶段跑完会 `pause`，看清楚结果再按任意键继续卸载。

## 使用

### 安装

**双击 `DaVinciBeauty_Setup_v<版本>.exe`**（或根目录同名文件）即可。会弹出 UAC，点「是」开始安装。

界面里能选安装目录（默认已按 OFX 规范填好，一般不用改）。

### 卸载

三种方式，任选其一：

1. **控制面板 → 程序和功能 → 找到「DaVinci Beauty 美颜」→ 卸载**
2. 运行安装包所在目录的卸载器：
   `C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Uninstall_DaVinciBeauty.exe`
3. 重新运行安装包，或命令行：
   ```
   DaVinciBeauty_Setup_v<版本>.exe /S           静默安装
   Uninstall_DaVinciBeauty.exe /S            静默卸载
   ```

### 安装位置

```
C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\
    Uninstall_DaVinciBeauty.exe          <- 卸载器
    Contents\
        Win64\                        <- OFX 规范要求的二进制层
            DaVinciBeauty.ofx            插件本体
            onnxruntime.dll  约 11 MB   推理运行时（CPU 构建版；换带 GPU 支持的构建版即可启用 GPU）
            yunet.onnx        约 0.2 MB  本地人脸检测模型
```

### 卸载清什么（全清策略）

恢复到装机前状态：

| 目标 | 内容 |
|---|---|
| `...\OFX\Plugins\DaVinciBeauty.ofx.bundle\` | 整个 bundle（约 12 MB） |
| `C:\ProgramData\DaVinciBeauty\` | 日志、jobs、live、engine 副本、disabled_scripts 等全部数据 |
| `%APPDATA%\Blackmagic Design\...\OFXPluginCacheV2.xml` | Resolve 的插件扫描缓存 |
| `HKLM\SOFTWARE\Microsoft\...\Uninstall\DaVinciBeauty.BeautyOFX` | 卸载信息注册表项 |

## 安装后

1. 启动（或**重启**）DaVinci Resolve
2. 打开项目，进 Color 或 Edit 页面
3. 左侧 Effects Library → OpenFX → 分组 `DaVinci Beauty`
4. 把「DaVinci Beauty 美颜」拖到片段上，右侧 Inspector 调参数

> **参数提示**：磨皮不为 0 时锐化不生效；八项全 0 时画面完全不变。
> **日志**：`C:\ProgramData\DaVinciBeauty\logs\ofx.log`

## Resolve 正在运行时

Resolve 会把插件 DLL 内存映射住，导致文件无法覆盖。安装器会：

1. **启动时检测**：发现 Resolve 在跑就弹窗提示，建议先退出（可强制继续）
2. **安装时自愈**：每次安装前先扫掉历史遗留的 `*.locked-*` / `*.old_*` 文件，
   避免"越装越多"（单个 `onnxruntime.dll` 约 11 MB）
3. **装完提示**：若之前 Resolve 在运行，明确告知**必须重启才能加载新插件**

## 从源码重新构建

前提：装好 NSIS（构建器会自动找，找不到会给安装命令）。

```bat
winget install --id NSIS.NSIS -e
```

然后：

```bat
cd 05_安装包
python build_nsis.py
```

构建前请先编译插件：

```bat
cd ..\01_ofx插件源码
build.bat
```

### 构建参数

```
--engine <dir>     引擎资源目录。不指定时按序回落：
                   C:\ProgramData\DaVinciBeauty\engine
                   → ..\01_ofx插件源码\bundle\...\Win64
                   → 已装 bundle 的 Win64
--ofx <path>       插件二进制（默认 ..\01_ofx插件源码\out\DaVinciBeauty.ofx）
--version <str>    版本号。**默认从 src\nama_engine.h 的 NAMA_BUILD_* 宏读取**，
                   不传就不会出现「产物名写死旧版本、实际已升级」的漂移
--out <path>       指定输出路径
--sandbox          沙箱构建：APPID 换成 DaVinciBeauty.SandboxTest、
                   bundle 名换成 DaVinciBeautySandbox.ofx.bundle，
                   用来验证安装/卸载而不影响正式装机
```

> **改动资产清单时必须同步三处**，缺一处就会产出「能装但一运行就崩」的包：
> `install.bat` 的 copy 行、`build_nsis.py` 的 `PAYLOAD_PLAN`、
> `davincibeauty.nsi` 的 `File` 指令。
> `04_验收工具\acceptance.py` 的工程组会对三处做静态校验，
> 缺任何一处直接判 FAIL —— 装机之前就能抓到，不用等运行崩溃。

## 工作原理

### 为什么是 NSIS

本机工具链排查记录（避免下次再走弯路）：

| 工具 | 状态 |
|---|---|
| NSIS / 7-Zip | 原本没有 → **用 winget 装上了 NSIS 3.13** |
| .NET `csc.exe` | 存在但被安全策略禁用（连命令行里出现这几个字母都被拦） |
| `dotnet` | 只装了运行时（6.0/8.0/10.0），**没有 SDK**，不能 `dotnet build` |
| `iexpress.exe` + `makecab.exe` | 存在，但 SED 配置反复调不通（静默 rc=1） |
| 自写 `.cmd` + 附加 zip | 试过：cmd 能跑，但**改成 `.exe` 扩展名后 Windows 拒绝执行**（PE 加载器报"版本不兼容"） |

结论：**NSIS 是这台机器上唯一能产出真正的 PE 单文件 exe 的方案.**

### 附带发现（下次开发有用）

- `System32\tar.exe` 是 **bsdtar 3.8.8，能直接解 zip**；但 PATH 上若有 Git 自带的
  **GNU tar**，它会先被找到，而 GNU tar 不认 zip。
- 喂给 bsdtar 的路径**不能含盘符** —— `tar -xf "C:\..."` 会被当成 `host:path`
  远端语法，报 `Cannot connect to C: resolve failed`。
- `makecab` + `extrac32` 组合**不能**解出附加在任意 exe 后的 cab。
- Python 以文本模式写 SED/批处理会把 `\r\n` 变成 `\r\r\n`，必须用二进制模式写。

### 关键技术点（改 NSI 脚本时注意）

1. **NSIS 脚本必须是 UTF-8 带 BOM**（`Unicode true` 的要求），
   否则 makensis 报 `Bad text encoding`。`build_nsis.py` 编译前会自动补 BOM。
2. **必须 `SetRegView 64`** —— NSIS 默认产出 32 位安装器，
   否则卸载信息会写进 `WOW6432Node`，在 64 位系统的「应用和功能」里看不到。
3. **必须 `SetShellVarContext all`** 才能让 `$APPDATA` 指向 `C:\ProgramData`；
   而 Resolve 的缓存位于**当前用户**的 Roaming，那一段要切回 `SetShellVarContext current`。
4. `$COMMONAPPDATA` **不是**合法 NSIS 常量；`$COMMONFILES64` 才是
   `C:\Program Files\Common Files`（注意不是 `$COMMONPROGRAMFILES64`）。
5. `InstallDir` 必须指到 `bundle\Contents\Win64`（OFX 规范），不是 bundle 根。

## 已知限制

- 需要 **Windows 10 1803+**（`nsExec` + 部分新常量）
- 杀毒软件可能对 NSIS 自解压 exe 误报 —— 这是此类安装包通病；
  如被拦截可对 exe 加白名单
- 安装器**不会**自动关掉 Resolve，只检测 + 提示
- 同一个安装包安装/卸载都走它自己；卸载优先从「应用和功能」走，最干净
