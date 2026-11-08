# 安装 / 升级 / 回滚

> 适用代码版本：1.3.6.0 ｜ 状态：Active ｜ 最后更新：2026-11-08 ｜ 维护人：Jian ｜ 替代文档：`安装说明.txt`（v1.0.7.0 时代，已删除）

**一句话**：先彻底退出 Resolve，再跑 `01_ofx插件源码\install.bat`；或者双击 `05_安装包\build\DaVinciBeauty_Setup_v1.3.6.0.exe`。装完重启 Resolve，效果面板在 `Effects Library > OpenFX > DaVinci Beauty`。

## 一、手动装（源码包形态）

1. **（可选）** 双击 `stage_engine.bat` —— 把引擎另存一份到 `C:\ProgramData\DaVinciBeauty`。`install.bat` 直接用包内 `02_引擎资产`，这一步只给旧流程或换机器兜底。
2. **完全退出 DaVinci Resolve**，再双击 `01_ofx插件源码\install.bat`。
   不退出会让被占用的 DLL 改名成 `.locked-*` 留在目录里，攒多了是纯垃圾（验收有一条「无 `.locked-*` 残留」专门查它）。
3. 重启 Resolve，打开效果面板。

落地位置：`C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle\Contents\Win64\`
必备文件：`DaVinciBeauty.ofx` + `onnxruntime.dll` + `yunet.onnx` + `LICENSE` + `THIRD-PARTY-NOTICES.md`。**缺一个插件就起不来**。

## 二、一键安装包（含卸载器）

| 操作 | 命令 |
|---|---|
| 图形安装 | 双击 `DaVinciBeauty_Setup_v1.3.6.0.exe`（会请求管理员权限） |
| 静默安装 | `DaVinciBeauty_Setup_v1.3.6.0.exe /S` |
| 静默卸载 | `DaVinciBeauty_Setup_v1.3.6.0.exe --uninstall /S` |
| 卸载（图形） | 「应用和功能」→「DaVinci Beauty 美颜」，或运行 `…\DaVinciBeauty.ofx.bundle\Uninstall_DaVinciBeauty.exe /S` |

安装包会顺带装上 `LICENSE`（GPL-2.0-only）与 `THIRD-PARTY-NOTICES.md`，随分发一起保留。
当前包：4,856,294 字节（4.63 MB），`build\DaVinciBeauty_Setup_v1.3.6.0.exe`，md5 `50436e915a79759c91dc99e4441eb8b1`。重出包见 `docs/guides/guide-package.md`。

## 三、升级与回滚

- **升级**：直接覆盖装。旧 DLL 按约定备份为 `DaVinciBeauty.ofx.bak-<旧版本>`（本轮装机后有 17 个 `.bak-*`）。
- **回滚**：把想要的 `.bak-<版本>` 改回 `DaVinciBeauty.ofx` 覆盖即可，不需要源码。
- **确认装的是哪一版**：看 `ofx.log` 里的 `build=` 行，或比对 md5 ——**构建可复现**，同源码重编 md5 恒定，所以 md5 能直接证明「装的是不是刚构建的那一份」。验收里这条是硬判据。

## 四、配置开关（`beauty.ini`）

路径：`C:\ProgramData\DaVinciBeauty\beauty.ini`（不在仓库里，首次运行后由插件生成）。

| 键 | 作用 |
|---|---|
| `gpu_smooth` | GPU 磨皮开关，出厂 `auto`（探测到可用设备才启用） |
| `neck_taper` | 脖子带下缘收口深度，默认 `0.30`；写 `0` 完全回到弧线之前的行为 |
| `neck_skinend` | 下缘是否跟随肤色末端；写 `0` 完全回到上一版行为，一个像素不差 |
| `dump_enable` | 诊断导出开关。从桌面图标启动 Resolve 时环境变量传不进去，所以开关落到了 ini |

环境变量仍然优先，但**只在从命令行/脚本启动宿主时有效**。完整清单见 `docs/reference/reference-architecture.md` 的「环境变量」一节。

## 五、装完自检

```
04_验收工具\verify.bat            # 或 python acceptance.py
```
期望：`PASS 28 / FAIL 0`，逐字节回归 18/18，mock host 23 项。包内自带 `03_测试夹具\probe_in.raw`（1 MB），装机前后都能自测。

## 六、常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| Resolve 里看不到效果 | 插件没装进 OFX 目录，或 OFX 缓存还指着旧文件 | 确认 `Win64` 下 5 个文件齐；删 `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml` 再启动 |
| 目录里一堆 `.locked-*` | 替换时 Resolve 没退干净 | 退出 Resolve 后删掉 `.locked-*`，重跑 `install.bat` |
| 装了新版但界面没变 | 宿主进程没重启 | 完全退出（含后台）再启动 |
| 莫名变慢、日志无报错 | 1.3.5.0 之前 GPU 掉驱动后会永久静默停用 | 升到 1.3.5.0；或临时把 `gpu_smooth` 设 `0` 走 CPU |

## 七、不要动的东西

- 插件 ID `com.nama.davinci.beauty`：Resolve 按 ID 找效果，改了历史工程里已挂的效果会失效。
- 分发时必须随附 `LICENSE` 与 `THIRD-PARTY-NOTICES.md`：GPL-2.0-only 的合规要求，不能只发二进制。
