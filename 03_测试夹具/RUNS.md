# 测试夹具 / 运行记录（原 14 份 `.txt` 抓屏的汇总）

> 适用代码版本：1.3.5.0（本文件记录的是 2026-11 早期台架的输出，方法仍可用）｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：未登记 ｜ 替代文档：本文件替代原 14 份 UTF-16 控制台抓屏

## 一句话摘要

原来的 14 份 `.txt` 全部是 **UTF-16 控制台重定向输出**（BOM `ÿþ`，中文在 GBK/UTF-8 混读下是乱码 `@b(WMOn Lˆ`）。其中 **11 份只有 DIAG 行、没有任何结论**；**2 份是 mock host 的通过清单**（唯一有留存价值的部分，已抄在下面）；**1 份是旧的 DLL 导出表**（对象已随闭源 SDK 移除）。原始文件已删除，本文件保留了它们唯一可复用的数据。

## 二、mock host 通过清单（来自 `mock_run.txt` / `mock_run2.txt`，两次运行结论一致）

### 2.1 宿主契约项（22 项，全 PASS）

```
8bit 渲染      PASS      连续 3 帧     PASS      float 渲染    PASS
全0 透传       PASS      IsIdentity    PASS      代理透传      PASS
缩略图恒等     PASS      ClipPrefs     PASS      参数全0       PASS
参数换算       PASS      亮度不变      PASS      只磨皮肤默认  PASS
首次默认参数   PASS      通道序正确    PASS      美白不提亮整帧 PASS
A1 小画幅      PASS      B8 补边       PASS      A4 RoD缺省    PASS
B5 多实例      PASS      下拉框接线    PASS      单项参数接线  PASS
NaN 参数防护   PASS
总判定         PASS
DestroyInstance -> 0      ActionUnload -> 0
```

> 这 22 项是当时手写台架的口径。当前 `04_验收工具` 的 mock host 契约是 **23 项**（多出的一项见 `docs/guides/guide-acceptance.md` 的检查项表），不要拿这份旧清单当现行判据。

### 2.2 单项参数接线（14 项逐个拉到 100，其余 0）

判据是「改这个参数，画面 Must 变」，用 MAD（平均绝对差）与改动像素占比衡量：

| SDK key | 面板名 | MAD | 改动像素 |
|---|---|---|---|
| `blur_level` | 磨皮 | 0.27185 | 7.52% |
| `clarity` | 清晰 | 0.41336 | 7.86% |
| `color_level` | 美白 | 0.69685 | 8.02% |
| `red_level` | 红润 | 0.10199 | 7.12% |
| `sharpen` | 锐化 | **0.01202** | **0.64%** |
| `global_sharpen` | 全局锐化 | 3.12522 | 61.18% |
| `face_threed` | 立体 | 0.13379 | 3.42% |
| `eye_bright` | 亮眼 | 0.00811 | 0.16% |
| `remove_pouch_strength` | 去眼袋 | 0.00705 | 0.30% |
| `remove_nasolabial_folds_strength` | 去法令纹 | 0.03782 | 0.69% |
| `delspot_level` | 祛斑 | 0.03553 | 2.61% |
| `cheek_thinning` | 瘦脸 | 1.99128 | 12.40% |
| `eye_enlarging` | 大眼 | 0.41232 | 2.45% |
| `intensity_chin` | 下巴 | 0.28992 | 2.72% |

**这张表里最值得记的一行是 `sharpen`**：MAD 0.01202、只动 0.64% 像素，比其它项低一到两个数量级 —— 这就是「锐化看起来没反应」的量化证据，成因与处置见 `docs/adr/adr-0001-sharpen-inert.md`。

## 三、已删除的 11 份 DIAG 抓屏

| 文件 | 行数 | 内容 | 为什么删 |
|---|---|---|---|
| `bug4_final.txt` | 1182 | 双实例交替渲染的 `DIAG call=… inst=… src=… dst=…` | 无结论行，且同批抓屏重复 |
| `bug4_sweep_slot.txt` | 1182 | 同上（sweep 的 slot 变体） | 同上 |
| `bug4_sweep.txt` | 894 | sweep 0/5/10/20/40/80 ms | 结论已写在 `docs/history/history-progress-1x.md` 的 BUG4 一节 |
| `bug4_sweep_fixed.txt` | 894 | 修复后同参数 | 同上 |
| `mock_run.txt` | 411 | 见 §2 | 有价值的已抄录 |
| `mock_run2.txt` | 411 | 见 §2 | 同上 |
| `bug4_run_gap0.txt` | 151 | gap=0 | 无结论 |
| `bug4_run.txt` | 123 | 基线 | 无结论 |
| `bug4_run_align0.txt` | 123 | `BEAUTY_MASK_ALIGN=0` | 无结论 |
| `bug4_align0.txt` | 164 | 对齐关闭对照 | 无结论 |
| `installed_check.txt` / `2` / `3` | 164 ×3 | 装机 DLL 的 DIAG 抓屏（三次几乎同形） | 无结论 |
| `all_exports.txt` | 1 | `total exports: 582` 的旧 DLL 导出表 | 对象已随闭源 SDK 在引擎替换时移除 |

## 四、怎么复现

脚本都在本目录，抓屏请**直接存 UTF-8**（`python … > out.txt` 在 PowerShell 下默认 UTF-16，这就是当年那批文件全是乱码的原因）：

```bat
chcp 65001 >nul
python mock_host.py > run.txt 2>&1
```

- 双实例串台：`python bug4_repro.py [--gap 40] [--align 0]`（BUG4 已修，此处只作历史复现）
- 宿主契约与参数接线：现在的做法是 `04_验收工具\acceptance.py` 的「接口」组，不要再手写台架
