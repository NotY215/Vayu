# VIB and the Integrator Architecture

## Purpose

**VIB (Vayu Integration Backend)** is the integrated compiler/runtime system inside the Vayu repository. **Integrator** is the architecture it implements. Vayu remains the language, `.vyu` remains the source extension, and the existing Vayu compiler/driver remains the user-facing entry point. A separate `vib.exe` is not required.

The first goal is a small, correct Integrator, not immediate parity with every feature in Vayu. The first supported subset should grow incrementally through values, arithmetic, strings, functions, and control flow.

## Target architecture

```text
.vyu source
    |
    v
Vayu lexer / parser / semantic analysis
    |
    v
Integrator subset lowering
    |
    v
Verified internal representation
    |
    v
.vye writer and validation
    |
    v
.vye package
    |
    v
Compatible Vayu / Integrator runtime
    |
    v
Program output and exit status
```

This is the target design, not a claim that each component is implemented.

## .vye Vayu Executable format

A `.vye` file is a versioned Vayu Executable package. It is not a native OS executable and cannot be run directly by the operating-system loader. It must be executed by a compatible Vayu/Integrator runtime, similar in broad deployment concept to a Java archive requiring a Java runtime.

The format needs a defined header, version, bounded sections, entry point, constant pool, function metadata, instructions or verified IR, optional debug metadata, deterministic serialization, and strict validation. The reader must reject malformed offsets, overflow, invalid indices, unsupported required features, invalid control flow, and incompatible versions before executing anything.

The intended CLI is conceptually:

```text
vayuc build hello.vyu -o hello.vye
vayuc run hello.vye
```

These command spellings are proposals until implemented.

## Design principles

### One language and one user-facing toolchain
- Vayu owns language syntax, semantics, frontend, diagnostics, package identity, and runtime policy.
- VIB is implemented inside this repository.
- The Integrator does not introduce another source language or duplicate frontend.
- Existing interpreter/VM execution stays available as reference paths.

### Small subset first
- Support only constructs with defined semantics and passing tests.
- Existing Vayu feature coverage does not imply Integrator support.
- Unsupported constructs must fail clearly.
- Expand in small groups, with strings and functions among the initial goals.

### No bundled external backend
The first Integrator and .vye format do not require bundled DLLs, LLD, external compiler backends, or platform import-library bundles. Native machine-code generation is a later research/implementation track, not a prerequisite for the portable .vye runtime.

### Safe package loading
Validate all package structure and instructions before execution. Use overflow-safe arithmetic, bounded allocation, version checks, and malformed-file tests. Do not serialize raw pointers or host-specific object layouts.

### Later work
Caching, incremental compilation, native compilation, adaptive execution, self-hosting, and Vayu-native ONNX support follow only after the minimal Integrator and .vye format are stable.

See [Roadmap.md](../Roadmap.md) and the [VIB technical reference](vib-documentation.md).
