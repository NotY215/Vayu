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
- The `.vye` executable-package track (design, writer, reader/runtime, dispatch adoption, and cache) is explicitly deferred under Phase 2A–2C. Do not treat it as the active prerequisite for the current VIR work.
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
| **Complete** | Implementation and the required tests/acceptance evidence are delivered. |
| **Next** | The next authorized implementation target. |
| **Recommended (needs authorization)** | A recommended task that changes a protected or separately owned subsystem; do not begin until explicitly authorized. |
| **Planned** | Accepted future work that is not yet the next active task. |
| **Deferred** | Intentionally postponed by project decision. |

## 4. Phase roadmap

| **Phase** | **Status** | **Goals** | **Output** |
| :--- | :--- | :--- | :--- |
| **0.1–0.4** | Complete | Baseline, feature matrix, curated fixtures. | Capability API + fixture list. |
| **1.1–1.4** | Complete | Integrator API expansion, adapter thinning, `--vib-status`, harness. | All delivered. |
| **2.1–2.6** | Complete | VIR structures, printer, verifier (structural), minimal lowering, integration. | `VIR Supported`, `Verifier Supported`. |
| **0.5** | **Recommended (needs authorization)** | **Fix pre-existing baseline failures:** `classes.vyu`, `collections.vyu` (interpreter), `exceptions.vyu`, `generators.vyu` (VM crash `0xC0000409`). Requires changes under `compiler/interp/**` and/or `compiler/vm/**`. | Baseline green; `run_all.ps1 -NoFixpoint` passes. |
| **3.1** | Complete | Arithmetic semantics contract in code. | `Vir.hpp` comment block. |
| **3.2** | Complete | VIR extended: `Add/Sub/Mul/Div/Mod/Neg/Cmp*/StoreLocal/LoadLocal`. | Extended opcode set. |
| **3.3** | Complete | Lowering: literals, arithmetic, unary, comparisons, simple `name = expr`. | Lowering context in `Vir.cpp`. |
| **3.4** | Complete | Verifier extended for new opcodes. | `regOK` + per-opcode range checks. |
| **3.5** | Complete | `printInstruction` extended. | Every opcode human-readable. |
| **3.6** | Complete | In-memory VIR interpreter. | `VIB/VirInterp.{hpp,cpp}`. |
| **3.7** | Complete | `Integrator::compile` = lower → verify → execute in-memory. | `--native` = Integrator-mediated execution. |
| **3.8** | Complete | `VirInterp.cpp` in `compiler/CMakeLists.txt`. | Build includes new file. |
| **3.9** | Complete | `vib_baseline.ps1` extended for Phase 3. | New assertions. |
| **4.1** | Next | UTF-8 policy, string semantics, index bounds. | `Feature::Strings = Supported` (pending). |
| **4.2** | Next | Lowering for string constants and operations. | String fixtures green. |
| **5.1** | Next | Functions, parameters, returns, scopes, calls, arity checks. | `Feature::Functions = Supported`; `--native` handles `print(...)` and user functions. |
| **5.2** | Planned | Recursion (stack-frame safety). | Recursion fixtures green. |
| **6.1–6.2** | Planned | Branches, loops, return/break/continue, verifier extension. | `ControlFlow = Supported`. |
| **7.1–7.2** | Planned | Collections (list → map/tuple/set). | `Collections = Supported`. |
| **8.1–8.2** | Planned | Modules and multi-file. | `Modules = Supported`. |
| **9.1–9.2** | Planned | Malformed-VIR suite + runtime-compat reporting. | Hardened verifier. |
| **10.1–10.2** | Planned | Exceptions, classes, closures, generators, pointers, standard library. | Each feature flips individually. |
| **2A–2C** | Deferred | `.vye` design → writer → reader/runtime; dispatch adoption; cache. | Deferred per project decision. |
| **11–14** | Planned | Measurement, native codegen, ONNX, release/self-host. | Unchanged from prior roadmap. |

### Phase 0.5 authorization boundary

Phase 0.5 is not part of the currently authorized VIR implementation sequence. It addresses failures that predate the Integrator work and may require edits to `compiler/interp/**` and/or `compiler/vm/**`. Keep it marked **Recommended (needs authorization)** until explicit approval is given. Do not modify those subsystems as an assumed part of Phases 4 onward.

### Phase 4 — Strings

Define string behaviour before implementing it: UTF-8 validity and encoding policy, representation, equality, concatenation, length, indexing, bounds behaviour, and error semantics. Add string constants and operations to VIR lowering, verification, printing, and interpretation. Keep behaviour consistent across all Integrator execution paths.

**Acceptance:** string fixtures cover empty strings, ASCII, multibyte UTF-8, concatenation, equality, length, valid indexing, out-of-range indexing, and invalid operations. Only then mark `Feature::Strings` supported.

### Phase 5 — Functions and calls

Implement function declarations, parameters, return values, local scopes, direct calls, and argument-count/type validation. The first subphase is 5.1. Recursion is deliberately separated into 5.2 because it requires safe call-frame and stack handling.

**Acceptance:** nested calls, parameter passing, return values, scope isolation, wrong-arity rejection, and call-stack safety have dedicated tests. Recursion remains unsupported until Phase 5.2 passes.

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

1. Continue with **Phase 4.1**: define and document the UTF-8/string/indexing contract.
2. Complete **Phase 4.2**: implement string constants and operations through lowering, verification, printing, and VIR interpretation.
3. Proceed to **Phase 5.1**: functions, parameters, returns, scopes, calls, and arity validation.
4. Keep **Phase 0.5** untouched unless separately authorized.
5. Keep **Phase 2A–2C** deferred.
6. Preserve the planned status of Phases 5.2–14 until their implementation work is actually undertaken and validated.

---

**Roadmap rule:** VIB support is established by implementation, verifier behaviour, and tests. Never infer Integrator capability from the broader historical feature set of the Vayu interpreter or VM.
