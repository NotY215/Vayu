# VIB Technical Documentation
## Architecture, internal components, data flow, and engineering analysis

**VIB** means **Vayu Integration Backend**. It is the integrated compiler and execution-toolchain system inside the Vayu repository. **Integrator** is the architecture VIB implements. Vayu remains the language, .vyu remains the source extension, and the existing Vayu compiler driver remains the user-facing entry point. VIB does not require a separate vib.exe.

This document distinguishes the intended architecture from implemented functionality. A design, placeholder, header, or CLI option is not evidence that a component is complete. Implementation status must be established through code, tests, and acceptance evidence.

## Contents
1. Purpose and system boundaries
2. Architecture overview
3. End-to-end compilation lifecycle
4. Component responsibilities
5. Vayu IR design
6. Verification and trust boundaries
7. Shared language semantics
8. Interpreter, VM, and native execution
9. Native code generation
10. Runtime, ABI, and memory model
11. Targets, object files, and linking
12. Artifact identity and cache
13. Incremental compilation
14. Execution dispatch and adaptive compilation
15. Diagnostics and observability
16. Correctness and test strategy
17. Performance analysis
18. Security and reliability
19. Failure modes and recovery
20. Implementation sequence
21. Repository layout
22. Architecture decisions
23. Definition of done

---

## 1. Purpose and system boundaries

VIB integrates the existing Vayu frontend and execution facilities with a Vayu-owned compiler representation and, in planned stages, a native compilation pipeline.

VIB is responsible for:
- integrating the existing lexer, parser, AST, name resolution, and type checking;
- lowering supported source constructs into a typed, versioned intermediate representation called VIR;
- verifying VIR before trusted consumers use it;
- connecting interpreter and VM execution to shared language semantics;
- lowering verified VIR into target-specific machine instructions;
- handling instruction selection, registers, stack frames, calls, control flow, symbols, relocations, and object emission;
- coordinating runtime helpers and ABI contracts;
- invoking an optional platform linker through a narrow adapter;
- maintaining artifact metadata, persistent cache entries, and dependency graphs;
- dispatching execution only to supported routes;
- providing diagnostics, conformance tests, and reproducible performance evidence.

### Vayu owns
Vayu owns language syntax and semantics, frontend processing, diagnostics, VIR, verification rules, lowering decisions, native code generation, runtime contracts, artifact identity, cache validity, incremental invalidation, and dispatch policy.

### Platform tools may provide
Platform tools may provide final linking, operating-system interfaces, system libraries, SDK files, and debugging utilities. They remain dependencies behind documented interfaces. They must not replace Vayu's language frontend, IR definition, or code generator.

### Explicit non-goals
- requiring a separate VIB executable;
- creating a second language frontend;
- silently changing execution modes when a requested mode fails;
- claiming unsupported language features work in native mode;
- claiming ahead-of-time compilation universally beats JIT compilers;
- treating placeholder artifacts as successful output.

## 2. Architecture overview

The intended source-to-execution flow is:

1. Source file and compiler options enter the existing Vayu driver.
2. The lexer and parser produce an AST.
3. Semantic analysis resolves names, types, scopes, and source errors.
4. The VIB frontend bridge adapts semantic results into VIR.
5. The VIR verifier checks structural and type invariants.
6. The selected execution route consumes supported input: interpreter bridge, VM route, or native lowering.
7. Native lowering selects instructions, allocates registers and stack slots, and prepares symbols and relocations.
8. The emitter creates an object or executable artifact.
9. An optional linker adapter produces the final linked executable when needed.
10. The driver validates the output and records metadata.
11. The cache stores only validated artifacts under a complete content identity.

Shared services include the runtime contract, target capabilities, diagnostics, dependency graph, artifact cache, dispatch policy, and conformance tests.

### Architectural layers
- **Source layer:** .vyu files and compiler options.
- **Frontend layer:** parsing, semantic analysis, source locations, diagnostics.
- **Representation layer:** VIR construction, canonical form, verification.
- **Execution layer:** interpreter and VM integration, native lowering.
- **Target layer:** instruction selection, ABI rules, object formats, relocation handling.
- **Runtime layer:** value representation, allocation, errors, I/O, runtime calls.
- **Build layer:** artifact identity, caching, dependency tracking, incremental invalidation.
- **Control layer:** CLI integration, route selection, diagnostics, tests, and benchmarks.

Each layer should depend on explicit contracts below it. The backend should not need to parse source again, and the frontend should not depend on target-specific machine-code encodings.

## 3. End-to-end compilation lifecycle

### 3.1 Input and option validation
The driver validates source paths, targets, execution modes, optimization settings, output paths, and incompatible options. Invalid combinations should fail before compilation with useful diagnostics.

### 3.2 Frontend processing
The existing frontend produces the AST and semantic information. VIB adapts these results rather than duplicating parsing or language rules.

### 3.3 VIR lowering
Only constructs with defined VIR semantics may be lowered. Source locations and type information should survive. Unsupported constructs must produce an explicit diagnostic rather than a partial success.

### 3.4 Verification
The verifier checks the resulting representation. Native code generation must never consume unverified VIR. If serialized VIR is loaded, deserialization is a trust boundary and verification must run again.

### 3.5 Route selection
Explicit execution modes must either run through the requested supported route or fail clearly. Automatic selection is later work and must account for feature support, target, runtime needs, compilation cost, cache state, and deterministic policy.

### 3.6 Native lowering and emission
Verified VIR is transformed into target-specific operations. The backend handles control flow, calls, register and stack usage, relocations, and object format details. Unsupported features must stop emission.

### 3.7 Linking and artifact validation
The optional linker adapter discovers the configured tool, captures diagnostics and exit status, and verifies the expected output. A successful process exit alone is not enough; the artifact must exist and pass structural or smoke validation.

### 3.8 Metadata and cache commit
Record compiler revision, target, VIR schema, relevant options, runtime ABI, dependencies, and external tool identity where relevant. Cache writes should use a temporary location and atomic commit. Failed builds must not leave a valid-looking cache entry.

## 4. Component responsibilities

| Component | Responsibility | Must not do |
|---|---|---|
| Driver | CLI, route selection, output paths, top-level diagnostics | Define language semantics or encode instructions |
| Frontend bridge | Adapt AST and semantic results to VIB | Duplicate parser or type checker |
| VIR builder | Construct typed IR and source metadata | Encode target machine bytes |
| Verifier | Check IR invariants and target-independent validity | Silently repair malformed IR |
| VM/interpreter bridge | Integrate execution with shared semantics | Redefine behavior independently |
| Native lowering | Map VIR operations to target operations | Parse source or guess semantics |
| Register allocator | Track live values, allocate registers, spill and reload | Change observable program behavior |
| Emitter | Encode instructions, sections, symbols, relocations | Choose language semantics |
| Runtime | Implement documented helper and ABI contracts | Hide backend-specific behavior |
| Target description | Describe architecture, ABI, formats, capabilities | Own CLI policy |
| Linker adapter | Discover and invoke platform linker | Replace Vayu's code generator |
| Cache | Validate artifact identity and store atomically | Guess undeclared dependencies |
| Incremental engine | Track dependencies and invalidation | Differ from clean-build results |
| Dispatcher | Choose supported execution route and explain why | Silently run unsupported code |
| Conformance suite | Compare route behavior | Mark untested features supported |

## 5. Vayu IR design

### Purpose
VIR is the internal contract between language semantics and execution implementations. It must make operations explicit enough that consumers do not infer hidden rules from AST shape.

### Initial core model
The initial design should support modules and functions, parameters and return types, typed values and constants, basic blocks, explicit terminators, arithmetic and comparisons, branches, direct calls and returns, local storage, supported memory operations, runtime operations, and source locations.

Closures, generators, complex object layouts, exceptions, async execution, and foreign calls should be added only after their semantics and lowering strategy are documented.

### Type and value rules
Every instruction must define operand types, result type, side effects, failure conditions, and runtime requirements. Integer width, signedness, overflow, floating-point behavior, string encoding, and pointer assumptions must be explicit. Host compiler behavior must not accidentally define Vayu semantics.

### Control flow
Each basic block needs a valid terminator. Branch targets must exist, and value definitions and uses must obey the chosen SSA or explicit-storage model. Choose one model for each stage and document its invariants rather than mixing models informally.

### Serialization and dumps
Deterministic textual dumps support debugging and golden tests. If serialization is added, it needs schema versioning, deterministic ordering, validation, and a compatibility policy. VIR should not be advertised as a stable external ABI unless that is an explicit, reviewed goal.

### Unsupported lowering
Diagnostics should identify the source construct, source location, selected mode, and limitation. The compiler must not emit a partial artifact and return success.

## 6. Verification and trust boundaries

The verifier is a mandatory correctness boundary.

**Structural checks**
- referenced blocks, values, and functions exist;
- every block has a valid terminator;
- branch edges are valid;
- definitions and uses follow the IR model;
- IDs and indices are in range;
- serialized sizes and counts are bounded.

**Type checks**
- operand and result types match instruction definitions;
- call arguments match function signatures;
- return values match declared return types;
- conversions are explicit and legal;
- memory operations obey supported type and alignment rules.

**Semantic and target checks**
- runtime operations have known contracts;
- side effects are represented correctly;
- target-specific operations are supported by the selected target;
- unsupported features are rejected before emission.

Errors should be deterministic and source-linked where possible. A verifier failure must not crash the compiler, execute generated code, or leave a successful output artifact. Fuzzing and malformed-IR tests should exercise the verifier independently from source parsing.

## 7. Shared language semantics

All routes must agree on observable behavior. The conformance contract should cover:
- evaluation order and side effects;
- lexical scope, shadowing, and name resolution;
- type conversions and inference;
- integer overflow and division;
- floating-point guarantees;
- truthiness and comparisons;
- strings, encoding, slicing, and indexing;
- collection ordering and mutation;
- calls, recursion, and returns;
- exceptions and error propagation;
- I/O, exit status, and process termination;
- global initialization and module loading;
- resource lifetime and runtime-managed values;
- foreign-function boundaries when supported.

The test harness should compare stdout, stderr, exit status, expected failures, and observable side effects. Platform-dependent behavior should be normalized only when the language contract justifies it.

## 8. Interpreter, VM, and native execution

The interpreter and VM are separate execution mechanisms but implement one language contract. Native execution is another implementation of the same contract, not a separate dialect.

- **Interpreter:** direct execution and a semantic reference.
- **VM:** bytecode execution and a second compatibility route.
- **Native:** compiled target code, available only for a tested feature subset.
- **Auto:** future policy layer, enabled only when capability data and conformance evidence justify it.

For every supported fixture, run each applicable route and compare observable results. Minimize and preserve mismatches. Do not hide discrepancies by weakening tests without documenting why behavior is unspecified.

## 9. Native code generation

Native generation is a collection of cooperating subsystems, not a single conversion function.

1. **Target capabilities:** architecture, word size, endianness, ABI, object format, supported operations.
2. **Instruction selection:** map VIR operations to legal instruction sequences.
3. **Calling convention:** parameter passing, return values, preserved registers, stack alignment, and call clobbers.
4. **Register allocation:** track live values, assign registers, insert spills and reloads, handle calls.
5. **Stack-frame layout:** locals, spills, saved registers, outgoing arguments, alignment.
6. **Control-flow emission:** labels, branches, conditional branches, and branch-range handling.
7. **Data emission:** constants, global data, alignment, and symbol visibility.
8. **Relocations:** references resolved later by a linker or loader.
9. **Source mapping:** preserve source locations where the output format supports it.
10. **Artifact validation:** ensure bytes, sections, symbols, and entry point are consistent.

Start with a deliberately small subset such as constants, basic integer operations, comparisons, simple branches, direct calls, and returns. Expand only after tests prove each operation. Closures, heap objects, exceptions, dynamic dispatch, FFI, and runtime calls need explicit strategies.

Correct lowering precedes optimization. Begin with simple, verifiable transformations such as constant folding and dead-code elimination where the IR permits them. Each optimization must preserve semantics and have tests. Optimization settings must affect artifact identity.

## 10. Runtime, ABI, and memory model

The runtime contract defines how generated code interacts with helper functions and values. Document:
- representations of booleans, integers, floats, strings, collections, and references;
- allocation and deallocation policy;
- ownership, aliasing, and lifetime guarantees;
- exception and error propagation;
- string encoding and length conventions;
- helper signatures and calling conventions;
- global initialization and shutdown;
- thread-safety expectations if threading is supported;
- operating-system and foreign-library boundaries.

Generated code and runtime helpers must agree on layout, alignment, symbol names, and calling conventions. ABI-affecting changes must update the runtime ABI version so incompatible cached artifacts are rejected.

## 11. Targets, object files, and linking

A target includes architecture, operating system, ABI, object format, runtime ABI, and capability set. It is not just a CPU name.

Target-specific behavior should be isolated behind an interface. Object emission requires sections, symbols, relocations, alignment, and format-specific metadata. Unsupported relocation forms must fail explicitly.

The linker adapter should:
- locate a supported linker deterministically;
- report the tool and version;
- use structured process arguments where available;
- capture stdout, stderr, and exit status;
- apply timeout or cancellation policy where available;
- verify expected output exists and is non-empty;
- preserve useful failure diagnostics;
- avoid reusing an old output after a failed build.

A system linker is a tool dependency, not the Vayu compiler. Document whether each target emits a complete executable or requires linking.

## 12. Artifact identity and cache

A cache hit is valid only if every input that can affect output matches.

### Cache-key inputs
Include source content, transitive dependency fingerprints, compiler revision, VIR schema, target and ABI, optimization settings, runtime ABI, relevant feature flags, linker identity and arguments when linked output is cached, and generated or configuration inputs.

### Cache safety
- Use content-addressed identities rather than filenames alone.
- Write to a temporary location, validate, then atomically commit.
- Validate metadata and artifact integrity before reuse.
- Reject corrupt, truncated, stale, or incompatible entries.
- Define concurrent-writer behavior.
- Make cache rejection reasons inspectable in debug mode.
- Provide cache bypass or clean-cache controls.
- Never let cache corruption change program semantics.

Tests should cover cold build, warm hit, source and dependency changes, option changes, compiler revision, target changes, runtime ABI changes, corrupted metadata, truncated artifacts, concurrent access, and interrupted writes.

## 13. Incremental compilation

Incremental compilation reuses work only when the dependency graph proves it remains valid.

Record relationships among source modules, imports, generated interfaces, runtime ABI, compiler options, and artifacts. Each node needs a stable identity and fingerprint. A change invalidates affected nodes and every dependent artifact whose result can change. Public interface changes may invalidate more dependents than private implementation changes if the module model supports that distinction.

**Correctness invariant:** an incremental build must produce the same observable program and equivalent artifacts as a clean build with identical inputs and configuration.

Test source mutations, build ordering, cache deletion, and dependency changes against clean builds. If dependency information is uncertain, rebuild conservatively.

## 14. Execution dispatch and adaptive compilation

Dispatch should arrive only after route capabilities are explicit and reliable.

Each route's capability record should describe supported constructs, target constraints, runtime needs, known limitations, and conformance status. Capability flags must be based on implementation evidence.

Explicit modes must be deterministic. An unsupported explicit request fails clearly. Automatic mode may choose only a route known to support the program and must expose the reason for its decision.

Profiling-guided or adaptive compilation is optional future work. Measure profiling overhead, compilation cost, cache hit rate, warm-up, startup latency, memory use, and steady-state speed. Prevent unbounded recompilation. Adaptive compilation is not a prerequisite for a correct static compiler.

## 15. Diagnostics and observability

Diagnostics should identify the phase, source location where available, selected mode and target, and a practical explanation.

Categories include source and semantic errors, unsupported route features, VIR construction and verification, target capability, register allocation and code generation, object emission and relocations, linker discovery and execution, cache validation, dependency invalidation, and runtime failures.

Debug output should make decisions inspectable: selected route, target, IR dumps, verifier results, cache hit or miss reasons, invalidated dependencies, toolchain identity, and per-phase timings. Normal output should remain concise and stable.

## 16. Correctness and test strategy

Use multiple levels of testing:
1. **Unit tests:** IR operations, verifier rules, target helpers, cache keys, diagnostics.
2. **Golden tests:** deterministic VIR dumps and representative diagnostics.
3. **Conformance tests:** identical fixtures across supported execution routes.
4. **Differential tests:** compare output, exit status, and side effects.
5. **Malformed-input tests:** invalid source, invalid IR, corrupt serialization, bad linker output.
6. **Property and fuzz tests:** IR invariants, lowering, verifier robustness, serialization boundaries.
7. **Integration tests:** clean checkout, build, compile, link, and run on supported targets.
8. **Regression tests:** every fixed bug receives a minimal reproducer.
9. **Reproducibility tests:** repeated builds and cache-on/cache-off results.
10. **Performance tests:** representative workloads with environment details and repeated measurements.

A test that could not run must be reported as skipped or unavailable, not passed.

## 17. Performance analysis

Measure separate costs:
- frontend and semantic analysis;
- VIR lowering and verification;
- native code generation;
- link time;
- cold, warm-cache, and incremental build;
- process startup and time to first useful work;
- steady-state runtime;
- memory use, artifact size, and cache size.

Use small startup-heavy programs, compute-heavy loops, allocation-heavy code, strings and collections, call-heavy programs, and realistic application samples. Record exact commands, machine, OS, CPU, compiler revision, target, optimization flags, input size, repetitions, median, spread, and noise sources.

AOT and JIT performance depend on workload. AOT may improve startup or predictable workloads while a JIT may win after warm-up elsewhere. Any claim that VIB beats a JIT must remain a hypothesis until reproducible results support it for named workloads.

## 18. Security and reliability

Treat source, serialized IR, cache entries, tool output, and paths as untrusted input.
- Bound file sizes, instruction counts, nesting, and serialized collection sizes where practical.
- Validate paths and artifact boundaries.
- Avoid unsafe command construction.
- Never execute an artifact after failed or incomplete compilation.
- Never present stale output as the result of a failed build.
- Make cache writes atomic and concurrent access defined.
- Record inputs that prevent reproducible output.
- Test from a clean checkout without undeclared local tools or stale generated files.

## 19. Failure modes and recovery

| Failure | Required behavior |
|---|---|
| Unsupported construct during lowering | Stop with a source-located diagnostic |
| Invalid VIR | Reject before execution or code generation |
| Target lacks an operation | Report unsupported capability or require an explicitly selected supported route |
| Register allocation failure | Fail compilation without a success artifact |
| Linker missing or failing | Report discovery details, exit status, and diagnostics |
| Missing or malformed output | Treat compilation as failed |
| Corrupt or stale cache entry | Reject and rebuild safely |
| Uncertain dependency graph | Rebuild conservatively |
| Runtime ABI mismatch | Reject incompatible artifacts and invalidate affected cache |
| Routes disagree | Preserve a minimal reproducer and add regression coverage |
| Auto mode cannot prove support | Do not choose that route |

## 20. Implementation sequence

The full phase plan and acceptance criteria are in Roadmap.md. The intended order is:
1. Establish reproducible baselines and existing behavior.
2. Define VIB interfaces and module ownership.
3. Specify shared semantics and conformance fixtures.
4. Implement VIR construction and deterministic dumps.
5. Implement the verifier and malformed-IR tests.
6. Connect a minimal VIR subset to an execution route.
7. Implement a small native subset with end-to-end tests.
8. Add object/executable emission and target validation.
9. Integrate runtime and driver without breaking existing modes.
10. Add complete cache identity and atomic storage.
11. Add dependency-aware incremental compilation and clean-build parity.
12. Add inspectable dispatch, then measured optimization and optional adaptive research.
13. Harden release, reproducibility, and support declarations.

Every gate requires implementation, automated tests, documentation, and reproducible evidence. A design document alone does not complete a phase.

## 21. Repository layout

The exact filenames can evolve, but responsibilities should remain separated:

| Area | Responsibility |
|---|---|
| VIB/frontend | Existing frontend integration |
| VIB/ir | VIR model, builder, schema, dumps |
| VIB/verify | IR verifier |
| VIB/vm | VM and interpreter bridge |
| VIB/native | Native lowering and code generation |
| VIB/runtime | Runtime contracts and helpers |
| VIB/targets | Target descriptions and format rules |
| VIB/link | Optional platform-linker adapter |
| VIB/cache | Artifact identity, validation, atomic cache |
| VIB/incremental | Dependency graph and invalidation |
| VIB/dispatch | Execution route selection |
| VIB/tests | Component and integration tests |
| VIB/docs | Component designs and benchmark notes |
| docs/vib.md | Concise architecture contract |
| docs/vib-documentation.md | Full technical documentation |
| Roadmap.md | Phases, dependencies, acceptance gates |

This is a responsibility map, not a claim that every directory already exists.

## 22. Architecture decisions

### Decision 1: VIB is integrated into Vayu
VIB lives inside the Vayu repository and is orchestrated by the existing compiler driver. This preserves one language, one primary driver, and one ownership boundary.

### Decision 2: VIR is Vayu-owned
Vayu defines its IR and semantic contract so the language compiler controls how semantics map to execution.

### Decision 3: Preserve interpreter and VM during native development
Existing execution routes remain compatibility references while native support grows. Support is reported per route, and explicit native requests must not silently fall back.

### Decision 4: Correctness before performance
Native support, caching, incremental builds, and optimizations require tests and reproducible evidence.

### Decision 5: External linking is an adapter
A system linker may be used behind a Vayu-owned interface. Tool versions and arguments are build inputs when they can affect output.

### Decision 6: Adaptive compilation is optional
Adaptive dispatch follows a correct deterministic pipeline and remains experimental until workload measurements justify its complexity.

## 23. Definition of done

VIB is not complete merely because the directory, API skeleton, IR declarations, or native-mode option exists. A component is complete only when:
- its responsibility and contract are documented;
- implementation exists in the repository;
- unsupported behavior fails explicitly;
- success and failure paths have automated tests;
- relevant routes pass conformance checks;
- clean-checkout build instructions work;
- diagnostics are actionable;
- cache and incremental behavior are proven safe when applicable;
- performance claims have reproducible measurements;
- the corresponding Roadmap acceptance criteria are met.

The final standard is a Vayu-owned integrated toolchain whose supported behavior is testable, whose decisions are inspectable, and whose performance claims are supported by evidence.
