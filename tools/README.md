# Vayu Tools Directory

This directory is intended to contain companion executables, Vayu-source utilities, example-generation helpers, and the embedded CPython runtime used by the Windows development setup. This guide documents the required local layout; it does not assert that every listed binary is currently tracked in GitHub.

## Expected layout

    tools/
      vcb.exe                 VCB backend executable
      lld-link.exe            COFF linker for the PE target
      ld.lld.exe              ELF linker for the ELF target
      kernel32.lib            Windows API import library for PE linking
      nva.vyu                 Vayu-source package manager
      nva-server.vyu          Reference read-only NVA registry server
      make_tiny_onnx.py       Generates examples/assets/tiny_mlp.onnx
      python313.dll           CPython runtime DLL
      python313._pth          Embedded Python path configuration
      python313.zip           CPython standard-library archive
      Lib.zip                 Additional Python library archive used by nva
      DLLs/                   CPython extension modules and their DLL dependencies
      vcruntime140.dll        Microsoft Visual C++ runtime dependency
      vcruntime140_1.dll      Microsoft Visual C++ runtime dependency

The exact directory contents can vary as the toolchain evolves. The PE linker, ELF linker, and Windows import library are required companion files for the corresponding VCB linking workflows. At the time of this update, the GitHub tools listing did not contain `lld-link.exe`, `ld.lld.exe`, or `kernel32.lib`; the local files still need to be committed or included through the release packaging process. Do not infer that a required file is present merely because it is listed here; check the repository tree and local distribution before building a release.

## VCB linking

Run VCB from the Vayu repository root or ensure its companion tools are discoverable in the expected location.

- PE target: `vcb link --target pe` requires `lld-link.exe` and `kernel32.lib`.
- ELF target: `vcb link --target elf` requires `ld.lld.exe`.

Keep the VCB executable and its linker tools from a compatible build/distribution. Avoid mixing arbitrary LLVM versions or copying linker DLLs from unrelated installations. Consult the [VCB repository](https://github.com/NotY215/VCB) for backend build details.

## NVA package manager and registry server

`nva.vyu` is the Vayu-source package manager. It includes command flows for project setup, package add/remove/list, local and remote registry configuration, install/update, search/publish, Git dependencies, and dependency-tree inspection. Its behavior and manifest format remain under active development.

`nva-server.vyu` is a reference read-only registry server. It serves an index and package files over HTTP. Its `POST /publish` endpoint intentionally returns HTTP 501; publish packages using the client's supported publishing workflow and registry implementation instead. Treat this server as a protocol/reference implementation, not as a hardened public production service.

Compile these `.vyu` files using a Vayu compiler build that supports their imported modules and runtime APIs.

## ONNX example helper

`make_tiny_onnx.py` creates `examples/assets/tiny_mlp.onnx`. It requires Python, NumPy, and ONNX:

    python -m pip install numpy onnx
    python tools/make_tiny_onnx.py

Run the command from the repository root. The generated file is an example asset, not a replacement for the generator source.

## CPython bridge runtime

The `python313.dll`, `python313.zip`, `Lib.zip`, `DLLs/`, and Visual C++ runtime files form a coordinated embedded-Python environment for native `py_*` bridge builtins. Keep these files compatible with the CPython build used by the compiler. Do not update only one DLL or extension module independently.

The current inventory includes extension modules for standard-library integrations such as SSL, SQLite, ctypes, compression, sockets, and Tcl/Tk. Some `_test*` extensions may be present in the Python distribution; remove them only after confirming they are not required by tests or packaging.

## Licensing and provenance

See [Third_Party.md](../Third_Party.md) before redistribution. Record the exact source/version of VCB, LLVM/LLD, Python, native extensions, import libraries, and Microsoft runtime files. Keep applicable license texts and notices with releases. The current inventory is not a binary-by-binary provenance audit and should not be treated as legal clearance.
