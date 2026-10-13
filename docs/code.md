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
    waveshare/  round360/  qualia/  lcd28c/  matouch21/  headless/
                 <- one per board
    rgb/         the RGB panels' output stage, for qualia/, lcd28c/ and
                 matouch21/
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
`chordHalfWidth()`. The DFR1221 reuses `ui/round/` unchanged at 1.5×, and the
2.8C and the MaTouch at 2×.

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
  ha_internal.h            what ha_client and ha_stream share
  player_list.cpp          cached media_player list, for the portal and the
                           device's lists
  browse.cpp               the swipe-up list's sections
  search.cpp               the artist search query and results
  history.cpp              the room's recent artists
  recommend.cpp            the room's recommendations, on a task of its own
  ma_api.cpp               Music Assistant's own API, and its settings
  display_settings.cpp     rotation and keyboard layout
  device_name.cpp          hostname and mDNS
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
  display_font.cpp         picks the embedded face for a height
  <board>/display.cpp      panel bring-up, backlight, sleep, rotation
  <board>/touch_raw.cpp    reads the touch controller
  <board>/font_table.cpp   the faces this board embeds
  <board>/boot_button.cpp  the reset button (the Qualia's is on its expander)
  rgb/rgb_panel.cpp        an RGB panel's framebuffer, copy and scroll
  qualia/expander.cpp      the TCA9554: panel reset, backlight, buttons
  lcd28c/panel.cpp         the 2.8C's TCA9554, and its ST7701's register init
  matouch21/panel.cpp      the MaTouch's ST7701 register init, bit-banged
scripts/
  build_ui_font.py         the VLW font generator
  board_header_stamp.py    rebuilds everything when the board header changes
  merge_firmware.py        pio run -t merge
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
| Arduino loop | Touch, drawing, the idle timer, service calls a button makes |
| Network task (`haPollTask`) | Owns the state stream, or polls without it; history, list loads, picture lookups |
| Artwork worker (`thumbs`, core 0) | The now-playing cover first, then list thumbnails |
| Recommendations (`recommend`, stack in PSRAM) | Builds the recommendation pool from Music Assistant's API |
| Touch sampler (Qualia, 2.8C, MaTouch) | Reads the touch controller into a queue of timestamped changes |

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

Service calls made from the loop (a button) are handed to the network task
through a one-deep slot and go over the stream; if that task cannot take them
within `kHaStreamPickupMs`, they go over REST instead rather than wait.
