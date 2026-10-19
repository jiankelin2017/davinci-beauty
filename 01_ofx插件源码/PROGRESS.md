# NamaBeauty OFX 插件 —— 状态

最后更新：2026-10-14 22:00

## 一句话现状

**阶段 1–3 全部完成并验证通过。** 插件已重编译、重装到 OFX 目录，
协议回归（含新增的全 0 透传 / IsIdentity）全绿，1080×1920 实测 60–75 fps（8bit）。
剩下唯一没做的是「在真实 Resolve 界面里挂一次」，需要用户手动确认。

## 交付位置

| 内容 | 路径 |
|---|---|
| 已安装插件 | `C:\Program Files\Common Files\OFX\Plugins\NamaBeauty.ofx.bundle\` |
| 源码 | `ofx\src\ofx_plugin.cpp`、`ofx\src\nama_engine.{h,cpp}` |
| 编译 / 安装 | `ofx\build.bat`、`ofx\install.bat` |
| 使用说明 | `ofx\README.md` |
| 模拟宿主测试 | `..\beauty_probe\mock_host.py`（`--bench` 加性能基准） |
| 引擎直测 | `..\beauty_probe\selftest_ofx.py` |
| 插件自带日志 | `C:\ProgramData\NamaBeauty\logs\ofx.log` |

## 本轮（阶段 1–3）改了什么

### 阶段 1 —— 可验证性

新增插件自带日志 `ofxLog()`，落 `C:\ProgramData\NamaBeauty\logs\ofx.log`：

- 线程安全（CRITICAL_SECTION），4MB 上限，超限整文件重来
- 记录 `onLoad`（含宿主名）/ `describe` / `describeInContext` /
  `createInstance`（含引擎预热结果与耗时、ownerTid）/ 每个 render 的
  尺寸、像素深度、rowBytes、人脸数、**分段耗时**、draft/interactive/seq 标志
- 默认详细记前 8 帧 + 每 120 帧一条；`NAMA_OFX_LOG_ALL=1` 逐帧；
  `NAMA_OFX_LOG=0` 关闭；`NAMA_OFX_LOG=<路径>` 改位置

### 阶段 2 —— 稳定性

| # | 问题 | 处理 |
|---|---|---|
| 1 | `renderMain` 用 `srcRect` 算宽高却写进 `dstPtr`，忽略 `dstRect` | 改为分别取 src/dst bounds，取交集，不一致时记警告 |
| 2 | 八项全 0 仍要走完整 SDK 链路 | 短路透传（逐字节 memcpy）+ 新增 `kOfxImageEffectActionIsIdentity` |
| 3 | 1842 ms 引擎初始化落在渲染线程首帧 | 挪到 `createInstance` 预热，并提供 `lastSdkMs()` 计量 |
| 4 | SDK 一旦抛访问违例会带崩 Resolve | 所有 SDK 入口包 `__try/__except`，崩溃后标记 `poisoned_` 本进程内不再调用 |
| 5 | 初始化失败后每帧重试，每帧重付 2.5 秒 | 失败计数上限 3 次 |
| 6 | `wglMakeCurrent` 失败被静默忽略 | 检查返回值；失败时不执行还原（避免无意义的切回） |
| 7 | 参数未变仍每帧下发 10 次 setter | 缓存上次应用的 `NamaSettings`，未变则跳过 |

### 阶段 3 —— 性能

| # | 改动 | 效果 |
|---|---|---|
| 1 | 像素缓冲从每帧 `std::string::assign` 改为**按渲染线程复用** | 1080p 每帧省两笔 8MB 分配/释放 |
| 2 | `float32 ↔ RGBA8` 转换改 AVX2 内联函数 + 标量兜底 | 1080p 单程约 0.8 ms（8bit）/ 3.5 ms（float） |
| 3 | `build.bat` 加 `/arch:AVX2 /std:c++17` | — |

**线程局部缓冲刻意不用 `thread_local std::vector`**：那是带析构函数的线程局部对象，
宿主 `FreeLibrary` 之后若有线程退出，DLL 里的析构函数指针会悬空。
改成 `__declspec(thread)` 存裸指针、指向刻意不释放的分配，进程退出交给 OS 回收。

## 实测数据（1080×1920，画面检出人脸）

数字取自插件自己写进 `ofx.log` 的分段耗时：

| 输入格式 | 转 RGBA8 | NamaSDK | 转回 | 合计 | 等效帧率 |
|---|---|---|---|---|---|
| 8bit | ~0.8 ms | 11–15 ms | ~0.8 ms | **13–16 ms** | 60–75 fps |
| float32 | ~3.5 ms | 15–16 ms | ~3.5 ms | **18–20 ms** | 50–55 fps |

同机负载高（Resolve 正在跑）时 SDK 段会涨到 20–30 ms，仍够 30fps 实时预览。

**预热确实生效**：`createInstance` 2.5 秒，之后首帧只要 40–50 ms（原来是 1842 ms 全砸在首帧）。

## 回归结果（`mock_host.py`）

```
8bit 渲染  : PASS      全0 透传   : PASS
连续 3 帧  : PASS      IsIdentity : PASS
float 渲染 : PASS      总判定     : PASS
```

新增的两项断言很关键：

- **全 0 逐字节透传**：`与源最大逐字节差=0`（不是「接近」，是 0）
- **IsIdentity**：全 0 时返回 `kOfxStatOK` 并把 `kOfxPropName` 指向 `Source`；
  参数非 0 时返回 `kOfxStatReplyDefault`(14)

## 一个重要的排查发现

Resolve 把 OFX 扫描结果缓存在
`%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml`，
以 `mtime + size` 判失效。本插件早已在里面：

```xml
<binary bundle_path="...\Plugins\NamaBeauty.ofx.bundle"
        path="...\Contents\Win64\NamaBeauty.ofx"
        mtime="1791976542" size="49664" status="0"/>
<plugin name="com.nama.davinci.beauty" api="OfxImageEffectPluginAPI" api_version="1" ...>
```

这解释了为什么某些会话的 `davinci_resolve.log` 里没有 `OFX: loading ...` ——
走缓存时不再打印。**改过插件后必须重启 Resolve**（新二进制是
`size=59904 / mtime=1791986187`，指纹已变，会触发重扫）。

## 待用户确认

- **在真实 Resolve GUI 里挂一次**：剪辑页 → 效果库 → OpenFX → `NamaBeauty` → `NamaBeauty 美颜`。
  挂上后把 `C:\ProgramData\NamaBeauty\logs\ofx.log` 发我，就能确认真实宿主给的
  是 8bit 还是 float、render 窗口是不是整帧、以及真实人脸检测情况。

## 已知的「没做」

- **未实现的动作**：`kOfxImageEffectActionGetClipPreferences`（让输出深度跟随输入，
  可再省一次转换）。因为没有真实宿主可验证，宁可不做也别做错。
- **仍保留 NamaSDK 的 CPU 内存接口**。Beauty Box 用 DirectML/D3D12，像素不离开显存；
  `fuRenderItemsEx` 只吃 CPU 指针，这个架构性差距优化补不上。
- **参数未分组**。目前是单个 `Main` 页；Beauty Box 那样分「皮肤/调色/细节/眼部/五官」的
  分组页属于下一阶段。
- **参数未扩充**。去油光、去法令纹等要先探测 SDK 实际认哪些参数名，不能凭空加。

## 已定的设计决策（别改回去）

- **sharpen 默认 0**：源码 g_params[] 里是 20，但实测磨皮非 0 时锐化恒无效果，给 0 才诚实。
- **filter_level 动态**：八项全 0 时为 0，否则 0.4（与原 OBS 插件默认一致）。
- **支持 float**：降到 8bit 处理再升回去，有损但保证 Color 页面能加载。
- **不做 Rust 版**：本机无 Rust 工具链；OFX 是 C ABI，Rust 不解决本次三个真难点。
- **Lua 脚本版已移出菜单**（`C:\ProgramData\NamaBeauty\disabled_scripts\`），
  代码保留但不再维护，两条路线只留 OFX 一条。

---

## 2026-10-14 22:30 播放定格 bug 修复（build 1.3）

**现象**：剪辑页/调色页挂上插件后播放，预览窗画面不动、声音照常。

**根因**：`renderMain` 向宿主取图时把时间写死成 `0.0`：

```cpp
// 修复前
gEffectHost->clipGetImage(outputClip, 0.0, &region, &outputImg);
gEffectHost->clipGetImage(sourceClip, 0.0, &region, &sourceImg);
```

render 动作 inArgs 里的 `kOfxPropTime` 是「这一次要渲第几帧」。不读它、传 0.0，
等于播放头走到第 N 帧我们仍向宿主索要第 0 帧 —— 画面自然定格在第一帧。

**修复**：读 inArgs 的 `kOfxPropTime`，原样传给两次 `clipGetImage`；
顺带读 `kOfxImageEffectPropRenderScale` 用于诊断。日志加 `time=%.3f scale=%.2f`。

**回归**：`mock_host.py` 增加「每帧不同时间」断言（96.5 / 103.75 / 111.0 / 118.25），
写死 0.0 会立刻判红；连续帧测试不再复用同一个 inArgs。

**顺带修的两个真问题**

1. 夹具 `mock_host.py` 没实现 `propGetDoubleN`，套件槽位是 0，插件一调用就空指针崩溃。
2. `install.bat` 先 `rmdir /s /q` 目标目录再 xcopy。Resolve 锁着 DLL 时删不掉 DLL，
   但 `model/`、`graphics/` 被删成功、xcopy 又整体失败没拷回 → 安装目录半空 →
   引擎报「资源文件缺失」并**静默降级为透传**（美颜完全不生效，界面不报错）。
   新脚本改为逐文件替换，锁住的文件先 `move` 改名（Windows 允许重命名已映射文件）再 copy，
   最后逐项校验。注意 .bat 必须是 CRLF，LF 会让 cmd 把 `rem` 当命令执行。

**验证**：1920x1080 基准 20.2ms/帧 ≈ 49fps；插件日志 `render #4 time=118.250` 确认时间已透传。

---

## 2026-10-14 23:00 代码审查整改（build 1.3，同版本号）

完整报告见仓库根目录 `CODE_REVIEW.md`。本轮改了 7 处，全部经夹具回归验证。

**真实日志暴露的头号问题**：Resolve 会以 `scale=0.15`（161×80）调 render，这种尺寸下
人脸检测必然失败（`faces=0`）却仍付 17.7ms。夹具以前**永远只发 `scale=[1.0,1.0]`**，
结构性地测不出这类问题。

| # | 问题 | 处理 |
|---|---|---|
| B1 | 代理/缩略图帧白付 SDK 耗时 | 新增 `isThrowawayRender()`；`isIdentity` 与 `renderMain` 双处拦截。实测 `scale=0.15` 帧 **15.9ms → 0.6ms**，逐字节差 **134 → 0** |
| B2 | `SupportsMultipleClipDepths=0` 与「声明支持 Byte+Float」矛盾 | 改为 `1`，这是 B3 的前提 |
| B3 | 未实现 `GetClipPreferences` | 已实现：输出深度/组件/PAR 跟随输入 + `FrameVarying=true`。⚠️ 属性名须手写拼接（头文件无宏） |
| B4 | 预热失败只写日志 | 补 `gMsgHost->message` 上报，用户可见 |
| B6 | 参数页在参数之后定义 | 改为先建页 + 每参数 `kOfxParamPropParent` 归类 |
| B7 | `f32_to_u8` 标量分支依赖 UB 处理 NaN | 改显式 `!(v >= 0)`，与 AVX2 分支严格对齐 |
| B9 | `warned` 是函数级 static（跨实例共享） | 移入 `InstanceData` |

**夹具已补 3 个用例**（代理透传 / 缩略图恒等 / ClipPrefs），现共 8 项全 PASS。

**未做**：B5（引擎单例 vs 多实例的参数串扰）保留现状 —— `Unsafe` 声明已保证结果正确，
只是多实例交替时参数缓存会失效；彻底根治要让 `item_`/`applied_` 变成 per-instance。

**待真实 Resolve 确认**：B3 的 `GetClipPreferences`（陷阱多，backout 路径见 CODE_REVIEW.md 第六节）。
