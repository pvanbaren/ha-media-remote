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

- **Sources:** a swipe right on the lower half of now playing lists the volume device's inputs, the current one marked. Tap one to switch to it, switching the device on first if it is off, and go straight back to now playing. When the player's input on the device is set to a particular input, it changes to the new one too; when it follows the input named after the player, it is left alone.
- **Back button on the search:** at the top left, beside the query, as on the other pages. It does what a swipe right does: from the results back to the keyboard, and from the keyboard back to the list or now playing. The results start a little lower to leave it room.
- **Search from now playing:** a swipe left on the lower half of now playing opens the artist search directly, without going through the list. A swipe right on its keyboard goes back to now playing. A sideways swipe on the top half still sets the volume.
- **Makerfabs MaTouch ESP32-S3 Rotary 2.1":** a 2.1" round 480 px ST7701 panel with CST826 touch, built as `matouch21-s3`. Its knob turns the volume on now playing -- switching the volume device on first, as play does, when it is off -- and scrolls the lists, a row a click; a press plays or pauses on now playing, goes back one step elsewhere, and taps a card. The USB-C port is the S3's own USB, so uploads and the serial console need no driver.
- **Back on Wi-Fi after a router outage:** a remote that started while its router was down -- after a power cut, say -- opened the setup portal and stayed there. It now tries the saved network again every minute while nobody is using the portal, and carries on by itself once the router is back.
- **Link again from a rejected token:** when Home Assistant refuses the remote's token -- deleted under your profile, say -- the card says **Token rejected** and **Tap to link again**, and a tap puts the sign-in QR code back up, keeping the URL. Until then the remote stops asking Home Assistant for anything, trying the token again only every ten minutes, so it does not fill Home Assistant's log with failed sign-ins or get its address banned.
- **Status and settings from a card:** on the cards that say something is wrong -- Home Assistant not reachable, Token rejected, No Wi-Fi, Link to HA -- a swipe down opens the status page, to see what, and a gear at the top right opens the settings, to change the network or the transmit power, or restart. The sign-in QR code has the swipe down, and the player list shown before a player is chosen has the gear. Going back lands on the card again.
- **Offline mark:** when Home Assistant has not been heard from for 10 s, the elapsed time on now playing turns amber and says **Offline**, or **Token rejected**, instead of running on from a state that may be stale. The status page says the same on an **HA** line, with the error. Both go when Home Assistant answers again.

### Changed

- **A device playing outside Music Assistant is controlled directly:** where the player is a Music Assistant entity that is idle while the native entity of the same name plays or pauses something of its own, such as a TV in Disney Plus, now playing shows the native entity's title, art and state, and play, pause, next and previous go to it.
- **Linked in the portal, on to the player list:** saving the URL and token in the portal while the Link to HA card or the QR code was up left the card there until it was tapped. The remote now moves on to the player list by itself, as a link made on the device does.
- **Losing Wi-Fi while running:** when the network dropped and could not be rejoined, the Connecting card stayed frozen over the screen, taps still went to what was underneath, and attempt followed blocking attempt. The No Wi-Fi card now replaces it (a tap chooses a network), the remote keeps trying in the background without blocking, and it goes back to now playing when the link returns.
- **Status page:** the gear is at the top right, across the title row from the back button, and the received signal and transmit power have a line each again. A swipe up goes back to the screen it was opened from -- a card, say, or the settings -- where the other gestures still go to now playing.
- **Sharper covers on the 2.8C:** it asks for 512 px covers instead of 256 px, so the backdrop is no longer scaled up. A new track's cover takes about 0.16 s longer to appear.
- **Volume with the volume device off:** a sideways swipe across the top of now playing switches the device on first, as play does -- its input and the player volume at switch-on with it -- and the next swipe sets the level. Before, the swipe went to a device that was off, so nothing could be heard. A device that cannot be switched on from Home Assistant takes the level as before.
- **List rows at the edge of a round screen:** a row scrolling past the top or bottom keeps half the panel's width, its corners cut by the bezel, instead of pinching down to a sliver as it goes. On every list: the music list, the search results, the settings and sources, the networks and the players.
- **Settings before linking:** with the remote not yet linked to Home Assistant, the settings page leaves out Media player and Volume & power, which are chosen from Home Assistant's lists.

### Fixed

- After choosing a media player or a Volume & power device on the device, the settings page and the status page showed the old one until they were opened again.

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

[Unreleased]: https://github.com/pvanbaren/ha-media-remote/compare/v1.2.0...HEAD
[1.2.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.1.0...v1.2.0
[1.1.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/pvanbaren/ha-media-remote/releases/tag/v1.0.0
