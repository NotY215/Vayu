# VIB / VIR Implementation Status

**Last updated:** 2026-11-05

Integrator path: `vayuc file.vyu --native`, `--dump-ir`, `--emit-vye` / `--run-vye`.

## Supported

| Area | Details |
|------|---------|
| Literals | int, float, bool, str, None |
| Arithmetic | `+ - * / // % **`, unary `-` |
| Bitwise | `& \| ^ << >> ~` |
| Logic / compare | `and or not`, `== != < <= > >=` |
| Strings | concat, repeat, methods: upper lower strip contains find starts_with ends_with is_digit is_alpha is_space replace char_at to_int **split join** |
| Functions | def, call, return, print, input, read_int, len, str/int/float/bool |
| Control | if/elif/else, while, break, continue |
| Loops | `for i in range(N)`, **`for i in range(start, end)`**, `for x in list` |
| Collections | list, map, tuple, set; index get/set; len |
| Modules | flat import merge |
| Packages | .vye emit/run, cache |

## IDE notes

- Run uses `--native` in a **background process** (`CREATE_NO_WINDOW` on Windows — no foreground cmd).
- `input` / `read_int` values are collected via dialogs and fed on stdin.
- Help → **Examples** loads prebuilt programs that run cleanly.

## Not on Integrator yet

Exceptions, classes, full generics, generators, lambdas, pointers, PE native codegen (Phase 10–12).

## Common mistakes

| Wrong | Right |
|-------|--------|
| `if c = x:` | `if c == x:` |
| `if c == heisenberg:` | `if c == "heisenberg":` |
| no indent under if | 4-space body |
| `print(try again)` | `print("try again")` |

### Working example
```
print("Hello World")
c = input("say my name: ")
if c == "heisenberg":
    print("You are God damn Right")
else:
    print("try again")
```
