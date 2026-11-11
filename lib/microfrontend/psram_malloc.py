# PlatformIO build script for this library alone (library.json): every file
# of it compiled with src/microfrontend_psram.h force-included, which points
# its malloc, calloc and free at PSRAM first. Appended to the compiler's
# flags here rather than given as library.json build flags, whose parser
# reads "-include <file>" as a library to link.
import os

Import("env")  # noqa: F821 -- PlatformIO's

header = os.path.join(env.subst("$PROJECT_DIR"), "lib", "microfrontend",  # noqa: F821
                      "src", "microfrontend_psram.h")
env.Append(CCFLAGS=["-include", header])  # noqa: F821
