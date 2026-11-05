# emit_exe

Creates a **real PE** `.exe` for the **host architecture** by compiling `launcher_main.c` with MSVC `cl.exe` (same machine → same arch).

Pipeline: `.vyu` → `.vye` → generate C → `cl /Fe:program.exe launcher.c` → ship `program.exe` + `program.vye` + `vayu.exe` on PATH or beside the exe.
