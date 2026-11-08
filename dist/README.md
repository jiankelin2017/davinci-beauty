# dist/ —— 发布产物

本目录只放**当前版本**的安装包，是给「从 GitHub 直接下载」的人用的。
构建目录 `05_安装包/build/` 里还留着历代旧包，那些不进仓库（`.gitignore` 忽略
`05_安装包/build/`，只对本目录开 `!dist/*.exe` 例外）。

| 项 | 值 |
|---|---|
| 文件 | `DaVinciBeauty_Setup_v1.3.6.0.exe` |
| 大小 | 4,858,896 字节（4.63 MB） |
| md5 | `e15aaf96fd53c03670ed623f4863f6cc` |
| sha256 | `3fa593fbc04e0bf85e7e2d4ff053df66db389a43d0b28d4f05ab30cf71522d29` |
| 构建来源 | 提交 `main` 上的 `05_安装包/build_nsis.py`，payload 6 个文件 |

## 校验

```bash
sha256sum -c SHA256SUMS.txt
```

Windows 上可用：

```powershell
Get-FileHash .\DaVinciBeauty_Setup_v1.3.6.0.exe -Algorithm SHA256
```

## 这个包做了什么

装到 `C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle`，
内含插件 DLL、ONNX Runtime、YuNet 人脸模型，以及 `LICENSE` / `COPYRIGHT` /
`THIRD-PARTY-NOTICES.md` 三份合规文件。卸载走控制面板或
`DaVinciBeauty_Setup_v1.3.6.0.exe --uninstall`。

## 为什么把 exe 放进 git

二进制约 4.6 MB，一个版本一个文件，能接受。好处是「下载地址 = 仓库路径」，
不必维护 Releases 与仓库两条线；坏处是仓库体积随版本线性增长 —— 等包体变大
或版本变密，就改走 GitHub Releases，本目录只留 `SHA256SUMS.txt` 与说明。
