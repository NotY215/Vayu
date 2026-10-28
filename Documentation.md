# 📚 Vayu Documentation

<div align="center">

# Vayu

**Python-inspired syntax · Native execution · Low-level control**

**Official source extension:** `.vyu`

[Website](https://vayu.gt.tc) · [Syntax Reference](docs/syntax.md) · [Package System](docs/package.md) · [AI / ML](docs/ai.md) · [Vision & Roadmap](docs/vision.md) · [VCB](https://github.com/NotY215/VCB)

</div>

---

# 1. Documentation Map

| Document | Purpose |
|---|---|
| [Syntax Reference](docs/syntax.md) | Current language syntax, operators, declarations, types, statements, expressions, runtime features, and compiler modes |
| [Package Documentation](docs/package.md) | Package-manager and ecosystem direction |
| [AI / ML Documentation](docs/ai.md) | AI/ML architecture and ecosystem direction |
| [Vision & Roadmap](docs/vision.md) | Long-term goals and phased development |
| [VCB](https://github.com/NotY215/VCB) | Dedicated Vayu Compiler Backend project |

---

# 2. What Is Vayu?

Vayu is an independent programming-language project designed around a combination of:

- Python-inspired readable syntax
- static typing and type inference
- compiled/native execution
- low-level memory and native interoperability
- object-oriented and functional programming
- C interoperability
- Python / CPython interoperability
- future self-hosting
- a dedicated native compiler backend through VCB

The official source format is:

```text
.vyu
```

Example:

```vyu
def greet(name: str) -> None:
    print("Hello, " + name)

greet("Vayu")
```

---

# 3. Language Model

Vayu source is processed through several stages.

```text
Vayu Source (.vyu)
        |
        v
      Lexer
        |
        v
      Tokens
        |
        v
      Parser
        |
        v
       AST
        |
        v
 Semantic / Type Checking
        |
        +-------------------+
        |                   |
        v                   v
 Interpreter          Bytecode Compiler
                            |
                            v
                           VM
        |
        +-------------------+
        |
        v
 Native Compiler / IR
        |
        v
   Native backend
        |
        v
      VCB
        |
        v
    Executable
```

VCB is developed as a separate backend project. The Vayu compiler now uses VCB for the native path; the older QBE + GCC backend chain is historical.

## Windows Toolchain Layout

The tools directory is intended to hold the companion tools and embedded runtime for Windows development. This list documents the required layout, not a claim that every binary is already present in the remote repository:

- `vcb.exe`: VCB backend executable.
- `lld-link.exe`: COFF linker for `vcb link --target pe`.
- `ld.lld.exe`: ELF linker for `vcb link --target elf`.
- `kernel32.lib`: Windows import library for PE linking.
- `nva.vyu`: Vayu-source package manager.
- `nva-server.vyu`: reference read-only registry server; its publish endpoint intentionally returns HTTP 501.
- `make_tiny_onnx.py`: generates the small ONNX model used by examples.
- `python313.dll`, `python313.zip`, `Lib.zip`, `DLLs/`, and the Visual C++ runtime DLLs: CPython bridge runtime and native dependencies.

At the time of this update, `lld-link.exe`, `ld.lld.exe`, and `kernel32.lib` were not present in the GitHub tools listing and still need to be committed from the local toolchain. See [tools/README.md](tools/README.md) for layout and usage details, and [Third_Party.md](Third_Party.md) for third-party notices and redistribution cautions.

---

# 4. Source Language

Vayu uses indentation-based blocks.

```vyu
if score >= 100:
    print("high")
else:
    print("low")
```

The lexer produces structural:

```text
Newline
Indent
Dedent
EndOfFile
Invalid
```

Comments use `#`.

```vyu
# comment
value = 10  # inline comment
```

---

# 5. Types

The compiler type system currently contains these major categories:

```text
None
Bool
Int
Float
Str
Char
Bytes
Any
Unknown
Error
Function
Struct
List
Map
Tuple
Set
Named
TypeParam
Unique
Shared
Weak
Ptr
```

Examples:

```vyu
count: int = 10
ratio: float = 3.5
name: str = "Vayu"
items: list<int> = [1, 2, 3]
owners: unique<Player>
shared_player: shared<Player>
weak_player: weak<Player>
pointer: ptr<int>
```

The compiler also supports generic/type-parameter metadata and type constraints.

---

# 6. Expressions

The AST currently represents:

```text
IntLit
FloatLit
StringLit
CharLit
BoolLit
NoneLit
NameRef
Unary
Binary
Grouping
Call
Attr
Index
ListLit
MapLit
Lambda
GenericType
TupleLit
SetLit
Slice
```

Examples:

```vyu
x = 10
y = (x + 2) * 3
name = player.name
value = items[0]
part = text[1:4]
fn = lambda n: n * n
result = fn(5)
```

---

# 7. Statements

The AST currently represents:

```text
Expr
Assign
AnnotAssign
If
While
Def
Return
Struct
Class
For
Try
Raise
Import
FromImport
Pass
Break
Continue
Const
Enum
Yield
Block
Extern
```

The parser also has dedicated handling for advanced constructs such as match, with, unsafe sections, namespaces, visibility modifiers, and compound assignment.

---

# 8. Functions

Functions use Python-inspired definitions with optional type annotations.

```vyu
def add(a: int, b: int) -> int:
    return a + b
```

Supported language concepts include:

- typed parameters
- return types
- None-returning functions
- bare return
- recursion
- function values
- lambdas
- closures
- higher-order functions
- generic/type-parameter support

---

# 9. Control Flow

## Conditions

```vyu
if x < 0:
    print("negative")
elif x == 0:
    print("zero")
else:
    print("positive")
```

## While

```vyu
while x > 0:
    x -= 1
```

## For

```vyu
for item in items:
    print(item)
```

## Range

```vyu
range(5)
range(2, 8)
range(0, 10, 2)
```

## Loop control

```text
break
continue
pass
```

---

# 10. Operators

### Arithmetic

```text
+  -  *  /  //  %  **
```

### Comparison

```text
==  !=  <  >  <=  >=
```

### Logical / membership

```text
and  or  not  in  is
```

### Bitwise

```text
&  |  ^  ~  <<  >>
```

### Compound assignment

```text
+=  -=  *=  /=  %=  **=  //=
&=  |=  ^=  <<=  >>=
```

### Native-oriented unary operations

```text
&value
*pointer
```

The parser precedence is:

```text
or
and
comparisons / in / is
|
^
&
<< >>
+ -
* / // %
**
```

Exponentiation is right-associative.

---

# 11. Collections

## Lists

```vyu
numbers = [1, 2, 3]
numbers.append(4)
print(numbers[0])
print(numbers[1:3])
```

List functionality includes indexing, negative indexing, assignment, iteration, append, pop, insert, membership, concatenation, repetition, nested lists, and slicing.

## Maps

```vyu
ages = {
    "alice": 30,
    "bob": 25
}

ages["carol"] = 40
print(ages["alice"])
```

Map functionality includes indexing, assignment, put, remove, contains, keys, length, membership, and iteration.

## Tuples

```vyu
point = (10, 20)
```

Tuple support includes creation, indexing, iteration, stringification, slicing, and concatenation.

## Sets

```vyu
values = set([1, 2, 3])
values.add(4)
values.remove(2)
```

## Strings

Strings support indexing and iteration plus runtime methods such as:

```text
strip
lower
upper
split
join
replace
find
contains
starts_with
ends_with
is_digit
is_alpha
is_space
char_at
```

---

# 12. Structs

Structs provide named fields.

```vyu
struct Player:
    name: str
    score: int

player = Player("NotY", 100)
player.score = 200
```

The compiler supports field types, construction, field access/modification, nested structures, and visibility metadata.

---

# 13. Classes and Inheritance

```vyu
class Player:
    def __init__(self, name: str):
        self.name = name

    def greet(self) -> None:
        print("Hello, " + self.name)
```

Class-related features include:

- fields
- methods
- constructors
- self
- inheritance
- method overriding
- super
- static members
- public/protected/private visibility
- generic class metadata

---

# 14. Constants and Enums

Constants use:

```vyu
const VERSION: str = "0.1"
```

Enums use named declarations with enum items:

```vyu
enum State:
    Idle
    Running
    Done
```

Exact enum value behavior follows the current compiler implementation.

---

# 15. Exceptions

Vayu supports try/except/finally and raise.

```vyu
try:
    value = 10 / 0
except ZeroDivisionError as e:
    print(e.message)
finally:
    print("finished")
```

The runtime handles exception values and currently demonstrates exception classes including:

```text
Exception
RuntimeError
TypeError
ValueError
ZeroDivisionError
```

The implementation also contains nested exception handling, multiple handlers, bare except, exception variables, re-raising, and raising other values.

---

# 16. Modules

```vyu
import math_helpers
import math_helpers as math

from math_helpers import helper
from math_helpers import helper as h
```

Modules use Vayu source files and can expose supported functions, constants, structs, and members.

---

# 17. Generators

Generators use yield.

```vyu
def numbers():
    yield 1
    yield 2
    yield 3
```

The VM contains generator state management, suspension/resume behavior, next, completion, and generator exception/cancellation handling.

---

# 18. Ownership and Native Memory

The type system contains:

```text
unique<T>
shared<T>
weak<T>
ptr<T>
ref<T>
```

Native-oriented operations include address-of, dereference, pointer arithmetic, malloc, and free.

These facilities are primarily associated with native compilation and low-level interoperability.

---

# 19. C Interoperability

The compiler contains an extern C declaration path.

Conceptual syntax:

```vyu
extern "C":
    def puts(s: str) -> int
```

Native support also includes function pointers, external-library linking, and supported native struct-by-value FFI cases.

---

# 20. CPython Interoperability

The native runtime exposes a py module.

Current native functionality includes:

```text
py.init()
py.version()
py.run()
py.exec()
```

The bridge also supports Python module import, attribute access, Python calls, primitive value conversion, and reference-count operations.

The Python bridge is a native interoperability layer; it is not a complete replacement for the CPython object model.

---

# 21. Runtime Modules

Current runtime registrations include:

```text
math
fs
time
json
regex
thread
net
crypto
random
os
py
```

Support may differ between interpreter/VM and native execution.

---

# 22. Runtime Utilities

Current runtime/native utility families include:

```text
print
str
len
abs
hash
id
callable
repr
round
pow
divmod
sign
gcd
lcm
clamp
enumerate
zip
reversed
isinstance
issubclass
hasattr
getattr
dir
setattr
delattr
map
filter
reduce
sorted
any
all
sum
```

---

# 23. Compiler Driver

The current compiler driver is named `vayuc`.

Basic usage:

```text
vayuc <file.vyu> [mode] [flags]
```

## Execution modes

```text
--run
--vm
--native
--native-out <path>
```

## Analysis / debugging modes

```text
--check
--dump-tokens
--dump-ast
--dump-bytecode
--dump-ir
```

## Additional flags

```text
--no-check
--no-opt
--bench [N]
--emit-runtime <path>
```

Examples:

```text
vayuc hello.vyu
vayuc hello.vyu --vm
vayuc hello.vyu --native
vayuc hello.vyu --check
vayuc hello.vyu --dump-tokens
vayuc hello.vyu --dump-ast
vayuc hello.vyu --dump-bytecode
vayuc hello.vyu --dump-ir
vayuc hello.vyu --bench 10
vayuc --emit-runtime runtime.c
```

---

# 24. Compiler Inspection

The compiler can expose intermediate stages for development:

```text
Source
  -> tokens       (--dump-tokens)
  -> AST          (--dump-ast)
  -> bytecode     (--dump-bytecode)
  -> native IR    (--dump-ir)
```

This makes the compiler easier to debug and allows language features to be traced through multiple stages.

---

# 25. Type Checking

The normal pipeline includes a semantic/type-checking stage.

Example:

```vyu
x: int = "hello"
```

This is rejected because the assigned string is not compatible with int.

The type system also includes numeric compatibility such as integer-to-float promotion where supported.

---

# 26. Compiler Components

The current source tree separates major compiler responsibilities:

```text
compiler/
├── ast/
├── bytecode/
├── compile/
├── driver/
├── interp/
├── lexer/
├── parser/
├── sema/
├── types/
└── vm/
```

Important responsibilities:

| Component | Role |
|---|---|
| lexer | source-to-token conversion |
| parser | token-to-AST conversion |
| ast | language structure representation |
| sema | semantic and type checking |
| types | type-system representation and compatibility |
| compile | AST-to-bytecode compilation |
| bytecode | bytecode representation and optimization |
| vm | bytecode execution |
| interp | tree-walking runtime and shared runtime support |
| driver | command-line compiler entry point |

---

# 27. VCB -- Vayu Compiler Backend

VCB is the companion backend project:

**[NotY215/VCB](https://github.com/NotY215/VCB)**

The intended architecture is:

```text
Vayu Source
    ↓
Vayu Frontend
    ↓
Vayu IR
    ↓
VCB
 ├─ Analysis
 ├─ Optimization
 ├─ Lowering
 └─ Native Code Generation
    ↓
Native Assembly
    ↓
Executable
```

VCB provides the backend boundary so frontend/language development and machine-level compiler engineering can evolve independently.

Current backend responsibilities include:

- IR analysis
- control-flow analysis
- optimization
- SSA-oriented work
- lowering
- instruction selection
- register allocation
- calling conventions
- native code generation

These are VCB development goals and should not be confused with every capability already present in the current Vayu C++ compiler.

---

# 28. Development Direction

Phases 27 through 31.5 are complete, including the VCB object-emission and external-linking path, VcbLower additions, and collection/string operations. Phase 31.6a and 31.6b cover string predicates and alignment/padding methods in this drop; 31.6c is the next drop for character-based trimming.

The C++ `vayuc` implementation acts as the bootstrap/reference compiler while the Vayu implementation is developed toward feature parity.

The long-term direction is:

```text
C++ bootstrap compiler
        ↓
Vayu implementation reaches parity
        ↓
Vayu compiles its own compiler
        ↓
Self-hosting
        ↓
C++ bootstrap can eventually be retired
```

---

# 29. Roadmap

| Phase / Item | Status | Scope |
|---|---|---|
| 27 Parts 1-17 | done | ELF writer, Linux runtime, PE correctness, validator |
| WDAC structural fix | done | `.rsrc`, rich header, 32 KB minimum image |
| Signing | removed | Not needed once obj+linker path lands |
| Tools | ready | `VCB\\tools\\`: `lld-link.exe`, `ld.lld.exe` (LLVM 22.1.3), `kernel32.lib`, LLVM inspection tools |
| 28.0 | done | Phase 28 test-harness and integration cleanup |
| 28.1 | done | COFF `.obj` emitter + `vcb emit-obj` |
| 28.2 | done | ELF `.o` emitter |
| 28.3 | done | `vcb link` with `lld-link` / `ld.lld` dispatch |
| 28.4 | done | `vayuc --native` external-link default |
| 28.5 | done | VcbLower builtin guard |
| 29 | done | MAC policy templates |
| 30 | done | Small VcbLower additions |
| 31.1 | done | Tuple literal/index/len, set literal/add/contains/len, string slicing |
| 31.2 | done | Open-ended string slices, tuple/set printing, set remove/clear |
| 31.3 | done | List slicing, `str.strip()`, `list.pop()`, `list.contains(v)` |
| 31.4 | done | `str.split`, single-character `str.replace`, `list.index`, `set.copy`, tuple slicing |
| 31.5 | done | Multicharacter `str.replace`, `str.count(sub)` |
| 31.6a | this drop | `str.is_digit`, `is_alpha`, `is_space`, `is_lower`, `is_upper`, `is_alnum`, `is_ascii` |
| 31.6b | this drop | `str.ljust(width[, fill])`, `str.rjust`, `str.center`, `str.zfill(width)` |
| 31.6c | next drop | `str.strip(chars)`, `str.lstrip([chars])`, `str.rstrip([chars])` |
| 31.7 | not started | Bug fixes: `types.vyu` float, `vm_arith.vyu`, `vm_flow.vyu`, `vcb_collections.vyu` truncation, `fib.vyu` segfault, stack-overflow guards |
| 31.8 | not started | Dict `keys/values/items/pop/pop_or/update/setdefault/get_or/copy/clear` |
| 31.9 | not started | List `sort/reverse/remove/insert/count/extend/clear/copy/first/last` |
| 31.10 | not started | String `find/rfind/index/partition/rpartition/format` |
| 31.11 | not started | Tuple `index/count`; set `union/intersection/difference/symmetric_difference/issubset/issuperset` |
| 32.1 | not started | Struct definitions, heap field layout, constructors, field access and assignment |
| 32.2 | not started | Struct methods, `self` binding, calls and field access through `self` |
| 32.3 | not started | Classes with `__init__` and positional constructor arguments |
| 32.4 | not started | Inherited fields visible in child types using the global field-offset table |
| 32.5 | not started | Method inheritance, overrides and dynamic-type dispatch |
| 32.6 | not started | `super().__init__(...)` and `super().method(...)` |
| 32.7 | not started | Static field reads/writes through `ClassName.field` |
| 32.8 | not started | Instance stringification and printing as `Name(field=value, ...)` |
| 32.9 | not started | `type(x)`, `isinstance(x, T)`, `hasattr(x, "f")` |
| 32.10 | not started | Enforce public/private/protected visibility in sema and VcbLower |
| 32.11 | not started | Keyword arguments for class constructors and method calls |
| 33.1 | not started | Single-expression lambda bodies without captures |
| 33.2 | not started | Lambda captures using heap-allocated upvalue blocks and one cell per captured name |
| 33.3 | not started | Nested `def` inside `def` using the same upvalue system |
| 33.4 | not started | First-class functions: return, pass as arguments, and store in lists/maps |
| 33.5 | not started | Bound method values captured as callable objects |
| 33.6 | not started | Vayu lambdas accepted by `map/filter/reduce/sorted(key=...)` |
| 33.7 | not started | Recursive closures and forward references |
| 33.8 | not started | Python-like late-binding closure capture semantics |
| 34.1 | not started | VCB `.pdata` and `.xdata` emission for COFF unwind metadata |
| 34.2 | not started | Basic `try/except` with one handler and no `as` binding |
| 34.3 | not started | `except Exc as e` binding |
| 34.4 | not started | Multiple exception handlers with type matching |
| 34.5 | not started | Unconditional `finally` execution |
| 34.6 | not started | `raise Exc(msg)` and bare `raise` |
| 34.7 | not started | Custom exception classes derived from `Exception` |
| 34.8 | not started | Exception propagation through the call stack |
| 34.9 | not started | Uncaught exception traceback to stderr and exit code 1 |
| 34.10 | not started | Linux exception path via `.eh_frame` or minimal `__cxa_throw` equivalent |
| 34.11 | not started | Linux `vayu_print_float` runtime support |
| 34.12 | not started | `defer` syntax sugar running on normal and exceptional exits |
| 35.1 | not started | Preserve `VType::Ptr` through VcbLower pointer expressions and assignments |
| 35.2 | not started | Address-of `&x` and dereference `*p` for locals |
| 35.3 | not started | `&obj.field` and `*obj.field = v` |
| 35.4 | not started | `&arr[i]` and `*arr[i] = v` |
| 35.5 | not started | Pointer arithmetic `p + i`, `p - i`, `p1 - p2` |
| 35.6 | not started | `malloc(size) -> ptr<T>` and `free(p)` using HeapAlloc/HeapFree on Windows and `brk` on Linux |
| 35.7 | not started | Single-function `extern "C"` declarations |
| 35.8 | not started | Mixed-signature foreign calls, up to 4 args on Win64 and 6 on SysV |
| 35.9 | not started | FFI struct layout with field alignment |
| 35.10 | not started | FFI callbacks passing Vayu function pointers to C and calling back into Vayu |
| 35.11 | not started | `str_to_cstr` and `cstr_to_str` conversions |
| 35.12 | not started | `unsafe:` blocks for raw pointer access and manual casts |
| 35.13 | not started | `ref<T>` with a non-null pointer invariant enforced by sema |
| 36 | not started | Modules and standard library |
| 37 | not started | `.text` size limit removal |
| 38 | not started | Self-hosting on VCB IR |
| 39 | not started | Full benchmark suite |
| 40 | not started | Version cut |

# 30. Current Status

Vayu is an early-stage independent language project with a growing compiler, runtime, VM, native compiler, FFI layer, and Python/CPython interoperability.

The source currently contains a substantially broader language surface than the original core compiler, including:

- static typing and inference
- functions and recursion
- lambdas and closures
- lists, maps, tuples, sets, and slicing
- structs and classes
- inheritance and overriding
- exceptions and modules
- constants and enums
- visibility and static members
- bitwise operations and compound assignment
- generators and yield
- ownership/reference types
- raw pointers and native memory operations
- C FFI
- Python/CPython interoperability
- multiple execution paths
- compiler inspection tools

Features may have different maturity levels across the interpreter, VM, and native compiler. The syntax reference therefore distinguishes language constructs from backend-specific functionality.

---

# 31. Recommended Reading Order

For someone learning Vayu:

1. [Syntax Reference](docs/syntax.md)
2. [Package Documentation](docs/package.md)
3. [AI / ML Documentation](docs/ai.md)
4. [Vision & Roadmap](docs/vision.md)
5. [VCB](https://github.com/NotY215/VCB)

For someone working on the compiler:

1. Syntax reference
2. AST / parser implementation
3. Type system
4. semantic checker
5. bytecode compiler
6. VM
7. native compiler
8. VCB architecture

---

# 32. Documentation Status

This document is the high-level technical documentation index and architecture guide.

The language-level reference is maintained in [docs/syntax.md](docs/syntax.md). The roadmap and long-term architecture are maintained in [docs/vision.md](docs/vision.md).

When implementation and documentation differ, the compiler source and tests are the authoritative implementation reference.

---

<div align="center">

**Vayu**

**Write simple. Run native. Control everything.**

[🌐 vayu.gt.tc](https://vayu.gt.tc)

</div>


## Current Codebase Notes

The compiler driver currently supports tree-walk execution, bytecode VM execution, type-check-only mode, token/AST/bytecode/IR inspection, native compilation, benchmarking, and native-runtime emission.

Native compilation exposes `--backend qbe|vcb`, `--native`, `--native-out <path>`, `--dump-ir`, and `--opt 0-3`. `--emit-runtime <path>` exports the native runtime C source.

VCB is now the native backend used by the Vayu native compiler. The backend flow is Vayu frontend/IR → VCB → native output. VCB provides the Windows PE and Linux ELF writers and its embedded runtime components.
