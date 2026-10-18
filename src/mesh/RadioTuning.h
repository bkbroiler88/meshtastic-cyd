#pragma once

#include "configuration.h"

// On-device radio knobs (LoRa Actions > Radio Tuning) for boards whose radio needs tuning in the
// field. Stored in NVS rather than the Meshtastic config, so the phone app and CLI do not see them
// and no protobuf changes are needed. A variant opts in with RADIO_TUNING_MENU.
#ifdef RADIO_TUNING_MENU

#include <stdint.h>

namespace radiotune
{

enum class AgcMode : uint8_t {
    WhenIdle = 0,    // upstream: reset only when nothing has decoded for a while, or daily
    EveryMinute = 1, // 2.8.1 behaviour: every maintenance tick
    DailyOnly = 2,   // only the once-a-day forced reset
};

/// TCXO start-up delay programmed after every begin(). Defaults to TCXO_STARTUP_DELAY_US.
uint32_t tcxoDelayUs();
AgcMode agcMode();
/// Continuous RX instead of duty-cycled: the chip never sleeps between RX windows.
bool continuousRx();

void setTcxoDelayUs(uint32_t us);
void setAgcMode(AgcMode mode);
void setContinuousRx(bool on);

/// One read-back of the SX126x's own state (Radio Tuning > Diagnostics). The E22's external PA is
/// switched by TXEN from DIO2 and has no read-back; this only shows what the chip itself reports.
struct ChipSnapshot {
    uint32_t atMs = 0;           // 0 = never taken
    const char *where = "";      // static string naming the call site
    bool busyHigh = false;       // BUSY level when the snapshot started
    bool answered = false;       // status byte was not 0x00/0xFF
    uint8_t status = 0;          // GetStatus: bits 6:4 chip mode, bits 3:1 command status
    uint16_t errors = 0;         // GetDeviceErrors (only read when BUSY was low)
    bool regsValid = false;      // registers below were read
    uint8_t ocp = 0, txClamp = 0, rxGain = 0;
    int8_t rxen = -1;            // RXEN pin level, -1 if the board has none
};

enum class SnapKind : uint8_t { TxDone, StandbyFail, Fault, Boot };

struct ChipStats {
    uint32_t txDone = 0;
    uint32_t paRampErr = 0;      // TX-done snapshots with PA_RAMP_ERR set
    uint32_t xoscErr = 0;        // any snapshot with XOSC_START_ERR set
    uint32_t standbyFail = 0;    // standby() returned an error (the -705 path)
    uint32_t busyStuck = 0;      // snapshots that found BUSY still high
    uint16_t errorsSeen = 0;     // OR of every device-error word read after boot
    uint16_t bootErrors = 0;     // errors latched by begin(), read once right after init
};

void recordSnapshot(const ChipSnapshot &s, SnapKind kind);
const ChipSnapshot &lastSnapshot();
const ChipSnapshot &lastFaultSnapshot(); // last one taken on a standby failure or re-init
const ChipStats &chipStats();
/// Resets the counts only; the caller clears the chip's latched flags (captureChipDiag)
void clearChipStats();
/// "STBY_RC", "RX", ... from a GetStatus byte
const char *chipModeName(uint8_t status);

} // namespace radiotune

#endif
