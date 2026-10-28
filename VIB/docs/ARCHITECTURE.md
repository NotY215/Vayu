# VIB Architecture Notes

This document is the design index for the Integrator implementation. The canonical phase plan is [../../Roadmap.md](../../Roadmap.md).

## Required components

- **Frontend bridge:** consume Vayu AST and semantic information without creating a second parser.
- **VIR:** typed, versioned, deterministic internal representation.
- **Verifier:** reject malformed or inconsistent IR before execution/code generation.
- **Runtime contract:** shared observable behavior across interpreter, VM, and native mode.
- **Native backend:** code generation and object/executable emission owned by Vayu.
- **Target/link adapter:** isolate platform-specific formats and optional standard linkers.
- **Artifact cache:** content-addressed artifacts, atomic writes, validation and recovery.
- **Incremental graph:** dependency-aware invalidation and clean-build equivalence.
- **Dispatcher:** explicit modes first, capability-aware automatic selection later.
- **Evidence:** conformance tests, reproducible benchmarks, and a prediction register.

## State

Only the API scaffold exists at this stage. VIR, verification, native emission, cache, incremental compilation, and adaptive dispatch remain planned work. Do not treat the diagram or planned interfaces as completed functionality.
