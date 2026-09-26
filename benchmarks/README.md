# Vayu benchmarks

Cross-language comparison for the same small programs, one folder per
language:

    benchmarks/
      run.ps1            builds everything, runs 5x, prints a table
      vayu/              .vyu sources
      cpp/               .cpp sources
      java/              .java sources
      python/            .py sources

## Programs

    arith     integer + float arithmetic in two hot loops
    fib       naive recursive fib (function-call overhead)
    startup   empty program (process launch + runtime boot)

## Running

From a Windows shell at the repo root:

    powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1

The runner auto-detects `g++`, `javac`, `java`, and `python` on PATH.
Anything missing is skipped; the column is just absent from the table.
`vayuc.exe` is located at `build\x64-debug\bin\vayuc.exe` relative to
the repo root, or on PATH.

## Methodology

  - 5 runs per program per language; the *best* (minimum) wall-clock
    time is reported.  Best-of-N is the standard for AOT-compiled
    languages where a warm disk cache is the norm; it also avoids the
    JIT-warmup penalty that would unfairly penalise Java.
  - C++ is built with `g++ -O3 -march=native` (no debug symbols).
  - Java is JDK 17 or newer, default JIT, no `-Xint`/`-Xcomp` override.
  - Python is CPython 3.12 or newer (no PyPy, no `-O`).
  - Vayu native is built via `vayuc --native-out` — once, before the
    timing loop.  Vayu VM and tree-walk are invoked through `vayuc`
    each run, so they include the frontend cost.
  - GCC optimisation level for Vayu native is the compiler's default
    (`--opt 2`).  Pass `--opt 3` and rebuild to compare.

## Reading the table

`vayu-native` is the number that matters for comparing Vayu-the-compiler
against C++/Java/Python.  `vayu-vm` and `vayu-tree` are there to show
the interpreter-vs-AOT gap on identical source — that's Vayu's three-tier
story.

Tree-walk is intentionally absent from `fib` in the default runner
because it takes several minutes at fib(28).  Pass `-IncludeSlow` to add
it.

## Adding a program

Drop `name.vyu`, `name.cpp`, `Name.java`, and `name.py` in the four
folders, then add `name` to the `$programs` array in `run.ps1`.  Keep
output identical across languages — the runner does not compare
outputs, but a mismatch means you're benchmarking different work.

## What this is not

It is not a language shootout.  The programs are short and self-
contained.  They measure specific things (arithmetic throughput,
call overhead, process launch) and their absolute numbers are shaped
by the machine you run them on.  What matters is the ratio between
languages on the same machine, averaged over a few runs.
