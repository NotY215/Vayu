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
- [`self_host.vyu`](self_host.vyu): consolidated self-hosting language-feature phases, including compound/bitwise operators, literals, enums, collections, exceptions, classes, match statements, and generators.
- [`self_host_native.vyu`](self_host_native.vyu): native-only self-hosting coverage for extern calls, unsafe pointers, and Python interop conversions.
- [`vm_flow.vyu`](vm_flow.vyu): conditional branches, loops, ranges, nested loops, and break behavior.
- [`native_arith.vyu`](native_arith.vyu), [`vm_arith.vyu`](vm_arith.vyu): arithmetic and expression behavior for their respective compiler paths.
- [`native_io.vyu`](native_io.vyu): input, file operations, string processing, and command-line arguments.

## Why some files remain separate

GUI and raster examples demonstrate distinct visual features and require native-only support. FFI, filesystem, networking, threading, tensor, CUDA/DirectML, compiler-tool, and package-layout examples remain separate because they exercise distinct subsystems or need independent diagnostics. Self-hosting phase examples are consolidated into `self_host.vyu` and `self_host_native.vyu`; the native-only suite remains separate because its prerequisites differ. Backend-specific `native_*` and `vm_*` examples are not merged when keeping the execution path explicit is useful for diagnosing regressions.

## Running an example

From the repository root:

```powershell
& ".\\build\\x64-Release\\bin\\vayuc.exe" examples\\hello.vyu
```

Package-manager demos live under `scratch/`, which is the maintained dependency-package example. It includes a versioned `mathlib` package with square, cube, greeting, and circle-area helpers. The older duplicate `demopkg/` tree has been removed.

The cross-backend harness is available at [`tests/run_all.ps1`](../tests/run_all.ps1). Some examples are intentionally skipped or require native-only capabilities still tracked in the VIB roadmap.
