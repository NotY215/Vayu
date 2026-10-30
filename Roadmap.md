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
| **0.5b (remaining)** | Working | `classes.vyu` / `collections.vyu` are type-checker errors (`compiler/sema`); `generators.vyu --vm` still crashes `0xC0000409`; `exceptions.vyu` has `--run` vs `--vm` stdout parity drift. | Baseline green after authorized sema/interp/vm fixes. |
| **4.3** | Completed | String method calls via type tracking. | `Strings` Supported. |
| **5.1–5.2b** | Completed | Functions, calls, recursion, return plumbing. | `Functions` Supported. |
| **6.1** | Completed | if/elif/else, while, for-range, break/continue, `**`/`//`/`and`/`or`/`not`/string `*`. | `ControlFlow` Supported. |
| **6.2** | Completed | Branch-target verification for Jump/JumpIfFalse. | Invalid targets rejected. |
| **7.1** | Completed | Lists: literals, index get/set, `len(list)`, print. | `Collections` Supported (list). |
| **7.2** | Planned | map, tuple, set, slicing, ownership refinements. | Full collections. |
| **8.1–8.2** | Planned | Modules and multi-file programs. | `Modules` Supported. |
| **9.1–9.2** | Planned | Malformed-VIR suite, runtime-compatibility reporting. | Hardened verifier. |
| **10.1–10.2** | Planned | Exceptions, classes, closures, generators, pointers, stdlib surface. | Each feature flips individually. |
| **2A–2C** | Completed | `.vye` emit/run, `--output` alias, dispatch, cache. | Opt-in package path. |
| **11–14** | Planned | Measurement, native codegen, ONNX, self-host. | Unchanged. |

### Tooling notes

- `vayuc` with no arguments opens an interactive REPL (copyright banner; `--help`).
- `--output <path>` is an alias for `--emit-vye` and writes a `.vye` Vayu Executable (not a native OS PE; requires `vayuc --run-vye`).
- Windows builds can embed `assets/Vayu_logo.ico` into `vayuc.exe` and build `Vayu-IDE.exe` (uses `assets/IDE.ico`) for a lightweight `.vyu` editor with Run/Compile.

## 5–8

Validation, performance, licensing, and roadmap rules are unchanged: support is established by implementation, verifier behaviour, and tests.
