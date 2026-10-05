#pragma once

#include "Observer.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>

/**
 * Backing for the built-in lite web console: the page itself (gzipped, in flash) plus the
 * two JSON endpoints the rest of the HTTP surface does not already cover.
 *
 * Messages are kept in a short RAM ring, not on flash. The ring only has to cover the gap
 * between client polls and the client being closed; the client keeps the long history in
 * browser storage. A node reboot therefore loses anything not yet fetched, which is the
 * deliberate trade for not writing every received text to flash.
 */

/// Messages retained on the device. Kept small on purpose - this is a poll buffer, not an
/// archive, and DRAM on this board is the scarcest resource there is.
#define LITEUI_MSG_COUNT 16
/// Bytes of text kept per message. Longer texts are truncated on a UTF-8 boundary.
#define LITEUI_MSG_TEXT 96

struct LiteMessage {
    uint32_t id;
    uint32_t from;
    uint32_t to;
    uint32_t rxTime;
    float snr;
    int32_t rssi;
    uint8_t channel;
    uint8_t hops;
    bool outgoing;
    char text[LITEUI_MSG_TEXT + 1];
};

/**
 * Allocate the ring and start observing text messages.
 *
 * Called when the web server starts, so a board that never brings up Wi-Fi never pays for
 * the buffer. Safe to call more than once. Allocation failure is not fatal: the endpoints
 * then report an empty history rather than taking the node down.
 */
void liteUIBegin();

void handleLiteMessages(httpsserver::HTTPRequest *req, httpsserver::HTTPResponse *res);
void handleLiteSend(httpsserver::HTTPRequest *req, httpsserver::HTTPResponse *res);
void handleLiteIndex(httpsserver::HTTPRequest *req, httpsserver::HTTPResponse *res);
