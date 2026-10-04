# Vayu + VCB Data Flow

This document describes the native compilation path currently implemented across the Vayu and VCB repositories.

## Complete Native Flow

```mermaid
flowchart TD
    S["Vayu Source (.vyu)"] --> L["Lexer"]
    L --> P["Parser"]
    P --> A["AST"]
    A --> T["Semantic / Type Checking"]
    T --> C["Compiler Pipeline"]
    C --> N["NativeCompiler"]
    N --> V["VcbLower"]
    V --> I["VCBIR (.vcbir)"]
    I --> D["VCB Driver"]
    D --> R["VCB IR Parser"]
    R --> M["VCB Module"]
    M --> X["x86-64 Code Generation"]
    M --> RT["Runtime Emitters"]
    X --> PE["PE Writer"]
    X --> ELF["ELF Writer"]
    RT --> PE
    RT --> ELF
    PE --> W["Windows PE Executable"]
    ELF --> LNX["Linux x86-64 ELF Executable"]
```

## Repository Boundary

Vayu owns the language frontend and the Vayu-to-VCBIR lowering layer. VCB owns the backend that parses VCBIR, generates x86-64 machine code, emits runtime helpers, and writes PE or ELF images.

```mermaid
flowchart LR
    subgraph VAYU["NotY215/Vayu"]
        F["Lexer / Parser / AST"]
        SA["Semantic Analysis"]
        CP["Compiler Pipeline"]
        VL["VcbLower"]
        NC["NativeCompiler"]
        F --> SA --> CP --> VL --> NC
    end

    subgraph VCB["NotY215/VCB"]
        VP["VCBIR Parser"]
        IR["VCB IR"]
        CG["x86-64 Codegen"]
        RR["Runtime Emitters"]
        PW["PE / ELF Writers"]
        VP --> IR
        IR --> CG
        IR --> RR
        CG --> PW
        RR --> PW
    end

    NC -->|"VCBIR file"| VP
```

## Vayu Frontend to VCB

The native compiler lowers the already parsed Vayu program into VCBIR, writes a temporary .vcbir file, and invokes VCB to build the final executable.

```mermaid
flowchart LR
    AST["Checked Vayu Program"] --> NC["NativeCompiler"]
    NC --> VL["VcbLower"]
    VL --> IR["VCBIR"]
    IR --> VCB["vcb build"]
    VCB --> EXE["Native Executable"]
```

The current native implementation uses `compiler/native/NativeCompiler.cpp` and `compiler/native/VcbLower.cpp`.

NativeCompiler locates VCB, writes the generated IR, invokes vcb build, and can run the resulting executable. The VAYU_VCB environment variable can override the VCB executable path.

## VcbLower

VcbLower converts supported Vayu expressions, functions, control flow, collections, strings, and runtime operations into VCBIR.

```mermaid
flowchart TD
    E["Vayu AST construct"]
    E --> I["Integer / Boolean"]
    E --> F["Float"]
    E --> S["String"]
    E --> LS["List"]
    E --> MP["Map"]
    E --> OP["Arithmetic / Comparison / Bitwise"]
    E --> CF["Control Flow"]
    E --> CALL["Function / Runtime Call"]

    I --> IR["VCBIR operations"]
    F --> IR
    S --> IR
    LS --> IR
    MP --> IR
    OP --> IR
    CF --> IR
    CALL --> IR
```

Collection operations use VCB runtime functions such as `vayu_list_new`, `vayu_list_push`, `vayu_map_new`, and `vayu_map_put`.

## VCB Backend

VCB parses the .vcbir input into its internal module representation and then performs native code generation.

```mermaid
flowchart TD
    FILE[".vcbir"] --> PARSER["src/ir/Parser.cpp"]
    PARSER --> IR["VCB Module / IR"]
    IR --> CG["x86-64 Code Generation"]
    IR --> RT["Runtime Dependency Analysis"]
    CG --> TEXT["Machine-code .text"]
    RT --> RTEXT["Embedded runtime .text"]
    CG --> DATA["Read-only data"]
    TEXT --> TARGET{"Target"}
    RTEXT --> TARGET
    DATA --> TARGET
    TARGET --> PE["PE Writer"]
    TARGET --> ELF["ELF Writer"]
    PE --> WIN["Windows executable"]
    ELF --> LIN["Linux executable"]
```

The backend currently handles integer and floating-point operations, comparisons, loads/stores, stack allocation, branches, jumps, phi handling, calls, returns, string constants, runtime dependency emission, and target-specific executable writing.

## Runtime Flow

VCB embeds required runtime helpers directly into the generated native image. Runtime emission is demand-driven.

```mermaid
flowchart LR
    CALL["VCBIR runtime call"] --> DEP["Runtime dependency closure"]
    DEP --> EMIT["Runtime emitter"]
    EMIT --> CODE["Embedded machine code"]
    CODE --> EXE["Native executable"]
```

Current runtime areas include printing, process exit, Linux syscall support, Linux brk allocation, lists, maps, string operations, collection printing, and Windows runtime/import support.

## String Flow

```mermaid
flowchart LR
    S["Vayu string literal"] --> VL["VcbLower"]
    VL --> I["VCBIR const.str"]
    I --> CG["x86-64 codegen"]
    CG --> RD["Read-only data"]
    CG --> REF["RIP-relative reference"]
    RD --> EXE["Executable"]
    REF --> EXE
```

## Collection Flow

```mermaid
flowchart LR
    V["Vayu collection operation"] --> VL["VcbLower"]
    VL --> I["VCBIR runtime call"]
    I --> R["VCB runtime emitter"]
    R --> H["Native collection storage"]
```

## Control-Flow Flow

```mermaid
flowchart TD
    FN["Vayu function"] --> B["VCBIR basic blocks"]
    B --> OPS["VCBIR operations"]
    OPS --> EM["x86-64 emitter"]
    EM --> FIX["Block and call fixups"]
    FIX --> MC["Final machine code"]
```

Branches and jumps are represented in VCBIR first. The x86-64 emitter resolves relative offsets after code layout.

## Windows PE Path

```mermaid
flowchart TD
    IR["VCBIR"] --> CG["x86-64 PE codegen"]
    RT["Windows runtime / imports"] --> CG
    CG --> TEXT[".text"]
    CG --> DATA[".rdata"]
    RT --> IDATA[".idata"]
    TEXT --> W["PE Writer"]
    DATA --> W
    IDATA --> W
    UNW[".pdata / .xdata from unwindEntries"] --> W
    W --> EXE["Windows PE executable"]
```

The vcb headers command inspects PE structure and reports validation errors and warnings.

## Linux ELF Path

```mermaid
flowchart TD
    IR["VCBIR"] --> CG["x86-64 ELF codegen"]
    RT["Linux syscall runtime"] --> CG
    CG --> TEXT[".text"]
    CG --> DATA["Read-only data"]
    TEXT --> W["ELF Writer"]
    DATA --> W
    TEST["tests\\elf_matrix.ps1"] --> W
    W --> EXE["Linux x86-64 ELF executable"]
```

The Linux entry code calls main, transfers its result to the Linux exit-code register, and invokes the Linux exit syscall. The current ELF matrix validates the generated Linux path; Linux `.eh_frame` support is the next unwind-related target.

## Inspection and Debug Flow

```mermaid
flowchart LR
    V["Vayu source"] --> L["VcbLower"]
    L --> IR[".vcbir"]
    IR --> D["vcb dump"]
    IR --> B["vcb build"]
    B --> PH["vcb headers"]
    B --> EH["vcb elfheaders"]
```

This boundary makes it possible to distinguish Vayu lowering problems from VCB backend problems.

## Current Architecture

```text
Vayu
  Lexer -> Parser -> AST -> Semantic Analysis -> VcbLower
                                                |
                                                v
                                             VCBIR
                                                |
                                                v
VCB
  Parser -> IR -> x86-64 Codegen + Runtime -> PE / ELF Writer
```

The .vcbir representation is the contract between the two repositories: Vayu owns language semantics and lowering, while VCB owns native backend generation and executable writing.


## Phase 27 Backend Status

Phase 27 Parts 1–16 are complete. The current VCB backend includes:

- Windows PE emission and validation
- Linux x86-64 ELF emission and the ELF test matrix
- Runtime `.pdata` / `.xdata` coverage through the shared unwind-entry collection
- `X64Common.hpp` as the shared x86-64 emitter layer
- VCB signing and verification commands
- WDAC supplemental-policy tooling
- Linux runtime support for syscalls, heap allocation, lists, maps, strings, and float printing

The next milestone is Phase 27 Part 17:

```text
Import construction
      ↓
move into writePe
      ↓
remove kIdataRva hardcode

Linux unwind path
      ↓
add .eh_frame
```
