# Home Assistant Media Remote

Firmware for an ESP32-S3 touch panel that drives a Home Assistant
`media_player`. Built with PlatformIO against
[pioarduino](https://github.com/pioarduino/platform-espressif32) (arduino-esp32
3.x / IDF 5.x).

```bash
pio run -t upload
```

## Layers

The panel is a build-time choice, not something the application knows about.

```
src/
  app/        what the remote decides and does   <- no display
  services/   Home Assistant and the network     <- no display
  ui/         one directory per panel
    headless/ draws nothing, narrates to serial
  hardware/   pins and peripherals
```

`include/ui/ui.h` is the seam. Above it nothing includes a graphics library;
below it, each panel implements the same handful of calls. `platformio.ini`
picks one with `build_src_filter`, so a new display is a directory and a
stanza rather than an `#if`.

`[env:headless]` is the default for now, and stays useful afterwards: if it
stops linking, something above the seam has grown a pixel.

## Fonts

Text is anti-aliased VLW, with **one embedded face per on-screen pixel height
the UI uses** -- 15, 17, 20 and 24 px -- each rendered natively at that size.
Nothing is scaled at runtime, because LovyanGFX's VLW scaler is
nearest-neighbour with no interpolation: a 15 px label taken from a 24 px
master is visibly chunkier than one rendered at 15 px.

Regenerate with:

```bash
python scripts/build_ui_font.py assets/fonts/NotoSans-Bold.ttf     --out-dir data --heights 15,17,20,24
```

The generator solves the EM size per target so the face's reported height --
`ascent + descent`, which is what `fontHeight()` returns and what a layout is
written against -- lands exactly on the requested pixel height.

175 glyphs: ASCII, the whole Latin-1 Supplement letter block, the Latin-1
punctuation those languages need, the OE ligature and capital sharp s, and
eight general-punctuation marks, because track titles arrive full of curly
quotes and en dashes. That sets Spanish, Portuguese, German and French
completely.

**U+0178 is deliberately excluded**, and the generator aborts if anything like
it is added back. LovyanGFX computes the line height from glyph extents but
skips U+00A0-U+00FF while doing it, precisely so accented capitals cannot
inflate it. Capital Y-diaeresis sits outside that range and is 4 px taller
than any ASCII letter, so including it solves every face one size smaller and
shrinks every label to leave room for an accent that is on essentially
nothing. See `check_metric_drivers()`.

The four faces are about 110 KB of flash. A panel embeds them with
`board_build.embed_files`; porting to a larger one means regenerating at the
sizes that panel asks for.

## Setup

On first boot the device brings up a captive portal on AP
**`MediaRemote-Setup`** (192.168.4.1) to collect Wi-Fi credentials. Once it is
on the network the same portal stays reachable at
`http://media-remote.local`.

Holding **BOOT** for 3 seconds clears the saved settings and reboots into the
portal. Touch is the primary input, so BOOT has no short-press action.
