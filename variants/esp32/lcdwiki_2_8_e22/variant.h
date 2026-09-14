// LCDWIKI 2.8" ESP32-32E display with an Ebyte E22-900M30S LoRa module (US 915 MHz).
// SX1262 behind a 10 dB external PA, 30 dBm rated output, TCXO on DIO3.
//
// The PA is the one thing that makes this module's config differ from the bare-SX1262
// parts this board has also carried (E22-900M22S, Seeed Wio-SX1262): it needs
// -DEBYTE_E22_900M30S so Meshtastic accounts for the extra gain. Pin map and RF-switch
// wiring are identical across all three; only the TX power path changes. See the
// SX126X_MAX_POWER note below before swapping modules again.
//
// Note the Ebyte suffix: "M" is the bare-SX1262 SPI part this config expects. The "T"
// parts are UART modules with an onboard MCU and cannot be driven by SX1262Interface.
//
// A 1 W PA draws far more on TX than the 22 dBm parts. If the supply sags mid-transmit
// the symptom is an SX126x SPI assert at SX126xInterface.cpp:331 (-705 timeout / -707
// command failed), which looks identical to a bad solder joint.
//
// Display: ILI9341 or ST7789 on HSPI (12/13/14) - selected below
// Touch:   XPT2046 on the same HSPI bus, separate CS
// LoRa:    SX1262 on VSPI (18/19/23), separate CS
//
// Hardware notes:
// - No PSRAM, 320KB RAM, 4MB flash -> see platformio.ini for feature exclusions.
// - Base UI only (not MUI).
// - GPIO21 is the LCD backlight. I2C is deliberately NOT defined: I2C_SDA would
//   also claim GPIO21 and whichever initialised second would break the other.

// Button
#define BUTTON_PIN 0 // GPIO 0, pulled low to activate
#define BUTTON_NEED_PULLUP

// NOTE: HAS_WIRE 0 was tried here and caused repeated
// "Wire.cpp endTransmission(): NULL TX buffer pointer" errors, because the I2C
// scanner still runs. Left enabled; the theoretical GPIO21 clash never manifested.

// GPS defaults to GPIO3, which UART0 (the USB serial console) also owns. These are
// only DEFAULTS: config.position.rx_gpio/tx_gpio are settable from the phone app and
// the CLI and override them at runtime (GPS.cpp createGps()).
//
// GPS_SHARES_UART0 opts this board into the collision handling in GPS.cpp, which
// checks the ACTUAL resolved pins at boot:
//   - GPS on GPIO3 (or TX on GPIO1) -> split the UARTs. UART0 keeps TX on GPIO1 for
//     debug logging with its RX detached; the GPS UART takes RX with its TX detached.
//     Cost: nothing can be transmitted to the receiver, so it must already emit NMEA
//     at GPS_BAUDRATE on power-up - no autobaud, no runtime config of the module - and
//     the serial CLI (meshtastic --port COMx) stops working. Serial logging is fine.
//   - GPS moved anywhere else (e.g. 16/17) -> no split, serial console fully intact.
// So if you want the serial CLI back, just move the GPS pins in the app; no reflash.
#define GPS_RX_PIN 3
#define GPS_SHARES_UART0
#define GPS_BAUDRATE 9600

// Use UART2 rather than Meshtastic's default UART1, matching cyd-bulb-controller,
// which drives an ATGM336H on these same GPIO3/GPIO1 pins from UART2
// (src/m5_gps.cpp: HardwareSerial gpsSerial(2)). On a classic ESP32 the UART1
// default pins (GPIO9/GPIO10) belong to the SPI flash, so UART2 is the safer of
// the two to remap.
//
// Chosen while chasing a receiver that read zero bytes at every baud rate; that
// turned out to be a failed GPS module, NOT the UART. So this is "match the known
// working reference", not a fix for that symptom - UART1 may well be fine.
#define GPS_SERIAL_PORT Serial2

// Display - HSPI bus. Panel CONTROLLER is selected below; see the note there.
#define HAS_SPI_TFT 1
#define USE_TFTDISPLAY 1
// Natively 240x320 (portrait). These feed both panel_width/height AND
// memory_width/height, so they must be the NATIVE values - claiming 320 columns on
// a 240-column controller truncates the right edge. TFT_ROTATION turns it landscape.
// (m5stack_core uses 320/240 because M5Stack Core is really an ILI9342, which IS
// natively 320x240; it shares this driver block.)
#define TFT_WIDTH 240
#define TFT_HEIGHT 320
#define TFT_OFFSET_X 0
#define TFT_OFFSET_Y 0
#define TFT_BUSY -1

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1 // panel reset is tied to EN on this board
#define TFT_BL 21 // PWM backlight

#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 16000000

// PANEL CONTROLLER. Boards sold as "LCDWIKI 2.8 / ESP32-2432S028R" ship with either an
// ILI9341 or an ST7789 - identical pinout, identical XPT2046 touch, different command
// set. Check the marking on the panel or the box; they are not interchangeable.
//
// Getting this wrong is quiet and misleading. The backlight lights, the boot log is
// clean ("Do TFT init", "Touchscreen: XPT2046 bound"), and touch works normally - the
// panel has no way to report that it received the wrong command stream. What you see is
// mirrored text, the wrong orientation, and half the frame misplaced, because MADCTL and
// the column/page address windows land on registers that mean something else.
//
// Select the ST7789 board with -DLCDWIKI_PANEL_ST7789=1 (see platformio.ini); the
// default is the ILI9341.

// CYD panels ship with either colour polarity under the same product name, so which
// way round looks correct is a per-board fact rather than a per-model one. Exposes
// a Display Options entry that drives the controller's INVON/INVOFF via
// invertDisplay(), persisted in NVS. cyd-bulb-controller does the same thing
// (src/main.cpp applyInversion()).
//
// NOT config.display.displaymode INVERTED - that only styles the monochrome header
// bitmap in SharedUIDisplay and does nothing once GRAPHICS_TFT_COLORING_ENABLED is on.
#define LCDWIKI_PANEL_INVERT_TOGGLE 1

#ifndef LCDWIKI_PANEL_ST7789
#define LCDWIKI_PANEL_ST7789 0
#endif

#if LCDWIKI_PANEL_ST7789
// TFTDisplay.cpp selects this block on ST7789_CS and reads the pins through these names.
// Geometry still comes from the TFT_* macros above (the hardcoded 240x240 case there is
// gated on T_WATCH_S3, which this board is not).
#define ST7789_SPI_HOST HSPI_HOST
#define ST7789_SCK TFT_SCLK
#define ST7789_SDA TFT_MOSI
#define ST7789_MISO TFT_MISO
#define ST7789_RS TFT_DC
#define ST7789_CS TFT_CS
#define ST7789_RESET TFT_RST
#define ST7789_BUSY TFT_BUSY
#else
#define ILI9341_DRIVER
#define ILI9341_SPI_HOST HSPI_HOST
#endif

// Orientation. The two knobs the ILI9341 path actually reads:
//   TFT_OFFSET_ROTATION - panel-level offset 0~7; 4~7 mirror the X axis.
//   TFT_ROTATION        - LovyanGFX setRotation() value 0~3 (90 degree steps).
// TFT_WIDTH/HEIGHT above are the panel's NATIVE portrait size.
//
// Orientation is a RUNTIME setting on this board: config.display.flip_screen selects
// landscape, so it can be changed from the phone app or the CLI with no reflash:
//   meshtastic --port COMx --set display.flip_screen true
// LCDWIKI_PORTRAIT is only the default baked into the image; once flip_screen is set the
// config value wins. DisplayConfig has no rotation field, and flipScreenVertically() is a
// no-op for every TFT except T_WATCH_S3, so flip_screen is unused here and free to mean
// this. The trade is that the name says "flip" while it rotates 90 degrees.
//
// The change costs a reboot. TFTDisplay sizes linePixelBuffer and repaintChunkBuffer from
// displayWidth in init(), so the width cannot move underneath a running UI - but the
// OLEDDisplay framebuffer is safe either way, since displayWidth * maxDisplayHeight / 8 is
// 9600 bytes for both 240x320 and 320x240.
//
// Both halves of the orientation must agree - the panel rotation AND the UI canvas
// geometry - or the UI draws 320 columns onto a 240-column canvas and the right edge is
// truncated. TFTDisplay.cpp derives both from one helper so they cannot drift apart.
#define LCDWIKI_PORTRAIT 1      // default orientation shipped in the image
#define SCREEN_ROTATE_RUNTIME 1 // config.display.flip_screen picks landscape at boot

#define TFT_OFFSET_ROTATION 0
#define TFT_ROTATION_PORTRAIT 0  // native 240x320; use 2 if it comes up upside down
#define TFT_ROTATION_LANDSCAPE 1 // rotate panel to landscape; use 3 for the other way up
#if LCDWIKI_PORTRAIT
#define TFT_ROTATION TFT_ROTATION_PORTRAIT
#else
#define TFT_ROTATION TFT_ROTATION_LANDSCAPE
#endif

// Touch controller (XPT2046) - shares the HSPI bus with the LCD.
// Consumed by both the ILI9341 and ST7789 LGFX blocks in TFTDisplay.cpp. Touch is polled;
// wake-on-touch would additionally need SCREEN_TOUCH_INT + ENABLE_TOUCH_INT
// (TOUCH_IRQ / TOUCH_INT_PIN are not read by this code path at all).
#define HAS_TOUCHSCREEN 1
#define USE_XPT2046 1
// ESP32-2432S028R puts the XPT2046 on its OWN pins, NOT the LCD bus. Verified
// against cyd-bulb-controller/src/cyd_config.h (2.8in branch):
//   Touch: CLK=25 MOSI=32 MISO=39 CS=33 IRQ=36
#define TOUCH_SCLK 25
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CS 33
#define TOUCH_IRQ 36

// On-screen keyboard for composing free text. Without this the canned-message
// module falls back to an osk_found I2C hardware check and the "[-- Free Text --]"
// entry never appears. See CannedMessageModule.cpp.
#define USE_VIRTUAL_KEYBOARD 1

// SD card. The slot rides the LoRa bus (VSPI 18/19/23) with its own CS on GPIO5, which
// is the ESP32-2432S028R stock wiring and also the esp32dev default SS.
//
// setupSDCard() (FSCommon.cpp:440) takes spiLock around SPI.begin()/SD.begin(), so the
// bus is arbitrated against the SX1262 - but ONLY for setup. Any later read has to take
// spiLock itself or it will corrupt a LoRa transaction in flight; see BaseMap.cpp.
//
// SPI_SCK/MISO/MOSI are referenced by FSCommon.cpp but are not defined anywhere in src/ -
// every variant that enables HAS_SDCARD has to supply them, or the build fails on an
// undeclared identifier at that one line.
#define HAS_SDCARD 1
#define SDCARD_CS 5
#define SPI_SCK 18
#define SPI_MISO 19
#define SPI_MOSI 23

// The card and Bluetooth do not both fit in this board's 8-bit DRAM (no PSRAM): mounting
// the card takes 30,408 bytes and NimBLE then aborts at init with 23,028 left. With this
// set, main.cpp mounts the card only while Bluetooth is off, toggling Bluetooth reboots
// in both directions, and the map frame explains itself when Bluetooth is on. Boards with
// PSRAM run both fine and should not define it.
#define SDCARD_EXCLUSIVE_WITH_BLUETOOTH 1

// Selects the SX1262 driver in RadioInterface.cpp. Without this the whole
// SX1262Interface block is compiled out and the radio is never probed at all,
// surfacing as critical error 3 (NO_RADIO) no matter how it is wired.
#define USE_SX1262

// LoRa SX1262 (Ebyte E22-900M30S) - VSPI, shared with the SD card slot
#define LORA_SCK 18
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_CS 27
#define LORA_RESET 22   // moved off 25 (touch CLK)
#define LORA_DIO1 35   // moved off 39 (touch MISO)

// RadioLib SX126x interface pins
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_RESET LORA_RESET
// BUSY is mandatory for SX126x - RadioLib polls it before and after every SPI
// command (Module.cpp:357, :394). It is a push-pull output from the SX1262, so the
// ESP32 side needs no pull-up and only ever reads it.
// GPIO17 is the RGB LED's blue channel, unused by the firmware; with no PSRAM on this
// module 16/17 are not reserved, and it is not a strapping pin. The blue LED may glow
// faintly while BUSY is low.
// Do not move it back to GPIO34. On the QDTech 2.8" CYD that pin is not free (believed
// to be its TP4056 charger circuit), and with BUSY there RadioLib stalled ~20 s waiting
// for BUSY to go low, init returned -2 (CHIP_NOT_FOUND), and the node recorded critical
// error 3 (NO_RADIO).
#define SX126X_BUSY 17
// Antenna switch, split across two sides:
//
// TX side: DIO2_AS_RF_SWITCH is a CHIP setting, not a pin. It goes over SPI as
// setDio2AsRfSwitch() (SX126xInterface.cpp:134) and tells the SX1262 to drive the
// switch from its own DIO2, which is internal to the module - no DIO2 pad is brought
// out and none needs wiring. TXEN therefore costs no MCU pin.
//
// This is "Option 2" of the four arrangements laid out in
// variants/esp32s3/EBYTE_ESP32-S3/variant.h (the reference board for this module):
// the E22's TXEN pad is jumpered to its own DIO2 pad so the SX1262 drives it, and
// only RXEN costs an MCU pin. That solder jumper is REQUIRED - without it TX never
// switches through the PA.
//
// RX side: RXEN does need an MCU pin. Leaving it undefined would default it to
// RADIOLIB_NC and the receive path would never be switched in.
//
// GPIO4 was chosen over GPIO26: 26 sits immediately next to TOUCH_SCLK (25) and is
// held asserted throughout continuous RX. GPIO4 is the RGB LED red channel - far from
// the touch pins - so red now tracks receive state, which is harmless.
//
// TCXO runs from DIO3 inside the module; the voltage is likewise a chip setting.
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_RXEN 4
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

// TX power is NOT set here. The E22-900M30S has a 10 dB external PA and a 30 dBm
// rated output, so platformio.ini defines EBYTE_E22_900M30S and configuration.h:138
// supplies both TX_GAIN_LORA 7 and SX126X_MAX_POWER 22 for it. RadioInterface.cpp:1446
// subtracts that gain from the requested power before driving the chip, so what leaves
// the antenna matches what the node reports. Defining SX126X_MAX_POWER here as well
// would just duplicate it.
//
// If this board is ever fitted with a bare SX1262 again (E22-900M22S, Wio-SX1262),
// drop -DEBYTE_E22_900M30S: those have no PA, and a 7 dB subtraction would under-drive
// transmit by that much.

// This board has NO PSRAM and only ~94KB free heap after init (of 229KB total).
// NimBLE plus the phone config exchange pushes it over: esp_littlefs reports
// "dir struct could not be malloced" / "Unable to allocate FD" and the node aborts on
// connect. Observed with 60: "Client wants config" -> two "Unable to allocate FD" ->
// abort() on core 1 -> reboot, repeatably, once per phone connection.
//
// MAX_NUM_NODES defaults to 120 on generic ESP32 and costs ~12KB of nodedb, so each
// entry runs about 100 bytes. 120 -> 60 returned roughly 6KB; 60 -> 40 returns about
// 2KB more. The trade is mesh visibility: the node remembers 40 peers instead of 60
// and evicts the oldest beyond that, which only bites on a dense mesh.
#define MAX_NUM_NODES 60

// Framerate used WHILE A TRANSITION IS RUNNING (menu moves, screen changes) - it is not
// a "how slow is the panel" knob. Screen::runOnce() returns (1000 / targetFramerate) as
// its reschedule delay, so this value directly sets the animation step time: 30 -> 33ms,
// 1 -> a full SECOND per frame. This was 1, which made every menu interaction crawl.
// 30 is the upstream default. A full 240x320 16bpp redraw is 153600 bytes, ~31ms at
// 40MHz SPI, so ~20-30fps is the practical ceiling for full-screen frames anyway.
#define SCREEN_TRANSITION_FRAMERATE 30

// Touch sampling cadence. Default idle poll is 100ms, which is the worst-case delay
// before a tap is even noticed; halving it takes a noticeable bite out of perceived lag
// for very little CPU, since the poll is a couple of cheap SPI reads.
#define TOUCH_POLL_INTERVAL_IDLE 50
