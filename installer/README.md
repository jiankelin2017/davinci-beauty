# NamaBeauty 一键安装包

达芬奇 DaVinci Resolve 的 NamaBeauty 美颜 OpenFX 插件 —— 一键安装 / 一键卸载。

## 交付物

| 文件 | 说明 |
|---|---|
| `build\NamaBeauty_Setup_v1.3.exe` | **单文件安装包**。双击即装，内置插件 + 美颜/AI 引擎 + 模型资源（约 83 MB） |
| `namabeauty.nsi` | NSIS 安装脚本（安装/卸载逻辑都在这里） |
| `build_nsis.py` | 构建器：收集资源 + 调 makensis 编译出 exe |
| `namabeauty_setup.py` | 等价的 Python 实现，供无 GUI 场景/调试使用 |

## 使用

### 安装

**双击 `NamaBeauty_Setup_v1.3.exe`** 即可。会弹出 UAC，点「是」开始安装。

界面里能选安装目录（默认已按 OFX 规范填好，一般不用改）。

### 卸载

三种方式，任选其一：

1. **控制面板 → 程序和功能 → 找到「NamaBeauty 美颜」→ 卸载**
2. 运行安装包所在目录的卸载器：
   `C:\Program Files\Common Files\OFX\Plugins\NamaBeauty.ofx.bundle\Uninstall_NamaBeauty.exe`
3. 重新运行安装包，或命令行：
   ```
   NamaBeauty_Setup_v1.3.exe /S           静默安装
   Uninstall_NamaBeauty.exe /S            静默卸载
   ```

### 安装位置

```
C:\Program Files\Common Files\OFX\Plugins\NamaBeauty.ofx.bundle\
    Uninstall_NamaBeauty.exe          <- 卸载器
    Contents\
        Win64\                        <- OFX 规范要求的二进制层
            NamaBeauty.ofx            插件本体
            CNamaSDK.dll  19.6 MB     美颜引擎
            fuai.dll      29.0 MB     AI 推理引擎
            w32-pthreads.dll
            api-ms-win-downlevel-shlwapi-l1-1-0.dll
            auth_package.bin
            model\                    AI 模型（人脸 23 MB / 人体 42 MB）
            graphics\                 特效资源
```

### 卸载清什么（全清策略）

恢复到装机前状态：

| 目标 | 内容 |
|---|---|
| `...\OFX\Plugins\NamaBeauty.ofx.bundle\` | 整个 bundle（约 162 MB） |
| `C:\ProgramData\NamaBeauty\` | 日志、jobs、live、engine 副本、disabled_scripts 等全部数据 |
| `%APPDATA%\Blackmagic Design\...\OFXPluginCacheV2.xml` | Resolve 的插件扫描缓存 |
| `HKLM\SOFTWARE\Microsoft\...\Uninstall\NamaBeauty.BeautyOFX` | 卸载信息注册表项 |

## 安装后

1. 启动（或**重启**）DaVinci Resolve
2. 打开项目，进 Color 或 Edit 页面
3. 左侧 Effects Library → OpenFX → 分组 `NamaBeauty`
4. 把「NamaBeauty 美颜」拖到片段上，右侧 Inspector 调参数

> **参数提示**：磨皮不为 0 时锐化不生效；八项全 0 时画面完全不变。
> **日志**：`C:\ProgramData\NamaBeauty\logs\ofx.log`

## Resolve 正在运行时

Resolve 会把插件 DLL 内存映射住，导致文件无法覆盖。安装器会：

1. **启动时检测**：发现 Resolve 在跑就弹窗提示，建议先退出（可强制继续）
2. **安装时自愈**：每次安装前先扫掉历史遗留的 `*.locked-*` / `*.old_*` 文件，
   避免"越装越多"（单个 `CNamaSDK.dll` 就 19 MB）
3. **装完提示**：若之前 Resolve 在运行，明确告知**必须重启才能加载新插件**

## 从源码重新构建

前提：装好 NSIS（构建器会自动找，找不到会给安装命令）。

```bat
winget install --id NSIS.NSIS -e
```

然后：

```bat
cd installer
python build_nsis.py
```

构建前请先编译插件：

```bat
cd ..\01_ofx插件源码
build.bat
```

### 构建参数

```
--engine <dir>     引擎资源目录（默认 C:\ProgramData\NamaBeauty\engine）
--ofx <path>       插件二进制（默认 ..\01_ofx插件源码\out\NamaBeauty.ofx）
--version <str>    版本号，写进文件名
--out <path>       指定输出路径
--no-python        不捆绑 Python 解释器（体积更小）
```

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
