# Changelog

What changed in each release, newest first. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/).

Changes go under **Unreleased** as they are made. To release, rename that
heading to the version and the date, add an empty Unreleased above it, update
the links at the foot, and push a `v` tag: the build workflow drafts the
GitHub release from that version's section, with
[`.github/release-footer.md`](.github/release-footer.md) after it.

## [Unreleased]

### Added

- **"Hey Jarvis" lights a dark screen,** as a tap does, so what a voice request does -- the Starting... card, the music paused, the volume moved -- can be seen.
- **"Hey Jarvis, play Khatia Buniatishvili"** plays on this remote's player, switching the volume device on and to the player's input first, as a pick from the search would, says "Playing Khatia Buniatishvili" at once and shows the words on the Starting... card: an artist near enough the words heard plays as radio, and anything else is whatever Music Assistant finds by that name. **"Play", "pause", "skip", "previous", "volume up" and "volume down"** work the remote's own player and volume device the same way, play and pause sent as the buttons send them whatever the player is doing, and **"stop" or "turn off"** switches the volume device off, as the power chip does -- the player, where there is no volume device. A request that names its player ("... on the stereo") still goes to Assist. Requests are now heard first and acted on second, as two runs of the pipeline.
- **A voice request that fails says so,** where before it went quiet after the beep: "Sorry, I didn't catch that" when Home Assistant heard nothing, "Sorry, Home Assistant couldn't answer that" for any other failure -- an error from the conversation agent, say -- in the pipeline's voice, and two low falling notes when Home Assistant cannot be reached to speak either.
- **Firmware from GitHub:** the settings page has a Firmware row showing the version running, and a tap on it asks GitHub for the latest release -- when it is newer, install it there and then, its download's progress on screen, and the remote restarts into it with its settings kept. The portal has the same, as **Firmware from GitHub** on its menu. Only that board's image is fetched, and GitHub's certificate is checked.
- **Memory on the portal's Info page:** internal RAM and PSRAM, used, free and total, with the largest free block -- the most one allocation can have -- and for internal RAM the least that has been free since boot.
- **Chinese, Japanese, Thai, Cyrillic, Greek, Vietnamese and symbols:** track titles and artist names in Chinese (GB2312's 6,763 characters, and the common Traditional ones), Japanese (kana and JIS X 0208's 6,355 kanji), Thai, Cyrillic, Greek and Vietnamese show in those scripts, where they were blanks, once the remote has downloaded its fonts: **Fonts** on the settings page, or **Download fonts** on the portal's menu. So do the rest of Latin's letters, letters spelt with combining accents -- an e followed by a combining acute -- and the symbols titles use: ♪ ♥ ★ ™ → ✓ ☀ ① ¼ † and the like. The fonts -- Noto Sans SC, JP, TC, KR, Thai, Math, Symbols, Symbols 2 and Noto Sans, 5.1 MB -- come from GitHub into flash that was going unused, take no room in the firmware or RAM, and are drawn from as soon as they are down, without a restart. A Han character Chinese and Japanese share is drawn in its Chinese form. Thai's tone marks are lifted clear of the vowel marks under them; other stacked marks are drawn as the font places them.

### Changed

- **A power tap shows at once:** tapping now playing to switch the room's volume device on or off turns the volume ring or bar a darker grey, without its knob, straight away, until Home Assistant's state shows what the tap did -- before, nothing changed on screen for the second or so that takes, and a tap could be taken for missed.
- **Short lists sit in the middle of a round panel:** a setting's choices, Restart? and the firmware's answer started up under the title, where the circle narrows the rows, and a one-row answer was cut to a sliver; a list short enough now sits in the middle, where they are widest.
- **The P4 4B's image is 1.5 MB smaller,** 2.3 MB of its 4 MB app slot where it was 3.8: its fonts are stored compressed and unpacked into PSRAM as it starts, and the Korean font is no longer in it.
- **Korean comes with the downloaded fonts,** as Chinese and Japanese do, rather than in the firmware: every board's image is 420 KB smaller for it. Until a remote has downloaded its fonts -- **Fonts** on the settings page, or **Download fonts** on the portal's menu -- Hangul shows as blanks.
- **Other languages' Latin letters and symbols come with the downloaded fonts** too, where 1.5.0 put them in the firmware: Azerbaijani's ə, Welsh's ẁ ẃ ẅ ỳ, the Sami letters, the ʻokina, Navajo's ǫ, Guarani's ẽ ỹ, Yoruba's and Igbo's dotted letters, Hausa's hooked letters and Akan's ɛ ɔ, and € £ ¥ © ® µ × ½ • ‚ ‹ ›. The firmware keeps the letters of western and central Europe, Romanian, Turkish and the Baltic languages; until a remote has downloaded its fonts, the rest show as blanks.

### Fixed

- **A second Home Assistant connection no longer outlives the stream's:** a request begun just as the state stream opened kept its own TLS connection for good, about 40 KB of internal RAM beside the stream's. On a MaTouch whose list loads its pictures over HTTPS, that left a picture's connection so little that internal RAM ran down to a few hundred bytes as it started, and Wi-Fi dropped packets. A connection kept that way is now closed once its request is done.
- **One TLS handshake at a time:** the Home Assistant connections and the list's pictures could each start one at once as the remote came up, and together they took nearly all the internal RAM a handshake has to have. Each now waits for the one before, and for room to start in.
- **Korean scrolls as fast as Latin text:** the Korean glyphs a face keeps were placed by code point, and Hangul's layout put most syllables in a quarter of the places, so a list of Korean names drew most of its letters again on every frame -- on the round 360, a page took twice as long as the same page in Latin. They are now spread over the whole cache, a few to a place, and a page shown once draws again at about the speed of Latin text.

## [1.5.0] - 2026-11-03

This release shows Korean -- artist names and track titles in Hangul, drawn from an outline font at the size of the text around them -- and the letters and symbols a dozen more Latin-script languages need, and adds the Waveshare AMOLED 1.75C, built and on the install page though not yet tried on a board. The list's top section can be Music Assistant's recent artists, which every player shares, the portal can forget the Home Assistant token, and a swipe down from the top edge of a round panel no longer switches the volume device on.

### Added

- **Waveshare ESP32-S3-Touch-AMOLED-1.75C**, built as `amoled466c-s3`: the 1.75's build with the 1.75C's display and touch resets, on GPIO 1 and 2. Not yet tried on a board; on the install page.
- **Korean:** artist names, track titles and anything else in Hangul show in Korean, where they were blanks -- the 2,350 common syllables, drawn from an outline font (Noto Sans KR, as the Latin text is Noto Sans) at the size of the text around them. About 420 KB in every board's image.
- **More letters and symbols:** the fonts have the letters Azerbaijani, Welsh, Skolt and Inari Sami, Uzbek, Hawaiian, Navajo, Guarani, Yoruba, Igbo, Hausa and Akan need beyond the European set, and € £ ¥ © ® µ × ½ • ‚ ‹ ›, which showed as blanks. A letter sent with a separate accent, such as Yoruba's ọ or the ụ of a Vietnamese name, is put together as one.
- **Recent list, a setting:** on the settings page and in the portal. *Music Assistant*, the default, makes the list's top section Music Assistant's recent artists, which every player shares -- and no longer only for want of a token; *Room only* makes it the artists played on this remote's player, as it was before.
- **Forget the Home Assistant token from the portal:** a checkbox under the token field. Saved ticked, the remote forgets the token, keeping the URL, and restarts to the sign-in QR code to link again. The next link deletes the forgotten token in Home Assistant, as it does the one a link replaces -- and so, now, does the link after a refused token is tapped away.

### Changed

- **Recommended without a Music Assistant token** is eight artists from the library at random, rather than your favourite artists topped up at random. With a token it is artists like the first three of the list's top section -- the server's recent artists when Recent list is set to Music Assistant.

### Fixed

- **A swipe down from the very top of a round panel** no longer switches the volume device on: a finger coming on from the bezel is read sliding along the rim at first, and the volume took that for a sideways drag. A press that lands at the rim now has to go plainly sideways before it moves the volume.

## [1.4.0] - 2026-10-30

This release adds three boards -- the Guition ESP32-4848S040, the Waveshare AMOLED 1.75 and the Waveshare ESP32-P4 4B, the first on an ESP32-P4 -- and shows how far through the track it is, in the album art's colour, with a drag along the line to seek. The search finds albums, tracks, playlists and radio as well as artists, an artist on the swipe-up list starts their radio, and the screen no longer stops while Home Assistant answers a button.

### Added

- **Waveshare ESP32-P4-WIFI6-Touch-LCD-4B:** a 4" square 720×720 ST7703 on MIPI-DSI with GT911 touch, on an ESP32-P4 with an ESP32-C6 for Wi-Fi, built as `lcd4b-p4`, laid out as the Qualia, and on the install page. For a P4 before revision 3; a revision 3 board needs the `esp32-p4_r3` board definition. It is built with ESP-Hosted 2.13.1 and its network buffers in PSRAM -- with the stock libraries the link to the C6 stalled for good at the first cover art download -- and the C6 should run ESP-Hosted 2.12.13 or later. It asks for 1024 px cover art; a cover smaller than that at its source comes whole.
- **Guition (Sunton) ESP32-4848S040**, also sold as AITRIP's "ESP32-S3 4.0 inch 480 × 480": a 4" square 480×480 ST7701 with GT911 touch, built as `guition4848-s3`, laid out as the Qualia at two thirds of its pixels, and on the install page. It has no BOOT button, GPIO 0 being a display line: a finger held on the screen for 10 s, begun within 15 s of power-up, clears the settings instead.
- **Waveshare ESP32-S3-Touch-AMOLED-1.75:** a 1.75" round 466 px CO5300 AMOLED with CST9217 touch, built as `amoled466-s3` and on the install page, at a brightness of 200 of 255 since an OLED ages with what it shows. For the 1.75 (Rev 1.1, with an SD card slot); the 1.75C has its resets on other pins and has not been tried.
- **Track progress on now playing:** an arc at the foot of the volume ring on the round boards, a bar along the foot of the screen on the square ones. It takes its colour from the album art -- the cover's most prominent strong colour, at full brightness -- as does the ring of a pressed transport button; a black and white cover, or none, leaves both in the accent colour.
- **Seek by dragging the progress line:** a sideways drag that starts on the arc or bar, or on the elapsed chip, moves the track to the point under the finger when it lets go. Only where the player can seek.
- **Search for more than artists:** a pill between the name and the keys picks Artist, Album, Track, Playlist or Radio, a tap stepping to the next. Albums and tracks are listed with their artist. An artist still starts their radio; anything else plays as itself.
- **"Off in this room":** while the player plays but this room's volume device is off, an amber chip at the top of now playing says so; a tap switches the room on.
- **Auto-off setting:** whether the volume device is switched off when the screen blanks after five minutes idle, on the device's settings page and in the portal. On by default, as before.
- **A source with its own power keeps the room on:** when the volume device is on a source that can switch itself off -- a Control4 room, a TV being cast to -- the screen no longer blanks and the device is not switched off while that source is idle or paused; the five minutes start once the source is off.
- **Favourite artists stand in for Recommended:** until the room has recommendations, or without a Music Assistant token, Recommended lists your favourite artists in Music Assistant, topped up from the library at random, instead of random albums. An artist already under Recent here is not listed again.
- **A way back from Starting...:** the card shown after a pick has the back button, which stops waiting -- as do a swipe right and the MaTouch's knob -- and a swipe down to the status page. A pick not yet sent to Home Assistant is dropped; one already sent still starts.
- The swipe-up list has the back button, on its first heading.

### Changed

- **The screen keeps going while Home Assistant answers:** play, pause, skip, the volume, power, an input and a pick were sent from the loop that reads touch and paints the screen, which stood still until the answer came -- six seconds for play on an idle Music Assistant player. They go from a task of their own now; the button shows the change at once and stays lit until the answer.
- **An artist on the swipe-up list starts their radio**, under Recent here or Recommended, as one picked in the search does, rather than playing their tracks straight through.
- **Swipes:** a swipe left goes back on the search and on the inputs, as a swipe right did. A quick flick across the top half of now playing is the swipe to the search or the inputs, as on the lower half; a slower drag there still sets the volume.
- On the square boards the swipe-up list starts near the top of the screen, without the empty band above it.
- **Sharper covers on the Qualia:** 512 px instead of 256 px, as on the 2.8C.
- With the screen turned a quarter, the RGB boards copy each repaint a tile at a time; on the 4848S040 the old copy showed as vertical noise.
- **More internal RAM for Home Assistant's secure connections:** allocations of 256 bytes and up go to PSRAM first, where before only those of 4 KB and up did -- 2 KB and up on the boards that scan their screen out of PSRAM, where smaller ones made the picture blink -- and the cover art worker's stack and the media player list are in PSRAM too.

### Fixed

- The volume device's inputs now and then came up "Could not load inputs", the TLS connection short of internal RAM. Such a request now waits for room, up to a second, and tries again.
- A remote could restart when internal RAM ran out just as an mDNS packet arrived.
- With no Music Assistant config entry set, the swipe-up list's preload was tried, and logged, several times a second; after a failure it now waits a minute.
- The Starting... card could give way to now playing, still showing the old track, seconds before the pick played. It now stays until the pick is playing, or for up to a minute.
- An accented letter sent as a plain letter and a separate accent, as Music Assistant sends some titles, lost its accent.

## [1.3.0] - 2026-10-13

This release adds a sixth board, the Makerfabs MaTouch ESP32-S3 Rotary 2.1", whose knob sets the volume and scrolls the lists, and makes the remote's error screens something you can act on from the device: every card about a problem leads to the status page and the settings, a rejected token can be linked again with a tap, and now playing says when Home Assistant has gone quiet.

### Added

- **Makerfabs MaTouch ESP32-S3 Rotary 2.1":** a 2.1" round 480 px ST7701 panel with CST826 touch, built as `matouch21-s3`, and on the install page. Its knob turns the volume on now playing and scrolls the lists, a row a click; a press plays or pauses on now playing, goes back one step elsewhere, and taps a card.
- **Sources:** a swipe right on the lower half of now playing lists the volume device's inputs; tap one to switch to it, switching the device on first if it is off.
- **Search from now playing:** a swipe left on the lower half opens the artist search directly. The search has a back button at the top left.
- **Status and settings from a card:** on Home Assistant not reachable, Token rejected, No Wi-Fi and Link to HA, a swipe down opens the status page and a gear at the top right opens the settings. The sign-in QR code has the swipe down, and the player list the gear. A swipe up on the status page goes back to where it was opened from.
- **Link again from a rejected token:** the card, or the amber chip on now playing, says **Token rejected**; a tap puts the sign-in QR code back up, keeping the URL. Meanwhile the remote asks Home Assistant for nothing, trying the token again only every ten minutes, so it does not fill the log with failed sign-ins or get its address banned.
- **Offline mark:** when Home Assistant has not been heard from for 10 s, now playing's elapsed time turns amber and says **Offline**, and the status page gives the error on an **HA** line.
- **Back on Wi-Fi after a router outage:** a remote that started while its router was down retries the saved network every minute from the setup portal, and carries on by itself once the router is back.
- **Documentation:** the README keeps the interface, setting up and the portal; building, the code, internals, troubleshooting and a table of every error card are in [docs/](https://github.com/pvanbaren/ha-media-remote/tree/main/docs).

### Changed

- **A device playing outside Music Assistant is controlled directly:** where the Music Assistant player is idle while its native entity plays something of its own -- a TV in Disney Plus -- now playing shows the native entity's title, art and state, and the buttons go to it.
- **Volume with the volume device off:** a volume swipe, or a turn of the MaTouch's knob, switches the device on first, as play does, where it can be switched on; the next one sets the level.
- **Losing Wi-Fi while running:** the No Wi-Fi card, "Signal lost, retrying", replaces a frozen Connecting card, the remote keeps retrying in the background, and goes back to now playing when the link returns.
- **Status page:** the gear is at the top right, and the received and transmitted power have a line each.
- **Sharper covers on the 2.8C:** 512 px instead of 256 px, about 0.16 s more per track change.
- **List rows at the edge of a round screen** keep half the panel's width instead of pinching to a sliver.
- **Settings before linking** leave out Media player and Volume & power.
- **The player list lights a tapped row** while the choice is stored.

### Fixed

- After choosing a media player or Volume & power device on the device, the settings and status pages showed the old one until reopened.
- Linking in the portal while the Link to HA card or QR code was up left the card on screen until it was tapped.
- After Wi-Fi dropped, the first attempt to rejoin could fail because the core's own reconnect was still running.
- Home Assistant logged a template error on every update when a list in the remote's templates was empty.
- The DFR1221 had no serial console on its USB port.

## [1.2.0] - 2026-10-06

This release adds a fifth board, the Waveshare ESP32-S3-Touch-LCD-2.8C, and two controls on now playing: a tap to switch the amplifier on or off, and Isolate, which turns off the other rooms sharing this one's input. It also makes now playing much faster to repaint on every board.

### Added

- **Waveshare ESP32-S3-Touch-LCD-2.8C:** a 2.8" round 480 px panel with GT911 touch, built as `lcd28c-s3`, and on the install page. The USB-C port is the S3's own USB, so uploads and the serial console need no driver.
- **Power tap:** a tap on the top half of now playing, where a sideways swipe sets the volume, switches the volume device on or off. This works for a device that supports switching on or off. Home Assistant decides which way, from the device's state at that moment. When the tap switches the device on, it also gets the player's input and the player volume at switch-on, as play does. The volume is drawn grey while the device is off.
- **Isolate a room:** when other zones of the volume device, such as other Triad outputs, are on this room's input, a **+N rooms** chip appears at the top of now playing. Tap it for a card listing those rooms, with **Isolate** and **Cancel** buttons. **Isolate** turns them off while this room keeps playing. A zone that changes input before you press it is left alone.

### Changed

- **Faster now playing:** play, pause and volume no longer redraw the cover and the track text from scratch. A tap's repaint went from 0.5–1 s to about 0.1 s on the 2.8C, and a new cover appears in about half the time.
- **A zone follows its input's player whatever that player is doing:** with the volume device on another input, the remote shows and controls that input's player even while it is idle or off, rather than falling back to its own player. Play and pause are sent as play and pause rather than a toggle, so a press meant to start a player can't pause one playing unheard behind another input. A phone casting to a device that Music Assistant wraps is controlled through the device itself.
- **Settings rows light up when tapped:** the moment a tap on the settings page or a list of choices registers, its row lights up. Some choices take a second or two to load or store, and before this nothing on screen showed that the tap had been taken.
- **More characters:** the fonts now cover all of Latin Extended-A plus Romanian's ș and ț, so names in central European, Romanian, Turkish and Baltic languages show in full. For example, "Andrei Ioniță" lost two of its letters before.
- **Sign-in QR code:** drawn with a thin white border rather than a wide one. The remote's address goes directly under the code, where it fits without being cut short, with the Home Assistant instance's name below it.
- **Linking to Home Assistant again:** the remote replaces only the token it made itself and names the new one with the first free name. Before, linking again failed when both names it tried were taken. When two remotes shared a hostname, linking one could also delete the other's token.

### Fixed

- The cover art cache had not been used since before v1.0.0, so every repaint fetched and decoded the cover again. The art still showed, only slowly.
- A fling on a list kept no momentum when the list was slow to redraw.
- The remote could restart while linking to Home Assistant, or during a fling on a list, when its main task ran out of stack.
- After linking on the device, the portal showed the Music Assistant entry as blank, and saving the page then sent it blank.
- In the portal, the player dropdowns now update when a player is renamed in Home Assistant. A stored player that is no longer listed is shown as itself and marked "(not listed now)".
- In the portal, after a save that changed only the media player, the page now shows the new player. Before, it showed the old one, so the save looked as if it hadn't taken.

## [1.1.0] - 2026-10-01

The remote can now be set up and adjusted on the device itself, without a phone or a browser: choose the Wi-Fi network, link to Home Assistant with a QR code, choose the media player, and change the everyday settings from a new settings page.

### Added

- **Settings on the device:** a gear on the status page opens a settings page for everything that needs no typing: the Wi-Fi network, the media player, the volume and power device, the player's input on it, the player volume at switch-on, Wi-Fi transmit power, the search keyboard and the screen rotation, and a restart. Tap a row for its choices, tap one to store it, and the portal shows the new value the next time it is opened.
- **Choose Wi-Fi on the device:** tap the Setup or No Wi-Fi card, or swipe left on the status page, for a list of the networks in range, and type the password on a phone-style keyboard. A new network is only kept once it connects, so a wrong password never costs the one that worked.
- **Link to Home Assistant with a QR code:** once on Wi-Fi, the remote finds Home Assistant on the network and shows a QR code. Scan it, sign in, and the remote makes its own long-lived token. There's no URL to type and no token to paste. Linking again replaces the token rather than adding another.
- **Choose the player on the device:** with Home Assistant linked and no player chosen, the remote lists the media players to pick from, with unavailable ones greyed.
- **Back button:** the status page, the settings and the Wi-Fi pages have a back button at the top left, which does what a swipe right does.

### Changed

- **The amplifier is switched on only for playback you start on the remote:** a press of play that starts playback, or something picked from the list or the search. A tap that wakes the screen no longer switches it on, nor does the player starting by itself, so casting from a phone to a player that several rooms share no longer switches on every room's amplifier.
- **Player volume at switch-on** (formerly *Player volume on wake*) is set only when the remote switches the amplifier on, so a cast from a phone keeps the volume it was cast at. A level already saved carries over.
- **Status page:** the received signal and transmit power share one line, to make room for the gear.

### Fixed

- An app that names itself but not what it is playing, such as a Roku channel, is now titled with the app's name instead of "Nothing playing".
- Reloading or going back to the portal's save page no longer clears settings: a save that does not come from the form is ignored, and the browser is sent back to the form.
- Spaces and line breaks pasted around a URL, token or entity id are trimmed when saved, and settings already stored with them are repaired at startup.
- The portal's input list for the volume device is fetched again when it would otherwise show the stored input as "not listed now".
- **Qualia:** a USB upload no longer also writes the TinyUF2 bootloader into the second app slot, where it could later be booted instead of the remote's firmware.

## [1.0.0] - 2026-09-28

The first release: a touch remote for one Home Assistant `media_player`, running on an ESP32-S3 with a small display. It talks to Home Assistant directly over its WebSocket and REST APIs, so there's nothing to install on the server.

### Added

- **Boards:** the Waveshare ESP32-S3-Touch-LCD-1.28, 240×240 round (`waveshare-s3`); the DFRobot DFR1221, 360×360 round (`round360-s3`); and the Adafruit Qualia ESP32-S3 with the 4" 720×720 square panel (`qualia-720`).
- **Now playing:** full-screen cover art with the title and artist, previous / play-pause / next, a volume swipe and the elapsed time.
- **Separate volume and power device:** an amplifier or receiver is switched on, off and to the player's input when needed.
- **With Music Assistant:** a swipe-up list of the room's recent artists and recommendations, and an artist search that starts the artist's radio.
- **Status page:** swipe down to see the device's name, its player, IP address, uptime and Wi-Fi link.
- **Blanking:** the screen blanks after five minutes idle and switches the volume device off with it.
- **Web portal setup:** Wi-Fi, Home Assistant URL and token, player, volume device, rotation, transmit power and more.
- **Network updates:** two app slots, and settings are kept across updates.

[Unreleased]: https://github.com/pvanbaren/ha-media-remote/compare/v1.5.0...HEAD
[1.5.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/pvanbaren/ha-media-remote/releases/tag/v1.0.0
