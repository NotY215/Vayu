# Vayu Tools Directory

This directory contains Vayu-source utilities and development helpers. It does not bundle a foreign-language runtime, DLL directory, platform-linker executable, import library, or external compiler backend.

## Current tools

- `nva.vyu`: Vayu-source package manager.
- `nva-server.vyu`: reference read-only registry server.

## Integrator and .vye

VIB (Vayu Integration Backend) implements the Integrator architecture inside the Vayu repository. The first goal is a small, correct subset and a versioned `.vye` Vayu Executable package. A `.vye` package is executed by a compatible Vayu/Integrator runtime; it is not a native OS executable.

The planned `.vye` writer, reader, verifier, and execution path must be validated before the format is described as implemented. See [Roadmap.md](../Roadmap.md), [VIB architecture](../docs/vib.md), and the [VIB technical reference](../docs/vib-documentation.md).

## AI/ML and ONNX

The Python-based ONNX fixture generator and its generated model are not part of the toolchain. Vayu-native ONNX support is deferred until after the Integrator core and `.vye` format are stable. Future tests should use Vayu-owned fixtures and a Vayu-owned ONNX reader/runtime.

## Package manager and registry server

`nva.vyu` includes command flows for project setup, package add/remove/list, local and remote registry configuration, install/update, search/publish, Git dependencies, and dependency-tree inspection. Its behavior and manifest format remain under active development.

`nva-server.vyu` is a reference read-only registry server. Its `POST /publish` endpoint intentionally returns HTTP 501. Treat it as a protocol/reference implementation, not as a hardened public production service.
