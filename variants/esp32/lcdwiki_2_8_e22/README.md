# lcdwiki_2_8_e22

Meshtastic for a hand-wired LCDWIKI 2.8" ESP32-32E (CYD-family, ESP32-2432S028R
pinout) with a **Seeed Wio-SX1262** LoRa module.

`variant.h` is the source of truth and documents the reasoning behind each pin
choice. This file is the summary.

| | |
|---|---|
| MCU | ESP32-D0WD-V3, 4 MB flash, **no PSRAM**, 320 KB RAM |
| Display | ILI9341, 240×320, portrait by default |
| Touch | XPT2046, resistive, **dedicated pins** (not the LCD bus) |
| Radio | Seeed Wio-SX1262 — 862–930 MHz, +22 dBm, TCXO, IPEX antenna |

## Pinout

**Display — ILI9341, HSPI**

```
MISO 12   MOSI 13   SCLK 14   CS 15   DC 2   BL 21 (PWM)
RST  -> not wired; panel reset is tied to EN on this board
```

**Touch — XPT2046, its own bus**

```
CLK 25   MOSI 32   MISO 39   CS 33   IRQ 36
```

Touch shares nothing with the LCD. This is why `LORA_RESET` and `LORA_DIO1` had
to move off 25 and 39 — those already belong to touch.

**LoRa — Wio-SX1262, VSPI**

```
SCK 18   MISO 19   MOSI 23   NSS 27   NRST 22   DIO1 35   BUSY 34
RF_SW 4        <- the module silkscreens this; it is RXEN
```

`BUSY` is mandatory — RadioLib polls it before and after every SPI command, and
GPIO34 is input-only, which is fine since it is only ever read.

The antenna switch is split. TX runs from the SX1262's own DIO2 line, internal
to the module, enabled by the `SX126X_DIO2_AS_RF_SWITCH` chip setting rather
than any wire — so no DIO2 pad exists and none is needed. RX is the `RF_SW` pad
and does cost an MCU pin. Do not remove `SX126X_DIO2_AS_RF_SWITCH` on the
assumption it refers to a pin; that would leave TX unswitched and off the
antenna.

**Other**

```
Button 0 (active low, pull-up)      GPS RX 3 @ 9600 (shares UART0)
```

I2C is deliberately undefined: `I2C_SDA` would also claim GPIO21, which is the
LCD backlight, and whichever initialised second would break the other.

## Screen orientation

Orientation is a runtime setting, not a rebuild:

```bash
meshtastic --port COM3 --set display.flip_screen true    # landscape
```

Also a toggle in the phone app under Display. `DisplayConfig` has no rotation
field and `flipScreenVertically()` is a no-op for every TFT except T_WATCH_S3,
so `flip_screen` is unused on this board and carries this instead — the name
says "flip" while it rotates 90°.

Takes effect on reboot: `linePixelBuffer` and `repaintChunkBuffer` are sized
from `displayWidth` at init, so the width cannot change under a running UI.
Touch calibration is stored per orientation, so flipping back and forth reuses
whichever calibration was already done rather than forcing a fresh 4-point tap.

`LCDWIKI_PORTRAIT` in `variant.h` sets only the default baked into the image.

## Build

```bash
pio run -e lcdwiki_2_8_e22
```

`platformio.ini` pins `tool-esptoolpy` to the vendored `tools/esptool-5.3.0`
via `symlink://`. That is deliberate — the header comment in `platformio.ini`
explains the failure mode it avoids (competing platform versions clobbering the
shared unversioned package directory, surfacing only at the very end of a full
build).

Flash a built image:

```bash
esptool --chip esp32 --port COM3 --baud 460800 write-flash 0x0 .pio/build/lcdwiki_2_8_e22/firmware-*.factory.bin
```

`factory.bin` writes from `0x0` and therefore erases NVS at `0x9000` — it wipes
saved settings and touch calibration. To update firmware while keeping them,
flash the app-only image at `0x10000` instead.

## First boot

1. **Set the LoRa region.** None is compiled in, and Meshtastic will not
   transmit until one is chosen (`--set lora.region US`). Until then the node
   boots, shows a UI, and stays silent — which looks like a radio fault.
2. Run the 4-point touch calibration when prompted. Hold the user button at
   boot to redo it.

## Flash budget

4 MB, no OTA. `partitions.csv` gives app0 `0x2E0000` (3,014,656 B); the current
image is ~1.81 MB.

Excluded on top of the `esp32_base` defaults: MQTT, Store-and-Forward, and the
telemetry sensor subsystems. **AdminModule is deliberately kept** — it is the
only handler for `ADMIN_APP` packets, which is how the phone app and CLI write
every config change including purely local ones over BLE. Excluded, the node
silently drops them with no error and the app just redisplays the old value.

## Known issues

- **GPS is off unless enabled in config.** The firmware defaults GPS to GPIO3
  but never initialises it while `config.position.gps_mode` is disabled.
- **GPS on GPIO3 disables the serial CLI.** GPIO3 is UART0 RX. The
  `GPS_SHARES_UART0` path splits the UARTs: debug logging still works on TX, but
  nothing can be sent *to* the receiver, so it must already emit NMEA at 9600
  and `meshtastic --port COMx` stops working. Moving the GPS pins to e.g. 16/17
  in the app restores the console with no reflash.
- **`board_upload.maximum_size` (3,088,384) overstates the app partition**
  (3,014,656) by 72 KB, and PlatformIO reports its percentage against a third
  figure again. Harmless at current usage, but the three numbers should be
  reconciled.
- **`SX126x standby ... assert failed` at `SX126xInterface.cpp:331`** with
  RadioLib `-705` (SPI timeout) or `-707` (SPI command failed) points at the
  SX1262 SPI or BUSY lines, usually a bad joint rather than firmware. This is
  what killed the original E22 on this board.
