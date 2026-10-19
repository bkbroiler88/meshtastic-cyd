// LCDWIKI 3.5" ESP32-32E display with an Ebyte E22-900M30S LoRa module (US 915 MHz).
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
// Display: ST7796U, 320x480, on HSPI (12/13/14)
// Touch:   XPT2046 on the SHARED LCD HSPI bus (12/13/14), CS 33
// LoRa:    SX1262 on VSPI (18/19/23), separate CS
//
// This is the 2.8" variant adapted for the 3.5" board. The LCDWIKI pinout
// (https://www.lcdwiki.com/3.5inch_ESP32-32E_Display) is the source, cross-checked against
// cyd-bulb-controller's proven "cyd35" env. Four things change vs the 2.8":
//   1. Controller is an ST7796U at 320x480 (the 2.8" is an ILI9341/ST7789 at 240x320).
//   2. Backlight is GPIO27 (the 2.8" had it on GPIO21).
//   3. The E22 LoRa chip-select moves off GPIO27 - now the backlight - onto GPIO21, the
//      board's broken-out "SPI peripheral CS" on the JST header. The LCD CS stays GPIO15.
//   4. Touch (XPT2046) is on the SHARED LCD HSPI bus (14/13/12, CS 33), not the 2.8"'s
//      dedicated 25/32/39 pins - so those three pins are free here. The ST7796 block in
//      TFTDisplay.cpp binds shared-bus touch for exactly this reason; see that block.
// SD and the rest of the E22 wiring are carried over unchanged; verify on hardware.
//
// Hardware notes:
// - No PSRAM, 320KB RAM, 4MB flash -> see platformio.ini for feature exclusions. The
//   320x480 panel costs ~10 KB more RAM than the 2.8" 240x320 (a bigger 1-bit framebuffer
//   and line buffers); watch the free-heap margin - it was already tight at 240x320.
// - Base UI only (not MUI).
// - Touch shares the LCD bus here, so GPIO25/32/39 (the 2.8"'s dedicated touch pins) are
//   FREE on this board, along with GPIO21's old backlight duty. I2C is still left
//   undefined, same as the 2.8", but 25/32 are open if you want to wire a sensor bus.

// Button
#define BUTTON_PIN 0 // GPIO 0, pulled low to activate
#define BUTTON_NEED_PULLUP

// Battery sense on GPIO34 (ADC1 channel 6). The QDTech 2.8" CYD carries a TP4056 charger
// and is believed to bring the battery to this pin through a resistor divider.
// ADC_MULTIPLIER is PROVISIONAL - the divider ratio has not been measured. Calibrate it by
// comparing the reported battery voltage with a meter across the cell, then set it here,
// or at runtime with no reflash via config.power.adc_multiplier_override (Power.cpp:435).
// Err high, not low: with no USB-detect pin, a reading between 2600 and 3100 mV counts as
// an empty battery on no USB, and Power.cpp deep-sleeps the node after 11 such readings.
// The default 12 dB attenuation reads ~150-3100 mV at the pin, enough for a Li-ion behind
// a 1:1 divider (4.2 V -> 2.1 V).
#define BATTERY_PIN 34
#define ADC_CHANNEL ADC_CHANNEL_6
#define ADC_MULTIPLIER 2.0

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
// Natively 320x480 (portrait) - the ST7796U's full resolution. These feed panel_width/
// height in the ST7796 LGFX block below, so they must be the NATIVE values.
// TFT_ROTATION turns it landscape.
#define TFT_WIDTH 320
#define TFT_HEIGHT 480
#define TFT_OFFSET_X 0
#define TFT_OFFSET_Y 0
#define TFT_BUSY -1

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1 // panel reset is tied to EN on this board
#define TFT_BL 27 // PWM backlight (the 2.8" had this on GPIO21)

#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 16000000

// PANEL CONTROLLER. The LCDWIKI 3.5" ESP32-32E board is an ST7796U at 320x480 - a single
// controller, so there is no build-time panel selection here (unlike the 2.8", which
// shipped as either ILI9341 or ST7789). TFTDisplay.cpp selects the ST7796 LGFX block on
// ST7796_CS being defined and reads the pins through the ST7796_* names below.
//
// If text comes up mirrored or the frame is misplaced, that is a rotation/offset issue
// (TFT_ROTATION / TFT_OFFSET_ROTATION), not a wrong-controller issue.

// CYD panels ship with either colour polarity under the same product name, so which
// way round looks correct is a per-board fact rather than a per-model one. Exposes
// a Display Options entry that drives the controller's INVON/INVOFF via
// invertDisplay(), persisted in NVS. cyd-bulb-controller does the same thing
// (src/main.cpp applyInversion()).
//
// NOT config.display.displaymode INVERTED - that only styles the monochrome header
// bitmap in SharedUIDisplay and does nothing once GRAPHICS_TFT_COLORING_ENABLED is on.
#define LCDWIKI_PANEL_INVERT_TOGGLE 1
// This board's ST7796U panel reads as a negative image with inversion off, so start inverted.
// Display Options > Invert Colors still overrides it per board (saved in NVS).
#define LCDWIKI_PANEL_INVERT_DEFAULT 1

// TFTDisplay.cpp selects the LovyanGFX ST7796 block on ST7796_CS and reads the pins
// through these names. Geometry comes from the TFT_* macros above. Do NOT define
// USE_ST7796 - that selects a different (AdafruitGFX) driver path in Screen.cpp; this
// board uses the LGFX path (USE_TFTDISPLAY + ST7796_CS), as the 2.8" did for ST7789.
#define ST7796_SPI_HOST HSPI_HOST
#define ST7796_SCK TFT_SCLK
#define ST7796_SDA TFT_MOSI
#define ST7796_MISO TFT_MISO
#define ST7796_RS TFT_DC
#define ST7796_CS TFT_CS
#define ST7796_RESET TFT_RST
#define ST7796_BUSY TFT_BUSY
#define ST7796_BL TFT_BL

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
// the same in both orientations (19200 bytes for this 320x480 panel).
//
// Both halves of the orientation must agree - the panel rotation AND the UI canvas
// geometry - or the UI draws 320 columns onto a 240-column canvas and the right edge is
// truncated. TFTDisplay.cpp derives both from one helper so they cannot drift apart.
#define LCDWIKI_PORTRAIT 1      // default orientation shipped in the image
#define SCREEN_ROTATE_RUNTIME 1 // config.display.flip_screen picks landscape at boot

#define TFT_OFFSET_ROTATION 0
#define TFT_ROTATION_PORTRAIT 0  // native 320x480; use 2 if it comes up upside down
#define TFT_ROTATION_LANDSCAPE 1 // rotate panel to landscape; use 3 for the other way up
#if LCDWIKI_PORTRAIT
#define TFT_ROTATION TFT_ROTATION_PORTRAIT
#else
#define TFT_ROTATION TFT_ROTATION_LANDSCAPE
#endif

// Touch controller (XPT2046). On this 3.5" board the XPT2046 SHARES the LCD's HSPI bus -
// CLK=14 MOSI=13 MISO=12 - with its own CS=33 and IRQ=36. This is the real wiring per the
// LCDWIKI pinout table (and confirmed against cyd-bulb-controller's cyd35 env), and it is
// why GPIO25/32/39 - the 2.8"'s DEDICATED touch pins - are free on the 3.5".
//
// Because the bus is shared, the ST7796 block in TFTDisplay.cpp binds touch onto the
// panel's own SPI host with bus_shared=true (the LGFX equivalent of TFT_eSPI serving touch
// from the bus it already owns). This differs from the 2.8" ST7789 path, which bit-bangs
// the XPT2046 on dedicated pins (spi_host=-1). Touch reads drop to 2.5 MHz either way.
//
// Touch is polled; the driver leaves pin_int = -1, so TOUCH_IRQ is documentation only.
#define HAS_TOUCHSCREEN 1
#define USE_XPT2046 1
#define TOUCH_SCLK 14   // shared with TFT_SCLK
#define TOUCH_MOSI 13   // shared with TFT_MOSI
#define TOUCH_MISO 12   // shared with TFT_MISO
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

// The card does not fit alongside a wireless stack in this board's 8-bit DRAM (no PSRAM):
// mounting it takes 30,408 bytes and NimBLE then aborts at init with 23,028 left. Wi-Fi is
// the same squeeze - it allocates its own buffers, and Meshtastic already treats Wi-Fi and
// BLE as mutually exclusive on ESP32 (main-esp32.cpp: BLE only starts when Wi-Fi is not
// available), so without this the card would quietly mount alongside Wi-Fi instead.
//
// With this set, main.cpp mounts the card only when BOTH are off. Maps are an offline
// feature, so a node on Wi-Fi is not the case that wants them. Toggling Bluetooth reboots
// in both directions and the map frame says which radio is holding the card off. Boards
// with PSRAM have room for both and should not define this.
#define SDCARD_EXCLUSIVE_WITH_BLE_WIFI 1

// Tried and rejected: SDCARD_ALLOW_WITH_WIFI, mounting the card alongside Wi-Fi so the web
// UI could be served from it. The card and Wi-Fi do fit together (117 KB of 8-bit DRAM
// free after mounting), but the web server then has 1,504 bytes where it needs ~30 KB, and
// its first allocation aborts. Measured, not assumed. The web UI is served from internal
// flash instead, so the card is only ever wanted for maps - which need both radios off
// anyway, exactly what the gate below already enforces.

// Selects the SX1262 driver in RadioInterface.cpp. Without this the whole
// SX1262Interface block is compiled out and the radio is never probed at all,
// surfacing as critical error 3 (NO_RADIO) no matter how it is wired.
#define USE_SX1262

// LoRa SX1262 (Ebyte E22-900M30S) - VSPI, shared with the SD card slot.
//
// On the 3.5" the whole control-pin group moves onto the board's cleanly broken-out
// expansion pins (freed because the backlight left GPIO21 and touch moved to the shared
// LCD bus, freeing 25/32/39). This pulls the radio OFF the RGB-LED and audio pins the 2.8"
// borrowed (17/4/22), so the LED and audio amp no longer twitch with radio state.
//
// The governing constraint is that GPIO35 and GPIO39 are INPUT-ONLY on the ESP32: they can
// carry only the two signals the MCU READS from the radio (DIO1, BUSY). CS, RESET and RXEN
// are MCU outputs and must sit on full-I/O pins (21, 25, 32).
#define LORA_SCK 18
#define LORA_MISO 19
#define LORA_MOSI 23
#define LORA_CS 21   // JST "SPI peripheral CS" (full I/O). Was 27 on the 2.8"; the 3.5"
                     // backlight owns 27 now.
#define LORA_RESET 25   // freed touch SCLK (full I/O). Output, so it cannot be 35/39.
#define LORA_DIO1 35   // freed/expansion input-only pin; DIO1 is an MCU input, so fine

// RadioLib SX126x interface pins
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_RESET LORA_RESET
// BUSY is mandatory for SX126x - RadioLib polls it before and after every SPI
// command (Module.cpp:357, :394). It is a push-pull output from the SX1262, so the
// ESP32 side needs no pull-up and only ever reads it - which makes it a perfect fit for
// the input-only GPIO39 (freed touch MISO). Do not put BUSY on GPIO34: on the QDTech 2.8"
// CYD that pin is not free (believed to be its TP4056 charger divider), and with BUSY
// there RadioLib stalled ~20 s waiting for BUSY to go low, init returned -2
// (CHIP_NOT_FOUND), and the node recorded critical error 3 (NO_RADIO).
#define SX126X_BUSY 39
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
// RXEN is an MCU output (it switches the antenna to RX), so it takes the one freed full-
// I/O expansion pin, GPIO32 (freed touch MOSI) - not 35/39, which are input-only. This
// also frees the 2.8"'s GPIO4 (the audio-amp enable) back to its stock duty.
//
// TCXO runs from DIO3 inside the module; the voltage is likewise a chip setting.
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_RXEN 32
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
// On-device LoRa Actions > Radio Tuning: TCXO start-up, AGC reset policy and RX mode, stored in
// NVS (src/mesh/RadioTuning.*). Added while chasing critical fault 7 after long idle on 2.8.1.
#define RADIO_TUNING_MENU
// LoRa Actions > Spectrum: sweep analyzer + test transmitter on the E22 (pauses the mesh)
#define SPECTRUM_ANALYZER
// Home long-press > Quick Settings, System > Settings (names on the colour keyboard)
#define LCDWIKI_EXTRAS
// Default 5 ms, RadioLib's own value and what 2.8.0 ran with. Upstream's 1 ms (#12018) is tuned on
// other boards; the E22's DIO3 TCXO may need longer when the chip wakes from duty-cycle sleep.
#define TCXO_STARTUP_DELAY_US 5000

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
//
// The 3.5" runs ~24 KB tighter than the 2.8" (its 320x480 framebuffer is double-buffered),
// measured at 70 KB free when NimBLE starts vs ~94 KB on the 2.8". 50 gives back ~1 KB.
#define MAX_NUM_NODES 50

// Framerate used WHILE A TRANSITION IS RUNNING (menu moves, screen changes) - it is not
// a "how slow is the panel" knob. Screen::runOnce() returns (1000 / targetFramerate) as
// its reschedule delay, so this value directly sets the animation step time: 30 -> 33ms,
// 1 -> a full SECOND per frame. This was 1, which made every menu interaction crawl.
// 30 is the upstream default. A full 320x480 16bpp redraw is 307200 bytes, ~61ms at
// 40MHz SPI (twice the 2.8"), so full-screen frames are slower here - the UI draws in
// bounded chunks rather than full frames, so this mostly bites on transitions.
#define SCREEN_TRANSITION_FRAMERATE 30

// Touch sampling cadence. Default idle poll is 100ms, which is the worst-case delay
// before a tap is even noticed; halving it takes a noticeable bite out of perceived lag
// for very little CPU, since the poll is a couple of cheap SPI reads.
#define TOUCH_POLL_INTERVAL_IDLE 50
