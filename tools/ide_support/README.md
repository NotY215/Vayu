# IDE support modules (V0.2-Alpha)

| Path | Role |
|------|------|
| `tools/vayu_ide_qt/` | Qt6 IDE (primary) |
| `tools/vayu_ide/` | Win32 fallback |
| `tools/ide_support/` | Shared notes for AppData, host-arch launchers |

AppData: `%AppData%/VayuIDE/VayuIDE.ini` (QSettings org VayuIDE).
Host .exe: currently a `.cmd` launcher bound to the local `vayu.exe` (same architecture as the PC). Full PE emit is Phase 12.
