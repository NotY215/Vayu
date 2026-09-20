# 🌌 Vayu Language Syntax

### Python-inspired syntax · Static type checking · Native-language foundations

This document describes the language surface currently represented by the Vayu compiler, interpreter, VM, native backend, and current tests/examples. Backend-specific limitations are called out where relevant.

**Official source extension: `.vyu`**

For the latest roadmap and benchmark information, visit the [official Vayu website](https://vayu.gt.tc).

---

# 1. Source Files

Vayu source files use `.vyu`:

```text
hello.vyu
main.vyu
control.vyu
fib.vyu
classes.vyu
```

Blocks are indentation-based and Python-inspired.

# 2. Comments

```vyu
# This is a comment
name = "Vayu"  # Inline comment
```

# 3. Variables & Types

Type inference:

```vyu
x = 5
y = 3.14
name = "Vayu"
flag = true
```

Explicit annotations:

```vyu
x: int = 10
y: float = 2.5
name: str = "Vayu"
flag: bool = true
```

Current demonstrated types include `int`, `float`, `bool`, `str`, `None`, `list<T>`, map values, struct/class types, and function values. Integer-to-float promotion is supported where applicable.

# 4. Strings & Printing

```vyu
name = "Vayu"
message = "Hello, " + name
print(message)
print("value:", 42)
print(str(42))
```

Current string functionality also demonstrates `strip()`, `lower()`, `upper()`, `split()`, `join()`, `replace()`, `find()`, `contains()`, `starts_with()`, `ends_with()`, `is_digit()`, `is_alpha()`, `is_space()`, and `char_at()`.

# 5. Functions

```vyu
def add(a: int, b: int) -> int:
    return a + b

print(add(2, 3))
```

Typed parameters, return types, `None` returns, bare `return`, and recursion are supported.

```vyu
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
```

# 6. Conditions

```vyu
if x < 0:
    print("negative")
elif x == 0:
    print("zero")
elif x < 10:
    print("small")
else:
    print("big")
```

# 7. Loops

`while` and `for` loops are supported:

```vyu
i = 5
while i > 0:
    print(i)
    i = i - 1

for color in ["red", "green", "blue"]:
    print(color)
```

`range()` supports one, two, and three arguments:

```vyu
range(5)
range(2, 8)
range(0, 10, 2)
```

Nested loops, `break`, and `continue` are supported.

# 8. Arithmetic

| Operator | Meaning |
|---|---|
| `+` | addition |
| `-` | subtraction / unary negative |
| `*` | multiplication |
| `/` | division |
| `//` | floor/integer division |
| `%` | modulo |
| `**` | exponentiation |

Example:

```vyu
print(1 + 2)
print(10 - 3)
print(4 * 5)
print(7 / 2)
print(7 // 2)
print(7 % 3)
print(2 ** 10)
print(-5)
print(- -5)
```

Exponentiation is right-associative:

```vyu
print(2 ** 3 ** 2)
```

Output:

```text
512
```

Equivalent to `2 ** (3 ** 2)`.

# 9. Comparisons & Logic

Comparison operators:

```text
==  !=  <  <=  >  >=
```

Logical operators:

```text
and  or  not
```

Examples:

```vyu
print(2 < 3)
print(1 == 1)
print(true and false)
print(true or false)
print(not true)
```

# 10. Operator Precedence

Parentheses control evaluation order:

```vyu
print(1 + 2 * 3)       # 7
print((1 + 2) * 3)     # 9
```

Exponentiation is evaluated right-to-left, while normal arithmetic, comparison, and logical precedence applies to the currently implemented operators.

# 11. Lists

```vyu
nums = [1, 2, 3, 4, 5]
```

The current implementation demonstrates:

- zero-based indexing
- negative indexing
- element assignment
- `len()`
- `append()`
- `pop()`
- `insert()`
- `contains()`
- `in`
- concatenation with `+`
- repetition with `*`
- nested lists
- iteration

```vyu
nums[0] = 100
nums.append(6)
last = nums.pop()
nums.insert(0, 50)
print(nums[-1])
print(3 in nums)
print(len(nums))
```

**List slicing is implemented.** Python-style slicing is currently supported for lists, tuples, and strings across the parser, AST/type-checking, VM/runtime, and native code-generation paths.

# 12. Maps

```vyu
ages = {
    "alice": 30,
    "bob": 25
}

print(ages["alice"])
ages["carol"] = 40
ages.put("dave", 22)
ages.remove("bob")
print("alice" in ages)
print(ages.keys())
print(len(ages))
```

Map indexing, assignment, `put()`, `remove()`, `contains()`, `keys()`, `len()`, membership, and iteration over keys are demonstrated by the current implementation.

# 13. Tuples & Sets

Tuples and sets are now first-class collection types.

Tuples support creation/conversion, indexing, iteration, stringification, slicing, and concatenation. Sets support creation/conversion, iteration, stringification, and set operations such as `add()`, `remove()`, and `union()`.

```vyu
a = (1, 2, 3)
b = (4, 5)
c = a + b
s = set([1, 2, 3])
s.add(4)
```

# 14. Structs

Structs provide named fields:

```vyu
struct Player:
    name: str
    score: int

p = Player("NotY", 100)
print(p.name)
p.score = 200
```

Positional and keyword construction, field access/modification, and nested structs are demonstrated.

# 15. Classes & Inheritance

Classes support fields, `__init__`, `self`, methods, inheritance, overriding, and `super()`.

```vyu
class Player:
    def __init__(self, name: str):
        self.name = name

    def greet(self) -> None:
        print("Hello, " + self.name)

p = Player("Vayu")
p.greet()
```

Inheritance and multi-level inheritance are demonstrated by the current examples.

# 16. Lambdas & Higher-Order Functions

The current implementation demonstrates function values, lambdas, closures, and:

```text
map()
filter()
reduce()
sorted()
any()
all()
sum()
```

# 17. Exceptions

Vayu supports `try`, `except`, `finally`, and `raise`:

```vyu
try:
    x = 10 / 0
except ZeroDivisionError as e:
    print(e.message)
finally:
    print("done")
```

The current implementation demonstrates `Exception`, `RuntimeError`, `TypeError`, `ValueError`, and `ZeroDivisionError`, plus exception variables, multiple handlers, bare `except`, nested exceptions, re-raising, raising another exception, and raising a string.

# 18. Modules

```vyu
import math_helpers
import math_helpers as math
from math_helpers import helper
from math_helpers import helper as h
```

Module constants, functions, structs, aliases, and chained member access are demonstrated. Modules use `.vyu` files.

# 19. Math Library

Current math functionality includes:

```text
sqrt()
floor()
ceil()
sin()
cos()
exp()
log()
log10()
log2()
pi
e
abs()
```

# 20. Expressions & Attribute Access

Expression statements, attribute chains, and indexing are supported:

```vyu
player.stats.score
items[0]
grid[0][1]
```

# 21. Type Checking

Vayu includes a type-checking stage for explicit annotations and supported type relationships.

```vyu
x: int = "hello"
```

This is invalid because a string cannot be assigned to an `int`.

# 22. AST

The compiler can generate/dump an AST representation for supported programs. The AST is used as a structured representation between parsing and later compiler stages.

# 23. Verification Examples

Arithmetic has been exercised with:

```vyu
print(1 + 2)
print(10 - 3)
print(4 * 5)
print(7 / 2)
print(7 // 2)
print(7 % 3)
print(2 ** 10)
print(-5)
print(- -5)
print(1 + 2 * 3)
print((1 + 2) * 3)
print(2 ** 3 ** 2)
```

The demonstrated results include `3`, `7`, `20`, `3.5`, `3`, `1`, `1024`, `-5`, `5`, `7`, `9`, and `512`.

Control-flow verification includes `if`/`elif`/`else`, `while`, `for` over ranges and lists, nested loops, `break`, `continue`, and loop-based accumulation.

```vyu
total = 0
for n in range(1, 11):
    total = total + n
print("sum 1..10 =", total)
```

Output:

```text
sum 1..10 = 55
```

# 24. Current Syntax Status

The reference documents implemented or demonstrated behavior only. Current areas include variables, types, functions, recursion, conditions, loops, arithmetic, comparisons, logical expressions, collections, structs, classes, inheritance, lambdas/closures, exceptions, modules, math functionality, type checking, AST generation, and VM-oriented execution.

Known limitations remain documented until the corresponding compiler features are implemented and tested.

The official Vayu source extension is **`.vyu`**.

---

## 🌐 More Vayu Information

For the latest roadmap, project updates, and benchmark information, visit the **[official Vayu website →](https://vayu.gt.tc)**.

**Vayu — Python-inspired syntax. Native ambition. One language.**


# 25. Current Advanced Language Features

The current source additionally includes:

- `const` declarations
- `enum` declarations
- static class members
- `public`, `protected`, and `private` visibility
- bitwise operators
- `match` statements
- `with` statements
- `defer` statements
- generators and `yield`
- generator state and `next()`
- generic/type parameters and constraints
- `unique<T>`, `shared<T>`, and `weak<T>` ownership types
- `ptr<T>` pointer/reference types
- address-of, dereference, and pointer arithmetic
- `malloc()` and `free()`
- `extern "C"` declarations
- C FFI and function pointers
- native struct-by-value FFI support for supported small single-field cases

## Reflection and utility built-ins

The current runtime/native implementation includes `hash()`, `id()`, `callable()`, `repr()`, `round()`, `pow()`, `divmod()`, `sign()`, `gcd()`, `lcm()`, `clamp()`, `enumerate()`, `zip()`, `reversed()`, `isinstance()`, `issubclass()`, `hasattr()`, `getattr()`, `dir()`, `setattr()`, and `delattr()`.

## Runtime modules

Current built-in/runtime module registrations include `math`, `fs`, `time`, `json`, `regex`, `thread`, `net`, `crypto`, `random`, `os`, and `py`.

## Python / CPython integration

The native backend currently exposes a `py` module with `init()`, `version()`, `run()`, `exec()`, primitive value bridging, Python module import, attribute access, Python calls, and reference-count operations. The interpreter/VM path provides stubs that report when the native backend is required.

The Python bridge is therefore an implemented native interoperability layer, not yet a complete Python object-model replacement.

## Backend note

Vayu currently has interpreter, bytecode/VM, and native execution paths. Native-only functionality includes the C FFI, raw native integration, and CPython bridge. The test suite explicitly separates tests that require the native backend.

## Current phase

**Phase 16 is complete. Phase 17 is the current development direction.**

Phase 16 expanded native CPython interoperability and compound assignment support. The next roadmap milestone is to bring the compiler toward self-hosting: the Vayu implementation must reach feature parity with the implemented C++ `vayuc` bootstrap compiler before the C++ bootstrap can eventually be retired.
