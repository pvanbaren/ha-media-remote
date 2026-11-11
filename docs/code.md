# How the code is organised

Part of the [Home Assistant Media Remote](../README.md) documentation.

## Layers

```
src/
  main.cpp       setup() and loop(), handed straight to app/
  app/           what the remote decides and does          <- no display
  services/      Home Assistant, Music Assistant, settings  <- no display
  ui/
    common/      every screen that does not care what shape the panel is
    round/       theme.cpp, now_playing.cpp   <- the round panels
    square/      theme.cpp, now_playing.cpp   <- the Qualia
    headless/    draws nothing, narrates to serial
  hardware/
    waveshare/  round360/  amoled466/  qualia/  lcd28c/  matouch21/
    guition4848/  lcd4b/  headless/    <- one per board
    rgb/         the RGB panels' output stage, for qualia/, lcd28c/,
                 matouch21/ and guition4848/, and the P4 4B's DSI panel
include/
  board/         one header per board: pins, sizes, scales, timings
  config.h       everything about the application that is a number
```

**The panel is a build-time choice.** `include/ui/ui.h` is the seam. Above it,
nothing includes a graphics library; each env in `platformio.ini` picks a
`board/<board>.h` with `-DBOARD_HEADER` and a `ui/<shape>/` and
`hardware/<board>/` with `build_src_filter`. A new panel is a header, a
directory and an env, not an `#if`.

**The input half is what earns the seam.** The display does its own hit
testing -- only it knows where it drew the buttons -- and hands back what the
gesture *meant*: `Intent::kPlayPause`, `kSetVolume` with a level,
`kPlayBrowseRow` with an index. The round panel reads a volume swipe as travel
along its arc and the square one as travel along its bar; `src/app/` gets the
same intent from both.

**Only two files know the panel's shape.** Of the UI, `theme.cpp` answers
"how wide is the screen at this height" (`chordHalfWidth()`: the chord on a
circle, the full width on a square) and `now_playing.cpp` holds the one
screen with a shape in it. The list, the search, the status cards, the
canvas, the artwork and the text layout are shared, and widen to fit by asking
`chordHalfWidth()`. The DFR1221 reuses `ui/round/` unchanged at 1.5×, the
AMOLED at about 1.94×, and the 2.8C and the MaTouch at 2×.

**Every dimension scales.** `include/ui/theme.h` is written against a 240 px
round panel and multiplied by the board's `kUiScale` (width / 240). Two more
per-board factors sit on top and nowhere else: `kTextScale` for panels whose
pixels are denser than the design's (0.75 on the Qualia, the 2.8C and the MaTouch), and
`kListScale` for
the lists (the Qualia's ⅔, which shows about six rows instead of three).

**`headless` is a standing check,** not a toy: it builds and runs the whole
remote with `ui/headless/`. If it stops linking, something in `src/app/` or
`src/services/` has grown a pixel.

## Files

```
src/app/app.cpp            the state machine: what to show, commands, idle
                           timer, and the network task that owns the stream
src/services/
  wifi_setup.cpp           WiFiManager: setup AP, LAN portal, every field
  ha_link.cpp              the QR-code sign-in, and the remote's own token
  ha_client.cpp            templates, service calls, the library and search,
                           Home Assistant settings in NVS
  ha_stream.cpp            the WebSocket state stream, and calls over it
  websocket.cpp            an RFC 6455 client over a Yielding socket
  tls_gate.cpp             one TLS handshake at a time, with room for it
  ha_internal.h            what ha_client and ha_stream share
  player_list.cpp          cached media_player list, for the portal and the
                           device's lists
  browse.cpp               the swipe-up list's sections
  search.cpp               the search: its kind, query and results
  history.cpp              the room's recent artists
  recommend.cpp            recommendations from the recent artists, the
                           room's or the server's, on a task of its own
  ma_api.cpp               Music Assistant's own API, and its settings
  display_settings.cpp     rotation and keyboard layout
  device_name.cpp          hostname and mDNS
  voice.cpp                Assist requests and spoken replies, the Voice
                           setting (builds with -DVOICE_ASSIST)
  wake_word.cpp            the wake word chosen, of microWakeWord's four,
                           on TensorFlow Lite Micro
  firmware_update.cpp      the latest release on GitHub, and installing it;
                           the portal's /firmware page
  fonts.cpp                the font pack from GitHub; the portal's /fonts
src/ui/round/, src/ui/square/
  theme.cpp                chordHalfWidth(): the panel's shape
  now_playing.cpp          the main screen: volume arc or bar, transport
src/ui/common/
  ui.cpp                   touch -> intents, drags, glides, what to show
  canvas.cpp               the composed frame, presenting, in-place scroll
  browse_list.cpp          the list, with section headings
  search.cpp               keyboard and results
  status_screens.cpp       the cards, the sign-in QR code, the status page
  settings_list.cpp        the settings page and each setting's choices
  player_pick.cpp          the player list, while none is chosen
  wifi_join.cpp            the Wi-Fi networks and the password keyboard
  peers_chip.cpp           the "+N rooms" chip on now playing
  isolate_card.cpp         Isolate's card
  back_button.cpp          the back button, top left
  text_glow.cpp            the dark glow behind text over the cover
  artwork.cpp              the worker: list thumbnails and the cover fetch
  cover_art.cpp            cover and thumbnail fetch, cache, decode
  text.cpp                 ellipsising and word wrap
src/hardware/
  touch_gestures.cpp       press tracking, tap / swipe, the touch queue
  encoder.cpp              the knob, on a board with one
  battery.cpp              the battery, on a board with an AXP2101
  display_font.cpp         picks the embedded face for a height
  audio.cpp                microphone and speaker streams, for voice
  es8311_es7210.cpp        the codec and microphone ADC
  i2c_bus.cpp              the shared I2C bus
  outline_fonts.cpp        other scripts and symbols behind each face
  font_pack.cpp            the font pack, in its flash partition
  <board>/display.cpp      panel bring-up, backlight, sleep, rotation
  <board>/touch_raw.cpp    reads the touch controller
  <board>/font_table.cpp   the faces this board embeds
  inflate.cpp              an embedded file compressed at build time, back
                           in PSRAM
  <board>/boot_button.cpp  the reset button (the Qualia's is on its expander)
  rgb/framebuffer.cpp      the framebuffer: copy, scroll and flush
  rgb/rgb_panel.cpp        the S3's RGB peripheral, opened for it
  qualia/expander.cpp      the TCA9554: panel reset, backlight, buttons
  lcd28c/panel.cpp         the 2.8C's TCA9554, and its ST7701's register init
  matouch21/panel.cpp      the MaTouch's ST7701 register init, bit-banged
  guition4848/panel.cpp    the 4848S040's ST7701 register init, bit-banged
  lcd4b/panel.cpp          the P4 4B's DSI bus, LDOs and ST7703 init
scripts/
  build_ui_font.py         the VLW font generator
  build_font_pack.py       the font pack the remote downloads
  deflate_embeds.py        compresses what an env embeds as data/deflated/
  board_header_stamp.py    rebuilds everything when the board header changes
  merge_firmware.py        pio run -t merge
  pin_components.py        holds a source build's IDF components
include/third_party/stb_truetype.h  draws the outline fonts (public domain)
partitions/media_remote.csv  the flash layout, shared by every board
.github/workflows/build.yml  CI: builds every board on each push
.github/workflows/pages.yml  the install page, on each published release
.github/release-footer.md    how to update, after each release's notes
CHANGELOG.md                 every release's changes, and its notes
docs/                        building, the code, internals, troubleshooting
                             and the error cards, beyond the README
site/                        the install page and its board manifests
```

## Tasks

| Task | Does |
|---|---|
| Arduino loop | Touch, drawing, the idle timer |
| Commands (`commands`) | Service calls a button makes, from a queue |
| Network task (`haPollTask`) | Owns the state stream, or polls without it; history, list loads, picture lookups |
| Artwork worker (`thumbs`, core 0) | The now-playing cover first, then list thumbnails |
| Recommendations (`recommend`, stack in PSRAM) | Builds the recommendation pool from Music Assistant's API |
| Voice (`voice`, `wake`, `audio_in`, `audio_out`; builds with `-DVOICE_ASSIST`) | The wake word, Assist requests, the microphone and the speaker |
| Touch sampler (Qualia, 2.8C, MaTouch, 4848S040, P4 4B) | Reads the touch controller into a queue of timestamped changes |

They share three kinds of thing, each with its own discipline:

- **One `PlayerState`** behind `g_state_mutex`. The network task writes it;
  the drawing code takes a copy. Nothing holds a pointer into it.
- **Self-serialising modules** -- browse, history, the cover, the artwork
  pool, the recommendations -- that hand back copies, never views, and create
  their locks in `init()` on the Arduino task before the other tasks exist.
  Never lazily: two tasks that each find a null handle and each make a mutex
  lock different ones.
- **Flags**, `std::atomic` rather than `volatile`: `volatile` promises nothing
  about ordering or about the other core seeing the write, and the S3 is
  dual-core. Every flag here is lock-free on it.

Service calls made by the command task (a button) are handed to the network task
through a one-deep slot and go over the stream; if that task cannot take them
within `kHaStreamPickupMs`, they go over REST instead rather than wait.
