DaVinci Beauty 源码备份

备份时间：2026-10-14
最后修订：2026-10-31（v3.0.8）

一、版本要点
v3.0.8 是交付前的工程收口版：画面算法零改动（18/18 逐字节不变），修好构建链路（中文路径 + 代码页导致的编译失败）、装机权限判据、每帧两次多余的 8.3MB 拷贝（1080p 24.5→20.5 ms），并把 14 个滑块全部纳入单项接线测试。
v3.0.0 已把引擎从旧版闭源商业 SDK 整体替换为自研纯 CPU 美颜引擎（CoreEngine 3.1.39），人脸检测用自带 ONNX 模型，引擎内自动检测 GPU（仅 Windows）：换成带 GPU 支持的 onnxruntime.dll 即走 GPU，否则退回 CPU。公开接口不变，宿主零改动。

二、当前状态

编译：通过（build.ps1；build.bat 为薄壳；产物 252928 B，md5 40df78a2dc7bce61ed6f8f5e027114fe）

装机：已装到 C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle，行为等价校验通过

无宿主回归：23/23 PASS（含 14 个滑块逐个独奏）

逐字节回归：18/18

一键验收：25 PASS / 0 FAIL / 0 WARN

性能：1080p 8bit 单帧 24.5→20.5 ms

安装包：已构建 DaVinciBeauty_Setup_v3.0.8.exe（4.6 MB，含卸载器）

Resolve 内实测：空项目 1080p 播放稳定流畅

仓库体积：约 2.5 MB（只有源码/文档/脚本）

回滚：源码已纳入 git，用 git log / git checkout <commit> -- 01_ofx插件源码/src/

三、目录与交付物

开发指南.md：开发入口，含 5 步工作流、目录地图、换机恢复、外部依赖

01_ofx插件源码/：OFX 插件 C++ 源码（当前主线）

src/ofx_plugin.cpp：OFX 协议实现主体

src/nama_engine.cpp / .h：自研引擎适配层（纯 CPU 美颜 + ONNX 人脸检测）

sdk/include/：OFX 官方头文件（编译必需）

build.bat / install.bat：编译与安装

PROGRESS.md / README.md：状态、设计决策、参数、性能、自测说明

02_引擎资产/：仓库内只有 stage.bat（把 onnxruntime.dll + yunet.onnx 放回 ProgramData），二进制不入库

03_测试夹具/：mock_host.py（19 项断言，支持 --bench）、bench_whiten.py、selftest_ofx.py、各阶段探针脚本

04_验收工具/：acceptance.py（一键验收 15 项）、baseline.py（逐字节回归）、perf_ab.py（性能 A/B）、baseline_manifest.txt、input_face.png

05_安装包/：davincibeauty.nsi、build_nsis.py、davincibeauty_setup.py

FILELIST.txt：文件清单（SHA256 + 大小）；gen_filelist.py：清单生成器

四、重新编译
双击 build.bat，再跑 install.bat。
编译环境 MSVC（沙箱拦截 reg.exe，vcvars64.bat 用不了，需手动设 INCLUDE/LIB/PATH）。本机验证：MSVC 14.51.36231 + Windows SDK 10.0.26100.0。

五、未收录内容（需另行备份）

引擎运行时资产：02_引擎资产\（onnxruntime.dll 约 11MB、yunet.onnx 约 0.2MB），从交付包或已装机 bundle 取回

已安装插件：由 install.bat / 安装包生成，可重建

编译产物：obj\、out\、bundle\，可重建

运行日志与任务队列：C:\ProgramData\DaVinciBeauty\logs\、jobs\、live\

六、恢复方法

把 01_ofx插件源码 放回任意目录，在 02_引擎资产\ 放好 onnxruntime.dll + yunet.onnx（从交付包或已装机 bundle 取回）

运行 build.bat 编译，再运行 install.bat 安装

启动 DaVinci Resolve，效果库 → OpenFX → DaVinci Beauty → 「DaVinci Beauty 美颜」

注意：改了插件之后必须重启 Resolve。OFX 缓存按「mtime + size」判断插件是否失效。
