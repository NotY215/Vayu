# Vayu Vision

Vayu is an independent programming language focused on readable syntax, static typing, native execution, low-level control, interoperability, and a growing developer ecosystem.

The language frontend, interpreter, bytecode compiler/VM, standard runtime, package tooling, and editor tooling are existing foundations. Native compilation is now being reorganized around **VIB (Vayu Integration Backend)**, which implements the **Integrator architecture** inside this repository.

## Architecture decision

Vayu remains the language and the compiler executable remains the user-facing entry point. VIB is the integrated toolchain system, not a separate required executable. The target system owns the Vayu IR, verification, native code generation, runtime integration, optional linker adapter, persistent cache, incremental builds, and execution dispatch.

The old external-backend implementation has been removed from the active native compiler source. VIB's replacement native code generator is **not yet implemented**, so native execution is temporarily unavailable on this transition branch. Use `--run` or `--vm` for execution while the VIB phases are implemented. This is an explicit transition state, not a claim that native compilation is complete.

## Roadmap and documents

- [Canonical engineering roadmap](../Roadmap.md): phase map, legacy-phase mapping, implementation gates, tests, and benchmark policy.
- [VIB architecture](vib.md): target architecture, IR, runtime, cache, dispatch, and migration safety.
- [VIB implementation home](../VIB/README.md): scope and intended module boundaries.
- [Benchmark prediction register](../VIB/docs/BENCHMARK_PREDICTIONS.md): hypotheses and evidence tracking.
- [Syntax reference](syntax.md)
- [Package system](package.md)
- [AI/ML direction](ai.md)

## Status policy

A planned feature is not an implemented feature. Mark work complete only after code, automated tests, documentation, and acceptance criteria are present. Published performance claims must be reproducible and workload-specific. “Better than JIT” remains a hypothesis to test, not a guarantee.
