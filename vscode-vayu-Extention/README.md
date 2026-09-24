# Vayu for VS Code

Language support for Vayu (`.vyu` files) — works with **VS Code** and
**VS Code - OSS / Community**.

## Features

- **Syntax highlighting** — keywords, types, builtins, operators, strings,
  comments.
- **Diagnostics** — parse errors and type errors shown inline as you type,
  powered by `vls.exe`.
- **Hover** — markdown tooltip with the declaration location.
- **Goto definition** — Ctrl-click a name to jump to its declaration.
- **Completion** — `.` triggers a list of builtins and top-level names.
- **Format file** — Ctrl+Shift+P → "Vayu: Format File" (uses `vfmt.exe`).
- **Lint file** — "Vayu: Lint File" (uses `vlint.exe`).
- **Restart server** — "Vayu: Restart Language Server".

## Setup

1. Build the toolchain:
```
cmake --build build/x64-debug
```
This produces `vls.exe`, `vfmt.exe`, `vlint.exe` under
`build/x64-debug/bin/`.

2. Install this extension (or the packaged `.vsix`).

3. Reload VS Code, open any `.vyu` file.

## Settings

| Key | Default | Meaning |
|---|---|---|
| `vayu.vlsPath`  | `build/x64-debug/bin/vls.exe`  | Language server |
| `vayu.vfmtPath` | `build/x64-debug/bin/vfmt.exe` | Formatter |
| `vayu.vlintPath`| `build/x64-debug/bin/vlint.exe`| Linter |

Paths are resolved relative to the workspace root unless absolute.

## Commands

- `Vayu: Format File`
- `Vayu: Lint File`
- `Vayu: Restart Language Server`

## Packaging
```
npm install -g @vscode/vsce
cd vscode-vayu-Extention
vsce package
```

Produces `vayu-%version%.vsix`, installable in both VS Code and VS Code - OSS.

## Repository

https://github.com/NotY215/Vayu

## License

Apache-2.0. See [LICENSE](./LICENSE).