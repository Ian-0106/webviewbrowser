!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"

!ifndef BUILD_UNINSTALLER
  Var DesktopShortcutCheckbox

  !macro customPageAfterChangeDir
    Page custom DesktopShortcutPageCreate DesktopShortcutPageLeave
  !macroend

  Function DesktopShortcutPageCreate
    nsDialogs::Create 1018
    Pop $0
    ${If} $0 == error
      Abort
    ${EndIf}
    !insertmacro MUI_HEADER_TEXT "Shortcuts" "Choose optional shortcuts."
    ${NSD_CreateCheckbox} 0 12u 100% 12u "Create a desktop shortcut"
    Pop $DesktopShortcutCheckbox
    ${NSD_Uncheck} $DesktopShortcutCheckbox
    nsDialogs::Show
  FunctionEnd

  Function DesktopShortcutPageLeave
  FunctionEnd

  !macro customInstall
    ${NSD_GetState} $DesktopShortcutCheckbox $0
    ${If} $0 == ${BST_CHECKED}
      CreateShortcut "$DESKTOP\WebView Browser Electron.lnk" "$INSTDIR\${APP_EXECUTABLE_FILENAME}"
    ${EndIf}
  !macroend
!else
  Var DeleteUserDataCheckbox

  !macro customUnWelcomePage
    UninstPage custom un.DeleteDataPageCreate un.DeleteDataPageLeave
  !macroend

  Function un.DeleteDataPageCreate
    nsDialogs::Create 1018
    Pop $0
    ${If} $0 == error
      Abort
    ${EndIf}
    !insertmacro MUI_HEADER_TEXT "Browsing data" "Choose whether to remove local activity records."
    ${NSD_CreateCheckbox} 0 12u 100% 24u "Delete browsing history and download records (downloaded files are never deleted)"
    Pop $DeleteUserDataCheckbox
    ${NSD_Uncheck} $DeleteUserDataCheckbox
    nsDialogs::Show
  FunctionEnd

  Function un.DeleteDataPageLeave
  FunctionEnd

  !macro customUnInstall
    DeleteRegKey HKCU "Software\Classes\CLSID\{923A6F87-8D59-4EBE-8F7D-6D1511D7E30C}"
    ${NSD_GetState} $DeleteUserDataCheckbox $0
    ${If} $0 == ${BST_CHECKED}
      RMDir /r "$APPDATA\WebView Browser Electron"
    ${EndIf}
  !macroend
!endif
