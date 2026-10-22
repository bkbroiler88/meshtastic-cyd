#include "PigPetModule.h"

#if HAS_SCREEN && defined(PIG_PET)

#include "FSCommon.h"
#include "GPSStatus.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "concurrency/LockGuard.h"
#include "gps/RTC.h"
#include "graphics/Screen.h"
#include "graphics/ScreenFonts.h"
#include "main.h"
#include "mesh/NodeDB.h"
#include "modules/Telemetry/DeviceTelemetry.h"
#include "modules/TraceRouteModule.h"
#include <ErriezCRC32.h>
#include <cmath>
#include <cstddef>
#include <cstring>

PigPetModule *pigPetModule;

namespace
{
constexpr const char *kPath = "/prefs/pigpet";
constexpr uint32_t kMagic = 0x50494731; // "PIG1"
constexpr uint8_t kVersion = 1;

constexpr uint32_t kFrameMs = 125;                 // ~8 fps while the frame is showing
constexpr uint32_t kFetchCooldownMs = 5 * 60000UL; // fetch sends real packets: at most one per 5 min
constexpr uint32_t kTelemetryWaitMs = 60000;
constexpr uint32_t kTraceWaitMs = 90000;
constexpr uint32_t kSaveAfterMs = 5 * 60000UL;     // batch passive changes, spare the flash
constexpr uint32_t kHeardWithinS = 2 * 3600;       // a playmate must have been heard this recently

constexpr int16_t kPigW = 47; // box including the snout and the trough
constexpr int16_t kPigTop = 12;

// --- drawing --------------------------------------------------------------------------
// The pig is ~44x30 px, built from circles, rectangles and single pixels, facing right when
// facing > 0. Every x goes through X() so the same code draws it facing left.
struct Pen {
    OLEDDisplay *d;
    int16_t x, y;
    int8_t facing;
    int16_t X(int dx) const { return facing > 0 ? (int16_t)(x + dx) : (int16_t)(x + 43 - dx); }
    void rect(int dx, int16_t yy, int16_t w, int16_t h) const
    {
        const int16_t a = X(dx), b = X(dx + w - 1);
        d->fillRect(a < b ? a : b, yy, w, h);
    }
    void white() const { d->setColor(WHITE); }
    void black() const { d->setColor(BLACK); }
};

/// The dark ring that separates the head from the body, only on the body side.
void headRing(const Pen &p, int16_t hx, int16_t hy)
{
    p.black();
    for (int16_t yy = hy - 10; yy <= hy + 10; yy++)
        for (int16_t xx = hx - 10; xx <= hx + 10; xx++) {
            const int d = (xx - hx) * (xx - hx) + (yy - hy) * (yy - hy);
            if (d >= 72 && d <= 90 && (xx - hx) * p.facing <= 1)
                p.d->setPixel(xx, yy);
        }
    p.white();
    p.d->fillCircle(hx, hy, 8);
}

void snout(const Pen &p, int16_t hy)
{
    const int16_t a = p.X(38), b = p.X(44);
    const int16_t lo = a < b ? a : b;
    p.black();
    p.d->fillRect(lo - 1, hy - 2, 9, 9);
    p.white();
    p.d->fillRect(lo, hy - 1, 7, 7);
    p.black();
    p.d->setPixel(lo, hy - 1);
    p.d->setPixel(lo + 6, hy - 1);
    p.d->setPixel(lo, hy + 5);
    p.d->setPixel(lo + 6, hy + 5);
    p.d->fillRect(p.X(40), hy + 1, 1, 2); // nostrils
    p.d->fillRect(p.X(42), hy + 1, 1, 2);
    p.white();
}

void happyFace(const Pen &p, int16_t hy)
{
    const int16_t ex = p.X(34);
    p.black();
    p.d->setPixel(ex - 1, hy - 2); // ^ eye
    p.d->setPixel(ex, hy - 3);
    p.d->setPixel(ex + 1, hy - 2);
    p.d->setPixel(ex - 1, hy - 6); // raised brow
    p.d->setPixel(ex, hy - 7);
    p.d->setPixel(ex + 1, hy - 6);
    const int16_t m0 = p.X(36), m1 = p.X(38);
    p.d->drawLine(m0 < m1 ? m0 : m1, hy + 4, m0 < m1 ? m1 : m0, hy + 4);
    p.white();
}

void drawTwerk(const Pen &p, uint8_t phase)
{
    const int shake = (phase & 1) ? 2 : -2;
    const int16_t by = p.y + 14;
    p.white();
    // Back legs bent and shaking with the hips, front legs straight
    static const int8_t legX[] = {6, 12, 25, 30};
    for (uint8_t i = 0; i < 4; i++) {
        const int16_t ly = by + 6 + (i >= 2 ? 0 : -2);
        p.rect(legX[i] + (i < 2 ? shake : 0), ly, 4, i >= 2 ? 9 : 10);
    }
    // Body tilted: rump up and shaking, chest low
    p.d->fillCircle(p.X(11 + shake), by - 3, 10);
    p.d->fillCircle(p.X(20), by - 1, 9);
    p.d->fillCircle(p.X(27), by + 2, 7);
    // Tail flicks against the hips
    static const int8_t tail[][2] = {{0, -8}, {-1, -9}, {-2, -9}, {-3, -8}, {-3, -7}, {-2, -6}};
    const int16_t tx = p.X(1 + shake);
    const int flip = (phase & 1) ? 1 : -1;
    for (const auto &t : tail)
        p.d->setPixel(tx + (p.facing > 0 ? t[0] : -t[0]) * flip, by + t[1]);
    const int16_t hy = by + 4;
    headRing(p, p.X(33), hy);
    snout(p, hy);
    happyFace(p, hy);
    // Motion ticks behind the rump
    for (int k = 0; k < 2; k++) {
        p.d->setPixel(p.X(-3 + shake), by - 6 + k * 3);
        p.d->setPixel(p.X(-4 + shake), by - 6 + k * 3);
    }
}

/// pose: 0 walk, 1 dance, 2 twerk, 3 eat, 4 sad, 5 sleep (PigPetModule::Pose order)
void drawPig(OLEDDisplay *d, int16_t x, int16_t y, uint8_t pose, uint8_t phase, int8_t facing)
{
    const Pen p{d, x, y, facing};
    if (pose == 2) {
        drawTwerk(p, phase);
        return;
    }
    const bool lie = pose == 5, dance = pose == 1, eat = pose == 3, sad = pose == 4, walk = pose == 0;
    const int16_t by = y + 12 + ((dance && (phase & 1)) ? -2 : 0) + (lie ? 4 : 0);
    p.white();

    if (!lie) {
        static const int8_t legX[] = {8, 13, 24, 29};
        uint8_t lift[4] = {0, 0, 0, 0};
        if (walk || sad) {
            const bool a = phase & 1;
            lift[0] = lift[3] = a ? 2 : 0;
            lift[1] = lift[2] = a ? 0 : 2;
        } else if (dance) {
            const bool a = phase & 1;
            lift[0] = lift[2] = a ? 3 : 0;
            lift[1] = lift[3] = a ? 0 : 3;
        }
        for (uint8_t i = 0; i < 4; i++) {
            const int16_t ly = by + 7 - lift[i];
            const int16_t h = 9 - (lift[i] ? 1 : 0);
            p.rect(legX[i], ly, 4, h);
            p.black();
            d->setPixel(p.X(legX[i] + 2), ly + h - 1); // hoof notch
            p.white();
        }
    }

    // Body: a wide egg
    d->fillCircle(p.X(12), by, 9);
    d->fillCircle(p.X(22), by - 1, 10);
    d->fillCircle(p.X(27), by + 1, 8);
    p.rect(12, by - 9, 12, 19);

    // Tail curl on the rump
    static const int8_t tail[][2] = {{0, -3}, {-1, -4}, {-2, -4}, {-3, -3}, {-3, -2}, {-2, -1}, {-1, -2}};
    const int16_t tx = p.X(3);
    for (const auto &t : tail)
        d->setPixel(tx + (facing > 0 ? t[0] : -t[0]), by - 2 + t[1]);
    d->setPixel(p.X(4), by - 3);

    const int16_t hx = p.X(33);
    const int16_t hy = by - 5 + (sad ? 2 : 0) + (lie ? 3 : 0) + (eat ? 4 : 0);
    headRing(p, hx, hy);
    snout(p, hy);

    // Eye and brow carry the mood
    const int16_t ex = p.X(34);
    p.black();
    if (lie) {
        d->drawLine(ex - 1, hy - 2, ex + 1, hy - 2);
        d->drawLine(ex - 1, hy - 5, ex + 1, hy - 5);
    } else if (dance) {
        p.white();
        happyFace(p, hy);
        p.black();
    } else if (sad) {
        d->fillRect(ex - 1, hy - 2, 2, 2);
        d->drawLine(p.X(32), hy - 5, p.X(35), hy - 4);
        d->setPixel(p.X(37), hy + 5);
    } else {
        const int16_t e2 = p.X(35);
        d->fillRect(ex < e2 ? ex : e2, hy - 3, 2, 3);
        const int16_t b0 = p.X(33), b1 = p.X(35);
        d->drawLine(b0 < b1 ? b0 : b1, hy - 5, b0 < b1 ? b1 : b0, hy - 5);
    }
    if (walk || eat) {
        const int16_t m0 = p.X(36), m1 = p.X(38);
        d->drawLine(m0 < m1 ? m0 : m1, hy + 4, m0 < m1 ? m1 : m0, hy + 4);
    }
    p.white();

    if (eat) {
        const int16_t t0 = p.X(33), t1 = p.X(46);
        const int16_t lo = t0 < t1 ? t0 : t1, hi = t0 < t1 ? t1 : t0;
        d->fillRect(lo, y + 26, 14, 2);
        d->setPixel(lo, y + 25);
        d->setPixel(hi, y + 25);
        if (phase & 1) { // crumbs flying
            d->setPixel(p.X(40), y + 23);
            d->setPixel(p.X(43), y + 24);
        }
    }
}

void drawZ(OLEDDisplay *d, int16_t x, int16_t y)
{
    d->drawLine(x, y, x + 3, y);
    d->drawLine(x + 3, y, x, y + 3);
    d->drawLine(x, y + 3, x + 3, y + 3);
}

void drawNote(OLEDDisplay *d, int16_t x, int16_t y)
{
    d->fillRect(x, y + 4, 2, 2);
    d->drawLine(x + 1, y, x + 1, y + 4);
    d->setPixel(x + 2, y);
}

const char *nodeShortName(NodeNum n)
{
    const meshtastic_NodeInfoLite *info = nodeDB ? nodeDB->getMeshNode(n) : nullptr;
    if (info && info->short_name[0])
        return info->short_name;
    static char hex[8];
    snprintf(hex, sizeof(hex), "%04x", (unsigned)(n & 0xFFFF));
    return hex;
}
} // namespace

PigPetModule::PigPetModule() : MeshModule("pigpet"), concurrency::OSThread("PigPet")
{
    isPromiscuous = true; // mesh mood: every packet heard counts, not only ones to us
    load();
    inputObserver.observe(inputBroker);
    lastMinuteMs = lastWalkCheckMs = millis();
    setIntervalFromNow(1000);
}

uint8_t PigPetModule::clampAdd(uint8_t v, int d)
{
    const int r = (int)v + d;
    return (uint8_t)(r < 0 ? 0 : r > 100 ? 100 : r);
}

void PigPetModule::setStatus(const char *s, uint32_t ms)
{
    strncpy(status, s, sizeof(status) - 1);
    status[sizeof(status) - 1] = '\0';
    statusUntilMs = millis() + ms;
}

void PigPetModule::celebrate(Pose p, uint32_t ms)
{
    celebratePose = p;
    celebrateUntilMs = millis() + ms;
}

// --- persistence ------------------------------------------------------------------------

void PigPetModule::load()
{
    memset(&st, 0, sizeof(st));
    bool ok = false;
#ifdef FSCom
    {
        concurrency::LockGuard g(spiLock);
        auto f = FSCom.open(kPath, FILE_O_READ);
        if (f) {
            ok = f.read(reinterpret_cast<uint8_t *>(&st), sizeof(st)) == sizeof(st);
            f.close();
        }
    }
#endif
    if (!ok || st.magic != kMagic || st.version != kVersion || crc32Buffer(&st, offsetof(State, crc)) != st.crc) {
        memset(&st, 0, sizeof(st));
        st.magic = kMagic;
        st.version = kVersion;
        st.food = 80;
        st.joy = 70;
        st.energy = 90;
        st.fitness = 10;
        st.sleepMode = (uint8_t)SleepMode::Auto;
        strncpy(st.name, "Piglet", sizeof(st.name) - 1);
        LOG_INFO("PigPet: a new pig is born");
        return;
    }
    st.name[sizeof(st.name) - 1] = '\0';
    LOG_INFO("PigPet: %s is back (food %u joy %u energy %u)", st.name, st.food, st.joy, st.energy);
}

void PigPetModule::save()
{
    dirty = false;
#ifdef FSCom
    {
        concurrency::LockGuard g(spiLock);
        FSCom.mkdir("/prefs");
    }
    st.crc = crc32Buffer(&st, offsetof(State, crc));
    auto sf = SafeFile(kPath, true);
    const size_t written = sf.write(reinterpret_cast<uint8_t *>(&st), sizeof(st));
    if (!sf.close() || written != sizeof(st))
        LOG_WARN("PigPet: save failed");
#endif
}

// --- behaviour --------------------------------------------------------------------------

bool PigPetModule::isAsleep() const
{
    switch ((SleepMode)st.sleepMode) {
    case SleepMode::Asleep:
        return true;
    case SleepMode::Awake:
        return false;
    default: {
        // Night by the local clock; without a valid time he simply stays up
        const uint32_t t = getValidTime(RTCQualityDevice, true);
        if (!t)
            return false;
        const uint32_t hour = (t % 86400) / 3600;
        return hour >= 22 || hour < 7;
    }
    }
}

PigPetModule::Pose PigPetModule::currentPose() const
{
    if ((int32_t)(celebrateUntilMs - millis()) > 0)
        return celebratePose;
    if (isAsleep())
        return Pose::Sleep;
    if (st.food < 20 || st.joy < 15 || st.energy < 10)
        return Pose::Sad;
    return Pose::Walk;
}

const char *PigPetModule::moodWord() const
{
    if (fetch != Fetch::None)
        return "fetching!";
    if (isAsleep())
        return "asleep";
    if (st.food < 20)
        return "hungry";
    if (st.energy < 15)
        return "tired";
    if (st.joy < 20)
        return "lonely";
    if (st.joy > 80)
        return "happy";
    return "content";
}

void PigPetModule::minuteTick()
{
    minutes++;
    const bool asleep = isAsleep();
    if (asleep) {
        if (minutes % 2 == 0)
            st.energy = clampAdd(st.energy, 1);
        if (minutes % 30 == 0)
            st.food = clampAdd(st.food, -1);
    } else {
        if (minutes % 15 == 0)
            st.food = clampAdd(st.food, -1);
        if (minutes % 20 == 0)
            st.joy = clampAdd(st.joy, -1);
        if (minutes % 12 == 0)
            st.energy = clampAdd(st.energy, -1);
    }

    // A manual sleep or wake lasts until the next morning or bedtime, then Auto takes over
    const uint32_t t = getValidTime(RTCQualityDevice, true);
    if (t) {
        const uint32_t hour = (t % 86400) / 3600, min = (t % 3600) / 60;
        if (min == 0 && ((hour == 7 && st.sleepMode == (uint8_t)SleepMode::Asleep) ||
                         (hour == 22 && st.sleepMode == (uint8_t)SleepMode::Awake)))
            st.sleepMode = (uint8_t)SleepMode::Auto;
    }

    // A node we have never heard before is a new friend
    if (nodeDB) {
        const uint32_t n = nodeDB->getNumMeshNodes();
        if (knownNodes && n > knownNodes) {
            st.joy = clampAdd(st.joy, 3);
            setStatus("A new friend nearby!");
        }
        knownNodes = n;
    }
    if (!dirty) {
        dirty = true;
        dirtySinceMs = millis();
    }
}

void PigPetModule::walkTick()
{
    if (!gpsStatus || !gpsStatus->getHasLock())
        return;
    const int32_t lat = gpsStatus->getLatitude(), lon = gpsStatus->getLongitude();
    if (!haveLastFix) {
        lastLat = lat;
        lastLon = lon;
        haveLastFix = true;
        return;
    }
    const double dLat = (lat - lastLat) * 1e-7 * 111132.0;
    const double dLon = (lon - lastLon) * 1e-7 * 111320.0 * cos(lat * 1e-7 * M_PI / 180.0);
    const double d = sqrt(dLat * dLat + dLon * dLon);
    if (d < 15.0)
        return; // GPS jitter; keep the reference so a slow walk still adds up
    lastLat = lat;
    lastLon = lon;
    if (d > 2000.0)
        return; // a jump (first fix after a gap, or driving): not exercise
    st.walkedM += (uint32_t)d;
    walkCarryM += (uint32_t)d;
    while (walkCarryM >= 100) { // every 100 m carried
        walkCarryM -= 100;
        st.fitness = clampAdd(st.fitness, 1);
        st.joy = clampAdd(st.joy, 2);
        st.food = clampAdd(st.food, -2); // walking makes him hungry
        st.energy = clampAdd(st.energy, -1);
    }
}

// --- mesh -------------------------------------------------------------------------------

bool PigPetModule::wantPacket(const meshtastic_MeshPacket *p)
{
    return p->which_payload_variant == meshtastic_MeshPacket_decoded_tag && nodeDB && p->from != nodeDB->getNodeNum();
}

ProcessMessage PigPetModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    const uint32_t now = millis();
    if (fetch == Fetch::Telemetry && mp.from == fetchTarget && mp.decoded.portnum == meshtastic_PortNum_TELEMETRY_APP) {
        fetch = Fetch::None;
        st.joy = clampAdd(st.joy, 15);
        st.fetches++;
        char s[40];
        snprintf(s, sizeof(s), "%s threw it back!", nodeShortName(mp.from));
        setStatus(s);
        celebrate(Pose::Twerk, 3000);
        save();
    } else if (mp.decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP && !isAsleep()) {
        st.joy = clampAdd(st.joy, 3);
        setStatus("Ooh, a message!");
        celebrate(Pose::Dance, 4000);
    } else if (now - lastMeshJoyMs > 60000) {
        lastMeshJoyMs = now; // the mesh being busy cheers him up, a little at a time
        st.joy = clampAdd(st.joy, 1);
    }
    return ProcessMessage::CONTINUE;
}

// --- menu -------------------------------------------------------------------------------

int PigPetModule::handleInputEvent(const InputEvent *event)
{
    if (!screen || screen->isOverlayBannerShowing())
        return 0;
    // Long press on our frame opens the Pig menu; everything else (short press = next frame)
    // falls through to the screen.
    if (event->inputEvent == INPUT_BROKER_SELECT && screen->isShowingModuleFrame(this)) {
        openMenu();
        return 1;
    }
    return 0;
}

void PigPetModule::openMenu()
{
    enum { Back, Feed, PlayTelem, PlayTrace, SleepWake, Stats, Rename, OptionEnd };
    static const char *options[OptionEnd];
    static int optionEnums[OptionEnd];
    static char title[24];
    snprintf(title, sizeof(title), "%s", st.name);
    int n = 0;
    options[n] = "Back";
    optionEnums[n++] = Back;
    options[n] = "Feed";
    optionEnums[n++] = Feed;
    options[n] = "Play fetch";
    optionEnums[n++] = PlayTelem;
    if (traceRouteModule) {
        options[n] = "Fetch: trace route";
        optionEnums[n++] = PlayTrace;
    }
    options[n] = isAsleep() ? "Wake up" : "Put to bed";
    optionEnums[n++] = SleepWake;
    options[n] = "Stats";
    optionEnums[n++] = Stats;
    options[n] = "Rename";
    optionEnums[n++] = Rename;

    graphics::BannerOverlayOptions b;
    b.message = title;
    b.optionsArrayPtr = options;
    b.optionsEnumPtr = optionEnums;
    b.optionsCount = n;
    b.bannerCallback = [this](int selected) {
        switch (selected) {
        case Feed:
            feed();
            break;
        case PlayTelem:
            startFetch(Fetch::Telemetry);
            break;
        case PlayTrace:
            startFetch(Fetch::TraceRoute);
            break;
        case SleepWake:
            st.sleepMode = (uint8_t)(isAsleep() ? SleepMode::Awake : SleepMode::Asleep);
            setStatus(isAsleep() ? "Good night..." : "Good morning!");
            save();
            break;
        case Stats:
            showStats();
            break;
        case Rename:
            pendingRename = true; // opened from runOnce once this banner has closed
            break;
        default:
            break;
        }
    };
    screen->showOverlayBanner(b);
}

void PigPetModule::feed()
{
    if (isAsleep()) {
        setStatus("Shh, he's asleep");
        return;
    }
    if (st.food >= 90) {
        st.joy = clampAdd(st.joy, -2);
        setStatus("Too full!");
        return;
    }
    st.food = clampAdd(st.food, 30);
    st.joy = clampAdd(st.joy, 3);
    setStatus("Yum!");
    celebrate(Pose::Eat, 3000);
    save();
}

void PigPetModule::startFetch(Fetch kind)
{
    const uint32_t now = millis();
    if (isAsleep()) {
        setStatus("Shh, he's asleep");
        return;
    }
    if (fetch != Fetch::None) {
        setStatus("Still fetching...");
        return;
    }
    if (st.energy < 15) {
        setStatus("Too tired to play");
        return;
    }
    if (lastFetchMs && now - lastFetchMs < kFetchCooldownMs) {
        char s[40];
        snprintf(s, sizeof(s), "Rest %lu min first", (unsigned long)((kFetchCooldownMs - (now - lastFetchMs)) / 60000 + 1));
        setStatus(s);
        return;
    }

    // A playmate: heard recently, not us, close by in hops; pick one at random
    NodeNum cands[16];
    uint8_t nc = 0;
    const uint32_t tNow = getTime();
    const NodeNum me = nodeDB->getNodeNum();
    for (size_t i = 0; i < nodeDB->getNumMeshNodes() && nc < 16; i++) {
        const meshtastic_NodeInfoLite *n = nodeDB->getMeshNodeByIndex(i);
        if (!n || n->num == me || !n->last_heard)
            continue;
        if (tNow > n->last_heard && tNow - n->last_heard > kHeardWithinS)
            continue;
        if (n->has_hops_away && n->hops_away > 2)
            continue;
        cands[nc++] = n->num;
    }
    if (!nc) {
        setStatus("Nobody to play with");
        return;
    }
    const NodeNum target = cands[random(nc)];

    bool sent = false;
    if (kind == Fetch::Telemetry && deviceTelemetryModule) {
        sent = deviceTelemetryModule->requestFrom(target);
    } else if (kind == Fetch::TraceRoute && traceRouteModule) {
        fetchRouteAtMs = traceRouteModule->lastRoute().atMs;
        sent = traceRouteModule->startTraceRoute(target); // note: shows the traceroute frame
    }
    if (!sent) {
        setStatus("The ball got stuck");
        return;
    }
    fetch = kind;
    fetchTarget = target;
    fetchStartMs = lastFetchMs = now;
    st.energy = clampAdd(st.energy, -10);
    st.food = clampAdd(st.food, -3);
    char s[40];
    snprintf(s, sizeof(s), "Fetch! -> %s", nodeShortName(target));
    setStatus(s, 6000);
    LOG_INFO("PigPet: fetch (%s) to 0x%08x", kind == Fetch::Telemetry ? "telemetry" : "traceroute", target);
}

void PigPetModule::showStats()
{
    static char msg[96];
    snprintf(msg, sizeof(msg), "Food %u  Joy %u\nEnergy %u  Fit %u\nWalked %lu.%lu km  Fetches %lu", st.food, st.joy,
             st.energy, st.fitness, (unsigned long)(st.walkedM / 1000), (unsigned long)((st.walkedM % 1000) / 100),
             (unsigned long)st.fetches);
    screen->showSimpleBanner(msg, 6000);
}

// --- loop -------------------------------------------------------------------------------

int32_t PigPetModule::runOnce()
{
    const uint32_t now = millis();

    if (pendingRename && screen && !screen->isOverlayBannerShowing()) {
        pendingRename = false;
        screen->showTextInput("Pig's name", st.name, 60000, [this](const std::string &text) {
            size_t a = text.find_first_not_of(' ');
            if (a == std::string::npos)
                return; // blank: keep the old name
            const std::string t = text.substr(a, sizeof(st.name) - 1);
            strncpy(st.name, t.c_str(), sizeof(st.name) - 1);
            st.name[sizeof(st.name) - 1] = '\0';
            save();
        });
    }

    if (now - lastMinuteMs >= 60000) {
        lastMinuteMs += 60000;
        minuteTick();
    }
    if (now - lastWalkCheckMs >= 10000) {
        lastWalkCheckMs = now;
        walkTick();
    }

    // Fetch by traceroute: the result lands in TraceRouteModule
    if (fetch == Fetch::TraceRoute && traceRouteModule) {
        const auto &r = traceRouteModule->lastRoute();
        if (r.atMs != fetchRouteAtMs && r.target == fetchTarget) {
            fetch = Fetch::None;
            st.joy = clampAdd(st.joy, 15);
            st.fetches++;
            char s[40];
            snprintf(s, sizeof(s), "Back from %s, %u hop%s!", nodeShortName(r.target), r.hopCount + 1, r.hopCount ? "s" : "");
            setStatus(s);
            celebrate(Pose::Twerk, 3000);
            save();
        }
    }
    if (fetch != Fetch::None && now - fetchStartMs > (fetch == Fetch::Telemetry ? kTelemetryWaitMs : kTraceWaitMs)) {
        fetch = Fetch::None;
        st.joy = clampAdd(st.joy, 2);
        setStatus("The ball got lost...");
    }

    if (dirty && now - dirtySinceMs > kSaveAfterMs)
        save();

    const bool visible = screen && screen->isShowingModuleFrame(this);
    if (!visible)
        return 1000;

    // Very happy and showing: an occasional spontaneous twerk
    if (st.joy > 90 && now - lastHappyTwerkMs > 10 * 60000UL && currentPose() == Pose::Walk) {
        lastHappyTwerkMs = now;
        celebrate(Pose::Twerk, 2500);
    }

    // Animate: walk back and forth; sad walks at a quarter of the speed
    phase++;
    const Pose pose = currentPose();
    if (pose == Pose::Walk || pose == Pose::Sad) {
        if (pose == Pose::Walk || (++subStep & 3) == 0) {
            pigX += (pose == Pose::Walk ? 2 : 1) * pigDir;
            if (pigX >= 128 - kPigW) {
                pigX = 128 - kPigW;
                pigDir = -1;
            } else if (pigX <= 2) {
                pigX = 2;
                pigDir = 1;
            }
        }
    }
    UIFrameEvent e;
    e.action = UIFrameEvent::Action::REDRAW_ONLY;
    notifyObservers(&e);
    return kFrameMs;
}

void PigPetModule::drawFrame(OLEDDisplay *display, OLEDDisplayUiState *state, int16_t x, int16_t y)
{
    const uint32_t now = millis();
    display->setColor(WHITE);
    display->setFont(FONT_SMALL);

    // Top line: a recent event, or name and mood
    if ((int32_t)(statusUntilMs - now) > 0) {
        display->setTextAlignment(TEXT_ALIGN_CENTER);
        display->drawString(x + 64, y, status);
    } else {
        display->setTextAlignment(TEXT_ALIGN_LEFT);
        display->drawString(x, y, st.name);
        display->setTextAlignment(TEXT_ALIGN_RIGHT);
        display->drawString(x + 128, y, moodWord());
    }
    display->setTextAlignment(TEXT_ALIGN_LEFT);

    const Pose pose = currentPose();
    // Celebrations and sleep happen mid-screen; walking uses the trot position
    const bool still = pose != Pose::Walk && pose != Pose::Sad;
    const int16_t px = x + (still ? 40 : pigX);
    const int8_t facing = still ? 1 : pigDir;
    // Walk alternates legs every other frame; twerk every frame
    const uint8_t ph = (pose == Pose::Twerk) ? phase : (pose == Pose::Sad ? phase / 4 : phase / 2);
    drawPig(display, px, y + kPigTop, (uint8_t)pose, ph, facing);

    if (pose == Pose::Sleep) {
        for (uint8_t i = 0; i < 3; i++) {
            const uint8_t k = (phase / 2 + i * 4) % 12;
            drawZ(display, x + 86 + i * 7, y + kPigTop + 14 - i * 6 - k / 4);
        }
    } else if (pose == Pose::Dance) {
        drawNote(display, x + 24 + (phase & 1) * 2, y + kPigTop + 4 - (phase & 3));
        drawNote(display, x + 98 - (phase & 1) * 2, y + kPigTop + (phase % 3));
    }

    // Food, Joy, Energy bars along the bottom
    const uint8_t vals[3] = {st.food, st.joy, st.energy};
    for (uint8_t i = 0; i < 3; i++) {
        const int16_t bx = x + 6 + i * 42, byy = y + 59;
        display->drawRect(bx, byy, 34, 5);
        display->fillRect(bx + 1, byy + 1, (int16_t)(32 * vals[i] / 100), 3);
    }
}

#endif
