# Vayu AI & Machine Learning

Vayu is designed with AI/ML as a first-class long-term ecosystem target while keeping the core language useful for general-purpose programming.

## Goals

The AI/ML roadmap includes:

- numerical computing
- tensor operations
- machine learning
- deep learning
- data processing
- computer vision
- GPU acceleration
- CUDA integration
- ONNX integration
- interoperability with established Python AI ecosystems

## Python Ecosystem Interoperability

Vayu is intended to interoperate with Python where that provides practical value, especially for mature AI, ML, scientific, and data-processing libraries.

Potential ecosystem targets include NumPy, PyTorch, TensorFlow, OpenCV, ONNX, CUDA, and ROCm.

The current source now has a real native CPython bridge, but these higher-level ecosystems are not automatically supported merely because CPython is available.

## Native AI Direction

The long-term goal is to make performance-sensitive AI workloads possible without requiring developers to abandon Vayu for native extensions whenever low-level control is needed.

The planned direction includes:

1. stable native compiler/runtime foundations
2. efficient numerical and memory primitives
3. native library interoperability
4. GPU/runtime integrations
5. higher-level tensor and ML APIs
6. tooling for AI development and deployment

## Current Status

Vayu now has native C/FFI and CPython interoperability foundations that are directly relevant to AI/ML integration. A complete native tensor/ML ecosystem is still a roadmap area; Phase 24.2 is the planned tensor-storage migration from Q16.16 to f32/f64 with a temporary compatibility layer.

For current project status and roadmap information, visit the [official Vayu website](https://vayu.gt.tc) or the [Vayu repository](https://github.com/NotY215/Vayu).


## Current Native Python Foundation

The native `py` module currently supports `py.init()`, `py.version()`, `py.run()`, `py.exec()`, primitive value bridging, `py.import()`, `py.getattr()`, `py.call()`, and `py.decref()`. The native compiler dynamically loads a compatible CPython runtime and routes supported operations through the CPython C API.

This is an implemented interoperability foundation, not yet complete NumPy/PyTorch/TensorFlow interoperability. Rich Python objects, buffers, callbacks, tensor exchange, and broader package integration remain future work.

## Current AI-Relevant Low-Level Features

The current language/runtime also provides `ptr<T>`, address-of/dereference, pointer arithmetic, `malloc()`, `free()`, `unique<T>`, `shared<T>`, `weak<T>`, C FFI, external library linking, tuples, sets, slicing, and expanded math/utility built-ins. These are foundations for future numerical and AI libraries.

## Tooling and Current Integration

Vayu's current developer tooling includes the Vayu Language Server (`vls`), formatter (`vfmt`), linter (`vlint`), and the official VS Code extension. These tools improve the workflow for developing AI-oriented Vayu code, while the actual tensor/ML runtime remains a future ecosystem layer.

The native compiler also supports `--emit-runtime` for exporting the native runtime C source and `--opt <0-3>` for native optimization-level selection.

## Phase Position

**Phases 1–23 are complete development phases and Phase 24 is current.** Phase 16 established the native CPython bridge; Phase 17 advanced the compiler toward self-hosting; Phase 18 added the LSP, formatter, linter, and VS Code integration; Phases 19–21 added GUI/graphics foundations; Phase 22 advanced the package ecosystem; and Phase 23 established the VCB native-backend direction.

Phase 24 now prioritizes native floats before serious benchmark work. The dedicated AI/ML roadmap remains focused on tensors, autodiff, ONNX, CUDA/GPU integration, and higher-level AI libraries. CPython availability does not by itself mean NumPy, PyTorch, TensorFlow, or other Python packages are fully supported.


## Current Implementation Status

The compiler/runtime has progressed through Phase 24.1. The native GUI/raster foundation from Phases 19–21 remains available, Phase 22 established the package ecosystem, Phase 23 established the backend transition boundary, Phase 24.0 added native floats, and Phase 24.1 added native math and float-aware collections.

The current AI/ML implementation is still foundational rather than a complete ML framework. Native C FFI and CPython interoperability exist, while tensors, autodiff, ONNX integration, CUDA/GPU execution, and high-level neural-network APIs remain development work.

These graphics capabilities provide useful infrastructure for future scientific and AI/ML visualization, but they are not an implemented tensor/autodiff/ONNX/CUDA stack.

Phase 24.0/24.1 now provide the numerical foundation needed for later AI work: native float values, mixed int/float operations, conversions, `math.*`, `list<float>`, and `map<str, float>`. The dedicated AI/ML roadmap remains focused on tensors, automatic differentiation, ONNX interoperability, GPU/CUDA acceleration and higher-level AI libraries.

The current native raster API includes framebuffers, depth buffers, Q16.16 matrices, mesh and texture resources, mipmaps, filtering, perspective-correct rasterization, lighting, render-to-texture and post-processing. These are native graphics APIs rather than AI/ML APIs.
