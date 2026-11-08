# 第三方组件与许可声明（THIRD-PARTY NOTICES）

本项目本体以 **GNU General Public License version 2（仅第 2 版，GPL-2.0-only）**
发布，全文见仓库根目录 `LICENSE`。本文件列出随本项目**分发**的第三方组件及其
许可条款。以下每一条都注明信息来源与文件指纹，便于复核。

分发规则很简单：MIT / BSD-3 都要求「在所有副本中保留版权声明与许可全文」，
所以只要发布了安装包或 DLL，本文件必须与 `LICENSE` 一起随包附带。

---

## 1. ONNX Runtime 1.20（onnxruntime.dll）

| 项 | 内容 |
|---|---|
| 文件 | `onnxruntime.dll`（x64 Windows） |
| 版本 | `1.20.20241119.10.5c1b7cc`（取自 PE 版本资源） |
| SHA-256 | `4cb41e89b8bf30578e1dd95e9c40292d61974a4bfcd666409302c4f0c5aa8ce0` |
| 版权 | Copyright (c) Microsoft Corporation |
| 许可 | MIT License |
| 来源 | https://github.com/microsoft/onnxruntime |

用途：加载并推理 `yunet.onnx`（人脸检测）。本项目以**未修改的动态库**形式分发它，
没有静态链接，也没有改动其二进制。

```
MIT License

Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

> ⚠ 待补：ONNX Runtime 自身还打包了 protobuf、abseil 等第三方依赖，它的官方发行包
> 里带一份 `ThirdPartyNotices.txt`。**正式对外分发安装包时应把那份一并附上**；
> 本仓库目前没有收录（本次只补到 ORT 本体这一层，这一点必须记住）。

---

## 2. YuNet 人脸检测模型（yunet.onnx）

| 项 | 内容 |
|---|---|
| 文件 | `yunet.onnx`（232,589 字节） |
| SHA-256 | `8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4` |
| 版权 | Copyright (c) 2020 Shiqi Yu <shiqi.yu@gmail.com> |
| 许可 | MIT License |
| 来源 | https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet |
| 训练上游 | https://github.com/ShiqiYu/libfacedetection.train |

许可依据要写清楚，这里有个容易踩的坑：**opencv_zoo 仓库根目录的 LICENSE 是
Apache-2.0，但 `models/face_detection_yunet/` 子目录自带一份 MIT LICENSE**
（其 README 的 License 段落也写明「All files in this directory are licensed under
MIT License」）。本项目取用的是该子目录下的模型文件，因此适用 MIT，而不是 Apache-2.0。
这一条是 2026-11-06 逐个文件核对过的 —— 早先的记录写成「MIT，via OpenCV Zoo」
结论没错，但没写清依据在子目录而不是仓库根。

（上游 libfacedetection 的**代码**是 3-clause BSD，Copyright (c) 2015-2019, Shiqi Yu。
本项目不分发它的代码，只分发重新训练/导出得到的 `.onnx` 权重，故此处以模型目录的
MIT 为准；若将来引入其源码，需要另行补 BSD-3 声明。）

```
MIT License

Copyright (c) 2020 Shiqi Yu <shiqi.yu@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## 3. OpenFX 头文件（sdk/include/*.h）

| 项 | 内容 |
|---|---|
| 文件 | `01_ofx插件源码/sdk/include/` 下的 17 个 `.h` |
| 版权 | Copyright OpenFX and contributors to the OpenFX project. |
| 许可 | BSD-3-Clause（每个文件头都有 `SPDX-License-Identifier: BSD-3-Clause`） |
| 来源 | https://github.com/ofxa/openfx |

用途：仅用于编译期声明 OFX 宿主接口，插件以动态库形式被宿主加载。

```
BSD 3-Clause License

Copyright OpenFX and contributors to the OpenFX project.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 4. 算法出处标注（MIT，仅标注、不分发代码）

源码注释里有若干「这条公式/这个口径来自哪个开源实现」的标注。这些上游都是 MIT
许可，本项目**没有分发它们的代码或二进制**（既没有静态链接，也没有把它们编译进
DLL），只在注释里写明公式来源。为避免把上游项目名散落在每一处注释里，正文统一
写成「该 MIT 实现 / 那份 MIT 实现」，具名出处集中在本节：

| 上游项目 | 许可 | 版权行（照抄上游 `LICENSE`） | 被引用的部分 | 用到哪里 |
|---|---|---|---|---|
| `dinjufen/lumina-beauty` | MIT（仓库根；见下方边界说明） | `Copyright (c) 2026 Lumina Beauty contributors` | `kComposeShader`、`kSkinBlurShader`、`skinMask`、`uSmoothProtect`、`uSmoothThreshold`、`smoothGrain` | 磨皮合成式、肤色判据、五官保护、阈值软膝、颗粒回填 |
| `wuxiwuhen/obs-beauty-filter` | MIT | `Copyright (c) 2026 wuxiwuhen` | `PSComposite` | Sobel 边缘门控的灵敏度口径（原值 ×10.0） |

**这两个上游的 MIT 全文（两份文本相同，仅版权行不同）：**

```
MIT License

Copyright (c) 2026 Lumina Beauty contributors
Copyright (c) 2026 wuxiwuhen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

> 上面把两个版权行并列写在同一份 MIT 文本下，是因为两份 `LICENSE` 除版权行外逐字相同。
> 若将来单独引用其中一个，按「一个上游一份文本」拆开即可。

**`dinjufen/lumina-beauty` 的许可边界（重要）**：该仓库不是全仓 MIT。根目录是 MIT，
但 `plugins/obs/` 是 **GPL-2.0-or-later**（因为它链接 libobs），`models/` 是
**Apache-2.0**。本项目只借鉴根目录 shader 里的公式与口径，**既没有引用 `plugins/obs/`
的代码，也没有引用 `models/` 的模型**。之所以要把这条写清楚：Apache-2.0 与
GPL-2.0 **不兼容**，一旦哪天把 `models/` 的东西并进来，整个授权结构就得重做。

**若将来把上述代码整段并入本项目**（而不是只对齐公式），本节要升级成第 1、2 节那样
的完整声明：保留版权行、附 MIT 全文、并在改动的源文件里注明来源。

## 5. 兼容性说明（给维护者看）

- GPL-2.0（仅第 2 版）与 **MIT、BSD-3-Clause 兼容**，可以并入并整体按 GPL-2.0 分发。
- GPL-2.0 与 **Apache-2.0 不兼容**。所以第 2 节那条「模型目录是 MIT、不是仓库根的
  Apache-2.0」不是文字游戏 —— 如果将来换成 Apache-2.0 的模型或组件，整个授权结构
  就要重做（通常只能改成 GPL-3.0 或放弃 GPL）。

## 6. 复核方法

三份文件的指纹都可以现场重算（在仓库根目录执行）：

```bash
# onnxruntime.dll 与 yunet.onnx 在 02_引擎资产/ 下
sha256sum "02_引擎资产/onnxruntime.dll" "02_引擎资产/yunet.onnx"
```

许可条款的原文来源：ONNX Runtime 的 `LICENSE`、
opencv_zoo `models/face_detection_yunet/LICENSE`、OpenFX 各头文件的 SPDX 行。
若上游条款更新，以官方仓库当前文本为准，并同步更新本文件。
