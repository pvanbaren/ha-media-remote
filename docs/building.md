# Building and installing

Part of the [Home Assistant Media Remote](../README.md) documentation.

## Prerequisites

- **PlatformIO**, with the platform `platformio.ini` names: **pioarduino**
  (arduino-esp32 3.x on IDF 5.x). The stock `espressif32` platform predates
  these board definitions, and the two cannot share a machine happily: they
  want different `tool-esptoolpy` packages under one name, so each build
  strips the other's copy.
- **On Windows, build from PowerShell or cmd, not git-bash.** pioarduino's
  `idf_tools.py` aborts with "MSys/Mingw is not supported" whenever `MSYSTEM`
  is set.

Libraries are fetched by PlatformIO: LovyanGFX and WiFiManager.

## Building

```bash
pio run -e waveshare-s3      # 1.28" round (the default env)
pio run -e round360-s3       # 1.85" round 360x360
pio run -e qualia-720        # 4" square 720x720
pio run -e lcd28c-s3         # 2.8" round 480x480
pio run -e matouch21-s3      # 2.1" round 480x480
pio run -e headless          # no panel
```

Each board's build embeds only the fonts it uses and compiles only its own
`src/hardware/<board>/` and `src/ui/<shape>/` (see
[the code](code.md)).

GitHub Actions builds every board on every push and pull request
(`.github/workflows/build.yml`). Each run's page under **Actions** has the
images to download: `firmware.bin` for a network update and
`firmware-merged.bin` for a blank board.

**Releasing.** What changes goes into [`CHANGELOG.md`](../CHANGELOG.md), under
*Unreleased*, as it is made. To release, rename that heading to the version
and the date and push a `v` tag: the workflow drafts a GitHub release with
every board's images, its notes that version's section of the changelog
followed by `.github/release-footer.md`, and refuses a tag the changelog has
no section for. Publishing the draft puts it on the install page.

## First install, from the browser

**[pvanbaren.github.io/ha-media-remote](https://pvanbaren.github.io/ha-media-remote/)**
installs the latest release on a board plugged in over USB, with nothing to
build: click **Install** for the board and choose its port. It needs Chrome,
Edge or Opera on a computer (Web Serial; not Firefox, Safari or a phone).
The **Qualia** goes into download mode by hand first -- hold BOOT, tap RESET,
let go of BOOT -- since its USB port is the firmware's own and the browser
cannot restart it into the bootloader. The **2.8C** is there from v1.2.0, the
first release built with it.

It writes the release's `firmware-<board>-merged.bin` from address 0, so like
any merged image **it wipes the settings**: it is for a blank board, or one
being set up again. Update a remote that is already set up from its portal. Either way, the
remote then starts on its **Setup** card: see [Setting it up](../README.md#setting-it-up).

The page is `site/`, published to GitHub Pages by
`.github/workflows/pages.yml` whenever a release is published: the page, a
manifest per board with the release's tag written in, and the release's
merged images beside them. The images are copied rather than linked, since a
release download carries no CORS headers and the browser would refuse it.
The flashing is [ESP Web Tools](https://esphome.github.io/esp-web-tools/).
GitHub Pages has to be set to build from GitHub Actions once, under
**Settings -> Pages**, and the workflow can be run by hand to publish the
page again for the latest release.

## First install, over USB

```bash
pio run -e round360-s3 -t upload
pio device monitor
```

This writes the bootloader, the partition table, the OTA data and the app,
each at its own offset -- and leaves the settings partition alone, so a
device keeps its settings across a USB flash.

- **Waveshare**: a CH343 USB-serial bridge; the port auto-detects.
- **DFR1221**: the S3's own USB port.
- **2.8C**: the S3's own USB port too, whatever Waveshare's wiki says about a
  CH343.
- **MaTouch 2.1"**: the S3's own USB port, which also carries the console.
- **Qualia**: the firmware's USB port reboots into the bootloader on a
  different COM port partway through. If PlatformIO reports the old port gone,
  run the upload again, or name the bootloader's port with `--upload-port`.

Pin a port on one machine with an untracked `platformio-local.ini` beside
`platformio.ini` (`upload_port`, `monitor_port`); it is optional and globbed
in.

`pio run -t merge` writes `firmware-merged.bin`, one image from address 0 for
a web flasher. **It wipes the settings** -- it is padded over the settings
partition -- so use it for a blank board, not an update.

## Updating over the network

The flash has **two 4 MB app slots** (`partitions/media_remote.csv`), so a
running device takes new firmware without a cable: open **Update** in its
portal and upload `.pio/build/<env>/firmware.bin` -- the app image, not the
merged one. It is written into the slot that is not running, and the device
reboots into it once it has arrived whole. Settings are kept. From a shell:

```bash
curl -F "update=@.pio/build/round360-s3/firmware.bin" http://bedroom-remote.local/u
```

A device flashed before the table had a second slot answers "Partition Could
Not be Found" and carries on unharmed; one USB flash gives it the new table.

## Fonts

Text is anti-aliased VLW, one embedded face per pixel height the UI uses on
that board -- LovyanGFX scales VLW nearest-neighbour, so every size is
rendered natively rather than scaled. Regenerate from the bundled Noto Sans
Regular:

```bash
python scripts/build_ui_font.py assets/fonts/NotoSans-Regular.ttf \
    --out-dir data --heights 23,26,30,36
```

The generator solves a whole-pixel size for each height, and four land a
pixel high: 15, 23, 36 and 38 measure 16, 24, 37 and 39. That is inside the
pixel `display_font.cpp` allows before it rescales, so they are still drawn
natively.

Three lists must agree for a board: `--heights`, `board_build.embed_files` in
its env, and `kFonts[]` in its `src/hardware/<board>/font_table.cpp`.

| Board | Faces |
|---|---|
| Waveshare | 15, 17, 20, 24 |
| DFR1221 | 23, 26, 30, 36 |
| Qualia | 34, 38, 45, 54, and 23, 26, 30 for the lists |
| 2.8C | 23, 26, 30, 36 -- the DFR1221's, at a text scale of 0.75 |
| MaTouch 2.1" | 23, 26, 30, 36 -- the 2.8C's |

The character set is 305 glyphs: ASCII, the Latin-1 letters and
punctuation, all of Latin Extended-A, Romanian's ș and ț, ẞ, and the dashes
and curly quotes track titles are full of -- the languages of western and
central Europe, Romanian, Turkish and the Baltic ones. A missing glyph
renders as a space.

Most of Latin Extended-A's capitals -- Č, Ś, Ž, Ă, Ÿ -- carry a mark above
the cap height, and LovyanGFX sets a face's line height from its tallest
glyph, sparing only U+00A0..U+00FF. Left to that, they would make every face
report a taller line and every label shrink to fit it. So the generator
measures the header without them, and `display_font.cpp` puts each face's
line height back to the header's when it loads the face: they overhang the
line, as À and É always have. A glyph outside both that would set the line
height still makes the generator stop.
