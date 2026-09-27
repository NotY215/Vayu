# Vayu Benchmarks

The benchmark suite compares the current Vayu execution paths with C++, Java, and CPython using the same small workloads.

## Layout

    benchmarks/
      run.ps1
      vayu/
      cpp/
      java/
      python/

## Current programs

    arith     integer + float arithmetic in two hot loops
    fib       naive recursive fib
    startup   empty program / process startup

This is the implemented control suite. The larger compute-heavy and allocation-heavy workloads described in the Phase 25 roadmap are not yet part of this directory.

## Running

From the repository root on Windows:

    powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1

The runner detects `g++`, `javac`, `java`, and `python` on PATH. Missing tools are skipped.

Vayu native compilation uses `vayuc --native-out <path>`. The runner locates `vayuc.exe` under `build\x64-debug\bin\vayuc.exe` or through PATH.

## Vayu execution paths

The benchmark harness reports:

- `vayu-native` — native compilation/execution
- `vayu-vm` — bytecode VM
- `vayu-tree` — tree-walking interpreter

The harness can include the slower tree-walk Fibonacci case with `-IncludeSlow`.

## Methodology

- 5 runs are used for the default measurements.
- The best wall-clock time is reported.
- C++ uses `g++ -O3 -march=native`.
- Java uses the default JIT on JDK 17+.
- Python uses CPython 3.12+.
- Vayu native is compiled before the timed native execution loop.
- Vayu optimization is controlled by `--opt`; the current runner uses the compiler default.

The suite is intended for reproducible comparisons on the same machine, not as a universal language ranking.

## Adding a benchmark

Add matching source files to:

    benchmarks/vayu/
    benchmarks/cpp/
    benchmarks/java/
    benchmarks/python/

Then add the program name to the `$programs` array in `run.ps1`.

Keep the work and output equivalent across languages. The runner measures time but does not prove semantic equivalence.

## Backend note

The compiler supports `--backend qbe|vcb`, but QBE remains the default native backend. VCB is developed separately. Benchmark results should record the backend used so QBE and VCB measurements are not mixed.

## Roadmap

Phase 25 expands this into the full pre-VCB suite. Phase 28 reuses the same workloads after the VCB backend transition and records the before/after delta.