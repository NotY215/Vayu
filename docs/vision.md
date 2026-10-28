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

The C++ `vayuc` compiler remains the bootstrap/reference compiler while the project continues toward self-hosting. Phases 1–24 are complete and Phase 25 is skipped for now. VCB Phases 26–30 are complete. Phase 31 is in progress: 31.1–31.5 are done, 31.6a and 31.6b are this drop, and 31.6c is the next drop. The WDAC structural fix is marked done, although behavior remains intermittent for some binaries. Signing is removed from the roadmap because the object + external-linker path replaces that dependency.

## Roadmap

The roadmap below tracks Vayu and the companion VCB backend together.

| Phase / Item | Status | Scope |
|---|---|---|
| 27 Parts 1-17 | done | ELF writer, Linux runtime, PE correctness, validator |
| WDAC structural fix | done | `.rsrc`, rich header, 32 KB minimum image |
| Signing | removed | Not needed once obj+linker path lands |
| Tools | ready | `VCB/tools/`: `lld-link.exe`, `ld.lld.exe` (LLVM 22.1.3), `kernel32.lib`, LLVM inspection tools |
| 28.0 | done | Phase 28 test-harness and integration cleanup |
| 28.1 | done | COFF `.obj` emitter + `vcb emit-obj` |
| 28.2 | done | ELF `.o` emitter |
| 28.3 | done | `vcb link` with `lld-link` / `ld.lld` dispatch |
| 28.4 | done | `vayuc --native` external-link default |
| 28.5 | done | VcbLower builtin guard |
| 29 | done | MAC policy templates |
| 30 | done | Small VcbLower additions |
| 31.1 | done | Tuple literal/index/len; set literal/add/contains/len; string slice `s[a:b]` |
| 31.2 | done | `s[a:]`, `s[:b]`, `s[:]`; tuple/set printing; set remove/clear |
| 31.3 | done | List slicing, `str.strip()`, `list.pop()`, `list.contains(v)` |
| 31.4 | done | `str.split`, single-character `str.replace`, `list.index`, `set.copy`, tuple slicing |
| 31.5 | done | Multicharacter `str.replace`, `str.count(sub)` |
| 31.6a | this drop | `str.is_digit`, `is_alpha`, `is_space`, `is_lower`, `is_upper`, `is_alnum`, `is_ascii` |
| 31.6b | this drop | `str.ljust(width[, fill])`, `str.rjust`, `str.center`, `str.zfill(width)` |
| 31.6c | next drop | `str.strip(chars)`, `str.lstrip([chars])`, `str.rstrip([chars])` |
| 31.7 | not started | Bug fixes: `types.vyu` float, `vm_arith.vyu`, `vm_flow.vyu`, `vcb_collections.vyu` truncation, `fib.vyu` segfault, stack-overflow guards |
| 31.8 | not started | Dict: `keys`, `values`, `items`, `pop`, `pop_or`, `update`, `setdefault`, `get_or`, `copy`, `clear` |
| 31.9 | not started | List: `sort`, `reverse`, `remove`, `insert`, `count`, `extend`, `clear`, `copy`, `first`, `last` |
| 31.10 | not started | String: `find`, `rfind`, `index`, `partition`, `rpartition`, `format` |
| 31.11 | not started | Tuple: `index`, `count`; set: `union`, `intersection`, `difference`, `symmetric_difference`, `issubset`, `issuperset` |
| 32.1 | not started | Struct definitions: heap field layout, constructor, field access and assignment |
| 32.2 | not started | Struct methods: `self` binding, method calls, field access through `self` |
| 32.3 | not started | Classes with `__init__` and positional constructor arguments |
| 32.4 | not started | Inherited fields visible in child types |
| 32.5 | not started | Method inheritance, overrides, dynamic-type dispatch |
| 32.6 | not started | `super().__init__(...)` and `super().method(...)` |
| 32.7 | not started | Static fields: `ClassName.field` read/write |
| 32.8 | not started | `str(obj)` and `print(obj)` as `Name(field=val, ...)` |
| 32.9 | not started | `type(x)`, `isinstance(x, T)`, `hasattr(x, "f")` |
| 32.10 | not started | Enforce public/private/protected visibility in sema and VcbLower |
| 32.11 | not started | Keyword arguments for class constructors and method calls |
| 33.1 | not started | Lambda expression, single-expression body, no captures |
| 33.2 | not started | Capturing lambdas with heap-allocated upvalue block, one cell per captured name |
| 33.3 | not started | Nested `def` inside `def` using the same upvalue system |
| 33.4 | not started | First-class functions: return, pass as arguments, store in lists/maps |
| 33.5 | not started | Bound method values: capture `obj.method` as callable |
| 33.6 | not started | `map`, `filter`, `reduce`, `sorted(key=...)` accept Vayu lambdas |
| 33.7 | not started | Closure recursion and forward references |
| 33.8 | not started | Python-like late-binding closure capture semantics |
| 34.1 | not started | VCB `.pdata` and `.xdata` emission (COFF prerequisite) |
| 34.2 | not started | Basic `try/except`, single handler, no `as` binding |
| 34.3 | not started | `except Exc as e` binding |
| 34.4 | not started | Multiple handlers with type matching |
| 34.5 | not started | Unconditional `finally` |
| 34.6 | not started | `raise Exc(msg)` and bare `raise` |
| 34.7 | not started | Custom exception classes subclassing `Exception` |
| 34.8 | not started | Exception propagation through call stack |
| 34.9 | not started | Uncaught exception stderr traceback and exit code 1 |
| 34.10 | not started | Linux exception path via `.eh_frame` or minimal `__cxa_throw` equivalent |
| 34.11 | not started | Linux `vayu_print_float` |
| 34.12 | not started | `defer` runs on normal and exception exits |
| 35.1 | not started | Preserve `VType::Ptr` through VcbLower expressions and assignments |
| 35.2 | not started | `ptr<T>`, address-of `&x`, and dereference `*p` on locals |
| 35.3 | not started | `&obj.field` and `*obj.field = v` |
| 35.4 | not started | `&arr[i]` and `*arr[i] = v` |
| 35.5 | not started | Pointer arithmetic: `p + i`, `p - i`, `p1 - p2` |
| 35.6 | not started | `malloc(size)` / `free(p)`: Windows HeapAlloc/HeapFree, Linux `brk` |
| 35.7 | not started | Single-function `extern "C"` declarations such as `def puts(s: str) -> int` |
| 35.8 | not started | Mixed-signature foreign calls: up to 4 Win64 or 6 SysV arguments |
| 35.9 | not started | Foreign struct layout with field alignment |
| 35.10 | not started | FFI callbacks passing Vayu function pointers to C and back |
| 35.11 | not started | C string conversions: `str_to_cstr`, `cstr_to_str` |
| 35.12 | not started | `unsafe:` blocks for raw pointer access and manual casts |
| 35.13 | not started | `ref<T>` with non-null invariant enforced by sema |
| 36 | not started | Modules and standard library |
| 37 | not started | `.text` size limit removal |
| 38 | not started | Self-hosting on VCB IR |
| 39 | not started | Full benchmark suite |
| 40 | not started | Version cut |

### Phase 25 -- Skipped for now

Benchmarking was intentionally postponed until after the VCB transition. The existing control benchmark suite remains available for later comparison, but Phase 25 is not an active development phase.

### Phase 26 -- VCB foundation and native backend transition

**26 Part 1 is completed:** VCB IR, parser, printer, and `vcb dump`.

**26 Part 2 is completed:** x86-64 code generation, PE + ELF writers, prebuilt runtime, and the Vayu native compiler transition from the old QBE+GCC chain to VCB.

### Phase 27 -- VCB native backend correctness and validation

**27 Parts 1–17 -- Done. The WDAC structural fix is complete but remains intermittent for some binaries; signing is removed from the roadmap.** ELF output, Linux runtime expansion, PE correctness, relocations/ASLR, unwind metadata, kernel32 heap APIs, Linux float printing, PE layout corrections, WDAC diagnosis/signing tools, WDAC supplemental-policy generation, shared x86-64 emitter extraction, the 11-program PE matrix, runtime unwind metadata, and the Linux ELF matrix.

**27 Parts 1–17 -- Done.** The completed phase covered PE import-table work, Linux ELF support, runtime metadata, and target validation.

### Completed VCB phases 28–31.5

Phase 28 completed relocatable COFF/ELF object emission, external-linker dispatch, native compiler integration, and VcbLower validation. Phases 29 and 30 completed their planned backend additions. Phases 31.1–31.5 completed tuple, set, slicing, and the listed string and collection operations.

### Phase 31.6 -- String methods and predicates

- **31.6a, current drop:** `str.is_digit`, `str.is_alpha`, `str.is_space`, `str.is_lower`, `str.is_upper`, `str.is_alnum`, and `str.is_ascii`.
- **31.6b, current drop:** `str.ljust(width[, fill])`, `str.rjust`, `str.center`, and `str.zfill(width)`.
- **31.6c, next drop:** `str.strip(chars)`, `str.lstrip([chars])`, and `str.rstrip([chars])`.


### Phase 19–21 -- GUI, 2D Graphics and 3D Raster

The current tooling layer is now a major part of the repository. It includes:

- `vls` -- Vayu Language Server
- `vfmt` -- Vayu formatter
- `vlint` -- Vayu linter
- VS Code extension for `.vyu`
- diagnostics, hover, completion and go-to-definition
- references, rename and signature help
- formatter and linter integration
- compiler `--opt` and runtime-emission tooling

The official VS Code extension is published as `Fliczo.vayu`.

**Marketplace:** https://marketplace.visualstudio.com/items?itemName=Fliczo.vayu

**Direct install:** `vscode:extension/Fliczo.vayu`

### Phase 19–21 -- GUI, 2D Graphics and 3D Raster

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

## ⚙️ VCB -- Vayu Compiler Backend

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

**Phases 1–24 are complete, Phase 25 is skipped for now, and VCB Phases 26–31.5 are complete. Phase 31.6a and 31.6b are the current drop; 31.6c is next.** Phase 15 delivered raw pointer/reference semantics, C FFI, external linking, function-pointer support, supported FFI struct wrapping, and `malloc`/`free`. Phase 16 established native CPython integration with primitive value bridging, Python import/attribute/call support, and reference-count handling. Phase 17 advanced the compiler toward self-hosting. Phase 18 added the LSP, formatter, linter, VS Code extension, expanded LSP capabilities, and related compiler/runtime tooling.

## Current Direction

The project is moving from broad core-language/runtime capability toward a complete development ecosystem. The immediate areas are compiler/bootstrap stabilization, self-hosting, memory/resource correctness, native backend evolution, standard-library growth, package tooling, concurrency, and continued developer-tooling improvements.

The project has progressed through the GUI/event/widget layer, 2D graphics foundation, software-raster/3D pipeline, package ecosystem, native floats, and the VCB transition. Current VCB roadmap work is Phase 31.6a and 31.6b, with character-based stripping next.

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

VCB is a companion backend project, not a replacement for the Vayu frontend. Its native path supports Windows PE and Linux ELF targets; VCB roadmap work is complete through Phase 31.5; Phase 31.6a and 31.6b are the current drop, with 31.6c next.

## Developer Ecosystem

The current editor-facing ecosystem consists of the compiler plus `vls`, `vfmt`, `vlint`, and the official VS Code extension. This establishes the tooling foundation required before the later GUI, AI, package, and native-backend phases can become a cohesive development platform.
