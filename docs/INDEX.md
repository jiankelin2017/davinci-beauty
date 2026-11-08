# 文档总表（INDEX）

> 适用代码版本：1.3.5.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：未登记
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
| 交付概览 | `README.md` | README | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 使用入口卡 | `说明.txt` | 入口卡 | Active | 1.3.5.0 | 未登记 | 2026-11-08 | 详版见 `docs/guides/guide-install.md` |
| 总索引 | `docs/INDEX.md` | reference | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 变更史 | `docs/CHANGELOG.md` | CHANGELOG | Active | 1.3.5.0 | 未登记 | 2026-11-08 | 原始过程见 `docs/history/history-progress-1x.md` |
| 安装 / 升级 / 回滚 | `docs/guides/guide-install.md` | guide | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 开发工作流 | `docs/guides/guide-dev-workflow.md` | guide | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 验收 | `docs/guides/guide-acceptance.md` | guide | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 出包 | `docs/guides/guide-package.md` | guide | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 架构与关键技术事实 | `docs/reference/reference-architecture.md` | reference | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 性能 | `docs/reference/reference-performance.md` | reference | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| GPU 磨皮模块 | `docs/design/design-gpu-smooth.md` | design | Active（已完成施工，留决策） | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 锐化为何无效 | `docs/adr/adr-0001-sharpen-inert.md` | adr | Active | 1.3.5.0 | 未登记 | 2026-11-08 | — |
| 过程记录（1.0.x–1.3.x） | `docs/history/history-progress-1x.md` | history | Active（只读归档） | 1.0.0.0–1.3.5.0 | 未登记 | 2026-11-08 | — |
| 旧 SDK 区域探测 PHASE0 | `docs/archive/archive-phase0-sdk-region.md` | archive | Deprecated | 闭源 SDK 时代 | 未登记 | 2026-11-08 | 对象已移除（引擎 v3.0.0 换自研） |
| 区域精准化 PHASE1 | `docs/archive/archive-phase1-region.md` | archive | Deprecated | 同上 | 未登记 | 2026-11-08 | 同上 |
| 语义区域 PHASE2 | `docs/archive/archive-phase2-semantic-region.md` | archive | Deprecated | 同上 | 未登记 | 2026-11-08 | 同上 |
| 旧开发 README（121 KB） | `docs/archive/archive-dev-readme-20261108.md` | archive | Deprecated | ≤1.1.7.0 | 未登记 | 2026-11-08 | 已并入 `docs/CHANGELOG.md` |
| 历史验收输出（1.0.7.0） | `docs/archive/archive-verification-1.0.7.0.txt` | archive | Deprecated | 1.0.7.0 | 未登记 | 2026-11-08 | 每次验收重新生成，不存当前版 |
| 旧包 sha256 清单 | `docs/archive/archive-manifest-v308.txt` | archive | Deprecated | 旧编号时代 | 未登记 | 2026-11-08 | 现由 `package.py` 在包内生成 `MANIFEST.txt` |
| 夹具运行记录 | `03_测试夹具/RUNS.md` | reference | Active | 1.3.5.0 | 未登记 | 2026-11-08 | 替代原 14 份 `.txt` 抓屏 |
| 合规声明 | `THIRD-PARTY-NOTICES.md` | 合规 | Active | — | 未登记 | 2026-11-08 | bundle 内另有副本（构建产出） |
| 文件清单 | `FILELIST.txt` | 生成物 | Active | 1.3.5.0 | 未登记 | 2026-11-08 | `gen_filelist.py` 生成，勿手改 |
| 基线数据 | `04_验收工具/baseline_manifest.txt` | data | Active | 1.2.5.0（头注释待重生成） | 未登记 | 2026-11-08 | — |

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

## 六、待人工确认

- `04_验收工具/baseline_manifest.txt` 头注释仍写 `plugin_version: 1.2.5`；重生成会改变黄金值，需在确认「当前画面即基线」后跑 `baseline.py update`。
- `README.md` 内仍有一段「v1.4 验收摘要（2026-10-21）」，属 2026-10 的历史快照，已标为历史；是否删除待定。
- 各文档「维护人」字段统一填「未登记」，需要真实责任人时请补。
