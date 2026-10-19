# NamaBeauty OFX —— DaVinci Resolve 美颜滤镜

一个给 DaVinci Resolve Studio 21.1 用的 OpenFX 滤镜，效果引擎是 FaceUnity NamaSDK，
跟 OBS 美颜插件同源。装上以后它就是一个普通滤镜：拖到片段上，拖滑块，检视器实时刷新。

这是本项目当前的主交付形态。早期做的「工作区 → 脚本」Lua 版已移出脚本菜单
（代码保留在 `C:\ProgramData\NamaBeauty\disabled_scripts\`），两条路线不再并存。

## 怎么装

已经装好了，位置是：

```
C:\Program Files\Common Files\OFX\Plugins\NamaBeauty.ofx.bundle\
```

要重新装（比如改过源码之后），管理员权限运行：

```
build.bat     :: 编译
install.bat   :: 打包并安装
```

### 什么时候要重启 Resolve

Resolve 把扫描结果缓存在
`%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`，
缓存以 `mtime + size` 判断是否失效：

- **重新编译过插件** → 二进制指纹变了 → **必须重启 Resolve** 才会重新扫描。
- **只是重启 Resolve、没改过插件** → 走缓存，不需要额外操作。

想确认 Resolve 认没认，直接看那个 xml 里有没有 `com.nama.davinci.beauty`，
并核对 `mtime` / `size` 与磁盘上的 `NamaBeauty.ofx` 是否一致。

## 怎么用

1. 打开 Resolve，进 **Color** 页面（或 Edit 页面）
2. 左侧 **Effects Library → OpenFX**，找到分组 **NamaBeauty**
3. 把 **NamaBeauty 美颜** 拖到片段上
4. 右侧 **Inspector** 里出现参数面板，拖滑块调效果

分组名叫 `NamaBeauty`，搜不到就在搜索框输入 `Nama`。

**注意：OFX 插件不在「工作区 → 脚本」里。** 那是 Fusion 脚本的入口，两条通道完全独立。

## 参数

八个效果滑块，刻度都是 0–100，0 表示不做该项。

| 参数 | 默认 | 实测效果强度 |
|---|---|---|
| 磨皮 | 70 | 最强项，肉眼明显 |
| 清晰 | 0 | 很强，满量程改动约 89% 像素 |
| 美白 | 15 | 很强，满量程改动约 99% 像素 |
| 红润 | 30 | 中等，满量程改动约 58% 像素 |
| 锐化 | 0 | **只要磨皮不为 0，锐化完全不起作用** |
| 五官立体 | 0 | 较弱，满量程改动约 16% 像素 |
| 亮眼 | 0 | 很弱，满量程只改动约 0.65% 像素，要放大才看得出 |
| 去黑眼圈 | 0 | 很弱，满量程只改动约 0.93% 像素 |

另外两项：

- **磨皮算法**：双边滤波（最强，推荐）/ 高斯（中等）/ 弱磨皮。
  实测细节能量越低越平滑：双边 2.358 < 高斯 2.719 < 弱磨皮 3.762 < 不磨皮 4.180。
- **只磨皮肤区域**：开了之后只处理肤色区域，背景和衣服不会被磨。

### 三个必须知道的坑

1. **磨皮和锐化互斥。** 磨皮一开，锐化的改动量实测恒为 0。想要锐化，先把磨皮拉到 0。
2. **八项全为 0 时画面完全不变。** 引擎内部挂了一个全局的 `ziran2` 滤镜（强度 0.4），
   代码里做了处理：八项都归零时把滤镜一并关掉，并直接短路透传，输出与输入逐字节相同。
   只要有任意一项不为 0，滤镜就恢复 0.4，此时**整帧**（含背景）都会有轻微色调变化。
3. **美颜带人脸识别。** 画面里没有人脸时只有滤镜生效，磨皮/亮眼这类定向效果看不出变化。

## 性能

在 1080×1920、画面检出人脸的前提下实测（数字取自插件自己写进 `ofx.log` 的分段耗时）：

| 输入像素格式 | 转成 RGBA8 | NamaSDK | 转回原格式 | 合计 | 等效帧率 |
|---|---|---|---|---|---|
| 8bit（Byte） | ~0.8 ms | 11–15 ms | ~0.8 ms | **~13–16 ms** | 60–75 fps |
| 32bit 浮点（Float） | ~3.5 ms | 15–16 ms | ~3.5 ms | **~18–20 ms** | 50–55 fps |

耗时的大头在 NamaSDK 自己（人脸检测 + 皮肤处理），像素格式转换只占 2–7 ms。
同机负载高时 SDK 那一段会涨到 20–30 ms，仍够 30fps 时间线实时预览。

三条关键设计：

- **引擎常驻 + 挂载时预热。** fuSetup、人脸模型、美颜道具要花约 2.5 秒，
  这段开销被放在 `createInstance`（也就是把效果拖到片段上的那一刻），
  不再落在渲染线程的第一帧上。实测：挂载 2.5 秒，之后首帧只要 40–50 ms。
- **每帧零堆分配。** 像素缓冲按渲染线程复用，1080p 每帧省掉两笔 8MB 的分配/释放。
- **转换走 AVX2。** `build.bat` 带 `/arch:AVX2`；CPU 不支持时自动退回标量分支。

如果回放仍然卡顿，用 **Playback → Render Cache** 缓存片段。

## 出问题怎么看

插件自带日志：

```
C:\ProgramData\NamaBeauty\logs\ofx.log
```

记录 `onLoad` / `describe` / `createInstance`（含预热耗时）/ 每个 render 的
尺寸、像素深度、rowBytes、人脸数、各段耗时；默认只详细记前 8 帧，之后每 120 帧抽一条。

| 环境变量 | 作用 |
|---|---|
| `NAMA_OFX_LOG_ALL=1` | 逐帧记录，用于定位偶发问题 |
| `NAMA_OFX_LOG=0` | 完全关闭日志 |
| `NAMA_OFX_LOG=<路径>` | 改日志落盘位置 |
| `NAMA_ENGINE_DIR=<目录>` | 覆盖引擎 DLL 目录（默认取 .ofx 自己所在目录） |

另外 **工作区 → 控制台（Console）→ Show All** 里能看到宿主侧的报错。

## 稳定性设计

NamaSDK 是逆向对接的闭源 DLL，跑在宿主的进程里。为了不让它把 Resolve 带崩：

- **每个 SDK 入口都包了 SEH 兜底。** 一旦抛访问违例，只让这一帧透传，
  并把引擎标记为「已停用」，本进程内不再调用 —— 否则接下来每一帧都会再崩一次。
- **初始化失败最多重试 3 次。** 每次重试要付约 2.5 秒，不能让它跟着每一帧反复发生。
- **GL 上下文用完必须还原。** 调用前保存调用线程原本的 DC/RC，调用后还原；
  切换失败时不还原（否则等于用一对旧值做无意义的 `wglMakeCurrent`）。
- **带析构函数的线程局部对象一律不用。** 缓冲用 `__declspec(thread)` 存裸指针、
  指向刻意不释放的分配，避免宿主 FreeLibrary 之后线程退出时析构函数指针悬空。
- **全 0 时走 IsIdentity。** 告诉宿主直接取源 clip 的图像，连 render 都不必调。

## 文件结构

```
ofx\
  sdk\include\       官方 OpenFX 头文件（17 个）
  examples\          官方最小示例 invert.cpp / basic.cpp
  src\nama_engine.*  NamaSDK 驱动层：隐藏 GL 窗口 + 上下文还原 + SEH 兜底 + 逐帧渲染
  src\ofx_plugin.cpp OpenFX 协议层：宿主对话、参数定义、日志、渲染
  build.bat          编译（手动设 INCLUDE/LIB/PATH，不依赖 vcvars64）
  install.bat        打包成 .ofx.bundle 并安装
  bundle\            打包产物
```

**注意：`.bat` 文件里不能写中文注释。** cmd.exe 按 GBK 解释批处理，
中文注释会被当成命令。源码里的中文是安全的（编译用了 `/utf-8`）。

## 自测

不开 Resolve 也能验证插件是否正常：

```
:: 协议回归：load → describe → describeInContext → createInstance → render
python C:\...\beauty_probe\mock_host.py

:: 再加 1080p 性能基准
python C:\...\beauty_probe\mock_host.py --bench
```

用真实素材帧跑基准（比合成图准，纯黑底会干扰人脸检测）：

```
set NB_BENCH_RAW=C:\...\beauty_probe\real_frame.raw
set NB_BENCH_W=1080
set NB_BENCH_H=1920
python C:\...\beauty_probe\mock_host.py --bench
```

`real_frame.raw` 是裸 RGBA 帧，用 ffmpeg 从素材抽：

```
ffmpeg -ss 5 -i 素材.mp4 -frames:v 1 -pix_fmt rgba -f rawvideo real_frame.raw
```

回归覆盖：8bit 渲染、连续 3 帧、float 渲染、**全 0 必须逐字节透传**、
**IsIdentity 在全 0 时回 OK / 非全 0 时回 ReplyDefault**。

## 卸载

删掉 `C:\Program Files\Common Files\OFX\Plugins\NamaBeauty.ofx.bundle` 整个目录，
然后重启 Resolve。
