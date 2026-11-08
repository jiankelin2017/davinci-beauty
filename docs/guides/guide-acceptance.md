# 验收：发版前的自动化检查

> 适用代码版本：1.3.6.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：Jian ｜ 替代文档：原 `04_验收工具/README.md`

**这份文档管**：怎么跑验收、28 项各查什么、失败先查哪、基线怎么维护。
**不在这里管**：出包看 `guide-package.md`，构建与部署看 `guide-dev-workflow.md` 与 `reference-architecture.md` §3–4，性能测量看 `reference-performance.md` §5。

---

## 0. 摘要

**一句话**：`python acceptance.py` 退出码 0 才算能发版。当前基线 **PASS 28 / FAIL 0 / WARN 0**。

五条结论：

| # | 结论 | 详见 |
|---|---|---|
| 1 | 28 项分 7 组：版本 5、参数 5、回归 1、并发 1、接口 2、装机 2、**工程 12** | §2 |
| 2 | 「工程」组占最多，且全是**静态判据**（读文件、正则匹配，不启动插件）——跑得快，抓的都是"发出去才发现"的错 | §2.6 |
| 3 | 「回归」是唯一耗时的（18 例逐字节，约 12.6 s），也是**性能优化的唯一防线**：变快了 ≠ 少算了 | §3 |
| 4 | 装机产物字节比对是**硬判据**：构建可复现，md5 不一致就是没跑 `install.bat`，不是"正常现象" | §2.1 |
| 5 | 「并发」组用**独立进程 + 20 秒看门狗**，卡死时自杀并打印 `DEADLOCK rows=.. n=..`，不会把 CI 挂住 | §4 |

---

## 1. 怎么跑

```bat
python acceptance.py                        :: 全部检查，退出码 0 = 通过
python acceptance.py --build                :: 先重新编译再验（发版时用）
python acceptance.py --skip 回归             :: 跳过某组
python acceptance.py --report 验收报告.txt    :: 落盘一份
```

或者双击 `verify.bat`（等同无参数运行）。

`--skip` 接受组名：`版本` / `参数` / `回归` / `并发` / `接口` / `装机` / `工程`。多个用空格分隔。

**退出码只有两个值**：`0` = 无 FAIL；`1` = 有 FAIL。**WARN 不阻断退出码**，只在汇总里单独列一行——判定标准是"发版条件是否成立"，不是"本机装没装编译器"（找不到 MSVC/SDK 时并发组记 WARN）。所以 **CI 看退出码，人还得看一眼 WARN**。

---

## 2. 28 项清单

> 下表按 2026-11-08 全量运行的实际输出整理。项数变化时先改这张表，再改脚本。

### 2.1 版本（5 项）

| 检查 | 判据 / 为什么 |
|---|---|
| `NAMA_BUILD_STR` 与宏一致 | 四段必须自洽。版本号唯一出处在 `nama_engine.h` 的 `NAMA_BUILD_*` |
| 二进制内嵌版本串 | `NamaVersion()` **只编码 MAJOR/MINOR，PATCH 位被有意丢弃**（保持对外 ABI），所以"装的到底是哪个版本"只能靠二进制里的字符串判 |
| `NamaVersion()` 编码 | `== (MAJOR<<16) \| MINOR`，两段各 16 位，不是每 8 位一段 |
| 装机产物**行为等价** | 让装机 DLL 跑同一组黄金用例，输出必须与编译产物相同 |
| 装机产物**字节比对** | **硬判据**。`build.ps1` 的 `Reset-PeTimestamp` 把 COFF 与 `IMAGE_DEBUG_DIRECTORY` 两个 `TimeDateStamp` 清零，同源码重编 md5 恒定（实测 `5655699…`）。不一致 = 没 `install.bat`，直接 FAIL。注意 `/Brepro` 在本工具链上单独**不生效**，别把它当成可复现的原因 |

### 2.2 参数（5 项）

| 检查 | 判据 / 为什么 |
|---|---|
| 三张表长度一致 | `P_*` 枚举 / `kParams[]` / `keys[]`，当前 14 / 14 / 14 |
| 面板名 ↔ SDK key 逐位对齐 | v2.2.3 踩过：换序时漏改 `keys[]`，后果是"拖 A 滑块、下发成 B 参数"，**编译期完全不报错** |
| 默认档 50/30/30 其余 0 | 用户决策，不是默认值漂移 |
| 锐化保持隐藏 | 当前隐藏项是 `delspot_level`、`sharpen`。磨皮 >0 时锐化完全无效，暴露出来用户会以为插件坏了 |
| 美型三项沉底 | Resolve 的 OFX 面板**不渲染 Page 也不渲染 Group**，只能靠定义顺序控制布局 |

### 2.3 回归（1 项）

18 例逐字节比对黄金基线，耗时长（约 12.6 s），输出 `18/18`。细则见 §3。

### 2.4 并发（1 项）

`parallelRows` 分块守恒，5227 组。细则见 §4。

### 2.5 接口（2 项）

| 检查 | 判据 / 为什么 |
|---|---|
| mock host OFX 契约 | `mock_host.py` 当前 **23 项** PASS。不开 Resolve 验插件与宿主的接口约定 |
| 自测链路 LoadLibrary→初始化→渲染 | 端到端跑通 |

### 2.6 装机（2 项）

| 检查 | 判据 / 为什么 |
|---|---|
| 引擎资产齐全 | 缺一个插件就起不来 |
| 无 `.locked-*` 残留 | `install.bat` 替换被占用 DLL 时会改名留垃圾；Resolve 没关干净就一直堆，攒几次上百 MB |

### 2.7 工程（12 项）

这一组全是静态判据，六项是"三处都要有"的形式（`install.bat` / `PAYLOAD_PLAN` / NSI 指令各查一遍）——因为这三个地方任何一个漏了，都会产出"能装但不完整"的包。

| 检查 | 判据 / 为什么 |
|---|---|
| `install.bat` 覆盖必备资产 | 当前 2/2。v2.3.1 踩的大坑：装机脚本从来没拷过 `ai_face_processor_pc_plain.bundle`（旧 bundle 里那份是手工放的），全新装机就缺资产，`NamaSelfTest` 直接崩 `0xC0000005` |
| 安装包 `PAYLOAD_PLAN` 覆盖必备资产 | 同上，安装包那条链路 |
| 安装包 NSI `File` 指令覆盖必备资产 | 同上，NSI 打包那条链路 |
| `install.bat` 收录许可文件 | 当前 2/2。漏了就是"能装但合规不全" |
| `PAYLOAD_PLAN` 收录许可文件 | 同上 |
| NSI `File` 指令收录许可文件 | 同上 |
| UI 无旧名残留 | 正则排除 `NamaBeautySDK/Settings/Engine/…` 后不得有 `NamaBeauty`。改名做一半没人发现 |
| 面板文案统一新名 | 只认插件级文案（变量 `props`）；参数级 `pp` 是"磨皮"这类功能名，本来就不带产品名 |
| 下拉项无括号后缀 | 用户要求简洁。取文案前先剥注释，否则注释里的历史叫法会被当成真字符串 |
| 文档版本号同步 | 5 份必须含当前版本串：`README.md`、`说明.txt`、`docs/INDEX.md`、`docs/CHANGELOG.md`、`docs/guides/guide-package.md` |
| 文档引用的安装包版本 | 任何 `DaVinciBeauty_Setup_vX.exe` 引用（除非该行含「历史」）必须等于当前版本。原先四份文档全线停在旧版本，连不存在的安装包都还列着 |
| 批处理编码 GBK+CRLF+无 chcp | 当前 17 个 `.bat` 全合规。实测三种失败形态都会让 cmd 把行切开：UTF-8 中文（cmd 按 GBK 解析，多字节吃掉行边界）、`chcp 65001`（中途换代码页，后续行字节偏移错位）、裸 LF（批处理只认 CRLF） |

> ⚠ 这条检查**只试 GBK 解码**，而 UTF-8 中文的字节恰好也能通过——所以它对"文件里有 UTF-8 中文"是漏的。`install.bat` 现在就有 3 行 UTF-8（`:24` echo、`:51-52` rem），936 代码页下 echo 会乱码。要真查，得加一条「含非 ASCII 且能按 UTF-8 解出 CJK → 违规」。是否改见 `docs/INDEX.md` 待确认项。

---

## 3. 逐字节回归基线

```bat
python baseline.py check              :: 比对（默认）
python baseline.py update             :: 重新生成基线（有意改像素后必做）
python baseline.py show               :: 打印当前清单
python baseline.py check --only blur62 sh50
```

- **18 个用例**：磨皮全档 / 美白 / 锐化 / 清晰 / 全局锐化 / 美型 / 三种磨皮模式 / 三种算法档。
- `shape100` 是唯一抓出过 v2.2.3 换序 bug 的用例，**不要删**。
- **raw 不入库**：15×2.6 MB ≈ 41 MB，每次现生成；仓库只留 `baseline_manifest.txt`（1 KB）。
- manifest 记录输入帧 md5 与插件版本。输入换了会显式提示"需重新标定"，不会变成一堆看不懂的 md5 失配。
- `update` 每个用例跑两次确认 md5 一致——验的是**测试本身是否确定性**，避免把测量抖动当成输出变了。
- **`update` 不需要任何环境变量**（1.3.6.0 起）。1.3.5.0 及之前曾要加 `set BEAUTY_INFER_THREADS=1`，那是在绕 `toneColor()` 的数据竞争；竞争已修，绕法随之作废。
- 输入帧 `input_face.png` 自带在仓库内，换台机器 clone 下来就能跑。

**偶发 17/18 曾是数据竞争，1.3.6.0 已修**：根因是 `toneColor()` 的 Sobel 门控读正被并行写的 `dst`，实测 1.5% 概率输出差 1 级。现在 `blur62_wh100` / `cl50` 各跑 1200 次都是单一 md5。`check` 保留的「失配自动复跑一次」仍有意义（防的是真抖动），但**再出现复跑一致的情况应当当成异常信号去查，而不是当成噪声**。机制与实验数据见 `reference-performance.md` §5.4。

---

## 4. 并发回归台架（5227 组）

`beauty_pool.cpp` 的 `parallelRows()` 是磨皮 / 色调 / 掩码共用的按行并行入口。它靠一个计数器判断"本代是否算完"，而这个数一度写成**线程数**而不是**块数**：`chunk = ceil(rows/nt)`，实际块数 `ceil(rows/chunk)` 可能小于 `nt` —— 计数器永远减不到 0，调用线程永久等待。而它当时还持着引擎的全局递归锁，**整个插件（含其它实例）一起卡死**。

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File pool_regress.ps1          :: 编译（按需）+ 跑
powershell -NoProfile -ExecutionPolicy Bypass -File pool_regress.ps1 -Force   :: 强制重编
```

- 扫描 `(rows 1..320) × (nt 1..16)` 再加 1080p 等真实尺寸，共 **5227 组**；判据是**每一行恰好被回调一次** —— 漏行、重行、块越界都能抓出来。
- 自带 **20 秒看门狗**：卡住时打印 `DEADLOCK rows=.. n=..` 并直接终止（退出码 2），不会把 CI 挂住。
- 找不到 MSVC/SDK 时以退出码 **9** 返回，`acceptance.py` 记为 WARN 而非 FAIL。

> 旧代码复现位置比预期更早：原以为要 `rows=65, nt=16` 才触发，实际看门狗在 `rows=4, n=3` 就判定 `DEADLOCK`。这条回归的价值在于它扫全组合，不靠"想当然的边界值"。

---

## 5. 失败定位速查

| 失败项 | 先查 | 再去 |
|---|---|---|
| 装机产物字节比对 FAIL | 是不是改完代码没跑 `install.bat` | `guide-dev-workflow.md` |
| 装机产物行为等价 FAIL | 装机 DLL 是不是旧版：看 `ofx.log` 的 `onLoad build=` | `reference-architecture.md` §6 |
| 参数组"面板名 ↔ SDK key 对齐" FAIL | `keys[]` 顺序与 `kParams[]` 是否同步 | 该组 FAIL 会导致"拖 A 下发 B"，编译期不报错 |
| 回归 FAIL（某个用例） | 是否故意改了像素 | 有意改 → `baseline.py update`；无意改 → 查该用例覆盖的参数 |
| 回归 17/18 | 1.3.6.0 起不该再出现；出现即为真问题，先查 `toneColor` 类并行读写 | `reference-performance.md` §5.4 |
| 并发组 WARN | 本机有没有 MSVC/SDK | 退出码 9 是环境问题，不是回归失败 |
| 并发组 FAIL（`DEADLOCK`） | `rows` / `n` 两个数 | 分块守恒又破了 |
| 工程组"资产/许可" FAIL | 三处（`install.bat` / `PAYLOAD_PLAN` / NSI）哪一处缺 | 静态判，装机之前就该抓出来 |
| 文档版本号同步 FAIL | 哪份文件没跟上，输出会列出文件名 | 改文档，不是改脚本 |

---

## 6. 已知噪声（别当失败）

| 现象 | 说明 |
|---|---|
| `data check fail` | 旧版 SDK 运行时往 stderr 打的内部校验日志，不影响结果 |
| `NamaSelfTest` 中文路径乱码 | UTF-8 被按本地码页输出，只在初始化失败时可见，不影响正常路径 |
| 回归偶发 17/18 | 1.3.6.0 已修数据竞争，不该再出现；见 §3 |
| 少一项「编译」 | 不带 `--build` 时不触发编译步骤（它不属于 7 个检查组之一），清单里少这一行是正常的 |

---

## 7. 发版流程

1. 改源码 → 升 `nama_engine.h` 的 `NAMA_BUILD_*`
2. `build.bat`
3. `install.bat`（**先完全退出 Resolve**）
4. `python acceptance.py --build --report 验收报告_vX.Y.Z.txt`
5. 若**有意**改了输出（不只是优化）：`baseline.py update` 重标基线，并在 `docs/CHANGELOG.md` 写明改了什么、降级点在哪
6. 打开 Resolve 实机确认 UI 与播放 —— **UI 类改动脚本测不出来，必须看截图**
7. 要归档 / 分发：`python package.py --update --zip`（见 `guide-package.md`）

---

## 附 A：dump 取证通道（看像素，不靠猜）

真机出现"白块""接缝"这类问题时，日志只有占比没有像素，必须落盘看图。

```bat
python dump_switch.py on                       :: 等价于把 beauty.ini 的 dump_enable 改成 1
:: 用 诊断_验证羽化.bat 跑一遍 → 停在问题帧按 ← → 触发重渲染
python dump_analyze.py [目录] [seq]            :: seq 可直接传 re
python dump_switch.py off                      :: 务必关掉，否则一直写盘
```

**开关为什么在 ini 而不是环境变量**：Resolve 从桌面图标启动时，bat 里 `set` 的变量继承不到。1.1.4.0 那次验证因此空手而归——`dumpDir` 成了空指针，日志留下 `诊断导出 dump_re 640x394 -> (null)`。现在三级兜底（环境变量 > `beauty.ini` > 默认目录），写文件前先判空，开关没开时也打一行 `诊断导出未启用`，把"分支没进"和"写失败"分开。

**两组文件，别混用**：

| 文件 | 什么时候写 | 用途 |
|---|---|---|
| `dump_N_{src,dst,mask}`（N=0..5） | 播放阶段，前 6 个"检出脸且有掩码"的帧 | 看整体趋势 |
| `dump_re_{src,dst,mask}` | 暂停后按 ← → 强制重渲染同一帧时覆写 | **看问题那一帧**（`reLeft=20` 防爆盘） |

**四个判据，用来分开两种修法相反的成因**：

| 输出 | 读法 |
|---|---|
| 变化区 bbox + **填充率** | 矩形 ≈1.00、椭圆 ≈0.785、更低说明是真实轮廓 |
| 变化区**掩码直方图** | 满值（≥250）占 80% 以上 = 边界是硬的（羽化没进去） |
| 变化区在 src 里的**颜色与饱和度** | R>G>B 且有饱和 → 真皮肤；接近中性灰/米色 → 衣物被误判 |
| **提亮幅度** | 美白 LUT 中调只有 8~10 级，明显超出就不是美白造成的 |

另出两张图：`analysis_N.png`（原图 + 掩码红叠 + 变化绿框）、`analysis_N_diff.png`（8 倍增益热图）。PPM/PGM 零依赖，行序已按 `imgBottomUp_` 翻正，三张叠图对得齐。

**验脖子几何要用真机帧，验肤色跟随夹具够用**。`input_face.png` 上脖子带下缘是被肤色截断的，几何不是限制项，`neck_taper` 换值后掩码几乎不变（1.1.6.0 实测）；而 `neck_skinend` 吃的正是"肤色在哪结束"，同一张夹具就能测出（1.1.7.0 实测差异 3,274 px、bbox `x293..442 / y533..570`、最大 10 级）。

> **只看"掩码覆盖率有没有降"是无效判据。** 1.1.7.0 第一版覆盖率确实降了，输出却逐字节不变——缩掉的地方本来就不落笔。真正判据是"同一帧两版输出的差异 bbox"。

---

## 附 B：周边工具

| 工具 | 用途 | 注意 |
|---|---|---|
| `perf_ab.py --res 720p --frames 3 <DLL>` | 两个 DLL 交替测量取最小值 | 同一份二进制在不同时段能差 15%，只有交替跑才可比 |
| `logcheck_gpu_timing.py [日志]` | 逐帧闭合验算 + 热状态分布 | 见下方两个坑 |
| `cleanup.py [--apply]` | 清理 `locked`/`build`/`probe`/`external` 四类 | 只删白名单，不做递归通配；`KEEP` 清单永远碰不到 |
| `package.py` | 生成整合包 | 见 `guide-package.md` |

`logcheck_gpu_timing.py` 解析 `GPU 磨皮[..] 上传=.. 算=.. 读回=.. 补齐=.. 合计=..` 行，做两件事：**逐帧闭合验算**（四项相加 vs 合计，最大偏差 < 0.05 ms 说明没有隐藏开销）与**热状态分布**（只统计累计帧号 ≥ 60 的行）。

> 它存在的原因：2026-11-07 曾凭"各项区间最小值相加"去比"合计均值"，误判出"1.3 ms 未计量段（29%）"；逐帧验算后缺口只有 0.01 ms。
> 两个坑：① 首帧与第 2 帧含着色器编译 + 纹理首次落实物理页，分项高低会反过来；② 决定拖滑块体感的是**尖峰**，看 max/min 比看 mean 有用。
