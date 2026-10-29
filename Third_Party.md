# Third-Party Software and Files

Vayu does not bundle a foreign-language runtime, DLL directory, LLD executables, platform import libraries, or an external compiler backend. These are not part of the intended Integrator toolchain.

## Development and test dependencies

The core Vayu compiler, Integrator, and future `.vye` runtime must not depend on Python, NumPy, or the ONNX Python package. The removed ONNX fixture generator and generated fixture are not part of the repository workflow.

Developers may use their own external tools for unrelated local tasks, but such tools must not become required runtime dependencies without an explicit design change, documentation, and tests.

## Ownership and licensing

Vayu source code is governed by the repository's `LICENSE`. Third-party packages retain their own copyrights and licenses. Before distributing any future external tool or library, verify its exact provenance, license, and redistribution requirements.

This document records the intended dependency boundary. It is not a legal opinion or a complete software composition analysis.

## Copyright and trademarks

Third-party copyrights and trademarks remain with their respective owners. Product names are used only to identify software. No affiliation with or endorsement by upstream projects is implied.
