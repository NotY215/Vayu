# Vayu examples

Each top-level `.vyu` file demonstrates a focused language feature or subsystem. Filenames describe the main work performed rather than using numbered names such as `lists2.vyu`.

## Getting started

- [`hello.vyu`](hello.vyu): printing, string input, variables, integer input, arithmetic, and string concatenation.

## Language features

- [`combinatorics.vyu`](combinatorics.vyu): combinations, permutations, integer square roots, and factorials.
- [`list_methods.vyu`](list_methods.vyu): list mutation, lookup, copying, reversing, and sorting.
- [`map_methods.vyu`](map_methods.vyu): map lookup, insertion, removal, merging, copying, and items.
- [`string_methods.vyu`](string_methods.vyu): string casing, padding, searching, partitioning, and character predicates.
- [`vm_flow.vyu`](vm_flow.vyu): conditional branches, loops, ranges, nested loops, and break behavior.
- [`native_arith.vyu`](native_arith.vyu), [`vm_arith.vyu`](vm_arith.vyu): arithmetic and expression behavior for their respective compiler paths.
- [`native_io.vyu`](native_io.vyu): input, file operations, string processing, and command-line arguments.

## Why some similar-looking files remain separate

Files prefixed with `native_` or `vm_` can overlap intentionally: they are backend-specific conformance examples and should remain independently runnable so a failure identifies the affected execution path. GUI, FFI, filesystem, networking, tensor, compiler-tool, self-hosting, and other subsystem fixtures also remain separate because they exercise distinct capabilities or have special execution requirements.

The former `control.vyu` example repeated the basic conditional and while-loop cases already covered by `vm_flow.vyu`, which also demonstrates additional loop forms. Use `vm_flow.vyu` for that combined coverage.

## Running an example

From the repository root, run an individual example with:

```powershell
& ".\\build\\x64-Release\\bin\\vayuc.exe" examples\\hello.vyu
```

The cross-backend harness is available at [`tests/run_all.ps1`](../tests/run_all.ps1). Some examples are intentionally skipped or require native-only capabilities that are still tracked in the VIB roadmap.
