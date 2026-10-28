# Vayu Compiler Data Flow

This document describes the intended VIB / Integrator architecture. It is a target design; the VIB native code generator and verified VIR are not yet complete.

## Target compilation flow

```text
Vayu source (.vyu)
        |
        v
Lexer -> Parser -> AST -> Semantic / Type Analysis
        |
        v
Verified Vayu IR (VIR)
        |
        +------------------+-------------------+
        |                  |                   |
        v                  v                   v
Tree interpreter       Bytecode compiler   Native lowering
        |                  |                   |
        v                  v                   v
Interpreter          Bytecode VM         Native codegen
                                                |
                                                v
                                    Object / executable emitter
                                                |
                                    Optional platform linker
                                                |
                                                v
                                       Artifact + metadata
```

## VIB responsibilities

VIB integrates the frontend with a Vayu-owned typed IR and verifier, interpreter/VM semantics, native code generation, runtime helpers, target-specific object/executable emission, optional platform linking, cache, incremental dependency graph, and execution dispatcher.

## Current transition state

- The existing tree-walking interpreter and bytecode/VM routes remain the supported execution paths on this transition branch.
- The old external-backend lowering and invocation path has been removed from the active native compiler.
- Native compilation currently reports that VIB native code generation is not implemented yet.
- The next engineering milestones are specified in [Roadmap.md](../Roadmap.md), beginning with baseline/conformance work and the VIB IR contract.
