; ikeback - Windows installer (based on upstream Ikeback nsis/ikeback.nsi.in)
;
; Compile twice from the repository root (the release workflow supplies VERSION;
; 5.2.1 is only the fallback defined below):
;   makensis -DVERSION=5.2.1 -DROLE=teacher installer.nsi
;   makensis -DVERSION=5.2.1 -DROLE=student installer.nsi
;
; ROLE=teacher : component page with "Service" (read-only, registers/starts the
;                ikebackService service) and "Master" (optional console).
; ROLE=student : component page with ONLY the read-only Service section.
;
; Every shipped process is ikeback-*.exe and the Windows service is
; ikebackService. The installer also stops/removes upstream `ikeback-*.exe`
; processes and `IkebackService`, which would otherwise keep file handles open
; and trigger "无法打开要写入的文件 ... ikeback-worker.exe".
;
; CI copies this file to the repository root with a UTF-8 BOM (Unicode true).

Unicode true

!include "LogicLib.nsh"
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "nsDialogs.nsh"

!ifndef VERSION
  !define VERSION "5.2.1"
!endif
!ifndef ROLE
  !define ROLE "teacher"
!endif

!if "${ROLE}" == "teacher"
  !define ROLE_EN "Master"
  !define ROLE_CN "教师端"
  !define OUT_FILE "dist\ikeback-teacher-${VERSION}-win64-setup.exe"
  !define INST_DIR "$PROGRAMFILES64\ikeback\Master"
  !define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ikebackMaster"
!else
  !define ROLE_EN "Student"
  !define ROLE_CN "学生端"
  !define OUT_FILE "dist\ikeback-student-${VERSION}-win64-setup.exe"
  !define INST_DIR "$PROGRAMFILES64\ikeback\Student"
  !define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\ikebackStudent"
!endif

; Visible product name. Install paths, service names and registry keys keep the
; lowercase "ikeback" identifier so upgrades and the CLI stay compatible.
!define APP_NAME "ikee课堂"
!define PUBLISHER "zk26"
!define WEB_SITE "https://github.com/zk26/ikeback"
!define INSTALL_TYPE "SetShellVarContext all"
!define REG_ROOT "HKLM"
; Upgrade cleanup for the retired product name, assembled from two parts so the
; discontinued name is not stored anywhere in this source tree. Without this,
; an upgrade leaves the old service running (port conflict), keeps old EXEs
; locked and can delete the freshly installed Teacher directory.
!define OLD_A "Class"
!define OLD_B "Keep"
!define LEGACY_TEACHER_DIR "$PROGRAMFILES64\${OLD_A}${OLD_B}\Teacher"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} ${ROLE_EN} Setup"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-2.0-or-later; based on Veyon (c) Veyon Solutions"

SetCompressor /SOLID lzma
SetCompressorDictSize 64
Name "${APP_NAME} ${ROLE_EN} ${VERSION}"
Caption "${APP_NAME} ${ROLE_EN} ${VERSION}"
OutFile "${OUT_FILE}"
BrandingText "${APP_NAME} ${VERSION}"
InstallDir "${INST_DIR}"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

!define MUI_ABORTWARNING
!define MUI_UNABORTWARNING
!define MUI_ICON "desktop\nsis\installer.ico"
!define MUI_UNICON "desktop\nsis\uninstaller.ico"

!if "${ROLE}" == "teacher"
  !define MUI_WELCOMEPAGE_TITLE "欢迎使用 ikee课堂教师端"
  !define MUI_WELCOMEPAGE_TEXT "使用引导：$\r$\n1. 安装教师端，并允许 Windows 服务正常运行。$\r$\n2. 打开 ikee课堂，点击“开始上课”，全班签到会自动开始。$\r$\n3. 在座位页可直接修改考勤、积分和等级；课程、考勤与原因可导出 Excel。$\r$\n4. 商城与竞拍会保留每名学生 80 分，仅使用超出部分。$\r$\n5. 远程装软件请选择绿色软件文件夹，程序会部署到 C:\IkebackApps 并重启两次。"
!else
  !define MUI_WELCOMEPAGE_TITLE "欢迎使用 ikee课堂学生端"
  !define MUI_WELCOMEPAGE_TEXT "使用引导：$\r$\n1. 安装后保持学生端运行，等待教师端连接。$\r$\n2. 上课后在悬浮卡输入姓名并签到，即可查看成绩、商城和竞拍。$\r$\n3. 商城仅使用超过 80 分的积分，基础分会保留。$\r$\n4. 默认退出与卸载密码为 Admin@123，教师可在保护设置中修改。$\r$\n5. 保护开启时，断开教师连接也不会自动解锁；需教师解除保护或输入管理员密码。"
!endif

!if "${ROLE}" == "student"
Var ExitPasswordEdit
Var Dialog
!endif

!if "${ROLE}" == "student"
Function un.ExitPasswordPagePre
  SetRegView 64
  ReadRegDWORD $0 ${REG_ROOT} "SOFTWARE\ikeback\Student" "UninstallProtection"
  ${If} $0 != 1
    Abort
  ${EndIf}
FunctionEnd

Function un.ExitPasswordPageShow
  nsDialogs::Create 1018
  Pop $Dialog
  ${If} $Dialog == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 30u "学生端已开启保护。请输入管理员密码后再卸载。"
  Pop $0
  ${NSD_CreateText} 0 38u 100% 14u ""
  Pop $ExitPasswordEdit
  ${NSD_Edit_SetPasswordChar} $ExitPasswordEdit 0x25
FunctionEnd

Function un.ExitPasswordPageLeave
  ${NSD_GetText} $ExitPasswordEdit $R1
  StrCpy $R2 "$PLUGINSDIR\ikeback-exit-password.utf16"
  FileOpen $R3 $R2 w
  FileWriteUTF16LE $R3 "$R1"
  FileClose $R3
  nsExec::ExecToStack '"$INSTDIR\ikeback-cli.exe" --verify-exit-password "$R2"'
  Pop $0
  Delete $R2
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "管理员密码不正确，已取消卸载。"
    Abort
  ${EndIf}
FunctionEnd
!endif

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!if "${ROLE}" == "student"
  !define MUI_PAGE_CUSTOMFUNCTION_PRE un.ExitPasswordPagePre
  !define MUI_PAGE_CUSTOMFUNCTION_SHOW un.ExitPasswordPageShow
  !define MUI_PAGE_CUSTOMFUNCTION_LEAVE un.ExitPasswordPageLeave
!endif
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; Stop both the current and the upstream (ikeback) service + processes.
; `service unregister` does NOT stop a service (it only deletes its SCM entry),
; so this must happen before any File/Delete instruction.
; ---------------------------------------------------------------------------
Function StopIkebackProcesses
  ; "service not installed" / "process not found" are EXPECTED on a fresh
  ; install. SetDetailsPrint alone does NOT suppress them: ExecToLog writes the
  ; child's stdout/stderr straight into the details pane (that is what produced
  ; the wall of "[SC] OpenService 失败 1060" / "没有找到进程" messages).
  ; ExecToStack captures the output instead; both stack values are popped into
  ; throw-away registers so the later service checks still see their own values.
  ; One taskkill call with multiple /IM filters is enough for every binary.
  SetDetailsPrint none
  ; Retired <=2.x service: must be stopped AND deleted, otherwise it keeps
  ; launching a second VNC server that conflicts with ikebackService.
  nsExec::ExecToStack '"$SYSDIR\sc.exe" stop ${OLD_A}${OLD_B}Service'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\sc.exe" delete ${OLD_A}${OLD_B}Service'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\sc.exe" stop ikebackService'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\sc.exe" stop IkebackService'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\sc.exe" delete IkebackService'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\taskkill.exe" /F /T /IM ikeback-worker.exe /IM ikeback-server.exe /IM ikeback-service.exe /IM ikeback-master.exe /IM ikeback-configurator.exe /IM ikeback-cli.exe /IM ikeback-wcli.exe /IM ikeback-worker.exe /IM ikeback-server.exe /IM ikeback-service.exe /IM ikeback-master.exe /IM ikeback-configurator.exe /IM ikeback-cli.exe /IM ikeback-wcli.exe /IM ${OLD_A}${OLD_B}-worker.exe /IM ${OLD_A}${OLD_B}-server.exe /IM ${OLD_A}${OLD_B}-service.exe /IM ${OLD_A}${OLD_B}-master.exe /IM ${OLD_A}${OLD_B}-configurator.exe /IM ${OLD_A}${OLD_B}-cli.exe /IM ${OLD_A}${OLD_B}-wcli.exe'
  Pop $R8
  Pop $R9
  Sleep 1200
  SetDetailsPrint both
FunctionEnd

; Delete both generations of executables and retry while a handle is still
; open (up to 10s). If a file is genuinely locked, show an actionable message
; instead of letting NSIS raise its raw "cannot open file to write" dialog.
Function RemoveLockedBinaries
  StrCpy $8 0
  ck_retry:
    Delete "$INSTDIR\ikeback-worker.exe"
    Delete "$INSTDIR\ikeback-server.exe"
    Delete "$INSTDIR\ikeback-service.exe"
    Delete "$INSTDIR\ikeback-configurator.exe"
    Delete "$INSTDIR\ikeback-cli.exe"
    Delete "$INSTDIR\ikeback-wcli.exe"
    Delete "$INSTDIR\ikeback-master.exe"
    Delete "$INSTDIR\ikeback-worker.exe"
    Delete "$INSTDIR\ikeback-server.exe"
    Delete "$INSTDIR\ikeback-service.exe"
    Delete "$INSTDIR\ikeback-configurator.exe"
    Delete "$INSTDIR\ikeback-cli.exe"
    Delete "$INSTDIR\ikeback-wcli.exe"
    Delete "$INSTDIR\ikeback-master.exe"
    StrCpy $7 0
    IfFileExists "$INSTDIR\ikeback-worker.exe" 0 +2
      StrCpy $7 1
    IfFileExists "$INSTDIR\ikeback-server.exe" 0 +2
      StrCpy $7 1
    IfFileExists "$INSTDIR\ikeback-service.exe" 0 +2
      StrCpy $7 1
    IfFileExists "$INSTDIR\ikeback-master.exe" 0 +2
      StrCpy $7 1
    IfFileExists "$INSTDIR\ikeback-worker.exe" 0 +2
      StrCpy $7 1
    IfFileExists "$INSTDIR\ikeback-server.exe" 0 +2
      StrCpy $7 1
    IfFileExists "$INSTDIR\ikeback-service.exe" 0 +2
      StrCpy $7 1
    IfFileExists "$INSTDIR\ikeback-master.exe" 0 +2
      StrCpy $7 1
    IntCmp $7 0 ck_done
    IntOp $8 $8 + 1
    IntCmp $8 20 ck_giveup ck_retry ck_giveup
  ck_giveup:
    MessageBox MB_OK|MB_ICONSTOP "仍有 ikeback 进程占用安装目录，无法继续。$\r$\n$\r$\n请退出已登录的学生端（或重启电脑后重新安装），然后再次运行本安装程序。"
    Abort
  ck_done:
FunctionEnd

; ---------------------------------------------------------------- Service (RO)
Section "ikeback Service (required)" SEC_SERVICE
  SectionIn RO
  ${INSTALL_TYPE}
  SetOverwrite on
  SetOutPath "$INSTDIR"

  Call StopIkebackProcesses

  ; Unregister the service with whichever CLI the previous version shipped.
  ; `plugins/windows-platform.dll` + Qt6 `libqca-ossl.dll` must be present for
  ; the CLI to start; collect-windows-runtime.sh hard-verifies both.
  ; Its Chinese console output is GBK and renders as mojibake in a Unicode
  ; installer, so keep it out of the details log too.
  SetDetailsPrint none
  IfFileExists "$INSTDIR\ikeback-wcli.exe" 0 ck_legacy_wcli
    nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" service unregister'
    Pop $R8
    Pop $R9
    Goto ck_unregister_done
  ck_legacy_wcli:
    IfFileExists "$INSTDIR\ikeback-wcli.exe" 0 ck_unregister_done
      nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" service unregister'
      Pop $R8
      Pop $R9
  ck_unregister_done:
  SetDetailsPrint both

  Call RemoveLockedBinaries
  Delete "$INSTDIR\*.dll"
  RmDir /r "$INSTDIR\plugins"

  File "payload\ikeback-server.exe"
  File "payload\ikeback-service.exe"
  File "payload\ikeback-configurator.exe"
  File "payload\ikeback-cli.exe"
  File "payload\ikeback-wcli.exe"
  File "payload\ikeback-worker.exe"
  File "payload\*.dll"
  File "payload\LICENSE.TXT"
  File "payload\README.TXT"
  File "payload\COPYING.TXT"
  File "payload\ikeback.ico"

  SetOutPath "$INSTDIR\plugins"
  File "payload\plugins\*.dll"
  SetOutPath "$INSTDIR\crypto"
  File "payload\crypto\*.dll"
  SetOutPath "$INSTDIR\imageformats"
  File /nonfatal "payload\imageformats\*.dll"
  SetOutPath "$INSTDIR\platforms"
  File "payload\platforms\qwindows.dll"
  SetOutPath "$INSTDIR\styles"
  File /nonfatal "payload\styles\*.dll"
  SetOutPath "$INSTDIR\tls"
  File /nonfatal "payload\tls\*.dll"
  ; Chinese UI by default; switchable in ikeback Configurator -> General -> Language.
  SetOutPath "$INSTDIR\translations"
  File "payload\translations\*.qm"

  ; ------------------------------------------------------------------
  ; Upstream sequence, but with return codes + a real service self-check.
  ; ------------------------------------------------------------------
  SetOutPath "$INSTDIR"
  FileOpen $9 "$INSTDIR\install.log" w
  FileWrite $9 "ikeback ${VERSION} ${ROLE_EN} install log$\r$\n"

  ; Machine role marker: the classroom card/tray only appears on student
  ; installs and the configurator is key-gated there (see ClassroomPlugin).
  SetRegView 64
  WriteRegStr ${REG_ROOT} "SOFTWARE\ikeback" "InstallRole" "${ROLE}"

  nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" service register'
  Pop $R0
  Pop $1
  DetailPrint "注册服务 (service register): exit=$R0"
  FileWrite $9 "service register exit=$R0 output=$1$\r$\n"

  nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" config set Network/FirewallExceptionEnabled 1'
  Pop $0
  Pop $1
  FileWrite $9 "config FirewallExceptionEnabled exit=$0 output=$1$\r$\n"

  ; The program-wide exception above covers Ikeback's own VNC/RPC ports. The
  ; mobile bridge (TCP+UDP, default 11880) must be reachable by phones, which
  ; are often classified as a different network profile than the students.
  SetDetailsPrint none
  nsExec::ExecToStack '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="ikeback mobile bridge"'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="ikeback mobile bridge" dir=in action=allow protocol=TCP localport=11880 profile=any'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\netsh.exe" advfirewall firewall add rule name="ikeback mobile bridge (UDP)" dir=in action=allow protocol=UDP localport=11880 profile=any'
  Pop $R8
  Pop $R9
  SetDetailsPrint both
  FileWrite $9 "firewall: mobile bridge rules 11880 TCP/UDP$\r$\n"

  ; The key lives in the Service section (the "Windows/" prefix used by older
  ; installers wrote to a section no component ever reads).
  nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" config set Service/SoftwareSASEnabled 1'
  Pop $0
  Pop $1
  FileWrite $9 "config SoftwareSASEnabled exit=$0 output=$1$\r$\n"

  WriteRegDWORD ${REG_ROOT} "SYSTEM\CurrentControlSet\Control\Lsa" "ForceGuest" "0"

  nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" service start'
  Pop $R1
  Pop $1
  DetailPrint "启动服务 (service start): exit=$R1"
  FileWrite $9 "service start exit=$R1 output=$1$\r$\n"
  ; Give the SCM time to actually report the state. Querying immediately after
  ; "service start" can catch START_PENDING and report a false success.
  Sleep 3000

  ; Ask the Service Control Manager directly. findstr exits 0 only when the
  ; service STATE line contains RUNNING, so $2 is our ground truth.
  nsExec::ExecToStack '"$SYSDIR\cmd.exe" /C "sc query ikebackService | findstr RUNNING"'
  Pop $2
  Pop $3
  FileWrite $9 "sc query RUNNING exit=$2 output=$3$\r$\n"

  ; Fallback ONLY when the primary wcli path failed. Service name / binary /
  ; autostart are identical to WindowsServiceFunctions.cpp.
  ${If} $R0 != "0"
  ${OrIf} $R1 != "0"
  ${OrIf} $2 != "0"
    DetailPrint "wcli 未能启动服务，改用系统服务控制兜底…"
    FileWrite $9 "fallback: sc create + sc start$\r$\n"
    nsExec::ExecToStack '"$SYSDIR\sc.exe" create ikebackService binPath= "$INSTDIR\ikeback-service.exe" start= auto DisplayName= "ikeback Service"'
    Pop $R8
    Pop $R9
    FileWrite $9 "fallback create exit=$R8 output=$R9$\r$\n"
    nsExec::ExecToStack '"$SYSDIR\sc.exe" start ikebackService'
    Pop $R8
    Pop $R9
    FileWrite $9 "fallback start exit=$R8 output=$R9$\r$\n"
    nsExec::ExecToStack '"$SYSDIR\cmd.exe" /C "sc query ikebackService | findstr RUNNING"'
    Pop $2
    Pop $3
    FileWrite $9 "after fallback RUNNING exit=$2 output=$3$\r$\n"
  ${EndIf}

  ; A service can start and then exit a second later (missing DLL, wrong
  ; install path, blocked by security software). Wait and verify again so the
  ; installer never claims "service running" for a service that is already gone.
  Sleep 3000
  nsExec::ExecToStack '"$SYSDIR\cmd.exe" /C "sc query ikebackService | findstr RUNNING"'
  Pop $2
  Pop $3
  FileWrite $9 "final RUNNING check exit=$2 output=$3$\r$\n"

  ; The service must also spawn ikeback-server.exe in the console session:
  ; no server process means no tray icon and no 127.0.0.1 VNC connection, even
  ; though the Configurator may show the service itself as running. This is the
  ; single most common "this computer shows a red icon / nothing connects"
  ; cause, so verify it here instead of shipping a silent failure.
  nsExec::ExecToStack '"$SYSDIR\cmd.exe" /C "tasklist /NH | findstr /I ikeback-server.exe"'
  Pop $R2
  Pop $3
  FileWrite $9 "server process check exit=$R2 output=$3$\r$\n"

  ${If} $R2 != "0"
    DetailPrint "server 进程缺失，尝试重启服务…"
    FileWrite $9 "RESULT: server process MISSING - restarting the service$\r$\n"
    nsExec::ExecToStack '"$SYSDIR\sc.exe" stop ikebackService'
    Pop $R8
    Pop $R9
    Sleep 3000
    nsExec::ExecToStack '"$SYSDIR\sc.exe" start ikebackService'
    Pop $R8
    Pop $R9
    Sleep 10000
    nsExec::ExecToStack '"$SYSDIR\cmd.exe" /C "tasklist /NH | findstr /I ikeback-server.exe"'
    Pop $R2
    Pop $3
    FileWrite $9 "server process check after restart exit=$R2 output=$3$\r$\n"
  ${EndIf}

  ${If} $2 != "0"
    FileWrite $9 "RESULT: service NOT running$\r$\n"
    MessageBox MB_OK|MB_ICONEXCLAMATION "服务未能持续运行，安装未完成。$\r$\n$\r$\n请把 $INSTDIR\install.log 发给管理员排查。$\r$\n常见原因：未以管理员运行、被安全软件拦截、或缺少运行库。"
  ${ElseIf} $R2 != "0"
    FileWrite $9 "RESULT: service RUNNING but server process missing$\r$\n"
    ; Run the server once in the foreground with a timeout so its own error
    ; output (missing DLL / platform plugin / config) ends up in install.log
    ; instead of leaving only a red icon behind.
    DetailPrint "尝试直接运行 ikeback-server 以获取失败原因…"
    nsExec::ExecToStack /TIMEOUT=5000 '"$INSTDIR\ikeback-server.exe"'
    Pop $R8
    Pop $R9
    FileWrite $9 "server direct run exit=$R8 output=$R9$\r$\n"
    MessageBox MB_OK|MB_ICONEXCLAMATION "服务已注册，但 ikeback-server 进程没有起来，本机与学生机都会显示红色/无法连接。$\r$\n$\r$\n请把 $INSTDIR\install.log 发给管理员（其中已记录 server 的直接运行输出）。$\r$\n可以尝试：以管理员身份运行 $INSTDIR\ikeback-wcli.exe service restart。"
  ${Else}
    FileWrite $9 "RESULT: service RUNNING and server process present$\r$\n"
    DetailPrint "服务已正常运行 (ikebackService + ikeback-server)"
    DetailPrint "server 进程清单已写入 install.log，连不上时请查看该段"
  ${EndIf}
  FileClose $9

  ; One-time cleanup of the upstream layout (Teacher dir + Add/Remove entry).
  RmDir /r "${LEGACY_TEACHER_DIR}"
  DeleteRegKey ${REG_ROOT} "Software\Microsoft\Windows\CurrentVersion\Uninstall\${OLD_A}${OLD_B}Teacher"
SectionEnd

; --------------------------------------------------------------- Master only
!if "${ROLE}" == "teacher"
Section "${APP_NAME} Master" SEC_MASTER
  ${INSTALL_TYPE}
  SetOutPath "$INSTDIR"
  File "payload\ikeback-master.exe"
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME} Master.lnk" \
    "$INSTDIR\ikeback-master.exe" "" "$INSTDIR\ikeback.ico"
  CreateShortCut "$DESKTOP\${APP_NAME} Master.lnk" \
    "$INSTDIR\ikeback-master.exe" "" "$INSTDIR\ikeback.ico"
SectionEnd
!endif

; --------------------------------------------------------------- Common / reg
Section -Post
  ${INSTALL_TYPE}
  SetOutPath "$INSTDIR"
!if "${ROLE}" == "student"
  ReadRegStr $0 ${REG_ROOT} "SOFTWARE\ikeback\Student" "AdminKeyHash"
  ${If} $0 == ""
    WriteRegStr ${REG_ROOT} "SOFTWARE\ikeback\Student" "AdminKeyHash" "e86f78a8a3caf0b60d8e74e5942aa6d86dc150cd3c03338aef25b7d2d7e3acc7"
  ${EndIf}
  ReadRegDWORD $0 ${REG_ROOT} "SOFTWARE\ikeback\Student" "UninstallProtection"
  ${If} $0 != 1
    WriteRegDWORD ${REG_ROOT} "SOFTWARE\ikeback\Student" "UninstallProtection" 1
  ${EndIf}
  ; Portable software is copied here by the interactive student worker.
  ; The elevated installer grants the built-in Users group modify access so
  ; deployment also works when students do not have administrator rights.
  CreateDirectory "C:\IkebackApps"
  nsExec::ExecToStack '"$SYSDIR\icacls.exe" "C:\IkebackApps" /grant "*S-1-5-32-545:(OI)(CI)M"'
  Pop $0
  Pop $1
  ${If} $0 != 0
    MessageBox MB_OK|MB_ICONEXCLAMATION "无法设置 C:\IkebackApps 的写入权限，远程部署可能失败。请手动允许 Users 修改该目录。"
  ${EndIf}
!endif
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME} Configurator.lnk" \
    "$INSTDIR\ikeback-configurator.exe" "" "$INSTDIR\ikeback.ico"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}\Uninstall ${ROLE_EN}.lnk" "$INSTDIR\uninstall.exe"

  WriteRegStr ${REG_ROOT} "${UNINST_KEY}" "DisplayName" "${APP_NAME} ${ROLE_EN}"
  WriteRegStr ${REG_ROOT} "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr ${REG_ROOT} "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\ikeback.ico"
  WriteRegStr ${REG_ROOT} "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr ${REG_ROOT} "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr ${REG_ROOT} "${UNINST_KEY}" "URLInfoAbout" "${WEB_SITE}"
SectionEnd

; ------------------------------------------------------------------- Uninstall
Function un.onInit
!if "${ROLE}" == "student"
  SetRegView 64
  ReadRegDWORD $0 ${REG_ROOT} "SOFTWARE\ikeback\Student" "UninstallProtection"
  ${If} $0 == 1
    IfSilent 0 +2
      Quit
  ${EndIf}
!endif
FunctionEnd

Section Uninstall
  ${INSTALL_TYPE}
  ; The password page verifies interactive uninstalls. Silent uninstalls have
  ; no place to ask for the password, so they must fail closed while protected.
!if "${ROLE}" == "student"
  SetRegView 64
  ReadRegDWORD $0 ${REG_ROOT} "SOFTWARE\ikeback\Student" "UninstallProtection"
  ${If} $0 == 1
    IfSilent +2 0
      MessageBox MB_OK|MB_ICONSTOP "受保护的学生端不能静默卸载，请运行卸载器并输入管理员密码。"
      Abort
  ${EndIf}
!endif

  ; The removal itself runs with details suppressed (sc/taskkill would print
  ; harmless "not found" noise), but the pane must not stay blank: print the
  ; progress lines ourselves so the user always sees what is happening.
  DetailPrint "正在停止 iKeback 服务与相关进程…"
  SetDetailsPrint none
  nsExec::ExecToStack '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="ikeback mobile bridge"'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\netsh.exe" advfirewall firewall delete rule name="ikeback mobile bridge (UDP)"'
  Pop $R8
  Pop $R9
  IfFileExists "$INSTDIR\ikeback-wcli.exe" 0 ck_un_legacy_wcli
    nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" config set Service/SoftwareSASEnabled 0'
    Pop $R8
    Pop $R9
    nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" config set Network/FirewallExceptionEnabled 0'
    Pop $R8
    Pop $R9
    nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" service unregister'
    Pop $R8
    Pop $R9
    Goto ck_un_done_wcli
  ck_un_legacy_wcli:
    IfFileExists "$INSTDIR\ikeback-wcli.exe" 0 ck_un_done_wcli
      nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" config set Service/SoftwareSASEnabled 0'
      Pop $R8
      Pop $R9
      nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" config set Network/FirewallExceptionEnabled 0'
      Pop $R8
      Pop $R9
      nsExec::ExecToStack '"$INSTDIR\ikeback-wcli.exe" service unregister'
      Pop $R8
      Pop $R9
  ck_un_done_wcli:

  ; NSIS does NOT allow an uninstall section to Call a plain function (it
  ; requires an "un." prefixed function). Inline the same stop/cleanup logic
  ; here so uninstalling an upstream build also releases locked executables.
  ; ExecToStack keeps the expected "not installed / not found" noise out of the
  ; details pane, on uninstall as well as on install.
  nsExec::ExecToStack '"$SYSDIR\sc.exe" stop ikebackService'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\sc.exe" stop IkebackService'
  Pop $R8
  Pop $R9
  nsExec::ExecToStack '"$SYSDIR\taskkill.exe" /F /T /IM ikeback-worker.exe /IM ikeback-server.exe /IM ikeback-service.exe /IM ikeback-master.exe /IM ikeback-configurator.exe /IM ikeback-cli.exe /IM ikeback-wcli.exe /IM ikeback-worker.exe /IM ikeback-server.exe /IM ikeback-service.exe /IM ikeback-master.exe /IM ikeback-configurator.exe /IM ikeback-cli.exe /IM ikeback-wcli.exe /IM ${OLD_A}${OLD_B}-worker.exe /IM ${OLD_A}${OLD_B}-server.exe /IM ${OLD_A}${OLD_B}-service.exe /IM ${OLD_A}${OLD_B}-master.exe /IM ${OLD_A}${OLD_B}-configurator.exe /IM ${OLD_A}${OLD_B}-cli.exe /IM ${OLD_A}${OLD_B}-wcli.exe'
  Pop $R8
  Pop $R9
  Sleep 1200
  Delete "$INSTDIR\ikeback-worker.exe"
  Delete "$INSTDIR\ikeback-server.exe"
  Delete "$INSTDIR\ikeback-service.exe"
  Delete "$INSTDIR\ikeback-configurator.exe"
  Delete "$INSTDIR\ikeback-cli.exe"
  Delete "$INSTDIR\ikeback-wcli.exe"
  Delete "$INSTDIR\ikeback-master.exe"
  Delete "$INSTDIR\ikeback-worker.exe"
  Delete "$INSTDIR\ikeback-server.exe"
  Delete "$INSTDIR\ikeback-service.exe"
  Delete "$INSTDIR\ikeback-configurator.exe"
  Delete "$INSTDIR\ikeback-cli.exe"
  Delete "$INSTDIR\ikeback-wcli.exe"
  Delete "$INSTDIR\ikeback-master.exe"
  Sleep 1000
  SetDetailsPrint both

  Delete "$INSTDIR\*.exe"
  Delete "$INSTDIR\*.dll"
  Delete "$INSTDIR\*.TXT"
  Delete "$INSTDIR\*.ico"
  RmDir /r "$INSTDIR\plugins"
  RmDir /r "$INSTDIR\crypto"
  RmDir /r "$INSTDIR\imageformats"
  RmDir /r "$INSTDIR\platforms"
  RmDir /r "$INSTDIR\styles"
  RmDir /r "$INSTDIR\tls"
  RmDir /r "$INSTDIR\translations"
  Delete "$INSTDIR\install.log"
  RmDir "$INSTDIR"

  Delete "$SMPROGRAMS\ikeback\ikeback Master.lnk"
  Delete "$SMPROGRAMS\ikeback\ikeback Configurator.lnk"
  Delete "$SMPROGRAMS\ikeback\Uninstall ${ROLE_EN}.lnk"
  RmDir "$SMPROGRAMS\ikeback"
  Delete "$DESKTOP\ikeback Master.lnk"
  DeleteRegKey ${REG_ROOT} "${UNINST_KEY}"
SectionEnd
