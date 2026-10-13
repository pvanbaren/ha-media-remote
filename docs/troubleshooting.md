# Troubleshooting

Part of the [Home Assistant Media Remote](../README.md) documentation.

**A weak or dropping Wi-Fi link** is easiest to judge where the remote lives:
open `http://<name>.local/link`. It shows the signal the device hears and the
power it transmits with -- a link can be strong one way and weak the other,
which shows as lost pings and stalled updates while the signal looks fine --
and **Scan for access points** lists every access point in range, strongest
first, with the one it joined marked. The device scans every channel before
joining and takes the strongest access point with the network's name, rather
than the first one it finds. The [status page](../README.md#the-status-page) shows the
same two numbers, Rx and Tx, on the remote itself.

The status card has room for one line, so the serial log is the place to
look:

```bash
pio device monitor
```

Each line is milliseconds since boot, a level letter -- **E**rror, **W**arn,
**I**nfo, **D**ebug, **V**erbose -- and the message. How much is printed is
fixed at build time by `config::kLogLevel`, `kInfo` by default; everything
below it is compiled out. Set it to `kDebug` to see every HTTP request and
every state change as well:

```
   18342 I HA: stream open, following media_player.living_room
   18503 D HTTP POST https://ha.example:8123/api/template -> 200 (tx 412, rx 318 bytes, 143 ms)
   19120 D HTTP GET http://homeassistant.local:8095/imageproxy/…?size=256&fmt=jpg -> 200 (tx 0, rx 28382 bytes, 94 ms)
```

A failed request logs at warn, so it shows at the default level, with the
full request and response bodies beside it.

The stream says what it is doing:

```
HA: stream connecting to homeassistant.local:8123/api/websocket
HA: stream open, following media_player.living_room
HA: stream closed, nothing heard
HA: stream closed soon after opening, polling; next try in 5 s
```

| Symptom | Look at |
|---|---|
| `HTTP 400` | The response body names the template or JSON problem |
| `token rejected` | Tap the card to link again, or re-paste the token -- Home Assistant's, or Music Assistant's for `MA:` lines |
| `HTTP 404` | A trailing path on the base URL, or an entity that no longer exists |
| `connection refused` / timeouts | The URL's host and port, or https against a plain-http server |
| `transport failure (heap free …, largest …)` | A TLS handshake that could not allocate; something new is using internal RAM |
| Player missing from the portal | `list hit the N entry cap` -- raise `kMaxPlayers` |
| The list says **Not set up** | No Music Assistant config entry id, and no Music Assistant token |
| The list says **Music Assistant** and an error | Every section failed; the console has the body |
| `no items in library response` | Check the config entry id |
| Recommended is random albums | No Music Assistant token, or the pool is not built yet; watch for `Recommend:` lines |
| `History: no artist found for "…"` | That name matched no artist exactly; it is not searched for again |
| `Portal: … has no input "…", cleared` | The new volume device has no input by the old name; choose one |
| `Cover art: … streaming instead` | The cover is larger than the board's buffer; it re-downloads on every repaint |
| `Cover art: decode failed` | Size, dimensions, format and scale follow; progressive JPEG is not supported |
| `Art: … refused, retrying unresized` | That host rejected the thumbnail size; harmless |
| `assert failed: pbuf_free … p->ref > 0` | A client destroyed before the `HTTPClient` using it; declare clients first |
| `UI: list tap ignored` | A tap on a moving list, discarded on purpose |
| Volume control missing | The volume device reports no `volume_level`, or is off |
| Taps land mirrored or transposed | `kTouchSwapXy` / `kTouchInvertX` / `kTouchInvertY` in the board header |
| `Partition Could Not be Found` on Update | The device has the old one-slot table; flash it once over USB |
| `ModuleNotFoundError: No module named 'esptool'` | A partial PlatformIO tool install: delete `~/.platformio/packages/tool-esptoolpy` and rebuild |
