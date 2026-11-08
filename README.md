# DaVinci Beauty —— DaVinci Resolve 美颜滤镜（OFX）

> 当前构建：**1.3.6.0（2026-11-08）** ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：Jian
> 文档总表：`docs/INDEX.md` ｜ 版本变化：`docs/CHANGELOG.md`

达芬奇 Resolve 的 OFX 美颜插件。**自研纯 CPU 美颜引擎** + **ONNX 人脸检测**；人脸推理有 GPU 时自动加速，无 GPU 时自动退回 CPU，效果一致。插件 ID：`com.nama.davinci.beauty`。


## 快速开始

```
1. 完全退出 DaVinci Resolve
2. 01_ofx插件源码\install.bat
   （或用安装包：dist\DaVinciBeauty_Setup_v1.3.6.0.exe，校验值见 dist\SHA256SUMS.txt）
3. 重启 Resolve → Effects Library > OpenFX > DaVinci Beauty
```

装完自检：`04_验收工具\verify.bat`，期望 `PASS 28 / FAIL 0`。
安装 / 升级 / 回滚 / 配置开关 / 常见问题：`docs/guides/guide-install.md`。



## 目录形态

```
01_ofx插件源码\   C++ 源码 + build.ps1 / build.bat / install.bat
02_引擎资产\      onnxruntime.dll + yunet.onnx（不入库，从交付包或已装机 bundle 取回）
03_测试夹具\      mock host、自测链路、探针脚本（运行记录见 RUNS.md）
04_验收工具\      acceptance.py / baseline.py / verify.bat / package.py
05_安装包\        NSIS 构建脚本（build_nsis.py）
dist\             发布产物：当前版本安装包 + SHA256SUMS.txt（唯一入库的 exe）
docs\             文档体系（总表见 docs/INDEX.md）
LICENSE / COPYRIGHT / THIRD-PARTY-NOTICES.md   许可、版权与第三方声明
FILELIST.txt      已跟踪文件的 sha256 清单（gen_filelist.py 生成）
```

## 引擎与硬件

引擎已从早期外部引擎整体替换为自研 CoreEngine，公开接口不变，宿主层（`ofx_plugin.cpp`）无需改动。GPU 只用于**人脸推理**（把 `onnxruntime.dll` 换成带 GPU 支持的构建版即自动启用）；磨皮自身的 GPU 路径是另一套 D3D11 compute 管线，见 `docs/design/design-gpu-smooth.md`。

## 恢复方法

仓库是源码状态的唯一可信来源。换机器或误删后：

1. clone 本仓库（代码、脚本、文档、测试夹具都在）；
2. 从交付包或已装机 bundle 取回 `onnxruntime.dll` 与 `yunet.onnx`，放进 `02_引擎资产\`；
3. 跑 `01_ofx插件源码\build.bat` 重编译，再 `install.bat` 装机；
4. `04_验收工具\verify.bat` 复核 28 项。

## 许可（License）

本项目以 **GNU General Public License version 2（仅第 2 版，GPL-2.0-only）** 发布，Copyright (C) 2026 Jian。

- 版权与授权声明：根目录 [`COPYRIGHT`](COPYRIGHT)
- 许可全文（GPL v2 原文，未作任何修改）：[`LICENSE`](LICENSE)
- 第三方组件与算法出处：[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)
- 每个源文件首行都有 `SPDX-License-Identifier: GPL-2.0-only`，机器可识别


