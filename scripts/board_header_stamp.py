# PlatformIO pre-build script: rebuild when the board header changes.
#
# board/board.h pulls the selected board in with `#include BOARD_HEADER`, the
# macro set by -DBOARD_HEADER in platformio.ini. SCons's dependency scanner
# does not expand macros, so it never learns that anything depends on that
# header -- and an edit to, say, board/qualia_720.h rebuilt nothing at all.
# The build reported success and flashed the old values. That is how a change
# of pixel clock went nowhere.
#
# So the header's contents are hashed into a define every file is compiled
# with. Change the header and the command line changes, and SCons rebuilds
# everything built with it. That is more than strictly depends on the header,
# but nearly everything does, and a rebuild that is too wide is a minute
# where one that is too narrow is a wrong firmware.

import hashlib
import os
import re

Import("env")  # noqa: F821  (SCons global)


def board_header():
    flags = env.GetProjectOption("build_flags", "")  # noqa: F821
    if isinstance(flags, (list, tuple)):
        flags = " ".join(flags)
    match = re.search(r"BOARD_HEADER\s*=\s*'?\\?\"([^\"\\]+)\\?\"", flags)
    if not match:
        return None
    path = os.path.join(env.subst("$PROJECT_INCLUDE_DIR"), match.group(1))  # noqa: F821
    return path if os.path.isfile(path) else None


path = board_header()
if path is None:
    print("board_header_stamp: no BOARD_HEADER for this env, nothing to track")
else:
    with open(path, "rb") as f:
        stamp = hashlib.md5(f.read()).hexdigest()[:12]
    env.Append(CPPDEFINES=[("BOARD_HEADER_STAMP", "0x" + stamp)])  # noqa: F821
    print("board_header_stamp: %s is %s" % (os.path.basename(path), stamp))
