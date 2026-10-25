Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"

!define APP_NAME "WebView Browser"
!define APP_VERSION "1.3.0"
!define APP_ID "OpenCode.WebViewBrowser"
!define APP_EXE "webview_example.exe"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\WebViewBrowser"

Name "${APP_NAME}"
OutFile "..\dist\WebView-Browser-Setup-${APP_VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\WebView Browser"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"

Var DesktopCheckbox
Var DeleteDataCheckbox

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom ShortcutPageCreate ShortcutPageLeave
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
UninstPage custom un.DataPageCreate un.DataPageLeave
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"

Function ShortcutPageCreate
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Shortcuts" "Choose optional shortcuts."
  ${NSD_CreateCheckbox} 0 12u 100% 12u "Create a desktop shortcut"
  Pop $DesktopCheckbox
  ${NSD_Uncheck} $DesktopCheckbox
  nsDialogs::Show
FunctionEnd

Function ShortcutPageLeave
FunctionEnd

Section "Install" SEC_MAIN
  SetOutPath "$INSTDIR"
  File "..\build\release\Release\webview_example.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateDirectory "$SMPROGRAMS"
  CreateShortcut "$SMPROGRAMS\WebView Browser.lnk" "$INSTDIR\${APP_EXE}"
  ${NSD_GetState} $DesktopCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    CreateShortcut "$DESKTOP\WebView Browser.lnk" "$INSTDIR\${APP_EXE}"
  ${EndIf}

  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Function un.DataPageCreate
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Browsing data" "Choose whether to remove local activity records."
  ${NSD_CreateCheckbox} 0 12u 100% 24u "Delete browsing history and download records (downloaded files are never deleted)"
  Pop $DeleteDataCheckbox
  ${NSD_Uncheck} $DeleteDataCheckbox
  nsDialogs::Show
FunctionEnd

Function un.DataPageLeave
FunctionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\WebView Browser.lnk"
  Delete "$DESKTOP\WebView Browser.lnk"
  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  DeleteRegKey HKCU "Software\Classes\CLSID\{18B30D36-17EC-4DF4-A4D5-E92F11B37D62}"

  ${NSD_GetState} $DeleteDataCheckbox $0
  ${If} $0 == ${BST_CHECKED}
    RMDir /r "$LOCALAPPDATA\WebView Browser"
  ${EndIf}
SectionEnd
