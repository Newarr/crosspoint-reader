# CrossPoint Reader (`x3fast`)

Xteink X3 performance fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) 2.0.0 for UC8279 and UC8253 panels. Paired with [Newarr/freeink-sdk](https://github.com/Newarr/freeink-sdk). Upstream `develop` stays untouched for syncing.

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
| Battery sleep wake (median, app timer) | 1,428 ms (~2.0 s cold boot wall time) | 1,110 ms (15 ms GPIO + panel wake) | **1.29x faster app timer** |
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

## Upstream and license

Based on [crosspoint-reader/crosspoint-reader](https://github.com/crosspoint-reader/crosspoint-reader) (MIT [LICENSE](LICENSE)). See [USER_GUIDE.md](USER_GUIDE.md) and the upstream repository for general CrossPoint features, supported boards, and flashing tools.
