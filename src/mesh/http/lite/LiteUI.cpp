#include "LiteUI.h"

#include "NodeDB.h"
#include "RadioLibInterface.h"
#include "gps/RTC.h"
#include "main.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/http/ContentHelper.h"
#include "mesh/http/WebServer.h"
#include "modules/TextMessageModule.h"
#include <HTTPURLEncodedBodyParser.hpp>
#include <new>

#include "WebUIAssets.h" // generated - see tools/gen_lite_assets.py

using namespace httpsserver;

static LiteMessage *ring = nullptr;
static uint16_t ringHead = 0;  // next slot to write
static uint16_t ringUsed = 0;  // slots filled, <= LITEUI_MSG_COUNT
static uint32_t ringSeq = 0;   // monotonic, so a client can tell new from seen

/// Copy text in, truncating on a UTF-8 boundary rather than mid-sequence - a split
/// continuation byte makes the whole JSON response invalid to the browser's parser.
static void copyText(char *dst, const uint8_t *src, size_t len)
{
    if (len > LITEUI_MSG_TEXT)
        len = LITEUI_MSG_TEXT;
    while (len > 0 && (src[len] & 0xC0) == 0x80) // stepped into a continuation byte
        len--;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static LiteMessage *ringPush()
{
    if (!ring)
        return nullptr;
    LiteMessage *m = &ring[ringHead];
    ringHead = (ringHead + 1) % LITEUI_MSG_COUNT;
    if (ringUsed < LITEUI_MSG_COUNT)
        ringUsed++;
    memset(m, 0, sizeof(*m));
    m->id = ++ringSeq;
    return m;
}

class LiteMessageSink
{
  public:
    CallbackObserver<LiteMessageSink, const meshtastic_MeshPacket *> textMessageObserver =
        CallbackObserver<LiteMessageSink, const meshtastic_MeshPacket *>(this, &LiteMessageSink::onTextMessage);

    int onTextMessage(const meshtastic_MeshPacket *mp)
    {
        if (!mp || mp->which_payload_variant != meshtastic_MeshPacket_decoded_tag)
            return 0;
        LiteMessage *m = ringPush();
        if (!m)
            return 0;
        m->from = mp->from;
        m->to = mp->to;
        m->rxTime = mp->rx_time;
        m->snr = mp->rx_snr;
        m->rssi = mp->rx_rssi;
        m->channel = mp->channel;
        // hop_start is what the sender set out with, so the difference is hops actually taken
        m->hops = (mp->hop_start >= mp->hop_limit) ? (uint8_t)(mp->hop_start - mp->hop_limit) : 0;
        m->outgoing = false;
        copyText(m->text, mp->decoded.payload.bytes, mp->decoded.payload.size);
        return 0;
    }
};

static LiteMessageSink *sink = nullptr;

void liteUIBegin()
{
    if (ring)
        return;
    // nothrow: exceptions are disabled, so a throwing new would abort() the node rather than
    // let it come up without a message buffer.
    ring = new (std::nothrow) LiteMessage[LITEUI_MSG_COUNT];
    if (!ring) {
        LOG_WARN("Lite UI: no DRAM for the message ring, history disabled");
        return;
    }
    sink = new (std::nothrow) LiteMessageSink();
    if (!sink) {
        delete[] ring;
        ring = nullptr;
        LOG_WARN("Lite UI: no DRAM for the message observer, history disabled");
        return;
    }
    sink->textMessageObserver.observe(textMessageModule);
    LOG_INFO("Lite UI: message ring ready (%d x %u B)", LITEUI_MSG_COUNT, (unsigned)sizeof(LiteMessage));
}

void handleLiteIndex(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    // Straight out of flash. No filesystem is touched, so this page is served whether or not
    // the SD card is mounted and whether or not LittleFS is healthy.
    res->setHeader("Content-Type", "text/html; charset=utf-8");
    res->setHeader("Content-Encoding", "gzip");
    res->setHeader("Content-Length", intToString(sizeof(WEBUI_INDEX_HTML_GZ)));
    res->setHeader("Cache-Control", "no-cache");
    res->write(WEBUI_INDEX_HTML_GZ, sizeof(WEBUI_INDEX_HTML_GZ));
}

void handleLiteMessages(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    res->setHeader("Content-Type", "application/json");

    std::string out;
    out.reserve(1024);
    out += "{\"data\":{\"messages\":[";

    bool first = true;
    for (uint16_t i = 0; i < ringUsed; i++) {
        // Walk oldest to newest
        const LiteMessage *m = &ring[(ringHead + LITEUI_MSG_COUNT - ringUsed + i) % LITEUI_MSG_COUNT];
        char id[16], toId[16];
        snprintf(id, sizeof(id), "!%08x", (unsigned int)m->from);
        snprintf(toId, sizeof(toId), "!%08x", (unsigned int)m->to);

        if (!first)
            out += ",";
        first = false;
        out += "{\"channel\":";
        out += jsonNum((int)m->channel);
        out += ",\"from\":";
        out += jsonEscape(id);
        out += ",\"hops\":";
        out += jsonNum((int)m->hops);
        out += ",\"id\":";
        out += jsonNum((int)m->id);
        out += ",\"outgoing\":";
        out += m->outgoing ? "true" : "false";
        out += ",\"rssi\":";
        out += jsonNum((int)m->rssi);
        out += ",\"rx_time\":";
        out += jsonNum((int)m->rxTime);
        out += ",\"snr\":";
        out += jsonNum(m->snr);
        out += ",\"text\":";
        out += jsonEscape(m->text);
        // Without this a client cannot tell a direct message from a broadcast - they arrive
        // on the same port and differ only in the destination.
        out += ",\"to\":";
        out += jsonEscape(toId);
        out += ",\"dm\":";
        out += (m->to != NODENUM_BROADCAST && m->to != 0) ? "true" : "false";
        out += "}";
    }

    out += "],\"now\":";
    out += jsonNum((int)getValidTime(RTCQualityFromNet));
    out += ",\"seq\":";
    out += jsonNum((int)ringSeq);
    out += "},\"status\":\"ok\"}";
    res->print(out.c_str());
}

void handleLiteSend(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    res->setHeader("Content-Type", "application/json");

    std::string chStr, toStr, text;
    HTTPURLEncodedBodyParser parser(req);
    while (parser.nextField()) {
        std::string name = parser.getFieldName();
        std::string value;
        char buf[128];
        while (!parser.endOfField()) {
            size_t n = parser.read((byte *)buf, sizeof(buf));
            if (!n)
                break;
            value.append(buf, n);
        }
        if (name == "ch")
            chStr = value;
        else if (name == "to")
            toStr = value;
        else if (name == "msg")
            text = value;
    }

    if (text.empty() || !router) {
        res->setStatusCode(400);
        res->print("{\"status\":\"error\",\"error\":\"empty message\"}");
        return;
    }

    meshtastic_MeshPacket *p = router->allocForSending();
    if (!p) {
        res->setStatusCode(503);
        res->print("{\"status\":\"error\",\"error\":\"no packet buffer\"}");
        return;
    }

    // "!xxxxxxxx" or a bare decimal node number; anything else is a broadcast
    NodeNum dest = NODENUM_BROADCAST;
    if (toStr.size() > 1 && toStr[0] == '!')
        dest = (NodeNum)strtoul(toStr.c_str() + 1, nullptr, 16);
    else if (!toStr.empty())
        dest = (NodeNum)strtoul(toStr.c_str(), nullptr, 10);

    size_t len = text.size();
    if (len > sizeof(p->decoded.payload.bytes))
        len = sizeof(p->decoded.payload.bytes);

    p->to = dest;
    p->channel = (uint8_t)atoi(chStr.c_str());
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    p->decoded.payload.size = len;
    memcpy(p->decoded.payload.bytes, text.c_str(), len);
    p->want_ack = (dest != NODENUM_BROADCAST);

    service->sendToMesh(p, RX_SRC_LOCAL, true);

    // The observer only sees packets arriving off the mesh, so our own traffic is recorded
    // here or it would never appear in the log.
    LiteMessage *m = ringPush();
    if (m) {
        m->from = nodeDB->getNodeNum();
        m->to = dest;
        m->rxTime = getValidTime(RTCQualityFromNet);
        m->channel = p->channel;
        m->outgoing = true;
        copyText(m->text, (const uint8_t *)text.c_str(), text.size());
    }

    res->print("{\"status\":\"ok\"}");
}
