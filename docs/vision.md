# Vayu Vision & Roadmap

Vayu is an independent programming-language project aiming to combine readable Python-inspired syntax with native performance, low-level control, modern application development, portability, and an AI-ready ecosystem.

> **Write simple. Run native. Control everything.**

## Current Foundation

The current source tree demonstrates a substantially expanded language/compiler foundation including:

- variables and type inference
- explicit type annotations
- primitive and collection types
- functions and recursion
- conditions and loops
- arithmetic, comparison, and logical expressions
- lists and maps
- structs
- classes and inheritance
- lambdas and closures
- exceptions
- modules and imports
- math functionality
- type checking
- AST generation
- VM-oriented execution
- tuples and sets
- Python-style slicing for lists, tuples, and strings
- constants and enums
- static members and visibility modifiers
- bitwise operations
- `match`, `with`, and `defer`
- generators and `yield`
- generic/type parameters and constraints
- ownership/reference types (`unique<T>`, `shared<T>`, `weak<T>`)
- raw pointers/references and pointer arithmetic
- `malloc` / `free`
- C FFI, function pointers, and external library linking
- native CPython integration and Python value/call bridging

The project is being developed incrementally; completed functionality should remain stable while new compiler phases are added.

## Roadmap

### 1. Compiler & Language Stabilization

- strengthen lexer and parser behavior
- expand semantic analysis
- improve type checking
- improve diagnostics and error reporting
- expand regression tests
- stabilize AST and intermediate representations

### 2. Runtime, VM & Native Code Generation

The native backend is now a substantial implemented execution path. Current work continues on stabilization, optimization, portability, and broader backend coverage.

- strengthen bytecode and VM execution
- introduce/expand IR optimization
- native machine-code generation
- linking and executable generation
- runtime performance improvements
- platform-specific backends

### 3. Memory & Resource Management

- ownership/resource-management model
- deterministic cleanup
- smart-pointer concepts such as `unique<T>`, `shared<T>`, and `weak<T>`
- low-level memory operations where appropriate
- safe and explicit escape hatches for systems programming

### 4. Standard Library

- collections
- strings and utilities
- filesystem
- processes
- time/date
- networking
- concurrency primitives
- serialization
- platform abstractions

### 5. Package Ecosystem

- `nva` package workflow
- dependency resolution
- versioning and lock files
- publishing
- native dependencies
- build integration

### 6. Interoperability

C interoperability is now implemented in the native backend, including `extern "C"`, function pointers, external linking, pointers/references, and supported small struct-by-value cases.

Current Python interoperability includes a native CPython loading/calling bridge. Future work is required for richer Python object interoperability and broader C++/Python ecosystem support.

- C interoperability
- C++ interoperability
- Python ecosystem/runtime interoperability
- future JVM integration
- future .NET integration

### 7. Concurrency & Networking

- threads/tasks
- synchronization primitives
- asynchronous execution
- networking APIs
- scalable server/application foundations

### 8. Application, GUI & Graphics

- desktop GUI framework
- cross-platform application APIs
- OpenGL/Vulkan/DirectX integrations
- rendering and graphics utilities
- audio/input/physics foundations for game development

### 9. AI, ML & Scientific Computing

- numerical primitives
- tensor APIs
- GPU acceleration
- CUDA/ROCm integration
- ONNX support
- Python AI ecosystem interoperability
- scientific/data-processing libraries

### 10. Developer Tooling

- formatter
- package/project tooling
- debugger
- language server / LSP
- IDE integrations
- profiling and diagnostics
- documentation tooling

### 11. Metaprogramming & Advanced Language Features

- compile-time capabilities
- metaprogramming
- stronger generics
- advanced type-system features
- safer low-level abstractions

### 12. Self-Hosting

A long-term goal is a mature Vayu compiler that can be substantially implemented in Vayu itself, reducing dependence on the bootstrap implementation and demonstrating the language's own capabilities.

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
