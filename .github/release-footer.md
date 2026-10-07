### Updating

- **A remote already set up:** upload your board's `firmware-<board>.bin` on its portal's **Update** page. Settings are kept. `<board>` is `waveshare-s3`, `round360-s3`, `lcd28c-s3` or `qualia-720`.
- **Install from the browser:** [pvanbaren.github.io/ha-media-remote](https://pvanbaren.github.io/ha-media-remote/) flashes a board plugged in over USB, using Chrome or Edge on a computer. It erases the settings, so use it for a new board.
- **First install over USB:** `pio run -e <board> -t upload`
- **Blank board:** `firmware-<board>-merged.bin` flashes it from address 0 with a web flasher. It erases the settings.

See the [README](https://github.com/pvanbaren/ha-media-remote#readme) for setup, and the [changelog](https://github.com/pvanbaren/ha-media-remote/blob/main/CHANGELOG.md) for every release.
