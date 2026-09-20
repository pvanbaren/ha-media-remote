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

The same portal collects the Home Assistant base URL, a long-lived token, and
a dropdown of every `media_player` entity -- plus a second dropdown for
whatever separate entity carries **volume and power**, for a player streaming
into an amplifier that owns the knob.

The player is chosen in the portal rather than on the device: a `<select>` in
a browser is a better list than forty rows on a small panel, and it is
reachable when the panel is not.

Holding **BOOT** for 3 seconds clears the saved settings and reboots into the
portal. Touch is the primary input, so BOOT has no short-press action.

## How it talks to Home Assistant

Reads go through `POST /api/template`, not `GET /api/states`. A Jinja template
renders exactly the fields needed into a few hundred bytes of
separator-delimited text; `/api/states` ships every attribute of every entity,
and `source_list` alone runs to tens of kilobytes on an AV receiver. The
template also returns how stale `media_position` is, so the elapsed time is
right the moment it appears.

There is no JSON library. Home Assistant rejects ArduinoJson's output with a
400, because it emits only the named escape sequences and writes every other
control character raw -- and the template separators are control characters.
`appendJsonString()` covers the whole 0x00-0x1F range.

All HTTP runs on one poll task; `setup()` and `loop()` own everything else.
They meet at one `PlayerState` behind a mutex and at a few `std::atomic`
flags. Polling is 2 s while playing, 8 s otherwise, and a transport press cuts
the wait short.

## The library list

A swipe up opens a list of Music Assistant library sections, defined by
`config::kBrowseSections` and flattened into one scrollable run of rows with a
heading above each. One tap plays an item.

Sections cost one request each, so that table is the network cost as well as
the layout. `get_library` is addressed by **config entry** rather than by
entity -- it asks the server what is in the library, not a player what it is
doing -- which is why the portal wants a Music Assistant config entry id.

A note on "Recommended", since the name is a promise this cannot quite keep.
Music Assistant does have real recommendations, the rows on its own Home page,
but they live behind its websocket API on port 8095 and are not exposed as a
Home Assistant service. What `get_library` offers is `order_by`, and a random
draw from the albums is a decent stand-in: it surfaces things the library has
and the last few weeks did not.

The list is **loaded in the background** by the poll task, so a swipe up lands
on a list rather than on a loading card. That means two tasks touch it, so
every entry point takes a mutex and returns by value -- `Row` carries a copy
of its title rather than a pointer into the item array, which would dangle the
moment the lock was released. The lock is recursive so a whole frame can be
drawn under one `ReadGuard`, rather than the list changing between
`rowCount()` and the last `rowAt()`.

Artwork is named here and drawn elsewhere: this layer deals in image URLs, and
turning one into a sprite at the right size is a display decision.

This is also the one place the firmware parses JSON. A service response *is*
JSON and no Jinja template can call a service, so there is no way to get it as
delimited text like everything else -- and it still does not link a JSON
library, because albums nest an `artists` array and a depth-aware walk is
about forty lines.

### Power and volume

`turn_on` goes to the control entity and `volume_set` to the player, in that
order, so the amplifier has the length of a round trip to wake before there is
anything to hear. It fires when a tap wakes a blanked panel, when play is
pressed, and when the player itself comes out of standby.

The counterweight is `kTurnOffControlOnBlank`: the panel blanks and the
amplifier goes off together, on the same `kIdleTimeoutMs`. One timer, because
it is one decision.

A commanded volume is believed over a poll that disagrees, for four seconds or
until Home Assistant catches up. Without that, a fetch already in flight when
the finger lifts returns the old level, the slider snaps back, and the next
swipe reads that stale level as its starting point -- so the volume ratchets
instead of rising.
