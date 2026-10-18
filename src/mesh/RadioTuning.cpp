#include "RadioTuning.h"

#ifdef RADIO_TUNING_MENU

#include "mesh/RadioLibInterface.h" // TCXO_STARTUP_DELAY_US
#include <Preferences.h>

namespace radiotune
{
namespace
{
constexpr const char *kNamespace = "radiotune";

bool loaded = false;
uint32_t tcxoUs = TCXO_STARTUP_DELAY_US;
AgcMode agc = AgcMode::WhenIdle;
bool continuous = false;

// Read once, on first use. A missing namespace (never saved) leaves the build defaults.
void load()
{
    if (loaded)
        return;
    loaded = true;
    Preferences prefs;
    if (!prefs.begin(kNamespace, true))
        return;
    tcxoUs = prefs.getUInt("tcxo", TCXO_STARTUP_DELAY_US);
    const uint8_t a = prefs.getUChar("agc", (uint8_t)AgcMode::WhenIdle);
    agc = a <= (uint8_t)AgcMode::DailyOnly ? (AgcMode)a : AgcMode::WhenIdle;
    continuous = prefs.getBool("rxc", false);
    prefs.end();
}

void save()
{
    Preferences prefs;
    if (!prefs.begin(kNamespace, false))
        return;
    prefs.putUInt("tcxo", tcxoUs);
    prefs.putUChar("agc", (uint8_t)agc);
    prefs.putBool("rxc", continuous);
    prefs.end();
}

ChipSnapshot lastSnap;
ChipSnapshot lastFault;
ChipStats stats;
} // namespace

void recordSnapshot(const ChipSnapshot &s, SnapKind kind)
{
    lastSnap = s;
    if (kind == SnapKind::Boot) {
        // begin() commonly leaves XOSC_START_ERR set before the TCXO settles; keep it out of the run counts
        stats.bootErrors = s.errors;
        return;
    }
    stats.errorsSeen |= s.errors;
    if (s.busyHigh)
        stats.busyStuck++;
    if (s.errors & RADIOLIB_SX126X_XOSC_START_ERR)
        stats.xoscErr++;
    if (kind == SnapKind::TxDone) {
        stats.txDone++;
        if (s.errors & RADIOLIB_SX126X_PA_RAMP_ERR)
            stats.paRampErr++;
        return;
    }
    lastFault = s;
    if (kind == SnapKind::StandbyFail)
        stats.standbyFail++;
}

const ChipSnapshot &lastSnapshot()
{
    return lastSnap;
}

const ChipSnapshot &lastFaultSnapshot()
{
    return lastFault;
}

const ChipStats &chipStats()
{
    return stats;
}

void clearChipStats()
{
    stats = ChipStats();
    lastSnap = ChipSnapshot();
    lastFault = ChipSnapshot();
}

const char *chipModeName(uint8_t status)
{
    switch ((status >> 4) & 0x07) {
    case 2:
        return "STBY_RC";
    case 3:
        return "STBY_XOSC";
    case 4:
        return "FS";
    case 5:
        return "RX";
    case 6:
        return "TX";
    default:
        return "?";
    }
}

uint32_t tcxoDelayUs()
{
    load();
    return tcxoUs;
}

AgcMode agcMode()
{
    load();
    return agc;
}

bool continuousRx()
{
    load();
    return continuous;
}

void setTcxoDelayUs(uint32_t us)
{
    load();
    tcxoUs = us;
    save();
}

void setAgcMode(AgcMode mode)
{
    load();
    agc = mode;
    save();
}

void setContinuousRx(bool on)
{
    load();
    continuous = on;
    save();
}

} // namespace radiotune

#endif
