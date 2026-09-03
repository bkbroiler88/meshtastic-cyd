// LCDWIKI 2.8" ESP32-32E display with a Seeed Wio-SX1262 LoRa module (US 915 MHz).
// Wideband 862-930 MHz, +22 dBm, TCXO on DIO3, IPEX/u.FL antenna.
//
// Originally built against an Ebyte E22-900M22S and the radio config is unchanged
// between the two - both are a bare SX1262 with no external PA, switching TX through
// the chip's own DIO2 and RX through one MCU pin. Only the pad naming differs: the
// E22 calls that pad RXEN, the Wio-SX1262 silkscreens it RF_SW.
//
// If swapping back to an Ebyte part, note the suffix: "M" is the bare-SX1262 SPI part
// this config expects; the "T" parts are UART modules with an onboard MCU and cannot
// be driven by SX1262Interface at all.
//
// Display: ILI9341 on HSPI (12/13/14)
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

// Display (ILI9341) - HSPI bus
#define HAS_SPI_TFT 1
#define ILI9341_DRIVER
#define USE_TFTDISPLAY 1
#define ILI9341_SPI_HOST HSPI_HOST
// ILI9341 is natively 240x320 (portrait). These feed both panel_width/height AND
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
// Consumed by the ILI9341 LGFX block in TFTDisplay.cpp. Touch is polled;
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

// SD card: Meshtastic has no per-pin SD macros - the SD card rides whatever bus
// SPI.begin() was called with, which here is the LoRa bus (18/19/23). Enabling it
// requires defining HAS_SDCARD plus SDCARD_CS; left off until the radio is proven.

// Selects the SX1262 driver in RadioInterface.cpp. Without this the whole
// SX1262Interface block is compiled out and the radio is never probed at all,
// surfacing as critical error 3 (NO_RADIO) no matter how it is wired.
#define USE_SX1262

// LoRa SX1262 (Seeed Wio-SX1262) - VSPI, shared with the SD card slot
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
// command (Module.cpp:357, :394). Wired to GPIO34, which is input-only; fine here
// since the ESP32 only ever reads it.
#define SX126X_BUSY 34
// Antenna switch, split across the two sides exactly as the E22 was:
//
// TX side: DIO2_AS_RF_SWITCH is a CHIP setting, not a pin. It goes over SPI as
// setDio2AsRfSwitch() (SX126xInterface.cpp:134) and tells the SX1262 to drive the
// switch from its own DIO2, which is internal to the module - no DIO2 pad is brought
// out and none needs wiring. TXEN therefore costs no MCU pin.
//
// RX side: the Wio-SX1262 labels this pad RF_SW on the underside rather than RXEN,
// but it is the same signal and it does need an MCU pin. Seeed's own board agrees -
// variants/esp32s3/seeed_xiao_s3/variant.h ties that pad to a GPIO and declares it
// as SX126X_RXEN alongside DIO2_AS_RF_SWITCH. Leaving it undefined would default it
// to RADIOLIB_NC and the receive path would never be switched in.
//
// GPIO4 was chosen over GPIO26: 26 sits immediately next to TOUCH_SCLK (25) and is
// held asserted throughout continuous RX. GPIO4 is the RGB LED red channel - far from
// the touch pins - so red now tracks receive state, which is harmless.
//
// TCXO runs from DIO3 inside the module; the voltage is likewise a chip setting.
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_RXEN 4 // module pad is silkscreened RF_SW
#define SX126X_DIO3_TCXO_VOLTAGE 1.8

// The Wio-SX1262 is a raw SX1262 with NO external PA, so it needs no TX_GAIN_LORA
// compensation. Do NOT define EBYTE_E22_900M30S here: that sets TX_GAIN_LORA 7 for a
// PA this module does not have, which would under-drive transmit by ~7dB.
// 22 is also the SX126X_MAX_POWER default; stated explicitly for clarity.
#define SX126X_MAX_POWER 22

// This board has NO PSRAM and only ~90KB free heap after boot (of 229KB total).
// NimBLE plus the phone config exchange pushes it over: esp_littlefs reports
// "dir struct could not be malloced" / "Unable to allocate FD" and the node can die
// on connect. MAX_NUM_NODES defaults to 120 on generic ESP32 and costs ~12KB of
// nodedb; 60 halves that and returns roughly 6KB to the heap.
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
