#if !MESHTASTIC_EXCLUDE_WEBSERVER
#include "NodeDB.h"
#include "PowerFSM.h"
#include "RadioLibInterface.h"
#include "airtime.h"
#include "main.h"
#include "mesh/Channels.h"
#include "mesh/http/ContentHelper.h"
#include "mesh/http/WebServer.h"
#include <memory>
#ifdef MESHTASTIC_WEBUI_LITE
#include "mesh/http/lite/LiteAuth.h"
#include "mesh/http/lite/LiteUI.h"
#endif
#if HAS_WIFI
#include "mesh/wifi/WiFiAPClient.h"
#endif
#include "Power.h"
#include "SPILock.h"
#include <FSCommon.h>
#include <HTTPBodyParser.hpp>
#include <HTTPMultipartBodyParser.hpp>
#include <HTTPURLEncodedBodyParser.hpp>
#include <cmath>
#include <sstream>

#ifdef ARCH_ESP32
#include "esp_task_wdt.h"
#endif

/*
  Including the esp32_https_server library will trigger a compile time error. I've
  tracked it down to a reoccurrance of this bug:
    https://gcc.gnu.org/bugzilla/show_bug.cgi?id=57824
  The work around is described here:
    https://forums.xilinx.com/t5/Embedded-Development-Tools/Error-with-Standard-Libaries-in-Zynq/td-p/450032

  Long story short is we need "#undef str" before including the esp32_https_server.
    - Jm Casler (jm@casler.org) Oct 2020
*/
#undef str

// Includes for the https server
//   https://github.com/fhessel/esp32_https_server
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPSServer.hpp>
#include <HTTPServer.hpp>
#include <SSLCert.hpp>

// The HTTPS Server comes in a separate namespace. For easier use, include it here.
using namespace httpsserver;

#include "mesh/http/ContentHandler.h"

#define DEST_FS_USES_LITTLEFS

// We need to specify some content-type mapping, so the resources get delivered with the
// right content type and are displayed correctly in the browser
char const *contentTypes[][2] = {{".txt", "text/plain"},     {".html", "text/html"},
                                 {".js", "text/javascript"}, {".png", "image/png"},
                                 {".jpg", "image/jpg"},      {".gz", "application/gzip"},
                                 {".gif", "image/gif"},      {".json", "application/json"},
                                 {".css", "text/css"},       {".ico", "image/vnd.microsoft.icon"},
                                 {".svg", "image/svg+xml"},  {"", ""}};

// const char *certificate = NULL; // change this as needed, leave as is for no TLS check (yolo security)

// Our API to handle messages to and from the radio.
HttpAPI webAPI;

void registerHandlers(HTTPServer *insecureServer, HTTPSServer *secureServer)
{

    // For every resource available on the server, we need to create a ResourceNode
    // The ResourceNode links URL and HTTP method to a handler function

    ResourceNode *nodeAPIv1ToRadioOptions = new ResourceNode("/api/v1/toradio", "OPTIONS", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1ToRadio = new ResourceNode("/api/v1/toradio", "PUT", &handleAPIv1ToRadio);
    ResourceNode *nodeAPIv1FromRadioOptions = new ResourceNode("/api/v1/fromradio", "OPTIONS", &handleAPIv1FromRadio);
    ResourceNode *nodeAPIv1FromRadio = new ResourceNode("/api/v1/fromradio", "GET", &handleAPIv1FromRadio);

    ResourceNode *nodeAdmin = new ResourceNode("/admin", "GET", &handleAdmin);

    ResourceNode *nodeRestart = new ResourceNode("/restart", "POST", &handleRestart);
    ResourceNode *nodeFormUpload = new ResourceNode("/upload", "POST", &handleFormUpload);

    ResourceNode *nodeJsonScanNetworks = new ResourceNode("/json/scanNetworks", "GET", &handleScanNetworks);
    ResourceNode *nodeJsonReport = new ResourceNode("/json/report", "GET", &handleReport);
    ResourceNode *nodeJsonNodes = new ResourceNode("/json/nodes", "GET", &handleNodes);
    ResourceNode *nodeJsonFsBrowseStatic = new ResourceNode("/json/fs/browse/static", "GET", &handleFsBrowseStatic);
    ResourceNode *nodeJsonDelete = new ResourceNode("/json/fs/delete/static", "DELETE", &handleFsDeleteStatic);

    // A wildcard spans a single path segment (the resolver ends each parameter at the next
    // '/'), so one "/*" route leaves every nested file unreachable - the web client's
    // translations sit at /i18n/locales/<lang>/*.json and its deep links run three segments
    // deep. Register one route per depth; they are disjoint, and handleStatic reads the whole
    // path off the request anyway. Depth is capped rather than using setDefaultNode(), because
    // a default node is also handed to websocket upgrades, which cast it to WebsocketNode.
    ResourceNode *nodeRoot = new ResourceNode("/*", "GET", &handleStatic);
    ResourceNode *nodeRoot2 = new ResourceNode("/*/*", "GET", &handleStatic);
    ResourceNode *nodeRoot3 = new ResourceNode("/*/*/*", "GET", &handleStatic);
    ResourceNode *nodeRoot4 = new ResourceNode("/*/*/*/*", "GET", &handleStatic);
    ResourceNode *nodeRoot5 = new ResourceNode("/*/*/*/*/*", "GET", &handleStatic);

    // Secure nodes - absent in an HTTP-only build, where secureServer is always null
#if !defined(MESHTASTIC_WEBSERVER_HTTP_ONLY)
    secureServer->registerNode(nodeAPIv1ToRadioOptions);
    secureServer->registerNode(nodeAPIv1ToRadio);
    secureServer->registerNode(nodeAPIv1FromRadioOptions);
    secureServer->registerNode(nodeAPIv1FromRadio);
    secureServer->registerNode(nodeRestart);
    secureServer->registerNode(nodeFormUpload);
    secureServer->registerNode(nodeJsonScanNetworks);
    secureServer->registerNode(nodeJsonFsBrowseStatic);
    secureServer->registerNode(nodeJsonDelete);
    secureServer->registerNode(nodeJsonReport);
    secureServer->registerNode(nodeJsonNodes);
    secureServer->registerNode(nodeAdmin);
    secureServer->registerNode(nodeRoot); // These have to be last
    secureServer->registerNode(nodeRoot2);
    secureServer->registerNode(nodeRoot3);
    secureServer->registerNode(nodeRoot4);
    secureServer->registerNode(nodeRoot5);
#else
    (void)secureServer;
#endif

    // Insecure nodes
    insecureServer->registerNode(nodeAPIv1ToRadioOptions);
    insecureServer->registerNode(nodeAPIv1ToRadio);
    insecureServer->registerNode(nodeAPIv1FromRadioOptions);
    insecureServer->registerNode(nodeAPIv1FromRadio);
#ifndef MESHTASTIC_WEBUI_LITE
    // The lite console is deliberately read-mostly: it reads the node and sends text, and
    // exposes nothing that reboots it, writes its filesystem or deletes from it. Leaving
    // these registered would keep that surface reachable behind the console's back -
    // /restart in particular is a side-effecting GET with a wildcard CORS header.
    insecureServer->registerNode(nodeRestart);
    insecureServer->registerNode(nodeFormUpload);
    insecureServer->registerNode(nodeJsonDelete);
    insecureServer->registerNode(nodeJsonScanNetworks);
#endif
    insecureServer->registerNode(nodeJsonFsBrowseStatic);
    insecureServer->registerNode(nodeJsonReport);
    // Upstream registers /json/nodes on the secure server only, so it 404s in an HTTP-only
    // build. It is a read-only endpoint and the console needs it.
    insecureServer->registerNode(nodeJsonNodes);
    insecureServer->registerNode(nodeAdmin);

#ifdef MESHTASTIC_WEBUI_LITE
    // The built-in console. Registered ahead of the wildcards so it answers "/" even when a
    // filesystem bundle is also present; anything else still falls through to handleStatic.
    liteUIBegin();
    insecureServer->registerNode(new ResourceNode("/", "GET", &handleLiteIndex));
    insecureServer->registerNode(new ResourceNode("/json/messages", "GET", &handleLiteMessages));
    insecureServer->registerNode(new ResourceNode("/json/send", "POST", &handleLiteSend));

    // One gate in front of everything, including the routes above and handleStatic.
    liteAuthBegin();
    insecureServer->addMiddleware(&liteAuthMiddleware);
#endif

    insecureServer->registerNode(nodeRoot); // These have to be last
    insecureServer->registerNode(nodeRoot2);
    insecureServer->registerNode(nodeRoot3);
    insecureServer->registerNode(nodeRoot4);
    insecureServer->registerNode(nodeRoot5);
}

void handleAPIv1FromRadio(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    LOG_DEBUG("webAPI handleAPIv1FromRadio");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    // Get access to the parameters
    ResourceParameters *params = req->getParams();

    // std::string paramAll = "all";
    std::string valueAll;

    // Status code is 200 OK by default.
    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto");

    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204); // Success with no content
        res->print("");
        return;
    }

    uint8_t txBuf[MAX_STREAM_BUF_SIZE];
    uint32_t len = 1;

    if (params->getQueryParameter("all", valueAll)) {

        // If all is true, return all the buffers we have available
        //   to us at this point in time.
        if (valueAll == "true") {
            while (len) {
                len = webAPI.getFromRadio(txBuf);
                res->write(txBuf, len);
            }

            // Otherwise, just return one protobuf
        } else {
            len = webAPI.getFromRadio(txBuf);
            res->write(txBuf, len);
        }

        // the param "all" was not specified. Return just one protobuf
    } else {
        len = webAPI.getFromRadio(txBuf);
        res->write(txBuf, len);
    }

    LOG_DEBUG("webAPI handleAPIv1FromRadio, len %d", len);
}

void handleAPIv1ToRadio(HTTPRequest *req, HTTPResponse *res)
{
    LOG_DEBUG("webAPI handleAPIv1ToRadio");

    /*
        For documentation, see:
            https://meshtastic.org/docs/development/device/http-api
            https://meshtastic.org/docs/development/device/client-api
    */

    res->setHeader("Content-Type", "application/x-protobuf");
    res->setHeader("Access-Control-Allow-Headers", "Content-Type");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "PUT, OPTIONS");
    res->setHeader("X-Protobuf-Schema", "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto");

    if (req->getMethod() == "OPTIONS") {
        res->setStatusCode(204); // Success with no content
        res->print("");
        return;
    }

    byte buffer[MAX_TO_FROM_RADIO_SIZE];
    size_t s = req->readBytes(buffer, MAX_TO_FROM_RADIO_SIZE);

    LOG_DEBUG("Received %d bytes from PUT request", s);
    webAPI.handleToRadio(buffer, s);

    res->write(buffer, s);
    LOG_DEBUG("webAPI handleAPIv1ToRadio");
}

// jsonEscape() and jsonNum() live in ContentHelper.h so the lite console can share them.

// One TLS record per write(); loop until the whole body is sent.
static bool writeAll(HTTPResponse *res, const std::string &body)
{
    size_t sent = 0;
    while (sent < body.size()) {
        const size_t remaining = body.size() - sent;
        const size_t written = res->write(reinterpret_cast<const uint8_t *>(body.data()) + sent, remaining);
        // An error code arrives as a huge count, write() returning mbedtls' int through a size_t.
        if (written == 0 || written > remaining)
            return false;
        sent += written;
    }
    return true;
}

// Build a serialized JSON array string listing files in `dirname`.
// Subdirectories recurse as nested arrays (up to `levels` deep).
std::string htmlListDir(const char *dirname, uint8_t levels)
{
    File root = FSCom.open(dirname, FILE_O_READ);
    std::string out = "[";
    bool first = true;
    if (!root) {
        out += "]";
        return out;
    }
    if (!root.isDirectory()) {
        out += "]";
        return out;
    }

    // iterate over the file list
    File file = root.openNextFile();
    while (file) {
        std::string element;
        bool haveElement = false;
        if (file.isDirectory() && !String(file.name()).endsWith(".")) {
            if (levels) {
#ifdef ARCH_ESP32
                element = htmlListDir(file.path(), levels - 1);
#else
                element = htmlListDir(file.name(), levels - 1);
#endif
                haveElement = true;
                file.close();
            }
        } else {
#ifdef ARCH_ESP32
            String fileName = String(file.path()).substring(1);
#else
            String fileName = String(file.name()).substring(1);
#endif
            String tempName = String(file.name()).substring(1);
            // Keys in the previous std::map<string,...> were emitted in
            // alphabetical order: name, nameModified, size.
            element = "{";
            element += jsonEscape("name");
            element += ":";
            element += jsonEscape(fileName.c_str());
            if (tempName.endsWith(".gz")) {
#ifdef ARCH_ESP32
                String modifiedFile = String(file.path()).substring(1);
#else
                String modifiedFile = String(file.name()).substring(1);
#endif
                modifiedFile.remove((modifiedFile.length() - 3), 3);
                element += ",";
                element += jsonEscape("nameModified");
                element += ":";
                element += jsonEscape(modifiedFile.c_str());
            }
            element += ",";
            element += jsonEscape("size");
            element += ":";
            element += jsonNum((int)file.size());
            element += "}";
            haveElement = true;
        }
        if (haveElement) {
            if (!first)
                out += ",";
            out += element;
            first = false;
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    out += "]";
    return out;
}

void handleFsBrowseStatic(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    std::string fileList;
    uint64_t total, used;
    {
        concurrency::LockGuard g(spiLock);
        fileList = htmlListDir("/static", 10);
        total = FSCom.totalBytes();
        used = FSCom.usedBytes();
    }

    // Key order matches the previous std::map-based emission (alphabetical).
    std::string out;
    out.reserve(fileList.size() + 128);
    out += "{\"data\":{\"files\":";
    out += fileList;
    out += ",\"filesystem\":{\"free\":";
    out += jsonNum((int)(total - used));
    out += ",\"total\":";
    out += jsonNum((int)total);
    out += ",\"used\":";
    out += jsonNum((int)used);
    out += "}},\"status\":\"ok\"}";

    writeAll(res, out);
}

void handleFsDeleteStatic(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string paramValDelete;

    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "DELETE");

    if (params->getQueryParameter("delete", paramValDelete)) {
        std::string pathDelete = "/" + paramValDelete;
        bool removed;
        {
            concurrency::LockGuard g(spiLock);
            removed = FSCom.remove(pathDelete.c_str());
        }
        LOG_INFO("%s", pathDelete.c_str());
        std::string out = "{\"status\":";
        out += jsonEscape(removed ? "ok" : "Error");
        out += "}";
        writeAll(res, out);
        return;
    }
}

void handleStatic(HTTPRequest *req, HTTPResponse *res)
{
    if (webServerThread)
        webServerThread->markActivity();

    // Take the path straight off the request rather than out of a path parameter. A wildcard
    // in a ResourceNode matches one segment only (the resolver stops each parameter at the
    // next '/'), so reading parameter 0 could never reach a nested file: the web client's
    // translations live at /i18n/locales/<lang>/*.json, and its deep links are several
    // segments deep. This handler is also the resolver's default node, which is reached with
    // no path parameters at all.
    {
        std::string relPath = req->getRequestString();
        const size_t queryStart = relPath.find('?');
        if (queryStart != std::string::npos)
            relPath.erase(queryStart);
        // Strip the leading '/' - "/static/" supplies it - and refuse to walk out of /static
        if (!relPath.empty() && relPath[0] == '/')
            relPath.erase(0, 1);
        if (relPath.find("..") != std::string::npos)
            relPath.clear();

        std::string filename = "/static/" + relPath;
        std::string filenameGzip = "/static/" + relPath + ".gz";

        // Try to open the file
        File file;

        bool has_set_content_type = false;

        if (filename == "/static/") {
            filename = "/static/index.html";
            filenameGzip = "/static/index.html.gz";
        }

        // spiLock covers filesystem calls only: a socket write or a syslog line can need the lock itself on a
        // shared-bus Ethernet board, and the lock is not recursive.
        bool exists;
        bool gzipExists = false;
        bool available;
        size_t size;
        {
            concurrency::LockGuard g(spiLock);
            exists = FSCom.exists(filename.c_str());
            if (!exists) {
                gzipExists = FSCom.exists(filenameGzip.c_str());
                if (!gzipExists)
                    filenameGzip = "/static/index.html.gz";
            }
            file = FSCom.open(exists ? filename.c_str() : filenameGzip.c_str());
            available = file.available();
            size = file.size();
            if (!available && !exists && !gzipExists)
                file.close();
        }

        if (!available)
            LOG_WARN("File not available - %s", exists ? filename.c_str() : filenameGzip.c_str());
        if (!exists && !gzipExists) {
            has_set_content_type = true;
            res->setHeader("Content-Type", "text/html");
            if (!available) {
                res->println("Web server is running.<br><br>The content you are looking for can't be found. Please see: <a "
                             "href=https://meshtastic.org/docs/software/web-client/>FAQ</a>.<br><br><a "
                             "href=/admin>admin</a>");
                return;
            }
        }
        if (!exists)
            res->setHeader("Content-Encoding", "gzip");

        res->setHeader("Content-Length", httpsserver::intToString(size));

        // Content-Type is guessed using the definition of the contentTypes-table defined above
        int cTypeIdx = 0;
        do {
            if (filename.rfind(contentTypes[cTypeIdx][0]) != std::string::npos) {
                res->setHeader("Content-Type", contentTypes[cTypeIdx][1]);
                has_set_content_type = true;
                break;
            }
            cTypeIdx += 1;
        } while (strlen(contentTypes[cTypeIdx][0]) > 0);

        if (!has_set_content_type) {
            // Set a default content type
            res->setHeader("Content-Type", "application/octet-stream");
        }

        // Read the file and write it to the HTTP response body
        size_t length = 0;
        do {
            uint8_t buffer[256];
            {
                concurrency::LockGuard g(spiLock);
                length = file.read(buffer, sizeof(buffer));
            }
            res->write(buffer, length);
        } while (length > 0);

        {
            concurrency::LockGuard g(spiLock);
            file.close();
        }

        return;
    }
}

void handleFormUpload(HTTPRequest *req, HTTPResponse *res)
{

    LOG_DEBUG("Form Upload - Disable keep-alive");
    res->setHeader("Connection", "close");

    // First, we need to check the encoding of the form that we have received.
    // The browser will set the Content-Type request header, so we can use it for that purpose.
    // Then we select the body parser based on the encoding.
    // Actually we do this only for documentary purposes, we know the form is going
    // to be multipart/form-data.
    LOG_DEBUG("Form Upload - Creating body parser reference");
    std::unique_ptr<HTTPBodyParser> parser;
    std::string contentType = req->getHeader("Content-Type");

    // The content type may have additional properties after a semicolon, for example:
    // Content-Type: text/html;charset=utf-8
    // Content-Type: multipart/form-data;boundary=------s0m3w31rdch4r4c73rs
    // As we're interested only in the actual mime _type_, we strip everything after the
    // first semicolon, if one exists:
    size_t semicolonPos = contentType.find(";");
    if (semicolonPos != std::string::npos) {
        contentType.resize(semicolonPos);
    }

    // Now, we can decide based on the content type:
    if (contentType == "multipart/form-data") {
        LOG_DEBUG("Form Upload - multipart/form-data");
        parser.reset(new HTTPMultipartBodyParser(req));
    } else {
        LOG_DEBUG("Unknown POST Content-Type: %s", contentType.c_str());
        return;
    }

    res->println("<html><head><meta http-equiv=\"refresh\" content=\"1;url=/static\" /><title>File "
                 "Upload</title></head><body><h1>File Upload</h1>");

    // We iterate over the fields. Any field with a filename is uploaded.
    // Note that the BodyParser consumes the request body, meaning that you can iterate over the request's
    // fields only a single time. The reason for this is that it allows you to handle large requests
    // which would not fit into memory.
    bool didwrite = false;

    // parser->nextField() will move the parser to the next field in the request body (field meaning a
    // form field, if you take the HTML perspective). After the last field has been processed, nextField()
    // returns false and the while loop ends.
    while (parser->nextField()) {
        // For Multipart data, each field has three properties:
        // The name ("name" value of the <input> tag)
        // The filename (If it was a <input type="file">, this is the filename on the machine of the
        //   user uploading it)
        // The mime type (It is determined by the client. So do not trust this value and blindly start
        //   parsing files only if the type matches)
        std::string name = parser->getFieldName();
        std::string filename = parser->getFieldFilename();
        std::string mimeType = parser->getFieldMimeType();
        // We log all three values, so that you can observe the upload on the serial monitor:
        LOG_DEBUG("handleFormUpload: field name='%s', filename='%s', mimetype='%s'", name.c_str(), filename.c_str(),
                  mimeType.c_str());

        // Double check that it is what we expect
        if (name != "file") {
            LOG_DEBUG("Skip unexpected field");
            res->println("<p>No file found.</p>");
            return;
        }

        // Double check that it is what we expect
        if (filename == "") {
            LOG_DEBUG("Skip unexpected field");
            res->println("<p>No file found.</p>");
            return;
        }

        // You should check file name validity and all that, but we skip that to make the core
        // concepts of the body parser functionality easier to understand.
        std::string pathname = "/static/" + filename;

        // spiLock covers filesystem calls only: the body is read from a socket, and on a shared-bus Ethernet board
        // the receive path needs the lock. Free space is taken once, as nothing else writes while this runs.
        File file;
        size_t freeBytes;
        {
            concurrency::LockGuard g(spiLock);
            // Create a new file to stream the data into
            file = FSCom.open(pathname.c_str(), FILE_O_WRITE);
            freeBytes = FSCom.totalBytes() - FSCom.usedBytes();
        }
        size_t fileLength = 0;
        didwrite = true;

        // With endOfField you can check whether the end of field has been reached or if there's
        // still data pending. With multipart bodies, you cannot know the field size in advance.
        while (!parser->endOfField()) {
            esp_task_wdt_reset();

            byte buf[512];
            size_t readLength = parser->read(buf, 512);

            // Abort the transfer if there is less than 50k space left on the filesystem, or a write comes up short.
            const bool full = fileLength + readLength + 51200 > freeBytes;
            size_t written = 0;
            if (!full) {
                concurrency::LockGuard g(spiLock);
                written = file.write(buf, readLength);
            }
            if (full || written != readLength) {
                {
                    concurrency::LockGuard g(spiLock);
                    file.flush();
                    file.close();
                }
                res->println(full ? "<p>Write aborted! Reserving 50k on filesystem.</p>" : "<p>Write failed.</p>");
                return;
            }
            fileLength += readLength;
            LOG_DEBUG("File Length %i", fileLength);
        }

        {
            concurrency::LockGuard g(spiLock);
            file.flush();
            file.close();
        }

        res->printf("<p>Saved %d bytes to %s</p>", (int)fileLength, pathname.c_str());
    }
    if (!didwrite) {
        res->println("<p>Did not write any file</p>");
    }
    res->println("</body></html>");
}

void handleReport(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string content;

    if (!params->getQueryParameter("content", content)) {
        content = "json";
    }

    if (content == "json") {
        res->setHeader("Content-Type", "application/json");
        res->setHeader("Access-Control-Allow-Origin", "*");
        res->setHeader("Access-Control-Allow-Methods", "GET");
    } else {
        res->setHeader("Content-Type", "text/html");
        res->println("<pre>");
    }

    auto arrayFromLog = [](const uint32_t *logArray, int count) -> std::string {
        std::string s = "[";
        for (int i = 0; i < count; i++) {
            if (i)
                s += ",";
            s += jsonNum((int)logArray[i]);
        }
        s += "]";
        return s;
    };

    // One constant sizes the buffer and the count, so they cannot drift. Buffer is per call, so a
    // report that fails emits zeros rather than the previous type's data.
    constexpr size_t periods = AirTime::getPeriodsToLog();
    auto reportFor = [&](reportTypes reportType) {
        uint32_t logArray[periods] = {0};
        (void)airTime->airtimeReport(reportType, logArray, periods);
        return arrayFromLog(logArray, (int)periods);
    };

    std::string txLog = reportFor(TX_LOG);
    std::string rxLog = reportFor(RX_LOG);
    std::string rxAllLog = reportFor(RX_ALL_LOG);

    String wifiIPString = WiFi.localIP().toString();
    std::string wifiIP = wifiIPString.c_str();

    spiLock->lock();
    uint64_t fsTotal = FSCom.totalBytes();
    uint64_t fsUsed = FSCom.usedBytes();
    spiLock->unlock();

    // Emit keys in the same alphabetical order as the previous
    // std::map-based JSON output to keep responses byte-compatible.
    std::string out;
    out.reserve(1024);
    out += "{\"data\":{";

    // airtime
    out += "\"airtime\":{";
    out += "\"channel_utilization\":";
    out += jsonNum(airTime->channelUtilizationPercent());
    out += ",\"periods_to_log\":";
    out += jsonNum(airTime->getPeriodsToLog());
    out += ",\"rx_all_log\":";
    out += rxAllLog;
    out += ",\"rx_log\":";
    out += rxLog;
    out += ",\"seconds_per_period\":";
    out += jsonNum((int)airTime->getSecondsPerPeriod());
    out += ",\"seconds_since_boot\":";
    out += jsonNum((int)airTime->getSecondsSinceBoot());
    out += ",\"tx_log\":";
    out += txLog;
    out += ",\"utilization_tx\":";
    out += jsonNum(airTime->utilizationTXPercent());
    out += "}";

    // device
    out += ",\"device\":{\"reboot_counter\":";
    out += jsonNum((int)myNodeInfo.reboot_count);
    out += "}";

    // memory
    out += ",\"memory\":{";
    out += "\"fs_free\":";
    out += jsonNum((int)(fsTotal - fsUsed));
    out += ",\"fs_total\":";
    out += jsonNum((int)fsTotal);
    out += ",\"fs_used\":";
    out += jsonNum((int)fsUsed);
    out += ",\"heap_free\":";
    out += jsonNum((int)memGet.getFreeHeap());
    out += ",\"heap_total\":";
    out += jsonNum((int)memGet.getHeapSize());
    out += ",\"psram_free\":";
    out += jsonNum((int)memGet.getFreePsram());
    out += ",\"psram_total\":";
    out += jsonNum((int)memGet.getPsramSize());
    out += "}";

    // node - identity and channel names. Nothing else in the JSON surface says who this node
    // is or what its channels are called, which a client served BY the node needs to render
    // its own header and channel picker without asking the user to configure a connection.
    {
        char myId[16];
        snprintf(myId, sizeof(myId), "!%08x", (unsigned int)nodeDB->getNodeNum());
        // Channels carry their role, so a client can offer the ones that are actually usable
        // instead of guessing from the name - an unnamed secondary is still a real channel,
        // and a DISABLED slot is not.
        out += ",\"node\":{\"channels\":[";
        for (ChannelIndex i = 0; i < channels.getNumChannels(); i++) {
            if (i)
                out += ",";
            out += "{\"index\":";
            out += jsonNum((int)i);
            out += ",\"name\":";
            out += jsonEscape(channels.getName(i));
            out += ",\"role\":";
            out += jsonNum((int)channels.getByIndex(i).role);
            out += "}";
        }
        out += "],\"firmware\":";
        out += jsonEscape(optstr(APP_VERSION));
        out += ",\"id\":";
        out += jsonEscape(myId);
        out += ",\"long_name\":";
        out += jsonEscape(owner.long_name);
        out += ",\"num\":";
        out += jsonNum((int)nodeDB->getNodeNum());
        out += ",\"role\":";
        out += jsonNum((int)config.device.role);
        out += ",\"short_name\":";
        out += jsonEscape(owner.short_name);
        out += "}";
    }

    // power (has_* / is_charging were serialized as the strings "true"/"false")
    out += ",\"power\":{";
    out += "\"battery_percent\":";
    out += jsonNum(powerStatus->getBatteryChargePercent());
    out += ",\"battery_voltage_mv\":";
    out += jsonNum(powerStatus->getBatteryVoltageMv());
    out += ",\"has_battery\":";
    out += jsonEscape(BoolToString(powerStatus->getHasBattery()));
    out += ",\"has_usb\":";
    out += jsonEscape(BoolToString(powerStatus->getHasUSB()));
    out += ",\"is_charging\":";
    out += jsonEscape(BoolToString(powerStatus->getIsCharging()));
    out += "}";

    // radio
    out += ",\"radio\":{\"frequency\":";
    out += jsonNum(RadioLibInterface::instance->getFreq());
    out += ",\"lora_channel\":";
    out += jsonNum((int)RadioLibInterface::instance->getChannelNum() + 1);
    out += "}";

    // wifi
    out += ",\"wifi\":{\"ip\":";
    out += jsonEscape(wifiIP);
    out += ",\"rssi\":";
    out += jsonNum(WiFi.RSSI());
    out += "}";

    out += "},\"status\":\"ok\"}";

    writeAll(res, out);
}

void handleNodes(HTTPRequest *req, HTTPResponse *res)
{
    ResourceParameters *params = req->getParams();
    std::string content;

    if (!params->getQueryParameter("content", content)) {
        content = "json";
    }

    if (content == "json") {
        res->setHeader("Content-Type", "application/json");
        res->setHeader("Access-Control-Allow-Origin", "*");
        res->setHeader("Access-Control-Allow-Methods", "GET");
    } else {
        res->setHeader("Content-Type", "text/html");
        res->println("<pre>");
    }

    // A couple of kB at a time: the whole body at once asked for a 64 kB block, one write per node
    // asks mbedTLS for a record per node.
    static const size_t NODES_FLUSH_BYTES = 2048;
    std::string out;
    out.reserve(NODES_FLUSH_BYTES + 1024); // a node's worth of headroom past the mark, so no regrowth
    out += "{\"data\":{\"nodes\":[";

    bool firstNode = true;
    uint32_t readIndex = 0;
    const meshtastic_NodeInfoLite *tempNodeInfo = nodeDB->readNextMeshNode(readIndex);
    while (tempNodeInfo != NULL) {
        // Emit every node in the DB, not only those we have a NodeInfo packet for. A node we
        // have heard but not yet been introduced to still has a number, an SNR and a last-heard
        // time - the device's own screen lists it, and omitting it here made the web client
        // look like it had discovered nothing while the mesh was still filling in.
        {
            char id[16];
            snprintf(id, sizeof(id), "!%08x", tempNodeInfo->num);
            const bool hasUser = nodeInfoLiteHasUser(tempNodeInfo);

            std::string position;
            if (nodeDB->hasValidPosition(tempNodeInfo)) {
                meshtastic_PositionLite posLite;
                if (nodeDB->copyNodePosition(tempNodeInfo->num, posLite)) {
                    position = "{\"altitude\":";
                    position += jsonNum((int)posLite.altitude);
                    position += ",\"latitude\":";
                    position += jsonNum((float)posLite.latitude_i * 1e-7);
                    position += ",\"longitude\":";
                    position += jsonNum((float)posLite.longitude_i * 1e-7);
                    position += "}";
                } else {
                    position = "null";
                }
            } else {
                position = "null";
            }

            if (!firstNode)
                out += ",";
            firstNode = false;

            // Alphabetical key order matches previous std::map-based output.
            out += "{\"hw_model\":";
            out += jsonNum(tempNodeInfo->hw_model);
            out += ",\"id\":";
            out += jsonEscape(id);
            out += ",\"last_heard\":";
            out += jsonNum((int)tempNodeInfo->last_heard);
            // Fall back to the node number for a node that has not sent its NodeInfo yet, so
            // it is still identifiable rather than appearing as a blank row.
            out += ",\"has_user\":";
            out += hasUser ? "true" : "false";
            out += ",\"long_name\":";
            out += jsonEscape(hasUser ? tempNodeInfo->long_name : id);
            out += ",\"mac_address\":";
            out += jsonEscape("00:00:00:00:00:00");
            out += ",\"position\":";
            out += position;
            out += ",\"short_name\":";
            out += jsonEscape(hasUser ? tempNodeInfo->short_name : (id + 5));
            out += ",\"snr\":";
            out += jsonNum(tempNodeInfo->snr);
            out += ",\"via_mqtt\":";
            out += jsonEscape(BoolToString(nodeInfoLiteViaMqtt(tempNodeInfo)));

            // Telemetry a node reported about itself. NodeInfoLite does not carry it - it is
            // kept in a side map - so look it up rather than reading it off the entry.
            out += ",\"hops_away\":";
            out += tempNodeInfo->has_hops_away ? jsonNum((int)tempNodeInfo->hops_away) : "null";
            out += ",\"role\":";
            out += jsonNum((int)tempNodeInfo->role);
            out += ",\"self\":";
            out += (tempNodeInfo->num == nodeDB->getNodeNum()) ? "true" : "false";
            auto metrics = nodeDB->nodeTelemetry.find(tempNodeInfo->num);
            if (metrics != nodeDB->nodeTelemetry.end()) {
                const meshtastic_DeviceMetrics &dm = metrics->second;
                out += ",\"metrics\":{\"air_util_tx\":";
                out += dm.has_air_util_tx ? jsonNum(dm.air_util_tx) : "null";
                out += ",\"battery_level\":";
                out += dm.has_battery_level ? jsonNum((int)dm.battery_level) : "null";
                out += ",\"channel_utilization\":";
                out += dm.has_channel_utilization ? jsonNum(dm.channel_utilization) : "null";
                out += ",\"uptime_seconds\":";
                out += dm.has_uptime_seconds ? jsonNum((int)dm.uptime_seconds) : "null";
                out += ",\"voltage\":";
                out += dm.has_voltage ? jsonNum(dm.voltage) : "null";
                out += "}";
            } else {
                out += ",\"metrics\":null";
            }
            out += "}";
            if (out.size() >= NODES_FLUSH_BYTES) {
                if (!writeAll(res, out))
                    return;
                out.clear(); // keeps the capacity, so the buffer never grows past the mark
            }
        }
        tempNodeInfo = nodeDB->readNextMeshNode(readIndex);
    }

    out += "]},\"status\":\"ok\"}";
    writeAll(res, out);
}

void handleAdmin(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    //    res->println("<a href=/admin/settings>Settings</a><br>");
    //    res->println("<a href=/admin/fs>Manage Web Content</a><br>");
    res->println("<a href=/json/report>Device Report</a><br>");
}

void handleRestart(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "text/html");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");

    res->println("<h1>Meshtastic</h1>");
    res->println("Restarting");

    LOG_DEBUG("Restarted on HTTP(s) Request");
    webServerThread->requestRestart = (millis() / 1000) + 5;
}

void handleScanNetworks(HTTPRequest *req, HTTPResponse *res)
{
    res->setHeader("Content-Type", "application/json");
    res->setHeader("Access-Control-Allow-Origin", "*");
    res->setHeader("Access-Control-Allow-Methods", "GET");
    // res->setHeader("Content-Type", "text/html");

    int n = WiFi.scanNetworks();

    std::string out = "{\"data\":[";
    bool firstNet = true;
    if (n > 0) {
        for (int i = 0; i < n; ++i) {
            char ssidArray[50];
            // The previous implementation pre-escaped quotes before handing
            // the value to the JSON serializer; preserve that (byte-compatible
            // even if it double-encodes a quote) so existing clients are not
            // affected by this refactor.
            String ssidString = String(WiFi.SSID(i));
            ssidString.replace("\"", "\\\"");
            ssidString.toCharArray(ssidArray, 50);

            if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) {
                if (!firstNet)
                    out += ",";
                firstNet = false;
                out += "{\"rssi\":";
                out += jsonNum((int)WiFi.RSSI(i));
                out += ",\"ssid\":";
                out += jsonEscape(ssidArray);
                out += "}";
            }
            // Yield some cpu cycles to IP stack.
            //   This is important in case the list is large and it takes us time to return
            //   to the main loop.
            yield();
        }
    }
    out += "],\"status\":\"ok\"}";
    writeAll(res, out);
}
#endif
