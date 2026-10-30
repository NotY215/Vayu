# Vayu Engineering Roadmap

**Language:** Vayu (`.vyu`)  
**Integrated compiler/runtime:** VIB (Vayu Integration Backend)  
**Architecture:** Integrator  
**Current internal representation:** Vayu Intermediate Representation (VIR)  
**Primary direction:** A progressively complete, verified compiler and native execution pipeline

This roadmap records the current implementation state and the intended sequence of work. A phase is complete only when its implementation, tests, and acceptance evidence exist. Statuses below reflect the supplied project baseline and must not be upgraded merely because a design, placeholder, or example exists.

## 1. Product contract

- Vayu remains the source language and the existing `vayu.exe` remains the user-facing entry point.
- VIB is the integrated compiler/runtime system inside the Vayu repository. Integrator is the architecture implemented by VIB, not a separate product or required executable.
- The current priority is to expand the Integrator and VIR into a correct execution pipeline, then progress toward native machine-code generation and the later roadmap milestones.
- The Integrator must not silently claim support for language features that are only available in the interpreter or VM.
- VIR must be structurally verified before interpretation or any later backend consumes it.
- The `.vye` executable-package track is maintained separately under Phase 2A–2C. The design is Completed; writer, reader/runtime, dispatch adoption, and cache tasks are Working. Do not treat package work as a prerequisite for unrelated VIR feature work.
- Existing interpreter/VM behaviour is useful reference material, but failures in those paths must be distinguished from Integrator/VIR failures.
- Do not reintroduce the removed external-backend design, bundled foreign-language runtimes, DLL directories, LLD executables, or platform import-library dependencies.
- ONNX remains a later Vayu-owned track. Do not add a Python ONNX generator or Python package dependency.

## 2. Current architecture

The active Integrator path is being built around this sequence:

```text
.vyu source
    |
    v
Vayu frontend (lexer / parser / semantic analysis)
    |
    v
Integrator lowering
    |
    v
VIR
    |
    v
VIR verifier
    |
    v
VIR interpreter / execution
    |
    v
Program output and exit status

Later roadmap work:
verified VIR -> native code generation -> native executable
```

The VIR interpreter is the current execution route for the implemented VIR subset. Native machine-code generation is a later milestone and must not be described as complete until it has real code generation and end-to-end tests.

## 3. Status definitions

| Status | Meaning |
| --- | --- |
| **Completed** | Implementation and the required tests/acceptance evidence are delivered. |
| **Working** | Actively being implemented or investigated. A blocked dependency remains Working when work is still active. |
| **Planned** | Accepted future work that has not started. |
| **Vacation** | Temporarily paused by project decision. |

## 4. Phase roadmap

| **Phase** | **Status** | **Goals** | **Output** |
| :--- | :--- | :--- | :--- |
| **0.1–4.2** | Completed | Baseline, capability API, VIR structures + printer + verifier, arithmetic lowering + interpreter, `--native` integration, string literals + concatenation + comparisons. | Delivered. |
| **0.5a** | Completed | Fix `run_all.ps1` `Invoke-Backend` regression. | Harness restored. |
| **0.5b (partial)** | Completed | Fix `exceptions.vyu` VM (`VMRuntimeError` catch). Provide `tests/diagnose_baseline.ps1`. | `exceptions.vyu --vm` green. |
| **0.5b (remaining)** | **Working — needs stderr** | Fix `classes.vyu` (`--run`), `collections.vyu` (`--run`), and `generators.vyu` (`--vm`, crash `0xC0000409`). Requires touching `compiler/interp/**` and/or `compiler/vm/**`. | Baseline green; `run_all.ps1 -NoFixpoint` passes fully. |
| **4.3** | Completed | String method calls via type tracking: `upper`, `lower`, `strip`, `contains`, `find`, `starts_with`, `ends_with`, `is_digit`, `is_alpha`, `is_space`, `replace`, `char_at`, `to_int`. | `Strings` note updated; methods dispatch through `CallBuiltin`. |
| **5.1** | Completed | Functions, calls, arity checks, recursion, `print`, `str`/`int`/`float`/`bool` conversions, `len(str)`. | `Functions Supported`. |
| **5.2b** | **Working** | Return-value plumbing through `Call`: `executeFunction` returns the `RtValue`, and `Call` pushes it to `destReg`. | Return values work end-to-end. |
| **6.1** | Planned | Conditional branches (`If`/`Elif`/`Else`), `while`/`for` loops, `return`/`break`/`continue`. | `ControlFlow Supported`. |
| **6.2** | Planned | Branch-target verification, unreachable-code diagnostics. | Invalid-branch tests reject. |
| **7.1–7.2** | Planned | Collections: `list` → `map`/`tuple`/`set`, indexing, slicing, ownership. | `Collections Supported`. |
| **8.1–8.2** | Planned | Modules and multi-file programs. | `Modules Supported`. |
| **9.1–9.2** | Planned | Malformed-VIR suite, runtime-compatibility reporting. | Hardened verifier. |
| **10.1–10.2** | Planned | Exceptions, classes, closures, higher-order functions, generators, pointers, standard-library surface. | Each feature flips individually. |
| **2A.1** | Completed | `.vye` design comment block in `Vir.hpp`. | Design captured. |
| **2A.2** | **Working** | Deterministic `.vye` writer, gated by `--emit-vye <path>`. | `--emit-vye` writes a package. |
| **2B.1** | **Working** | Strict `.vye` reader + verifier. | Reader passes malformed-package tests. |
| **2B.2** | **Working** | `vayu run <file>.vye` execution verb. | Round-trip + execution tests. |
| **2C.1** | **Working** | Dispatch adoption for Interpreter backend. | `run_all.ps1` green; dispatch unified. |
| **2C.2** | **Working** | Dispatch adoption for VM backend. | `run_all.ps1` green. |
| **2C.3** | **Working** | Content-addressed, atomic, recoverable artifact cache + incremental graph. | Cache + incremental equivalence. |
| **11–14** | Planned | Measurement, native codegen, ONNX, release/self-host. | Unchanged. |

### Phase 0.5 — Baseline repair

Phase 0.5 is tracked separately from the VIR feature sequence because it repairs pre-existing Interpreter/VM and harness regressions. Phase 0.5a is **Completed**. The exceptions VM catch fix and diagnostic script are **Completed** under 0.5b. The remaining classes, collections, and generators failures are **Working** and blocked pending stderr-based diagnosis. Changes may touch `compiler/interp/**` and/or `compiler/vm/**`; keep the work scoped to the listed failures and preserve regression coverage.

### Phase 4 — Strings

Phases 4.1–4.2 are **Completed**: the baseline string contract, literals, concatenation, and comparisons are implemented. Phase 4.3 is also **Completed**: type-tracked string method calls cover `upper`, `lower`, `strip`, `contains`, `find`, `starts_with`, `ends_with`, `is_digit`, `is_alpha`, `is_space`, `replace`, `char_at`, and `to_int`. These methods dispatch through `CallBuiltin`.

**Acceptance:** preserve tests for string literals, comparisons, method dispatch, empty strings, ASCII and multibyte text, and invalid operations.

### Phase 5 — Functions and calls

Phase 5.1 is **Completed** for function declarations/calls, arity checks, recursion, `print`, the `str`/`int`/`float`/`bool` conversions, and `len(str)`. Phase 5.2b is **Working**: `executeFunction` returns an `RtValue`, and the `Call` instruction pushes that result into `destReg`. Keep return-value plumbing marked Working until its complete regression suite passes.

**Acceptance:** preserve tests for nested calls, parameter passing, recursion, return values, scope isolation, wrong-arity rejection, and call-stack safety.

### Phase 6 — Control flow

Add conditional branches and loops, then return, break, and continue semantics. Extend VIR verification to validate branch targets, register/local usage, and structurally valid control-flow edges.

**Acceptance:** branch and loop fixtures pass, invalid targets are rejected, and control-flow support is backed by verifier and execution tests.

### Phase 7 — Collections

Expand collection support incrementally, beginning with lists and then maps, tuples, and sets. Specify representation, ownership/lifetime, indexing, mutation, equality, iteration, and error behaviour for each supported type.

**Acceptance:** each collection type has independent lowering, verifier, interpreter, and fixture coverage before the aggregate feature is marked supported.

### Phase 8 — Modules and multi-file programs

Define module identity, import resolution, symbol visibility, duplicate handling, and deterministic multi-file compilation. Keep dependency behaviour explicit and testable.

**Acceptance:** a multi-file Vayu program resolves imports, rejects missing or duplicate symbols, and executes with deterministic results.

### Phase 9 — Verifier hardening and runtime compatibility

Build malformed-VIR tests covering invalid opcodes, out-of-range registers/locals, invalid branch targets, malformed operands, invalid type/feature combinations, and runtime compatibility mismatches. Report unsupported features clearly and safely.

**Acceptance:** malformed VIR is rejected before execution, with stable diagnostics and regression coverage.

### Phase 10 — Broader language coverage

Expand support feature by feature: exceptions, classes, closures, generators, pointers, and standard-library operations. Each feature must be independently designed, implemented, tested, and reflected in the capability matrix. Existing interpreter/VM support is not proof of Integrator support.

**Acceptance:** every advertised feature has focused positive and negative tests and an explicit support-level transition.

### Phases 11–14 — Later milestones

These milestones remain planned and retain their prior scope:

- **Phase 11 — Measurement:** establish reproducible measurements for compilation, execution, startup, memory, and generated artifact size. Use measurements to guide optimization rather than assumptions.
- **Phase 12 — Native code generation:** generate native machine code from verified VIR, define target/ABI rules, and prove correctness with end-to-end tests for each supported target. Do not label native code generation complete while `--native` is only Integrator-mediated in-memory execution.
- **Phase 13 — Vayu-native ONNX and AI/ML:** implement a Vayu-owned ONNX reader/runtime, validate model structure and tensor metadata, and expand operator coverage through numerical correctness tests. No Python generator or Python runtime dependency.
- **Phase 14 — Release and self-host:** publish an accurate support matrix, test clean builds and installs, document compatibility and diagnostics, and consider self-hosting only when the compiler subset is stable.

## 5. Validation and completion rules

Every phase requires the relevant combination of:
- focused unit and integration tests;
- positive fixtures for supported behaviour;
- negative fixtures for unsupported or invalid behaviour;
- verifier and malformed-input tests where applicable;
- deterministic expected output and exit status;
- reproducible build/test commands;
- updated capability reporting and documentation.

A roadmap row, placeholder, sample program, or successful compilation alone is not implementation evidence. Do not mark a feature supported until the required tests demonstrate its semantics.

## 6. Performance policy

Performance claims must be based on reproducible measurements. Track compilation time, execution time, startup overhead, memory use, and generated artifact size separately. Comparisons with interpreters, JITs, or native compilers must use equivalent workloads and documented test conditions. Correctness and semantic coverage take priority over optimization.

## 7. Immediate implementation order

1. Continue **Phase 0.5b (remaining)** once stderr-based diagnosis is available of the classes, collections, and generators regressions. Keep changes limited to the affected Interpreter/VM paths and tests.
2. Continue and validate **Phase 5.2b** return-value plumbing through `Call`, including end-to-end regression tests.
3. Start **Phase 6.1** control-flow lowering and execution after the current Working items are resolved.
4. Keep Phase 6.2 and Phases 7–10 planned until implementation and acceptance tests are complete.
5. Continue the **2A–2C** `.vye` writer, reader, run verb, dispatch, and cache work, which remain Working.
6. Keep Phases 11–14 planned until their implementation and validation are delivered.

---

**Roadmap rule:** VIB support is established by implementation, verifier behaviour, and tests. Never infer Integrator capability from the broader historical feature set of the Vayu interpreter or VM.


## 8. Repository licensing

Vayu uses the **Vayu Proprietary License (All Rights Reserved)** in the repository's `LICENSE` file. It does not grant a general license to copy, modify, redistribute, or incorporate Vayu code into other projects. Separate written permission from the rights holder(s) is required, except for rights that apply under mandatory law or GitHub's platform terms.

A public GitHub repository is still publicly viewable, and GitHub's Terms of Service may permit platform-level viewing and forking. A proprietary license cannot technically prevent copying or make enforcement automatic. If the goal is to keep the source inaccessible to the public, the repository must also be made private. Third-party files may remain subject to their own licenses.
