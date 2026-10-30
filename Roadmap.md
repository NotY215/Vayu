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
- The `.vye` executable-package track is maintained under Phase 2A–2C and is **Completed** as opt-in infrastructure (`--emit-vye`, `--run-vye`, `--output`, `--dispatch=integrator`, `--enable-cache`). Do not treat package work as a prerequisite for unrelated VIR feature work.
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
| **0.5b (remaining)** | Completed | TypeChecker current-scope assign + for-loop scoped vars (classes/collections). Exceptions VM catch earlier. Residual: optional stdout parity polish / lambdas VM if still red. | Type errors fixed; baseline type-check green. |
| **4.3** | Completed | String method calls via type tracking: `upper`, `lower`, `strip`, `contains`, `find`, `starts_with`, `ends_with`, `is_digit`, `is_alpha`, `is_space`, `replace`, `char_at`, `to_int`. | `Strings` note updated; methods dispatch through `CallBuiltin`. |
| **5.1** | Completed | Functions, calls, arity checks, recursion, `print`, `str`/`int`/`float`/`bool` conversions, `len(str)`. | `Functions Supported`. |
| **5.2b** | Completed | Return-value plumbing through `Call`: `executeFunction` returns the `RtValue`, and `Call` pushes it to `destReg`. | Return values work end-to-end. |
| **6.1** | Completed | Conditional branches (`If`/`Elif`/`Else`), `while`, `for x in range(N)`, `break`/`continue`, plus `**`/`//`/`and`/`or`/`not`/string `*`. | `ControlFlow Supported`. |
| **6.2** | Completed | Branch-target verification for Jump/JumpIfFalse instruction indices. | Invalid targets rejected. |
| **7.1–7.2** | Completed | list/map/tuple/set literals, index get/set, `len()`. Slicing refinements may continue. | `Collections Supported`. |
| **8.1–8.2** | Completed | `import` / `from import` load `.vyu`/`.vayu` and merge functions into VIR module (flat). | `Modules Supported`. |
| **9.1–9.2** | Completed | Verifier covers Jump targets, all arith/logic/collection opcodes, empty module, Call arity; runtime-compat reporting remains light. | Hardened verifier. |
| **10.1–10.2** | Planned | Exceptions, classes, closures, higher-order functions, generators, pointers, standard-library surface. | Each feature flips individually. |
| **2A.1** | Completed | `.vye` design comment block in `Vir.hpp`. | Design captured. |
| **2A.2** | Completed | Deterministic `.vye` writer, gated by `--emit-vye <path>`. | `--emit-vye` writes a package. |
| **2B.1** | Completed | Strict `.vye` reader + verifier. | Reader passes malformed-package tests. |
| **2B.2** | Completed | `--run-vye <path>` execution verb. | Round-trip + execution tests. |
| **2C.1** | Completed | Dispatch adoption via `--dispatch=integrator`. | Opt-in Integrator dispatch. |
| **2C.2** | Completed | Dispatch adoption for supported VIR subset. | Opt-in Integrator dispatch. |
| **2C.3** | Completed | Content-addressed artifact cache (`--enable-cache`). | Cache populates on `--native`/`--emit-vye`. |
| **11–14** | Planned | Measurement, native codegen, ONNX, release/self-host. | Unchanged. |

### Phase 0.5 — Baseline repair

Phase 0.5 is tracked separately from the VIR feature sequence because it repairs pre-existing Interpreter/VM and harness regressions. Phase 0.5a is **Completed**. The exceptions VM catch fix and diagnostic script are **Completed** under 0.5b. TypeChecker assignment/for-scope fixes are **Completed** under 0.5b remaining. Optional residual: exceptions stdout parity between `--run` and `--vm`, and any remaining `lambdas.vyu` VM failures.

### Phase 4 — Strings

Phases 4.1–4.2 are **Completed**: the baseline string contract, literals, concatenation, and comparisons are implemented. Phase 4.3 is also **Completed**: type-tracked string method calls cover `upper`, `lower`, `strip`, `contains`, `find`, `starts_with`, `ends_with`, `is_digit`, `is_alpha`, `is_space`, `replace`, `char_at`, and `to_int`. These methods dispatch through `CallBuiltin`.

**Acceptance:** preserve tests for string literals, comparisons, method dispatch, empty strings, ASCII and multibyte text, and invalid operations.

### Phase 5 — Functions and calls

Phase 5.1 and **5.2b are Completed**: function declarations/calls, arity checks, recursion, `print`, conversions, `len(str)`, and return-value plumbing through `Call`.

**Acceptance:** preserve tests for nested calls, parameter passing, recursion, return values, scope isolation, wrong-arity rejection, and call-stack safety.

### Phase 6 — Control flow

Phase **6.1 is Completed**: if/elif/else, while, `for x in range(N)`, break, continue, plus `**`, `//`, `and`/`or`/`not`, and string `*`. Phase **6.2 is Completed**: Jump/JumpIfFalse target verification.

**Acceptance:** branch and loop fixtures pass, invalid targets are rejected, and control-flow support is backed by verifier and execution tests.

### Phase 7 — Collections

Phases **7.1–7.2 are Completed** for list, map, tuple, and set literals with indexing and `len()`. Slicing and ownership refinements may continue under later work without demoting the feature row.

**Acceptance:** each collection type has independent lowering, verifier, interpreter, and fixture coverage before the aggregate feature is marked supported.

### Phase 8 — Modules and multi-file programs

Phases **8.1–8.2 are Completed** for Integrator: `import` / `from import` resolve `.vyu`/`.vayu` beside the source directory and merge top-level functions into the VIR module (flat merge). Runtime module objects and attribute paths remain available on the interpreter/VM routes.

**Acceptance:** a multi-file Vayu program resolves imports, rejects missing modules, and executes merged functions with deterministic results on the Integrator path.

### Phase 9 — Verifier hardening and runtime compatibility

Phases **9.1–9.2 are Completed** for structural verifier coverage: Jump/JumpIfFalse targets, empty modules, Call/CallBuiltin arity, and register bounds for arithmetic, logic, string, and collection opcodes (List/Map/Tuple/Set). Dedicated malformed-VIR fixture files may still be expanded later without demoting the phase.

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

1. Optional residual **0.5b** stdout parity / lambdas VM when evidence is pasted.
2. Start **Phase 10** feature-by-feature (exceptions/classes in VIB) when authorized.
3. Keep Phases 11–14 planned until implementation and acceptance tests are complete.

### Tooling notes

- `vayuc` with no arguments opens an interactive REPL (copyright banner; `--help`).
- `--output <path>` is an alias for `--emit-vye` and writes a `.vye` Vayu Executable (not a native OS PE; requires `vayuc --run-vye`).
- Windows builds embed `assets/Vayu_logo.ico` into `vayuc.exe` and build `Vayu-IDE.exe` (dark theme, OUTPUT pane, `assets/IDE.ico`) for a lightweight `.vyu` editor with Run (F5) and Compile (F7).

---

**Roadmap rule:** VIB support is established by implementation, verifier behaviour, and tests. Never infer Integrator capability from the broader historical feature set of the Vayu interpreter or VM.


## 8. Repository licensing

Vayu uses the **Vayu Proprietary License (All Rights Reserved)** in the repository's `LICENSE` file. It does not grant a general license to copy, modify, redistribute, or incorporate Vayu code into other projects. Separate written permission from the rights holder(s) is required, except for rights that apply under mandatory law or GitHub's platform terms.

A public GitHub repository is still publicly viewable, and GitHub's Terms of Service may permit platform-level viewing and forking. A proprietary license cannot technically prevent copying or make enforcement automatic. If the goal is to keep the source inaccessible to the public, the repository must also be made private. Third-party files may remain subject to their own licenses.
