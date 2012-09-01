Name "Mixmaster"

OutFile "Mixmaster-Setup.exe"

InstallDir $PROGRAMFILES\Mixmaster

; Registry key to check for directory (so if you install again, it will 
; overwrite the old one automatically)
InstallDirRegKey HKLM "Software\Mixmaster" "Install_Dir"

;--------------------------------

; Pages

Page components
Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

;--------------------------------

Section "Mixmaster"
  SectionIn RO
  SetOutPath $INSTDIR
  File "..\release\mix.exe"
  File "..\release\mixlib.dll"
  File "..\..\Src\openssl\out32dll\libeay32.dll"
  File "c:\winnt\system32\msvcr71.dll"

  WriteRegStr HKLM SOFTWARE\Mixmaster "Install_Dir" "$INSTDIR"

  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Mixmaster" "DisplayName" "Mixmaster"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Mixmaster" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Mixmaster" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Mixmaster" "NoRepair" 1
  WriteUninstaller "uninstall.exe"
SectionEnd

Section "Start Menu Shortcuts (All Users)"
  SetShellVarContext all
  CreateDirectory "$SMPROGRAMS\Mixmaster"
  CreateShortCut "$SMPROGRAMS\Mixmaster\Mixmaster.lnk" "$INSTDIR\mix.exe" "" "$INSTDIR\mix.exe" 0
  CreateShortCut "$SMPROGRAMS\Mixmaster\Uninstall Mixmaster.lnk" "$INSTDIR\uninstall.exe" "" "$INSTDIR\uninstall.exe" 0
SectionEnd

Section "Create Desktop Item (All Users)"
  SetShellVarContext all
  CreateShortCut "$DESKTOP\Mixmaster.lnk" "$INSTDIR\mix.exe" "" "$INSTDIR\mix.exe" 0
SectionEnd

Section "Uninstall"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Mixmaster"
  DeleteRegKey HKLM SOFTWARE\Mixmaster

  Delete $INSTDIR\mix.exe
  Delete $INSTDIR\mixlib.dll
  Delete $INSTDIR\libeay32.dll
  Delete $INSTDIR\msvcr71.dll
  Delete $INSTDIR\uninstall.exe

  SetShellVarContext all
  Delete "$SMPROGRAMS\Mixmaster\*.*"
  RMDir "$SMPROGRAMS\Mixmaster"
  Delete "$DESKTOP\Mixmaster.lnk"

  RMDir "$INSTDIR"
SectionEnd
