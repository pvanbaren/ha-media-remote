# PlatformIO post-build script: merge bootloader + partitions + app into one .bin
# Usage: pio run -t merge -e waveshare-s3

Import("env")

import os
from os.path import join


def merge_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    progname = env.subst("${PROGNAME}")
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    esptool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    boot_app0 = join(framework_dir, "tools", "partitions", "boot_app0.bin")
    merged = join(build_dir, "firmware-merged.bin")
    # The board definition supplies both; the fallbacks only matter if it
    # ever stops doing so, and they describe the board this project builds for.
    mcu = env.BoardConfig().get("build.mcu", "esp32s3")
    flash_size = env.BoardConfig().get("upload.flash_size", "16MB")

    bootloader = join(build_dir, "bootloader.bin")
    partitions = join(build_dir, "partitions.bin")
    firmware = join(build_dir, f"{progname}.bin")

    for path, label in (
        (bootloader, "bootloader.bin"),
        (partitions, "partitions.bin"),
        (boot_app0, "boot_app0.bin"),
        (firmware, f"{progname}.bin"),
    ):
        if not os.path.isfile(path):
            raise FileNotFoundError(f"Missing {label}: {path}")

    # `python -m esptool`, not the esptool.py shim beside it: esptool 5
    # deprecated that wrapper along with the underscore spellings of the
    # command and its options, and warns on every one of them. The package
    # directory is not on the interpreter's path by default, hence PYTHONPATH.
    env["ENV"]["PYTHONPATH"] = os.pathsep.join(
        p for p in (esptool_dir, env["ENV"].get("PYTHONPATH")) if p
    )

    cmd = [
        env.subst("$PYTHONEXE"),
        "-m",
        "esptool",
        "--chip",
        mcu,
        "merge-bin",
        "-o",
        merged,
        "--flash-mode",
        "keep",
        "--flash-freq",
        "80m",
        "--flash-size",
        flash_size,
        "0x0",
        bootloader,
        "0x8000",
        partitions,
        "0xe000",
        boot_app0,
        "0x10000",
        firmware,
    ]
    print(f"Merging flash image -> {merged}")
    env.Execute(" ".join(f'"{c}"' if " " in c else c for c in cmd))
    return None


env.AddCustomTarget(
    name="merge",
    dependencies="${BUILD_DIR}/${PROGNAME}.bin",
    actions=env.Action(merge_firmware, "Merging flash image for web flasher"),
    title="Merge firmware",
    description="Create firmware-merged.bin (bootloader + partitions + app)",
)
