#!/usr/bin/env python3
"""Make the WoW64 backend overridable per run.

The first version of this change documented `HODLL=wowbox64.dll d3drun ...` as the
revert path, and that does NOT work: each wrapper already sets HODLL itself (and
this block sets it again, deliberately, because it must win over those lines), so
an HODLL from the caller's environment is overwritten and silently ignored. A
documented escape hatch that does not work is worse than none.

Fix: derive the effective value from a distinct variable, so the caller's escape
hatch cannot collide with the assignments the wrapper already makes.
"""
import re

FILES = ['/usr/local/bin/d3drun', '/usr/local/bin/winrun', '/usr/local/bin/guirun']

OLD_COMMENT = """# To revert this one line, either comment it out or prefix a run with
# HODLL=wowbox64.dll to go back to box64 for that run.
export HODLL=libwow64fex.dll"""

NEW = """# Revert for a single run with HODLL_OVERRIDE, which cannot collide with the
# HODLL assignments above:
#     HODLL_OVERRIDE=wowbox64.dll <this wrapper> <app>
export HODLL="${HODLL_OVERRIDE:-libwow64fex.dll}\""""

for path in FILES:
    src = open(path).read()
    if OLD_COMMENT not in src:
        print(f'pattern not found, left alone: {path}')
        continue
    open(path, 'w').write(src.replace(OLD_COMMENT, NEW))
    print(f'fixed revert path in {path}')
