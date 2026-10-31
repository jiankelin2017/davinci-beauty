# 04_验收工具

DaVinci Beauty 发版前的自动化验收。目标：**任何一次改动，能否发版由脚本说了算**，
不靠"我记得跑过"。

## 一键验收

```
python acceptance.py                 # 全部检查，退出码 0 = 通过
python acceptance.py --build         # 先重新编译再验（发版时用）
python acceptance.py --skip 回归     # 跳过某组（版本/参数/回归/接口/装机）
python acceptance.py --report 验收报告.txt
```

或者双击 `verify.bat`。

## 检查项

| 组 | 检查 | 为什么查这一条 |
|---|---|---|
| 版本 | `NAMA_BUILD_STR` 与 `MAJOR/MINOR/PATCH` 一致 | 版本号唯一出处在 `nama_engine.h`，三处宏必须自洽 |
| 版本 | 二进制内嵌版本串 | `NamaVersion()` **只编码 MAJOR/MINOR，PATCH 位被有意丢弃**（保持对外 ABI），所以判断装的到底是哪个版本只能靠二进制里的字符串 |
| 版本 | `NamaVersion() == (MAJOR<<16)\|MINOR` | 编码是两段各 16 位，不是每 8 位一段 |
| 版本 | 装机产物**行为等价** | 让装机 DLL 跑同一组黄金用例，输出必须与编译 DLL 相同 |
| 版本 | 装机产物字节比对（仅提示） | **v3.0.8 起作为硬判据**：`build.ps1` 链接加了 `/Brepro`，同源码重编逐字节相同（此前 MSVC 会写 PE 时间戳，md5 每次都变，只能判行为） |
| 参数 | 三张表长度一致 | `P_*` 枚举 / `kParams[]` / `keys[]` 三张表 |
| 参数 | **面板名 ↔ SDK key 逐位对齐** | v2.2.3 踩过：换序时漏改 `keys[]`，后果是"拖 A 滑块、下发成 B 参数"，**编译期完全不报错**，只有回归用例的 md5 会变 |
| 参数 | 默认档 磨皮50/美白30/红润30，其余 0 | v2.2.3 用户决策，不是默认值漂移 |
| 参数 | `sharpen` 保持隐藏 | 磨皮>0 时锐化完全无效，暴露出来用户会以为插件坏了 |
| 参数 | 美型三项沉底 | Resolve 的 OFX 面板**不渲染 Page 也不渲染 Group**，只能靠定义顺序控制布局 |
| 回归 | 18 用例逐字节比对黄金基线 | 性能优化的防线：变快了 ≠ 少算了 |
| 接口 | mock host OFX 宿主契约（23 项） | 不打开 Resolve 验证插件与宿主的接口约定 |
| 接口 | 自测链路 LoadLibrary→初始化→渲染 | 端到端跑通 |
| 装机 | bundle 引擎资产齐全 | 缺一个插件就起不来 |
| 装机 | 无 `.locked-*` 残留 | install.bat 替换被占用 DLL 留下的垃圾，Resolve 没关干净就会一直堆 |
| 工程 | **install.bat 拷贝清单 ⊇ 装机必备资产** | v2.3.1 踩的大坑：装机脚本从来没拷过 `ai_face_processor_pc_plain.bundle`（旧 bundle 里那份是手工放的），全新装机就缺资产，`NamaSelfTest` 直接崩 rc=0xC0000005。静态判，装机**之前**就该抓出来 |
| 工程 | UI 无旧项目名残留 | 改名做一半没人发现 |
| 工程 | 面板文案统一新名 | 只认插件级文案（变量 `props`）；参数级 `pp` 是「磨皮」这类功能名，本来就不带产品名 |
| 工程 | 下拉项无括号后缀 | 用户明确要求简洁。取文案前先剥注释，否则注释里的历史叫法会被当成真字符串 |
| 工程 | 文档版本号同步 | 之前四份用户可见文档全线停在旧版本（连不存在的安装包都还列着），照着看会得到错误信息 |

## 逐字节回归基线

```
python baseline.py check              # 比对（默认）
python baseline.py update             # 重新生成基线（升版本后必做）
python baseline.py show               # 打印当前清单
python baseline.py check --only blur62 sh50
```

- 18 个用例：磨皮全档 / 美白 / 锐化 / 清晰 / 全局锐化 / 美型 / 三种磨皮模式 / 三种算法档。
- `shape100` 是唯一抓出过 v2.2.3 换序 bug 的用例，**不要删**。
- **raw 不入库**：15×2.6MB ≈ 41MB，每次现生成，仓库只留 `baseline_manifest.txt`（1KB）。
- manifest 记录输入帧 md5 与插件版本。输入换了会显式提示"需重新标定"，
  不会变成一堆看不懂的 md5 失配。
- `update` 会每个用例跑两次确认 md5 一致——验证的是**测试本身是否确定性**，
  避免把测量抖动当成输出变了。
- 输入帧 `input_face.png` 自带在仓库内，换台机器 clone 下来就能跑。

## 性能 A/B 测量

```
python perf_ab.py --res 720p --frames 3 <DLL路径>
```

两个 DLL 交替测量取最小值。**同一份二进制在不同时段能差 15%**（后台进程抢
CPU+GPU），只有交替跑才可比。

## 工程清理

```
python cleanup.py            # 预演：列清单 + 体积，不动手
python cleanup.py --apply    # 真删（会再问一次确认）
python cleanup.py --only build --apply
```

四类：`locked`（install.bat 的 `.locked-*` 残留与汇总备份）、`build`（bundle/
obj/旧产物，每次构建都会重生）、`probe`（探针输出，`probe_in.raw` 是输入不算）、
`external`（改名前的旧 bundle，在仓库外）。

设计原则：**只删白名单**，不做递归通配匹配；`KEEP` 清单里的文件永远碰不到。

## 生成整合包

```
python package.py            # 生成 D:\插件\DaVinciBeauty_v<版本>_整合包
python package.py --update   # 原地同步（不删整棵树）
python package.py --force    # 重建
python package.py --zip      # 额外打同名 .zip
python package.py --no-engine  # 不带 130MB 引擎资产
```

含源码 / 测试夹具 / 验收工具 / 05_安装包 / 引擎资产 + `安装说明.txt`
+ `stage_engine.bat`（把引擎放回 install.bat 认的路径）+ `MANIFEST.txt`（sha256）。

**03/04/05 三个历史遗留目录不进包**（与当前插件主线无关）。

## 已知的噪声

- 运行时 SDK 会往 stderr 打 `face_dde.cc:202] data check fail`。这是 NamaSDK
  内部日志，不影响结果（回归仍 15/15），过滤掉即可。
- `NamaSelfTest` 的错误消息里中文路径会乱码（UTF-8 被按本地码页输出）。
  只在初始化失败时才会看到，不影响正常路径。

## 发版操作流程

1. 改源码 → 升 `nama_engine.h` 的 `NAMA_BUILD_*`
2. `build.bat`
3. `install.bat`
4. `python acceptance.py --build --report 验收报告_vX.Y.Z.txt`
5. 若有意改动了输出（不只是优化），`python baseline.py update` 重新标定，
   并在 PROGRESS.md 写明改了什么、为什么
6. 打开 Resolve 实机确认 UI 与播放（**UI 类改动脚本测不出来，必须看截图**）
7. 需要归档/分发：`python package.py --update --zip`
