# Vayu Engineering Roadmap

**Project:** Vayu programming language  
**Integrated toolchain:** VIB (Vayu Integration Backend)  
**Architecture:** Integrator  
**User-facing compiler:** the existing Vayu compiler/driver

This document is the single forward-looking implementation plan for building Vayu's own integrated compiler and execution system. It describes the intended work, ordering, dependencies, and acceptance criteria. A phase is complete only when its implementation, automated tests, documentation, and acceptance evidence are present.

## 1. Product and architecture contract

- Vayu remains the only source language; source files use `.vyu`.
- VIB means **Vayu Integration Backend**. It is the integrated toolchain system inside the Vayu repository, not a separate language or required standalone product.
- **Integrator** is the architecture implemented by VIB, not a separate executable or product.
- The existing Vayu compiler/driver remains the user-facing entry point. A separate `vib.exe` is not required.
- VIB is responsible for connecting the existing frontend to a Vayu-owned intermediate representation, verification, interpreter/VM integration, native code generation, runtime helpers, object and executable emission, optional platform-linker integration, artifact caching, incremental compilation, execution dispatch, diagnostics, and performance evidence.
- Reuse platform tools behind narrow Vayu-owned interfaces when appropriate. Platform tools must not replace Vayu's language frontend, IR, or native compiler implementation.
- Preserve the existing lexer, parser, AST, semantic analysis, interpreter, bytecode compiler, VM, package tooling, and editor workflows unless a phase explicitly changes them and includes compatibility tests.
- A feature is supported in a mode only when that mode implements it and passes the corresponding conformance tests.
- Correctness, diagnosability, and reproducibility take priority over performance.

## 2. Current starting point

The repository has an established Vayu frontend and interpreter/VM execution routes. The `VIB/` directory contains an initial Integrator API scaffold and documentation. The native compile entry currently reports that VIB native code generation is not implemented. Verified Vayu IR, native code emission, persistent caching, dependency-aware incremental compilation, and adaptive dispatch remain implementation work.

Until native implementation passes the relevant acceptance gates, use the supported interpreter or VM execution route. Do not describe a planned component as implemented merely because a placeholder, header, design, or CLI flag exists.

## 3. Target architecture

```text
Vayu source (.vyu)
       |
       v
Lexer -> Parser -> AST -> Semantic / Type Analysis
                              |
                              v
                    Frontend Integration
                              |
                              v
                    Verified Vayu IR (VIR)
                              |
              +---------------+----------------+
              |               |                |
              v               v                v
       Interpreter bridge  Bytecode / VM   Native lowering
              |               |                |
              +---------------+                v
                                  Native code generation
                                             |
                                             v
                               Object / executable emission
                                             |
                                Optional platform linker
                                             |
                                             v
                                  Native artifact + metadata

Shared services: runtime contract, diagnostics, target capabilities,
dependency graph, artifact cache, execution dispatcher, benchmarks.
```

The IR is the common compiler contract, not a promise that every execution route must immediately consume IR in the first milestone. Integration must proceed in small steps and retain working execution routes throughout.

## 4. Implementation phases

### Phase 0 — Establish the Integrator project baseline
**Goal:** make the starting state measurable and reviewable.

- Record clean build instructions, supported platforms, compiler configurations, and the current CLI surface.
- Inventory frontend, interpreter, VM, runtime, test, package, and tooling dependencies.
- Capture representative program outputs, diagnostics, exit codes, and side effects.
- Establish baseline compile time, startup time, interpreter/VM runtime, memory use, and artifact size where measurements are available.
- Mark flaky, unavailable, or platform-specific tests explicitly.
- Create a feature matrix for interpreter, VM, and planned native execution.

**Acceptance criteria**
- A clean checkout can follow the documented build procedure.
- Baseline commands and results are recorded.
- The feature matrix distinguishes implemented, partial, unsupported, and untested behavior.
- Existing interpreter and VM behavior has regression coverage.

### Phase 1 — VIB foundation and stable interfaces
**Goal:** create a buildable home for the Integrator implementation.

- Organize VIB responsibilities into clear modules: frontend bridge, IR, verifier, VM integration, native backend, runtime, targets, linking, cache, incremental compilation, and dispatch.
- Define common result/error types, diagnostics, target descriptions, capability reporting, and artifact metadata.
- Keep the public driver integrated with Vayu.
- Document component ownership and dependency direction.
- Add small compile-time/API tests for the VIB scaffold.
- Ensure unsupported operations fail explicitly rather than returning success-shaped placeholders.

**Acceptance criteria**
- VIB builds as part of the normal Vayu build.
- Module boundaries and API contracts are documented.
- Error paths identify unimplemented functionality clearly.
- Existing interpreter and VM commands remain unchanged and tested.

### Phase 2 — Shared language-semantics contract
**Goal:** define the observable behavior all execution modes must preserve.

- Specify evaluation order, scoping, integer and floating-point behavior, truthiness, strings and encoding, collection semantics, functions, recursion, exceptions, control flow, I/O, exit codes, and side effects.
- Document ownership and lifetime rules for runtime-managed values.
- Define FFI and ABI behavior separately from language-level semantics.
- Build reusable conformance fixtures that can run against the interpreter, VM, and later native execution.
- Compare stdout, stderr, exit status, observable side effects, and expected failures.
- Add regression fixtures for every discovered semantic mismatch.

**Acceptance criteria**
- Core semantics are documented and testable.
- Tests report results separately for each execution mode.
- Unsupported capabilities are reported rather than silently skipped.
- Every fixed mismatch gains a regression test.

### Phase 3 — Vayu Intermediate Representation (VIR)
**Goal:** introduce a deterministic, typed internal compiler representation owned by Vayu.

- Define value and type representations, functions, basic blocks, instructions, terminators, calls, branches, returns, and source locations.
- Represent runtime operations explicitly instead of embedding implicit frontend assumptions.
- Add an IR builder and AST-to-IR lowering for a deliberately small language subset.
- Define schema/versioning rules for internal serialized IR.
- Add deterministic textual dumps for debugging and test fixtures.
- Keep serialized IR an internal development interface until stability and compatibility requirements are explicitly established.
- Emit source-located errors for unsupported language constructs.

**Acceptance criteria**
- A documented subset lowers from AST to VIR.
- IR output is deterministic for equivalent inputs.
- Golden IR tests pass.
- Invalid or unsupported source constructs produce clear diagnostics.

### Phase 4 — IR verifier and invariants
**Goal:** reject invalid IR before interpretation or native code generation.

- Verify type consistency, definition/use rules, control-flow edges, terminators, function signatures, call arguments, return values, and target constraints.
- Validate constants, memory operations, runtime calls, and source metadata.
- Define verifier error codes and stable diagnostic formatting.
- Add malformed-IR tests, property-based tests, and fuzzing where practical.
- Require verification at internal trust boundaries, including deserialization if that becomes supported.

**Acceptance criteria**
- Valid IR from Phase 3 passes verification.
- Each malformed fixture is rejected with a deterministic diagnostic.
- Verifier failures do not crash the compiler or produce output artifacts.
- Native code generation cannot consume unverified IR.

### Phase 5 — Runtime contract and VIR execution bridge
**Goal:** align the new IR with existing execution semantics before optimizing native code.

- Define shared runtime operations for strings, collections, allocation, errors, I/O, and other runtime-managed behavior.
- Decide which operations can be implemented directly in the VM and which require runtime helpers.
- Add an IR evaluator or bridge only where it helps validate the IR contract; avoid maintaining duplicate semantic implementations without a clear reason.
- Compare the IR execution route with existing interpreter and VM behavior.
- Document memory ownership, error propagation, and cleanup rules.

**Acceptance criteria**
- The selected VIR subset can be executed or validated through a documented route.
- Observable behavior matches the semantic contract.
- Runtime errors and cleanup paths have tests.
- The runtime ABI is documented before native code depends on it.

### Phase 6 — Native backend: initial machine-code subset
**Goal:** generate correct native code for a small, testable subset.

- Choose an initial target and ABI based on a working, reproducible toolchain.
- Define target data layout, calling convention, symbol naming, stack layout, relocations, and runtime-call conventions.
- Implement constants, local values, integer arithmetic, comparisons, branches, functions, calls, and returns.
- Add register/stack management appropriate to the first target.
- Reject unsupported operations before artifact emission.
- Keep target-specific details behind VIB interfaces.
- Preserve source locations in diagnostics and, where practical, generated debug metadata.

**Acceptance criteria**
- Native output is generated by Vayu-owned implementation code.
- A minimal native suite passes on the selected target.
- Generated programs have correct exit status and observable output.
- Unsupported constructs fail clearly and do not create misleading output artifacts.

### Phase 7 — Object and executable emission
**Goal:** turn native code into validated runnable artifacts.

- Implement or integrate target-specific object emission behind VIB-owned interfaces.
- Support symbols, sections, relocations, alignment, constants, and runtime references required by the implemented subset.
- Define when a platform linker is invoked and how it is located, configured, and diagnosed.
- Keep Windows PE/COFF and Linux ELF handling isolated behind target abstractions.
- Write output files atomically and clean temporary files after failure.
- Validate produced artifacts using platform tools in tests where available.
- Record target, compiler version, ABI, and relevant runtime metadata with artifacts.

**Acceptance criteria**
- Native executable output is reproducible for the same inputs and configuration where deterministic output is intended.
- The artifact runs on each explicitly supported target.
- Linker absence, failure, and invalid output have actionable diagnostics.
- A clean checkout does not require an undocumented executable or environment variable.

### Phase 8 — Native runtime and language-feature expansion
**Goal:** grow support feature-by-feature without losing semantic parity.

Implement in staged groups:
1. primitive values and control flow;
2. functions, recursion, and stack behavior;
3. strings, formatting, and printing;
4. lists, tuples, sets, maps, and collection operations;
5. structs, classes, and method dispatch;
6. exceptions and cleanup;
7. memory operations, pointers, and unsafe features where defined;
8. standard-library operations and file/process interaction;
9. Vayu-owned runtime helpers and documented platform ABI boundaries, only where explicitly implemented and tested.

For each group:
- define its capability boundary;
- add positive, negative, and edge-case tests;
- test resource lifetime and failure behavior;
- compare results with interpreter and VM;
- document platform-specific restrictions.

**Acceptance criteria**
- Each feature is marked native-supported only after its conformance suite passes.
- Unsupported features fail before execution or linking where possible.
- Runtime and FFI boundaries have dedicated tests.
- Feature coverage is reported by target and execution mode.

### Phase 9 — Compiler driver integration
**Goal:** expose VIB through the existing Vayu compiler experience.

- Route native compilation and native-output requests through the VIB API.
- Keep interpreter, VM, type-check-only, token/AST/bytecode dumps, and package workflows stable.
- Give each mode explicit capability reporting and deterministic diagnostics.
- Distinguish source errors, unsupported features, verifier errors, code-generation errors, linker errors, and internal failures.
- Define output-path behavior, exit codes, temporary files, and cancellation/error cleanup.
- Remove obsolete flags only with clear diagnostics and documentation.

**Acceptance criteria**
- CLI integration tests pass for supported modes.
- Existing non-native workflows remain green.
- Native execution and native artifact generation use VIB only.
- No command reports success when requested output was not produced.

### Phase 10 — Persistent artifact cache
**Goal:** reuse valid compilation work safely.

- Cache verified IR, object files, and final artifacts when appropriate.
- Compute content keys from source, transitive dependencies, compiler build/version, IR schema, target, optimization settings, runtime ABI, and all toolchain inputs that can affect output.
- Store metadata with every artifact and validate it before reuse.
- Use atomic writes and concurrency-safe publication.
- Provide cache disable, clear, inspect, and diagnostic controls.
- Handle interrupted writes, corrupt entries, incompatible schemas, and stale artifacts.
- Never let cache state change observable program semantics.

**Acceptance criteria**
- Cold and warm builds are measured separately.
- Cache hits and misses are explainable through diagnostics.
- Corrupt, stale, or incompatible entries are rejected safely.
- Cached and uncached builds pass equivalent correctness tests.

### Phase 11 — Dependency-aware incremental compilation
**Goal:** rebuild only what safely needs rebuilding.

- Track modules, imports, generated interfaces, and dependency edges.
- Separate public-interface fingerprints from implementation fingerprints where language semantics permit.
- Invalidate dependants when signatures, generic constraints, ABI, target, compiler options, runtime contracts, or relevant dependencies change.
- Support reliable recovery from deleted or renamed modules.
- Record why each unit was rebuilt or reused.
- Compare incremental builds against clean builds in CI.

**Acceptance criteria**
- Incremental and clean builds produce equivalent behavior and compatible artifacts.
- Dependency changes invalidate all affected units.
- Unrelated changes avoid unnecessary rebuilds when safe.
- Missing or corrupt dependency metadata triggers a safe rebuild.

### Phase 12 — Explicit execution modes and dispatcher
**Goal:** make execution choice understandable and deterministic.

- Support explicit interpreter, VM, and native modes independently.
- Attach capability metadata to each route and target.
- Add automatic route selection only after conformance and cost models exist.
- Consider startup cost, expected workload, compile cost, cache state, target availability, and required features.
- Provide a deterministic mode override for debugging and benchmarking.
- Permit fallback only when explicitly enabled and semantics are preserved.
- Log or expose why a route was selected.

**Acceptance criteria**
- Every selected route supports the program's required features.
- Dispatch decisions can be reproduced from recorded inputs.
- Automatic selection never silently changes program behavior.
- Explicit execution modes remain available.

### Phase 13 — Optimization and code-quality improvements
**Goal:** improve performance based on measurements.

- Introduce optimization passes incrementally: constant folding, dead-code elimination, control-flow simplification, inlining where safe, and target-specific improvements.
- Validate each pass with IR invariants and differential tests.
- Track compile-time cost as well as runtime benefit.
- Keep optimization levels explicit and reproducible.
- Use representative programs rather than optimizing only synthetic arithmetic loops.
- Remove or disable passes that add complexity without measurable benefit.

**Acceptance criteria**
- Each optimization preserves conformance.
- Performance reports include compile time, execution time, memory, and artifact size.
- Regressions are detected by automated benchmarks or release review.
- Default optimization settings are based on evidence.

### Phase 14 — Profiling and adaptive compilation research
**Goal:** evaluate hybrid execution without assuming it will outperform alternatives.

- Add opt-in profiling for execution counts and hot functions/loops.
- Explore selective native compilation for frequently executed regions while retaining interpreted or VM execution for cold code.
- Define code-version, runtime-state, and fallback semantics before enabling adaptive behavior.
- Include profiler overhead, compilation cost, warm-up, cache hits, and deoptimization costs in evaluation.
- Compare against the same program, inputs, and correctness requirements in each baseline.
- Keep adaptive mode experimental until it demonstrates repeatable end-to-end benefit.

**Acceptance criteria**
- Correctness is preserved across interpreted, VM, and compiled regions.
- Any claimed speedup is reproducible on named workloads.
- Workloads that do not benefit can remain on simpler execution routes.
- No universal superiority claim is made without broad, independently repeatable evidence.

### Phase 15 — Reliability, security boundaries, and release hardening
**Goal:** make the integrated toolchain reliable for real projects.

- Test malformed source, malformed IR, invalid artifacts, and hostile cache contents.
- Test parallel builds, interrupted compilation, file permissions, path quoting, unusual filenames, and linker errors.
- Fuzz parser and verifier boundaries where practical.
- Verify temporary-file cleanup and atomic output replacement.
- Test clean builds on every claimed platform and compiler configuration.
- Review runtime memory safety and FFI boundaries.
- Document supported targets, ABI constraints, limitations, and reproducible build steps.

**Acceptance criteria**
- Release CI passes on supported configurations.
- Failures are diagnosable and do not leave success-shaped partial artifacts.
- Cache, incremental, and parallel-build tests pass.
- Published compatibility and security statements match tested behavior.

### Phase 16 — Self-hosting investigation
**Goal:** determine whether Vayu can eventually build parts or all of its own compiler.

- Identify components suitable for implementation in Vayu after the native subset is stable.
- Keep a bootstrap compiler and documented bootstrap process.
- Compare generated artifacts and run the same conformance suite.
- Test repeatable rebuilds and bootstrap recovery.
- Treat self-hosting as a separate milestone, not a prerequisite for the initial native compiler.

**Acceptance criteria**
- The bootstrap sequence is documented and repeatable.
- Self-hosted builds pass conformance tests.
- A reliable recovery path remains available.

## 5. Benchmark plan and prediction rules

Performance is an empirical question. A design goal is not a benchmark result, and “better than JIT” is a hypothesis rather than a guarantee.

### Required measurements

- Clean cold compilation time.
- Warm-cache build time.
- Incremental build time for implementation-only and interface changes.
- Process startup and time-to-first-output.
- Steady-state execution time and throughput.
- Peak memory usage.
- Native artifact size.
- Cache hit/miss rate and cache recovery cost.
- Correctness and output equivalence.

### Required workload families

- Short-lived command-line programs.
- Integer and floating-point arithmetic.
- Branch-heavy and function-call-heavy programs.
- Recursion.
- String and collection processing.
- Multi-module projects and large dependency graphs.
- File-processing workloads.
- Long-running workloads with hot loops.
- Programs that compile quickly but run briefly, and programs with high runtime work.

### Experiment protocol

Every benchmark report must include hardware, OS, compiler revision, target, flags, source/input, number of repetitions, warm-up policy, median and spread, and the exact command used. Report compile time separately from runtime. Use the same semantics and equivalent work for comparisons. Repeat measurements under controlled conditions and preserve raw results.

Maintain predictions in `VIB/docs/BENCHMARK_PREDICTIONS.md`. Each entry should include:
- hypothesis and proposed mechanism;
- baseline and expected direction of change;
- confidence and assumptions;
- benchmark and reproduction command;
- result that would disprove the hypothesis;
- observed data and updated prediction.

## 6. Milestone gates

Do not advance by renaming a phase or creating placeholder files. Each milestone must pass its acceptance criteria.

1. **Foundation gate:** clean build, baseline results, and existing interpreter/VM regression tests.
2. **IR gate:** deterministic lowering, verifier coverage, and invalid-input rejection.
3. **Native gate:** generated native artifact passes conformance on the named target.
4. **Parity gate:** every advertised native feature has mode-specific conformance coverage.
5. **Cache gate:** warm/cold tests, corruption recovery, and semantic equivalence.
6. **Incremental gate:** dependency invalidation and clean-build equivalence.
7. **Dispatcher gate:** explicit overrides, capability-aware decisions, and deterministic behavior.
8. **Performance gate:** reproducible workload-specific gains without correctness regressions.
9. **Release gate:** clean-checkout builds, platform matrix, documentation review, and full CI success.

## 7. Definition of completion

The Integrator architecture is ready for a production release only when:

- VIB owns the integrated compilation pipeline and native code generation.
- The user-facing Vayu compiler can produce and run native artifacts on every claimed target.
- IR verification, runtime contracts, and execution-mode conformance tests are in place.
- Existing interpreter and VM workflows remain supported.
- Cache and incremental compilation are safe, observable, and tested.
- Dispatcher decisions are capability-aware and deterministic.
- Benchmarks are reproducible and describe measured workloads accurately.
- Build, runtime, target, ABI, and third-party tool dependencies are documented.
- Unsupported language features fail clearly rather than silently producing incorrect code.
- The clean-checkout build and release test matrix pass.

**Rule:** implementation status is determined by code and test evidence, not by the existence of a roadmap entry, interface, placeholder, or design document.
