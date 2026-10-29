# VIB Technical Documentation
## Integrator architecture, .vye format, and implementation analysis

**VIB** means **Vayu Integration Backend**. It is the integrated compiler/runtime system inside the Vayu repository. **Integrator** is the architecture VIB implements. Vayu remains the language, `.vyu` remains the source extension, and the existing Vayu compiler driver remains the user-facing entry point. A separate `vib.exe` is not required.

This document distinguishes planned architecture from implemented functionality. A design, placeholder, header, example, or CLI option is not evidence that a component is complete.

## 1. First objective

Vayu already has broad frontend, interpreter/VM, and tooling coverage. Integrator deliberately starts smaller. The first goal is a correct and testable subset covering basic values, arithmetic, strings, functions, and control flow. Expand only after each subset passes its own conformance and malformed-input tests.

## 2. Target data flow

```text
.vyu source
  -> existing Vayu frontend
  -> Integrator lowering for supported subset
  -> verified internal representation
  -> deterministic .vye writer
  -> validated .vye package
  -> Vayu / Integrator runtime
  -> program output and exit status
```

This is a target design, not a statement that the full pipeline is already implemented.

## 3. Responsibilities and boundaries

VIB/Integrator owns:
- integration with the existing lexer, parser, and semantic analysis;
- lowering the supported source subset into a small internal representation;
- verifying types, control flow, indices, and runtime operations;
- deterministic `.vye` package writing and strict package loading;
- execution through a compatible Vayu/Integrator runtime;
- diagnostics, tests, version compatibility, and capability reporting.

The initial Integrator does not require:
- a separate VIB executable;
- a foreign-language runtime;
- a bundled DLL directory;
- bundled LLD executables or import libraries;
- an external compiler backend;
- Python, NumPy, or the ONNX Python package.

Native machine-code generation, caching, incremental compilation, adaptive execution, and self-hosting are later work.

## 4. .vye format design

A `.vye` file is a Vayu Executable package interpreted/executed by a compatible Vayu/Integrator runtime. It is not a native Windows executable and cannot be executed directly by the OS loader. The Java archive comparison is about the deployment model only; the Vayu format is independent.

The format specification must define:
- magic bytes and version;
- bounded section directory and entry point;
- function metadata and constant pool;
- string encoding and typed instructions or verified IR;
- optional source/debug metadata;
- deterministic serialization and runtime compatibility;
- explicit handling for unknown required features and unsupported versions.

The loader must validate the whole package before execution. Validate every offset and length using overflow-safe arithmetic; reject overlapping or out-of-range sections, invalid indices, unknown opcodes, invalid types, invalid branch targets, malformed signatures, invalid entry points, and unsupported feature flags. Bound allocation and execution resources. Never trust embedded lengths or execute a partially verified package.

Do not serialize raw pointers, process addresses, host-specific object layouts, or other process-local state.

## 5. Initial language subset

Implement the smallest useful subset in order:
1. constants, local variables, basic types, arithmetic, comparisons;
2. strings, UTF-8 policy, equality, concatenation, and safe bounds behavior;
3. functions, parameters, return values, scopes, and validated calls;
4. conditional branches and loops;
5. only then, selected collections and additional runtime operations.

For each operation define evaluation order, types, error behavior, runtime representation, and serialization. Unsupported constructs must fail at compile time or package validation with a source-aware diagnostic where possible.

## 6. Testing strategy

Each feature requires:
- positive tests for expected behavior;
- negative tests for unsupported or invalid operations;
- deterministic package-output tests;
- package round-trip tests;
- malformed/truncated/corrupt package tests;
- execution-result comparison against documented reference semantics;
- clean-checkout reproduction commands.

The loader/verifier boundary should receive fuzzing or property tests when practical. A malformed package must never become a host crash or success-shaped result.

## 7. Runtime and compatibility

The runtime contract must define value representation, strings and lifetimes, call frames, stack limits, error propagation, entry-point behavior, exit status, and supported runtime-version policy. Compatibility must be explicit. Unknown required features and unsupported versions must fail clearly; the loader must not guess.

The planned commands `vayuc build source.vyu -o output.vye` and `vayuc run output.vye` are illustrative until implemented. The existing compiler driver remains the user-facing interface.

## 8. Expansion after the first milestone

After a small verified .vye subset is stable:
- expand collections, exceptions, classes, closures, modules, and system APIs in small tested groups;
- add cache and incremental compilation only after deterministic output and dependency identity are defined;
- explore native code generation without requiring bundled LLD or an external compiler backend;
- investigate self-hosting only after a reliable bootstrap path exists;
- implement Vayu-native ONNX parsing and model execution later, using native fixtures instead of a Python generator.

## 9. Performance policy

Correctness comes first. Measure package size, build time, startup, steady-state execution, and memory separately. Any claim about beating JIT or native compilers requires reproducible, workload-specific evidence.

## 10. Definition of done

The first major Integrator milestone is complete when a documented subset lowers to a verified representation, the writer creates deterministic versioned .vye packages, Vayu/Integrator executes them without source recompilation, malformed packages are safely rejected, strings/functions/control flow have conformance coverage, and existing interpreter/VM workflows remain intact.

Use [Roadmap.md](../Roadmap.md) as the implementation order. No phase is complete merely because it is described here.
