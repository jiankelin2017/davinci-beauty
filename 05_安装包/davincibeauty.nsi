; ---------------------------------------------------------------------------
; DaVinci Beauty 美颜插件 —— 一键安装 / 一键卸载
;
; 由 build_nsis.py 调用 makensis 编译。所有需要打包的文件由构建脚本
; 先 stage 到一个临时目录，再通过 /DSTAGE=<dir> 传给本脚本。
;
; 设计要点：
;   * 安装目标：C:\Program Files\Common Files\OFX\Plugins\DaVinciBeauty.ofx.bundle
;   * 卸载必须是「全清」：bundle + %ProgramData%\DaVinci Beauty + Resolve 的 OFX 缓存
;   * 检测到 Resolve 运行时要提示；用户坚持则走改名替换策略
;   * 写入 HKLM 的卸载信息，让它出现在「应用和功能」里
; ---------------------------------------------------------------------------

Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

; --- 编译期变量（构建脚本通过 -D 传入）---
!ifndef STAGE
  !error "必须指定 /DSTAGE=<staged dir>"
!endif
!ifndef VERSION
  !define VERSION "1.4"
!endif
!ifndef OUTFILE
  !define OUTFILE "DaVinciBeauty_Setup_v${VERSION}.exe"
!endif
; VIProductVersion 必须是四段 x.x.x.x。由构建脚本按 --version 注入 VI_VERSION，
; 没注入时从 VERSION 兜底派生 —— 这样版本号只有一个来源，不再和 VERSION 脱节。
!ifndef VI_VERSION
  !define VI_VERSION "${VERSION}.0.0"
!endif

!define APPNAME        "DaVinci Beauty 美颜"
!define PUBLISHER      "DaVinci Beauty"
!define VERSIONSTR     "${VERSION}"
; APPID / BUNDLE_NAME 允许构建时用 -D 覆盖：便于做「不碰正式安装」的沙箱回归。
!ifndef APPID
  !define APPID          "DaVinciBeauty.BeautyOFX"
!endif
!ifndef BUNDLE_NAME
  !define BUNDLE_NAME    "DaVinciBeauty.ofx.bundle"
!endif

Name "${APPNAME} ${VERSIONSTR}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

; OFX bundle 的规范布局：
;   DaVinciBeauty.ofx.bundle\
;       Contents\
;           Win64\   <- 二进制与资源都在这一层
; 所以 InstallDir 必须指到 ...\bundle\Contents\Win64
; $COMMONFILES64 = C:\Program Files\Common Files
!define BUNDLE_ROOT "$COMMONFILES64\OFX\Plugins\${BUNDLE_NAME}"
InstallDir "${BUNDLE_ROOT}\Contents\Win64"
InstallDirRegKey HKLM "Software\${APPID}" "InstallDir"

VIProductVersion "${VI_VERSION}"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "FileDescription" "DaVinci Beauty OpenFX 插件安装程序"
VIAddVersionKey "FileVersion"     "${VERSIONSTR}.0.0"
VIAddVersionKey "ProductVersion"  "${VERSIONSTR}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "LegalCopyright"  "© ${PUBLISHER}"

; --- 界面 ---
!define MUI_ABORTWARNING
!define MUI_ICON   "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

!define MUI_WELCOMEPAGE_TITLE    "安装 ${APPNAME}"
!define MUI_WELCOMEPAGE_TEXT     "将把 DaVinci Beauty 美颜滤镜安装到达芬奇 DaVinci Resolve。$\r$\n$\r$\n安装内容：插件本体 + 自研美颜/AI 引擎（ONNX Runtime + YuNet 模型，约 11 MB）。$\r$\n$\r$\n安装前请先关闭 DaVinci Resolve，否则部分文件可能无法替换。$\r$\n$\r$\n点「下一步」继续。"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; 安装
; ---------------------------------------------------------------------------
Section "DaVinci Beauty Plugin" SecMain
  SectionIn RO

  ; NSIS 默认产出 32 位安装器，注册表会落到 WOW6432Node。
  ; 强制 64 位视图，卸载信息才会出现在 64 位系统的「应用和功能」里。
  SetRegView 64
  ; $APPDATA 指向所有用户的 ProgramData
  SetShellVarContext all

  SetOutPath "$INSTDIR"
  SetOverwrite on

  ; --- 残留清理：把历史 .locked-* 挪走，避免越装越多 ---
  ; NSIS 的 FindFirst 不支持 "*.locked-*" 这种双通配，所以扫两遍：
  ; 先扫 *.locked-*（用通配前缀），再扫 *.old_*。
  DetailPrint "清理历史残留文件 ..."
  ClearErrors
  FindFirst $0 $1 "$INSTDIR\*.locked-*"
  loop_locked:
    IfErrors done_locked
    StrCmp $1 "" done_locked
    DetailPrint "  删除 $1"
    Delete "$INSTDIR\$1"
    FindNext $0 $1
    Goto loop_locked
  done_locked:
  FindClose $0

  ClearErrors
  FindFirst $0 $1 "$INSTDIR\*.old_*"
  loop_old:
    IfErrors done_old
    StrCmp $1 "" done_old
    DetailPrint "  删除 $1"
    Delete "$INSTDIR\$1"
    FindNext $0 $1
    Goto loop_old
  done_old:
  FindClose $0

  DetailPrint "复制插件与引擎资源 ..."
  ; 只装 bundle 需要的文件。${STAGE} 里可能还有构建期辅助目录
  ; （如 python\ 解释器），那些**不能**进 Contents\Win64 —— Resolve 会扫描这个目录。
  ; 自研引擎（上一代宿主 CoreEngine + ONNX 人脸检测模型）只需要插件本体、ONNX Runtime 与本地
  ; 人脸检测模型，不再依赖任何闭源 SDK。
  File "${STAGE}\DaVinciBeauty.ofx"
  File "${STAGE}\onnxruntime.dll"
  File "${STAGE}\yunet.onnx"
  SetOutPath "$INSTDIR"

  ; --- 注册表：卸载信息，让它出现在「应用和功能」---
  WriteRegStr HKLM "Software\${APPID}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\${APPID}" "BundleRoot" "${BUNDLE_ROOT}"
  WriteRegStr HKLM "Software\${APPID}" "Version"   "${VERSIONSTR}"

  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "DisplayName"     "${APPNAME}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "DisplayVersion"  "${VERSIONSTR}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "Publisher"       "${PUBLISHER}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "UninstallString" '"${BUNDLE_ROOT}\Uninstall_DaVinciBeauty.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "QuietUninstallString" '"${BUNDLE_ROOT}\Uninstall_DaVinciBeauty.exe" /S'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}" \
              "EstimatedSize" "$0"

  ; --- 卸载器放在 bundle 根，不在 Contents\Win64 里 ---
  ; （放进 Win64 会被 Resolve 当成插件资源扫描；放根目录更干净）
  SetOutPath "${BUNDLE_ROOT}"
  WriteUninstaller "${BUNDLE_ROOT}\Uninstall_DaVinciBeauty.exe"
  SetOutPath "$INSTDIR"

  ; --- 校验关键文件 ---
  DetailPrint "校验关键文件 ..."
  ${IfNot} ${FileExists} "$INSTDIR\DaVinciBeauty.ofx"
    ${IfNot} ${Silent}
      MessageBox MB_ICONSTOP "安装未完成：DaVinciBeauty.ofx 缺失。$\r$\n请关闭 DaVinci Resolve 后重试。"
    ${EndIf}
    SetErrors
    Abort
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\onnxruntime.dll"
    ${IfNot} ${Silent}
      MessageBox MB_ICONSTOP "安装未完成：onnxruntime.dll 缺失。$\r$\n请关闭 DaVinci Resolve 后重试。"
    ${EndIf}
    SetErrors
    Abort
  ${EndIf}
  ${IfNot} ${FileExists} "$INSTDIR\yunet.onnx"
    ${IfNot} ${Silent}
      MessageBox MB_ICONSTOP "安装未完成：yunet.onnx 缺失。$\r$\n请关闭 DaVinci Resolve 后重试。"
    ${EndIf}
    SetErrors
    Abort
  ${EndIf}
SectionEnd

; ---------------------------------------------------------------------------
; 收尾：清 Resolve 缓存 + 提示重启
; ---------------------------------------------------------------------------
Section "-PostInstall"
  ; Resolve 的 OFX 扫描缓存位于**当前用户**的 Roaming 下
  ; （%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml）。
  ; 这里必须用 SetShellVarContext current，否则 $APPDATA 会指向 C:\ProgramData。
  SetShellVarContext current
  Delete "$APPDATA\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml"

  ; 静默安装（/S）绝不能弹对话框，否则自动化 / 包管理器会一直卡在弹窗上。
  ${IfNot} ${Silent}
    nsExec::ExecToStack 'cmd /c tasklist /FO CSV /NH | findstr /I /C:"Resolve"'
    Pop $0
    ${If} $0 == "0"
      MessageBox MB_ICONINFORMATION|MB_OK \
        "安装完成。$\r$\n$\r$\n检测到 DaVinci Resolve 正在运行 —— 必须重启它才能识别新插件。$\r$\n$\r$\n使用方法：$\r$\n  1. 重启 DaVinci Resolve$\r$\n  2. 进 Color 或 Edit 页面$\r$\n  3. 左侧 Effects Library -> OpenFX -> DaVinci Beauty$\r$\n  4. 把「DaVinci Beauty 美颜」拖到片段上"
    ${Else}
      MessageBox MB_ICONINFORMATION|MB_OK \
        "安装完成。$\r$\n$\r$\n使用方法：$\r$\n  1. 启动 DaVinci Resolve$\r$\n  2. 进 Color 或 Edit 页面$\r$\n  3. 左侧 Effects Library -> OpenFX -> DaVinci Beauty$\r$\n  4. 把「DaVinci Beauty 美颜」拖到片段上$\r$\n$\r$\n参数提示：磨皮不为 0 时锐化不生效；八项全 0 时画面完全不变。"
    ${EndIf}
  ${EndIf}
SectionEnd

; ---------------------------------------------------------------------------
; 卸载（全清）
; ---------------------------------------------------------------------------
Section "Uninstall"
  SetRegView 64
  ; 先按 all-users 语境删 bundle 与 ProgramData
  SetShellVarContext all

  ; 先把输出/工作目录挪出 bundle：Windows 不允许删除仍是某进程「当前目录」的文件夹，
  ; 旧的写法会让 RMDir /r 中途失败，留下一堆空目录，达不到本脚本声称的「全清」。
  SetOutPath "$TEMP"

  DetailPrint "删除插件与引擎资源 ..."
  ; 删整个 bundle 根（含 Contents\Win64、卸载器自身所在的目录）
  ; 注意：正在运行的卸载器不能删除自己，NSIS 会自动处理（延后删除）。
  RMDir /r "${BUNDLE_ROOT}"
  ; 若已删空，把空的 bundle 根目录也一并去掉
  RMDir "${BUNDLE_ROOT}"

  ; --- ProgramData 下的数据（日志、jobs、live、disabled_scripts、engine 等）---
  DetailPrint "删除 DaVinci Beauty 数据目录 ..."
  RMDir /r "$APPDATA\DaVinciBeauty"

  ; --- Resolve 的 OFX 缓存（属于当前用户）---
  SetShellVarContext current
  DetailPrint "清理 DaVinci Resolve OFX 缓存 ..."
  Delete "$APPDATA\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml"

  ; --- 注册表 ---
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPID}"
  DeleteRegKey HKLM "Software\${APPID}"
SectionEnd

; ---------------------------------------------------------------------------
; 初始化
; ---------------------------------------------------------------------------
Function .onInit
  ; --- --uninstall：转发给已安装的卸载器 ---
  ; （build_nsis.py 与 README 一直宣传可运行 "... --uninstall"，但此前并未实现。）
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "--uninstall" $R1
  ${IfNot} ${Errors}
    StrCpy $R2 0
    ${If} ${FileExists} "${BUNDLE_ROOT}\Uninstall_DaVinciBeauty.exe"
      ${If} ${Silent}
        ExecWait '"${BUNDLE_ROOT}\Uninstall_DaVinciBeauty.exe" /S' $R2
      ${Else}
        ExecWait '"${BUNDLE_ROOT}\Uninstall_DaVinciBeauty.exe"' $R2
      ${EndIf}
    ${Else}
      ${IfNot} ${Silent}
        MessageBox MB_ICONINFORMATION|MB_OK "没有找到已安装的 DaVinci Beauty，无需卸载。"
      ${EndIf}
    ${EndIf}
    ; 不能用 Abort：NSIS 在 .onInit 里 Abort 会固定返回退出码 2，
    ; 导致 winget / 脚本把「卸载成功」判成失败。转发真实退出码后 Quit。
    SetErrorLevel $R2
    Quit
  ${EndIf}

  ; --- 检测 Resolve 是否在运行 ---
  nsExec::ExecToStack 'cmd /c tasklist /FO CSV /NH | findstr /I /C:"Resolve" /C:"davinci" /C:"fuscript"'
  Pop $0
  ${If} $0 == "0"
    ; 静默安装不弹确认框，直接继续（文件替换阶段会按需改名绕过占用）
    ${IfNot} ${Silent}
      MessageBox MB_ICONEXCLAMATION|MB_OKCANCEL \
        "检测到 DaVinci Resolve 正在运行。$\r$\n$\r$\nResolve 会锁定插件 DLL，导致部分文件无法替换。$\r$\n建议先完全退出 Resolve 再安装。$\r$\n$\r$\n仍要继续吗？" \
        /SD IDOK
      ${If} ${Errors}
        Abort
      ${EndIf}
    ${EndIf}
  ${EndIf}
FunctionEnd
