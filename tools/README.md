# Vayu Tools Directory

This directory contains Vayu-source utilities and example-generation helpers. It does not bundle DLL runtimes, platform-linker executables, or import libraries.

## Current and expected contents

- `nva.vyu`: Vayu-source package manager.
- `nva-server.vyu`: reference read-only registry server.
- `make_tiny_onnx.py`: generates `examples/assets/tiny_mlp.onnx`.

This list describes roles, not a guarantee that every file is present in every checkout. Verify the repository tree and release package.

## VIB and native toolchain

VIB (Vayu Integration Backend) is implemented inside the Vayu repository. It owns the planned VIR, verifier, native code generation, runtime integration, cache, incremental build graph, and execution dispatcher. The old external backend executable is no longer part of the active native compiler source.

No `lld-link`, `ld.lld`, or bundled import library is required or included in the current tool layout. Native compilation is temporarily unavailable until VIB's own code generator is implemented; see [Roadmap.md](../Roadmap.md).

## NVA package manager and registry server

`nva.vyu` includes command flows for project setup, package add/remove/list, local and remote registry configuration, install/update, search/publish, Git dependencies, and dependency-tree inspection. Its behavior and manifest format remain under active development.

`nva-server.vyu` is a reference read-only registry server. Its `POST /publish` endpoint intentionally returns HTTP 501. Treat it as a protocol/reference implementation, not as a hardened public production service.

## ONNX example helper

`make_tiny_onnx.py` creates `examples/assets/tiny_mlp.onnx`. It requires Python, NumPy, and ONNX:

```sh
python -m pip install numpy onnx
python tools/make_tiny_onnx.py
```

Run the command from the repository root. The generated file is an example asset, not a replacement for the generator source.

## Licensing and provenance

See [Third_Party.md](../Third_Party.md) for the remaining external dependencies used by development helpers.
