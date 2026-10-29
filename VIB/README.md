# VIB — Vayu Integration Backend

VIB means **Vayu Integration Backend**. It is the integrated compiler and runtime system inside the Vayu repository and implements the **Integrator architecture**. It is not a separate language or required `vib.exe`; the existing Vayu compiler/driver remains the user-facing entry point.

## First objective

Build a small, correct Integrator before expanding to the full Vayu language. The first supported subset should grow in deliberate steps: basic values and arithmetic, strings, functions, and control flow. Vayu's existing interpreter/VM features are reference material, not proof of Integrator support.

## .vye Vayu Executable

Integrator is planned to emit a versioned `.vye` package. A `.vye` file is not a native OS executable. It requires a compatible Vayu/Integrator runtime, similar in broad deployment concept to a Java archive requiring a Java runtime.

The format needs a strict reader, deterministic writer, verifier, version policy, bounded sections, and malformed-input tests. Never execute a package before validation. Do not claim `.vye` support until the format and execution path are implemented and tested.

## Responsibilities

- connect the existing Vayu frontend to a small Integrator representation;
- define and verify the supported subset's semantics;
- write and load versioned `.vye` packages;
- execute packages through Vayu/Integrator;
- provide explicit diagnostics, tests, and runtime compatibility rules;
- expand language coverage only after each smaller subset passes conformance.

Native machine-code generation, cache, incremental compilation, adaptive execution, self-hosting, and Vayu-native ONNX support are later work. ONNX must not require a Python generator or Python runtime dependency.

## Engineering rules

1. Correctness and clear errors come before performance.
2. Every supported feature requires tests and capability reporting.
3. The .vye reader must validate all offsets, lengths, indices, opcodes, types, and branch targets before execution.
4. Malformed or unsupported packages must fail safely.
5. No bundled DLL folder, LLD executable, import-library bundle, foreign-language runtime, or external compiler backend is required by the initial Integrator.
6. Existing interpreter and VM routes must remain available during development.
7. Do not claim universal superiority over JITs or native compilers without reproducible evidence.

See [Roadmap.md](../Roadmap.md), [Integrator architecture](../docs/vib.md), and the [VIB technical reference](../docs/vib-documentation.md).
