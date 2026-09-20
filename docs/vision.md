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

The latest development history marks **Phase 16 complete**. Phase 17 is the next development direction: bringing the Vayu compiler toward self-hosting and eventual removal of the C++ bootstrap implementation.

## Roadmap

### Phased implementation plan

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
| **17** | `vayu.vyu` supports the compiler's implemented C++ `vayuc` feature set, followed by dropping the C++ bootstrap backend. |
| **18** | LSP · formatter · linter · debugger hooks · VS Code extension. |
| **19** | Window / event / widget layer. |
| **20** | 2D first, then 3D. |
| **21** | Tensors · autodiff · ONNX · CUDA. |
| **22** | `vayu install` · `vypy install` · public index · ecosystem. |

### Phase 17 — Current Direction

The immediate objective is not to rewrite the project from scratch. The existing C++ `vayuc` remains the bootstrap/reference compiler while the Vayu implementation is developed toward feature parity. Once Vayu can reliably compile the compiler and the bootstrap process is stable, the long-term objective is to drop the C++ compiler backend.

### Later Ecosystem Direction

After self-hosting, the roadmap continues through developer tooling, GUI/window/event/widget APIs, 2D and then 3D graphics, AI/ML infrastructure, and the package ecosystem. These later phases depend on a stable compiler, runtime, standard library, interoperability layer, and tooling foundation.

## Benchmarking

Benchmarking is part of the development process. Performance measurements should track representative compiler/runtime workloads as native execution and optimization improve.

Published benchmark information can change as the implementation and test environment change, so the [official Vayu website](https://vayu.gt.tc) is the preferred location for current benchmark information.

## Long-Term Goal

The ultimate goal is not simply "Python but faster" or "C++ with Python syntax." Vayu aims to become a unified ecosystem capable of covering:

**simple programs → applications → AI → games → systems → native software**

while keeping the language approachable and giving developers progressively more control when they need it.


## Current Phase Status

**Phase 15 is complete. Phase 16 is active.** Phase 15 delivered raw pointer/reference semantics, C FFI, external linking, function-pointer support, supported FFI struct wrapping, and `malloc`/`free`. Phase 16 has begun native CPython integration and is expanding it with primitive value bridging, Python import/attribute/call support, reference-count handling, and bundled Python tooling.

## Current Direction

The project is currently moving from broad core-language/runtime capability toward stronger interoperability and ecosystem foundations. The next major areas are compiler/native-backend stabilization, memory/resource correctness, broader Python/C++ interoperability, standard-library growth, package tooling, concurrency, and eventually GUI/graphics/scientific/AI ecosystems.
