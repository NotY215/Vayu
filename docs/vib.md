# VIB and the Integrator Architecture

## Purpose

**VIB (Vayu Integration Backend)** is the integrated toolchain system inside the Vayu repository. **Integrator** is the architecture VIB implements. They are not separate products: Vayu remains the language, and the existing Vayu compiler executable remains the user-facing entry point.

VIB is responsible for connecting the frontend, shared intermediate representation, VM, native compiler, runtime, linker interface, cache, incremental build graph, and execution dispatcher into one coherent system.

## Target architecture

```text
Vayu source (.vyu)
        |
        v
Lexer -> Parser -> AST -> Type/Semantic Analysis
        |
        v
Verified Vayu IR (VIR)
        |
        +---------------------+----------------------+
        |                     |                      |
        v                     v                      v
 Interpreter route        Bytecode/VM            Native route
        |                     |                      |
        +---------- shared semantic contract -------+
                                                      |
                                                      v
                                             Native code generation
                                                      |
                                                      v
                                           Object/executable emission
                                                      |
                                           Optional platform linker
                                                      |
                                                      v
                                             Native artifact/cache
```

The diagram describes the target design, not a claim that every box is already implemented.

## Design principles

### One source language and one product

- Source extension: `.vyu`.
- Vayu owns language semantics, diagnostics, compiler orchestration, and artifact identity.
- VIB is implemented inside this repository under `VIB/`.
- No separate VIB command-line product is required.
- The Integrator architecture must not create a second language frontend or duplicate language semantics.

### Shared semantics, multiple execution routes

Interpreter, VM, and native execution must agree on observable behavior for every feature claimed as supported. The shared conformance suite compares standard output, standard error, exit status, exceptions, side effects, and relevant resource behavior. A route must reject unsupported constructs rather than silently produce incorrect output.

### Vayu-owned IR

VIB will define a typed, versioned internal representation called VIR. It must represent control flow, values, types, calls, memory operations, runtime operations, and source locations. A verifier must run before code generation. VIR serialization is initially a debugging and test facility, not a stable public interchange format.

### Native output and platform tools

VIB owns lowering and code generation. It may call a standard system linker through a small, tested adapter when object linking is appropriate. A linker does not define Vayu's language semantics or replace VIB's code generator. Target-specific details such as PE/COFF and ELF must be isolated behind target interfaces.

### Cache and incremental builds

Artifact identity must include source and transitive dependency fingerprints, compiler revision, VIR schema, target, optimization settings, runtime ABI, and any external tool identity that can affect output. Writes must be atomic. Corrupt or stale artifacts must be rejected. Incremental builds must match clean-build behavior.

### Execution policy

Explicit interpreter, VM, and native modes come first. Automatic dispatch can be introduced only when capability metadata and conformance evidence are available. The dispatcher must make its decision inspectable and offer deterministic overrides. Profiling-guided/adaptive compilation is a later experimental phase, not a prerequisite for a correct compiler.

## Performance claims and AIFIAR-style prediction

Performance expectations are hypotheses until measured. Keep predictions in `VIB/docs/BENCHMARK_PREDICTIONS.md`, recording:

- hypothesis and proposed mechanism;
- confidence and assumptions;
- baseline and exact benchmark command;
- workload, machine, OS, compiler revision and target;
- cold build, warm-cache build, incremental build, startup and steady-state runtime;
- repetitions, median, variance/spread, memory and output size;
- a condition that would disprove the hypothesis;
- observed result and the updated forecast.

The idea of outperforming a JIT is workload-dependent. It must not be promised in advance; compilation cost, startup, cache hits, warm-up, profiling overhead and steady-state speed must be compared separately.

## Migration safety

The old native implementation must not be removed until the VIB replacement has a clear compatibility boundary and tests. However, the final architecture must not resolve or invoke the old backend. The migration gate is:

1. inventory all source, build, CLI, test, tool, and documentation references;
2. implement and test VIB IR and a minimal native subset;
3. establish interpreter/VM/native parity for that subset;
4. route native CLI modes through VIB;
5. test from a clean checkout with no legacy executable present;
6. remove legacy files, formats, environment variables, tests, and documentation;
7. run repository-wide reference search and full supported test suite.

See [`../Roadmap.md`](../Roadmap.md) for phase-by-phase work and exit criteria.

## Full technical reference

For the deep architecture and implementation reference, see [VIB Technical Documentation](vib-documentation.md). It covers compilation flow, component ownership, VIR, verification, semantics, native code generation, runtime and ABI, targets and linking, caching, incremental compilation, dispatch, diagnostics, testing, benchmarking, and failure handling.
