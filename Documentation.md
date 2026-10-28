# Vayu Documentation

**Python-inspired syntax · Native-execution goals · Low-level control**

- [Official website](https://vayu.gt.tc)
- [Syntax reference](docs/syntax.md)
- [VIB / Integrator architecture](docs/vib.md)
- [Canonical engineering roadmap](Roadmap.md)
- [VIB implementation home](VIB/README.md)
- [Benchmark prediction register](VIB/docs/BENCHMARK_PREDICTIONS.md)
- [Package system](docs/package.md)
- [AI / ML direction](docs/ai.md)
- [Compiler data flow](docs/data-flow.md)
- [Tools directory](tools/README.md)
- [Third-party software and files](Third_Party.md)
- [Support](SUPPORT.md)

## Project model

Vayu is the source language; source files use `.vyu`. The compiler repository contains the frontend, AST, semantic analysis, interpreter, bytecode compiler/VM, runtime integrations, and developer tools.

**VIB (Vayu Integration Backend)** is the integrated toolchain system being implemented inside this repository. **Integrator** is the architecture VIB implements, not a separate product. The user-facing entry point remains the Vayu compiler executable; no separate VIB executable is required.

## Current transition state

The old external-backend lowering and invocation path has been removed from the active native compiler source. The VIB native code generator and verified IR are not implemented yet, so native compilation is temporarily unavailable. Use `--run` or `--vm` for execution. This state is documented explicitly to avoid claiming the target architecture is complete before it is tested.

The canonical implementation sequence, historical phase mapping, acceptance criteria, and benchmark policy live in [Roadmap.md](Roadmap.md).
