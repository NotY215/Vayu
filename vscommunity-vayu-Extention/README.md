# Vayu for Visual Studio Community

Native Visual Studio Community language support for .vyu files using Visual Studio's TextMate grammar and VSIX packaging.

## Current package

- Version: `1.0.0`
- Publisher: `NotY215`
- Display name: **Vayu Language Support**
- Target: Visual Studio Community 17.14+
- Architecture: amd64
- Package: `vayu-lang-support-1.0.0.vsix`

## Features

- Vayu .vyu language registration
- TextMate syntax highlighting
- Language configuration
- Light and dark `.vyu` icon assets included in the VSIX
- Native Visual Studio VSIX installation

This extension is intentionally lightweight and does not embed the Vayu compiler or Language Server. Its current TextMate-based registration associates `.vyu` files with the Vayu language, but does not by itself implement theme-aware Solution Explorer file icons. Both icon assets are packaged for future Visual Studio file-icon integration; their presence in the VSIX should not be treated as proof that Visual Studio automatically displays them.

## Build

Run PowerShell from this folder:

    powershell -ExecutionPolicy Bypass -File .\build.ps1

The script uses the .NET SDK and the VSSDK build pipeline. It creates:

    vayu-lang-support-1.0.0.vsix

Generated bin and obj directories are removed by the build script.

Do **not** use `vsce package` for this extension. `vsce` creates a VS Code-style package; this project builds a Visual Studio VSIX with the official VSSDK tooling.

## Install

Double-click the generated .vsix file, or use Visual Studio's VSIX installer.

The package targets the Visual Studio 17.14+ Community core editor.

For the current icon implementation status and the distinction between VS Code and Visual Studio icon behavior, see the [icon support guide](../docs/icons.md).

## Repository

https://github.com/NotY215/Vayu

## License

Apache-2.0.