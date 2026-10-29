# PlatformIO pre-build script: pin managed IDF components for an env that
# builds ESP-IDF from source (custom_sdkconfig).
#
# Set custom_component_pins in the env, one name@version a line. The Arduino
# framework's component manifest asks for ranges -- esp_hosted ^2.9.2,
# esp_wifi_remote ^1.2.2, mdns ^1.11.2 -- which the prebuilt libraries
# resolved once, when they were built, and a source build resolves afresh to
# whatever is newest. On the P4 4B that moved a dozen components at once,
# and esp_wifi_init failed with ESP_ERR_INVALID_ARG. So a source build is
# pinned to what the prebuilt libraries were built with (their versions.txt),
# with only the deliberate changes on top.
#
# The pins go into the manifest of the project pioarduino compiles the
# libraries in, .dummy/ in the project folder, as exact versions for the
# component manager to solve together with the Arduino framework's ranges.
# Not into the framework's own manifest: pioarduino reinstalls the framework
# whenever custom_sdkconfig changes -- after this script has run -- and the
# edit went with it. Nor with custom_component_remove/_add, which deletes a
# removed component's headers after the build even when it is added back.
#
# And a change of pins rebuilds the libraries, which pioarduino would not do
# on its own: it rebuilds them when custom_sdkconfig changes, which it finds
# by a hash of it on the first line of sdkconfig.defaults, and a pin is not
# part of that hash. On the P4 4B the libraries went on being built with
# ESP-Hosted 2.13.0 after the pin said 2.13.1, and the Wi-Fi wedge that
# 2.13.1 fixes stayed. So when the pins differ from the last build's, this
# deletes sdkconfig.defaults, and pioarduino rebuilds as for a new sdkconfig.

import os
import re
import shutil

Import("env")  # noqa: F821  (SCons global)

BEGIN = "  # pin_components: begin\n"
END = "  # pin_components: end\n"

pins = env.GetProjectOption("custom_component_pins", "")  # noqa: F821
pins = [p.strip() for p in pins.splitlines() if "@" in p]
if pins:
    project = env.subst("$PROJECT_DIR")  # noqa: F821
    dummy = os.path.join(project, ".dummy")
    if not os.path.isdir(dummy):
        # As pioarduino would make it, a moment later.
        shutil.copytree(os.path.join(env.PioPlatform().get_dir(),  # noqa: F821
                                     "builder", "build_lib"), dummy)
    path = os.path.join(dummy, "idf_component.yml")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    # The block this script wrote last time, out; this time's in, at the end
    # of the dependencies map, which is the file's last.
    block_re = re.escape(BEGIN) + r".*?" + re.escape(END)
    last = re.search(block_re, text, flags=re.S)
    text = re.sub(block_re, "", text, flags=re.S)
    mcu = env.BoardConfig().get("build.mcu")  # noqa: F821
    block = BEGIN
    for pin in pins:
        component, version = pin.split("@", 1)
        block += ('  %s:\n    version: "==%s"\n    rules:\n'
                  '      - if: "target == %s"\n' % (component, version, mcu))
    block += END
    # Pins that differ from the last build's: have the libraries rebuilt.
    stamp = os.path.join(project, "sdkconfig.defaults")
    if (last is None or last.group(0) != block) and os.path.exists(stamp):
        os.remove(stamp)
        print("pin_components: pins changed; the libraries will be rebuilt")
    text = text.rstrip("\n") + "\n" + block
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print("pin_components: %s on %s (%s)" % (path, mcu, ", ".join(pins)))
