# Meshtastic for the CYD 2.8" + Ebyte E22

Board support for running [Meshtastic](https://meshtastic.org) on a hand-wired
**CYD 2.8"** (LCDWIKI / QDTech ESP32-32E, ESP32-2432S028R pinout) with an
**Ebyte E22-900M30S** LoRa module — touchscreen UI, an offline vector map with
street names, and battery sensing.

**Lora board can be purchased [here](https://Witelesswizard.net)

<img width="960" height="1280" alt="IMG_1621" src="https://github.com/user-attachments/assets/5222b058-ffde-42e3-ba0d-4ac82d70dab3" />

> **This is not official Meshtastic, and not affiliated with the Meshtastic
> project.** It is a personal fork adding support for one hand-wired board.
> Upstream firmware lives at
> [meshtastic/firmware](https://github.com/meshtastic/firmware).
>
> This branch holds only the ready-to-flash builds and the map generator. The
> **full firmware source is on the [`lcdwiki-2.8.1`](../../tree/lcdwiki-2.8.1)
> branch**, which is the Meshtastic tree with these boards' variants added.

## Flash it

**Easiest: flash from your browser.** Open the [web flasher](https://bkbroiler88.github.io/meshtastic-cyd/)
in Chrome or Edge, pick your panel, and click Install. No tools to install. (Firefox and
Safari lack Web Serial, and GitHub Pages must be enabled on this repo for the link to work.)

Or use `esptool` from the command line, below.

### Versions

| Version | Built from |
|---|---|
| `2.8.1.9d8d166` | Current. CYD 2.8"/3.5" only: map packs (any number of areas in `/maps` on the SD card that join up, filled water/parks/woods, trails, railways, POIs, town/water names), zoom to 50 m, much faster map redraws. GPS works when wired receive-only. The Heltec V3 image (no map, no GPS) is a rebuild with no functional change. |
| `2.8.1.267074e` | Adds the LCDWIKI 3.5" and a Heltec V3 build, on-device Quick Settings, Node Details, traceroute on the map, a spectrum analyzer + test transmitter (LoRa Actions → Spectrum), Radio Tuning, and upstream SX126x radio fixes. |
| `2.8.1.1aae089` | Meshtastic 2.8.1 + this fork. |
| `2.8.0.21d15f6` | previous release, kept for rollback |

The ILI9341 `2.8.1` image is **built but not yet verified on hardware** - if the screen
misbehaves, try the ST7789 image, which is tested.

Pick the image that matches your panel controller. These boards ship with either
one under the same product name, and there is no way to detect it — the wrong
image gives a garbled, mirrored or blank screen.

| Panel | Image |
|---|---|
| ILI9341 | `bins/firmware-lcdwiki_2_8_e22-*.factory.bin` |
| ST7789 | `bins/firmware-lcdwiki_2_8_e22_st7789-*.factory.bin` |
| LCDWIKI 3.5" (ST7796) | `bins/firmware-lcdwiki_3_5_e22-*.factory.bin` |
| Heltec V3 (ESP32-S3) | `bins/firmware-heltec-v3-spectrum-*.factory.bin` — flash with `--chip esp32s3 --flash-size 8MB` |
| Piglet: XIAO ESP32-S3 DIY | `bins/firmware-seeed-xiao-s3-diy-*.factory.bin` — `--chip esp32s3 --flash-size 8MB`; wiring below |

```bash
pip install esptool
esptool --chip esp32 --port COM3 --baud 460800 write-flash \
  --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x0 bins/firmware-lcdwiki_2_8_e22_st7789-2.8.1.9d8d166.factory.bin
```

Write it at `0x0`: these are complete images including the bootloader and
partition table. To clear any previous config as well, run
`esptool --chip esp32 --port COM3 erase-flash` first.

After flashing, set your **region** (LoRa will not transmit until you do) from
the Meshtastic phone app over Bluetooth.

## Wiring

The display and touchscreen are already on the board. These are the wires you
add for the radio:

| E22-900M30S | GPIO-CYD | |
|---|---|---|
| SCK | 18 | shared with the microSD slot |
| MISO | 19 | shared |
| MOSI | 23 | shared |
| NSS | 27 | |
| NRST | 22 | |
| DIO1 | 35 | |
| BUSY | 17 | |
| RXEN | 4 | |
| TXEN | — | wire to the module's own **DIO2** pad, not to a GPIO |

Three of these matter more than they look:

- **BUSY on 17, not 34.** GPIO34 carries the TP4056 battery divider on this
  board. With BUSY there the radio never initialises.
- **RXEN on 4, not 26.** GPIO26 sits next to the touch clock and kills the
  touchscreen while the radio receives.
- **TXEN to DIO2, never a GPIO.** DIO2 is output-only.

Full pinout, including display and touch, is in the
[variant README](../../blob/lcdwiki-2.8-e22/variants/esp32/lcdwiki_2_8_e22/README.md).

## Piglet (XIAO ESP32-S3 DIY)

A Piglet wardriver turned into a Meshtastic node, with a pet pig on his own
screen. He gets hungry over a few hours, cheers up when the mesh is busy and
dances when a message arrives, gets fitter when you carry him (GPS distance),
sleeps at night, and plays fetch: a telemetry request or traceroute to a
nearby node, at most one every five minutes, with a twerk when it comes back.
Short press moves between screens; a long press on his screen opens his menu
(Feed, Play fetch, Fetch: trace route, Put to bed, Stats, Rename).

| Part | XIAO pin (GPIO) |
|---|---|
| Wio-SX1262 SCK / MISO / MOSI | D8 (7) / D9 (8) / D10 (9) |
| Wio-SX1262 NSS | GPIO43 (rerouted from the board's GPS-TX pad) |
| Wio-SX1262 RESET / DIO1 / BUSY | D2 (3) / D1 (2) / D3 (4) |
| 0.96" SSD1306 OLED SDA / SCL | D4 (5) / D5 (6) |
| L76K GPS TX → XIAO | D7 (44); receive-only, nothing to the GPS RX |
| Button | D0 (1) to GND |

The antenna switch is driven by the SX1262's DIO2; there is no battery sense.
The pig is inspired by the Piglet wardriver's walking pig and drawn from scratch
for this firmware. The image reports version `2.8.1.9d8d166`; its source is
commit `3f959ae` on `lcdwiki-2.8.1` (`9d8d166` plus the Piglet frame).

## Maps

The firmware ships **without** map data — you generate a map pack for wherever you are.

**Easiest: the [Map Builder](https://bkbroiler88.github.io/meshtastic-cyd/maps.html).** Find
a town or click the map, pick a size, Build, Download. It runs in the browser (Chrome,
Edge, Firefox or Safari) and makes exactly the same pack as the command-line tool.

**For a whole state or region**, use `tools/make_map_region.py`. It reads a Geofabrik
extract instead of querying Overpass, so there is no rate limit; Delaware takes about a
minute. Needs `pip install osmium`.

```bash
python tools/make_map_region.py --region us/maryland --no-local
python tools/make_map_region.py --region europe/malta --copy-to E:```

**Or from the command line**, for a town:

```bash
python tools/make_map.py "Edgewood, MD"
python tools/make_map.py "Boulder, Colorado" --km 30 --no-local
python tools/make_map.py --lat 39.4187 --lon -76.2944 --km 24 --copy-to E:```

It looks up the place, downloads from OpenStreetMap and writes a pack such as
`edgewood-md.map`. Put it in a folder called **`maps`** on the microSD card
(`--copy-to` does that for you). Add as many packs as you like: neighbouring
areas join up on the map. `--km` sets how much area to cover; `--no-local`
leaves out residential streets and trails for a much smaller file.

A pack holds roads, water, filled parks, woods and lakes, trails, railways,
points of interest and town and water names, at three detail levels that the map
picks by zoom.

Needs Python 3. No API key, no account. Firmware older than `2.8.1.9d8d166` reads
only a single `basemap.bin` on the card root; make one with `make_map.py --legacy`.

The map frame is one swipe right of the home screen. It plots the nodes you have
heard, north-up around your own position. Swipe up and down to zoom, from about
50 m across out to 200 km.

**Maps and Bluetooth cannot both be on.** The microSD card is mounted only while
Bluetooth is off (Menu → Bluetooth), because the two do not fit in this board's
memory at once. With Bluetooth on, the map frame tells you so. The node reboots
when you toggle it.

## What is left out

4 MB of flash and 320 KB of RAM with no PSRAM, so this build drops: MQTT,
store-and-forward, environment / air-quality / health / power telemetry and its
history, the detection-sensor module, and file browsing from the phone app.

Messaging, channels, position, traceroute, canned messages and the admin
interface all work normally. Details and reasoning are in the
[variant README](../../blob/lcdwiki-2.8-e22/variants/esp32/lcdwiki_2_8_e22/README.md).

## Status

Verified on hardware, ST7789 build: display, touch, radio, microSD, offline map
with street names, battery sensing, the Bluetooth/SD switch.

The **ILI9341 image compiles but has never been run.** Treat it as untested.

Known rough edges:

- The GPS only works with USB unplugged — it shares a pin with the USB serial
  chip. The `meshtastic --port` serial CLI is unavailable for the same reason;
  use Bluetooth.
- The battery percentage is calibrated from an assumed divider ratio and has not
  been checked against a meter. It can be corrected from the app without
  reflashing.

## Licence

GPL-3.0, inherited from Meshtastic — see [LICENSE](LICENSE). The binaries here
were built from commit `9d8d166` on the
[`lcdwiki-2.8.1`](../../tree/lcdwiki-2.8.1) branch of this repository, which
is the corresponding source (older images: `267074e` and `1aae089` on `lcdwiki-2.8.1`,
`21d15f6` on `lcdwiki-2.8-e22`).

Map data from [OpenStreetMap](https://www.openstreetmap.org/copyright),
© OpenStreetMap contributors, ODbL.
