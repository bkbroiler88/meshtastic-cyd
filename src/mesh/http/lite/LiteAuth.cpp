#include "LiteAuth.h"

#include "SPILock.h"
#include "configuration.h"
#include <FSCommon.h>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <esp_random.h>

using namespace httpsserver;

#ifndef WEBUI_AUTH_USER
#define WEBUI_AUTH_USER "mesh"
#endif

#define WEBUI_CRED_PATH "/prefs/webui"

static std::string authUser;
static std::string authPass;

/// Ambiguity-free alphabet: no 0/O, 1/l/I - this gets read off a serial log and typed by hand.
static const char kAlphabet[] = "abcdefghijkmnopqrstuvwxyzACDEFGHJKLMNPQRSTUVWXYZ23456789";

static std::string randomPassword(size_t len)
{
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; i++)
        out += kAlphabet[esp_random() % (sizeof(kAlphabet) - 1)];
    return out;
}

void liteAuthBegin()
{
    if (!authPass.empty())
        return;

    authUser = WEBUI_AUTH_USER;

#ifdef WEBUI_AUTH_PASS
    // Pinned at build time. Nothing is read from or written to the filesystem.
    authPass = WEBUI_AUTH_PASS;
    LOG_INFO("Web UI: auth user '%s', password set at build time", authUser.c_str());
    return;
#else
    concurrency::LockGuard g(spiLock);

    if (FSCom.exists(WEBUI_CRED_PATH)) {
        File f = FSCom.open(WEBUI_CRED_PATH, FILE_O_READ);
        if (f) {
            String u = f.readStringUntil('\n');
            String p = f.readStringUntil('\n');
            f.close();
            u.trim();
            p.trim();
            if (u.length() && p.length()) {
                authUser = u.c_str();
                authPass = p.c_str();
                LOG_INFO("Web UI: auth user '%s' loaded from %s", authUser.c_str(), WEBUI_CRED_PATH);
                return;
            }
        }
        LOG_WARN("Web UI: %s unreadable, generating new credentials", WEBUI_CRED_PATH);
    }

    // 8 characters: long enough for a LAN gate out of a 55-symbol alphabet, short enough that
    // "Login: mesh / xxxxxxxx" fits one line on the device's WiFi screen.
    authPass = randomPassword(8);
    File f = FSCom.open(WEBUI_CRED_PATH, FILE_O_WRITE);
    if (f) {
        f.print(authUser.c_str());
        f.print("\n");
        f.print(authPass.c_str());
        f.print("\n");
        f.close();
        // Printed once, here, because there is nowhere else to get it from. Delete the file
        // to have a new one issued on the next boot.
        LOG_INFO("Web UI: new credentials -> user '%s' password '%s' (saved to %s)", authUser.c_str(), authPass.c_str(),
                 WEBUI_CRED_PATH);
    } else {
        LOG_ERROR("Web UI: cannot write %s - the password below is good for this boot only", WEBUI_CRED_PATH);
        LOG_INFO("Web UI: user '%s' password '%s'", authUser.c_str(), authPass.c_str());
    }
#endif
}

const char *liteAuthUser()
{
    return authUser.c_str();
}

const char *liteAuthPassword()
{
    return authPass.c_str();
}

/// Length-independent compare, so a reply cannot be timed character by character.
static bool constantTimeEquals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++)
        diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

void liteAuthMiddleware(HTTPRequest *req, HTTPResponse *res, std::function<void()> next)
{
    if (authPass.empty()) {
        // Never been initialised - fail closed rather than serving the node wide open.
        res->setStatusCode(503);
        res->setStatusText("Service Unavailable");
        res->setHeader("Content-Type", "text/plain");
        res->println("Web UI authentication is not configured.");
        return;
    }

    if (constantTimeEquals(req->getBasicAuthUser(), authUser) && constantTimeEquals(req->getBasicAuthPassword(), authPass)) {
        next();
        return;
    }

    res->setStatusCode(401);
    res->setStatusText("Unauthorized");
    res->setHeader("WWW-Authenticate", "Basic realm=\"Meshtastic\"");
    res->setHeader("Content-Type", "text/plain");
    res->println("Authentication required.");
    // No next() - the matched handler never runs.
}
