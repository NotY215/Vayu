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

Vayu now has native C/FFI and CPython interoperability foundations that are directly relevant to AI/ML integration. A complete native tensor/ML ecosystem is still a roadmap area.

For the latest roadmap and project updates, visit the [official Vayu website](https://vayu.gt.tc).


## Current Native Python Foundation

The native `py` module currently supports `py.init()`, `py.version()`, `py.run()`, `py.exec()`, primitive value bridging, `py.import()`, `py.getattr()`, `py.call()`, and `py.decref()`. The native compiler dynamically loads a compatible CPython runtime and routes supported operations through the CPython C API.

This is an implemented interoperability foundation, not yet complete NumPy/PyTorch/TensorFlow interoperability. Rich Python objects, buffers, callbacks, tensor exchange, and broader package integration remain future work.

## Current AI-Relevant Low-Level Features

The current language/runtime also provides `ptr<T>`, address-of/dereference, pointer arithmetic, `malloc()`, `free()`, `unique<T>`, `shared<T>`, `weak<T>`, C FFI, external library linking, tuples, sets, slicing, and expanded math/utility built-ins. These are foundations for future numerical and AI libraries.

## Phase Position

Phase 16 is complete. Its expanded CPython bridge strengthens the interoperability foundation for practical access to the existing Python AI ecosystem. The later AI/ML roadmap targets tensors, autodiff, ONNX, and CUDA as dedicated capabilities.
