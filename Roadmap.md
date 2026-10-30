# Vayu Engineering Roadmap

**Language:** Vayu (`.vyu`)  
**Integrated compiler/runtime:** VIB (Vayu Integration Backend)  
**Architecture:** Integrator  
**Current internal representation:** Vayu Intermediate Representation (VIR)

## Phase roadmap (summary)

| **Phase** | **Status** | **Notes** |
| :--- | :--- | :--- |
| **0.5a** | Completed | `run_all` Invoke-Backend fix |
| **0.5b** | Completed | TypeChecker: current-scope assign + for-loop scoped vars (classes/collections type errors); exceptions VM catch earlier |
| **0.5b residual** | Working | `exceptions.vyu` --run vs --vm stdout parity; `lambdas.vyu` VM if still failing |
| **4–6.2** | Completed | Strings, functions, control flow, branch verification |
| **7.1–7.2** | Completed | list/map/tuple/set literals, index get/set, len() |
| **8+** | Planned | Modules, verifier suite, exceptions/classes in VIB, native codegen |
| **2A–2C** | Completed | `.vye`, `--output`, dispatch, cache |

### Tooling

- No-args REPL; `--help`; `--output` = `--emit-vye`
- `Vayu-IDE.exe` (Win32) with IDE.ico; `vayuc.exe` icon from Vayu_logo.ico

**Rule:** Support is established by implementation, verifier behaviour, and tests.
