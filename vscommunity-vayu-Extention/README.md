# Vayu for Visual Studio Community

Syntax highlighting for \`.vyu\` files in Visual Studio Community.
Uses Visual Studio's native TextMate grammar support — no compiled component.

## Build

Run PowerShell from this folder:

    powershell -ExecutionPolicy Bypass -File .\\build.ps1

This creates:

    vayu-vs-community-0.2.0.vsix

Do not use \`vsce package\` for this extension. \`vsce\` is the VS Code Extension Manager and creates a VS Code-style package layout. The included \`build.ps1\` creates the Visual Studio VSIX package directly with \`extension.vsixmanifest\` at the VSIX root.

## Install

Double-click the generated \`.vsix\` file, or run:

    "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\VSIXInstaller.exe" "vayu-vs-community-0.2.0.vsix"

After installation, restart Visual Studio if it was open.
