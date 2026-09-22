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

The current development history places **Phase 17 as completed and Phase 18 as the current/most recently completed developer-tooling phase**. Phase 18 introduced the Vayu Language Server, formatter, linter, VS Code integration, additional LSP capabilities, compiler optimization/runtime options, and related tooling infrastructure.

The C++ `vayuc` compiler remains the bootstrap/reference compiler while the project continues toward self-hosting. The long-term native-backend direction is being developed separately through VCB.

## Roadmap

The roadmap below records the project's planned development phases. A phase may contain work that is implemented incrementally across multiple commits.

| Phase | Scope |
|---:|---|
| **1** | Core lexer, parser, AST, expressions, variables and indentation-based language structure. |
| **2** | Conditions, loops, `range()`, `break`, `continue`, typed functions, return values and recursion. |
| **3** | Static type checking, inference, primitive types, generic collections, lists, maps and indexing. |
| **4** | Lambdas, closures, higher-order functions, structs, classes, constructors, methods, inheritance, overriding and `super()`. |
| **5** | Runtime behavior, exception handling, nested handlers, re-raising and the module system. |
| **6** | Peephole optimization on emitted IL to collapse redundant comparison chains. |
| **7** | Rewrite `vayuc` in Vayu itself, then bootstrap the compiler so Vayu can compile its own compiler. |
| **8** | Stack-allocate instances whose address never escapes a function, closing the remaining OOP performance gap. |
| **9** | `nva` · `nova.toml` · `nova.lock` · dependency resolver · registry. |
| **10** | `regex` · `thread` · `net` · `crypto` · `random` · `os` · math extras. |
| **11** | `with` · `match/case` · `enum` · `interface` · `namespace` · `const` · `static` · `defer` · `yield` · `is / is_not` · bitwise · compound assignment · visibility. |
| **12** | `unique<T>` · `shared<T>` · `weak<T>` · move semantics · opt-in borrow checking. |
| **13** | `repr` · `hash` · `id` · `isinstance` · `enumerate` · `zip` · `reversed` · `round` · `pow` · `divmod` · `sign` · `gcd` · `lcm` · `clamp`. |
| **14** | String, list, map, set, tuple, math, file/OS and functional helpers. |
| **15** | `extern` blocks · `dlopen` / `LoadLibrary` · struct layout · variadic calls. |
| **16** | CPython embedding · `import py "…"` · Vayu `Value` ↔ `PyObject` and expanded Python interoperability. |
| **17** | Self-hosting/compiler-bootstrap work: bring the Vayu implementation toward the C++ `vayuc` feature set and stabilize the bootstrap path. **Completed development phase; the C++ compiler remains the current reference/bootstrap implementation.** |
| **18** | LSP · formatter · linter · VS Code extension · references · rename · signature help · compiler/runtime tooling improvements. **Current/most recently completed tooling phase.** |
| **19** | Window / event / widget layer. |
| **20** | 2D first, then 3D graphics and rendering. |
| **21** | Tensors · autodiff · ONNX · CUDA/GPU AI infrastructure. |
| **22** | Package ecosystem: `vayu install` · `vypy install` · public index · dependency resolution · lockfiles · publishing. |
| **23** | **Single-binary native toolchain:** retire the existing QBE/GCC backend chain and move toward VCB emitting native machine code directly; Windows-first, then ELF/Mach-O targets. |

### Phase 18 — Developer Tooling

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

### Self-Hosting Direction

The immediate compiler objective remains self-hosting rather than rewriting the project from scratch. The existing C++ `vayuc` remains the bootstrap/reference compiler while the Vayu implementation is developed toward feature parity. Once Vayu can reliably compile the compiler and the bootstrap process is stable, the long-term objective is to remove dependence on the C++ bootstrap implementation.

### Later Ecosystem Direction

After self-hosting, the roadmap continues through developer tooling, GUI/window/event/widget APIs, 2D and then 3D graphics, AI/ML infrastructure, and the package ecosystem. These later phases depend on a stable compiler, runtime, standard library, interoperability layer, and tooling foundation.

## ⚙️ VCB — Vayu Compiler Backend

<a href="https://github.com/NotY215/VCB">
  <img src="https://raw.githubusercontent.com/NotY215/VCB/master/Assets/VCB_logo.png" alt="VCB Logo" width="160">
</a>

**[VCB](https://github.com/NotY215/VCB) is a companion project and planned native backend component of the Vayu compiler ecosystem.**

VCB is designed to provide a clean boundary between Vayu's frontend/IR and machine-code generation:

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

**Phase 16 is complete, Phase 17 is complete, and Phase 18 is the current/most recently completed tooling phase.** Phase 15 delivered raw pointer/reference semantics, C FFI, external linking, function-pointer support, supported FFI struct wrapping, and `malloc`/`free`. Phase 16 established native CPython integration with primitive value bridging, Python import/attribute/call support, and reference-count handling. Phase 17 advanced the compiler toward self-hosting. Phase 18 added the LSP, formatter, linter, VS Code extension, expanded LSP capabilities, and related compiler/runtime tooling.

## Current Direction

The project is moving from broad core-language/runtime capability toward a complete development ecosystem. The immediate areas are compiler/bootstrap stabilization, self-hosting, memory/resource correctness, native backend evolution, standard-library growth, package tooling, concurrency, and continued developer-tooling improvements.

Longer-term work expands into GUI/event/widget APIs, graphics, scientific computing, AI/ML, package distribution, and the VCB-based native toolchain.

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
