# Third-Party Software and Files

Vayu does not bundle a foreign-language runtime, a DLL directory, LLD executables, or platform import libraries. The removed runtime and linker files are not part of the supported Vayu toolchain.

## Development helper dependencies

| Component | Use | Dependency boundary |
|---|---|---|
| Python | Runs the optional `tools/make_tiny_onnx.py` asset-generation script only | Python is a developer-installed tool for this helper, not a Vayu runtime or language-compatibility layer. |
| NumPy | Used by the optional ONNX asset generator | Install only when running that helper. |
| ONNX | Used by the optional ONNX asset generator | Install only when running that helper. |

To generate the example model, install the helper's dependencies in your own development environment and run:

```sh
python -m pip install numpy onnx
python tools/make_tiny_onnx.py
```

Vayu's compiler and runtime do not depend on these Python packages. The generated ONNX file is an example asset, not a bundled runtime dependency.

## Ownership and licensing

Vayu source code is governed by the repository's `LICENSE`. Third-party packages retain their own copyrights and licenses. Before redistributing generated assets or packaging optional developer tools, review the applicable upstream terms.

This document records the intended dependency boundary. It is not a legal opinion or a complete software composition analysis.

## Copyright and trademarks

Third-party copyrights and trademarks remain with their respective owners. Product names are used only to identify development tools and libraries. No affiliation with or endorsement by their respective projects is implied.
