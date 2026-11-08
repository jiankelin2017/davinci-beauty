# 出包：安装包与整合包

> 适用代码版本：1.3.5.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：未登记 ｜ 替代文档：原 `05_安装包/README.md`

**这份文档管**：怎么出一条命令出安装包、包里装了什么、装到哪、怎么卸干净、改资产清单要动哪几处、沙箱怎么用。
**不在这里管**：装完怎么用看 `guide-install.md`，发版前跑哪些检查看 `guide-acceptance.md`，编译看 `guide-dev-workflow.md`，历史踩坑看 `docs/history/history-progress-1x.md`。

---

## 0. 摘要

**一句话**：`cd 05_安装包 && python build_nsis.py`，版本号自动取自源码宏，产物在 `build\` 下。

1. **版本号只有一个来源** —— `01_ofx插件源码\src\nama_engine.h` 的 `NAMA_BUILD_STR`。构建器自己读，不需要也不该手动传 `--version`；传了反而会造出「文件名与源码不符」的包。
2. **当前产物**：`build\DaVinciBeauty_Setup_v1.3.5.0.exe`，4,855,809 字节（4.63 MB），md5 `500f7b1754977decbaa54aee61c04a82`。
3. **"包"有两种，别混**：安装包（NSIS 出 exe，给用户装）与整合包（`04_验收工具\package.py` 出源码目录+zip，给开发/交接）。本文前半讲前者，§8 讲后者。
4. **改资产清单要同步四处**，`acceptance.py` 只硬查其中三处 —— 第四处漏了不会报错，只会在用户机器上崩。
5. **沙箱是两个独立开关**：`build.bat -Sandbox` 换插件标识，`build_nsis.py --sandbox` 换安装位置。要真正隔离，两个都得开。

---

## 1. 出安装包

```bat
cd 05_安装包
python build_nsis.py
```

前置条件两条，缺一条会当场报错并告诉你缺什么：

| 前置 | 检查方式 | 缺了怎么办 |
|---|---|---|
| 插件已编译 | `01_ofx插件源码\out\DaVinciBeauty.ofx` 存在 | `cd 01_ofx插件源码 && build.bat` |
| NSIS 已装 | 依次找 `Program Files (x86)\NSIS`、`Program Files\NSIS`、`%LOCALAPPDATA%\Programs\NSIS`、`PATH` | `winget install --id NSIS.NSIS -e` |

构建器做的事只有两步，输出里能直接看到：`[1/2]` 收集 5 个文件到临时 stage，`[2/2]` 调 `makensis` 编译。stage 是临时目录，构建完即删，不留中间产物。

> **`nsi` 脚本的 BOM 会自动补。** `Unicode true` 要求脚本是 UTF-8 带 BOM，否则 makensis 报 `Bad text encoding`；`build_nsis.py` 编译前检查并补上。所以别用记事本「另存为 ANSI」去修编码问题。

### 产物

| 文件 | 说明 |
|---|---|
| `build\DaVinciBeauty_Setup_v<版本>.exe` | 正式包，4.63 MB。装到 `DaVinciBeauty.ofx.bundle` |
| `build\DaVinciBeauty_Setup_v<版本>_sandbox.exe` | 仅 `--sandbox` 产出，装到 `DaVinciBeautySandbox.ofx.bundle` |

`build\` 下还堆着历代产物（最老到 1.0.7.0）。它们只是本地留档，**不是发布物，也不参与构建**。仓库根目录同样躺着两个早期历史安装包，同样别当发布物用——根目录那份不是「必须再复制一遍」的交付副本，构建器只写 `build\`。

---

## 2. 安装包装了什么

五个文件，全落在 `Contents\Win64`：

```
C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\
    Uninstall_DaVinciBeauty.exe          卸载器（NSIS 生成，不在 Win64 内）
    Contents\
        Win64\                          <- OFX 规范要求的二进制层
            DaVinciBeauty.ofx      348 KB   插件本体
            onnxruntime.dll       11.03 MB  推理运行时（CPU 构建版）
            yunet.onnx             227 KB   本地人脸检测模型
            LICENSE               17.6 KB   GPL-2.0 全文（本项目自身）
            THIRD-PARTY-NOTICES.md  7.8 KB  MIT / BSD-3 三方声明
```

装机后约 12 MB。安装包只有 4.63 MB，因为运行时 DLL 压缩率高。

**两个许可文件是硬性要求，不是可选项**：GPL-2.0、MIT、BSD-3 三条都要求「随副本附带声明与许可全文」。只放在 git 仓库里不算分发。`install.bat`、`PAYLOAD_PLAN`、NSI 的 `File` 指令三处都必须带着它们，`acceptance.py` 会静态校验。

**`Contents\Win64` 里不能放别的东西**。Resolve 会扫描这个目录，构建期的辅助目录（例如某些方案里会出现的 `python\`）一旦落进去就会被当成插件资源扫。`验证沙箱安装.bat` 专门有一项检查这个。

---

## 3. 卸载

三种方式，效果相同（都是跑同一份卸载逻辑，全清）：

1. **设置 → 应用 → 已安装的应用 → 「DaVinci Beauty 美颜」→ 卸载**
2. 直接跑 `DaVinciBeauty.ofx.bundle\Uninstall_DaVinciBeauty.exe`
3. 让安装包转发：`DaVinciBeauty_Setup_v<版本>.exe --uninstall`（可配 `/S` 静默）

第 3 条是 1.3.4.0 之后才真能用的。之前文档一直宣传 `--uninstall`，但 `.onInit` 里没实现，传了等于重装一遍。现在的实现会转发到已装的卸载器，并且**把真实退出码带回去**——不能在中途 `Abort`，因为 NSIS 在 `.onInit` 里 `Abort` 会固定返回 2，`winget` 和脚本会把「卸载成功」判成失败。

### 全清清掉什么

| 目标 | 内容 |
|---|---|
| `...\OFX\Plugins\DaVinciBeauty.ofx.bundle\` | 整个 bundle，含历史上的 `.bak-*` 残留 |
| `C:\ProgramData\DaVinciBeauty\` | 日志、jobs、live、engine 副本、disabled_scripts |
| `%APPDATA%\Blackmagic Design\...\OFXPluginCacheV2.xml` | Resolve 的插件扫描缓存（当前用户） |
| `HKLM\...\Uninstall\DaVinciBeauty.BeautyOFX`<br>`HKLM\SOFTWARE\DaVinciBeauty.BeautyOFX` | 卸载信息与安装记录两个键 |

两条容易踩的实现细节：

- 删之前必须 `SetOutPath "$TEMP"`。Windows 不允许删掉仍是某进程「当前目录」的文件夹，旧写法会让 `RMDir /r` 中途失败，留下一堆空目录，却仍然对外宣称「全清」。
- `$APPDATA` 在 `SetShellVarContext all` 下指向 `C:\ProgramData`，所以删数据目录用的是 `$APPDATA\DaVinciBeauty`；紧接着清 Resolve 缓存必须切回 `SetShellVarContext current`，因为那份缓存位于**当前用户**的 Roaming。

---

## 4. 改资产清单：四处，验收只查三处

新增或改名一个运行时资产时，必须同时改这四个地方：

| # | 位置 | 作用 | `acceptance.py` 查吗 |
|---|---|---|---|
| 1 | `01_ofx插件源码\install.bat` 的 copy 行 | 本机装测 | ✅ 硬查 |
| 2 | `05_安装包\build_nsis.py` 的 `PAYLOAD_PLAN` | 收进 stage | ✅ 硬查 |
| 3 | `05_安装包\davincibeauty.nsi` 的 `File` 指令 | 打进 exe | ✅ 硬查 |
| 4 | `05_安装包\davincibeauty_setup.py` 的 `PAYLOAD_FILES` | 无 GUI 的等价实现 | ❌ **不查** |

**第四处是已知缺口。** 它不影响 NSIS 出包（`build_nsis.py` 里只留了一个未被使用的 `SETUP_PY` 常量，构建并不调用它），只影响直接用 `python davincibeauty_setup.py install` 调试的路径。但缺口真实存在：改了前三处、漏了第四处，验收全绿，调试安装器却缺文件。

前三处的缺漏后果是「能装但一跑就崩」或产物里少一个文件，所以验收把这三处做成了装机前的静态校验——不用等 Resolve 报错。历史上 `ai_face_processor_pc_plain.bundle` 就是因为 `install.bat` 从没拷过它，全新装机必崩。

---

## 5. 沙箱：两个开关，别混

两件事都叫「沙箱」，但改的是不同维度，可以单开也可以同开：

| 开关 | 改什么 | 产物 | 用途 |
|---|---|---|---|
| `build.bat -Sandbox`（或 `NAMA_SANDBOX=1`） | 插件标识换成 `com.nama.davinci.beauty.sandbox`（`ofx_plugin.cpp:7`） | 仍写 `out\DaVinciBeauty.ofx` | 验证「插件本身」在宿主里的行为，不遮蔽正式插件 |
| `build_nsis.py --sandbox` | APPID → `DaVinciBeauty.SandboxTest`，bundle → `DaVinciBeautySandbox.ofx.bundle` | `build\..._sandbox.exe` | 验证「安装/卸载逻辑」，不碰正式装机 |

**完整的隔离验证要两步都做：**

```bat
cd 01_ofx插件源码
build.bat -Sandbox
copy out\DaVinciBeauty.ofx out\DaVinciBeauty_sandbox.ofx

cd ..\05_安装包
python build_nsis.py --sandbox --ofx ..\01_ofx插件源码\out\DaVinciBeauty_sandbox.ofx
验证沙箱安装.bat
```

`-Sandbox` 不自动改输出文件名，这是坑：编完不另存，下一次正常出包就会把沙箱标识打进正式包。所以第二步那个 `copy` 不是多余的。

`验证沙箱安装.bat` 会跑四段：装 → 查 5 项（插件、运行时、模型、卸载器、注册表键）+ 查 `Win64` 里有没有 `python` 目录 → 卸 → 查残留。中间和结尾各 `pause` 一次，全程两次 UAC。它**只找 `*_sandbox.exe`**，同目录、上级、上上级、`build\` 依次找，找不到就报 `sandbox installer not found` 直接退出——绝不退而求其次去装正式包。

> **这条限制是踩过事故才加的。** 曾经沙箱包与正式包同名同路径，正式构建覆盖沙箱构建后，验证脚本把正式包当沙箱包装了一遍，把正式安装刷掉，还误报 9 项「缺文件」（它查的是 Sandbox 路径，包装的是正式路径）。现在沙箱包一律带 `_sandbox` 后缀，脚本也只认这个后缀。

### 1.3.4.0 修掉的根因

在 1.3.4.0 之前，沙箱构建出来的包**不含沙箱标识**，看起来像「宏没传到编译器」。根因是 `build.ps1` 是 UTF-8 无 BOM，而 Windows PowerShell 5.1 按 GBK 解码脚本字节：一行中文注释的末字节被当成 GBK 前导字节，把行尾 CRLF 一起吃掉，于是下一行数组元素（`$sandboxDefine`、`/Brepro`）被当成注释内容吞掉。

证据是 `obj\cl.rsp` 里这几项直接不存在，而没有中文注释的项都在。修法是 `build.ps1` 保持纯 ASCII 注释，现在已经全英文。沙箱构建产物在 1.3.4.0 上已确认含沙箱标识（md5 `b1c767388847aa8f736d11b33068d31b`）。**沙箱包在 Resolve 里加载成功的记录目前没有**——所以沙箱装测只覆盖安装/卸载，不覆盖宿主加载。

---

## 6. 构建参数

```
--engine <dir>     引擎资源目录。默认按序回落，取第一个含 onnxruntime.dll 与 yunet.onnx 的：
                   ① 02_引擎资产（仓库自带，优先，保证打包自包含）
                   ② C:\ProgramData\DaVinciBeauty\engine
                   ③ 01_ofx插件源码\bundle\...\Contents\Win64（中间产物，可能被清理）
                   ④ 已装 bundle 的 Win64
--ofx <path>       插件二进制（默认 01_ofx插件源码\out\DaVinciBeauty.ofx）
--version <str>    一般不要传。默认读 nama_engine.h 的 NAMA_BUILD_STR
--out <path>       指定输出路径
--sandbox          隔离 APPID 与 bundle 名，见 §5
```

候选顺序里 `02_引擎资产` 排第一是有原因的：`01_ofx插件源码\bundle\` 是 `install.bat` 的 stage 中间产物，会被 `cleanup.py` 判成构建垃圾清掉；`C:\ProgramData\DaVinciBeauty\engine` 虽然可用，但卸载后会消失。把仓库自带那份放最前，打包才不依赖「上一台机器装过什么」。

---

## 7. 改 `davincibeauty.nsi` 必须守的五条

| 约束 | 不遵守的后果 |
|---|---|
| 脚本 UTF-8 **带 BOM** | makensis 报 `Bad text encoding`（构建器会自动补，手改后别存成 ANSI） |
| 必须有 `SetRegView 64` | NSIS 默认产 32 位安装器，卸载信息写进 `WOW6432Node`，64 位系统的「已安装的应用」里看不到 |
| `SetShellVarContext all` 才能用 `$APPDATA` 指 ProgramData；清 Resolve 缓存前必须切回 `current` | 缓存清不掉，或误删别的用户的目录 |
| 用 `$COMMONFILES64`，不是 `$COMMONAPPDATA`（后者不存在），也不是 `$COMMONPROGRAMFILES64` | 编译期报未定义常量 |
| `InstallDir` 指到 `...\bundle\Contents\Win64` | 装到 bundle 根，Resolve 扫不到插件 |

安装前还有一段残留清理：扫 `*.locked-*` 与 `*.old_*` 并删除。之所以扫两遍，是因为 NSIS 的 `FindFirst` 不支持 `*.locked-*` 这种双通配。这段存在的理由是 `install.bat` 替换被占用 DLL 时会留下改名的旧文件，最大的那个约 11 MB，不清就会越装越多。

## Resolve 正在运行时

`.onInit` 用 `tasklist | findstr` 查 `Resolve` / `davinci` / `fuscript`，命中就弹框问要不要继续；`/S` 静默安装不弹框，直接继续。安装器**不会**替用户关掉 Resolve，只检测 + 提示，装完再提示「必须重启才能加载新插件」。

---

## 8. 整合包（`04_验收工具\package.py`）

和安装包是两件不同的事：安装包给最终用户，整合包给开发与交接（源码 + 文档 + 工具 + 引擎资产）。

```bat
cd 04_验收工具
python package.py                  :: 出到 D:\插件\DaVinciBeauty_v<版本>_整合包
python package.py --zip            :: 额外打一个同名 zip
python package.py --update         :: 目标已存在时原地同步
python package.py --no-engine      :: 不带引擎资产，出轻包
```

| 参数 | 作用 |
|---|---|
| `--name <名字>` | 自定义目录名 |
| `--setup <路径>` | 把已出的安装包一并放进整合包 |
| `--no-engine` | 不带 `onnxruntime.dll` + `yunet.onnx`（约省 11 MB） |
| `--force` / `--update` | 目标已存在：覆盖 / 原地同步 |
| `--zip` / `--zip-only` | 额外压缩 / 只压缩不铺目录 |

包内会生成 `MANIFEST.txt`（每个文件一行 sha256）和 `stage_engine.bat`（把自带引擎放回 `install.bat` 认的位置）。版本号同样取自 `nama_engine.h`。

`--update` 而不是 `--force` 的场合：已经发出去的整合包要跟着改几处、不想整目录重建时用它，避免大批量删除又重建。

---

## 9. 已知限制与作废说法

**限制**（都是设计选择，不是待修 bug）：

- 需要管理员权限（`RequestExecutionLevel admin`）：写 `Program Files\Common Files` 与 `HKLM`。
- 安装器不自动关 Resolve，只检测 + 提示。
- 只装 Win64，没有 32 位与 macOS 包。
- 沙箱装测只覆盖安装/卸载，未覆盖宿主内加载（见 §5）。

**作废说法**（旧文档里出现过，别再引用）：

| 说法 | 实际情况 |
|---|---|
| 「根目录还要手动复制一份安装包」 | 构建器只写 `05_安装包\build\`。根目录那两个 exe 是早期历史残留 |
| 「`gpu_smooth` 沙箱包在 Resolve 上加载失败，暂不交付」 | 那是 1.3.4.0 之前构建标识没编进去的表现，根因已修（§5）。没有「加载失败」这个独立缺陷 |
| 「引擎目录先找 ProgramData」 | `02_引擎资产` 现在排第一 |
| 「`--uninstall` 未实现」 | 1.3.4.0 之后已实现并转发真实退出码 |
| 「双击 `..\01_验证沙箱安装.bat` 快捷入口」 | 根目录没有这个文件，只在 `05_安装包\` 下有真身 |

---

## 附：排错

| 现象 | 原因 | 做什么 |
|---|---|---|
| `find makensis` 失败 | NSIS 未装或不在四个候选路径 | `winget install --id NSIS.NSIS -e` |
| `找不到插件二进制` | 没编，或 `out\` 被清了 | `cd 01_ofx插件源码 && build.bat` |
| `找不到引擎资源目录，以下候选都不可用` | 四个候选都没 `onnxruntime.dll`/`yunet.onnx` | 用 `--engine` 指定，或跑 `install.bat` 重建 stage |
| `Bad text encoding` | nsi 不是 UTF-8 带 BOM | 让构建器自动补；别存成 ANSI |
| 装完 Resolve 里没出现插件 | Resolve 未重启，或扫描缓存未清 | 重启 Resolve；安装时已删当前用户的 `OFXPluginCacheV2.xml` |
| 装的时候提示文件被占用 | Resolve 正持有 DLL | 退出 Resolve 再装。安装器只会把旧文件改名成 `.locked-<pid>`，下次安装前才清 |
| 沙箱脚本报 `sandbox installer not found` | 没有 `*_sandbox.exe`，或放错目录 | 跑 `python build_nsis.py --sandbox`，产物留在 `build\` 即可 |
| 装机目录里 `.bak-*` 越积越多 | `install.bat` 每次替换都留一份旧 DLL | 正常现象（当前 17 份，从 1.1.7.0 起）。占空间但不影响加载；卸载器会整个删掉 |
