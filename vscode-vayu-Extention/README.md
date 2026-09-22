# Vayu for VS Code

Language support for Vayu (`.vyu` files).

## Features

- Syntax highlighting (TextMate grammar).
- Diagnostics via `vls.exe` on every edit.
- Hover, goto-definition, completion (`.` triggers).
- Format file: `vayu.formatFile` (uses `vfmt.exe`).
- Lint file: `vayu.lintFile` (uses `vlint.exe`).
- Restart server: `vayu.restartServer`.

## Setup

1. Build the toolchain:
```
cmake --build build/x64-debug
This produces `vls.exe`, `vfmt.exe`, `vlint.exe` under
`build/x64-debug/bin/`.
```
2. Install this extension:
```
cd vscode-vayu-Extention
code --install-extension . --force

Or copy the `vscode-vayu/` folder into
`%USERPROFILE%\.vscode\extensions\vayu-lang.vayu-0.1.0\`.
```
3. Reload VS Code, open any `.vyu` file.

## Settings

| Key | Default | Meaning |
|---|---|---|
| `vayu.vlsPath`  | `build/x64-debug/bin/vls.exe`  | Language server |
| `vayu.vfmtPath` | `build/x64-debug/bin/vfmt.exe` | Formatter |
| `vayu.vlintPath`| `build/x64-debug/bin/vlint.exe`| Linter |

Paths are resolved relative to the workspace root unless absolute.

## Commands

- `Vayu: Format File` — Ctrl+Shift+P then type "Vayu Format".
- `Vayu: Lint File`
- `Vayu: Restart Language Server`