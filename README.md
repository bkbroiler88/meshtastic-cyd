# Meshtastic for the CYD 2.8" + Ebyte E22

Board support for running [Meshtastic](https://meshtastic.org) on a hand-wired
**CYD 2.8"** (LCDWIKI / QDTech ESP32-32E, ESP32-2432S028R pinout) with an
**Ebyte E22-900M30S** LoRa module — touchscreen UI, an offline vector map with
street names, and battery sensing.

> **This is not official Meshtastic, and not affiliated with the Meshtastic
> project.** It is a personal fork adding support for one hand-wired board.
> Upstream firmware lives at
> [meshtastic/firmware](https://github.com/meshtastic/firmware).
>
> This branch holds only the ready-to-flash builds and the map generator. The
> **full firmware source is on the [`lcdwiki-2.8-e22`](../../tree/lcdwiki-2.8-e22)
> branch**, which is the Meshtastic tree with this board's variant added.

## Flash it

Pick the image that matches your panel controller. These boards ship with either
one under the same product name, and there is no way to detect it — the wrong
image gives a garbled, mirrored or blank screen.

| Panel | Image |
|---|---|
| ILI9341 | `bins/firmware-lcdwiki_2_8_e22-*.factory.bin` |
| ST7789 | `bins/firmware-lcdwiki_2_8_e22_st7789-*.factory.bin` |

```bash
pip install esptool
esptool --chip esp32 --port COM3 --baud 460800 write-flash \
  --flash-mode dio --flash-freq 40m --flash-size 4MB \
  0x0 bins/firmware-lcdwiki_2_8_e22_st7789-2.8.0.21d15f6.factory.bin
```

Write it at `0x0`: these are complete images including the bootloader and
partition table. To clear any previous config as well, run
`esptool --chip esp32 --port COM3 erase-flash` first.

After flashing, set your **region** (LoRa will not transmit until you do) from
the Meshtastic phone app over Bluetooth.

## Wiring

The display and touchscreen are already on the board. These are the wires you
add for the radio:

| E22-900M30S | GPIO | |
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

## Maps

The firmware ships **without** map data — you generate it for wherever you are.

```bash
python tools/make_map.py "Edgewood, MD"
python tools/make_map.py "Boulder, Colorado" --km 30 --no-local
python tools/make_map.py --lat 39.4187 --lon -76.2944 --km 24 --copy-to E:\
```

It looks up the place, downloads from OpenStreetMap and writes `basemap.bin`.
Copy that to the **root of a microSD card** and put the card in the node.
`--km` sets how much area to cover; `--no-local` leaves out residential streets
for a much smaller file.

Needs Python 3. No API key, no account.

The map frame is one swipe right of the home screen. It plots the nodes you have
heard, north-up around your own position, over roads and water, and names the
streets. Swipe up and down to zoom.

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
were built from commit `21d15f6` on the
[`lcdwiki-2.8-e22`](../../tree/lcdwiki-2.8-e22) branch of this repository, which
is the corresponding source.

Map data from [OpenStreetMap](https://www.openstreetmap.org/copyright),
© OpenStreetMap contributors, ODbL.
