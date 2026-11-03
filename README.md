DaVinci Beauty 源码备份

备份时间：2026-10-14 22:36（CST）
最后修订：2026-10-31（v3.0.8：工程验收 + 性能优化）

一、v3.0.8 变更要点

编译链路改为 PowerShell 驱动：build.bat 仅为 build.ps1 的薄壳，工具链与中文路径由 PowerShell 以 UTF-16 处理；链接加 /Brepro，同源码重编逐字节相同。

install.bat 的「是否管理员」判据改为「能否创建目标目录」，受限管理员令牌下不再误报。

8bit 且无行填充时引擎直接读写宿主图像，省掉两次全帧拷贝，1080p 实测 24.5→20.5 ms（-16%）。

mock host 新增「14 个滑块逐个独奏」用例（23 项），补上立体/亮眼/去眼袋/去法令纹/祛斑/红润从未单独测过的空白。

清掉 v2.x 旧版闭源 SDK 遗留的 20 个死成员与死方法声明，修掉 faceScore_ 的 double→float 截断告警与 version() 的可变静态串悬空风险。

备份内容：源码、文档、脚本。二进制运行时/模型资产、构建产物（out/、obj/、bundle/、pycache）与本地归档都不入库；FILELIST.txt 只列仓库内的源码/文档/脚本，每个文件带 SHA256。
文件数：123 个，合计约 2.54 MB（含输入源图 input_face.png；大体积素材不计入。以 FILELIST.txt 头部为准；由 gen_filelist.py 生成，只收录 git 已跟踪的文件，改完源码重跑一次即可）。

二、当前状态（2026-10-31，v3.0.8）

v3.0.0 已把引擎从旧版闭源商业 SDK 整体替换为自研的纯 CPU 美颜引擎（CoreEngine，对应引擎 3.1.39），人脸检测用自带的 ONNX 模型，并在引擎内自动检测 GPU（仅 Windows）：把 onnxruntime.dll 换成带 GPU 支持的构建版，人脸推理就自动走 GPU，否则自动退回 CPU。公开接口不变，宿主（ofx_plugin.cpp）零改动。

v3.0.8 是交付前的工程收口版：画面算法零改动（18/18 逐字节不变），修好构建链路（中文路径 + 代码页导致的编译失败）、装机权限判据、每帧两次多余的 8.3MB 拷贝（1080p 24.5→20.5 ms），并把 14 个滑块全部纳入单项接线测试。

状态一览：

编译：通过（build.ps1，build.bat 为薄壳；产物 252928 B，md5 40df78a2dc7bce61ed6f8f5e027114fe）

装机：已装（C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle），行为等价校验通过

无宿主回归：23/23 PASS（含 14 个滑块逐个独奏）

逐字节回归：18/18（04_验收工具/baseline.py）

一键验收：25 PASS / 0 FAIL / 0 WARN（04_验收工具/acceptance.py）

性能：1080p 8bit 单帧 24.5→20.5 ms（直通优化，A/B 各 5 轮取最小值）

整合包：本仓库即整合包（v3.0.8，工作区 + 交付留档；对外副本用 package.py 另生成）

安装包：已构建 DaVinciBeauty_Setup_v3.0.8.exe（4.6 MB，含卸载器；05_安装包/build_nsis.py 可重建）

Resolve 内实测：已做，空项目 1080p 播放稳定流畅

仓库体积：约 2.5 MB（只有源码/文档/脚本；运行时二进制、模型与本地归档都不入库）

回滚方式：源码已纳入 git，回滚请用 git 历史（git log / git checkout <commit> -- 01_ofx插件源码/src/ 等）。

三、交付物

下次继续开发请先看「开发指南.md」——里面有改动流程、目录地图、四条硬约束。

开发指南.md：开发入口，含 5 步工作流、目录地图、换机恢复、外部依赖

01_ofx插件源码/：OFX 插件 C++ 源码（当前主线）

03_测试夹具/：无宿主回归测试与性能基准

02_引擎资产/：引擎运行时资产目录，仓库内只有 stage.bat（把 onnxruntime.dll + yunet.onnx 放回 ProgramData 的入口），二进制本身不入库

04_验收工具/：一键验收、逐字节回归基线、性能 A/B 测量

05_安装包/：NSIS 安装包构建脚本

FILELIST.txt：文件清单（SHA256 + 大小）

gen_filelist.py：清单生成器

四、目录说明

01_ofx插件源码 —— OFX 插件的 C++ 源码（当前主线）

src/ofx_plugin.cpp：OFX 协议实现主体（describe / describeInContext / render / IsIdentity / GetClipPreferences）

src/nama_engine.cpp / .h：自研引擎适配层，把 OFX 宿主请求转给 CoreEngine（纯 CPU 美颜 + ONNX 人脸检测），公开接口与旧版完全一致

sdk/include/：OFX 官方头文件（第三方，编译必需）

examples/：参考用 OFX 示例（不参与编译）

build.bat：编译脚本（MSVC，输出 out\DaVinciBeauty.ofx）

install.bat：打包 + 安装到 %COMMONPROGRAMFILES%\OFX\Plugins

PROGRESS.md：精简版，含当前状态、设计决策、宿主行为坑、GT 数据、版本索引

README.md：插件设计、参数、性能、自测说明

重新编译：双击 build.bat，再跑 install.bat。
编译环境是 MSVC（沙箱拦截 reg.exe，vcvars64.bat 用不了，需手动设 INCLUDE/LIB/PATH）。本机验证版本：MSVC 14.51.36231 + Windows SDK 10.0.26100.0。

03_测试夹具 —— 无宿主回归测试
来自早期的探针工作目录（只收录 .py / .lua / .json / .txt；raw 与 mp4 素材未收录）

mock_host.py：模拟最小 OFX 宿主，直接驱动插件跑 describe → describeInContext → createInstance → render，验证协议行为与像素结果。共 19 项断言，支持 --bench 跑性能基准。

bench_whiten.py：美白 CPU 掩码成本基准（同一帧只切 color_level=0/100）。

selftest_ofx.py：不经 OFX 协议，直接调 NamaSelfTest 跑「加载引擎 → 初始化 → 渲染 → 落盘」（自研引擎，无需 GL 上下文）。

probe_nama.py / probe_.py / sweep_.py / diag_*.py：各阶段探针与排查脚本（保留过程证据）。
运行基准时需自备原始帧，可通过 NB_BENCH_RAW（+NB_BENCH_W/H）指定；也可用 NAMA_OFX_DLL 指向尚未安装的插件二进制，用 NAMA_ENGINE_DIR 指向引擎资源目录。

04_验收工具 —— 发版前的一键验收

acceptance.py：一键验收，覆盖版本一致性 / 三表对齐 / 逐字节回归 / mock host 契约 / bundle 完整性，共 15 项，退出码即结论

verify.bat：双击运行版

baseline.py：逐字节回归基线，生成 / 比对；raw 不入库，只留 md5 清单

perf_ab.py：单 DLL 性能测量，供 A/B 交替比较用

baseline_manifest.txt：黄金基线 md5 清单（15 用例）

input_face.png：自带输入源图，仓库自包含

常用命令：
python acceptance.py # 退出码 0 = 通过
python acceptance.py --build --report x.txt # 发版用
python baseline.py check # 只跑逐字节回归

两个判据上的坑：NamaVersion() 不含 PATCH 位，判版本要靠二进制内嵌字符串；MSVC 构建不逐字节可复现（PE 时间戳），判装机同步要用行为等价而非 md5。

05_安装包 —— 安装包构建（NSIS / Python）

davincibeauty.nsi：NSIS 安装 / 卸载脚本，由 build_nsis.py 调用 makensis 编译

build_nsis.py：单文件 exe 安装包构建器（版本号自动从 src/nama_engine.h 读取）

davincibeauty_setup.py：无 GUI / 脚本化场景的等价安装实现与调试工具

README.md：安装器说明

根目录

FILELIST.txt：本清单（每个文件的 SHA256）

gen_filelist.py：清单生成器，改完源码跑 python gen_filelist.py 即可重算

.gitignore：忽略 out/、obj/、bundle/、pycache/、.pyc、.locked-*

说明.txt：面向使用者的简短交付说明

五、未收录的内容（如需完整环境请另行备份）

自研引擎运行时资产：02_引擎资产\（onnxruntime.dll 约 11MB、yunet.onnx 约 0.2MB）。二进制运行时资产，体积大，不入库；从交付包或已装机的 bundle 取回即可；如需启用 GPU，换成带 GPU 支持的构建版。

已安装的插件：C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\。由 install.bat / 安装包生成，可重建。

编译产物：01_ofx插件源码\obj\、out\、bundle\。可重建（bundle\ 是 install.bat 的暂存副本）。

运行日志与任务队列：C:\ProgramData\DaVinciBeauty\logs\、jobs\、live\。运行时数据。

六、恢复方法

把 01_ofx插件源码 放回任意目录，并在 02_引擎资产\ 放好引擎资源（onnxruntime.dll + yunet.onnx，从交付包或已装机的 bundle 取回），或直接用 bundle...\Contents\Win64、已安装的 bundle；

运行 build.bat 编译，再运行 install.bat 安装；

启动 DaVinci Resolve，效果库 → OpenFX → DaVinci Beauty → 「DaVinci Beauty 美颜」。

注意：改了插件之后必须重启 Resolve。
OFX 缓存 %APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml 按「mtime + size」判断插件是否失效。
