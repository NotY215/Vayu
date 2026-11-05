404: Not Found

## Common beginner mistakes

| Wrong | Right |
|-------|--------|
| `if c = x:` | `if c == x:` (comparison is `==`) |
| `if c == heisenberg:` | `if c == "heisenberg":` (strings need quotes) |
| body not indented | indent 4 spaces under `if` / `else` |
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
