# Vayu Vision & Roadmap

Vayu is an independent programming-language project aiming to combine readable Python-inspired syntax with native performance, low-level control, modern application development, portability, and an AI-ready ecosystem.

> **Write simple. Run native. Control everything.**

## Current Foundation

The current source tree demonstrates a substantially expanded language/compiler foundation including:

- variables, type inference and explicit type annotations
- primitive and collection types
- functions, recursion, lambdas and closures
- conditions, loops and compound assignment
- lists, maps, tuples and sets, including slicing
- structs, classes, inheritance, overriding and `super()`
- exceptions and modules
- constants, enums, static members and visibility
- bitwise operations, `match`, `with`, `defer`, `yield`, and generators
- generic/type parameters and constraints
- ownership/reference types (`unique<T>`, `shared<T>`, `weak<T>`)
- raw pointers/references, pointer arithmetic, `malloc` / `free`
- C FFI, function pointers, external library linking, and supported struct-by-value FFI
- runtime modules including regex, thread, net, crypto, random, OS, math and related utilities
- native CPython loading and expanded Python value/call interoperability
- interpreter, bytecode/VM and native execution paths

The current development history now records **Phase 23 as completed**. Phases 19 and 20 established the native GUI and 2D graphics foundation, Phase 21 completed the software raster and 3D pipeline, Phase 22 established the package-ecosystem direction, and Phase 23 completed the single-binary native-toolchain direction by establishing VCB as a separate backend project.

The C++ `vayuc` compiler remains the bootstrap/reference compiler while the project continues toward self-hosting. Phase 24.0 native floats and 24.1 native math/float collections are implemented; 24.2 is the remaining tensor-storage migration milestone.

## Roadmap

The roadmap below records the project's planned development phases. A phase may contain work that is implemented incrementally across multiple commits.

| Phase | Scope |
|---:|---|
| **1–17** | Core language, runtime, native compilation, self-hosting preparation, ownership, FFI and CPython interoperability. **Completed development phases.** |
| **18** | LSP, formatter, linter, VS Code extension and compiler/runtime tooling. **Completed.** |
| **19** | Native window / event / widget layer and GUI canvas foundation. **Completed.** |
| **20** | 2D graphics, transforms, text, image/UI primitives and canvas functionality. **Completed foundation.** |
| **21** | Software raster + 3D pipeline: framebuffers, depth buffers, Q16.16 matrices, meshes, textures, mipmaps, perspective-correct rasterization, lighting and post-processing. **Completed.** |
| **22** | Package ecosystem and continued runtime/UI performance work: `vayu install`, `vypy install`, public index, dependency resolution, lockfiles, publishing and optimization. **Completed development phase.** |
| **23** | **Native backend transition:** establish VCB as a separate backend project and add a selectable Vayu `--backend qbe|vcb` boundary. QBE remains the current default. **Completed direction.** |
| **24** | **Native floats** — 24.0 float type + ABI; 24.1 math + collections; 24.2 tensor migration. **Current.** |
| **25** | **Benchmarking Round 1 (pre-VCB)** — harness/control programs, compute-heavy workloads, allocation-heavy workloads and reproducible reports. |
| **26** | **Version cut 1** — Linux/macOS toolchain builds, SDL3 GUI/raster direction, VS Community LSP, VS Code Marketplace + Open VSX publishing, Vayu rewrites of `tools/*.py`, documentation and release. |
| **27** | **VCB — separate project** — 27.0 PE object emitter; 27.1 direct codegen; 27.2 linker + runtime merge; 27.3 ELF/Mach-O; 27.4 ARM64. |
| **28** | **Benchmarking Round 2 (post-VCB)** — same benchmark suite with before/after VCB delta reporting and regression detection. |
| **29** | **Version cut 2** — VCB-era single-binary toolchain release; QBE/GCC retired from docs and packaging; target `v1.0`. |
| **30+** | **Open / parking lot** — shaders, escape analysis, multi-input ONNX, GPU tensor support, optimizer/loss additions and other future work. |

### Phase 24 — Native floats

Phase 24 is now partially implemented. Native floating-point values have landed in the compiler/runtime, followed by native math and float-aware collections. Tensor storage migration remains the final Phase 24 milestone.

- **24.0 — Float type + ABI.** **Implemented.** Native `float` values, arithmetic, comparisons, unary negate, mixed integer/float operations, conversions and formatting.
- **24.1 — Math library + collections.** **Implemented.** Native `math.*`, `list<float>`, `map<str, float>`, and float/int/string conversions.
- **24.2 — Tensor migration.** **Remaining.** Move `tensor` from Q16.16 to f32/f64 internally, with a temporary compatibility layer before retiring the old representation.

**Exit criteria:** fixpoint still passes; `examples/native_floats.vyu` compiles and matches tree-walk output; `math.*` outputs are bit-identical to tree-walk on roughly 30 expressions.

**Why before benchmarks:** integer-only benchmark programs would distort comparisons with C++, Java and Python. Floats land first so the benchmark suite represents the workloads developers would actually write.

### Phase 25 — Benchmarking (Round 1, pre-VCB)

A dedicated root-level `benchmarks/` tree will contain the methodology, runner, four-language programs and generated results. Vayu runs each program through tree-walk, VM and native execution.

**25.0 — Harness + control programs:** `arith`, `fib`, `startup`.

**25.1 — Compute-heavy:** `matmul` (512×512 dense), `nbody` (5-body, 1M steps), `mandelbrot` (2000×2000).

**25.2 — Allocation-heavy:** `string_concat` (100k `+=`), `hashmap` (1M string→int insert/lookup), `json_parse` (10 MB), `sort` (10M ints).

Comparison targets: C++ `-O3 -march=native`, Java 17 with `-Xmx4g`, and CPython 3.12. Each program runs 5×; the best wall-clock time and peak RSS are reported. Results include a markdown table, per-program bars and `results/SUMMARY.md`.

### Phase 26 — Version cut 1

First coherent release cut: Linux + macOS toolchain builds, SDL3 replacing Win32 in the GUI/raster direction, VS Community LSP integration, VS Code Marketplace + Open VSX publication, Vayu rewrites of `tools/*.py`, documentation site and release notes. Target version remains a release decision at cut time (`v0.9` or `v1.0`).

### Phase 27 — VCB (separate project)

VCB is a sister project, not a Vayu phase in the same tree. Its projected work is:

- **27.0 — Object file emitter:** x86-64 PE `.obj` from the Vayu SSA IL boundary.
- **27.1 — Direct codegen:** x86-64 machine code from the compiler IR/AST path with Vayu-aware optimization, inlining and register allocation.
- **27.2 — Linker + runtime merge:** produce executables without invoking GCC.
- **27.3 — ELF + Mach-O:** Linux and macOS object formats.
- **27.4 — ARM64:** Apple Silicon and ARM Linux, with other targets considered later.

**Exit criteria:** `vayuc file.vyu --native` can produce an `.exe` without invoking `qbe` or `gcc`; fixpoint still passes with VCB; peak fixpoint RSS falls from roughly 15 GB to under 1 GB.

### Phase 28 — Benchmarking (Round 2, post-VCB)

The same benchmark tree, ten-program suite and comparison languages are reused. Only Vayu's native backend changes from QBE + GCC to VCB. `results/SUMMARY_AFTER_VCB.md` records per-program time/memory deltas and highlights regressions.

### Phase 29 — Version cut 2

Ship the VCB-era toolchain as the single-binary release: bundle VCB for Windows/Linux/macOS, remove QBE and GCC dependency references from docs and packaging, publish the benchmark delta table, and target `v1.0`.

### Beyond Phase 29

Known parking-lot items include escape analysis, programmable shaders, DirectML only if needed, GPU-accelerated tensors beyond CUDA, Adam/cross-entropy/softmax additions, and multi-input ONNX for CNNs and transformers.

### Phase 19–21 — GUI, 2D Graphics and 3D Raster

The current tooling layer is now a major part of the repository. It includes:

- `vls` — Vayu Language Server
- `vfmt` — Vayu formatter
- `vlint` — Vayu linter
- VS Code extension for `.vyu`
- diagnostics, hover, completion and go-to-definition
- references, rename and signature help
- formatter and linter integration
- compiler `--opt` and runtime-emission tooling

The official VS Code extension is published as `Fliczo.vayu`.

**Marketplace:** https://marketplace.visualstudio.com/items?itemName=Fliczo.vayu

**Direct install:** `vscode:extension/Fliczo.vayu`

### Phase 19–21 — GUI, 2D Graphics and 3D Raster

The GUI foundation now provides native windows, events, callbacks, canvas drawing, widgets, text, transforms and image-oriented examples. The graphics layer has been extended with a software/hybrid raster pipeline.

Phase 21 adds:

- framebuffer allocation and presentation
- optional depth buffering
- 4x4 Q16.16 matrix operations
- translation, rotation, perspective and look-at transforms
- dynamic meshes, UVs and normals
- texture creation, pixel access and mipmaps
- nearest, bilinear and trilinear filtering
- perspective-correct textured triangle rasterization
- depth-tested 3D mesh rendering
- ambient and directional lighting
- per-vertex/Gouraud/Phong-style lighting and specular support
- render-to-texture through framebuffer snapshots
- gamma, invert, tint, brightness and threshold post-processing
- native examples for triangle, textured cube, lit cube and post-processing

The native compiler dispatcher and runtime now expose these APIs through the `raster` module. The GUI layer also caches GDI+ graphics, fonts and pens to reduce repeated paint-time setup.

### Self-Hosting Direction

The immediate compiler objective remains self-hosting rather than rewriting the project from scratch. The existing C++ `vayuc` remains the bootstrap/reference compiler while the Vayu implementation is developed toward feature parity. Once Vayu can reliably compile the compiler and the bootstrap process is stable, the long-term objective is to remove dependence on the C++ bootstrap implementation.

### Later Ecosystem Direction

After self-hosting, the roadmap continues through developer tooling, GUI/window/event/widget APIs, 2D and then 3D graphics, AI/ML infrastructure, and the package ecosystem. These later phases depend on a stable compiler, runtime, standard library, interoperability layer, and tooling foundation.

## ⚙️ VCB — Vayu Compiler Backend

<a href="https://github.com/NotY215/VCB">
  <img src="https://raw.githubusercontent.com/NotY215/VCB/master/Assets/VCB_logo.png" alt="VCB Logo" width="160">
</a>

**[VCB](https://github.com/NotY215/VCB) is a companion project and planned native backend component of the Vayu compiler ecosystem.**

VCB is designed to provide a clean boundary between Vayu's frontend/IR and machine-code generation. The Vayu compiler currently selects QBE by default, while VCB is selectable and developed as a separate project:

```text
Vayu Source
    │
    ▼
Lexer → Parser → AST → Semantic Analysis
    │
    ▼
Vayu IR
    │
    ▼
VCB
├── Analysis
├── Optimization
├── Lowering
└── x86-64 Code Generation
    │
    ▼
Native Assembly
    │
    ▼
Executable
```

### Why VCB exists

The Vayu project is intended to separate language design from backend engineering. VCB gives the ecosystem a focused backend project where compiler analysis, optimization, lowering, instruction selection, register allocation, calling conventions, and native code generation can be developed independently.

This separation is important for the long-term Vayu architecture: the frontend can evolve its syntax, type system, modules, and language features while the backend can evolve its machine-level implementation without requiring the two areas to become one large codebase.

### Long-term VCB role

VCB is **not a replacement for the Vayu language frontend**. It is intended to become one of the native backend layers through which Vayu programs can eventually reach optimized machine code.

The planned relationship is:

**Vayu → Vayu IR → VCB → Native Assembly → Executable**

As Vayu approaches self-hosting, VCB can provide the native code-generation foundation needed by the bootstrapped compiler and later compiler/runtime tooling.

**[→ View the VCB repository](https://github.com/NotY215/VCB)**

---

## Benchmarking

Benchmarking is part of the development process. Performance measurements should track representative compiler/runtime workloads as native execution and optimization improve.

Published benchmark information can change as the implementation and test environment change, so the [official Vayu website](https://vayu.gt.tc) is the preferred location for current benchmark information.

## Long-Term Goal

The ultimate goal is not simply "Python but faster" or "C++ with Python syntax." Vayu aims to become a unified ecosystem capable of covering:

**simple programs → applications → AI → games → systems → native software**

while keeping the language approachable and giving developers progressively more control when they need it.


## Current Phase Status

**Phases 1–23 are complete development phases, and Phase 24 is the current direction.** Phase 15 delivered raw pointer/reference semantics, C FFI, external linking, function-pointer support, supported FFI struct wrapping, and `malloc`/`free`. Phase 16 established native CPython integration with primitive value bridging, Python import/attribute/call support, and reference-count handling. Phase 17 advanced the compiler toward self-hosting. Phase 18 added the LSP, formatter, linter, VS Code extension, expanded LSP capabilities, and related compiler/runtime tooling.

## Current Direction

The project is moving from broad core-language/runtime capability toward a complete development ecosystem. The immediate areas are compiler/bootstrap stabilization, self-hosting, memory/resource correctness, native backend evolution, standard-library growth, package tooling, concurrency, and continued developer-tooling improvements.

The project has now progressed through the GUI/event/widget layer, 2D graphics foundation, software-raster/3D pipeline, package-ecosystem direction, and the Phase 23 native-toolchain transition. Current work is Phase 24 native floats, followed by benchmark-driven release and VCB work.

## Architecture Direction

```text
Vayu Source (.vyu)
        ↓
Lexer → Parser → AST → Semantic / Type Checking
        ↓
   Vayu IR / compiler pipeline
        ↓
Interpreter / VM / Native compilation
        ↓
   Future VCB backend
        ↓
Native Assembly / Machine Code
        ↓
Executable
```

VCB is a companion project, not a replacement for the Vayu frontend. Its long-term role is to provide the native machine-code backend boundary for the compiler ecosystem.

## Developer Ecosystem

The current editor-facing ecosystem consists of the compiler plus `vls`, `vfmt`, `vlint`, and the official VS Code extension. This establishes the tooling foundation required before the later GUI, AI, package, and native-backend phases can become a cohesive development platform.
