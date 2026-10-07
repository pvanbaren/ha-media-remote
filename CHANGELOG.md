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

[Unreleased]: https://github.com/pvanbaren/ha-media-remote/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/pvanbaren/ha-media-remote/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/pvanbaren/ha-media-remote/releases/tag/v1.0.0
