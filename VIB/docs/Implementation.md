# Vayu VIB Implementation — V0.2-Alpha

## Host commands
`shell(cmd)`, `system(cmd)`, and `run_command(cmd)` are type-checker builtins and VIR builtins.
They run `std::system(cmd)` and return an int exit code.

```
print(shell("cmd /c echo hello"))
shell("powershell -NoProfile -Command \"Write-Host hi\"")
```

## Strings, lists, loops, functions
See IDE Help → Syntax for the full brief. Integrator supports arithmetic, bitwise, logic,
string methods (including on literals), `in`, `append`, `range`/`for`/`while`, `def`, modules, `.vye`.

## IDE Terminal
Interactive PowerShell process; type commands at the bottom line. Select text in Terminal/Output and **Ctrl+C** to copy.
