# VIB / VIR Implementation — V0.2-Alpha

## Method calls
String methods work on **any** string expression (literal, variable, call):
```
print("a,b,c".split(","))
print(",".join(["a","b"]))
```

## Supported (Integrator `--native`)
Arithmetic, bitwise, logic, strings (+ split/join), functions, input/read_int/print,
if/while/for range(N)|range(a,b)|list, collections, modules, .vye.

## CLI
```
vayu file.vyu --native
vayu --version          # V0.2-Alpha + repo link
vayu file.vyu --output out.vye
vayu out.vye
```
(`vayuc.exe` still produced as a copy of `vayu.exe` for tests.)

## IDE V0.2-Alpha
- Title: **Vayu-IDE V0.2-Alpha**
- File / Edit (IDLE-style) / Tools / Compile / Help
- Ctrl+Wheel zoom, Alt+Z wrap
- Recents + last file in `%AppData%/VayuIDE`
- Run with **CREATE_NO_WINDOW** + **WIN32 GUI subsystem** (no host cmd)
- Terminal line at bottom for `input`/`read_int` (no popup boxes)
- Compile: emit .vye, run .vye, host-arch .cmd launcher

## Not yet
Classes, exceptions, generators, PE native codegen (Phase 10–12).
