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

## Setup

On first boot the device brings up a captive portal on AP
**`MediaRemote-Setup`** (192.168.4.1) to collect Wi-Fi credentials. Once it is
on the network the same portal stays reachable at
`http://media-remote.local`.

Holding **BOOT** for 3 seconds clears the saved settings and reboots into the
portal. Touch is the primary input, so BOOT has no short-press action.
