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
 Native Compiler
        |
        v
      QBE IL
        |
        v
 Native Toolchain
        |
        v
    Executable
```

VCB is being developed separately as the future native backend layer.

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

# 27. VCB — Vayu Compiler Backend

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

VCB is intended to provide a focused backend boundary so frontend/language development and machine-level compiler engineering can evolve independently.

Planned backend responsibilities include:

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

The current project is moving toward compiler independence and self-hosting.

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

The project roadmap covers:

1. Core language
2. Control flow and functions
3. Types and collections
4. OOP and functional programming
5. Runtime and modules
6. Native compilation
7. Self-hosting preparation
8. Object/escape optimization
9. Package ecosystem
10. Standard-library expansion
11. Advanced syntax
12. Ownership and memory safety
13. Runtime utility expansion
14. Collections and interoperability
15. Native FFI
16. CPython integration
17. Vayu self-hosting
18. Developer tooling
19. GUI framework
20. 2D and 3D development
21. AI/ML
22. Package ecosystem expansion
23. Single-binary native toolchain with VCB as the direct native backend

The detailed phase descriptions remain in [docs/vision.md](docs/vision.md).

---

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
