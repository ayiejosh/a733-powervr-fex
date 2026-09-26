#!/usr/bin/env python3
"""Switch the WoW64 (32-bit) Windows CPU backend to FEX in the three wrappers.

Surgical by design: it replaces only the block between the tuning marker and the
`exec wine` line, leaving every original line of each wrapper untouched, and it
backs each file up first. Run with sudo (the wrappers are root-owned).
"""
import re
import shutil
import time

FILES = ['/usr/local/bin/d3drun', '/usr/local/bin/winrun', '/usr/local/bin/guirun']

BLOCK = """# --- measured tuning (2026-09-22, docs/FEX-BOX-TUNING-2026-09-22.md) ---
# box64 settings reach it only through the environment: a .box64rc at
# %USERPROFILE% hangs WowBox64, and /etc/box64.box64rc is never read by it.
# CALLRET=1 measured -2.6% on emulated throughput, output identical.
# Revert for one run by prefixing this command with BOX64_DYNAREC_CALLRET=0
export BOX64_DYNAREC_CALLRET=1
#
# WoW64 (32-bit) CPU backend switched from box64 to FEX. Measured -14.2%
# (3.01 s vs 3.51 s) on an emulated i386 PE scanning a 100 MB file, 3/3 runs,
# with the line count identical on every run.
#
# VERIFIED SAFE FOR 64-BIT: an x86-64 PE ignores HODLL completely and always
# loads libarm64ecfex.dll, so this only changes 32-bit Windows programs. Both
# paths were tested with this exact wiring.
#
# To revert this one line, either comment it out or prefix a run with
# HODLL=wowbox64.dll to go back to box64 for that run.
export HODLL=libwow64fex.dll
"""

stamp = time.strftime('%Y%m%d-%H%M%S')
for path in FILES:
    src = open(path).read()
    shutil.copy(path, f'{path}.bak-hodll-{stamp}')
    match = re.search(r'# --- measured.*?(?=exec wine )', src, re.S)
    if not match:
        print(f'MARKER NOT FOUND, left alone: {path}')
        continue
    open(path, 'w').write(src[:match.start()] + BLOCK + src[match.end():])
    print(f'updated {path}')
