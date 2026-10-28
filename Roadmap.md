# Vayu Roadmap: Native Integrator and VIB

> **Purpose:** this is the canonical engineering roadmap for Vayu's transition from the legacy external-backend arrangement to Vayu's own integrated compiler/runtime architecture.
>
> **Status discipline:** a phase is complete only when its code, tests, documentation, and acceptance criteria are all satisfied. Roadmap entries are plans unless explicitly marked complete and backed by repository evidence.

## 1. Product and architecture decisions

- **Vayu is the language.** Source files remain `.vyu`.
- **VIB** means **Vayu Integration Backend**. The name is retained for the integrated toolchain system, not a separately installed backend product.
- **Integrator** is the architecture implemented inside VIB. It is not a separate executable or product.
- **`vayu.exe` / `vayuc.exe`** is the user-facing compiler/driver. The intended end state is one Vayu-owned toolchain entry point, not a required `vib.exe`.
- The VIB system owns the full path: frontend integration, verified intermediate representation, interpreter/VM integration, native code generation, runtime helpers, object/executable emission, external-linker integration where useful, artifact cache, incremental dependency tracking, and execution selection.
- No VCB repository, VCB executable, VCBIR format, VCB-specific environment variable, VCB-named lowering layer, or VCB-specific command is part of the target architecture.
- Reuse of standard platform toolchain components (for example, a platform linker) is allowed behind a Vayu-owned interface. Such tools must not be the compiler's language/backend implementation.
- Existing language features must not be declared supported by VIB merely because the interpreter or legacy native path supports them. Each VIB execution mode needs explicit conformance evidence.

## 2. Current baseline and transition rule

The repository has an established C++ frontend, AST, type checker, tree-walking interpreter, bytecode compiler, VM, language tooling, runtime integrations, examples, and tests. Historically, the native path delegated lowering and executable generation to an external backend. That historical implementation is **not** the target architecture.

The transition is deliberately staged. Removing an external backend before the replacement passes conformance would make native compilation unavailable. Therefore:

1. inventory and isolate the legacy path;
2. define VIB's contracts and tests;
3. implement Vayu-owned IR and verification;
4. implement native emission and linking;
5. prove parity;
6. switch the CLI to VIB;
7. only then remove remaining legacy implementation, fixtures, references, and packaging assumptions.

During the transition, documentation must state the actual state of the branch. Never claim that VCB has been fully removed from implementation until a source-tree search, build, and test run confirm it.

## 3. Architecture overview

Target pipeline:

```text
.vyu source
    |
    v
Vayu lexer + parser
    |
    v
AST + semantic/type analysis
    |
    v
Verified Vayu IR (VIR; owned and versioned by Vayu)
    |
    +--------------------+---------------------+
    |                    |                     |
    v                    v                     v
Interpreter bridge   Bytecode / VM       Native lowering
    |                    |                     |
    +--------------------+                     v
              Shared semantic rules       Native codegen
                                                |
                                    Object / executable emitter
                                                |
                                      Optional platform linker
                                                |
                                       Native artifact + metadata
```

The Integrator dispatcher chooses an execution route only when that route supports the program's required features and target. Correctness is the first constraint; speed is a selection criterion only after semantic parity.

## 4. Phase map

### Phase 0 — Baseline inventory and compatibility contract
**Status: required gate before implementation work**

- Record current build commands, supported platforms, CLI options, test categories, and current interpreter/VM/native behavior.
- Inventory every legacy-backend source file, format, command, environment variable, fixture, tool binary, documentation link, and test assumption.
- Capture golden output and exit codes for representative language tests.
- Establish a baseline report for build time, interpreter/VM time, native compile time where the existing path can be measured, memory, and executable size.
- Mark unsupported or flaky tests explicitly instead of silently omitting them.

**Exit criteria:** baseline artifacts committed; dependency inventory complete; no undocumented CLI or build dependency remains.

### Phase 1 — VIB foundation and contracts
**Status: planned**

- Create `VIB/` as the home for Vayu-owned Integrator implementation and its design notes.
- Define module boundaries: `frontend_bridge`, `ir`, `verify`, `vm_bridge`, `native`, `runtime`, `link`, `cache`, `incremental`, and `dispatch`.
- Define stable internal APIs, error types, target descriptions, diagnostics, artifact metadata, and feature-capability reporting.
- Keep the public driver entry point in Vayu; do not create a required separate VIB executable.
- Add architecture decision records for IR semantics, object formats, runtime ABI, cache identity, and dispatch policy.

**Exit criteria:** interfaces documented; skeleton builds; no claim of native-code support until native emission exists.

### Phase 2 — Shared semantic contract and conformance suite
**Status: planned**

- Specify evaluation order, integer/float behavior, truthiness, string encoding, collection behavior, exceptions, control flow, ownership, FFI, and process exit semantics.
- Create reusable conformance tests that can run against the interpreter, VM, and later native execution.
- Compare stdout, stderr, exit status, observable side effects, and failure modes.
- Add differential tests and minimized regression cases for every mismatch.
- Separate tests by feature capability so unsupported backend features are reported clearly.

**Exit criteria:** a repeatable suite defines the required behavior and reports per-mode coverage.

### Phase 3 — Vayu-owned IR (VIR)
**Status: planned**

- Design a documented, typed, versioned IR with explicit control-flow blocks and terminators.
- Represent values, types, calls, memory operations, branches, returns, runtime operations, and source locations.
- Add deterministic serialization for debugging and test fixtures; serialized IR is an internal development format, not a promised public ABI.
- Add a verifier for type consistency, definitions/uses, control-flow validity, call signatures, and target constraints.
- Implement AST-to-VIR lowering for the supported language subset.
- Reject unsupported constructs with source-located diagnostics rather than generating invalid code.

**Exit criteria:** verified IR for the initial subset; malformed IR is rejected; golden IR tests are stable.

### Phase 4 — Native code generation and executable emission
**Status: planned**

- Select a maintainable first target, initially x86-64 on Windows and/or Linux according to the tested toolchain.
- Define calling conventions, stack layout, register allocation strategy, relocations, symbol naming, data layout, and runtime ABI.
- Implement a small correct native subset first: constants, integer arithmetic, comparisons, branches, functions, calls, returns, and process output/exit.
- Emit relocatable objects or executable images through Vayu-owned code; use a platform linker behind a narrow interface where needed.
- Implement deterministic diagnostics and preserve source locations for native failures.
- Keep PE/COFF and ELF concerns target-specific behind shared interfaces.

**Exit criteria:** generated programs pass native conformance tests; output artifacts are validated by platform tools; no external VCB component is required.

### Phase 5 — Runtime and language-feature parity
**Status: planned**

Expand native support in measured slices:

1. primitive values and control flow;
2. functions, recursion, and stack safety;
3. strings and printing;
4. lists, maps, tuples, and sets;
5. structs and classes;
6. exceptions and cleanup semantics;
7. pointers, allocation, and unsafe operations;
8. C FFI, callbacks, and ABI layout;
9. standard runtime modules;
10. CPython bridge integration where supported.

Each slice requires tests for semantics, error behavior, resource lifetime, and parity with the interpreter/VM. Do not mark a feature complete based only on successful compilation.

**Exit criteria:** every claimed native feature has automated conformance coverage and documented platform limits.

### Phase 6 — Driver integration
**Status: planned**

- Route `--native` and `--native-out` through the VIB Integrator API.
- Keep `--run`, `--vm`, `--check`, token/AST/bytecode diagnostics, and existing language tooling stable.
- Replace legacy-specific IR and backend flags with VIB-owned diagnostics or documented compatibility errors.
- Ensure failure messages identify unsupported features, target failures, linker failures, and internal compiler errors separately.
- Keep output paths, exit codes, and temporary-file cleanup deterministic.

**Exit criteria:** CLI integration tests pass; normal interpreter/VM workflows remain unaffected; native mode works without the legacy executable.

### Phase 7 — Persistent artifact cache
**Status: planned**

- Cache verified IR, object files, and final artifacts where valid.
- Use content hashes over source, transitive dependencies, compiler version, IR schema, target triple, optimization settings, runtime ABI, and relevant toolchain identity.
- Write artifacts atomically; validate metadata before reuse.
- Add cache inspection, clear, disable, and diagnostic controls.
- Handle corrupt entries, interrupted writes, schema changes, and concurrent processes safely.

**Exit criteria:** cold/warm results are reproducible; corrupt or stale entries are rejected; cache state never changes program semantics.

### Phase 8 — Incremental compilation
**Status: planned**

- Track module imports and dependency edges.
- Rebuild only invalidated units when interface and dependency hashes allow it.
- Separate interface hashes from implementation hashes where semantics permit.
- Correctly invalidate dependants when public signatures, generic constraints, ABI, compiler options, or runtime contracts change.
- Add clean-build equivalence checks against incremental builds.

**Exit criteria:** incremental and clean outputs are semantically equivalent; dependency changes trigger the minimum safe rebuild set.

### Phase 9 — Unified execution dispatcher
**Status: planned**

- Support explicit modes first: interpreter, VM, and native.
- Add automatic selection only after each route has capability metadata and conformance coverage.
- Choose routes based on startup cost, expected work, target availability, compilation cost, cached artifacts, and required language features.
- Always provide a deterministic override for debugging and benchmarking.
- Fall back only when semantics are preserved and fallback is explicitly allowed; never silently change behavior.

**Exit criteria:** dispatch decisions are observable and reproducible; all modes pass shared conformance tests.

### Phase 10 — Profiling and adaptive compilation
**Status: research stage; no performance guarantee**

- Add opt-in profiling for hot functions/loops and execution frequency.
- Explore selective native compilation for hot code while keeping cold paths interpreted or in the VM.
- Require safe deoptimization/fallback design before any speculative optimization is enabled.
- Include profiling overhead, compilation cost, cache effects, and warm-up in benchmark reports.
- Compare against a clearly named baseline and representative workloads.

**Exit criteria:** adaptive mode demonstrates repeatable end-to-end gains on specified workloads without correctness regressions. If evidence does not support a gain, keep the feature experimental or remove it.

### Phase 11 — Optimization and release hardening
**Status: planned**

- Add optimization passes only with correctness tests and measured value.
- Track compile time, cold startup, warm startup, throughput, memory, artifact size, and incremental rebuild time.
- Fuzz parser/IR verifier boundaries and exercise malformed inputs.
- Test cache corruption, interrupted compilation, parallel builds, path quoting, unusual filenames, and linker failures.
- Verify clean checkout builds on supported platforms.
- Document platform, ABI, FFI, and runtime limitations.

**Exit criteria:** release gates pass in CI and results can be reproduced from documented commands.

### Phase 12 — Self-hosting investigation
**Status: future**

- Evaluate which compiler components can be expressed in Vayu after the native toolchain stabilizes.
- Bootstrap from the existing C++ compiler and compare outputs.
- Keep a reproducible bootstrap path and test that self-hosted builds can rebuild themselves.
- Do not make self-hosting a prerequisite for correctness of the initial VIB native backend.

**Exit criteria:** a documented bootstrap chain reproduces the compiler and passes conformance tests.

## 5. Benchmark and prediction policy

No speed or superiority claim is assumed. In particular, “better than JIT” is a research hypothesis, not a guaranteed outcome.

Every performance claim must specify:

- machine, OS, compiler revision, target, and configuration;
- exact benchmark source and input size;
- cold compile, warm cache, incremental build, startup, and steady-state results separately;
- warm-up policy, repetitions, median, spread, and outliers;
- peak memory and artifact size;
- comparison baseline and whether all tools use equivalent semantics;
- correctness result for the same workload.

Required workload families include short-lived CLI programs, arithmetic-heavy loops, recursion, string/collection-heavy work, multi-module projects, large dependency graphs, file processing, and long-running hot-loop workloads.

Keep predictions in `VIB/docs/BENCHMARK_PREDICTIONS.md` with hypothesis, mechanism, confidence, experiment, disconfirmation condition, measured result, and updated forecast.

## 6. Non-negotiable release gates

A release cannot claim the Integrator architecture is complete until:

1. VIB builds from a clean checkout without a VCB executable or VCB-specific environment variables.
2. Native output is generated by the Vayu-owned implementation and runs on each claimed target.
3. Interpreter, VM, and native conformance results are reported independently.
4. Clean and incremental builds produce equivalent behavior.
5. Cache corruption and invalidation tests pass.
6. Existing non-native CLI and tooling tests remain green.
7. Benchmarks are reproducible and do not mix compile time with runtime without labeling it.
8. Documentation and source-tree searches contain no active legacy-backend dependency.
9. Third-party linker/runtime tools are documented accurately with provenance and license obligations.
10. Unsupported features fail clearly rather than silently miscompile.

## 7. Legacy phase mapping

The earlier phase series (including language, tooling, GUI/graphics, packages, and the old backend-specific Phases 26–31) describes historical work. It is not a roadmap for continuing the old backend.

| Historical area | Treatment in the Integrator roadmap |
|---|---|
| Phases 1–18: language, compiler foundations, runtime, tooling, interoperability | Preserve existing implementation; revalidate with the Phase 0 baseline and shared conformance suite. |
| Phases 19–21: GUI and graphics | Preserve public APIs; add native conformance tests when VIB supports the required runtime/FFI features. |
| Phase 22: package ecosystem | Preserve the package manager; later connect dependency metadata and cache keys to VIB's incremental build graph. |
| Phase 23 and old Phase 26: single-binary/native-backend direction | Superseded as an architecture plan by VIB Phases 1–6. Historical achievements do not imply the new backend is implemented. |
| Old Phase 27: executable writers and validation | Reassess each requirement under VIB Phase 4; do not port backend-specific implementation blindly. |
| Old Phase 28: object emission and external linking | Reimplement behind VIB's target/link interfaces in Phase 4. Standard platform linkers may remain optional tools. |
| Old Phases 29–30: backend-specific policy/templates and lowering additions | Reassess security, runtime, and lowering requirements under Phases 3–5; discard backend-specific assumptions. |
| Old Phase 31: collection/string methods and bug fixes | Preserve the language feature goals, but implement and test them against the shared semantic contract and VIB native parity suite. |

## 8. Definition of done for the transition

The transition is done only when VIB owns the native compilation pipeline, the normal Vayu executable no longer resolves or invokes the legacy backend, no old backend-specific IR is required, tests pass from a clean checkout, and the source tree/docs/package layout have been audited. Until then, report the exact transition stage and do not label the removal complete.
