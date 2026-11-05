# VIB / VIR Implementation Status

**Last updated:** 2026-11-05  
**Scope:** Features lowered and executed on the Integrator path (`vayuc --native`, `--dump-ir`, `--emit-vye` / `--run-vye`).

Interpreter (`--run`) and VM (`--vm`) may support a wider language surface; this document tracks **Integrator/VIR only**.

## Execution model

```text
.vyu → lexer/parser/sema → VIR lowering → verifier → VIR interpreter
```

- Temporaries and named locals share one slot array; temps are allocated **strictly above** the current local watermark so they never clobber variables.
- `print` writes live to stdout (so `input` / `read_int` interleave correctly).
- `.vye` packages round-trip the Phase 3–7 subset.

## Supported on Integrator

| Area | What works |
|------|----------------|
| **Literals** | `int`, `float`, `bool`, `str`, `None` |
| **Arithmetic** | `+ - * / // % **`, unary `-` |
| **Bitwise** | `& \| ^ << >> ~` |
| **Logic** | `and or not`, comparisons `== != < <= > >=` |
| **Strings** | concat `+`, repeat `*`, comparisons; methods: `upper lower strip contains find starts_with ends_with is_digit is_alpha is_space replace char_at to_int`; `len(str)` |
| **Functions** | `def`, calls, recursion, return values, `print`, `str/int/float/bool`, `input`, `read_int`, `len` |
| **Control flow** | `if/elif/else`, `while`, `for x in range(N)`, `for x in list`, `break`, `continue` |
| **Collections** | list / map / tuple / set literals; index get/set; `len()` |
| **Modules** | `import` / `from import` — flat merge of top-level functions from `.vyu`/`.vayu` beside the source |
| **Packages** | `--emit-vye` / `--output`, `--run-vye`, `vayuc file.vye`, `--enable-cache` |

## Not yet on Integrator (Phase 10+)

| Feature | Notes |
|---------|--------|
| **Exceptions** | `try/except/raise` — interpreter/VM only |
| **Classes / OOP** | `class`, methods, inheritance — interpreter/VM only |
| **Generics** | Syntax accepted by frontend; Integrator erases types where possible but does not implement full generic lowering |
| **Generators** | `yield` — interpreter/VM only |
| **Lambdas / closures** | interpreter/VM only |
| **Pointers** | interpreter/VM only |
| **Native PE codegen** | `--native` runs VIR in-process; machine-code backend is Phase 12 |

## CLI reminders

| Command | Role |
|---------|------|
| `vayuc file.vyu` | Tree-walk interpreter |
| `vayuc file.vyu --vm` | Bytecode VM |
| `vayuc file.vyu --native` | VIB Integrator (this matrix) |
| `vayuc file.vyu --output out.vye` | Write Vayu Executable package |
| `vayuc out.vye` or `--run-vye` | Run package |
| `vayuc --vib-status` | Capability table |
| `vayuc` (no args) | REPL |

## Changelog (Integrator)

- **Phase 3–5:** arithmetic, strings, functions, returns  
- **Phase 6:** control flow, `** // and or not`, for-range and for-list  
- **Phase 7:** collections  
- **Phase 8:** flat module import  
- **Phase 9:** verifier coverage expansion  
- **Phase 10 (partial):** `input` / `read_int`; bitwise ops  
- **Regalloc fix:** temps never alias named locals (fixes corrupted `print`/`input` values and `sum_int`)
