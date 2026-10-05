#pragma once

#include <functional>
#include <string>

// Forward-declared rather than included: the middleware signature only needs pointers, and
// this header is also pulled in by the display code, which has no business seeing the HTTP
// server's headers.
namespace httpsserver
{
class HTTPRequest;
class HTTPResponse;
} // namespace httpsserver

/**
 * HTTP Basic authentication for the whole web surface.
 *
 * The console is read-mostly - it reads the node and sends text - but "read-mostly" still
 * means anyone on the LAN can read your messages and your position, so the front door is
 * locked rather than individual handlers.
 *
 * Be clear-eyed about what this is: Basic auth over plain HTTP sends the password
 * base64-encoded, which is not encryption. It stops a browser that wandered onto the page
 * and anyone casually poking at the IP. It does not stop someone already on your Wi-Fi who
 * is capturing packets.
 *
 * Credentials come from /prefs/webui, a two-line file (user, then password). If it is
 * missing, a password is generated at first boot, written there and printed to the serial
 * log once - so nothing secret is ever baked into the firmware or committed. Define
 * WEBUI_AUTH_USER / WEBUI_AUTH_PASS at build time to pin them instead.
 */
void liteAuthBegin();

/// Middleware: challenges anything without valid credentials and calls next() otherwise.
void liteAuthMiddleware(httpsserver::HTTPRequest *req, httpsserver::HTTPResponse *res, std::function<void()> next);

/// Credentials for display on the device's own WiFi screen. Empty until liteAuthBegin() has
/// run, which happens when the web server starts.
const char *liteAuthUser();
const char *liteAuthPassword();
