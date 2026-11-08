# 文档总表（INDEX）

> 适用代码版本：1.3.5.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：Jian
> 本表是文档体系的唯一索引。任何新增/移动/废弃文档，先改这张表，再动文件。

## 一、规则（四条，违反即视为文档缺陷）

1. **固定分类，每类规定份数**：README（全仓 1）、design（每模块 1）、guides（每场景 1）、reference（每主题 1）、CHANGELOG（全局 1）、adr（一事一篇）、archive（隔离存放，不限）。
2. **主目录每类只留当前版**：同一主题不允许两份并存。旧版一律进 `docs/archive/`，命名 `archive-主题-YYYYMMDD.md`，文首标注「已废弃，见 xxx」。
3. **统一命名**：`类型-主题.md`（如 `guide-install.md`、`adr-0001-sharpen-inert.md`），禁止 `final_v3_副本` 类命名。
4. **统一文档头**：版本｜最后更新｜适用代码版本｜维护人｜状态（Active/Deprecated）｜替代文档。缺一即视为待补。

新文档只能放进 `docs/{design,guides,reference,adr}` 或 `docs/archive/`；根目录只保留 `README.md`、`说明.txt`（交付入口）与合规文件。

## 二、目录

```
docs/
├── INDEX.md                 ← 本表
├── CHANGELOG.md             ← 全局一份，只记「用户能看出来的变化」
├── guides/                  ← 每场景一份：怎么装、怎么开发、怎么验收、怎么打包
├── reference/               ← 每主题一份：架构、性能
├── design/                  ← 每模块一份：GPU 磨皮
├── adr/                     ← 一事一篇：已经定死的取舍
├── history/                 ← 过程记录（按时间倒序，不追求可读性，只追求可追溯）
└── archive/                 ← 已废弃 / 已过时的文档，隔离存放
```

## 三、当前文档清单

| 主题 | 路径 | 类型 | 状态 | 适用版本 | 维护人 | 最后核对 | 替代 / 归档去向 |
|---|---|---|---|---|---|---|---|
| 交付概览 | `README.md` | README | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 使用入口卡 | `说明.txt` | 入口卡 | Active | 1.3.5.0 | Jian | 2026-11-08 | 详版见 `docs/guides/guide-install.md` |
| 总索引 | `docs/INDEX.md` | reference | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 变更史 | `docs/CHANGELOG.md` | CHANGELOG | Active | 1.3.5.0 | Jian | 2026-11-08 | 原始过程见 `docs/history/history-progress-1x.md` |
| 安装 / 升级 / 回滚 | `docs/guides/guide-install.md` | guide | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 开发工作流 | `docs/guides/guide-dev-workflow.md` | guide | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 验收 | `docs/guides/guide-acceptance.md` | guide | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 出包 | `docs/guides/guide-package.md` | guide | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 架构与关键技术事实 | `docs/reference/reference-architecture.md` | reference | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 性能 | `docs/reference/reference-performance.md` | reference | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| GPU 磨皮模块 | `docs/design/design-gpu-smooth.md` | design | Active（已完成施工，留决策） | 1.3.5.0 | Jian | 2026-11-08 | — |
| 锐化为何无效 | `docs/adr/adr-0001-sharpen-inert.md` | adr | Active | 1.3.5.0 | Jian | 2026-11-08 | — |
| 过程记录（1.0.x–1.3.x） | `docs/history/history-progress-1x.md` | history | Active（只读归档） | 1.0.0.0–1.3.5.0 | Jian | 2026-11-08 | — |
| 旧 SDK 区域探测 PHASE0 | `docs/archive/archive-phase0-sdk-region.md` | archive | Deprecated | 闭源 SDK 时代 | Jian | 2026-11-08 | 对象已移除（引擎替换为自研） |
| 区域精准化 PHASE1 | `docs/archive/archive-phase1-region.md` | archive | Deprecated | 同上 | Jian | 2026-11-08 | 同上 |
| 语义区域 PHASE2 | `docs/archive/archive-phase2-semantic-region.md` | archive | Deprecated | 同上 | Jian | 2026-11-08 | 同上 |
| 旧开发 README（121 KB） | `docs/archive/archive-dev-readme-20261108.md` | archive | Deprecated | ≤1.1.7.0 | Jian | 2026-11-08 | 已并入 `docs/CHANGELOG.md` |
| 历史验收输出（1.0.7.0） | `docs/archive/archive-verification-1.0.7.0.txt` | archive | Deprecated | 1.0.7.0 | Jian | 2026-11-08 | 每次验收重新生成，不存当前版 |
| 旧包 sha256 清单 | `docs/archive/archive-manifest-v308.txt` | archive | Deprecated | 旧编号时代 | Jian | 2026-11-08 | 现由 `package.py` 在包内生成 `MANIFEST.txt` |
| 夹具运行记录 | `03_测试夹具/RUNS.md` | reference | Active | 1.3.5.0 | Jian | 2026-11-08 | 替代原 14 份 `.txt` 抓屏 |
| 合规声明 | `THIRD-PARTY-NOTICES.md` | 合规 | Active | — | Jian | 2026-11-08 | bundle 内另有副本（构建产出） |
| 文件清单 | `FILELIST.txt` | 生成物 | Active | 1.3.5.0 | Jian | 2026-11-08 | `gen_filelist.py` 生成，勿手改 |
| 基线数据 | `04_验收工具/baseline_manifest.txt` | data | Active | 1.3.5 | Jian | 2026-11-08 | 18 个 md5 自 1.2.5 起未变，仅头注释随版本重生成 |

## 四、已清理的内容（本轮，2026-11-08）

| 原路径 | 处置 | 理由 |
|---|---|---|
| `01_ofx插件源码/PROGRESS.md`（322 KB） | → `docs/history/history-progress-1x.md` | 过程记录，读者拿不到结论；本体保留以维持可追溯 |
| `01_ofx插件源码/README.md`（121 KB） | → `docs/archive/archive-dev-readme-20261108.md` | 与 PROGRESS 同源扩写，无独有结论；1.2.x–1.3.x 已进 CHANGELOG |
| `03_测试夹具/*.txt`（14 份，约 1.1 MB） | 整合为 `03_测试夹具/RUNS.md` 后删除 | UTF-16 控制台抓屏，含大量乱码噪声，无结论行 |
| `FILELIST.txt` | 重新生成 | 生成物，旧清单停留在今天上午（161 文件）。已按 `gen_filelist.py` 重出（163 文件） |
| `MANIFEST.txt` | → `docs/archive/archive-manifest-v308.txt` | 旧编号时代的 134 文件 sha256 快照，包内清单由 `package.py` 现生成 |
| `安装说明.txt` | 删除，内容并入 `docs/guides/guide-install.md` | 通篇 v1.0.7.0，称 `build.bat` 为唯一入口，验收写 27 项 |
| `验收报告.txt` | → `docs/archive/archive-verification-1.0.7.0.txt` | 1.0.7.0 的一次运行输出，被旧安装说明称作「本次交付结论」 |
| `03_测试夹具/PHASE0/1/2*.md` | → `docs/archive/` | 探针对象是已在引擎替换时移除的闭源 SDK |
| `03_测试夹具/锐化无效排查报告.md` | → `docs/adr/adr-0001-sharpen-inert.md` | 结论仍成立，属「一事一篇」的取舍记录 |

## 五、防腐化机制（不靠记性，靠脚本和模板）

文档烂掉从来不是因为「没人写」，而是因为「写的人不知道写到哪、写完了没人查、旧的那份没人删」。三条机械规则比十条倡议有用：

1. **模板固化**：所有文档第一个块必须是六行头（版本｜最后更新｜适用代码版本｜维护人｜状态｜替代文档）。缺字段的文档在 INDEX 里标「待补」，不允许当当前版引用。
2. **脚本联动**：`acceptance.py` 的「工程」组已经把两件事做成硬判据 ——
   - `文档版本号同步`：`README.md`、`说明.txt`、`docs/INDEX.md`、`docs/CHANGELOG.md`、`docs/guides/guide-package.md` 五份必须含当前版本串；
   - `文档引用的安装包版本`：任何 `DaVinciBeauty_Setup_vX.exe` 引用（除非该行含「历史」）必须等于当前版本。
   发版时漏改文档 = 验收红，不靠人记。
3. **改代码必动 CHANGELOG**：一次提交里如果改了 `src/*.cpp` 或参数表，同一提交必须带一条 `docs/CHANGELOG.md` 记录；只有内部改动（打点、日志、诊断开关）可以合并成一行，但仍要有行。
4. **巡检**：每次发版前跑一遍 `python acceptance.py`，另外人工过一眼 —— ① INDEX 里状态为 Active 的文档是否都还成立；② 有没有两份讲同一主题；③ archive 里有没有被重新引用的（说明归档错了）。

## 六、逐篇改写进度

治理分两步：先搬迁归位（结构对），再逐篇把「过程叙事混结论」改成「结论先行」。**两步均已全部完成**（2026-11-08）。下表留作改造记录：每篇改完把状态改为「已重写」，不记 CHANGELOG——这属于可读性改造，不改行为。

| 文档 | 体积 | 搬迁 | 重写 | 备注 |
|---|---|---|---|---|
| `reference/reference-architecture.md` | 341 行 | ✅ | ✅ 2026-11-08 | 拆除 50 行调参流水；补 §1.2 两条 GPU 通路、§5.1 `beauty.ini` 9 键、§9 排错决策表、§10 作废说法 |
| `reference/reference-performance.md` | 15 KB | ✅ | ✅ 2026-11-08 | 由「按版本流水」改为「按段归因」；补口径 A/B 声明（10.61 vs 24 ms 的矛盾）、§7 作废说法；构建/沙箱两节移出正文 |
| `design/design-gpu-smooth.md` | 20 KB | ✅ | ✅ 2026-11-08 | 由「施工流水（548 行）」改为「决策 / 验证数据 / 未决项」；时间线、冻结过程、BUG3/BUG4 移入附录指针；补 1.1.2.0 的「夹具假象」二次踩坑 |
| `guides/guide-acceptance.md` | 16 KB | ✅ | ✅ 2026-11-08 | 按实测输出改正：27 项 → **28 项**，补齐「工程」组 12 项（原只写 8 项）；退出码改为「只有 0/1，WARN 不阻断」；dump 通道与周边工具移入附录 |
| `guides/guide-package.md` | 16 KB | ✅ | ✅ 2026-11-08 | 按「出包 → 装了什么 → 卸载 → 同步点 → 沙箱 → 参数 → 限制」重排；改正引擎回落顺序（`02_引擎资产` 优先）、补 `--uninstall` 与两个沙箱开关的区别、补「第四处资产清单验收不查」缺口、指向已失效的「沙箱包加载失败」与根目录快捷入口两条作废说法 |

## 七、待人工确认

- **回归抖动的机制仍未定论**（`baseline.py update` 曾因此拒绝写入）。已测到的事实：默认推理线程数下，顺序跑 18 个用例 ×2 遍会有一个用例首跑失配、复跑一致，且失配用例不固定（先后见到 `cl50`、`sh50_cl100`、`blur62_wh100`）；把 `BEAUTY_INFER_THREADS=1` 固定为单线程后，同样跑两遍 36/36 全绿。`BEAUTY_GPU_SMOOTH` 强制开/关的 md5 完全相同，所以先前「GPU 争用」的解释与这批数据不符。是 ORT 内部并行归约顺序，还是纯粹窗口变窄，尚无证据，未下结论。当前处理：`update` 一律加 `BEAUTY_INFER_THREADS=1` 跑，见 `reference-performance.md` §5.4。
- **三处旧编号残留已清（2026-11-08 同日重切）**：`beauty.ini` 模板头、`CoreEngine::version()`、`install.bat` 的非 ASCII 行都已处理。前者改为 `namaBuildStr()` 现拼（实现 `return NAMA_BUILD_STR`，不写字面量副本），`install.bat` 改纯 ASCII。**同版本号重出了一份安装包**（未对外交付），所以 1.3.5.0 对应两份字节：DLL `2299c328…` → `5655699…`，安装包 `500f7b17…` → `5d422ef0…`；画面不变（基线仍 18/18）。细节见 `docs/CHANGELOG.md` 的「同日重切」。
- **`P_*` 面板枚举有两份定义**：`beauty_params.h`（`beauty_core.h` 引用，标注「唯一出处」）与 `nama_engine.h`（名字略异：`P_THIN_FACE/P_BIG_EYE` vs `P_CHEEK/P_EYE`，下标逐位一致）。同时包含会报 C2365，是这次不得不加 `beauty_build.h` 转出版本串的原因。是否合并成一份待定——合并要改 `ofx_plugin.cpp` 的标识符，需连带验证参数接线。
- **`1.2.6.0` / `1.3.0.0` / `1.3.2` / `1.3.3` 已核实：不是缺条目，是编号跳号**（2026-11-08）。前两个编号从未被写进 `NAMA_BUILD_STR`（1.2.5.0 → 1.2.7.0、1.2.9.0 → 1.3.1.0 直接跨过），全库零命中；后两个是 commit 说明里的工作标签，版本宏仍是 `1.3.1.0`，也没出包。所以没有补录，改为在 `CHANGELOG.md` 的「编号跳号与工作标签」一节交代证据与规则。
