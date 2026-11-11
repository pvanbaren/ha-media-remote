# PlatformIO pre-build script: compress embedded files the env asks for so.
#
# An env that lists data/deflated/<name>.z in board_build.embed_files gets
# data/<name> compressed into it: four bytes of the original length, little
# endian, then the zlib stream. hw::inflateEmbedded() turns it back into the
# original in PSRAM at run time (see include/hardware/inflate.h).
#
# The VLW faces are glyph bitmaps, which deflate to about a fifth; on the
# P4 4B, whose seven large faces were a third of a 4 MB app slot, that is the
# difference between the image fitting comfortably and barely.
#
# The compressed copies are outputs, not sources (.gitignore): written before
# every build, and only when what they would hold has changed, so that an
# unchanged one does not rebuild anything.

import os
import struct
import zlib

Import("env")  # noqa: F821  (SCons global)

PREFIX = "data/deflated/"


def embedded():
    files = env.GetProjectOption("board_build.embed_files", "")  # noqa: F821
    if isinstance(files, str):
        files = files.split()
    return [f.strip() for f in files if f.strip()]


project = env.subst("$PROJECT_DIR")  # noqa: F821
for target in embedded():
    if not (target.startswith(PREFIX) and target.endswith(".z")):
        continue
    source = os.path.join(project, "data", target[len(PREFIX):-2])
    out = os.path.join(project, target)
    with open(source, "rb") as f:
        raw = f.read()
    packed = struct.pack("<I", len(raw)) + zlib.compress(raw, 9)
    if os.path.isfile(out):
        with open(out, "rb") as f:
            if f.read() == packed:
                continue
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "wb") as f:
        f.write(packed)
    print("deflate_embeds: %s, %d -> %d bytes" % (target, len(raw), len(packed)))
