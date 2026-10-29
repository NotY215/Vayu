# Vayu examples

The examples folder is organized around runnable feature suites. Files that test a unique subsystem or backend remain separate; overlapping language-feature demos have been combined to reduce clutter without dropping their code coverage.

## Getting started

- [`hello.vyu`](hello.vyu): printing, string input, variables, integer input, arithmetic, and string concatenation.
- [`lang_features.vyu`](lang_features.vyu): namespaces, constants, enums, type inference and promotion, pattern matching, context managers, and deferred actions.
- [`stdlib.vyu`](stdlib.vyu): string processing, character conversion, list/string utilities, math functions, and common standard-library operations.
- [`collections.vyu`](collections.vyu): lists and list methods, maps and map methods, sets, tuples, slicing, tuple unpacking, string methods, enumerate, zip, loops, and collection functions.
- [`classes.vyu`](classes.vyu): classes, inheritance, overriding, static members, attribute inspection and mutation, `setattr`/`delattr`, reflection helpers, and instance methods.
- [`generics.vyu`](generics.vyu): generic functions and typed collection examples.
- [`generics_classes.vyu`](generics_classes.vyu): generic classes, multiple type parameters, and constrained generic functions.
- [`generics_advanced.vyu`](generics_advanced.vyu): advanced generic inference and type-bound examples.
- [`generators.vyu`](generators.vyu): countdown and Fibonacci generators, iteration, explicit `next()`, cleanup, and VM-focused generator coverage.
- [`combinatorics.vyu`](combinatorics.vyu): combinations, permutations, integer square roots, factorials, and recursive Fibonacci.
- [`vm_flow.vyu`](vm_flow.vyu): conditional branches, loops, ranges, nested loops, and break behavior.
- [`native_arith.vyu`](native_arith.vyu), [`vm_arith.vyu`](vm_arith.vyu): arithmetic and expression behavior for their respective compiler paths.
- [`native_io.vyu`](native_io.vyu): input, file operations, string processing, and command-line arguments.

## Why some files remain separate

GUI and raster examples demonstrate distinct visual features and require native-only support. FFI, filesystem, networking, threading, tensor, CUDA/DirectML, compiler-tool, self-hosting, and package-layout examples also remain separate because they exercise distinct subsystems, have special prerequisites, or need to remain individually diagnosable in conformance tests. Backend-specific `native_*` and `vm_*` examples are not merged when keeping the execution path explicit is useful for diagnosing regressions.

## Running an example

From the repository root:

```powershell
& ".\\build\\x64-Release\\bin\\vayuc.exe" examples\\hello.vyu
```

The cross-backend harness is available at [`tests/run_all.ps1`](../tests/run_all.ps1). Some examples are intentionally skipped or require native-only capabilities still tracked in the VIB roadmap.
