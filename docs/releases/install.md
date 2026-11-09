## Download

| System | File | What is in it |
|---|---|---|
| macOS 11 or later (Apple silicon and Intel) | `VCSIDE-{{VERSION}}-macos.zip` | `VCSIDE.app` |
| Windows 10 or later, 64-bit | `vcside-{{VERSION}}-windows-x86_64.zip` | `vcside.exe` and its `data` folder: keep them together |
| Linux x86-64, glibc 2.41 or later (Debian 13, Ubuntu 25.04, Fedora 42) | `vcside-{{VERSION}}-linux-x86_64.tar.gz` | `vcside` and its `data` folder: keep them together |

VCSIDE needs OpenGL 2.1 or later and nothing else of its own: SDL3 is built in.

**These builds are not signed yet.** On macOS, the first time you open VCSIDE the system says it cannot check the developer: open *System Settings → Privacy & Security* and choose *Open Anyway* (or run `xattr -dr com.apple.quarantine VCSIDE.app`). On Windows, SmartScreen may stop it: *More info → Run anyway*.

VCSIDE does not bundle the tools it works with. Install the ones your projects need: DASM for assembly, batari Basic 1.9 and wasmtime for batari Basic, the Arm GNU toolchain and GNU Make for C on ELF cartridges, and Stella or Gopher2600 to run and debug. *Settings → Tools* says what it found, with each download page.

### Checksums (SHA-256)
