# Vayu AI and Machine Learning

This document describes Vayu's native AI/ML direction. Vayu is its own language and runtime. Python-inspired syntax is a design influence, not Python compatibility. Vayu does not currently embed or execute Python, import Python modules, or promise compatibility with NumPy, PyTorch, TensorFlow, or other foreign-language packages.

## Goals

The long-term AI/ML direction includes:
- tensor and matrix operations implemented for Vayu;
- automatic differentiation;
- neural-network building blocks;
- data loading and transformation;
- computer-vision and image-processing primitives;
- ONNX model support through an explicitly implemented Vayu-native reader/runtime;
- GPU acceleration through documented platform interfaces where implemented;
- performance measurement against representative workloads.

These are goals, not a claim that every subsystem is currently implemented.

## Native-first design

AI features should be exposed through Vayu modules, Vayu types, and documented native APIs. Core functionality must not depend on a Python interpreter or Python package manager. External model formats and native platform libraries may be supported only through dedicated, tested implementations.

The repository does not bundle a Python ONNX generator or generated ONNX fixture. Future ONNX tests should use Vayu-owned fixtures and a Vayu-owned reader/runtime.

## Current implementation status

The VIB native compiler is under implementation. Native feature support must be established feature-by-feature through code and shared conformance tests. Interpreter or VM support does not automatically establish native support.

Tensors, automatic differentiation, ONNX execution, CUDA/GPU execution, and high-level neural-network APIs must each be considered unsupported until their implementations and tests are present. ONNX work must follow the Integrator-first roadmap and must not add a Python/NumPy/ONNX package dependency to Vayu's compiler or runtime. Do not infer support from an example file, planned API, or external dependency.

## Development sequence

1. Establish the VIB IR, verifier, and execution contracts.
2. Implement and test Vayu-native numeric and tensor primitives.
3. Add deterministic tensor operations and shape validation.
4. Add automatic differentiation with numerical-gradient tests.
5. After the Integrator and `.vye` format are stable, implement a Vayu-owned ONNX reader with malformed-file tests.
6. Add optional GPU paths behind capability checks and CPU-equivalence tests.
7. Benchmark compile time, startup, memory, and steady-state execution separately.

## Support policy

The documented support matrix must identify each feature, execution route, platform, and passing test. Features without implementation evidence remain planned. No foreign-language runtime or package compatibility is implied.

For project status, see the [Vayu repository](https://github.com/NotY215/Vayu) and [Roadmap](../Roadmap.md).
