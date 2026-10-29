# Third-Party Software and Files

This inventory documents third-party software and externally maintained runtime files currently visible in the Vayu repository. It is based on repository paths and project configuration, not a binary-by-binary provenance or license audit. Where the exact upstream build or archive contents have not been verified, that limitation is stated rather than guessed.

## Ownership and scope

Vayu's original source code is governed by the license in the repository's `LICENSE` file. That license does not replace or override the licenses that apply to separately distributed third-party components.

NotY215 does not claim ownership of third-party software, source code, binaries, libraries, icons, or trademarks. The original rights holders retain their rights. Listing a component here is attribution, not a claim of ownership or endorsement.

## Components present in the repository

| Component | Repository paths | Version / provenance | License and required review |
|---|---|---|---|
| OpenSSL | `tools/DLLs/libcrypto-3.dll`, `tools/DLLs/libssl-3.dll` | OpenSSL 3 family, based on filenames; exact version unverified. | OpenSSL 3 uses Apache License 2.0. Include the applicable license and notices. |
| SQLite | `tools/DLLs/sqlite3.dll`, used with `tools/DLLs/_sqlite3.pyd` | Exact SQLite version unverified. | SQLite is generally placed in the public domain by its authors; confirm the provenance and notices of the particular binary distributed. |
| libffi | `tools/DLLs/libffi-8.dll` | libffi ABI family 8, based on filename; exact release unverified. | Review and include the license notice for the exact libffi build. |
| zlib | `tools/DLLs/zlib1.dll` | Exact version unverified. | zlib license. Preserve the applicable copyright and license notice. |
| Tcl/Tk | `tools/DLLs/tcl86t.dll`, `tools/DLLs/tk86t.dll`, `tools/DLLs/_tkinter.pyd` | Tcl/Tk 8.6 family, based on filenames; exact release unverified. | Tcl/Tk license terms and notices apply. Verify the license accompanying the exact build. |
| Microsoft Visual C++ runtime | `tools/vcruntime140.dll`, `tools/vcruntime140_1.dll` | Microsoft Visual C++ runtime; exact redistributable package/build unverified. | Microsoft Visual Studio / Visual C++ Redistributable terms govern redistribution. Verify these files came from an authorized redistributable package and that its terms permit the intended distribution. |
| LLVM/LLD linker tools | Optional platform-linker tools, if included in a distribution | Potentially usable behind a future VIB-owned linker adapter; exact distribution/version must be verified from the binaries. | LLVM components are generally Apache License 2.0 with LLVM exceptions. Ship matching license/notice files and verify the exact distribution and any bundled runtime dependencies. |
| Windows import library | Optional platform import library, if included | May be required by a future PE linking configuration; exact source package and provenance must be recorded. | Verify the library's origin and redistribution terms before packaging. Do not assume its license from the filename alone. |
| NVA package manager | `tools/nva.vyu` | Vayu source maintained in this repository. | Covered by the Vayu repository license, subject to any dependencies it invokes. |
| NVA reference registry server | `tools/nva-server.vyu` | Vayu source for a read-only package registry reference server. | Covered by the Vayu repository license, subject to any dependencies it invokes. |
| ONNX example generator | `tools/make_tiny_onnx.py` | Python helper using NumPy and the `onnx` package to generate `examples/assets/tiny_mlp.onnx`. | The helper is project source. NumPy and ONNX are external dependencies when running it; comply with their respective licenses. The generated model should be tracked separately from the generator. |

## Official license and project references

- OpenSSL license: https://www.openssl.org/source/license.html
- SQLite copyright and public-domain statement: https://www.sqlite.org/copyright.html
- libffi source and licensing: https://github.com/libffi/libffi
- zlib license and source: https://zlib.net/
- Tcl/Tk source and license information: https://www.tcl-lang.org/
- Microsoft Visual C++ Redistributable and deployment guidance: https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist

These references explain upstream licensing; they do not prove that every binary currently in the repository came from the referenced official package. The linker and import-library entries above describe the expected toolchain layout; verify the actual tracked files and their provenance before making a release. Keep the exact upstream license files and notices associated with the package actually used.

## Redistribution checklist

Foreign-language runtimes are not part of Vayu's supported runtime surface. Audit and remove any legacy bundled runtime files that are no longer referenced by the compiler or development workflow.

Before publishing releases or distributing installers:

5. Retain the applicable notices for OpenSSL, libffi, zlib, Tcl/Tk, and any other bundled component. Verify the exact SQLite and Microsoft runtime distribution terms as well.
6. Remove components that Vayu does not need, especially test-only extensions, if doing so is compatible with the build and runtime.
7. Keep this document and a copy of the relevant license texts with the release package. Update the inventory whenever bundled binaries change.

## Important limitation

This document is a repository inventory, not a legal opinion or a completed software composition analysis. The exact binary origins, archive contents, and transitive dependencies must be verified before claiming that every file is cleared for redistribution.

## Copyright and trademarks

All third-party copyrights and trademarks remain with their respective owners. Product names are used only to identify components. No affiliation with or endorsement by OpenSSL, SQLite, Tcl/Tk, Microsoft, or other upstream projects is implied.
