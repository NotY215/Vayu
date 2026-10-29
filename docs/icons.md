# Vayu executable and source-file icons

This document defines the icon assets and how they are applied. Icon support must not be confused with compiler support: the native Vayu compiler currently reports that VIB native code generation is not implemented, so a fresh `vayu.exe` cannot currently be produced by the repository's `--native-out` path. The build script applies the icon only after native compilation succeeds.

## Canonical assets

| Asset | Purpose |
|---|---|
| `assets/Vayu_logo.ico` | Icon embedded into the generated Windows `vayu.exe` |
| `assets/Vyu_Icon.ico` | Light-theme icon for `.vyu` source files in editor integrations |
| `assets/Vyu_Icon_Dark.ico` | Dark-theme icon for `.vyu` source files in editor integrations |

Keep these files as the canonical repository assets. Extension packages may carry copies under their own `assets/` directories because the editor packages must be self-contained.

## Windows executable icon

The root `Build_exe.bat` build flow invokes `tools/apply_vayu_icon.ps1` after `vayuc` successfully creates `vayu.exe`. The helper writes the icon image and icon-group resources into the PE executable using the Windows resource-update API. It does not require Resource Hacker, a third-party linker, a DLL bundle, or a separate icon-editing application.

Run from a Windows environment with PowerShell:

```bat
Build_exe.bat
```

The build script stops if the compiler fails, if the executable is not created, or if the icon cannot be embedded. It will not silently report an icon-enabled executable when the build failed. The other generated tools are not assigned the Vayu application icon by this step.

## .vyu file icons in VS Code

The VS Code extension declares the Vayu language's light and dark icons in `vscode-vayu-Extention/package.json`:

- `light` uses `assets/Vyu_Icon.ico`
- `dark` uses `assets/Vyu_Icon_Dark.ico`

The extension package includes both files. The editor selects the corresponding language icon based on the active light/dark theme context where the installed icon theme honors language-provided icons. A third-party file-icon theme can override this mapping; Vayu cannot force an icon over an explicit user-selected icon theme.

To use the icons, install or update the Vayu VS Code extension and reload the editor window. If the icon does not change, check the active File Icon Theme and whether it provides its own `.vyu` association.

## Visual Studio Community

The Visual Studio Community VSIX includes both `.ico` assets and registers `.vyu` as a Vayu language. Its current TextMate-based package provides syntax highlighting and language registration. TextMate grammar registration alone does not guarantee a theme-aware file icon in Visual Studio's Solution Explorer; that requires Visual Studio's file-icon/package integration. Do not claim automatic dark/light Solution Explorer icon switching until that integration is implemented and verified in the target Visual Studio version.

## Maintenance and verification

When changing an icon:

1. Update the canonical file under `assets/`.
2. Synchronize the corresponding file in each editor extension's `assets/` directory.
3. Keep the `light` and `dark` manifest paths valid and include both files in the packaged extension.
4. Build the Windows executable on a machine where the native compiler works, then inspect `vayu.exe` in Windows Explorer and its file Properties.
5. Install the VS Code extension and test `.vyu` files with both a light and a dark theme.
6. Build/install the Visual Studio Community VSIX separately; do not infer file-icon support from the presence of packaged icon files alone.
