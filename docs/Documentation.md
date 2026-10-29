# Vayu Documentation

**Python-inspired syntax · Native-execution goals · Low-level control**

## Reference index

- [Official website](https://vayu.gt.tc)
- [Syntax reference](syntax.md)
- [VIB / Integrator architecture](vib.md)
- [Complete VIB technical documentation](vib-documentation.md)
- [Canonical engineering roadmap](../Roadmap.md)
- [VIB implementation home](../VIB/README.md)
- [Benchmark prediction register](../VIB/docs/BENCHMARK_PREDICTIONS.md)
- [Package system](package.md)
- [AI / ML direction](ai.md)
- [Compiler data flow](data-flow.md)
- [Executable and source-file icons](icons.md)
- [Tools directory](../tools/README.md)
- [Third-party software and files](../Third_Party.md)
- [Support](../Legal/SUPPORT.md)

## Project model

Vayu is the source language; source files use `.vyu`. The compiler repository contains the frontend, AST, semantic analysis, interpreter, bytecode compiler/VM, runtime integrations, and developer tools.

**VIB (Vayu Integration Backend)** is the integrated toolchain system being implemented inside this repository. **Integrator** is the architecture VIB implements, not a separate product. The user-facing entry point remains the Vayu compiler executable; no separate VIB executable is required.

## Current compiler status

The active VIB native compiler currently returns an explicit not-implemented error. As a result, a fresh native `.exe` cannot currently be generated through `--native-out`. Use `--run` or `--vm` for supported execution workflows. The build script applies the Vayu application icon only after native compilation successfully creates `vayu.exe`; icon handling does not imply that native compilation is ready.

## Icon behavior

- `assets/Vayu_logo.ico` is the intended icon embedded into a generated Windows `vayu.exe`.
- `assets/Vyu_Icon.ico` and `assets/Vyu_Icon_Dark.ico` are the canonical light and dark source-file icon assets.
- The VS Code extension declares both source icons. A third-party file-icon theme may override language-provided icons.
- The Visual Studio Community package currently registers the language and includes both assets, but its TextMate-only registration does not guarantee theme-aware Solution Explorer file icons.

See [Icon Support](icons.md) for implementation and verification details.

## Roadmap and acceptance

The canonical implementation sequence, historical phase mapping, acceptance criteria, and benchmark policy live in [../Roadmap.md](../Roadmap.md). A feature is not considered implemented solely because a document or build script mentions it; it needs working code and passing tests.
