# CrossPoint Reader (`x3fast`)

Xteink X3 performance fork of CrossPoint Reader 2.0.0 for UC8279 and UC8253 panels. Paired with [Newarr/freeink-sdk](https://github.com/Newarr/freeink-sdk). Upstream `develop` stays untouched for syncing.

- Latest release: [v2.0.0-x3fast7](https://github.com/Newarr/crosspoint-reader/releases/tag/v2.0.0-x3fast7)
- Full benchmark report: [docs/x3fast-benchmark.md](docs/x3fast-benchmark.md)
- Companion hardware SDK: [Newarr/freeink-sdk](https://github.com/Newarr/freeink-sdk)

## Measured performance on Xteink X3

Both builds ran the same 5 chapter EPUB (`bench_x3.epub`) on the same Xteink X3 (UC8279, 528x792) with `esp_timer_get_time()` hooks from input to refresh end.

![Xteink X3 Latency Chart](./docs/images/x3fast-latency-chart.png)

| Metric | Stock 2.0.0 | `x3fast` | Improvement |
|---|---:|---:|---:|
| Normal page turn (median) | 566 ms | 286 ms | **1.97x faster** |
| Rapid 4 tap skim (150 ms apart) | 1,195 ms (2 of 4 moved) | 933 ms (4 of 4 moved) | **1.28x faster, 0 dropped taps** |
| Chapter boundary turn (median) | 905 ms | 434 ms | **2.08x faster (0 ms parse)** |
| Cold JPEG to B/W on glass | 1,790 ms | 553 ms | **3.23x faster** |
| Cold JPEG full 4 gray settled | 2,581 ms | 1,345 ms | **1.92x faster** |
| Cold PNG to B/W on glass | 2,226 ms | 562 ms | **3.96x faster** |
| Cold PNG full 4 gray settled | 3,024 ms | 1,367 ms | **2.21x faster** |
| Cold GIF to B/W on glass | 1,367 ms | 683 ms | **2.00x faster** |
| Cold GIF full settled | 1,740 ms | 1,207 ms | **1.44x faster** |
| Warm cached image (median) | 738 ms | 560 ms | **1.32x faster** |
| Skip during cold JPEG load | 3,343 ms | 400 ms | **8.37x faster** |
| Skip during cold PNG load | 3,762 ms | 394 ms | **9.54x faster** |
| Skip during cold GIF load | 2,469 ms | 575 ms | **4.29x faster** |
| Standby wake (`Quick Resume` / `Blank`) | ~2,000 ms wall (1,428 ms app) | ~400 ms | **~5x faster** |
| Standby wake (other sleep screens) | ~2,000 ms wall (1,428 ms app) | ~900 ms (turn 1 at 286 ms) | **~2.2x faster** |
| Idle free DRAM (largest block) | 134,552 B (114,676 B) | 131,272 B (114,676 B) | **Same contiguous headroom** |

## Image dithering: Bayer grid vs blue noise

`x3fast` replaces the 4x4 ordered Bayer grid with a 64x64 blue noise threshold matrix for 1 bit B/W image rendering. Gradients stay smooth without staircase banding.

![Dithering Comparison](./docs/images/x3fast-dither-comparison.png)

## Parameters improved

### Display and waveforms ([Newarr/freeink-sdk](https://github.com/Newarr/freeink-sdk))

| Parameter | Stock 2.0.0 | `x3fast` | Effect |
|---|---|---|---|
| UC8279 display SPI clock | `10 MHz` | `20 MHz` | Cuts full plane transfer time in half |
| UC8279 fast DU LUT (`kUc8279X3_BwDu`) | `26` transition frames | `14` transition frames (200 Hz) | Drops normal page turns to ~286 ms |
| UC8253 fast LUT (`_fast`) | `18` transition frames | `6` transition frames (80 Hz) | Cuts UC8253 differential refresh time |
| LUT bank upload | Sent on every refresh | Cached in `_loadedBank` | Skips redundant SPI writes |
| Next page frame staging | None | Interruptible background `DTM2` upload | Fires refresh immediately on button press |
| Idle panel power rail | Powered while awake | `POF` after `30 s` (`EPD_IDLE_POF_MS`) | Lowers idle draw without losing `DTM1` |
| Standby wake panel sync | 2 boot full clears | Fast DU on `Quick Resume` / `Blank`, 1 `BwGc` on others | Removes ghosting and keeps turn 1 at 286 ms |

### Input and turn pipeline

| Parameter | Stock 2.0.0 | `x3fast` | Effect |
|---|---|---|---|
| X3 button sampling | Main loop polling | `5 ms` async FreeRTOS sampler (`beginAsync(5)`) | Captures short taps during renders |
| Button debounce | Main loop ticks | `5 ms` (`DEBOUNCE_DELAY = 5`) | Commits clicks in 2 ticks |
| Idle wake poll slice | `50 ms` sleep | `5 ms` slices (`3 ms` active contact) | Cuts idle wake input lag |
| Rapid tap handling | Drops taps during refresh | Turn netting and skim mode | Lands 4 of 4 taps instead of 2 of 4 |

### Reader, images, and fonts

| Parameter | Stock 2.0.0 | `x3fast` | Effect |
|---|---|---|---|
| Next chapter layout | Blocking parse on turn | Background prebuild (`2` pages/tick, >= `32 KB` heap) | Cuts chapter turns from 905 ms to 434 ms |
| Cold image display | Blocks until gray pass finishes | B/W first paint + `250 ms` deferred 4 gray upgrade | Shows images 2x to 4x sooner |
| Image decode abort | Uninterruptible | Cancellable JPEG, PNG, and GIF loops | Cuts skip latency by 4.3x to 9.5x |
| 1 bit image dithering | 4x4 ordered Bayer | 64x64 blue noise threshold matrix | Removes grid banding on photos |
| Font glyph cache | SD reads on misses | `128 KB` flash blob cache + `48` entry (`4 KB`) ring + `768` advance cache | Keeps page layout fast without extra DRAM |
| Reading progress writes | Immediate write per turn | `1.5 s` debounce (`15 s` max), A/B atomic files | Cuts SD wear and turn jitter |
| Truncated read guard | Unguarded `resize(len)` | `64 KB` max length, zero init soft fail | Prevents `bad_alloc` crashes on SD faults |

### Power, standby, and storage

| Parameter | Stock 2.0.0 | `x3fast` | Effect |
|---|---|---|---|
| Short sleep mode (X3) | Deep sleep + cold reboot | ESP32 C3 light sleep standby (`1 hour` window) | Wakes from GPIO edge in 15 ms |
| Display `BUSY` wait | Busy poll | ESP32 C3 light sleep slice hook | Saves CPU power during every refresh |
| X3 battery gauge poll | Frequent I2C reads | `30 s` cached poll, rejects `>100%` reads | Cuts I2C overhead and filters bad spikes |
| SD removal in standby | Stale handle crash on wake | Physical SPI `CMD58` probe + automatic `reinit()` | Recovers cleanly when card leaves slot |
| BLE transport buffers (C3) | Default NimBLE pool | `8` events (`4` discardable), controller in flash | Preserves C3 DRAM headroom |

## Flash on Xteink X3

1. Download `crosspoint-2.0.0-x3fast7.bin` from [Releases](https://github.com/Newarr/crosspoint-reader/releases).
2. Copy the file to the root of your X3 SD card.
3. Open CrossPoint on your X3 and flash the file through the SD firmware updater.

## Build from source

1. Clone this repository with its SDK submodule:

```bash
git clone --recurse-submodules https://github.com/Newarr/crosspoint-reader.git
cd crosspoint-reader
```

2. Build the X3 target with PlatformIO:

```bash
pio run -e x3_fast
```

The built binary sits at `.pio/build/x3_fast/firmware.bin`.

---

# CrossPoint Reader (Upstream)

[![Fund contributors](https://img.shields.io/badge/%F0%9F%91%91_Fund_contributors-royalty.dev-BB953A?style=for-the-badge&labelColor=1a1a1a)](https://app.royalty.dev/crosspoint-reader/crosspoint-reader)

CrossPoint is open-source e-reader firmware - community-built, fully hackable, free forever. It's maintained by a growing community of developers and readers who believe your device should do what you want - not what a manufacturer decided for you.

### Now running on:

- **ESP32C3-based** Xteink X4 and X3.
- **ESP32S3-based** Xteink X4Pro and X4Classic, Seeed reTerminal Sticky, M5PaperMono, Metalio E-Ink 4, EEGOREAD A4 and Boox Picco.

Check [our Devices page](https://crosspointreader.com/devices) for the full list.

![CrossPoint Reader running on Xteink device](./docs/images/cover.jpg)

> If you're planning to buy an Xteink device, consider purchasing an **X3/X4 Developer Edition** through https://crosspointreader.com. CrossPoint receives a small share of each sale, helping fund development costs.

## What can CrossPoint do?

- **Reader engine**: EPUB 2/3 rendering with embedded-style option, image handling, hyphenation, kerning, adaptive table layouts, native CJK ruby annotations, chapter navigation, footnotes, bookmarks, dictionary lookups ([StarDict](docs/dictionary.md)), go-to-percent, auto page turn, orientation control, focus reading, KOReader progress sync and more.

- **Various formats**: native handling for `.epub`, `.xtc/.xtch`, `.txt`, and `.bmp`.

- **Touch reading**: follow EPUB links and look up words in the dictionary on touch-enabled devices.

- **Highlights and clippings**: highlight text in EPUBs and save excerpts as clippings. Use the reader menu on button devices, or hold a word and drag the selection handles on touch devices. Look up selected words or phrases in the dictionary. Enable Sync Clippings to sync saved excerpts through a compatible CrossPoint Sync server. See [Clippings and Highlights](USER_GUIDE.md#53-clippings-and-highlights).

- **Screenshots.**

- **Custom fonts**: install your favorite fonts on the SD card.

- **Tilt page turn (X3 and Sticky)**.

- **Bluetooth page turners**: pair a remote under Settings → Connectivity → Bluetooth Page Turner. Map short and long presses to page turns, chapter navigation, the reader menu, or clipping selection.

- **USB Drive mode (X4Pro)**: access the SD card as USB mass storage.

- **Library workflow**: indexed title/author search, recently-added and alphabetical views, multilingual grouping, folder browser, recent books, and SD-cache management.

- **Wireless workflows**:
  
  - File transfer web UI
  - EPUB Optimizer
  - Web settings UI/API (edit many device settings from browser)
  - WebSocket fast uploads
  - WebDAV handler
  - AP mode (hotspot) and STA mode (join existing Wi-Fi), both with QR helpers
  - Calibre wireless connect flow
  - OPDS browser with saved servers (up to 8), search, pagination, and direct download; hold Back for one second and release in a catalog to return to the main menu
  - OTA update checks and installs from GitHub releases

- **Customization**: night mode, multiple themes (Classic, Lyra, Lyra Extended, RoundedRaff), sleep screen modes including transparent overlays, front/side button remapping, status bar controls, power-button behavior, refresh cadence, and more.

- **Localization**: 34 UI languages and counting, including CJK font fallback and RTL support.

- **SD card plugins**: install plugins for book catalogs, downloads, and tools in the device's web interface. Enable Plugins under Settings → Connectivity to show them on Home. On-device plugins use existing firmware screens and operations. JavaScript runs in a browser on your phone or computer, and its page must stay open while it works. Plugins follow [CrossPoint's reading-focused scope](SCOPE.md#out-of-scope). See the [plugin guide](docs/sd-plugins.md).

### Coming soon:

- More themes.

- Much more! stay tuned.

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with USB flashing locked from the factory.
If your device is locked, you will need to use the **Xteink Unlocker** tool available at
https://crosspointreader.com/#unlock-tool before you can flash CrossPoint.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

> ### ⚠️ WARNING: READ THIS BEFORE USING THE UNLOCKER ⚠️
> 
> **The only officially supported firmwares in the unlock tool are CrossPoint and CrossInk.**
> 
> Flashing any other firmware on a USB-locked device may **permanently brick the device** or leave it **permanently
> stuck on that firmware with no recovery path**. Once USB flashing is re-locked, your only way back is via OTA, and if
> the firmware you flashed doesn't support OTA, **there is no way out**.

## Install firmware

### Web installer (recommended)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Go to https://crosspointreader.com/#flash-tools, select your device (X3, X4, Xteink X4Pro, Seeed reTerminal Sticky, or M5PaperMono), and choose an official CrossPoint release.

### Web installer (specific version)

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Download the firmware file for your device from [Releases](https://github.com/crosspoint-reader/crosspoint-reader/releases), or compile yourself.
3. Go to https://crosspointreader.com/#flash-tools, select your device, click "Custom .bin" and upload the firmware file.

### Revert to Official Firmware

To revert to the official firmware, you can also flash the latest official firmware using https://crosspointreader.com/#flash-tools.

### Command line

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download the firmware file for your device from the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash an X3 or X4:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

   Flash an Xteink X4Pro, Seeed reTerminal Sticky, or M5PaperMono:

```bash
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

On devices with external RAM enabled in CrossPoint, copy `.ttf`, `.otf`, or `.ttc` files to the SD card and select them as reader fonts. Put one file in `/fonts/` or `/.fonts/`, or put one family's files in a subfolder. See the [SD card font guide](./docs/sd-card-fonts.md) for the folder layout and styles.

On other devices, convert the font to `.cpfont` first. `.cpfont` files also work on devices with external RAM enabled and have better performance. No firmware reflash is needed to add fonts.

To make `.cpfont` files:

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md) - how to build new screens on the FreeInkUI activity bases (UiListActivity and friends), plus build envs for the non-Xteink touch devices

---

## Development quick start

### Prerequisites

- [pioarduino PlatformIO Core](https://github.com/pioarduino/platformio-core) or [VS Code + pioarduino IDE](https://github.com/pioarduino/pioarduino-vscode-ide)
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/crosspoint-reader/crosspoint-reader
cd crosspoint-reader

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the [contributing docs](./docs/contributing/README.md). For things to work on, check the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas) — leave a comment before starting so we don't duplicate effort.

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

## Community forks

One of the best things about open source is that anyone can take the code in a different direction. If you need something outside CrossPoint's [scope](./SCOPE.md), check out the community forks:

- [CrossInk](https://github.com/uxjulia/CrossInk) — UX focused with minimal reading stats and broader customizations for the reading experience.

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) — Adds FB2 and MD format support. Actively maintained with Arabic script support. Custom themes.

- [inx](https://github.com/obijuankenobiii/inx) — Completely reimagines the user interface with tabbed navigation.

- [Witch(hunt) Reader](https://github.com/jpirnay/witchhunt-reader) — More faithful CSS styling and background work for slightly snappier interaction. Weather information panel. Markdown support.

**Note:** Many of these features will make their way into CrossPoint over time. Each project chooses its own priorities and tradeoffs.

Want to build your own device? Be sure to check out the [Silkscreen](https://silkscreenreader.com) project or [OnePage Reader](https://github.com/MoveCall/onepage-reader).

---

CrossPoint Reader is **not affiliated with Xteink or any device manufacturer**.
