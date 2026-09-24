# Vayu for Visual Studio Community 2022

Syntax highlighting for `.vyu` files in Visual Studio 2022 Community.
Uses VS's native TextMate grammar support — no compiled component.

## Build

    powershell -ExecutionPolicy Bypass -File build.ps1

Produces `vayu-vs.vsix`.

## Install

    .\build.ps1
    # then double-click vayu-vs.vsix
    # or: devenv /updateconfiguration  (after installing via VSIXInstaller)

VSIXInstaller.exe is at:
  C:\Program Files\Microsoft Visual Studio\2022\Community\
      Common7\IDE\VSIXInstaller.exe