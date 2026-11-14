; JSONfs installer / uninstaller for Windows XP SP2 .. Windows 11 (NSIS 3).
;
; Build (done by scripts/build-release.sh):
;   makensis -DVERSION=1.0.0 -DARCH=x64 -DBINDIR=build/windows-x64/bin \
;            -DSRCROOT=. -DOUTFILE=dist/jsonfs-1.0.0-windows-x64-setup.exe \
;            [-DWINFSP_MSI=path\to\winfsp.msi] [-DDOKAN_EXE=path\to\DokanInstall_0.6.0.exe] \
;            (with the driver defines: the "-setup-with-drivers.exe" variant, see docs/PACKAGES.md)
;            installer/windows/jsonfs.nsi
;
; The installer copies jsonfs.exe (toolkit) and jsonfs-mount.exe (driver
; front end), registers an uninstaller in "Add/Remove Programs", optionally
; adds the installation directory to the system PATH and checks for a kernel
; filesystem driver:
;   Windows 7 and later : WinFsp  (https://winfsp.dev)
;   Windows XP / Vista  : Dokan 0.6.0
; The driver packages can be embedded with -DWINFSP_MSI / -DDOKAN_EXE;
; otherwise the user is pointed to their download pages.
; Silent install: setup.exe /S [/D=C:\path];  silent uninstall: uninstall.exe /S

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "1.0.0"
!endif
!ifndef ARCH
  !define ARCH "x86"
!endif
!ifndef SRCROOT
  !define SRCROOT "..\.."
!endif
!ifndef BINDIR
  !define BINDIR "${SRCROOT}\build\windows-${ARCH}\bin"
!endif
!ifndef OUTFILE
  !define OUTFILE "jsonfs-${VERSION}-windows-${ARCH}-setup.exe"
!endif

!define PRODUCT "JSONfs"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\JSONfs"
!define WINFSP_URL "https://winfsp.dev/rel/"
!define DOKAN_URL "https://github.com/dokan-dev/dokany/releases/tag/0.6.0"

!ifdef WINFSP_MSI
  !define FLAVOR " with drivers"
!else
  !define FLAVOR ""
!endif
Name "${PRODUCT} ${VERSION} (${ARCH})${FLAVOR}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
BrandingText "${PRODUCT} ${VERSION}"
!if "${ARCH}" == "x86"
  InstallDir "$PROGRAMFILES\JSONfs"
!else
  InstallDir "$PROGRAMFILES64\JSONfs"
!endif

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "FileDescription" "JSONfs filesystem installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "JSONfs contributors (MIT license)"

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This will install JSONfs ${VERSION}: a POSIX filesystem stored in a single JSON document.$\r$\n$\r$\nIncluded: the jsonfs maintenance toolkit and the jsonfs-mount driver front end. Mounting needs WinFsp (Windows 7 and later) or Dokan 0.6.0 (Windows XP / Vista).$\r$\n$\r$\nWinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos (https://github.com/winfsp/winfsp)."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCROOT}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var DRIVER

Function .onInit
  ${IfNot} ${AtLeastWinXP}
    MessageBox MB_ICONSTOP "JSONfs requires Windows XP or later." /SD IDOK
    Abort
  ${EndIf}
!if "${ARCH}" != "x86"
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "This is the 64-bit package. Please use the x86 installer on 32-bit Windows." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
!endif
!if "${ARCH}" == "arm64"
  ${IfNot} ${AtLeastWin10}
    MessageBox MB_ICONSTOP "The ARM64 package needs Windows 10 or later." /SD IDOK
    Abort
  ${EndIf}
!endif
FunctionEnd

Function un.onInit
!if "${ARCH}" != "x86"
  SetRegView 64
!endif
FunctionEnd

; Sets $DRIVER to "winfsp", "dokan" or "".
Function DetectDriver
  StrCpy $DRIVER ""
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\WinFsp" "InstallDir"
  ${If} $0 == ""
    ReadRegStr $0 HKLM "SOFTWARE\WinFsp" "InstallDir"
  ${EndIf}
  ${If} $0 != ""
    StrCpy $DRIVER "winfsp"
    Return
  ${EndIf}
  ${If} ${RunningX64}
    ${DisableX64FSRedirection}
  ${EndIf}
  IfFileExists "$SYSDIR\dokan.dll" 0 +2
    StrCpy $DRIVER "dokan"
  ${If} ${RunningX64}
    ${EnableX64FSRedirection}
  ${EndIf}
FunctionEnd

Section "JSONfs toolkit and mount driver (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "${BINDIR}\jsonfs.exe"
  File "${BINDIR}\jsonfs-mount.exe"
  File /oname=README.txt "${SRCROOT}\README.md"
  File /oname=LICENSE.txt "${SRCROOT}\LICENSE"
  File /oname=THIRD_PARTY_NOTICES.txt "${SRCROOT}\THIRD_PARTY_NOTICES.md"
  SetOutPath "$INSTDIR\docs"
  File "${SRCROOT}\docs\FORMAT.md"
  File "${SRCROOT}\docs\USAGE.md"
  File "${SRCROOT}\docs\BUILDING.md"
  File "${SRCROOT}\docs\PACKAGES.md"
  SetOutPath "$INSTDIR"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\JSONfs" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\JSONfs" "Version" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT} ${VERSION} (${ARCH})"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "JSONfs contributors"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\jsonfs.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 1024

  CreateDirectory "$SMPROGRAMS\JSONfs"
  CreateShortCut "$SMPROGRAMS\JSONfs\JSONfs Command Prompt.lnk" "$SYSDIR\cmd.exe" '/k "set PATH=$INSTDIR;%PATH% && jsonfs help"' "$INSTDIR\jsonfs.exe"
  CreateShortCut "$SMPROGRAMS\JSONfs\README.lnk" "$INSTDIR\README.txt"
  CreateShortCut "$SMPROGRAMS\JSONfs\Uninstall JSONfs.lnk" "$INSTDIR\uninstall.exe"
SectionEnd

Section "Add to the system PATH" SecPath
  ; done by the toolkit itself: NSIS strings are limited to 1024 characters,
  ; which could truncate a long PATH.
  nsExec::ExecToLog '"$INSTDIR\jsonfs.exe" win-path add "$INSTDIR"'
  Pop $0
  ${If} $0 != 0
    DetailPrint "Could not update PATH (exit code $0); use the JSONfs Command Prompt shortcut."
  ${EndIf}
  WriteRegDWORD HKLM "Software\JSONfs" "AddedToPath" 1
SectionEnd

!ifdef WINFSP_MSI
Section "Install the filesystem driver if missing (WinFsp / Dokan included)" SecDriver
!else
Section "Check for the filesystem driver (WinFsp or Dokan)" SecDriver
!endif
  Call DetectDriver
  ${If} $DRIVER != ""
    DetailPrint "Filesystem driver found: $DRIVER"
    Return
  ${EndIf}
  ${If} ${AtLeastWin7}
!ifdef WINFSP_MSI
    DetailPrint "Installing WinFsp..."
    InitPluginsDir
    File "/oname=$PLUGINSDIR\winfsp.msi" "${WINFSP_MSI}"
    ExecWait 'msiexec /i "$PLUGINSDIR\winfsp.msi" /passive' $0
    DetailPrint "WinFsp installer finished with code $0"
!else
    MessageBox MB_YESNO|MB_ICONINFORMATION "JSONfs needs WinFsp to mount images on this version of Windows.$\r$\nOpen the WinFsp download page now?" /SD IDNO IDNO +2
      ExecShell "open" "${WINFSP_URL}"
!endif
  ${Else}
!ifdef DOKAN_EXE
    DetailPrint "Installing Dokan 0.6.0..."
    InitPluginsDir
    File "/oname=$PLUGINSDIR\DokanInstall.exe" "${DOKAN_EXE}"
    ExecWait '"$PLUGINSDIR\DokanInstall.exe"' $0
    DetailPrint "Dokan installer finished with code $0"
!else
    MessageBox MB_YESNO|MB_ICONINFORMATION "JSONfs needs the Dokan 0.6.0 driver to mount images on Windows XP / Vista.$\r$\nOpen the Dokan download page now?" /SD IDNO IDNO +2
      ExecShell "open" "${DOKAN_URL}"
!endif
  ${EndIf}
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "jsonfs.exe (mkfs, fsck, snapshot, versioning, recompress, upgrade, import/export) and jsonfs-mount.exe."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecPath} "Lets you run jsonfs and jsonfs-mount from any command prompt."
!ifdef WINFSP_MSI
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver} "Installs the bundled WinFsp 2.0 (Windows 7+) or Dokan 0.6.0 (XP/Vista) unless a driver is already present."
!else
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDriver} "WinFsp (Windows 7+) or Dokan 0.6.0 (XP/Vista) provides the kernel side of the mount; offers the download page when missing."
!endif
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ReadRegDWORD $0 HKLM "Software\JSONfs" "AddedToPath"
  ${If} $0 == 1
    nsExec::ExecToLog '"$INSTDIR\jsonfs.exe" win-path remove "$INSTDIR"'
    Pop $0
  ${EndIf}
  Delete "$INSTDIR\jsonfs.exe"
  Delete "$INSTDIR\jsonfs-mount.exe"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
  Delete "$INSTDIR\docs\PACKAGES.md"
  Delete "$INSTDIR\docs\FORMAT.md"
  Delete "$INSTDIR\docs\USAGE.md"
  Delete "$INSTDIR\docs\BUILDING.md"
  RMDir "$INSTDIR\docs"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\JSONfs\JSONfs Command Prompt.lnk"
  Delete "$SMPROGRAMS\JSONfs\README.lnk"
  Delete "$SMPROGRAMS\JSONfs\Uninstall JSONfs.lnk"
  RMDir "$SMPROGRAMS\JSONfs"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\JSONfs"
  DetailPrint "WinFsp / Dokan are separate products and were left installed."
SectionEnd
