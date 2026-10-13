# Internals

Part of the [Home Assistant Media Remote](../README.md) documentation.

## Talking to Home Assistant

**State comes from one Jinja template.** Rather than `GET /api/states` --
every attribute of every entity, and an AV receiver's `source_list` alone is
tens of kilobytes -- the firmware renders a template that emits exactly the
fields it needs, separated by the ASCII unit and record separators: state,
title, artist, picture, features, duration, position and *how stale* the
position is, name, then the volume device's volume, mute, features, state,
source and input, the artist and track for the history, and which entity is
being followed. A few hundred bytes. The position age is why the progress is
right the moment it appears: the firmware adds it back, having no clock of its
own.

**It is pushed, not polled**, whenever Home Assistant will give a stream. The
firmware opens the WebSocket API (`/api/websocket`, over TLS for an https URL),
authenticates with the same token, and subscribes to the template with
`render_template`. Home Assistant works out which entities it reads and sends
a fresh rendering whenever one changes: a volume change, a track starting, a
pause from a phone -- each in one round trip, and nothing asked while nothing
happens. The template reads `now()`, so it re-renders once a minute too, which
keeps the position honest and proves the link alive. A burst of changes (a
track change is title, then art, then duration) is settled for
`kHaStreamSettleMs` into one repaint.

**Steady state is one connection and one TLS session.** Service calls go over
the same socket -- transport, volume, power, `select_source`, `play_media` --
and so do Music Assistant's `get_library` and `search`, which ask for the
service's response back. mbedTLS takes its buffers from internal RAM only,
and a second TLS session beside the stream's, with a cover being fetched as
well, is one more than fits. Of the Home Assistant requests, only the player
list and a volume device's inputs -- for the portal and the settings page --
use REST while the stream is up.

**Without a stream it polls**: before the first connection, after a drop, or
against a server that refuses one. Every `kHaPollPlayingMs` (2 s) while
playing and `kHaPollIdleMs` (8 s) otherwise, retrying the stream on a backoff
from 5 s to a minute. A stream that had been working for a minute is reopened
at once; one closed sooner counts as a failure, so a server that accepts the
connection and refuses the subscription cannot cause a reconnect loop.
Changing the server, token, player or volume device resubscribes. The TLS
handshake is bounded by `kHaStreamHandshakeMs`. The WebSocket client is a few
hundred lines of RFC 6455 over the project's own sockets rather than a
library: the libraries create their own TLS client (which could not be made to
sleep instead of spin) or need an IDF rebuild.

**Sockets sleep, not spin.** `services::Yielding` wraps the Arduino clients so
every wait blocks for a tick. A task spinning on a slow server at the idle
task's priority starved the idle task on its core into the watchdog, and
priority inheritance through the connection mutex meant a low priority alone
could not prevent it.

**Requests are built by hand.** JSON bodies escape every control character as
`\u00XX` (`appendJsonString`); ArduinoJson writes the separators raw, which
Home Assistant rejects with HTTP 400. Responses -- Music Assistant's are JSON,
and no template can call a service -- are parsed by walking each object's own
members and stepping over every value whole, because an album nests an
`artists` array whose objects have their own `name` and `image`.

> **Never use `{%- … -%}` in these templates.** Python counts `0x1C`–`0x1F` as
> whitespace, so a trimming tag silently eats the separator next to it and
> shifts every field after it by one.

Duplicate friendly names are the norm -- a Music Assistant player and the
device it wraps share one -- so the player lists relabel a duplicate with its
`object_id`, and nothing shown carries the `media_player.` prefix.
`kMaxPlayers` (96) caps the list.

## When the volume device is on another input

With a separate **Volume & power** entity, the template decides which entity
is being *heard* and reports it:

1. The **player**, normally.
2. When the volume device is on an input other than the player's and that
   input is **named after another player**, that player, whatever it is
   doing: idle or off, it is still what the room hears, so the remote shows
   it idle rather than its own player. Where several share the name, one
   playing or paused comes before one that is not, then one that is
   available, and the Music Assistant entity among equals, since that one has
   the art and the queue. A Triad zone on "Kitchen" is playing whatever the
   player called Kitchen is.
3. Failing any player by that name, the **volume device itself** when it is
   playing or paused with artwork of its own: a receiver on its own Spotify or
   net radio.

Everything about what is playing -- state, title, art, progress, features,
the history -- comes from that entity, and previous / play-pause / next go to
it. Volume always goes to the volume device. Pressing play while the room
hears another input plays that input's player where it is -- paused, idle or
off -- and never switches the device back to this remote's player; choosing
something from the list or the search does that. Play and pause go out as
`media_play` and `media_pause`, decided from the state of the entity they
are sent to, never as a toggle that could pause a player playing unheard.
Only while the volume device is on another input does the subscription read
every `media_player`, which Home Assistant rate-limits to once a second.

**A phone casting to a Music Assistant player's device is controlled
through the device.** While something other than Music Assistant is
casting to the Chromecast a Music Assistant player wraps -- the player
reports it as its `app_id`, `youtube_music` say -- play, pause, next and
previous go to the native entity of the same name, which that app answers
to, and the buttons are greyed by that entity's features. The track and the
art still come from the Music Assistant player, which follows the cast, and
the list and the search still play through it. Whether it is playing comes
from the native entity, since Music Assistant reports a cast that is
buffering as playing, and one started from a phone while paused sits
buffering until it is played. Play and pause are decided from that state, so
the buttons go to the entity it was read from -- although Music Assistant
2.10.5 does pass play, pause and next on to a cast.

The same goes for a device playing something of its own that Music Assistant
does not report at all -- a TV in Disney Plus while its Music Assistant
player sits idle. Then the native entity is playing or paused and the Music
Assistant player is not, so the buttons go to the native entity, and what is
shown comes from it too: its title (or the app's name), its art and its
state, which play and pause are decided from.

The native entity is found once, by name, when the stream opens, and passed
into the subscription, which then reads just that entity besides the player.
A rename shows up the next time the stream opens. Without an answer to that
lookup the subscription looks for the entity itself, reading every
`media_player`.

## Powering the volume device

Switched **on** (`turn_on`, skipped when it is already on or has no
`TURN_ON`) only from the remote: when a press of play starts playback --
power first, then play, so the amplifier is awake before the audio starts --
when something is chosen from the list or the search, before the
`play_media` that takes several seconds anyway, and by a tap in the top half
of now playing. A press that pauses does not, nor does skip-next in a dark
room, which is a mis-tap rather than a request for music.

**The tap toggles.** A tap in the top half of now playing -- the half a
sideways swipe sets the volume in -- calls `media_player.toggle` on the
volume device, for one that claims `TURN_ON` or `TURN_OFF`. Home Assistant
decides which, from its own state at that moment, so the tap does not
depend on what the remote last heard. The remote then waits, up to five
seconds, for the state stream to report the device's power changed -- the
volume greys when that arrives -- and if the device came on, it also gets
the player's input and the player volume at switch-on, as play's power-on
does. It waits for the stream rather than asking: a request of its own
needs a second TLS session, which the Qualia has no internal RAM for. A zone amplifier needs the input: a
Triad output switched off is disconnected from its input, and switched back
on with none it is silent. A tap is a press that moved less than the tap
slop and lifted within `kTouchTapMaxMs`, so a volume swipe is never taken
for one; a tap on a dark screen only wakes it.

Nothing else switches it on: not a touch that wakes the blanked screen, and
not the player starting by itself. A player can feed several rooms -- a
Chromecast that every zone can be switched to, say -- and a cast from a phone
to it would otherwise switch on the amplifier of every remote that follows
it.

**A zone amplifier also gets its input.** A Triad output switched on routes
nothing and comes up silent, so where the volume device has an input for the
player (chosen in the settings, or named after it) powering it on also calls
`select_source`, unless it is already on that input. A device that was on
already is taken over whatever it is on, since only play pressed on the
remote, or something picked from its list or search, gets this far. A press
that pauses never touches the source. `kSelectControlSource` turns this off.

**Switching it on also sets the player's volume**, to **Player volume at
switch-on**, if a level is set; by default it is left alone.
Only when the remote has just switched the volume device on -- not when it was
on already, and never when the player starts by itself. With a separate
amplifier the player's `volume_level` is the gain on the signal it hands over;
left to drift, a quiet source gets the amplifier turned up and the next
correctly-set track is deafening. Power, input and volume all come before the
play.

Switched **off** when the screen blanks (`kTurnOffControlOnBlank`), once per
stretch of darkness, and only when it claims `TURN_OFF`. Blanking already
needs the player stopped for the whole five minutes, so the amplifier cannot
be cut under something playing.

## The volume swipe

Relative, not absolute: the level moves the same distance along the arc or
bar that the finger moved across the glass -- about two and a half full-width
swipes for the whole range on the round panels. Only a press that commits
sideways (`|dx| > |dy|`, past the tap slop) is claimed, so a swipe up still
opens the list, and where the press *started* decides it. The arc repaints as
you drag; `volume_set` is throttled to one per `kVolumeSendIntervalMs`, with a
final one on release, so what lags is the audio, not the screen.

**A state update cannot undo a level the device just set.** The level last
commanded is believed over any report that disagrees until one confirms it
(within 0.02, since receivers quantise) or four seconds pass; the level is set
optimistically at the moment of each command, before the drag flag clears.
Without that, a report issued before the command arrived snapped the arc back,
and the next swipe re-applied the same increase from the stale level.

## Recent here: the room's history

The device keeps the last **16 artists heard on its player**, most recent
first, from the state it already receives -- anything that plays counts,
started from the remote, a phone or an automation. A new artist costs one
Music Assistant artist search, to turn the name into something playable with a
picture; only an exact match is kept, trying the lead artist of "A feat. B" or
"A & B" when the whole credit finds nothing. An artist already on the list
moves to the front. A name that finds nothing -- a radio station's "Artist -
Title" -- is remembered and not searched again. An artist picked from the list
or the search goes on at once, from the pick itself.

It lives in its own NVS namespace, so a BOOT reset keeps it. NVS keeps each
artist's **name and URI** -- about 2 KB for all sixteen, in a 20 KB partition
the radio's calibration data and the Wi-Fi settings share. It is written at
once when an artist new to the list is added, and otherwise at most once an
hour (`kHistorySaveIntervalMs`), so artists moving back to the front do not
rewrite it each time. **Pictures are kept in RAM**: after a restart the list
shows names, and the network task finds one picture again every five seconds
(`kHistoryPictureLookupMs`) by the same search, keeping it only for the same
artist. The last track heard by each artist is kept in RAM too, as a seed for
the recommendations.

The list reloads as soon as the history changes. With a Music Assistant
token, opening it costs nothing: both sections come from memory.

## Recommended: artists like this room's

Home Assistant does not pass similarity through -- the Music Assistant
integration's services are `search`, `get_library`, `play_media`,
`play_announcement`, `transfer_queue` and `get_queue` -- so this comes from
Music Assistant's own API (`POST <server>/api`), which is why it needs its
own token. The server is plain HTTP on the LAN, so it costs no TLS session.

For each of the room's **three most recent artists** it asks for **similar
artists**. A library artist gets a good answer, merged from its providers,
with pictures. YouTube Music offers no similar artists of its own, so for one
of its artists it asks instead for **tracks similar** to one of theirs that
played here -- what artist radio would pick -- and takes their artists, each
shown with that track's cover. After a restart, until one of theirs plays
again, the artist's top track stands in as the seed.

Those calls take seconds each, so they run on a task of their own
(`services::recommend`, its stack in PSRAM). It builds a **pool of up to 48
artists** when the three change -- once they have held still for ten
seconds, so artist radio working through a run of names is one rebuild -- and
every six hours regardless. Each artist in the pool counts how many seeds
pointed at it. A list load **draws eight at random, weighted by that count**,
leaving out anyone already on Recent here: the section changes from one load
to the next and leans toward the artists more than one seed agrees on. Until
there is a pool, a random draw of library albums stands in.

## The list and the search

**Loaded before you ask.** The network task loads the list in the background
(`browse::preload()`), so a swipe up lands on a list rather than a loading
card. It does nothing unless the list is missing, past `kBrowseTtlMs` (five
minutes) or its sources changed, and never while the list is on screen. Every
entry point in `services/browse.h` takes a mutex and returns by value;
`browse::Row` carries a copy of the title for exactly that reason.

**Scrolling** follows the finger: the list claims a press once it commits
vertically, and cancels the gesture so the release is not also a tap. On
release it glides, decaying 12% a frame, stopping at either end; a finger that
came to rest before lifting gets no glide. Position is in pixels, not rows,
and the list scrolls until the **last row's centre reaches the middle** of the
panel -- on a circle the last row would otherwise rest in the pinched bottom,
narrowest and hardest to hit.

**Tapping acknowledges before it blocks.** `play_media` on an artist makes
Music Assistant build a queue before it answers -- measured at over four
seconds, past six on another instance -- so the row is drawn alone with
"Starting..." first, and nothing more is drawn after: the next state replaces
it with the track. That call has `kHaServiceTimeoutMs` (25 s) rather than the
ordinary 6 s, which turned a working request into a failure that was then
retried.

**Thumbnails** are fetched on the artwork worker and kept **decoded**, as small
sprites claimed once at boot -- they come from the provider's CDN, so decoding
at draw time would mean re-fetching on every repaint. They are requested at
thumbnail size: Music Assistant hands back whatever the provider stored, once
1.9 MB for ten images. `thumbnailUrl()` rewrites the two hosts it returns --
`googleusercontent.com`'s `=w600-h600-p` to the thumbnail's size, and
`resources.tidal.com`'s `/750x750.jpg` to `/80x80.jpg` -- and a host that
refuses costs one retry with the original URL. The worker keeps its connection
open across a batch and takes the queue grouped by host, so a list's worth
costs a handshake per host rather than per image, and closes it when the queue
empties: an idle TLS session is tens of kilobytes of internal RAM. Names go on
screen at once; arrivals are gathered into one repaint per `kThumbRepaintMinMs`,
and a row without its picture centres its name, so nothing jumps.

## Cover art

**Fetched on the artwork worker, ahead of any thumbnail**, not on the task
that services the stream. The network task only asks for it
(`ui::requestArtwork`) and goes back to the stream, so a slow cover never holds
up a volume change or the next track. At most one thumbnail fetch is ever
ahead of it, and a newer cover replaces one still waiting.

**Held compressed** in one static buffer (`board::kCoverArtBufferBytes`,
128 KB to 512 KB by board, in PSRAM), claimed at boot before Wi-Fi and never
freed -- sizing it per image meant a free and a differently sized `malloc`
every track, which is how a long-running heap fragments. The image's
dimensions come from its JPEG `SOF` or PNG `IHDR` so it is centre-cropped to
fill. A cover too big for the buffer is streamed through the decoder on every
repaint instead, which is the case that costs.

**Asked for at the panel's size, or below it.** Music Assistant's image
proxy accepts `size=` from a ladder -- 0, 80, 160, 256, 512, 1024 -- and
answers 400 to anything else, so the request rounds up to a power of two
(`board::kCoverArtRequestPx`): 256 for a 240 px panel, a 28 KB cover instead of
88 KB. The Qualia asks for 256 too and scales it up: the cover is a
backdrop under a scrim. The 480 px round panels, the 2.8C and the MaTouch,
ask for 512, which a 256 px cover scaled up made look soft; on the 2.8C that
costs about 0.16 s more per track change. A refusal is retried with the
original URL and rewriting stops for the session.

**A small cover is scaled in one pass.** One smaller than the panel is decoded
at its own size into a PSRAM sprite laid out as the frame is, then copied in
nearest neighbour, a row at a time. Scaling while decoding, through
LovyanGFX's drawing calls, cost about 200 ms on a 480 px frame whatever the
source's size.

**Decodes are serialised.** LovyanGFX keeps one PNG decoder in a static and
reuses it, so two tasks decoding at once corrupt each other -- which once
surfaced as a crash in the Wi-Fi task seconds later, with nothing of this
firmware in the backtrace. One lock covers every decode. TJpgD is
baseline-only, so a progressive JPEG (Tidal serves them) renders without art.
And every byte the decoder asks for is delivered: LovyanGFX asks for a minimum
of 2 whatever it wants, pngle tolerates a short read and TJpgD does not.

## Compositing

Every screen composes into one off-screen frame in PSRAM, the panel's size at
16 bits -- 115 KB on the Waveshare, 259 KB on the DFR1221, 450 KB on the
2.8C and the MaTouch, 1 MB on the Qualia -- and presents it, or the rectangle that changed, to
the panel. Everything draws in screen coordinates. The elapsed chip sits on an
opaque pill, so its once-a-second update is drawn and presented alone,
without recomposing the cover; for the same reason a state update only
recomposes when something visible changed, and position alone does not.

**Two frames are kept** beside it, in PSRAM: the cover under its scrim, and
that with the track's text and its glow on it, each keyed on what it shows.
A play, a pause or a volume step restores the second and draws only the
controls over it; a radio title changing over the same cover restores the
first and draws the text. Only a new cover is decoded. On the 2.8C that took
a tap's repaint from 0.5-1 s to about 95 ms.

**Rotation** is below the seam. The round SPI panels turn in their own
controller, with LovyanGFX turning the touch to match, at no cost. The RGB
panels, the Qualia's, the 2.8C's and the MaTouch's, scan out a framebuffer in a fixed order,
so there the frame is drawn upright
and turned as it is copied into the scan-out buffer (`rgbPresent()`), and every
touch is turned back. Upside down costs nothing extra; at ±90° the copy writes
down columns, a full present is slower, and a list scroll repaints instead of
moving rows. The frame is not drawn into a turned LovyanGFX sprite, which is
how it used to be done: anti-aliased text drawn that way came out with every
other row black.

## The Qualia

**A different kind of panel, not just a bigger one.** The TL040HDS20 is
RGB-parallel: the S3's LCD peripheral streams a framebuffer continuously over
16 data lines plus PCLK/HSYNC/VSYNC/DE, with **no command channel**. Its reset,
the backlight and two user buttons hang off a TCA9554 I2C expander at `0x3F`;
there is no controller to program, only a reset pulse; and GPIO 0 carries a
blue data bit, so the expander's DOWN button stands in for BOOT.

**One framebuffer, scrolled in place.** esp_lcd scans out one framebuffer
straight from PSRAM; screens compose into a separate canvas and
`rgbPresent()` copies the changed rectangle across. One buffer rather than two
because it then always holds the frame on the glass: a list scroll **moves the
rows already on the panel** (`rgbScroll()`, through `canvasScrollPanel()`) and
composes only the strip the move uncovers and the scroll bar's column -- a
row on a square panel does not change shape with its height. The bar sits
flush with the right edge so each row still moves as one run.
`canvasPanelWrites()` counts every panel write, so the list knows when
something else drew over it and falls back to a whole repaint; so does a jump
of more than half the view. The 2.8C and the MaTouch take this path too, sharing the output
stage; the round SPI boards never do, since their panels only write.

**The bus is kept off the PSRAM's neck.** The scan-out reads continuously and
cannot be starved of a byte: `copyRegion()` and `rgbScroll()` stand off every
`kCopySlabRows` rows' worth of bytes, composing every `kComposeSlabRows`,
`board::kListRedrawMinMs` stops a scroll asking for frames faster than one can
finish, and the every-VSYNC restart (on in Arduino's prebuilt libraries) pulls
the picture back if an underrun still gets through.

**The panel bus talks over the Wi-Fi.** Nineteen pins switching at the pixel
clock beside a 2.4 GHz front end made every Home Assistant request take tens
of seconds -- 72 s once -- with RSSI reading a healthy −54 dBm throughout,
because RSSI measures the frames that arrived, not the noise. Edge rate and
clock both matter:

| Drive strength | First request | Steady state |
|---|---|---|
| `GPIO_DRIVE_CAP_2` (default) | 72 s | connects timing out |
| `GPIO_DRIVE_CAP_1` | 11.4 s | 2.7–26.7 s |
| `GPIO_DRIVE_CAP_0` | 0.5 s | 16–28 ms |

It ships at **`CAP_0` and a 12 MHz pixel clock** (`quietenBus()` in
`hardware/rgb/rgb_panel.cpp`, after esp_lcd has configured the pins): the drive
carries the radio, and the clock is down for PSRAM bandwidth, since at 16 MHz
a scrolling list shifted the frame sideways. That is 19.5 Hz on the glass. If
the panel ever speckles or drops columns, the drive strength is the first
thing to put back. The number to watch when changing any of this is the first
request after boot, which is the TLS handshake's run of round trips.

| Symptom | Fix |
|---|---|
| Colours inverted or byte-swapped | `board::kFrameNativeByteOrder` |
| Touch does nothing | The FT6336 is at `0x48`, not `0x38`; the boot log prints an I2C scan |
| Image offset sideways | Swap `kRgbHsyncBackPorch` / `kRgbHsyncFrontPorch` |
| Image torn or shimmering | `kRgbPclkActiveNeg` |
| Picture breaks up while a list scrolls | PSRAM bandwidth: lower `kCopySlabRows` or raise `kListRedrawMinMs` |
| Stale rows or a scroll-bar trail after scrolling | Something presented without `canvasPresentRegion()` or `canvasScrollPanel()` |
| Requests take tens of seconds | The panel bus: `kRgbPclkHz` and the drive in `quietenBus()` |

## The 2.8C

The Qualia's kind of panel in the round screens' shape: an ST7701 on the same
RGB output stage (`hardware/rgb/`), with the same drive strength and a 12 MHz
pixel clock -- 43 Hz on its smaller frame. Waveshare's demo runs it at 18 MHz,
which is 36 MB/s of scan-out from PSRAM, half as much again as the Qualia
breaks up at.

Unlike the Qualia's panel, the ST7701 has to be told its registers before it
shows anything: a 3-wire SPI on GPIO 1 and 2, chip-selected through a TCA9554
expander at `0x20`, which also holds the panel's and the touch controller's
resets and the buzzer (`hardware/lcd28c/panel.cpp`). The expander powers up
with every output high, so its first write drives the buzzer low; its three
sensor interrupt lines are left as inputs. The GT911 touch controller is read
on a task of its own, as the Qualia's is, at `0x5D`, the address it takes
with its INT line held low as its reset is released.

## The MaTouch 2.1"

The Makerfabs MaTouch ESP32-S3 Rotary 2.1" is the 2.8C's kind of panel -- a
480×480 round ST7701 on the RGB output stage, at 12 MHz -- without the
expander: its chip select is GPIO 1, it has no reset line (the init starts
with a software reset), and touch has a bus of its own. It also has a knob:
a rotary encoder on GPIO 13 and 10, counted by the S3's pulse counter, and
its push switch on GPIO 14, caught on an interrupt (`hardware/encoder.cpp`,
for any board whose header defines `BOARD_HAS_ENCODER`).

Its pins and init are those of Makerfabs' factory test as built against
Arduino_GFX 1.3.8, whose image shows true colours on this glass, and three
things about them were found on it:

- **Red is 4/41/5/40/6 and blue 11/15/12/16/21**, with the panel in BGR
  order and 18-bit colour, as that build has it. Makerfabs' older sketches
  label the two groups the other way round.
- **The init is bit-banged** (`hardware/matouch21/panel.cpp`), as
  Arduino_GFX's software SPI sends it. Through the S3's SPI peripheral, at
  4 MHz the colours came out wrong whatever the colour settings, and at
  500 kHz the panel stayed dark. GPIO 0, its data line, is the BOOT button
  too, and is handed back to it after.
- **The CST826 touch controller** (`0x15`, no interrupt or reset wired) is
  polled. It reports the far edge as 480, now and then answers with noise
  when idle, and sometimes says no finger in the middle of a press; the edge
  is clamped, any read that is not one or two fingers on the glass ignored,
  and a press held through 60 ms of misses.
- **The knob counts up anticlockwise**, so `kEncoderReverse` turns it round.

## Memory

Internal RAM is what mbedTLS needs -- 16 KB in and 16 KB out, contiguous, per
TLS session -- so everything large lives in **PSRAM** and is claimed **once, at
boot, before Wi-Fi**, while the heap is unfragmented: the frame, the cover
buffer, the thumbnail sprites, the history, the recommendation pool and its
response buffer, the recommendation task's stack. The two kept frames are
claimed by the first repaint that keeps one, and are likewise never freed. What breaks on this class of
board is contiguity, not total free memory, and memory claimed once cannot
fragment anything later.

**Flash** (`partitions/media_remote.csv`, 16 MB on every board): the settings
partition (NVS, 20 KB at 0x9000), OTA data, two 4 MB app slots, spiffs,
unused, and 64 KB at the end for a core dump. The DFR1221's env declares its 16 MB explicitly; the generic
board definition it builds on says 8.

**Settings** are in NVS, each group in its own namespace so a reset can clear
one and keep another: `hamedia` (Home Assistant), `ma` (Music Assistant),
`device` (the name), `display` (rotation, keyboard) and `history`. The Wi-Fi
credentials are the Wi-Fi stack's own, beside a `wifi` namespace holding one
flag: open the setup portal on the next boot.

## Adding a panel

1. `include/board/<board>.h` -- pins, size, `kUiScale`, `kTextScale`,
   `kListScale`, touch raster, thumbnail and cover sizes.
2. `src/hardware/<board>/` -- `display.cpp`, `touch_raw.cpp`,
   `font_table.cpp`, `boot_button.cpp`.
3. `src/ui/<shape>/` -- only if the shape is new; a round panel reuses
   `ui/round/`.
4. An env in `platformio.ini` excluding the other boards' `hardware/` and
   `ui/` directories -- and exclude the new ones from the existing envs,
   which is the half that is easy to forget.
5. Fonts at the sizes the board asks for (see [Fonts](building.md#fonts)).

`scripts/board_header_stamp.py` hashes the board header into the build, since
the header is included through a macro SCons cannot follow: without it an edit
to the header rebuilt nothing.
