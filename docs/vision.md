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

The current development history records **Phase 23 as completed**. Phases 19 and 20 established the native GUI and 2D graphics foundation, Phase 21 completed the software raster and 3D pipeline, Phase 22 established the package-ecosystem direction, and Phase 23 completed the single-binary native-toolchain direction by establishing VCB as a separate backend project.

The C++ `vayuc` compiler remains the bootstrap/reference compiler while the project continues toward self-hosting. Phases 1–24 are complete and Phase 25 is skipped for now. VCB Phase 26 Part 1 and Part 2 are complete. Phase 27 Parts 1–10 and 12–16 are complete; Part 11 is partial, with Part 17 as the next backend milestone.

## Roadmap

The roadmap below tracks Vayu and the companion VCB backend together.

| Phase | Scope |
|---:|---|
| **1–24** | Core language, runtime, tooling, graphics, package ecosystem, native floats and related compiler/runtime work. **Completed.** |
| **25** | **Skipped for now** — benchmarking postponed until after the VCB transition. |
| **26 Part 1** | **Completed** — VCB project, IR, parser, printer, and `vcb dump`. |
| **26 Part 2** | **Completed** — x86-64 code generation, PE + ELF writers, prebuilt runtime, and Vayu's native path transitioned from QBE+GCC to VCB. |
| **27 Part 1** | **Done** — ELF writer, Linux syscall runtime subset. |
| **27 Part 2** | **Done** — Linux heap using a `brk`-based bump allocator, list + map + string-method runtime support. |
| **27 Part 3** | **Done** — PE padding heuristic. |
| **27 Part 4** | **Done** — `DYNAMIC_BASE` disabled, dynamic sections, shadow space in `vayu_exit`. |
| **27 Part 5** | **Done** — Minimal `.reloc` support. |
| **27 Part 6** | **Done** — ASLR re-enabled; `.pdata` / `.xdata` for user functions and the entry stub. |
| **27 Part 7** | **Done** — kernel32 heap APIs. |
| **27 Part 8** | **Done** — Linux `vayu_print_float`. |
| **27 Part 9** | **Done** — Always emit `.rdata`, always pad, `RELOCS_STRIPPED` + ASLR off. |
| **Diagnosis** | **Done** — Block identified as inbox WDAC (`VerifiedAndReputableDesktop`). |
| **27 Part 10** | **Done** — `vcb sign` / `vcb verify` / `vcb build --sign`. |
| **27 Part 11** | **Partial** — WDAC supplemental policy requires `-UserPEs` and a Microsoft-trusted signer; full acceptance requires a CA-trusted certificate or a machine without the inbox policy. |
| **27 Part 12** | **Done** — Import-table construction remains in `X64.cpp` for now; runtime `.pdata` deferred to Part 15. |
| **27 Part 13** | **Done** — `X64Common.hpp` extracted; `X64.cpp` reduced to a thin wrapper. |
| **27 Part 14** | **Done** — PE test matrix covering 11 programs; `t10` fixed. |
| **27 Part 15** | **Done** — Runtime `.pdata` emitted through `emitRuntime(..., &unwindEntries)` and `.xdata` built from all unwind entries. |
| **27 Part 16** | **Done** — `tests\\elf_matrix.ps1` added for Linux target coverage. |
| **27 Part 17** | **Next** — Move import-table construction into `writePe`; remove the `kIdataRva` hardcode; add Linux `.eh_frame`. |
| **28** | Full benchmarking on VCB-built binaries. |
| **29** | Version cut. |
| **30+** | Open — BigFloat, shaders, escape analysis 8.1–8.3, multi-input ONNX, GPU tensor, Adam/softmax. |

### Phase 25 — Skipped for now

Benchmarking was intentionally postponed until after the VCB transition. The existing control benchmark suite remains available for later comparison, but Phase 25 is not an active development phase.

### Phase 26 — VCB foundation and native backend transition

**26 Part 1 is completed:** VCB IR, parser, printer, and `vcb dump`.

**26 Part 2 is completed:** x86-64 code generation, PE + ELF writers, prebuilt runtime, and the Vayu native compiler transition from the old QBE+GCC chain to VCB.

### Phase 27 — VCB native backend correctness and validation

**27 Parts 1–10 and 12–16 — Done; Part 11 — Partial.** ELF output, Linux runtime expansion, PE correctness, relocations/ASLR, unwind metadata, kernel32 heap APIs, Linux float printing, PE layout corrections, WDAC diagnosis/signing tools, WDAC supplemental-policy generation, shared x86-64 emitter extraction, the 11-program PE matrix, runtime unwind metadata, and the Linux ELF matrix.

**27 Part 17 — Next:** move PE import-table construction into `writePe`, remove the `kIdataRva` hardcode, and add Linux `.eh_frame` support.

### Phase 28 — Benchmarking on VCB

The benchmark suite returns after the VCB native path is sufficiently complete. Results should distinguish Windows PE and Linux ELF targets and record the VCB revision used.

### Phase 29 — Version cut

The VCB-era Vayu toolchain is prepared for a release cut once the required native targets, runtime coverage, packaging and validation are complete.


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

**[VCB](https://github.com/NotY215/VCB) is the companion native backend used by the Vayu compiler.**

VCB provides the native backend boundary between Vayu's frontend/IR and machine-code generation. The Vayu compiler now uses VCB for native compilation, while VCB remains a separate companion repository:

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

VCB is **not a replacement for the Vayu language frontend**. It owns the native backend boundary: VCBIR parsing, x86-64 code generation, runtime emission, PE/ELF writing, and related executable-format support.

The current relationship is:

**Vayu → VcbLower → VCBIR → VCB → x86-64 codegen + runtime → PE / ELF executable**

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

**Phases 1–26 Part 1 are complete development phases. Phase 26 Part 2 is the current direction. Phase 25 is skipped for now and will be revisited after VCB.** Phase 15 delivered raw pointer/reference semantics, C FFI, external linking, function-pointer support, supported FFI struct wrapping, and `malloc`/`free`. Phase 16 established native CPython integration with primitive value bridging, Python import/attribute/call support, and reference-count handling. Phase 17 advanced the compiler toward self-hosting. Phase 18 added the LSP, formatter, linter, VS Code extension, expanded LSP capabilities, and related compiler/runtime tooling.

## Current Direction

The project is moving from broad core-language/runtime capability toward a complete development ecosystem. The immediate areas are compiler/bootstrap stabilization, self-hosting, memory/resource correctness, native backend evolution, standard-library growth, package tooling, concurrency, and continued developer-tooling improvements.

The project has progressed through the GUI/event/widget layer, 2D graphics foundation, software-raster/3D pipeline, package ecosystem, native floats, and the VCB transition. Current work is Phase 26 Part 2: native x86-64 code generation, PE/ELF output, prebuilt runtime integration, and migration of `vayuc` from QBE+GCC to VCB.

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
   VCB backend
        ↓
x86-64 machine code + runtime
        ↓
PE / ELF executable
```

VCB is a companion backend project, not a replacement for the Vayu frontend. Its current native path is implemented for Windows PE and Linux ELF targets, with Phase 27 continuing backend hardening and target validation.

## Developer Ecosystem

The current editor-facing ecosystem consists of the compiler plus `vls`, `vfmt`, `vlint`, and the official VS Code extension. This establishes the tooling foundation required before the later GUI, AI, package, and native-backend phases can become a cohesive development platform.
