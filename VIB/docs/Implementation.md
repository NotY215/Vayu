# VIB / VIR — V0.2-Alpha

## New in this drop
- **`x in xs`**: membership for list/tuple, substring for str, key for map
- **`list.append(x)`**
- String methods on **literals**: `"a,b".split(",")`, `",".join([...])`
- CMake: single producer of `bin/vayu.exe` (bootstrap); self-host optional as `vayu_selfhost.exe`
- IDE: **Qt6 only** (no Win32 Forms)

## CLI
```
vayu.exe --version          # V0.2-Alpha + repo
vayu.exe file.vyu --native
vayuc.exe                   # copy of vayu.exe for tests
```

## IDE
Vayu-IDE V0.2-Alpha — File/Edit/Tools/Compile/Help, Ctrl+Wheel zoom, Alt+Z wrap,
%AppData%/VayuIDE recents, Terminal stdin line, no host console window.
