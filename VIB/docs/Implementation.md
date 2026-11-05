# Vayu VIB / VIR — Complete Implementation Brief (V0.2-Alpha)

**Repo:** https://github.com/NotY215/Vayu  
**CLI:** `vayu.exe` only (bootstrap compiler + Integrator).  
**IDE:** Vayu-IDE V0.2-Alpha (Qt6).

---

## 1. What VIB is

VIB (Vayu Integration Backend) lowers `.vyu` AST → **VIR** (Vayu IR), verifies it, then runs it on an in-process **VIR interpreter**.  
`vayu file.vyu --native` is this path. It is a hybrid: compile-to-IR once, execute linearly (not a tracing JIT).

| Mode | Flag | Role |
|------|------|------|
| Tree interpreter | (default) | Full language surface as implemented in C++ interp |
| Bytecode VM | `--vm` | Chunk + VM |
| Integrator / VIR | `--native` | Supported subset below |
| Package | `--output` / `--emit-vye`, run `.vye` | Serialize VIR subset |

---

## 2. Syntax implemented on Integrator (`--native`)

### 2.1 Literals & variables
```
x = 1
s = "hello"
b = true
n = None
```

### 2.2 Arithmetic
`+ - * / // % **` and unary `-`

### 2.3 Bitwise
`& | ^ << >> ~`

### 2.4 Logic & compare
`and or not` · `== != < <= > >=`

### 2.5 Membership
```
print(2 in [1, 2, 3])      # list / tuple
print("ab" in "xabz")      # substring
print("k" in {"k": 1})     # map keys
```

### 2.6 Strings
Concat `+`, repeat `*`, methods on **any** string expression (literal or variable):

| Method | Example |
|--------|---------|
| upper/lower/strip | `"Hi".upper()` |
| split / join | `"a,b".split(",")` · `",".join(["a","b"])` |
| contains / find | `s.contains("x")` |
| starts_with / ends_with | |
| is_digit / is_alpha / is_space | |
| replace / char_at / to_int | |

### 2.7 Collections
List / map / tuple / set literals · index get/set · `len` · `list.append(x)`

```
xs = [1, 2]
xs.append(3)
print(xs[0])
```

### 2.8 Control flow
```
if cond:
    ...
elif other:
    ...
else:
    ...

while cond:
    ...
    break / continue

for i in range(5):
    ...
for i in range(2, 6):
    ...
for x in [10, 20]:
    ...
```

### 2.9 Functions
```
def add(a, b):
    return a + b
print(add(2, 3))
```

Builtins: `print`, `input`, `read_int`, `len`, `str`, `int`, `float`, `bool`, `range`, **`shell` / `system`**

### 2.10 Host shell (cmd / PowerShell / sh text)
Run a host command string (exit code returned as int):

```
# Windows cmd
code = shell("echo Hello from cmd")
print(code)

# PowerShell one-liner
shell("powershell -NoProfile -Command \"Write-Host 'hi'\"")

# Multi-step via cmd
shell("cmd /c echo line1 & echo line2")
```

### 2.11 Modules
`import` / `from import` — flat merge of top-level functions from sibling `.vyu` files.

### 2.12 Packages
```
vayu file.vyu --output out.vye
vayu out.vye
```

---

## 3. Not yet on Integrator (interpreter/VM may still work)

Exceptions, classes/OOP, full generics, generators, lambdas/closures, raw pointers, PE machine-code codegen for arbitrary programs (Phase 10–12).

---

## 4. CLI

```
vayu --version          # V0.2-Alpha + repo URL
vayu file.vyu           # tree interpreter
vayu file.vyu --vm
vayu file.vyu --native
vayu file.vyu --dump-ir
vayu file.vyu --output out.vye
vayu out.vye
vayu --vib-status
vayu                    # REPL
```

---

## 5. Emit host `.exe` (architecture-matched)

IDE **Compile → Create host .exe** (or tools):

1. Emit `.vye` for the program  
2. Generate a small **C launcher**  
3. Compile with the **host** `cl.exe` (x64/x86 as on this PC) → real PE  

The PE calls `vayu.exe` to run the adjacent `.vye`. Same architecture as the machine that built it (avoids “This app can’t run on your PC” when built on the target class of machine).

Full AOT machine code for Vayu itself remains Phase 12.

---

## 6. Common mistakes

| Wrong | Right |
|-------|--------|
| `if c = x:` | `if c == x:` |
| `if c == name:` without quotes for text | `if c == "name":` |
| no indent under `if` | 4-space body |
| PowerShell `Set-Content` with `` `n `` inside **single** quotes | use here-string `@" ... "@` for real newlines |

```powershell
# Correct multi-line write:
$t = Join-Path $env:TEMP "t.vyu"
@'
print("a,b,c".split(","))
print(",".join(["x","y"]))
'@ | Set-Content $t -Encoding utf8
vayu $t --native
```

---

## 7. IDE overview (V0.2-Alpha)

- Activity bar + Explorer (folder / multi-file tabs)  
- Welcome, Terminal, Output  
- Help → Syntax (detailed) · Examples (load) · Repo  
- Git activity: “Coming soon”  
- No Extensions button  
- Settings stored under `%AppData%/VayuIDE`
