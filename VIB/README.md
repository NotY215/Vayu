# VIB — Vayu Integration Backend

VIB means **Vayu Integration Backend**. It is the Vayu repository's integrated compiler/runtime system and the implementation home for the **Integrator architecture**.

VIB is not a separate language, required standalone product, or required `vib.exe`. The user-facing entry point remains Vayu's compiler/driver (`vayuc.exe`, distributed as the Vayu compiler executable).

## Responsibility

VIB is intended to own the complete Vayu execution toolchain:

- integration with the existing Vayu lexer, parser, AST, and type checker;
- a Vayu-owned verified intermediate representation (VIR);
- shared semantic rules across interpreter, VM, and native execution;
- native lowering and machine-code/object/executable generation;
- runtime helpers and native ABI boundaries;
- optional platform-linker invocation through a Vayu-owned interface;
- persistent artifact caching and safe cache invalidation;
- dependency-aware incremental compilation;
- explicit execution dispatch and later opt-in profiling/adaptive compilation;
- diagnostics, conformance tests, benchmark reporting, and artifact metadata.

## Architectural rule

The long-term native path must not require a VCB repository, executable, VCBIR, VCB-specific environment variable, or VCB-specific command. Any platform linker retained for object linking is a tool dependency, not the language compiler implementation.

## Current implementation status

This directory establishes the architecture and documentation boundary. It does **not** by itself mean the native compiler, VIR, artifact cache, incremental compiler, or adaptive dispatcher has been implemented. See [`../Roadmap.md`](../Roadmap.md) for the phased implementation and exit criteria, and [`../docs/vib.md`](../docs/vib.md) for the architecture contract.

Do not report a phase as complete until its code, tests, and acceptance criteria exist in the repository. During migration, preserve working interpreter/VM paths and clearly report native-mode limitations instead of silently depending on the legacy backend.

## Intended module layout

The exact filenames can evolve, but responsibilities should remain separated:

| Area | Responsibility |
|---|---|
| `frontend/` | Bridge from the existing Vayu frontend and semantic analysis |
| `ir/` | VIR data model, builder, serializer, and schema version |
| `verify/` | IR validation and target capability checks |
| `vm/` | VM integration and shared semantic behavior |
| `native/` | Native lowering, code generation, register/stack handling |
| `runtime/` | Runtime helpers, ABI contracts, allocation and error behavior |
| `targets/` | Target descriptions, object formats, relocations and executable output |
| `link/` | Optional platform linker discovery, invocation and diagnostics |
| `cache/` | Content-addressed artifact storage and corruption handling |
| `incremental/` | Module dependency graph and invalidation |
| `dispatch/` | Explicit/automatic execution route selection |
| `tests/` | IR verifier, backend parity, cache, incremental and differential tests |
| `docs/` | Architecture decisions, benchmark predictions and implementation notes |

## Engineering rules

1. Correctness and language semantics take precedence over speed.
2. Every backend feature must have tests and explicit capability reporting.
3. Cache keys must include all inputs that can affect generated output.
4. Cache writes must be atomic; invalid/corrupt entries must be rejected.
5. Incremental builds must be equivalent to clean builds.
6. Benchmark claims must include reproducible commands and environment details.
7. No claim of universal superiority over JIT compilers is permitted without workload-specific evidence.
