# Windows apps on A733 via Hangover (trixie / 6.6)

Run Windows applications on the Cubie A7A using **Hangover 11.9** = **wine 11.9** with
**FEX/box64 WoW64** (x64 -> FEX, x86-32 -> wowbox64). Two launchers cover the two cases:
console apps and GUI apps. For **GPU Direct3D** see `../gpu/dxvk/` (a separate wine prefix
+ DXVK-Sarek).

> Bring your own Windows app binaries — none are bundled. Prefix: `~/.wine-hangover`.

## Launchers

| Launcher | For | GPU / GL |
|---|---|---|
| **`winrun <app.exe>`** | CLI / console Windows apps | n/a (no window) |
| **`guirun <app.exe>`** | GUI Windows apps | **software-GL window** (llvmpipe) |

- `winrun` runs console apps under Hangover's WoW64 (x64 via FEX, x86-32 via wowbox64).
- `guirun` opens a real window but renders GL with **software llvmpipe**, because the
  native PowerVR GL blob **deadlocks wine's graphics init** (this is the whole reason a
  software GL context is forced). Fine for productivity apps; GL/3D windows are
  CPU-rendered. A benign `libEGL ... failed to open llvmpipe /usr/local/lib/dri/...`
  warning may appear (wrong search path) — the app falls back and renders fine; it's
  cosmetic.

> For GPU-accelerated 3D in a Windows app, use **D3D via DXVK-Sarek** (`../gpu/dxvk/`,
> the `d3drun` launcher, prefix `~/.wine-dxvk`) — that path runs the render on the real
> GPU.

## Verified apps

| App | Launcher | Result |
|---|---|---|
| **7-Zip 24.08** (`7z.exe` compress / test / extract / list) | `winrun` | PASS — full workflow, **byte-perfect** (all MD5s match), exit 0 |
| 7-Zip benchmark (`7z b`) | `winrun` | PASS — multithreaded (2T) LZMA codec |
| **Notepad** (`notepad.exe`) | `guirun` | PASS — real titled window, clean lifecycle |
| **WordPad** (`wordpad.exe`) | `guirun` | PASS — real window + toolbar/ruler |
| **7-Zip File Manager** (`7zFM.exe`) | `guirun` | PASS — real window + file-list |

These are real applications doing real work (the 7-Zip CLI run is a full
compress/verify/extract with byte-perfect integrity, not a smoke test), with real windows
and clean SIGTERM lifecycle.

## Relationship to the emulator layer

Hangover invokes its own WoW64 interpreters directly, so it is **independent of the Linux
binfmt routing** (FEX-as-default for bare `./prog.x86_64`). The static-glibc-MT box64
issue that affects *Linux* binaries (see `../box64/`) does not apply to the Windows path —
Hangover's wowbox64 handles the Win32 x86-32 side, FEX handles x64.
