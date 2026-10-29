# Vayu Engineering Roadmap

**Language:** Vayu (`.vyu`)  
**Integrated compiler/runtime:** VIB (Vayu Integration Backend)  
**Architecture:** Integrator  
**Primary deliverable:** Vayu Executable package (`.vye`)

This is the forward-looking implementation plan. A phase is complete only when implementation, tests, and acceptance evidence exist. The first priority is a small, correct Integrator. Expand it in small steps rather than trying to support all of Vayu's existing language coverage at once.

## 1. Product contract

- Vayu remains the only source language; source files use `.vyu`.
- VIB is the integrated compiler/runtime system inside this repository. Integrator is the architecture VIB implements, not a separate product or required `vib.exe`.
- The existing Vayu compiler/driver remains the user-facing entry point.
- Integrator first supports a deliberately small subset: basic values, variables, arithmetic, strings, functions, and simple control flow.
- Integrator compiles supported programs into `.vye` files, meaning **Vayu Executable** packages.
- A `.vye` is not a native Windows `.exe`. It is an executable package that requires a compatible Vayu/Integrator runtime, similar in broad deployment concept to a Java archive requiring a Java runtime.
- Vayu itself and the Integrator runtime must share the `.vye` specification. No separate Integrator executable is required.
- Existing interpreter/VM coverage is reference material, not proof that every feature works in Integrator.
- Do not require a bundled foreign-language runtime, DLL directory, LLD executable, platform import library, or external compiler backend.
- ONNX support is deferred. Later, Vayu will have its own ONNX reader/runtime and native test fixtures, without a Python ONNX generator or Python package dependency.

## 2. Starting point

Vayu already has substantial frontend, language, interpreter/VM, tooling, and example coverage. Integrator is a new, intentionally smaller execution path. Its first job is correctness and completeness for a narrow subset, not feature parity or optimization.

The native compiler path and .vye format must not be called complete until code, tests, and acceptance gates pass. Do not claim a planned feature is implemented because documentation, an example, or a placeholder exists.

## 3. Target pipeline

```text
.vyu source
    |
    v
Vayu lexer / parser / semantic analysis
    |
    v
Integrator lowering for the supported subset
    |
    v
Verified Integrator representation
    |
    v
.vye package writer -> strict format validation
    |
    v
Vayu / Integrator runtime
    |
    v
Program output and exit status
```

Keep the first internal representation small. Add abstraction only when needed by implemented semantics.

## 4. .vye format requirements

The format specification must define:
- magic bytes and format version;
- bounded section table, offsets, lengths, and entry point;
- function metadata, constant pool, string encoding, and typed instructions or verified IR;
- optional debug/source metadata;
- deterministic serialization;
- integrity checks and explicit runtime compatibility policy;
- behavior for unknown required features and unsupported versions.

The reader must validate offsets, lengths, overflow, section overlap, constant/function indices, opcodes, types, branch targets, signatures, and entry point before execution. Never execute malformed or partially validated packages. Do not serialize raw pointers, process addresses, or host-specific object layouts.

A `.vye` is a Vayu/Integrator executable package, not a native executable or a promise of universal cross-version compatibility. Compatible Vayu/Integrator runtime installation is required.

## 5. Phased roadmap

### Phase 0 — Baseline and feature boundary
- Record clean build/run commands and current driver behavior.
- Inventory the frontend, type checker, interpreter, VM, runtime, and tests.
- Create a feature matrix distinguishing existing Vayu support from Integrator support.
- Capture reference output, errors, and exit codes for initial programs.

**Exit:** reproducible baseline, feature matrix, and reference fixtures.

### Phase 1 — Minimal Integrator skeleton
- Integrate a small pipeline into the existing driver.
- Define diagnostics, result types, and explicit unsupported-feature errors.
- Add a dedicated Integrator test harness.
- Avoid premature optimizer, cache, linker, and adaptive-compilation work.

**Exit:** Integrator builds with Vayu; unsupported constructs fail clearly; current interpreter/VM routes remain intact.

### Phase 2 — Versioned .vye container
- Specify and implement a deterministic writer and strict reader.
- Add version checks, bounds validation, and malformed/truncated file rejection.
- Add a compile-to-`.vye` path and a run-`.vye` path in the existing driver.
- Running a package must not silently recompile source or invoke a foreign runtime.

**Exit:** hello-world package works; round-trip and corruption tests pass.

### Phase 3 — Basic values and arithmetic
- Start with a small documented set such as integers and booleans; add floats only with explicit semantics.
- Implement constants, local variables, assignment, arithmetic, comparisons, and type checks.
- Define evaluation order, overflow, conversions, and division-by-zero behavior.

**Exit:** positive/negative tests, stable diagnostics, verified instructions, and deterministic output.

### Phase 4 — Strings
- Define UTF-8 policy, representation, equality, concatenation, length/index semantics, and conversions.
- Add string constants to the package constant pool.
- Implement safe lifetime, allocation, bounds checks, and runtime errors.

**Exit:** empty, ASCII, multibyte UTF-8, concatenation, equality, boundary, and invalid-operation tests pass.

### Phase 5 — Functions and calls
- Add functions, parameters, return values, local scopes, and direct calls.
- Validate function indices, argument counts/types, return types, and call-stack limits.
- Add recursion only after stack-frame safety is implemented.

**Exit:** nested calls, parameter/return tests, and malformed-call rejection pass.

### Phase 6 — Control flow
- Add conditional branches, loops, and explicit return behavior.
- Validate branch targets and reachable control flow.
- Add documented resource limits where needed.

**Exit:** branch/loop fixtures pass; invalid targets are rejected.

### Phase 7 — Core runtime and small collections
- Add only required runtime operations and first collection types, each with explicit representation and ownership rules.
- Specify indexing, mutation, length, iteration, and error behavior.
- Verify every runtime operation before execution.

**Exit:** collection and resource-safety tests pass.

### Phase 8 — Modules and multi-file programs
- Define module identity, import resolution, and package dependency behavior.
- Detect missing modules, duplicate symbols, and incompatible dependencies.
- Keep builds reproducible from declared inputs.

**Exit:** a small multi-file program builds and runs from `.vye`.

### Phase 9 — Format hardening and compatibility
- Fuzz/property-test the reader and verifier.
- Test huge counts, overflow, invalid indices/opcodes, truncated sections, bad strings, and stack exhaustion.
- Document runtime requirements and compatibility policy.
- Add clean-checkout CI coverage.

**Exit:** malformed packages are safely rejected; all supported-subset tests pass.

### Phase 10 — Incrementally expand Vayu coverage
Only after the earlier phases pass, add features in small groups: richer numeric operations, tuples/lists/maps/sets, exceptions, classes/objects, closures, higher-order functions, and system APIs. Concurrency comes only after a clear memory and scheduling model exists.

Each feature requires a design, implementation, tests, documentation, and parity evidence. Existing Vayu support must never be treated as automatic Integrator support.

**Exit:** every advertised Integrator feature has dedicated conformance tests.

### Phase 11 — Performance, cache, and incremental builds
- Measure build time, startup, execution, memory, and `.vye` size before optimizing.
- Add caching only after artifact identity and invalidation are specified.
- Add incremental compilation only after it matches clean-build output.
- Keep adaptive/profiling-guided execution as later optional research.

**Exit:** reproducible measurements, cache recovery tests, and clean/incremental equivalence.

### Phase 12 — Native compilation research
Native machine-code generation is a later track, not a prerequisite for the first working Integrator or `.vye` runtime.
- Reuse the verified representation where appropriate.
- Keep target/ABI rules in Vayu-owned code.
- Do not require bundled LLD, external backend executables, DLL folders, or bundled import libraries.
- Advertise native support only for targets and features with end-to-end tests.
- Keep `.vye` execution as the portable baseline.

**Exit:** native artifacts pass conformance on each explicitly supported target.

### Phase 13 — Vayu-native ONNX and AI/ML
ONNX is deferred until Integrator and `.vye` are stable.
- Design a Vayu-owned ONNX parser/reader and internal model representation.
- Validate format structure, tensor shapes/types, operator support, and resource limits.
- Implement a small operator subset first, expanding through tests.
- Build native fixtures without a Python ONNX generator.
- Add numerical correctness tests before performance or GPU work.

**Exit:** native reader/runtime passes valid and malformed-model tests; compiler/runtime has no Python, NumPy, or ONNX package dependency.

### Phase 14 — Release and long-term expansion
- Publish the supported feature/runtime-version matrix.
- Test clean installs and `.vye` execution on each claimed platform.
- Document debugging, deterministic builds, security, compatibility, and diagnostics.
- Consider self-hosting only after the Integrator subset is stable.
- Tie performance claims to reproducible workload measurements.

**Exit:** clean build/install, full supported-subset conformance, and reproducible CI evidence.

## 6. Required validation for every phase

Each phase needs unit tests, supported-behavior tests, negative tests, malformed-input tests where applicable, comparison against reference semantics, reproducible commands, and updated support documentation. A placeholder, roadmap entry, or example alone is not implementation evidence.

## 7. Performance policy

Measure compile time, runtime startup, steady-state execution, memory, and `.vye` size separately. Do not promise that Integrator outperforms JITs or native compilers without repeatable workload-specific evidence. Correctness and a complete small subset come first.

## 8. First major milestone definition

The first major Integrator milestone is complete when:
- a small, documented Vayu subset lowers into a verified representation;
- the compiler writes a valid, versioned `.vye` package;
- the Vayu/Integrator runtime executes it without recompiling source;
- malformed packages are safely rejected;
- strings, functions, and control flow work according to documented semantics;
- deterministic output, failure cases, and runtime compatibility are tested;
- no Python runtime, ONNX generator, DLL bundle, LLD executable, or external backend is required;
- existing interpreter/VM workflows remain intact.

**Rule:** Integrator support is established by implementation and tests, not by Vayu's broader historical feature coverage.
