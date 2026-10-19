#if RADIOLIB_EXCLUDE_SX126X != 1
#include "SX126xInterface.h"
#include "configuration.h"
#include "error.h"
#include "mesh/NodeDB.h"
#include "mesh/RadioTuning.h"
#ifdef ARCH_PORTDUINO
#include "PortduinoGlue.h"
#endif
#if defined(ARCH_ESP32)
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#endif

#include "Throttle.h"
#include "UptimeClock.h"

// Particular boards might define a different max power based on what their hardware can do, default to max power output if not
// specified (may be dangerous if using external PA and SX126x power config forgotten)
#if ARCH_PORTDUINO
#define SX126X_MAX_POWER portduino_config.sx126x_max_power
#endif
#ifndef SX126X_MAX_POWER
#define SX126X_MAX_POWER 22
#endif

template <typename T>
SX126xInterface<T>::SX126xInterface(LockingArduinoHal *hal, RADIOLIB_PIN_TYPE cs, RADIOLIB_PIN_TYPE irq, RADIOLIB_PIN_TYPE rst,
                                    RADIOLIB_PIN_TYPE busy)
    : RadioLibInterface(hal, cs, irq, rst, busy, &lora), lora(&module)
{
    LOG_DEBUG("SX126xInterface(cs=%d, irq=%d, rst=%d, busy=%d)", cs, irq, rst, busy);
#ifdef RADIO_TUNING_MENU
    diagCs = cs;
    diagBusy = busy;
#endif
}

/// Initialise the Driver transport hardware and software.
/// Make sure the Driver is properly configured before calling init().
/// \return true if initialisation succeeded.
template <typename T> bool SX126xInterface<T>::init()
{

// Typically, the RF switch on SX126x boards is controlled by two signals, which are negations of each other (switched RFIO
// paths). The negation is usually performed in hardware, or (suboptimal design) TXEN and RXEN are the two inputs to this style of
// RF switch. On some boards, there is no hardware negation between CTRL and ¬CTRL, but CTRL is internally connected to DIO2, and
// DIO2's switching is done by the SX126X itself, so the MCU can't control ¬CTRL at exactly the same time. One solution would be
// to set ¬CTRL as SX126X_TXEN or SX126X_RXEN, but they may already be used for another purpose, such as controlling another
// PA/LNA. Keeping ¬CTRL high seems to work, as long CTRL=1, ¬CTRL=1 has the opposite and stable RF path effect as CTRL=0 and
// ¬CTRL=1, this depends on the RF switch, but it seems this usually works. Better hardware design, which is done most the time,
// means this workaround is not necessary.
#ifdef SX126X_ANT_SW // Perhaps add RADIOLIB_NC check, and beforehand define as such if it is undefined, but it is not commonly
                     // used and not part of the 'default' set of pin definitions.
    digitalWrite(SX126X_ANT_SW, HIGH);
    pinMode(SX126X_ANT_SW, OUTPUT);
#endif

#ifdef SX126X_POWER_EN // Perhaps add RADIOLIB_NC check, and beforehand define as such if it is undefined, but it is not commonly
                       // used and not part of the 'default' set of pin definitions.
    pinMode(SX126X_POWER_EN, OUTPUT);
    digitalWrite(SX126X_POWER_EN, HIGH);
#endif

#if HAS_LORA_FEM
    loraFEMInterface.init();
    // Apply saved FEM LNA mode from config
    if (loraFEMInterface.isLnaCanControl()) {
        loraFEMInterface.setLNAEnable(config.lora.fem_lna_mode != meshtastic_Config_LoRaConfig_FEM_LNA_Mode_DISABLED);
    }
#endif

#ifdef RF95_FAN_EN
    pinMode(RF95_FAN_EN, OUTPUT);
    digitalWrite(RF95_FAN_EN, HIGH);
#endif

#if ARCH_PORTDUINO
    // An explicit Vref wins; probing with none given tries the radio default first.
    bool tcxoVoltageExplicit = portduino_config.dio3_tcxo_voltage > 0;
    if (tcxoVoltageExplicit)
        tcxoVoltage = (float)portduino_config.dio3_tcxo_voltage / 1000;
    else if (TCXO_OPTIONAL_ENABLED)
        tcxoVoltage = TCXO_OPTIONAL_DEFAULT_VOLTAGE;
    else
        tcxoVoltage = 0;
    if (portduino_config.lora_sx126x_ant_sw_pin.pin != RADIOLIB_NC) {
        digitalWrite(portduino_config.lora_sx126x_ant_sw_pin.pin, HIGH);
        pinMode(portduino_config.lora_sx126x_ant_sw_pin.pin, OUTPUT);
    }
    // The knob here is the YAML key, not the variant define the other branch reports.
    if (tcxoVoltage == 0.0)
        LOG_DEBUG("Lora.DIO3_TCXO_VOLTAGE not set, DIO3 not used as TCXO Vref");
    else if (!tcxoVoltageExplicit)
        LOG_DEBUG("TCXO_OPTIONAL: no Vref configured, probing default TCXO Vref %f V on DIO3", tcxoVoltage);
    else
        LOG_DEBUG("Lora.DIO3_TCXO_VOLTAGE set, DIO3 as TCXO Vref %f V", tcxoVoltage);
#else
    if (tcxoVoltage == 0.0)
        LOG_DEBUG("SX126X_DIO3_TCXO_VOLTAGE not defined, DIO3 not used as TCXO Vref");
    else
        LOG_DEBUG("SX126X_DIO3_TCXO_VOLTAGE defined, DIO3 as TCXO Vref %f V", tcxoVoltage);
#endif
    setTransmitEnable(false);

    RadioLibInterface::init();

    if (!reinitChip())
        return false;
    boundBusyWait();
#ifdef RADIO_TUNING_MENU
    captureChipDiag("boot", radiotune::SnapKind::Boot); // read + clear what begin() latched
#endif

    startReceive(); // start receiving

    return true;
}

// begin() and the chip-side setup that a reset chip loses: begin() hardware-resets the chip, then
// PA ramp, OCP limit, DIO2-as-RF-switch, RF switch pins, RX gain, the 0x8B5 RX patch, and CRC are
// reprogrammed. Shared by init() and by reconfigure()'s recovery path.
template <typename T> bool SX126xInterface<T>::reinitChip()
{
    // Clamp here, not just in programModemParams(): applyModemConfig() resets `power` to the raw
    // config value, and the recovery path reaches begin() without passing through the params clamp
    limitPower(SX126X_MAX_POWER);
    // Make sure we reach the minimum power supported to turn the chip on (-9dBm)
    if (power < -9)
        power = -9;

    // FIXME: May want to set depending on a definition, currently all SX126x variant files use the DC-DC regulator option
    bool useRegulatorLDO = false; // Seems to depend on the connection to pin 9/DCC_SW - if an inductor DCDC?

    int res = lora.begin(getFreq(), bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage, useRegulatorLDO);

    // Chip answered but would not start on the TCXO: retry on the XTAL. CHIP_NOT_FOUND is a
    // wiring or SPI fault, where a second attempt only hides it. Portduino only - an embedded
    // board gets this from the second interface instance in initLoRa()'s ladder.
    // TODO: consider deferring to RadioLib, which has autocorrected this itself since 7.5.0:
    // SX126x::modSetup() retries config() on the XTAL when begin() fails with SPI_CMD_FAILED and
    // XOSC_START_ERR, so the ordinary "TCXO configured, XTAL fitted" case never reaches here and
    // what does is mostly invalid settings. Narrowing this to SPI_CMD_TIMEOUT - the oscillator
    // symptom RadioLib's condition misses - would keep the cover and drop the misdiagnosis.
#if ARCH_PORTDUINO
    if (TCXO_OPTIONAL_ENABLED && res != RADIOLIB_ERR_NONE && res != RADIOLIB_ERR_CHIP_NOT_FOUND && tcxoVoltage > 0) {
        LOG_WARN("SX126x init failed with TCXO Vref %f V (err %d), retrying without TCXO", tcxoVoltage, res);
        tcxoVoltage = 0;
        res = lora.begin(getFreq(), bw, sf, cr, syncWord, power, preambleLength, tcxoVoltage, useRegulatorLDO);
        if (res == RADIOLIB_ERR_NONE)
            LOG_INFO("SX126x init success without TCXO (XTAL mode)");
    }
#endif
    if (res == RADIOLIB_ERR_NONE) {
        applyTcxoStartupDelay(lora, tcxoVoltage);
#ifdef SX126X_PA_RAMP_US
        // Set custom PA ramp time for boards requiring longer stabilization (e.g., T-Beam 1W needs >800us)
        lora.setPaRampTime(SX126X_PA_RAMP_US);
#endif
    }
    // \todo Display actual typename of the adapter, not just `SX126x`
    LOG_INFO("SX126x init result %d", res);
    if (res == RADIOLIB_ERR_CHIP_NOT_FOUND || res == RADIOLIB_ERR_SPI_CMD_FAILED)
        return false;

    LOG_INFO("Frequency set to %f", getFreq());
    LOG_INFO("Bandwidth set to %f", bw);
    LOG_INFO("Power output set to %d", power);

    // Overriding current limit
    // (https://github.com/jgromes/RadioLib/blob/690a050ebb46e6097c5d00c371e961c1caa3b52e/src/modules/SX126x/SX126x.cpp#L85) using
    // value in SX126xInterface.h (currently 140 mA) It may or may not be necessary, depending on how RadioLib functions, from
    // SX1261/2 datasheet: OCP after setting DeviceSel with SetPaConfig(): SX1261 - 60 mA, SX1262 - 140 mA For the SX1268 the IC
    // defaults to 140mA no matter the set power level, but RadioLib set it lower, this would need further checking Default values
    // are: SX1262, SX1268: 0x38 (140 mA), SX1261: 0x18 (60 mA)
    // FIXME: Not ideal to increase SX1261 current limit above 60mA as it can only transmit max 15dBm, should probably only do it
    // if using SX1262 or SX1268
    res = lora.setCurrentLimit(currentLimit);
    LOG_DEBUG("Current limit set to %f", currentLimit);
    LOG_DEBUG("Current limit set result %d", res);

    if (res == RADIOLIB_ERR_NONE) {
#ifdef SX126X_DIO2_AS_RF_SWITCH
        bool dio2AsRfSwitch = true;
#elif defined(ARCH_PORTDUINO)
        bool dio2AsRfSwitch = false;
        if (portduino_config.dio2_as_rf_switch) {
            dio2AsRfSwitch = true;
        }
#else
        bool dio2AsRfSwitch = false;
#endif
        res = lora.setDio2AsRfSwitch(dio2AsRfSwitch);
        LOG_DEBUG("Set DIO2 as %sRF switch, result: %d", dio2AsRfSwitch ? "" : "not ", res);
    }

// If a pin isn't defined, we set it to RADIOLIB_NC, it is safe to always do external RF switching with RADIOLIB_NC as it has
// no effect
#if ARCH_PORTDUINO
    if (res == RADIOLIB_ERR_NONE) {
        LOG_DEBUG("Use MCU pin %i as RXEN, pin %i as TXEN for RF switching", portduino_config.lora_rxen_pin.pin,
                  portduino_config.lora_txen_pin.pin);
        lora.setRfSwitchPins(portduino_config.lora_rxen_pin.pin, portduino_config.lora_txen_pin.pin);
    }
#else
#ifndef SX126X_RXEN
#define SX126X_RXEN RADIOLIB_NC
    LOG_DEBUG("SX126X_RXEN not defined, default RADIOLIB_NC");
#endif
#ifndef SX126X_TXEN
#define SX126X_TXEN RADIOLIB_NC
    LOG_DEBUG("SX126X_TXEN not defined, default RADIOLIB_NC");
#endif
    if (res == RADIOLIB_ERR_NONE) {
        LOG_DEBUG("Use MCU pin %i as RXEN, pin %i as TXEN for RF switching", SX126X_RXEN, SX126X_TXEN);
        lora.setRfSwitchPins(SX126X_RXEN, SX126X_TXEN);
    }
#endif
    if (config.lora.sx126x_rx_boosted_gain) {
        uint16_t result = lora.setRxBoostedGainMode(true);
        LOG_INFO("Set RX gain to boosted mode; result: %d", result);
    } else {
        uint16_t result = lora.setRxBoostedGainMode(false);
        LOG_INFO("Set RX gain to power saving mode; result: %d", result);
    }

    // Undocumented SX1262 register patch recommended by Heltec/Semtech for improved RX sensitivity.
    // Sets bit 0 of register 0x8B5.
    if (module.SPIsetRegValue(0x8B5, 0x01, 0, 0) == RADIOLIB_ERR_NONE) {
        LOG_INFO("Applied SX1262 reg 0x8B5 RX patch");
    } else {
        LOG_WARN("Can't apply SX1262 reg 0x8B5 RX patch");
    }

    if (res == RADIOLIB_ERR_NONE)
        res = lora.setCRC(RADIOLIB_SX126X_LORA_CRC_ON);

#ifdef SX126X_NO_POWER_OPTIMIZATION_TABLE
    // begin() applied the optimization table; re-apply the fixed PA config.
    if (res == RADIOLIB_ERR_NONE)
        res = lora.setOutputPower(power, false);
#endif

    if (res != RADIOLIB_ERR_NONE)
        LOG_ERROR("SX126x re-init failed %s%d", radioLibErr, res);
    return res == RADIOLIB_ERR_NONE;
}

template <typename T> int16_t SX126xInterface<T>::programModemParams()
{
    // configure publicly accessible settings
    int16_t err = lora.setSpreadingFactor(sf);
    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X setSpreadingFactor(%u) %s%d", sf, radioLibErr, err);
        return err;
    }

    err = lora.setBandwidth(bw);
    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X setBandwidth(%.1f) %s%d", bw, radioLibErr, err);
        return err;
    }

    err = lora.setCodingRate(cr);
    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X setCodingRate(%u) %s%d", cr, radioLibErr, err);
        return err;
    }

    err = lora.setSyncWord(syncWord);
    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X setSyncWord %s%d", radioLibErr, err);
        return err;
    }

    err = lora.setCurrentLimit(currentLimit);
    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X setCurrentLimit %s%d", radioLibErr, err);
        return err;
    }

    err = lora.setPreambleLength(preambleLength);
    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X setPreambleLength(%u) %s%d", preambleLength, radioLibErr, err);
        return err;
    }

    err = lora.setFrequency(getFreq());
    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X setFrequency(%.3f) %s%d", getFreq(), radioLibErr, err);
        return err;
    }

    limitPower(SX126X_MAX_POWER);
    // Make sure we reach the minimum power supported to turn the chip on (-9dBm)
    if (power < -9)
        power = -9;

#ifdef SX126X_NO_POWER_OPTIMIZATION_TABLE
    err = lora.setOutputPower(power, false); // external PA: fixed PA config
#else
    err = lora.setOutputPower(power);
#endif
    if (err != RADIOLIB_ERR_NONE) {
        // Don't abort: this power is operator config (tx_power/SX126X_MAX_POWER); a value above the
        // driver's max would crash the daemon before reloadConfig() persists. Flag it and keep prior power.
        LOG_ERROR("SX126X setOutputPower %d dBm rejected (%s%d); keep previous Tx power", power, radioLibErr, err);
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);
    }

    // Apply RX gain mode - valid in STDBY (datasheet §9.6), matches resetAGC() pattern
    err = lora.setRxBoostedGainMode(config.lora.sx126x_rx_boosted_gain);
    if (err != RADIOLIB_ERR_NONE)
        LOG_WARN("SX126X setRxBoostedGainMode %s%d", radioLibErr, err);

    return RADIOLIB_ERR_NONE;
}

template <typename T> bool SX126xInterface<T>::reconfigure()
{
    RadioLibInterface::reconfigure();

    // set mode to standby - a chip that lost its state to a reset/brownout can time out here (-707),
    // so don't let setStandby()'s assert fire before the recovery below gets a chance
    int16_t err = trySetStandby();
    if (err == RADIOLIB_ERR_NONE)
        err = programModemParams();

    if (err != RADIOLIB_ERR_NONE) {
        // Chip likely lost its state (reset/brownout); recover in place rather than crash - see
        // RadioLibInterface::recoverChipStateLoss().
        RECORD_CRITICALERROR(meshtastic_CriticalErrorCode_INVALID_RADIO_SETTING);
        LOG_ERROR("SX126x rejected modem params, chip state lost? Full re-init");
        if (!reinitChip() || (err = programModemParams()) != RADIOLIB_ERR_NONE) {
            LOG_ERROR("SX126x unrecoverable %s%d, radio down until reboot", radioLibErr, err);
            return false;
        }
        LOG_INFO("SX126x recovered after re-init");
    }

    startReceive(); // restart receiving

    return true;
}

template <typename T> int16_t SX126xInterface<T>::getCurrentRSSI()
{
    float rssi = lora.getRSSI(false);
    return (int16_t)round(rssi);
}

template <typename T> void SX126xInterface<T>::setRadioIsr(void (*callback)())
{
#ifdef LORA_DIO1_SOFTWARE_POLL
    irqPollingActive = true;
    pollTxMode = isIsrTxCallback(callback);
    scheduleIrqPollTick();
#else
    lora.setDio1Action(callback);
#endif
}

template <typename T> void SX126xInterface<T>::clearRadioIsr()
{
#ifdef LORA_DIO1_SOFTWARE_POLL
    irqPollingActive = false;
#else
    lora.clearDio1Action();
#endif
}

#ifdef LORA_DIO1_SOFTWARE_POLL
template <typename T> void SX126xInterface<T>::handleSoftwareLoraIrqPoll()
{
    if (!irqPollingActive)
        return;

    // getIrqFlags()/clearIrqFlags() both operate on the raw SX126x IRQ register, so use the
    // chip-specific RADIOLIB_SX126X_IRQ_* masks on both the read and the clear.
    uint16_t irq = lora.getIrqFlags();
    const uint16_t rxEventMask =
        RADIOLIB_SX126X_IRQ_RX_DONE | RADIOLIB_SX126X_IRQ_TIMEOUT | RADIOLIB_SX126X_IRQ_CRC_ERR | RADIOLIB_SX126X_IRQ_HEADER_ERR;
    const uint16_t noisyRxMask = RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED | RADIOLIB_SX126X_IRQ_HEADER_VALID;

    // Do NOT treat a preamble/header-only IRQ as a full RX event: noisy preamble detections would
    // repeatedly trigger readData() and starve TX scheduling. Clear these non-terminal bits, or the
    // poll loop spins at high rate while they stay latched.
    if (!pollTxMode && (irq & noisyRxMask) && ((irq & ~noisyRxMask) == 0U)) {
        lora.clearIrqFlags(noisyRxMask);
        scheduleIrqPollTick();
        return;
    }

    if (pollTxMode) {
        if (irq & (RADIOLIB_SX126X_IRQ_TX_DONE | RADIOLIB_SX126X_IRQ_TIMEOUT)) {
            deliverPendingIrqFromPoll(ISR_TX);
            return;
        }
    } else if (irq & rxEventMask) {
        deliverPendingIrqFromPoll(ISR_RX);
        return;
    }

    scheduleIrqPollTick();
}
#endif

template <typename T> int16_t SX126xInterface<T>::trySetStandby()
{
    checkNotification(); // handle any pending interrupts before we force standby

    int16_t err = lora.standby();

    if (err != RADIOLIB_ERR_NONE)
        LOG_DEBUG("SX126x standby %s%d", radioLibErr, err);
#ifdef RADIO_TUNING_MENU
    if (err != RADIOLIB_ERR_NONE)
        captureChipDiag("standby-fail", radiotune::SnapKind::StandbyFail);
#endif
#ifdef ARCH_PORTDUINO
    if (err != RADIOLIB_ERR_NONE)
        portduino_status.LoRa_in_error = true;
#endif
    isReceiving = false; // If we were receiving, not any more
    activeReceiveStart = 0;
    disableInterrupt();
    completeSending(); // If we were sending, not anymore
    RadioLibInterface::setStandby();
    return err;
}

#ifdef SPECTRUM_ANALYZER
#ifndef SPECTRUM_SETTLE_US
#define SPECTRUM_SETTLE_US 1000 // PLL lock + RSSI averaging after SetRx, before GetRssiInst
#endif

template <typename T> bool SX126xInterface<T>::spectrumBegin()
{
    if (sendingPacket)
        return false; // let the packet in flight finish first
    disableInterrupt();
    isReceiving = false;
    activeReceiveStart = 0;
    setTransmitEnable(false);
    // XOSC standby keeps the TCXO running between steps, so a retune never waits on its start-up
    if (lora.standby(RADIOLIB_SX126X_STANDBY_XOSC) != RADIOLIB_ERR_NONE)
        return false;
    if (lora.setBandwidth(125.0) != RADIOLIB_ERR_NONE) // RX bandwidth = sweep step
        return false;
    // RadioLib drives RXEN only from its own mode calls; the raw SetRx in spectrumSample() is not
    // one, so select the receive path here or the sweep would measure through an idle switch
    module.setRfSwitchState(Module::MODE_RX);
    spectrumPaused = true;
    return true;
}

template <typename T> int16_t SX126xInterface<T>::spectrumSample(uint32_t hz)
{
    // Raw commands: RadioLib's setFrequency/startReceive add range checks, IRQ and packet setup
    // that a few hundred retunes per sweep do not need.
    const uint32_t frf = (uint32_t)(((uint64_t)hz << 25) / 32000000ULL);
    const uint8_t stby = RADIOLIB_SX126X_STANDBY_XOSC;
    const uint8_t freq[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
    const uint8_t rxInf[3] = {0xFF, 0xFF, 0xFF};
    if (module.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_STANDBY, &stby, 1) != RADIOLIB_ERR_NONE ||
        module.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_RF_FREQUENCY, freq, 4) != RADIOLIB_ERR_NONE)
        return INT16_MIN;
    // Same as RadioLib's launchMode(): receive path selected, then SetRx without status verify
    module.setRfSwitchState(Module::MODE_RX);
    if (module.SPIwriteStream(RADIOLIB_SX126X_CMD_SET_RX, rxInf, 3, true, false) != RADIOLIB_ERR_NONE)
        return INT16_MIN;
    delayMicroseconds(SPECTRUM_SETTLE_US);
    uint8_t raw = 0;
    if (module.SPIreadStream(RADIOLIB_SX126X_CMD_GET_RSSI_INST, &raw, 1) != RADIOLIB_ERR_NONE)
        return INT16_MIN;
    return -(int16_t)raw / 2;
}

template <typename T> void SX126xInterface<T>::spectrumProbe(uint32_t hz)
{
    // Once per sweep, for the log: chip mode right after a raw sample, then the same frequency
    // measured through RadioLib's own receive path. If only the second reads a real level, the
    // raw retune sequence is what is wrong.
    const int16_t rawDbm = spectrumSample(hz);
    uint8_t st[2] = {RADIOLIB_SX126X_CMD_GET_STATUS, RADIOLIB_SX126X_CMD_NOP};
    uint8_t in[2] = {0, 0};
    module.hal->spiBeginTransaction();
    module.hal->digitalWrite(diagCs, module.hal->GpioLevelLow);
    module.hal->spiTransfer(st, 2, in);
    module.hal->digitalWrite(diagCs, module.hal->GpioLevelHigh);
    module.hal->spiEndTransaction();

    lora.standby(RADIOLIB_SX126X_STANDBY_XOSC);
    float hl = -999;
    if (lora.setFrequency(hz / 1e6f, true) == RADIOLIB_ERR_NONE &&
        lora.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, MESHTASTIC_RADIOLIB_IRQ_RX_FLAGS) == RADIOLIB_ERR_NONE) {
        delay(5);
        hl = lora.getRSSI(false);
    }
    LOG_INFO("Spectrum probe %lu Hz: raw %d dBm (status 0x%02x %s), RadioLib %d dBm, rxen=%d", (unsigned long)hz, rawDbm, in[1],
             radiotune::chipModeName(in[1]), (int)hl, SX126X_RXEN != RADIOLIB_NC ? digitalRead(SX126X_RXEN) : -1);
    lora.standby(RADIOLIB_SX126X_STANDBY_XOSC);
    module.setRfSwitchState(Module::MODE_RX);
}

template <typename T> bool SX126xInterface<T>::spectrumTestTxStart(int8_t outDbm)
{
    // Antenna power -> chip power through the board's PA gain, the same table limitPower() uses
    static const uint16_t gain[NUM_PA_POINTS] = {TX_GAIN_LORA};
    int chip = outDbm - (int)gain[NUM_PA_POINTS - 1];
    if (chip > SX126X_MAX_POWER)
        chip = SX126X_MAX_POWER;
    if (chip < -9)
        chip = -9;

    lora.standby(RADIOLIB_SX126X_STANDBY_XOSC);
    // On the mesh channel, short (SF7) and on a private sync word: other nodes see energy, not packets
    if (lora.setFrequency(getFreq()) != RADIOLIB_ERR_NONE || lora.setSpreadingFactor(7) != RADIOLIB_ERR_NONE ||
        lora.setSyncWord(0x12) != RADIOLIB_ERR_NONE)
        return false;
#ifdef SX126X_NO_POWER_OPTIMIZATION_TABLE
    int16_t err = lora.setOutputPower(chip, false);
#else
    int16_t err = lora.setOutputPower(chip);
#endif
    if (err != RADIOLIB_ERR_NONE)
        return false;
    setTransmitEnable(true);
    static const uint8_t payload[16] = {'S', 'P', 'E', 'C', 'T', 'R', 'U', 'M', ' ', 'T', 'E', 'S', 'T'};
    return lora.startTransmit(payload, sizeof(payload)) == RADIOLIB_ERR_NONE;
}

template <typename T> bool SX126xInterface<T>::spectrumTestTxDone()
{
    if (!(lora.getIrqFlags() & RADIOLIB_SX126X_IRQ_TX_DONE))
        return false;
    lora.finishTransmit();
    setTransmitEnable(false);
    lora.standby(RADIOLIB_SX126X_STANDBY_XOSC);
    module.setRfSwitchState(Module::MODE_RX); // back to the receive path for the sweep
    return true;
}
#endif

#ifdef RADIO_TUNING_MENU
template <typename T> void SX126xInterface<T>::captureChipDiag(const char *where, radiotune::SnapKind kind)
{
    radiotune::ChipSnapshot s;
    s.atMs = Time::skipZero(Time::getMillis());
    s.where = where;
    RadioLibHal *hal = module.hal;
    s.busyHigh = diagBusy != RADIOLIB_NC && hal->digitalRead(diagBusy);
#if defined(SX126X_RXEN)
    if (SX126X_RXEN != RADIOLIB_NC)
        s.rxen = digitalRead(SX126X_RXEN);
#endif

    // Raw GetStatus, bypassing RadioLib's BUSY wait: the chip answers it even with BUSY high, and a
    // stuck BUSY is exactly the case worth seeing. 0x00/0xFF means nothing drove MISO (asleep / dead).
    if (diagCs != RADIOLIB_NC) {
        uint8_t out[2] = {RADIOLIB_SX126X_CMD_GET_STATUS, RADIOLIB_SX126X_CMD_NOP};
        uint8_t in[2] = {0, 0};
        hal->spiBeginTransaction();
        hal->digitalWrite(diagCs, hal->GpioLevelLow);
        hal->spiTransfer(out, sizeof(out), in);
        hal->digitalWrite(diagCs, hal->GpioLevelHigh);
        hal->spiEndTransaction();
        s.status = in[1];
        s.answered = s.status != 0x00 && s.status != 0xFF;
    }

    // Everything else needs BUSY low; skip it rather than burn the BUSY timeout on a stuck chip
    if (!s.busyHigh && s.answered) {
        uint8_t e[2] = {0, 0};
        if (module.SPIreadStream(RADIOLIB_SX126X_CMD_GET_DEVICE_ERRORS, e, 2, true, false) == RADIOLIB_ERR_NONE) {
            s.errors = ((uint16_t)e[0] << 8) | e[1];
            module.SPIreadRegisterBurst(RADIOLIB_SX126X_REG_OCP_CONFIGURATION, 1, &s.ocp);
            module.SPIreadRegisterBurst(RADIOLIB_SX126X_REG_TX_CLAMP_CONFIG, 1, &s.txClamp);
            module.SPIreadRegisterBurst(RADIOLIB_SX126X_REG_RX_GAIN, 1, &s.rxGain);
            s.regsValid = true;
            if (s.errors) {
                // Latched until cleared - clear so the next snapshot shows only new errors
                const uint8_t zero[2] = {RADIOLIB_SX126X_CMD_NOP, RADIOLIB_SX126X_CMD_NOP};
                module.SPIwriteStream(RADIOLIB_SX126X_CMD_CLEAR_DEVICE_ERRORS, zero, 2, true, false);
            }
        }
    }

    radiotune::recordSnapshot(s, kind);

    if ((kind == radiotune::SnapKind::TxDone || kind == radiotune::SnapKind::Boot) && !s.busyHigh && !s.errors) {
        LOG_DEBUG("SX126x diag %s: status=0x%02x %s err=0x%04x ocp=0x%02x", where, s.status, radiotune::chipModeName(s.status),
                  s.errors, s.ocp);
        return;
    }
    LOG_WARN("SX126x diag %s: busy=%d status=0x%02x (%s, cmd=%u) err=0x%04x%s%s%s ocp=0x%02x clamp=0x%02x rxgain=0x%02x "
             "rxen=%d",
             where, s.busyHigh, s.status, s.answered ? radiotune::chipModeName(s.status) : "no answer", (s.status >> 1) & 0x07,
             s.errors, (s.errors & RADIOLIB_SX126X_XOSC_START_ERR) ? " XOSC_START" : "",
             (s.errors & RADIOLIB_SX126X_PA_RAMP_ERR) ? " PA_RAMP" : "", (s.errors & RADIOLIB_SX126X_PLL_LOCK_ERR) ? " PLL_LOCK" : "",
             s.ocp, s.txClamp, s.rxGain, s.rxen);
}
#endif

template <typename T> void SX126xInterface<T>::setStandby()
{
    int16_t err = trySetStandby();
#ifdef ARCH_PORTDUINO
    (void)err;
#else
    assert(err == RADIOLIB_ERR_NONE);
#endif
}

/**
 * Add SNR data to received messages
 */
template <typename T> void SX126xInterface<T>::addReceiveMetadata(meshtastic_MeshPacket *mp)
{
    // LOG_DEBUG("PacketStatus %x", lora.getPacketStatus());
    mp->rx_snr = lora.getSNR();
    mp->rx_rssi = lround(lora.getRSSI());
    mp->has_rx_rssi = true; // rx_rssi has explicit presence - a genuine reading must be marked present to survive encoding
    LOG_TRACE("Corrected frequency offset: %f", lora.getFrequencyError());
}

/** We override to turn on transmitter power as needed.
 */
template <typename T> void SX126xInterface<T>::configHardwareForSend()
{
    setTransmitEnable(true);
    RadioLibInterface::configHardwareForSend();
}

// For power draw measurements, helpful to force radio to stay sleeping
// #define SLEEP_ONLY

template <typename T> void SX126xInterface<T>::startReceive()
{
#ifdef SLEEP_ONLY
    sleep();
#else

#ifdef SPECTRUM_ANALYZER
    if (spectrumPaused)
        return; // the analyzer owns the chip; spectrumEnd() re-arms RX through reconfigure()
#endif
    setTransmitEnable(false);

#ifdef ARCH_PORTDUINO_WASM
    // Continuous RX in the browser: duty-cycle sleep parks BUSY high between RX
    // windows and stalls the slow WebUSB SPI link. No battery to save here.
    constexpr bool continuous = true;
#elif defined(RADIO_TUNING_MENU)
    const bool continuous = radiotune::continuousRx(); // LoRa Actions > Radio Tuning > Receive
#else
    constexpr bool continuous = false;
#endif
    const char *rxMethod = continuous ? "startReceive" : "startReceiveDutyCycleAuto";
    auto tryStartRx = [&]() -> int16_t {
        if (continuous)
            return lora.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, MESHTASTIC_RADIOLIB_IRQ_RX_FLAGS);
        // We use a 16 bit preamble so this should save some power by letting radio sit in standby mostly.
        return lora.startReceiveDutyCycleAuto(preambleLength, 8, MESHTASTIC_RADIOLIB_IRQ_RX_FLAGS);
    };

    int16_t err = trySetStandby();
    if (err == RADIOLIB_ERR_NONE)
        err = tryStartRx();

    if (err != RADIOLIB_ERR_NONE) {
        LOG_ERROR("SX126X %s %s%d", rxMethod, radioLibErr, err);
        if (maybeRecoverChipStateLoss())
            err = tryStartRx();
    }

    if (err != RADIOLIB_ERR_NONE) {
#ifdef ARCH_PORTDUINO
        portduino_status.LoRa_in_error = true;
#else
        // No assert: leave RX off rather than reboot; periodicRadioMaintenance() re-arms it, throttled
        LOG_ERROR("SX126X RX offline %s%d", radioLibErr, err);
        rxOffline = true;
        return;
#endif
    }

    RadioLibInterface::startReceive();

    // Must be done AFTER, starting transmit, because startTransmit clears (possibly stale) interrupt pending register bits
    enableInterrupt(isrRxLevel0);
    checkRxDoneIrqFlag();
#endif
}

/** Is the channel currently active? */
template <typename T> bool SX126xInterface<T>::isChannelActive()
{
    // check if we can detect a LoRa preamble on the current channel
    ChannelScanConfig_t cfg = {.cad = {.symNum = NUM_SYM_CAD,
                                       .detPeak = RADIOLIB_SX126X_CAD_PARAM_DEFAULT,
                                       .detMin = RADIOLIB_SX126X_CAD_PARAM_DEFAULT,
                                       .exitMode = RADIOLIB_SX126X_CAD_PARAM_DEFAULT,
                                       .timeout = 0,
                                       .irqFlags = RADIOLIB_IRQ_CAD_DEFAULT_FLAGS,
                                       .irqMask = RADIOLIB_IRQ_CAD_DEFAULT_MASK}};
    setTransmitEnable(false);
    int16_t result = trySetStandby();
    if (result == RADIOLIB_ERR_NONE) {
        result = lora.scanChannel(cfg);
        if (result == RADIOLIB_LORA_DETECTED)
            return true;
        if (result != RADIOLIB_CHANNEL_FREE)
            LOG_ERROR("SX126X scanChannel %s%d", radioLibErr, result);
        if (result != RADIOLIB_ERR_WRONG_MODEM)
            return false;
    }
#ifdef ARCH_PORTDUINO
    portduino_status.LoRa_in_error = true;
#endif
    // standby failed or the LoRa modem type is gone - the chip lost its runtime state
    maybeRecoverChipStateLoss();
    return false; // report the channel free: a recovered chip can TX, a dead one fails startSend safely
}

/** Could we send right now (i.e. either not actively receiving or transmitting)? */
template <typename T> bool SX126xInterface<T>::isActivelyReceiving()
{
    // The IRQ status will be cleared when we start our read operation. Check if we've started a header, but haven't yet
    // received and handled the interrupt for reading the packet/handling errors.
    return receiveDetected(lora.getIrqFlags(), RADIOLIB_SX126X_IRQ_HEADER_VALID, RADIOLIB_SX126X_IRQ_PREAMBLE_DETECTED);
}

template <typename T> bool SX126xInterface<T>::sleep()
{
    // Not keeping config is busted - next time nrf52 board boots lora sending fails  tcxo related? - see datasheet
    // \todo Display actual typename of the adapter, not just `SX126x`
    LOG_DEBUG("SX126x entering sleep mode"); // (FIXME, don't keep config)
    (void)trySetStandby(); // Stop any pending operations - the chip is being put to sleep, a failure must not crash

    // turn off TCXO if it was powered
    // FIXME - this isn't correct
    // lora.setTCXO(0);

    // put chipset into sleep mode (we've already disabled interrupts by now)
    bool keepConfig = true;
    lora.sleep(keepConfig); // Note: we do not keep the config, full reinit will be needed

#ifdef SX126X_POWER_EN
    digitalWrite(SX126X_POWER_EN, LOW);
#endif

#if HAS_LORA_FEM
    loraFEMInterface.setSleepModeEnable();
#endif

    return true;
}

template <typename T> bool SX126xInterface<T>::resetAGC()
{
    // Safety: don't reset mid-packet
    if (sendingPacket != NULL || (isReceiving && isActivelyReceiving()))
        return false;

    LOG_DEBUG("SX126x AGC reset: warm sleep + Calibrate(0x7F)");

    // 1. Warm sleep - powers down the entire analog frontend, resetting AGC state.
    //    A plain standby→startReceive cycle does NOT reset the AGC.
    lora.sleep(true);

    // 2. Wake to RC standby for stable calibration
    lora.standby(RADIOLIB_SX126X_STANDBY_RC, true);

    // 3. Calibrate all blocks (ADC, PLL, image, RC oscillators)
    uint8_t calData = RADIOLIB_SX126X_CALIBRATE_ALL;
    module.SPIwriteStream(RADIOLIB_SX126X_CMD_CALIBRATE, &calData, 1, true, false);

    // 4. Wait for calibration to complete (BUSY pin goes low)
    module.hal->delay(5);
    uint32_t start = millis();
    while (module.hal->digitalRead(module.getGpio())) {
        if (millis() - start > 50)
            break;
        module.hal->yield();
    }

    if (module.hal->digitalRead(module.getGpio())) {
        LOG_WARN("SX126x AGC reset: calibration not done in 50ms");
        startReceive();
        return false; // incomplete: retried on the next maintenance tick
    }

    // 5. Re-calibrate image rejection for actual operating frequency
    //    Calibrate(0x7F) defaults to 902-928 MHz which is wrong for other regions.
    lora.calibrateImage(getFreq());
    // 6. CalibrateImage keeps working internally after it returns, and BUSY does not
    //    stay asserted for it; a register write in that window fails write-verify
    //    (RADIOLIB_ERR_SPI_WRITE_FAILED) and stalls the chip.
    module.hal->delay(50);

    // Re-apply settings that calibration may have reset

    // DIO2 as RF switch
#ifdef SX126X_DIO2_AS_RF_SWITCH
    lora.setDio2AsRfSwitch(true);
#elif defined(ARCH_PORTDUINO)
    if (portduino_config.dio2_as_rf_switch)
        lora.setDio2AsRfSwitch(true);
#endif

    // RX boosted gain mode
    lora.setRxBoostedGainMode(config.lora.sx126x_rx_boosted_gain);

    // Re-apply the undocumented 0x8B5 RX sensitivity patch that was set in init().
    // The CALIBRATE_ALL (0x7F) command above clears bit 0 of register 0x8B5, which
    // silently removes the RX sensitivity improvement introduced in #9571 / #9777.
    // Without this re-apply, every SX1262 node loses its RX boost ~60s after boot
    // and never recovers until reboot. See empirical evidence in the PR description.
    const bool patched = module.SPIsetRegValue(0x8B5, 0x01, 0, 0) == RADIOLIB_ERR_NONE;
    if (!patched) {
        LOG_WARN("SX126x resetAGC: 0x8B5 RX patch re-apply failed");
    }

    // 7. Resume receiving
    startReceive();
    return patched; // without the patch the reset is incomplete: retried on the next maintenance tick
}

/** Control PA mode for GC1109 FEM - CPS pin selects full PA (txon=true) or bypass mode (txon=false) */
template <typename T> void SX126xInterface<T>::setTransmitEnable(bool txon)
{
#if HAS_LORA_FEM
    if (txon) {
        loraFEMInterface.setTxModeEnable();
    } else {
        loraFEMInterface.setRxModeEnable();
    }
#endif
}

#endif
