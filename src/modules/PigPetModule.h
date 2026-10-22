#pragma once

#include "configuration.h"

#if HAS_SCREEN && defined(PIG_PET)

#include "Observer.h"
#include "concurrency/OSThread.h"
#include "input/InputBroker.h"
#include "mesh/MeshModule.h"

/**
 * A pet pig on its own BaseUI frame (128x64 mono). It gets hungry over time, cheers up when the
 * mesh is busy, gets fitter as the node is carried around (GPS distance), sleeps at night, and
 * plays fetch: a telemetry request or traceroute to a node it has heard, celebrating when the
 * answer comes back. A short press still moves to the next frame; a long press on this frame
 * opens the Pig menu. State is kept in /prefs/pigpet.
 *
 * Inspired by the Piglet wardriver's walking pig; the artwork here is drawn from scratch.
 */
class PigPetModule : public MeshModule, public Observable<const UIFrameEvent *>, private concurrency::OSThread
{
  public:
    PigPetModule();

    bool wantUIFrame() override { return true; }
    Observable<const UIFrameEvent *> *getUIFrameObservable() override { return this; }
    void drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y) override;

  protected:
    bool wantPacket(const meshtastic_MeshPacket *p) override;
    ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    int32_t runOnce() override;

  private:
    enum class Pose : uint8_t { Walk, Dance, Twerk, Eat, Sad, Sleep };
    enum class SleepMode : uint8_t { Auto, Asleep, Awake };
    enum class Fetch : uint8_t { None, Telemetry, TraceRoute };

    // Persisted. Stats are 0..100.
    struct State {
        uint32_t magic;
        uint8_t version;
        uint8_t food, joy, energy;
        uint8_t fitness;
        uint8_t sleepMode;
        uint8_t reserved[2];
        uint32_t walkedM; // lifetime metres carried
        uint32_t fetches; // lifetime successful fetches
        char name[16];
        uint32_t crc;
    } st{};

    void load();
    void save();
    static uint8_t clampAdd(uint8_t v, int d);

    // Behaviour
    void minuteTick();
    void walkTick();
    bool isAsleep() const;
    Pose currentPose() const;
    const char *moodWord() const;
    void celebrate(Pose p, uint32_t ms);

    // Pig menu
    int handleInputEvent(const InputEvent *event);
    CallbackObserver<PigPetModule, const InputEvent *> inputObserver =
        CallbackObserver<PigPetModule, const InputEvent *>(this, &PigPetModule::handleInputEvent);
    void openMenu();
    void feed();
    void startFetch(Fetch kind);
    void showStats();
    bool pendingRename = false;

    // Fetch in flight
    Fetch fetch = Fetch::None;
    NodeNum fetchTarget = 0;
    uint32_t fetchStartMs = 0;
    uint32_t fetchRouteAtMs = 0; // traceroute result stamp before we threw the ball
    uint32_t lastFetchMs = 0;

    // Timers and animation
    uint32_t lastMinuteMs = 0;
    uint32_t minutes = 0;
    uint32_t lastWalkCheckMs = 0;
    int32_t lastLat = 0, lastLon = 0;
    bool haveLastFix = false;
    uint32_t walkCarryM = 0;
    uint32_t lastMeshJoyMs = 0;
    uint32_t knownNodes = 0;
    uint32_t dirtySinceMs = 0;
    bool dirty = false;
    uint32_t lastHappyTwerkMs = 0;

    Pose celebratePose = Pose::Walk;
    uint32_t celebrateUntilMs = 0;
    uint8_t phase = 0;
    int16_t pigX = 4;
    int8_t pigDir = 1;
    uint8_t subStep = 0;
    uint32_t lastFrameMs = 0;
    char status[40] = ""; // one-line message shown over the name for a few seconds
    uint32_t statusUntilMs = 0;
    void setStatus(const char *s, uint32_t ms = 4000);
};

extern PigPetModule *pigPetModule;

#endif
