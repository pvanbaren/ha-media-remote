# Home Assistant Media Remote

Firmware for the **Waveshare ESP32-S3-Touch-LCD-1.28** — a 1.28" round GC9A01
at 240×240 with a CST816S touch panel, a switchable backlight, 16 MB flash and
2 MB PSRAM, all integrated. Pick a Home Assistant `media_player`, then skip
back / play-pause / skip next, with the current track's cover art as the
backdrop and the track text over it.

```bash
pio run -t upload
```

## Layers

The panel is a build-time choice, not something the application knows about.

```
src/
  app/        what the remote decides and does   <- no display
  services/   Home Assistant and the network     <- no display
  ui/
    common/   every screen that does not care what shape the panel is
    round/    theme.cpp, now_playing.cpp         <- 240 px GC9A01 circle
    square/   theme.cpp, now_playing.cpp         <- 720 px Qualia square
    headless/ draws nothing, narrates to serial
  hardware/
    waveshare/  qualia/  round360/  headless/    <- one directory per board
```

Three panels, and the square one needed **two files**; the second round
one needed none of them. Of the ten under
`src/ui/`, eight never ask what shape the panel is: `browse_list`, `search`,
`status_screens` and `ui.cpp` lay out by calling `chordHalfWidth()`, and
`artwork`/`canvas`/`cover_art`/`text` deal in pixels and URLs. Only `theme.cpp`
(which answers the chord question) and `now_playing.cpp` (the one screen with
a circular idea in it) are per-shape.

`include/ui/ui.h` is the seam, and the input half is what earns it. A display
does its own hit testing -- only it knows where it drew the buttons -- and
hands back what the gesture *meant*: `kPlayPause`, `kSetVolume`,
`kPlayBrowseRow`. This panel reads a swipe round its bezel as a volume change;
a square one might use a strip down the side. Both return the same intent, and
`src/app/` is none the wiser.

Nothing above the seam includes a graphics library. `platformio.ini` picks an
implementation with `build_src_filter`, so a new display is a directory and a
stanza rather than an `#if`.

```bash
pio run -e waveshare-s3      # 1.28" round GC9A01 (default)
pio run -e qualia-720        # 4" square 720x720 on an Adafruit Qualia
pio run -e round360-s3       # 1.85" round 360x360 ST77916, quad SPI
pio run -e headless          # no panel at all
```

`round360-s3` is a **DFRobot DFR1221** round display board. It is easily
mistaken for a Waveshare ESP32-S3-Touch-LCD-1.85 -- same size, shape and
controller, different wiring -- which is why the header is named for the panel
and carries its provenance.

`[env:headless]` is not a toy. It builds and runs the whole remote -- polling,
the idle timer, every service call -- narrating to the serial console instead
of drawing. It is useful while a new panel is being brought up, and a standing
check that nothing has leaked back across the seam: if it stops linking,
something in `src/app/` or `src/services/` has grown a pixel.

## The Qualia square panel

`env:qualia-720` drives an **Adafruit Qualia ESP32-S3 for RGB-666**
([5800](https://www.adafruit.com/product/5800)) carrying the **4" square
720x720 capacitive panel** ([5794](https://www.adafruit.com/product/5794),
TL040HDS20). 16 MB flash, 8 MB octal PSRAM.

**Build this env from PowerShell or cmd, not git-bash.** pioarduino's
`idf_tools.py` aborts with "MSys/Mingw is not supported" whenever `MSYSTEM` is
set.

### It is a different kind of panel, not just a bigger one

The GC9A01 is an SPI display with a command channel. This one is
RGB-parallel -- the S3's LCD peripheral streams a framebuffer continuously over
16 data lines plus PCLK/HSYNC/VSYNC/DE, and there is **no command channel at
all**. Three consequences run through `src/hardware/qualia/`:

- **The panel's own config lines are not GPIOs.** Reset, the backlight and two
  user buttons hang off a TCA9554 I2C expander at `0x3F` (SDA 8 / SCL 18).
- **There is no controller to program.** The TL040HDS20 wants a reset pulse and
  nothing else; Adafruit's CircuitPython driver for it ships an empty init
  sequence, where the round 720x720 NV3052C needs about a hundred register
  writes.
- **There is no BOOT button.** GPIO 0 is an RGB data line here, carrying blue
  bit 3. The expander's DOWN button stands in for it, so `wifi_setup.cpp` needs
  no notion of which board it is on.

### One framebuffer, scrolled in place

`hardware/qualia/rgb.cpp` configures esp_lcd with **one framebuffer** and no
bounce buffer, so GDMA reads straight out of PSRAM and every repaint writes
the buffer being scanned. Screens compose into a separate 1 MB canvas and
`rgbPresent()` copies the rectangle that changed across.

**Why one.** The binding constraint is memory bandwidth. An RGB panel has
no frame memory, so the scan-out reads continuously: at 12 MHz that is
24 MB/s of PSRAM the panel cannot be starved of a byte of. A second buffer
would always be a flip behind, so it would never hold the frame on the glass
-- and a scrolling list could only ever be recomposed and copied, 624 rows a
step. One buffer does hold it, which is what makes the scroll below
possible.

**A scroll moves the rows already on the glass.** Every row a list scroll
keeps is on the panel already, pixel for pixel, only somewhere else -- on a
square panel a row does not change shape with its height. So
`browse_list::redrawRows()` moves the band in place (`rgbScroll()`, through
`canvasScrollPanel()`), then composes and presents only what the move cannot
supply: the strip it uncovers, and the narrow column the scroll bar runs
down. That column is left out of the move -- the thumb travels the opposite
way to the rows, so moving it with them showed it jumping backwards first --
and repainted once the rows are in place. The bar sits flush with the right
edge for this reason: its columns are then the end of every row, and each
row moves as one run rather than two either side of it. The search results do the same
through `search::redrawResults()`, less the column, having no scroll bar;
their background is the list's flat one on the square panel for the same
reason -- a circle behind full-width rows is a background that changes with
height, which no row can be moved across.

The canvas is left behind while this happens -- outside the two strips it
still holds the rows where they were -- which is safe for as long as nothing
else presents over the band. `canvasPanelWrites()` is how the list knows:
it counts every write to the panel, and a count that moved since the list's
own last write means someone else drew there, so the next repaint is a
whole one. So is anything that changes what rows *show* rather than where
they are -- a thumbnail arriving goes through `repaintRows()` -- and a jump
of more than half the view, which is mostly new rows anyway. The round
boards never take this path: their panels hold the frame behind an SPI bus
that only writes, and their rows change width with height.

What keeps the single buffer from breaking up the picture:

- `rgbPresent()` copies **only the rectangle that changed**, so the elapsed
  chip costs ~84 rows, not a megabyte.
- `copyRegion()` and `rgbScroll()` stand off the bus once per
  `kCopySlabRows` full-width rows' worth of bytes, letting GDMA refill the
  FIFO while the CPU waits -- bytes, not rows, so a narrow column does not
  sleep as often as a full-width band. Composing does the same every
  `kComposeSlabRows`.
- `board::kListRedrawMinMs` stops a scrolling list asking for repaints faster
  than one can finish.
- The every-VSYNC restart pulls the picture back if a burst still gets
  through, so an underrun costs a frame rather than the session. Arduino's
  prebuilt libraries already turn it on, so the Qualia builds against them
  like every other board. Their handler lives in flash, which lets a flash
  write -- a portal save -- delay it by a frame; moving it to IRAM would need
  the IDF built from source, and that one frame is all it would buy.

### Things that will bite

| Symptom | Fix |
|---|---|
| All colours look inverted / byte-swapped | `board::kFrameNativeByteOrder`. esp_lcd reads the framebuffer as native little-endian; LovyanGFX's default RGB565 is byte-swapped for SPI |
| Touch does nothing | The FT6336 is at **`0x48`**, not the usual `0x38`. The boot log prints an I2C scan |
| Image offset sideways | Swap `kRgbHsyncBackPorch` / `kRgbHsyncFrontPorch`. Adafruit and the panel datasheet disagree about which is which; blanking totals match, so only the offset differs |
| Image torn or shimmering | `kRgbPclkActiveNeg`. This panel latches on the falling edge; the round one does not |
| Image shifted vertically after a reboot, fine after a power cycle | Already handled by the `periph_module_reset` in `rgbInit()` |
| Picture breaks up or jumps sideways while a list scrolls | PSRAM bandwidth: the copy or the in-place move is starving the scan-out. Lower `kCopySlabRows` or raise `board::kListRedrawMinMs` to spread it out. Dropping `kRgbPclkHz` is how to confirm it, not how to fix it. If the shift *persists* rather than lasting a frame, the VSYNC restart is off |
| Rows in a scrolled list show stale content, or the scroll bar leaves a trail | The in-place scroll built on a panel that no longer held the list. Something presented over the band without going through `ui::canvasPresentRegion()` or `canvasScrollPanel()`, so `canvasPanelWrites()` did not see it |
| Home Assistant requests take tens of seconds, though the Wi-Fi is fine on every other device | The panel bus is desensing the radio. `kRgbPclkHz` and the drive strength in `quietenBus()` -- see below |

### The panel bus talks over the Wi-Fi

Nineteen pins switch at the pixel clock a few centimetres from a 2.4 GHz
front end, and on this board that is not a theoretical concern. At the
default GPIO drive strength every Home Assistant request took tens of
seconds -- one succeeded in 72 -- while a PC on the same network completed
the TLS handshake to the same server in 4 ms.

It presents as anything but a radio problem. DNS resolves to the right
address, the heap is healthy, the server is fast, and **RSSI reads -54 dBm**
throughout. RSSI is the strength of frames that *did* arrive, so it cannot
show the noise they arrived over and says nothing about whether this end is
being heard at all. A strong RSSI beside a request that timed out is the
ordinary look of a lossy link, not a contradiction.

Two measurements isolate it. Dropping `kRgbPclkHz` to 12 MHz cut that 72 s
request to 4.3 s -- and sped up no PSRAM copy at all, so the MSPI was never
the bottleneck and the CPU was never starved of it. Only the radio changed.

Radiated emission follows edge rate as well as clock, so there are two
levers and they add rather than substitute. Measured at 16 MHz, on the
first request after boot and then in steady state once keep-alive holds the
connection open:

| Drive | First request | Steady state |
|---|---|---|
| `GPIO_DRIVE_CAP_2` (~20 mA, the default) | 72 s | connects timing out at 6 s |
| `GPIO_DRIVE_CAP_1` (~10 mA) | 11.4 s | 26.7 / 9.4 / 2.7 s |
| `GPIO_DRIVE_CAP_0` (~5 mA) | 0.5 s | 28 / 16 / 16 ms |

For scale, 12 MHz at the *default* drive reached 77 ms in steady state, so
either lever pushed far enough is sufficient on its own.

It ships at **12 MHz and `CAP_0`**. The drive alone carries the radio; the
clock is down for PSRAM bandwidth instead, because at 16 MHz a scrolling list
left the frame shifted sideways (see the framebuffer section above). The cost
is 19.5 Hz on the glass instead of 26.1. 5 mA into a ribbon is thin for
signal integrity: it works, and it is the first thing to doubt if the panel
shows speckle or dropped columns.

`quietenBus()` in `src/hardware/qualia/rgb.cpp` is called after
`esp_lcd_new_rgb_panel()`, which configures those pins itself and would
otherwise overwrite the capability. HSYNC and VSYNC are left alone: they
switch at a thousandth of the data rate, so they would buy nothing for the
risk of a weakened sync edge.

**If the glass speckles, drops columns or shifts, the drive strength is the
first thing to put back** -- `GPIO_DRIVE_CAP_2` is the default.

The cost lands almost entirely on the TLS handshake, which is the exchange
needing several round trips in a row. That is why one boot could show both
72 s and 77 ms, and why the number to watch when changing any of this is
**the first request after boot**, not the steady state.

### Adding a panel

1. `include/board/<board>.h` -- pins, panel size, touch raster.
2. `src/ui/<shape>/` -- the drawing, and `poll()` turning touches into intents.
   Only if the shape is new: `round360-s3` reuses `ui/round/` unchanged.
3. `src/hardware/<board>/` -- `display.cpp`, `touch_raw.cpp`, `font_table.cpp`,
   `boot_button.cpp`.
4. An env in `platformio.ini` that excludes the other `ui/` and `hardware/`
   directories -- and remember to exclude the new one from the existing envs,
   which is the easy half to forget.

Regenerate the fonts at the sizes the new panel asks for; `theme::textPx()`
scales every text size by `kUiScale` and then by the board's `kTextScale`, so
a 360 px round panel (1.5, 1.0) wants 23/26/30/36 px faces and the 720 px
Qualia (3.0, 0.75) 34/38/45/54, rather than rescaled 24 px glyphs.
`kTextScale` is for a panel whose pixels are denser than the 240 px design's,
where scaling by pixel count alone makes the type too big; layout ignores it.
LovyanGFX's VLW scaler is nearest-neighbour, which is why each size is its own
face:

```bash
python scripts/build_ui_font.py assets/fonts/NotoSans-Bold.ttf \
    --out-dir data --heights 23,26,30,36
```

Three lists have to agree: `--heights` here, `board_build.embed_files` in the
env, and `kFonts[]` in that board's `font_table.cpp`.

Hardware and project layout follow
[ESP32-Plane-Radar](../ESP32-Plane-Radar); the Home Assistant integration
follows [home-assistant-epaper-remote](https://github.com/pdvb/home-assistant-epaper-remote)
in talking to the REST API directly, with no add-on or custom component on the
server.

## What it does

1. **Wi-Fi setup** (if needed) — captive portal on AP **`MediaRemote-Setup`**
2. **Link to Home Assistant** — base URL and long-lived token, entered in the
   same portal
3. **Pick a player** — a dropdown in that same portal, listing every
   `media_player` entity, plus a second one for whatever carries **volume and
   power**
4. **Now playing** — cover art backdrop, track title and artist, a draggable
   volume arc round the bezel, the three transport buttons, and an elapsed /
   total chip in the gap beneath them
5. **Swipe up** — recent artists and a recommended shelf, with artwork, one
   tap to play
6. **Swipe left from there** — type an artist's name and play their radio

**The player is chosen in the portal, not on the device.** A `<select>` in a
browser is a better list than forty rows on a 240 px circle, it is reachable
when the panel is not, and it takes the device's only list gesture back for
something used more than once a year. The device screen is now purely a
*remote*: it shows the track, not the thing playing it. Nothing on it names the
player.

The selection is remembered in NVS, so the device comes back to it after a
reboot. Until one is stored the device shows a card pointing at the portal,
and the dropdown reads *Choose a player…*. `config::kDefaultPlayerEntityId`
can name a default instead, for a build that should land on the now-playing
screen straight after setup.

## Controls

| Gesture | Effect |
|---------|--------|
| **Tap a transport button** | Previous track / play-pause / next track |
| **Swipe sideways, top half** | Set volume. Anywhere up there, not on the arc |
| **Swipe up** on now-playing | Open the browse list |
| **Swipe left** in the list | Search for an artist |
| **Drag** in the list | Scroll it; it follows your finger and glides on release |
| **Tap an item** | Play it; the item stays on screen while it starts |
| **Tap the title strip** or **swipe right** | Leave the list |
| **Touch a blanked panel** | Wake it, and switch the volume/power entity on |
| **Hold BOOT 3 s** | Clear Wi-Fi *and* HA settings, reboot into setup |

Touch is the only input for normal use. BOOT is the escape hatch and has no
short-tap action.

## Wiring

Not a choice — this is the board variant's own pinout
(`framework-arduinoespressif32/variants/waveshare_esp32s3_touch_lcd_128/pins_arduino.h`).
The touch controller shares its I2C bus with an unused QMI8658 IMU.

| Signal | GPIO |
|--------|------|
| LCD RST | 14 |
| LCD CS | 9 |
| LCD SDA (MOSI) | 11 |
| LCD SCL (SCLK) | 10 |
| LCD DC | 8 |
| LCD backlight | 2 (PWM) |
| Touch SDA | 6 |
| Touch SCL | 7 |
| Touch INT | 5 |
| Touch RST | 13 |
| BOOT button | 0 (on-board, active LOW) |

If reds and blues look swapped, flip `kDisplayRgbOrder` (true = RGB,
false = BGR); this panel is BGR, and the error is easy to miss on flat UI
colour but obvious on a photograph. `kDisplayInvert` is the other panel quirk
worth knowing about.

If taps land mirrored or transposed, flip `kTouchSwapXy` / `kTouchInvertX` /
`kTouchInvertY` in `config.h`.

## Porting to a different round display

**One value** controls the whole layout:

```cpp
// include/config.h
constexpr int kDisplayDiameter = 240;
```

Every dimension in [`include/ui/theme.h`](include/ui/theme.h) is authored
against a 240 px baseline and multiplied by `kUiScale`, so a different diameter
re-lays-out the interface proportionally rather than cropping it. Glyphs are
drawn as vectors, not bitmaps, so they stay crisp at any size. The picker even
shows more rows on a larger panel, because its row count comes from the
geometry.

Text is clipped to the *chord* of the circle at its own y rather than to a
bounding box, so nothing runs off the curved edge at any diameter.

Two things to check when you scale up:

- The frame buffer is diameter² × 2 bytes of PSRAM, so a bigger panel costs
  memory rather than passes. 480 px would be 460 KB of the 2 MB available.
- A non-GC9A01 controller means editing
  [`include/hardware/lgfx_config.hpp`](include/hardware/lgfx_config.hpp) to the
  matching LovyanGFX panel class.

## Fonts

Text is anti-aliased VLW, with **one embedded font per on-screen pixel height**
the UI uses — 15, 17, 20 and 24 px — each rendered natively at that size.
Nothing is scaled at runtime.

That matters because LovyanGFX's VLW scaler is nearest-neighbour, with no
interpolation: a 15 px label taken from a 24 px master is visibly chunkier than
one rendered at 15 px. The firmware used to embed a single 15 px face and reach
every other size through `setTextSize()`, which is exactly that trade in the
other direction.

`displayFontApplyHeight(gfx, px)` picks the font whose native height matches and
draws it at `setTextSize(1.0)`. It measures each font's height at boot rather
than trusting the filename, keeps a one-slot cache so a run of same-size draws
reloads once, and falls back to a fractional rescale of the nearest face only
when the request is more than a pixel away from anything embedded — which,
for every size in `ui/theme.h`, never happens.

Regenerate with:

```bash
python scripts/build_ui_font.py assets/fonts/NotoSans-Bold.ttf \
    --out-dir data --heights 15,17,20,24
```

The generator solves EM size per target so the font's reported height —
`ascent + descent`, which is what `fontHeight()` returns and what the layout is
written against — lands exactly on the requested pixel height. Keep the
`--heights` list, `board_build.embed_files` and `s_fonts[]` in
`display_font.cpp` in step.

### Charset

175 glyphs: ASCII 33–126, the whole Latin-1 Supplement letter block
(U+00C0–U+00FF less the two maths operators), the Latin-1 punctuation those
languages need (¡ ¿ « » ª º · °), Œ/œ and ẞ, and eight general-punctuation
marks (– — ‘ ’ “ ” „ …) because track titles arrive full of them. Space is
derived from the advance width rather than stored.

That sets **Spanish, Portuguese, German and French** completely, with Italian,
Catalan, Dutch and the Nordic languages as a side effect of taking the Latin-1
block whole. A codepoint the font does not have renders as a blank space, not
as garbage — LovyanGFX falls back to `drawCharDummy()`.

**U+0178 (Ÿ, French capital Y with diaeresis) is deliberately excluded**, and
the generator will abort if anything like it is added back. LovyanGFX computes
the line height from the glyph extents but skips U+00A0–U+00FF while doing it
(`VLWfont::loadFont`), precisely so accented capitals cannot inflate it. Ÿ
sits outside that range, and it is 4 px taller than any ASCII letter — so
including it solved every font one size smaller and shrank every label on the
panel by about 8%, to leave room for an accent that is on essentially nothing.
Lowercase ÿ is inside Latin-1 and costs nothing, so it is kept.
`check_metric_drivers()` in the generator enforces this.

The four fonts cost about 110 KB of flash, up from 56 KB for the ASCII-only
set. Runtime cost is unchanged: VLW bitmaps stay in flash and only the metric
arrays are allocated, about 1.6 KB for whichever font is loaded, PSRAM first.

**Porting to a larger panel means regenerating them**: `theme::px()` scales
every size by `kUiScale`, so a 480 px panel asks for 30/34/40/48 px and would
otherwise get rescaled 24 px glyphs.


## Compositing

Every screen draws into one 240×240 16bpp sprite in PSRAM — 115 KB — and
`canvasPresent()` pushes it to the panel over SPI. One pass, no tearing, and
`ui::canvas()` is simply the buffer. Everything draws in screen coordinates:
nothing takes a y offset and no drawing code converts between spaces.

Artwork is cached **decoded**, both the cover backdrop and the list
thumbnails. These are network resources — the cover comes from Home Assistant
and the thumbnails from the provider's CDN — so decoding at draw time would
mean re-fetching over TLS on every repaint. Holding the pixels is the only
shape that works.

If the buffer cannot be had, `canvas()` returns the panel instead: everything
still draws, repaints tear, and the scrim helpers go quiet. On 2 MB of PSRAM
that only happens if the PSRAM is faulty.

## Setup

### Get a Home Assistant token

In Home Assistant: click your username (bottom left) → **Security** → **Create
Token** under *Long-lived access tokens*. Copy it — HA shows it once.

### Flash and configure

```bash
pio run -t upload
```

1. Join the **`MediaRemote-Setup`** access point
2. Open **`http://media-remote.local`** (or **`http://192.168.4.1`**) — the
   captive portal may open on its own
3. Set your Wi-Fi, the **Home Assistant URL** (e.g.
   `http://homeassistant.local:8123`, no trailing slash or path) and paste the
   **token**. Optionally paste the **Music Assistant config entry id** as well,
   for the swipe-up list
4. Save; the device reboots onto your network and shows the default player
5. Reload the portal once it is connected and pick a different player from the
   **Media player** dropdown, if you want one

The same portal stays available on the LAN afterwards, at
`http://media-remote.local` -- or whatever name you give the device, below --
or the device's IP; use it to change the URL or token later. Leaving the token field blank keeps the stored one; the field
never echoes it back.

### Device name

The portal's **Device name** field is what the device calls itself on the
network: its hostname, so your router's client list shows it, mDNS answers
for **`<name>.local`**, and the portal's heading carries it instead of
`esp32s3-XXXXXX`. It defaults to `media-remote`. Useful as soon as there is
more than one of these -- `kitchen-remote.local` and `bedroom-remote.local`
rather than two devices fighting over one name.

What you type is cleaned into a valid hostname: lowercase letters, digits and
single hyphens, spaces and underscores turned into hyphens, at most 31
characters. Saving a different name restarts the device a moment later, since
the network stack only takes a hostname when the interface comes up. Like the
rotation it is stored apart from the Home Assistant settings, so a BOOT reset
keeps it -- and the setup card then tells you the name to look for.

### Finding the Music Assistant config entry id

The swipe-up list calls `music_assistant.get_library` and
`music_assistant.search`, and both take a **`config_entry_id`** -- Home
Assistant's internal handle for one configured integration. Music Assistant
never shows it to you, and it is not the server's name, its host or an
entity id.

It is typed into the portal rather than picked from a dropdown the way the
player is, and that is a limitation rather than a preference: no REST
endpoint lists config entries for a long-lived token, so the device cannot
look it up the way it enumerates `media_player` entities.

It looks like `01KM24MRTQZSBJKRTQB1JAGV0S` -- 26 characters, uppercase.
Entries created by older Home Assistant versions are 32 lowercase hex
characters instead. Either fits the portal field, which holds 40.

**From Developer tools.** The only route that shows the id beside a readable
name, which matters if you run more than one Music Assistant server:

1. **Developer tools -> Actions**
2. Choose the action **Music Assistant: Get Library**
3. In the **Config Entry Id** picker, select your Music Assistant server
4. Switch the editor to **YAML mode** (the three-dot menu, top right)
5. Copy the value on the `config_entry_id:` line

**From the URL.** **Settings -> Devices & services -> Music Assistant**,
click through to its devices or entities, and the address bar carries
`?config_entry=01KM24MRTQZSBJKRTQB1JAGV0S`.

Paste it into the portal's **Music Assistant config entry id** field.
Leaving it empty is a supported configuration: now-playing, transport and
volume all work without it and only the swipe-up list is unavailable, which
is what that list's **Not set up** card means.

### Choosing the player

The portal carries a **Media player** dropdown listing every `media_player`
entity, with the current one selected. This is the only place the choice is
made; there is no picker on the device.

It only appears once the device can reach Home Assistant, so on a first run
save the URL and token, let the device reconnect, then reload the portal and
the list will be populated. The choice is stored in NVS and takes effect on the
next poll, **without a reboot** — a device sitting on the "No player" card
rechecks every two seconds and moves on by itself.

The list itself lives in
[`services/player_list.cpp`](src/services/player_list.cpp), cached for
`kHaPlayerListTtlMs`, so reloading the portal page does not re-ask Home
Assistant for something that changes on the order of never.

### Volume and power can be a different entity

Below the player dropdown is a second one, **Volume & power**, defaulting to
*Same as the media player*. Set it when the thing that plays is not the thing
that makes the room loud — a Music Assistant player streaming into an
amplifier or receiver, where the player's own `volume_level` is missing or a
software gain nobody wants to touch, the real knob belongs to the receiver, and
the receiver is also what has to be switched on before any of it is audible.

What follows the control entity, and what does not:

| | Entity |
|---|---|
| Cover art, title, artist, elapsed | the **player** |
| Previous / play-pause / next | the **player** |
| Volume slider, mute, `VOLUME_SET` | the **control entity** |
| `turn_on` | the **control entity** |

Both come out of the **same** template render, so the second entity costs no
extra request: `kStateTemplateFmt` takes two entity ids and reads
`volume_level`, `is_volume_muted`, `supported_features` and `state` from the
control one. Where nothing separate is configured the two ids are the same
string and every field means what it always did.

An empty value is a real choice here rather than a missing one — it stores
"follow the player" — so unlike the URL and token fields, saving it blank
writes it.

### When the player wakes from standby

On the edge from standby into activity — idle, off, unavailable or unknown
becoming playing or paused — the device does two things, and they are two
halves of the same handoff:

1. **Switches the amplifier on**, because nothing the player does is audible
   until it is.
2. **Pins the player's own volume** to `kPlayerWakeVolume` (0.70).

The second needs explaining. When a separate entity carries volume and power,
the *player's* `volume_level` stops being a volume and becomes the **gain on
the signal handed to the amplifier**. Whatever it happens to be when the player
wakes is arbitrary, and a value that drifted low once poisons everything after
it: the amplifier gets turned up to compensate for a quiet source, and the next
track through a correctly-set player is deafening. Pinning it means the
amplifier's own setting means the same thing from one session to the next.

Note which entity each targets — `turn_on` to the **control entity**,
`volume_set` to the **player**. Power goes first, so the amplifier has the
length of the second round trip to wake before there is anything to hear.

Three conditions, all of them load-bearing:

- **Only when the control entity is separate.** When the two are the same
  thing, the volume the user last chose is the volume they want back, and
  powering it on is already covered by the paths below.
- **Only on the edge**, not on every poll, so it never fights the slider.
- **Never on the first poll after boot.** The first observation is not a
  transition, and treating it as one would yank the volume of a system that
  was playing happily before the remote was switched on.

Paused counts as active, so resuming a paused player is not a fresh start.
Set `kPlayerWakeVolume` negative to keep the power-on and drop the volume pin.

### Powering the control entity on

Several things switch it on, all of them chosen because they already mean
"I want this now":

- **Touching a blanked panel.** Reaching for a dark screen is the request.
  Waking because playback resumed does *not* do it — whatever started the
  music clearly did not need the help. The call is handed to the poll task
  rather than made on the touch path, so the screen lights immediately instead
  of holding black for a round trip.
- **Pressing play.** An amplifier that is off will not make a sound whatever
  the player does, so play powers it and *then* plays, in that order, so the
  amp is awake before the audio starts. Only play: skip-next in a dark room is
  a mis-tap, not a request for music.
- **Choosing something from the swipe-up list.** Same reasoning — picking an
  artist or a station is a request to hear it. Ordered before `play_media`, so
  the amp has that call's several seconds to wake up.
- **The player waking from standby**, as above.

`turn_on` is skipped when the last snapshot says the entity is already on, or
that it has no `TURN_ON` feature bit — calling it anyway would just fill the
Home Assistant log. An entity reporting no features at all is treated as
supporting everything, as everywhere else here.

**A zone amplifier also gets its input.** Switching a Triad output on routes
nothing to it: it comes up silent, and its integration drops it again unless
the linked player starts playing first. So where the volume/power entity lists
the player among its sources -- by the player's own name, "Stonebridge Master
bedroom" on a Triad whose input is linked to that player -- powering it on also
calls `media_player.select_source` with it. Straight after the `turn_on`, unless
the zone is already on that input. For a zone that was on already it depends
on what asked: **play** pressed on the remote takes the zone over whatever it
is on -- someone in the room wants this player -- while anything else only
fills a zone with no input at all, so one deliberately switched to another
source is left alone. A press that pauses never touches the source. Anything
without such a source -- a receiver, a speaker -- is untouched. `config::kSelectControlSource = false` turns it off.

### ...and off again

`kTurnOffControlOnBlank` switches it off at the moment the panel blanks —
the same `kIdleTimeoutMs` (5 minutes) of idleness, not a second timer running
from darkness. One timer, because it is one decision: a screen could stand
down sooner, since the only thing a short timer has to clear is the gap
between two tracks, but an amplifier wants a claim that has held for a while,
so the longer of the two sets the pace and the screen waits with it.

That claim is narrow by construction. Standing down already requires the
player to be paused, idle, off or unavailable for the whole timeout, and the
panel wakes the instant that stops being true — so the timer cannot run
while anything is playing, and a touch or resumed playback cancels it. The
call goes out once per stretch of darkness, and only to an entity whose
`supported_features` claim `TURN_OFF`.

`kBlankWhenPaused` is **on** for this to mean anything. It was off originally
— a paused player is often one you are about to resume, and the screen saying
what it is holding has its own value — but leaving it off would mean a pause
never leads to the amplifier switching off, which is the case that most wants
it. Set it back to `false` if you would rather a pause kept the screen lit, and
accept that only a genuine stop then powers down.

Set `kTurnOffControlOnBlank` to `false` to never power off, and
`kTurnOnControlOnWake` to `false` to drop the wake half too.

WiFiManager only renders `<input>` elements, so the `<select>` is stuffed
through its custom-attribute slot and a hidden input carries the value — which
means the stored selection survives even if the dropdown never fires. The
option list is held in a buffer sized against the real player count, because
`WiFiManagerParameter` keeps a pointer to it rather than copying, and that
buffer is permanent: every kilobyte spent there is one the TLS handshake and
the cover art cache do not get.

**HTTP vs HTTPS:** both work. HTTPS skips certificate validation, since local
installs are usually behind a self-signed or internal-CA certificate and the
board has no clock at boot to check validity dates against. Pin a root CA in
`ha_client.cpp` if you need the server authenticated.

**The connection is held open** (`kHaKeepAlive`). A fresh `WiFiClientSecure`
per request meant a full TLS handshake on *every poll* — every 2 s while
playing — costing hundreds of milliseconds of CPU and cycling mbedTLS's 16 KB
in and 16 KB out buffers through the heap each time. Kept alive, they are
allocated once, early, and never returned to fragment anything.

That holds ~35 KB permanently rather than in bursts, which is the point: the
failures on this board come from *contiguity*, and memory claimed once at the
start cannot fragment later. A connection the server has since dropped fails on
the first write, so the request is retried once on a fresh socket — that is the
ordinary cost of keep-alive, not an error.

The connection is shared state, so calls are serialised behind a mutex:
`fetchState` runs on the poll task while transport presses, the volume drag and
the portal's player list all call in from the main loop. Cover art keeps its own
per-request client — a different host, over plain HTTP, with no handshake worth
saving.

## How it talks to Home Assistant

Rather than `GET /api/states`, both reads go through `POST /api/template` with a
Jinja template that renders exactly the fields needed, separated by ASCII unit
and record separators:

- **Player list** — entity_id, friendly name and availability for every
  `media_player`, sorted by name
- **Now playing** — state, title, artist/album/app, `entity_picture`,
  `supported_features`, duration, position, and *how stale* the position is,
  then volume, mute, features and state for the **control entity**

That last field is why the progress arc is correct the moment it appears:
`media_position` is a snapshot taken at `media_position_updated_at`, so the
template returns the age and the firmware adds it back before interpolating
locally.

The now-playing response is a few hundred bytes. `GET /api/states` would ship
every attribute of every entity — `source_list` alone runs to tens of kilobytes
on an AV receiver, which is more than this board wants to hold, let alone parse.

Request bodies are built by hand rather than with ArduinoJson, which escapes
only the named sequences (`\"`, `\\`, `\b`, `\f`, `\n`, `\r`, `\t`) and writes
every other control character raw. RFC 8259 requires `\u00XX` across the whole
`0x00`–`0x1F` range, so serialising a separator-delimited template with it
produces a body Home Assistant rejects with **HTTP 400**. See
`appendJsonString` in `ha_client.cpp`.

> **Never use `{%- ... -%}` in these templates.** Python classifies `0x1C`–`0x1F`
> as whitespace, so `"\x1f".strip()` is empty — a whitespace-trimming Jinja tag
> silently eats any separator next to it. This is not hypothetical: `{%- endfor -%}`
> ate every record separator in the player list, shifting the fields one place
> per record, and `{%- set pu ... -%}` ate the separator before the position-age
> field, which pushed the *muted* value into `volume` and left the slider
> reading near zero. Plain `{% ... %}` emits nothing and strips nothing, which
> is what both templates use.

Duplicate friendly names are the norm — a Music Assistant player and the device
it wraps report the same one, as does every TV an integration discovers twice.
Entries whose name is not unique are relabelled with their `object_id`, which
is unique by construction, so no two rows in the picker ever read alike. An
entity with no friendly name at all falls back to the same thing.

**Nothing shown to a person carries the `media_player.` prefix.** It is
thirteen characters that say nothing on a list of media players, and rather
more than that on a 240 px circle, so `ha::entityLabel()` takes everything
past the dot — for the picker rows above and for the boot card. The prefix
stays in the stored value, in the `<option value>` and in every request.

`config::kMaxPlayers` (96) caps the list; entities past it are unselectable, and
hitting the cap is logged.

Commands are ordinary service calls: `POST /api/services/media_player/`
`media_previous_track` | `media_play_pause` | `media_next_track`.

Transport buttons grey out when the player's `supported_features` says the
action is unavailable. A player reporting no features at all is treated as
supporting everything — that usually means an integration never set the
attribute, not that nothing works.

## The swipe-up list

Swipe up on now-playing for a short shelf of the Music Assistant library, in
sections, each row with its artwork and one tap to play.

What the sections are is a table in `config.h`, and the table is the network
cost as well as the layout — one `get_library` request per section when the
list is opened:

```cpp
constexpr BrowseSection kBrowseSections[] = {
    {"Recent artists", "artist", "last_played_desc", 5},
    {"Recommended",    "album",  "random",           5},
};
```

`media_type` is `artist`, `album`, `playlist`, `radio` or `track`; `order_by`
is any of the integration's sort keys (`last_played_desc`, `play_count_desc`,
`random`, `timestamp_added_desc`, `name`, ...).

### "Recommended" is a stand-in, and the name is a promise this cannot keep

Music Assistant has real recommendations — the rows on its own Home page —
but they live behind its websocket API on port 8095 and are **not exposed as a
Home Assistant service**, so nothing reachable over the REST API can ask for
them. The six services the integration does register are `search`,
`get_library`, `play_media`, `play_announcement`, `transfer_queue` and
`get_queue`; there is no seventh.

What `get_library` does offer is `order_by`, and a random draw from the albums
is a decent stand-in: it surfaces things the library has and the last few weeks
did not. `play_count_desc` is the other honest option, if "what we actually
play" suits better than "something else for a change". Swap the `order_by` in
the table above.

**Recent artists** needs no such apology: `last_played_desc` over artists is
exactly what it says.

### Loaded before you ask for it

The poll task loads the list in the background, so a swipe up lands on a list
rather than on a loading card. `browse::preload()` runs after each state poll
and does nothing unless the list is missing or past `kBrowseTtlMs`, which is
once every few minutes at most. It is skipped while the list is actually on
screen — correctness would not require that, since the drawing side takes the
same lock, but there is no reason to refresh a list someone is reading, and
every reason not to make their scrolling stutter against a fetch.

The loading card is still there for the two cases left: the first swipe after a
boot that beat the preload, and a swipe landing mid-load, where
`ensureLoaded()` waits for the load already running rather than starting a
second one.

That means two tasks touch this state, so every entry point in
[`services/browse.h`](include/services/browse.h) takes a mutex and returns **by
value**. `Row` carries a copy of the title rather than a pointer into the item
array for exactly that reason: a pointer would dangle the moment the lock was
released. `play()` copies the URI out under the lock and makes the service call
without holding it, since that call blocks for a round trip.

### Tapping an item

`handleTap()` deliberately does **not** play anything. `play_media` on an
artist makes Music Assistant resolve that artist's tracks and build a queue
before it answers -- measured at 4.3 seconds on a live instance, and past six
on another -- and a screen that simply stops for that long reads as a device
that missed the tap.

So the tap reports which row was hit, the caller draws `showStarting(row)` --
the same card the list drew, alone and centred, with "Starting..." under it --
and only then makes the call. When it returns, nothing else is drawn: leaving
that frame up means the screen carries straight on from the tap to the track,
with no card flashing in between, and the next poll replaces it with the
now-playing screen as soon as there is state worth drawing.

`kHaServiceTimeoutMs` (25 s) applies to that call rather than the ordinary
`kHaHttpTimeoutMs` (6 s), which was short enough to turn a working request into
a failure the device then retried -- doubling the load on a server already busy
doing the thing it had been asked to do.

### Scrolling

The list follows your finger rather than paging. `hardware/touch.cpp` only
resolves a gesture once a press *completes*, which is no use to a list that
should move as the finger does, so `handleBrowseDrag()` tracks the live
position itself — exactly as the volume arc does — and cancels the gesture
so the release does not also land as a tap.

It only claims a press that has committed to the **vertical** axis
(`|dy| > |dx|`, past the tap slop). A horizontal one is left alone so it can
still resolve as the swipe-right that leaves the list.

On release the list glides, decaying 12% per frame and stopping at either end
rather than spinning against the clamp. A finger that came to rest before
lifting gets no glide, because stopping before letting go means stopping.

The bottom of the list scrolls until the **last row's centre reaches the middle
of the panel**, not merely until its bottom reaches the bottom edge. On a round
display the last row would otherwise come to rest in the pinched bottom of the
circle, where it is both the narrowest row on screen and the hardest to hit.
The empty space below it costs nothing.

Scroll position is in **pixels**, not rows, for two reasons: a row index cannot
express "half a row showing", and rows here are not a uniform height anyway
since a section heading is a line of text rather than a card. Rows are clipped
to the area below the title strip, so a half-scrolled row slides under it
rather than over it.

### The artwork is decoded once, not per frame

Each image is fetched once, when the list is first opened, and kept as a small
**decoded** sprite. That is not an optimisation, it is the only workable shape:
these images live on the provider's CDN rather than anywhere the device
caches, so decoding at draw time would mean re-fetching on every repaint.
Decoded is also smaller than the source: 2.9 KB for a 38 px thumbnail against
tens of kilobytes for the JPEG it came from.

The sprites are claimed once at boot and never freed, like every other buffer
here.

**Two things about that fetch are not optional.**

*The artwork is requested at thumbnail size.* Music Assistant hands back
whatever the provider stored, which meant downloading **1.9 MB across ten
images** — one of them 601 KB — to produce ten 38 px squares, with three of
them failing to decode at all. `thumbnailUrl()` rewrites the two hosts Music
Assistant actually returns, both using documented resize conventions:

| Host | Form | Rewritten to |
|------|------|--------------|
| `googleusercontent.com` | trailing `=w600-h600-p` or `=s600` | `=w38-h38-p` |
| `resources.tidal.com` | a `/750x750.jpg` path segment | `/80x80.jpg` (nearest on their ladder) |

Anything else is left exactly as it came, and a host that refuses the rewrite
costs one retry with the original URL rather than a missing image. Both
rewrites were checked against the live hosts, which return 38x38 and 80x80
respectively.

*Every byte the decoder asks for is delivered.* `StreamWrapper::read` takes a
maximum and a minimum, and LovyanGFX passes a minimum of **2** whatever it
actually wants. Filling only that minimum is a short read, which pngle tolerates
(it is a push parser and asks again) and TJpgD does not — every header read in
`lgfx_jd_prepare` is `if (infunc(...) != len) return JDR_INP`. Returning early
therefore broke *every* JPEG whose bytes were not already in the socket buffer,
while leaving every PNG working, which made it look as though particular images
were malformed. They were not.

*The decode is serialised against every other decode on the device.* LovyanGFX
keeps **one** `pngle_t` in a file-scope static and reuses it deliberately
(`LGFXBase.cpp`: `static pngle_t* pngle`), so two tasks decoding PNGs at once
tear up each other's decoder state. This runs on the poll task while the
Arduino loop composes cover art into a frame — exactly that case. It cost a
`LoadProhibited` panic inside the **Wi-Fi MAC task**, several seconds after the
damage was done and with nothing of this firmware in the backtrace, which is
what that class of bug looks like. `drawJpg` is not affected (its decoder is a
local plus a malloc'd pool) but shares the lock anyway, since the format is not
known until the response headers arrive.

Ten of them are about 29 KB, which against 2 MB of PSRAM is not a number worth
worrying about. They come out of PSRAM rather than internal RAM for the usual
reason: the internal heap is what mbedTLS wants 16 KB in and 16 KB out of, and
it is not worth competing for.

### Parsing the response

This is the one place the firmware parses JSON, and it still does not link a
JSON library. A service response *is* JSON and no Jinja template can call a
service, so there is no way to get this as delimited text like everything else.

It is **not** a flat scan for `"name":`, and the reason is worth keeping:
an album nests an `artists` array whose objects carry their own `name`, `uri`
and `image`. A scan that stopped at the first `}` would end *inside* the first
artist — reading an artist's fields for some albums and then treating the
remaining artists as further albums. So `readLibraryItem` walks each object's
own members and steps over every value whole (`skipJsonValue`), taking only
what is at its own depth. `readJsonString` handles escapes, including
`\uXXXX` decoded to UTF-8, because names do contain quotes and accents.

The scanner was ported line-for-line to Python and run against real
`get_library` responses — artists, albums with nested artists, and a
synthetic item whose name contains `"`, `\` and an em dash — all matching
`json.loads`.

(`get_queue` was the other candidate for this gesture and is not worth having:
it reports that the queue holds 25 items and which one is playing, but returns
only `current_item` and `next_item`. A scrollable queue would need the
websocket API too.)

## Artist search

Swipe left on the browse list for a keyboard. Type a name, press SEARCH, tap a
result, and that artist's **radio** starts — an endless queue seeded from
them, which is the reason to look an artist up from a wall remote in the first
place. Playing a discography in order is something you would reach for a phone
to do. `kSearchPlaysRadio` turns that off and plays the artist straight.

**Typing on a 240 px circle is only worth doing because the search is
forgiving.** `music_assistant.search` reaches past the local library into every
configured provider and matches partial names: six letters, "radioh", finds
Radiohead among YouTube Music's catalogue. Nobody has to type "Noordpool
Orchestra".

The keyboard is four rows of seven keys plus a wide SEARCH bar, laid out
against the chord of the circle at each row — the narrowest, the top row,
allows 205 px and the row needs 201. Keys are 27×26 px. It is **alphabetical,
not QWERTY**: this is a keyboard used a few times a year, where hunting for a
letter in a familiar-but-scrambled layout is slower than reading down the
alphabet. The last two keys of the bottom row are space and backspace, drawn as
glyphs rather than labelled.

Results are the browse list's rows -- same shape, same artwork, same
finger-following scroll and glide -- and **the artwork is fetched on a worker
task of its own** (`ui/common/artwork.cpp`), pinned to core 0 away from the
loop. It used to be one image per loop pass, and each fetch -- 560 to 860 ms,
most of it a TLS handshake -- was that long with touch unread: at boot the
panel was blind for over half of every ten seconds and caught about one tap
in six. The worker keeps one connection open across a batch and takes its
queue grouped by host, so a list's sixteen thumbnails from two CDN hosts cost
two handshakes rather than sixteen, and closes it when the queue empties. The
names go on screen immediately, the pictures land under them, and arrivals
are gathered into one repaint every `kThumbRepaintMinMs` at most, and only
when one is on screen. A row with no artwork yet centres its name, so nothing
shifts sideways when the picture arrives.

Touch itself is sampled on its own task on the Qualia
(`board::kTouchSampleTask`) into a queue of timestamped changes, which the
loop replays through the gesture state machine -- so a slow pass delays a tap
rather than losing it. The other boards keep sampling from the loop, into the
same queue: their touch goes through LovyanGFX's device object, which the
loop is drawing through at the same time.

The list and search screens go back to now playing after
`config::kListIdleReturnMs` (a minute) without a touch.

Tapping a result draws it alone and centred with "Starting radio..." before
the call, because `play_media` in radio mode has to build a queue as well as
resolve the artist -- the longest wait anywhere in the firmware.

Swipe right backs out one step at a time — results to keyboard with the query
intact, so a near miss can be edited rather than retyped, then keyboard back to
the list. Swiping up and down scrolls the results. Tapping anywhere on the results that is not a result does the same,
since a miss there usually means "that was not the artist".

`services::search` needs no mutex, unlike `services::browse`: everything runs
on the Arduino loop, because a search is a response to a keypress and there is
nothing to preload.

Both lists share one drag handler. `ScrollableList` is three function pointers
— scroll, limit, redraw — and `handleListDrag()` takes it; the two are never
on screen together, so they share the drag state as well.

## Idle blanking

Once the selected player has been idle for `kIdleTimeoutMs` (5 minutes) the
panel is cleared, the backlight cut and the controller put to sleep — and the
volume/power entity switched off with it, see
[…and off again](#and-off-again). Any touch wakes it, as does playback
resuming; polling carries on while blanked, which is what notices.

The delay exists so the gap between two tracks does not flick the screen off and
straight back on, and so the amplifier is not cut on a thirty-second pause.
`kBlankWhenPaused` decides whether paused counts as idle; **on**, since a pause
that lasts five minutes is a stop in everything but name, and leaving it off
would mean a pause never powered the amplifier down. Only the now-playing
screen blanks: the picker and the status cards are on screen because someone is
looking at them.

**The backlight needs a pin.** `kDisplayPinBacklight` is GPIO 2 on this board
and really switchable, so blanking is a genuine cut rather than a black frame,
and `kDisplayBrightness` is a real dimmer.

Nothing is drawn while blanked -- a sleeping controller ignores it -- and panel
RAM is not trustworthy across a sleep, so waking forces a fresh compose rather
than assuming the old frame survived.

## Volume slider

The bezel arc is a `media_player.volume_set` slider. It spans
`kVolumeArcFraction` (70%) of the circumference — 252° centred on 12 o'clock,
so it runs from the lower left round to the lower right and leaves a 108° gap
across the bottom for the transport row and the elapsed chip.

**You do not grab it.** A horizontal swipe anywhere in the **top half** of the
panel drives it. Landing a fingertip on a 7 px track drawn 111 px out from the
centre is a precision task on a 1.28" circle, and this is the one control here
that gets used without looking; nothing else lives up there, since the
transport row is at y=181 and the title text is not a target.

The swipe is **relative, not absolute**: it adjusts the volume from wherever it
already was rather than jumping it to whatever angle the finger points at. The
knob moves the same distance along the arc that the finger moved across the
glass — the arc is about 504 px long at a 240 px diameter, so a full-width
swipe is a little under half the range and the whole span takes about two and a
half of them.

Only a press that commits to the horizontal is claimed (`|dx| > |dy|`, past the
tap slop), so swipe-up still opens the browse list. Where the press *started*
is what decides it, so a swipe that begins up top stays a volume swipe even
once it has wandered below centre.

The arc repaints locally as you drag, while `volume_set` is throttled to
`kVolumeSendIntervalMs` (250 ms) with an authoritative set on release — so what
lags is the audio, not the UI.

**A poll cannot undo a level the device just set.** Two ways it used to: a
fetch already in flight when the finger lifted was issued before the new level
and returned the old one, and even a fetch begun afterwards could, because Home
Assistant's state for a receiver lags the service call that changed it. Either
way the arc snapped back — and worse, the next swipe read that stale level as
its starting point and re-applied the same increase, so two swipes in a row
ratcheted instead of rising.

So the level last commanded is remembered and believed over any poll that
disagrees, until one confirms it (within `kVolumeConfirmEpsilon`, since
receivers quantise) or `kVolumeConfirmTimeoutMs` passes — after which the
server is trusted again, because asserting a level nobody agrees with is worse
than accepting a wrong one. The command is recorded *before* the drag flag
clears; between those two there is no guard at all, and a poll landing in that
window is precisely the one carrying the stale value. Players that report no `volume_level`, or lack the
`VOLUME_SET` feature bit, simply get no slider.

Progress is an elapsed / total chip in the arc's bottom gap rather than a second
ring. It sits on an opaque pill so its once-a-second repaint goes straight to
the panel — recomposing the frame would mean decoding the cover art again, or
re-fetching it on the streaming path. For the same reason a poll only triggers a
recompose when something *visible* changed; position alone does not.

## Cover art

Downscaling locally does not help — the cache holds the **compressed** image,
and a decoded 240×240 RGB565 is 115 KB against ~40 KB for a 512 px JPEG. The
only way to store less is to ask for less.

`kCoverArtRequestPanelSize` does exactly that, rewriting an existing `size=`
query parameter down towards `kDisplayDiameter`.

Servers offer a **ladder** of sizes rather than any value. Music Assistant's
image proxy accepts `0, 80, 160, 256, 512, 1024` and answers 400 to anything
else — it says so in the response body. So the request rounds **up to a power
of two**, which lands on that ladder without ever upscaling: a 240 px panel
asks for 256. Measured on one album:

| `size=` | Result |
|---------|--------|
| 512 (integration default) | 200, 88122 bytes |
| 256 (what we now ask for) | 200, 28382 bytes |
| 240 (a naive exact match) | 400 |

That 3.1× cut is the difference between caching the art and re-fetching it on
every compose pass. It is safe to leave on: a refusal is retried with the
untouched URL and rewriting is then dropped for the session.

`entity_picture` is then held compressed, so one buffer serves every repaint:

- The buffer is **static**: `kCoverArtBufferBytes` (40 KB internal, 128 KB with
  PSRAM) claimed once by `cover::init()` at boot and reused for every cover for
  the life of the device. It is never freed and never resized. Art that fits is
  read into it; its dimensions come from the JPEG `SOF` / PNG `IHDR` header so
  it can be **centre-cropped to fill** the circle rather than letterboxed.

  Sizing it per image instead meant a free and a differently-sized `malloc` on
  every track change — which is how a long-running heap fragments: the hole left
  by a 28 KB cover does not necessarily take the 35 KB one that follows. Taking
  it at boot, *before Wi-Fi*, also means it comes from a pristine heap and
  cannot fail later.

  The cost is holding 40 KB whether or not anything is playing. On a device with
  one job that is the right trade — what breaks here is contiguity, not total
  free memory, and memory claimed once at the start cannot fragment anything.
  For scale, a 256 px cover measured 28 KB against 88 KB at the 512 px default.

- Anything larger is streamed and decoded off the socket instead. This is the
  case that costs: a cached cover is fetched **once per track**, while an
  uncached one is fetched again on every recompose. The `size=` rewrite above
  is what keeps most art on the cheap side of that line.

  With caching disabled entirely (`kCoverArtCacheBytesInternal = 0`),
  `prepare()` does no network work at all: it just records the URL, since
  probing it would spend a round trip per track change to learn what the budget
  already said.

Every art change logs `heap free`, `largest block` and whether the cache is on,
since those are what any tuning here has to be argued from.

Positioning note, since it is easy to get wrong: LovyanGFX's `drawJpg`/`drawPng`
take `x`/`y` as the **top-left of the fit box**, and use the datum only to place
the image *within* that box. Passing the screen centre with a full-screen box
puts the art off-screen, where it is silently clipped to nothing.
- Larger art is re-streamed from HA on each repaint through a `DataWrapper`
  that feeds LovyanGFX's decoder straight off the socket, so only the decoder's
  working buffer is in RAM. That path fits the art inside the circle instead of
  cropping, since the dimensions aren't known without buffering. For square
  album art — nearly all of it — the two look identical.

Absolute `entity_picture` URLs (some integrations link art off-site) are used
as given; relative ones are resolved against the HA base URL.

The art is then scrimmed: a flat dim over the whole frame plus a gradient under
the text and transport row, so white text stays legible over any image.

## Architecture

```
src/
  main.cpp                  Arduino entry points, nothing else
  app/
    app.cpp                 state machine, poll task, commands, idle timer
  services/                 no graphics library below this line
    wifi_setup.cpp          WiFiManager portal + HA URL/token/player fields
    player_list.cpp         cached media_player list, for the portal dropdown
    browse.cpp              library sections, by URL rather than by pixel
    search.cpp              artist search query and results
    ha_client.cpp           template reads, service calls, NVS settings
    device_name.cpp         the device's name: hostname and mDNS
  hardware/
    boot_button.cpp         the one pin that is not a panel detail
    display.cpp             LovyanGFX bring-up
    display_font.cpp        embedded VLW smooth fonts
    touch.cpp               press tracking, tap/swipe classification
  ui/
    headless/ui.cpp         the seam with no panel behind it
    round/
      ui.cpp                touch -> intents, and what to show
      canvas.cpp            off-screen frame buffer + scrim blending
      theme.cpp             round-panel chord geometry
      text.cpp              ellipsising and word wrap
      now_playing.cpp       main screen
      browse_list.cpp       the swipe-up list, with section headings
      search.cpp            keyboard and result list
      status_screens.cpp    boot / setup / error cards
      artwork.cpp           decoded thumbnails, at this panel's size
      cover_art.cpp         art fetch, cache, decode
```

All HTTP runs on `haPollTask`; `setup()` and `loop()` own the display. Polling
is 2 s while playing, 8 s otherwise, and a transport press cuts the wait short
so the UI catches up immediately.

### What the two tasks share

Three things, each with its own discipline.

**One `PlayerState` behind `g_state_mutex`.** The poll task writes it, the
drawing code snapshots it by value, and nothing holds a pointer into it.

**Two self-serialising caches**, `services::cover` and `services::browse`.
Both hand back copies rather than views — `browse::Row` carries the title as
a `char[]` for exactly that reason — and both create their mutex in `init()`,
on the Arduino task before `haPollTask` exists. Never lazily: two tasks that
each find a null handle and each make a mutex end up locking different ones,
which is no lock at all. `browse`'s lock is recursive so a whole frame can be
drawn under one `ReadGuard`, rather than the list changing between
`rowCount()` and the last `rowAt()`.

**A handful of flags**, which are `std::atomic` rather than `volatile`.
`volatile` only promises the compiler will not invent or elide the access; it
says nothing about ordering against ordinary memory, and nothing about the
other core seeing the write — which matters, because the S3 is dual-core and
`haPollTask` is deliberately left unpinned. On `xtensa-esp32s3-elf` the core
has `S32C1I`, so `__GCC_ATOMIC_BOOL_LOCK_FREE` and `__GCC_ATOMIC_INT_LOCK_FREE`
are both `2`: every flag here is lock-free, a sequentially consistent access
compiles to a plain load or store plus a `memw`, and the linked image contains
no `__atomic_*` library calls. Only 64-bit types would fall back to a lock,
and nothing here is one.

Presses acknowledge before the round trip completes: the button lights for the
duration of the call, and play/pause flips locally straight after, to be
replaced by whatever HA actually reports on the next poll.

## Troubleshooting

The on-screen error card only has room for one clipped line, so the serial log
is the place to look:

```bash
pio device monitor
```

Each line is milliseconds since boot, a level letter — **E**rror, **W**arn,
**I**nfo, **D**ebug, **V**erbose — and the message. How much is printed is
fixed at build time by `config::kLogLevel`, `kInfo` by default; everything
below it is compiled out. At `kDebug`, **every** outbound HTTP request logs
one line as well — method, URL, status, bytes sent and received, and elapsed
time:

```
   18503 D HTTP POST https://ha.example:8123/api/template -> 200 (tx 412, rx 318 bytes, 143 ms)
   19120 D HTTP GET http://192.168.1.199:8095/imageproxy/6011…?size=256&fmt=jpg -> 200 (tx 0, rx 28382 bytes, 94 ms)
```

That covers the state poll, the player list, every service call, and every
cover art fetch. It is deliberately chatty — a playing poll runs every 2 s, and
an uncached cover is re-fetched on every recompose — which is why it sits
below the default level.

A *failed* request logs at warn, so it shows at the default level, and adds
the **full request body** and the **full response body** on top — Home
Assistant names the failing line of a bad template and
says so plainly on an auth failure, and seeing the request next to it is
usually enough on its own.

| Symptom | Where to look |
|---------|---------------|
| `HTTP 400` | Response body names the template or JSON problem |
| `token rejected (401)` | Re-paste the long-lived token in the portal |
| `HTTP 404` | Base URL has a trailing path, or the entity no longer exists |
| `connection refused` / timeouts | Base URL host/port, or HTTPS against a plain-HTTP instance |
| `transport failure (heap free …, largest …)` | An HTTPS handshake that could not allocate. mbedTLS needs 16 KB + 16 KB of contiguous *internal* RAM; check nothing new is claiming it |
| Player missing from the picker | `list hit the N entry cap` — raise `kMaxPlayers` |
| `Cover art: N byte buffer` | Boot line confirming the static buffer was claimed; absent means it could not be |
| `vs M byte buffer, streaming instead` | This cover is larger than the static buffer; raise `kCoverArtBufferBytes` if there is heap to spare |
| `Cover art: decode failed` | Prints size, source dimensions, format and scale |
| `Art: <url> refused, retrying unresized` | That host does not use the resize convention `thumbnailUrl()` assumed; harmless, costs one round trip |
| `assert failed: pbuf_free ... p->ref > 0` | A socket outlived by the `HTTPClient` pointing at it. Declare the client **before** the `HTTPClient`; see the note in `cover_art.cpp` |
| `decode failed ... progressive JPEG is not supported` | Not a bug. TJpgD is baseline-only and some hosts (Tidal) serve progressive at every size; that row renders without artwork |
| Volume slider missing | The control entity reports no `volume_level`, or is off — check which entity **Volume & power** points at |
| Swipe up does nothing | No Music Assistant config entry id in the portal, or no player selected |
| `no items in library response` | The whole response body follows; check the entry id and the section's `media_type` |
| A section heading is missing | That section came back empty; a heading over nothing is dropped rather than shown |
| List says **Not set up** | No Music Assistant config entry id in the portal |
| List says **Music Assistant** + an error | Every section's request failed; the line is `ha::lastError()` and the console has the body |
| List shown without artwork | The thumbnail sprites could not be claimed; check PSRAM |
| `Touch: no answer at 0x15` | Normal at boot on an untouched panel. An I2C scan follows: addresses that answer mean wrong chip/address, silence means wiring, rails or RST held low |
| Taps land mirrored or transposed | `kTouchSwapXy` / `kTouchInvertX` / `kTouchInvertY` |

## PlatformIO quick reference

```bash
pio run             # build
pio run -t upload   # flash
pio device monitor  # serial log
pio run -t merge    # single flashable .bin
```

The board enumerates as a CH343, so the port is left to auto-detect. Put an
untracked `platformio-local.ini` beside `platformio.ini` with `upload_port` /
`monitor_port` if a machine needs pinning; it is globbed in and optional.

### Updating over the network

The flash holds two 4 MB app slots (`partitions/media_remote.csv`), so a
running device can take new firmware without a cable: open **Update** in its
setup portal (`/u`) and upload `.pio/build/<env>/firmware.bin` -- the app
image, not `firmware-merged.bin`. It is written into the slot that is not
running and booted into once it has arrived whole. From a shell:

```bash
curl -F "update=@.pio/build/<env>/firmware.bin" http://<device-name>.local/u
```

A device flashed before the table had a second slot answers "Partition Could
Not be Found" and carries on unharmed; it needs one USB flash with
`pio run -t upload` to take the new table. That keeps its settings, since nvs
has not moved. Flashing `firmware-merged.bin` instead wipes them: it is one
image from address 0, padded over nvs.

The platform is **pioarduino** (arduino-esp32 3.x / IDF 5.x) rather than the
stock `espressif32`, which predates this board definition. Note that the two
cannot share a project: they want different `tool-esptoolpy` packages under the
same name and `~/.platformio/packages` holds one directory for it, so each
build strips the other's copy and the next one dies with `ModuleNotFoundError:
No module named 'esptool'`.

## Status

Builds clean: **32.9%** of 4 MB flash, **23.6%** of internal RAM. The frame
buffer (115 KB), the cover art cache (128 KB) and the list thumbnails (~29 KB)
live in PSRAM — about 270 KB of 2 MB.

Runs on hardware. The Music Assistant service shapes, both Jinja templates and
the artwork URL rewriting were all verified against a live instance.

If a build fails with `ModuleNotFoundError: No module named 'esptool'` or a
bare `TypeError: expected str, bytes or os.PathLike object, not NoneType`, a
PlatformIO tool package is a partial install — delete
`~/.platformio/packages/tool-esptoolpy` and rebuild to re-fetch it.
