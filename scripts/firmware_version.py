# PlatformIO pre-build script: name the build, for the firmware update.
#
# services/firmware_update.cpp compares this build's version with the latest
# GitHub release's, and fetches the release's image named for this env. So it
# is compiled with two defines:
#
#   FIRMWARE_VERSION  the release tag on a tag's build in CI ("v1.6.0");
#                     elsewhere `git describe`: "v1.5.0" on a tagged commit,
#                     "v1.5.0-23-gc83026a" 23 commits after it, "-dirty" with
#                     changes not committed, a bare hash with no tag in reach
#                     (CI's shallow clone of a branch).
#   FIRMWARE_ENV      the env, which names its image in a release:
#                     firmware-<env>.bin.
#
# Only that one file gets them. Every file built with a define that changes
# with each commit would make every build a full rebuild.

import os
import subprocess

Import("env")  # noqa: F821  (SCons global)


def version():
    if os.environ.get("GITHUB_REF_TYPE") == "tag":
        return os.environ.get("GITHUB_REF_NAME", "unknown")
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty",
             "--match", "v[0-9]*"],
            cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
            capture_output=True, text=True, timeout=10)
        described = out.stdout.strip()
        return described if out.returncode == 0 and described else "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


VERSION = version()
ENV_NAME = env.subst("$PIOENV")  # noqa: F821
print("firmware_version: %s, %s" % (VERSION, ENV_NAME))


def stamp(env, node):
    return env.Object(
        node,
        CPPDEFINES=list(env["CPPDEFINES"]) + [
            ("FIRMWARE_VERSION", env.StringifyMacro(VERSION)),
            ("FIRMWARE_ENV", env.StringifyMacro(ENV_NAME)),
        ])


env.AddBuildMiddleware(stamp, "*firmware_update.cpp")  # noqa: F821
