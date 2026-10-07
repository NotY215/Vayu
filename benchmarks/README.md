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

This is the implemented control suite. Phase 25 is currently skipped, so the larger benchmark expansion is postponed until after the VCB transition.

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

Native benchmarking should use the VCB backend and record the VCB revision and target used. The older QBE + GCC path is historical and should not be mixed with current VCB measurements.

## Roadmap

Phase 25 is skipped for now.

**Phase 28 — Next:** object emission and external linking. Benchmark expansion moves to Phase 39.

Phase 39 will compare Vayu tree-walk, VM, and native execution against C++, Python, and Java.