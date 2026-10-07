# Third-Party Software and Files

This document lists third-party executables, libraries, and other externally maintained files used or distributed with this project.

## Disclaimer

NotY215 does not claim ownership or copyright over any third-party software, executable, library, source code, trademarks, or other materials listed below.

Vayu and VCB do not own these third-party files and do not claim copyright over them. Their respective copyright holders retain all applicable rights.

Third-party software is used and/or distributed according to its own license terms. Users must review and comply with the applicable license before redistributing or modifying any third-party component.

Nothing in this document transfers ownership of third-party software to NotY215, Vayu, or VCB.

## Third-Party Components

| Component | File(s) | Version / Source | Official Project | Source Code | License | License Link |
|---|---|---|---|---|---|---|
| LLVM / LLD | `lld-link.exe`, `ld.lld.exe` | LLVM 22.1.3 | [LLVM Project](https://llvm.org/) | [llvm-project source](https://github.com/llvm/llvm-project) | Apache License 2.0 with LLVM Exceptions | [LLVM LICENSE.TXT](https://github.com/llvm/llvm-project/blob/main/llvm/LICENSE.TXT) |
| LLVM inspection tools | `llvm-nm.exe`, `llvm-objdump.exe`, `llvm-readobj.exe` | LLVM 22.1.3 | [LLVM Project](https://llvm.org/) | [llvm-project source](https://github.com/llvm/llvm-project) | Apache License 2.0 with LLVM Exceptions | [LLVM LICENSE.TXT](https://github.com/llvm/llvm-project/blob/main/llvm/LICENSE.TXT) |
| Microsoft Windows SDK / Kernel32 import library | `tools/kernel32.Lib` | Windows SDK component | [Windows SDK](https://learn.microsoft.com/en-us/windows/apps/windows-sdk/) | Not applicable as a standalone source file; `kernel32.lib` is an import library for the Windows API | Microsoft Software License Terms for the Windows SDK | [Microsoft SDK license terms](https://github.com/microsoft/win32metadata/blob/main/licenses/sdk_license.txt) |

## Notes

### LLVM / LLD

The bundled LLVM binaries are third-party builds of LLVM 22.1.3. They are not part of the VCB or Vayu source code and remain subject to the LLVM Project's licensing and attribution requirements.

The LLVM Project states that its current licensing framework is the Apache License 2.0 with LLVM Exceptions. Some historical components may have different applicable terms, so the original LLVM distribution and its license notices remain authoritative.

### Microsoft Windows SDK

`kernel32.lib` is an import library used to link against Windows system APIs. It is not source code owned or authored by VCB or Vayu.

The applicable Microsoft SDK license terms govern the use and redistribution of SDK components. The Microsoft documentation identifies `Kernel32.lib` as the library associated with the Windows Kernel32 API.

## Ownership and Copyright

All third-party copyrights remain with their respective owners and contributors.

- NotY215 does not claim ownership of the listed third-party files.
- Vayu does not claim ownership of the listed third-party files.
- VCB does not claim ownership of the listed third-party files.
- The Vayu and VCB project licenses apply to Vayu/VCB original code, not to third-party components that are separately licensed.
- Third-party trademarks and names belong to their respective owners.

For redistribution, always follow the current license and notice requirements supplied by the respective third-party project.
