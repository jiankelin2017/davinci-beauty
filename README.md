# DaVinci Beauty —— DaVinci Resolve 美颜滤镜（OFX）

> 当前构建：**1.3.5.0（2026-11-08）** ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：Jian
> 文档总表：`docs/INDEX.md` ｜ 版本变化：`docs/CHANGELOG.md`

达芬奇 Resolve 的 OFX 美颜插件。**自研纯 CPU 美颜引擎**（移植自上一代宿主 3.1.x）+ **ONNX 人脸检测**；人脸推理有 GPU 时自动加速，无 GPU 时自动退回 CPU，效果一致。插件 ID：`com.nama.davinci.beauty`（不要改，改了历史工程里已挂的效果会失效）。

**1.3.5.0 只修三处失败路径，正常渲染路径一行未动，画面逐字节不变**（基线 18/18、验收 28 项全过）：① 并行任务计数器记错（死锁，整插件卡死）；② GPU 掉驱动后永久静默停用（表现是「莫名变慢」）；③ ORT 空 session options 崩溃。细节见 `docs/CHANGELOG.md`。

## 快速开始

```
1. 完全退出 DaVinci Resolve
2. 01_ofx插件源码\install.bat
   （或用安装包：05_安装包\build\DaVinciBeauty_Setup_v1.3.5.0.exe）
3. 重启 Resolve → Effects Library > OpenFX > DaVinci Beauty
```

装完自检：`04_验收工具\verify.bat`，期望 `PASS 28 / FAIL 0`。
安装 / 升级 / 回滚 / 配置开关 / 常见问题：`docs/guides/guide-install.md`。

## 文档在哪

| 想知道 | 看这里 |
|---|---|
| 全部文档与状态 | `docs/INDEX.md` |
| 每个版本用户能看出来什么 | `docs/CHANGELOG.md` |
| 怎么装、怎么退 | `docs/guides/guide-install.md` |
| 怎么改代码、发版流程 | `docs/guides/guide-dev-workflow.md` |
| 怎么验收、判据是什么 | `docs/guides/guide-acceptance.md` |
| 怎么出安装包 | `docs/guides/guide-package.md` |
| 架构、关键事实、排错 | `docs/reference/reference-architecture.md` |
| 性能实测、被证伪的猜想 | `docs/reference/reference-performance.md` |
| 当时的完整过程 | `docs/history/history-progress-1x.md` |

## 目录形态

```
01_ofx插件源码\   C++ 源码 + build.ps1 / build.bat / install.bat
02_引擎资产\      onnxruntime.dll + yunet.onnx（不入库，从交付包或已装机 bundle 取回）
03_测试夹具\      mock host、自测链路、探针脚本（运行记录见 RUNS.md）
04_验收工具\      acceptance.py / baseline.py / verify.bat / package.py
05_安装包\        NSIS 构建脚本（build_nsis.py）
docs\             文档体系（总表见 docs/INDEX.md）
LICENSE / THIRD-PARTY-NOTICES.md   许可与第三方声明
FILELIST.txt      已跟踪文件的 sha256 清单（gen_filelist.py 生成）
```

`安装说明.txt` 由 `package.py` 在**包内**生成，仓库里不留副本 —— 留一份就一定会过期。
**仓库里没有**：推理运行时与模型的入库副本、构建产物（`out/ obj/ bundle/`）、安装包 exe、本地归档 `_archive/`。
`package.py` 有防呆：目标目录 == 本仓库时直接拒绝，避免把 `.git` 哈希进清单或 `--force` 删库重来。

## 引擎与硬件

引擎已从早期闭源商业 SDK 整体替换为自研 CoreEngine，公开接口不变，宿主层（`ofx_plugin.cpp`）无需改动。GPU 只用于**人脸推理**（把 `onnxruntime.dll` 换成带 GPU 支持的构建版即自动启用）；磨皮自身的 GPU 路径是另一套 D3D11 compute 管线，见 `docs/design/design-gpu-smooth.md`。

## 恢复方法

仓库是源码状态的唯一可信来源。换机器或误删后：

1. clone 本仓库（代码、脚本、文档、测试夹具都在）；
2. 从交付包或已装机 bundle 取回 `onnxruntime.dll` 与 `yunet.onnx`，放进 `02_引擎资产\`；
3. 跑 `01_ofx插件源码\build.bat` 重编译，再 `install.bat` 装机；
4. `04_验收工具\verify.bat` 复核 28 项。

## 许可（License）

本项目以 **GNU General Public License version 2（仅第 2 版，GPL-2.0-only）** 发布。

- 许可全文：根目录 [`LICENSE`](LICENSE)
- 第三方组件（ONNX Runtime 1.20、YuNet 人脸模型、OpenFX 头文件）的版权与许可见 [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)

两点需要说清楚：

1. **「仅第 2 版」不是笔误**。文件头没有写 "or (at your option) any later version"，所以下游不能自行按 GPLv3 使用。要改成「或更高版本」，只需改本段声明，`LICENSE` 正文一个字都不用动。
2. **GPL-2.0 与 Apache-2.0 不兼容**，与 MIT、BSD-3-Clause 兼容。现有三个第三方组件都是 MIT/BSD-3，所以这个组合成立；将来若引入 Apache-2.0 组件，整个授权结构必须重做。
