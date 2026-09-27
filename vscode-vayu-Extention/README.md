# Vayu for VS Code

Language support for Vayu (.vyu) in Visual Studio Code and VS Code-compatible editors.

## Features

- Syntax highlighting for the current Vayu grammar
- Language Server Protocol integration through `vls`
- Inline diagnostics
- Hover information
- Go to definition
- Completion
- Formatter integration through `vfmt`
- Linter integration through `vlint`
- Restart-language-server command

## Toolchain

Build the Vayu compiler/tooling from the repository root:

    cmake --build build/x64-debug

The debug build provides:

    build/x64-debug/bin/vls.exe
    build/x64-debug/bin/vfmt.exe
    build/x64-debug/bin/vlint.exe

The extension can auto-detect these paths from the workspace or use explicit settings.

## Settings

| Setting | Default | Purpose |
|---|---|---|
| `vayu.vlsPath` | empty | Path to `vls`; empty enables workspace auto-detection |
| `vayu.vfmtPath` | empty | Path to `vfmt`; empty enables auto-detection |
| `vayu.vlintPath` | empty | Path to `vlint`; empty enables auto-detection |

## Commands

- `Vayu: Format File`
- `Vayu: Lint File`
- `Vayu: Restart Language Server`

## Packaging

This is a normal VS Code extension and uses `@vscode/vsce`:

    npm install -g @vscode/vsce
    cd vscode-vayu-Extention
    vsce package

The current package version is `1.0.0` and the Marketplace publisher is `Fliczo`.

## Repository

https://github.com/NotY215/Vayu

## License

Apache-2.0. See [LICENSE](./LICENSE).