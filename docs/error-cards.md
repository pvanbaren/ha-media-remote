# Error cards

Which screen the remote shows for each startup and running condition, what a
tap, a swipe down and the gear do on it, and what takes it away.

A *card* is a full-screen message: yellow for setup, black with white text for
the network, black with amber text for Home Assistant. On a board with a knob,
a press on a card counts as a tap on it.

## At startup

| # | Situation | Screen | Tap, or knob press | Swipe down | Gear | It goes away when |
|---|---|---|---|---|---|---|
| 1 | No saved Wi-Fi, or just reset with BOOT | **Setup**, yellow: "Join MediaRemote-Setup, or tap to choose Wi-Fi". The portal stays open with no time limit. | Opens the network list | No | No | A network is joined on the device or through the portal |
| 2 | Saved Wi-Fi, connecting | **Connecting**, black: the network's name, with dots running round the bezel | Nothing (the attempt blocks) | No | No | It connects (4), or fails (3) |
| 3 | Saved Wi-Fi fails: router down, wrong password | **Setup**, yellow. The portal is open, and the saved network is tried again every 60 s while nobody is using the portal or the network screens. | Opens the network list | No | No | The router comes back, or a network is joined |
| 4 | On Wi-Fi, not linked, Home Assistant not found | **Link to HA**, yellow: "Set URL and token at `<name>.local`". It looks for Home Assistant again every 30 s. | Nothing | Status page | Settings | Home Assistant is found (5), or a URL and token are saved in the portal (7) |
| 5 | Home Assistant found on the network | **Scan to link**, black: the QR code, the page's address under it, and which Home Assistant | Nothing | Status page | No: the code fills the card | Sign-in finishes (7 or 8) |
| 6 | Sign-in fails | **Home Assistant not reachable**, amber, with the error, for 3 s | Nothing (the remote waits) | No | No | Goes back to 4 or 5 |
| 7 | Linked, no player chosen | **Choose a player** list, or "Could not load players". If the token is refused and no list was loaded before: **Token rejected** (10). | Chooses a player | No: it scrolls the list | Settings, top right | A player is chosen here or in the portal (8) |
| 8 | Player chosen | **Loading**, black, with the player's name | Nothing | No | No | The first state arrives: now playing |
| 9 | The first state never arrives: Home Assistant down, wrong URL | **Home Assistant not reachable**, amber, with the error. It asks for an update every 2 s. | Nothing | Status page, with an "HA Offline (error)" line | Settings | The state arrives |
| 10 | Token refused, no state yet | **Token rejected**, amber: "Tap to link again". It stops asking Home Assistant for anything, trying the token again only every ten minutes, so Home Assistant's log does not fill with failed sign-ins. | Brings the QR code back, keeping the URL | Status page, with an "HA Token rejected" line | Settings | The remote is signed in again |

## While running

| # | Situation | Screen | Tap, or knob press | Swipe down | Gear | It goes away when |
|---|---|---|---|---|---|---|
| 11 | Wi-Fi drops | Nothing for 4 s, then one blocking attempt behind **Connecting**. If that fails: **No Wi-Fi**, black, "Signal lost, retrying, Tap to choose", and a quiet retry every 60 s. Never while the network screens are up. (A start that never connected says "Saved network failed" instead.) | Opens the network list | Status page | Settings: the network, the transmit power, a restart | Wi-Fi returns: now playing |
| 12 | Home Assistant unreachable after the remote had a state | **Now playing**, still showing the last state. After 10 s with no stream and every poll failing, the elapsed chip turns amber and says **Offline**. | Controls as usual; commands fail | Status page, with an "HA Offline (error)" line | From the status page | Home Assistant answers again: the chip goes back to the time |
| 13 | Token revoked while running | **Now playing**. After 10 s the chip says **Token rejected**, in amber. | A tap on the chip links again: the QR code, keeping the URL. Elsewhere, controls as usual; commands fail. | Status page, with an "HA Token rejected" line | From the status page | The remote is signed in again, from the chip's QR code or the portal |
| 14 | Player off, idle or unavailable | **Now playing**, idle. The screen blanks after the idle timeout and switches the volume device off. | Wakes the screen (the knob too) | Status page | From the status page | The player starts |
| 15 | Volume device (the amplifier) off | **Now playing**, the volume grey. A sideways swipe across the top half, or a turn of the knob, switches it on as play does, with its input and the player volume at switch-on; the next swipe or turn sets the level. | A tap on the top half toggles its power | Status page | From the status page | Home Assistant reports it on |
| 16 | BOOT held for 3 s (Qualia: DOWN) | **Reset**, yellow: "Settings cleared, Rebooting" | Nothing | No | No | Restarts into 1 |
| 17 | Rotation changed, or Restart chosen, in settings | **Restarting**, black | Nothing | No | No | Restarts |

## Getting back to a card

- A swipe up on the status page goes back to the screen it was opened from:
  now playing, the card, the settings or the network list.
- Settings back goes to the status page. The status page's back button, and
  any other tap or swipe there, go to now playing.
- Now playing first works out what should be up: No Wi-Fi when the network is
  lost, Link to HA or the QR code when not linked, the player list when none is
  chosen, Not reachable or Token rejected when there is no state. So going back
  from a card's status page, by swipe up or by back, lands on that card while
  the problem lasts, and on now playing once it is fixed.
- Left alone for 60 s, the status page and settings go back by themselves the
  same way.
- With Home Assistant not linked, settings leaves out Media player and Volume &
  power, since both are chosen from Home Assistant's lists.
