# 开发工作流

> 适用代码版本：1.3.5.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：未登记 ｜ 替代文档：原根目录 `开发指南.md`
> 技术架构 / 关键事实 / 排错见 `docs/reference/reference-architecture.md`；文档总表见 `docs/INDEX.md`。

**这份代码就在你手上这个目录里，改它、编它、装它、验它都在这里完成。**
注意当前检出目录名是 `DaVinciBeauty_整合_20261031`（带日期后缀），它是**唯一在维护的工作副本**，不是只读快照；名字里的日期只表示这一批整合的时点。
（此前的说明写成「`_整合_<日期>` 都是可一键重生的分发快照，不从那里读任何东西」，与实际情况不符，2026-11-08 更正。）

## 一、日常改动的 5 步

```
1. 改源码    01_ofx插件源码\src\          (beauty_*.cpp 自研引擎 / nama_engine.cpp 适配层 / ofx_plugin.cpp / nama_engine.h)
2. 改版本号  01_ofx插件源码\src\nama_engine.h 的 NAMA_BUILD_MAJOR/MINOR/PATCH/STR
             —— 版本号只有这一个出处：面板显示、二进制内嵌、安装包文件名全从这里派生
3. 编译      01_ofx插件源码\build.bat     → out\DaVinciBeauty.ofx
             （薄壳，实际编译在 build.ps1；自动探测 VS/MSVC/Windows SDK）
4. 装机      01_ofx插件源码\install.bat   → C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle
5. 验收      04_验收工具\verify.bat       → 期望全 PASS / 0 FAIL / 0 WARN
```

**第 4 步之前必须完全退出 DaVinci Resolve。** 不退出会被占用的 DLL 改名成 `.locked-<pid>` 留存，攒几次就是上百 MB 纯垃圾。

改完要给别人 / 换机器用：

```
04_验收工具\package.py            # 生成 _整合包 到上一级目录
04_验收工具\package.py --update   # 已存在时原地同步（避免大批量删除）
05_安装包\build_nsis.py           # 打 exe 安装包（需本机装 NSIS）
```

## 二、目录地图

| 目录 | 干什么 | 能不能删 |
|---|---|---|
| `01_ofx插件源码\` | C++ 源码、`build.bat`/`build.ps1`、`install.bat` | ❌ |
| `docs\` | 文档体系（guides / reference / design / adr / history / archive），入口 `docs\INDEX.md` | ❌ |
| `03_测试夹具\` | 无宿主契约测试、mock host、自测链路 | ❌ |
| `02_引擎资产\` | 自研引擎运行时资产：推理运行时（onnxruntime.dll）+ 人脸检测模型（yunet.onnx），按需自备 | ❌ 丢了需从仓库恢复 |
| `04_验收工具\` | 一键验收、回归基线、性能 A/B、清理、打包 | ❌ |
| `05_安装包\` | NSIS 安装包脚本 | ❌（打包 exe 才需要） |
| `03/04/05_*` | 历史遗留（早期 Python 脚本版、Lua 脚本版等，已归档至 `_archive/`） | ⚠️ 与当前主线无关 |

## 三、十条不能忘的约束（按踩坑先后编号）

**① 插件 ID `com.nama.davinci.beauty` 不要改。**
Resolve 是按 ID 找效果的，改了历史工程里已挂的美颜效果会全部失效。面板文字随便改，ID 不行。

**② 装机资产清单有三份，改动必须同步。**
`01_ofx插件源码\install.bat` 的 copy 行、`05_安装包\build_nsis.py` 的 `PAYLOAD_PLAN`、
`05_安装包\davincibeauty.nsi` 的 `File` 指令。

漏一处不会报错，是安装后引擎初始化失败（缺 `onnxruntime.dll` / `yunet.onnx` 时
人脸检测与美颜无法启动，画面直接透传）。`acceptance.py` 的工程组现在会静态校验
这三处，缺任何一处直接判 FAIL。

**③ UI 改动必须装机 + 重启 Resolve + 截图确认。**
Resolve 不渲染 OFX 的 Page 和 Group——试过三版折叠分组全部被证伪，面板顺序只能靠
`ofx_plugin.cpp` 里 paramDefine 的**定义顺序**控制。mock host 验证不出 UI。

**④ 写 / 改 `.bat` 必须是 GBK 编码 + CRLF 换行 + 不加 `chcp`。**
三条缺一条就会炸，症状是满屏「`'xx'` 不是内部或外部命令」，看不出跟编码有关：

| 写法 | 后果 |
|---|---|
| UTF-8 中文 | cmd 按 ANSI(GBK) 解析，多字节序列吃掉行边界，`rem` 注释被当命令执行 |
| `chcp 65001` | 中途换代码页，后续行的字节偏移全部错位（实测 `rem` 与 `title` 两行被拼成一条） |
| 裸 LF 换行 | 批处理只认 CRLF，行会被从中间切开 |

**编辑器里看不到这个坑**——文件在 VS Code / 本工具里看着完全正常，只有 cmd 会崩。
`acceptance.py` 的工程组会扫全仓 `.bat` 校验这三项，违规直接 FAIL。

**⑤ 人脸检测预处理必须是 raw 0–255（不要归一化）。**
`beauty_face.cpp::detect()` 喂模型的输入是原始 0–255；归一化（`(x−127.5)/128` 或 `x/255`）会让 人脸检测**全部归零**（实测 bestScore 0.92→0.05），引擎随之降级成全局磨皮。这是早期版本「清晰正脸却全局磨皮」的真因（3.x 编号已退役，见 CHANGELOG 末尾说明），引擎替换后已修。改检测相关代码前务必看 `docs/reference/reference-architecture.md` §2；环境变量 `BEAUTY_YUNET_PREPROC`（0..5）可翻档，默认 0=raw。

**⑥ 排错先落盘，别靠猜；也别信夹具的 `PASS`。**
「没效果 / 0 检出」先开 `BEAUTY_DEBUG_DETECT=1` 看每帧 `ncand/faces/best/输入均值`，必要时开 `BEAUTY_DEBUG_DUMP=<纯ASCII目录>` 把 `in.f32` 落盘交给 Python 用同一个 onnx 重跑——这一步能一次性切开「输入错 / 推理错 / 解码错」。
另外 `selftest_ofx.py` 的 `PASS` **只表示画面有改动**，输出文件是上次残留的旧文件也能过；判据要看 `msg=faces=N best=X`。早期两版就是因为 `NamaSelfTest` 把文件路径当图像、而夹具仍报 `PASS`，白查了三轮。详见 `docs/reference/reference-architecture.md` §2-b。

**⑦ OFX 帧是 bottom-up，夹具也必须按 bottom-up 造帧。**
`kOfxImagePropData` 指向**图像左下角**，Resolve 严格照做：插件拿到的第 0 行是画面最下面一行。检测要按视觉方向读行（`letterboxRgb` 的 `flipY`），检测出的框/关键点再映射回缓冲行号（`f.y = h - vy - f.h`、`kps.y = h - vy`），下游掩码/磨皮/变形才不用改。
**新增或修改任何夹具时，帧必须先过 `mock_host.py::_to_bottom_up()`**——以前夹具按 top-down 造帧，等于替插件掩盖了方向 bug，离线 0.92 全 PASS、真机 0.25 全灭。离线单帧工具喂 top-down 文件时，`NamaSelfTest` 已内部强制 top-down；要回归真机帧设 `BEAUTY_IMG_BOTTOMUP=1`。详见 `docs/reference/reference-architecture.md` §2-c。

**⑧ 凡是「上/下」有语义的几何量，都必须显式接收 `flipY`，不许靠注释约定。**
「美形 y 方向」那次的教训（早期版本，编号已退役）：检测的行序修好之后，下游 `beauty_warp.cpp::makeGeom()` 仍然按 top-down 用 `f.y` 推「下巴底 / 眼线 / 嘴线」，结果瘦脸压到眉毛、下巴压到发际线。**伪装性极强**——径向各向同性的「大眼」完全不受影响，看起来像「只有某一项坏了」，实际是方向整体反了。
规矩：函数里只要出现 `f.y + k·f.h` 表示「往下 k 距离」，或者出现 `sy > 某个 y` 这种不等式，就必须有 `flipY` 参数并用 `sgn = flipY ? -1 : +1` 统一表达方向；比例值也要换成「从缓冲最小行往上数」（`0.38 → 0.62`、`0.72 → 0.28`）。

**⑨ 面板参数有两条链路，两边都要有用例。**
`baseline.py` 走 `NamaSelfTest` 的 `NAMA_ST_*` 钩子，直接调引擎、**绕过 `ofx_plugin.cpp::readParams()`**；`mock_host.py` 走 `paramSetValue`，才是真人拖控件的路径。「下拉框对调」那次就是因为只测了前者，`readParams` 里的 `map[3]={1,0,2}` 把「双边滤波」和「高斯」对调了而 18/18 全绿。
规矩：改任何面板参数，面板侧必须补一条 `mock_host.py` 用例，且断言**方向**（档位对调时「有变化」依然成立，只有方向能抓住）。
另：`mock_host.py` 默认驱动**已装机**版，验证新产物要显式 `set NAMA_OFX_DLL=...\out\DaVinciBeauty.ofx`，否则量到的是旧二进制。

**⑩ 「幅度不足」和「接线错误」表现一样，先用数字分开。**
「磨皮算法三档 / 磨皮模式四档无明显变化」那次的触发点：当时 `ofx.log` 已经证明参数送达
（`blurType` 0/1/2、`smoothMode` 0–3 全出现过），真正的数字是**满档在人脸区平均只改 0.955 灰阶**——
不是没接线，是幅度太小。
规矩：用户报「没效果」时，先做两件事再动代码 —— ① 查日志确认参数值真的到了引擎；② 用真机帧
量「与源帧的平均绝对差 / 峰值 / 改动>3 的占比」。两个都过了再改算法。
另：放大磨皮时**外推和加宽核要分清**（`docs/reference/reference-architecture.md` §6-e）——`dst = src + (dst−src)·gain`
在 gain>2 之后放大的是振幅门控结构而非磨皮本身；混合类路径外推到 `tBlend>1` 会出光晕。
现场调档用 `BEAUTY_SMOOTH_GAIN`（0.25–3.0，默认 1.8）。

## 四、换新机器 / 引擎被删时

```
02_引擎资产\stage.bat      把引擎放回 C:\ProgramData\DaVinciBeauty\engine
01_ofx插件源码\install.bat 装机
```

引擎资产在系统里实际存在三份：`02_引擎资产\`、`C:\ProgramData\DaVinciBeauty\engine`、
装机 bundle 的 `Contents\Win64`。丢两份还有一份，`stage.bat` 能从任意一份补回。

打包脚本会自动按顺序找引擎，**优先用仓库自带那份**（`build_nsis.py` 与 `package.py`
的 `ENGINE_CANDIDATES` 都是这三个候选），所以正常情况不用手动指定、也不用先跑 stage：

```
02_引擎资产  →  ProgramData\DaVinciBeauty\engine  →  装机 bundle  →  兜底
```

三份全丢时才打不出带引擎的包——届时会输出「不含引擎」的轻包并提示原因。

## 五、外部依赖（本机已具备，换机器要重装）

| 依赖 | 用途 |
|---|---|
| **Visual Studio / MSVC** | `build.ps1` 用 vswhere 自动探测 VS 与 Windows SDK（不再写死路径） |
| **Python 3.12** | 验收、基线、打包脚本；本机在 `%LOCALAPPDATA%\Programs\Python\Python312`，不在 PATH 时用 `py -3.12` |
| **onnxruntime / numpy / Pillow** | 仅 `perf_ab.py` 等性能脚本需要；验收与 mock host 只用标准库 |
| **NSIS 3.x** | 仅打包 exe 时需要；`build_nsis.py` 找不到会给安装命令 |
| （可选）NVIDIA GPU | 人脸推理可走 GPU 自动加速；无 GPU 时引擎自动退回 CPU，效果不变 |

## 六、版本号同步清单（发版前核对）

改动版本号时会自动/需要同步的地方：

- `nama_engine.h` 的四个 `NAMA_BUILD_*` 宏 ← **唯一出处**
- 面板右下角「版本」参数（引用宏，自动）
- 安装包文件名、NSIS 卸载项注册表 `DisplayVersion`（`build_nsis.py` 从源码宏读取）
- 文档：`README.md`、`说明.txt`、`docs/CHANGELOG.md`、`docs/guides/guide-package.md`（`acceptance.py` 的文档组按这份清单校验）

最后一项由 `acceptance.py` 的「文档版本号同步」自动校验，忘了会被判 FAIL。
