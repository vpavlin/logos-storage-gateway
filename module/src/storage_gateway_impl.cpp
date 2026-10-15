#include "storage_gateway_impl.h"

#include "vendor/httplib.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

static const char* kVersion = "0.1.1";
static const char* kApiBase = "/api/storage/v1";
static const int kDefaultTimeoutMs = 20000;
// Transfers: a big file, a slow node or a fetch from the network take long between bytes.
static const int kTransferTimeoutMs = 10 * 60 * 1000;

// ── where the node is ────────────────────────────────────────────────────────

struct Conn {
    std::string url, host, base, token, network;
    int port = 0;
    int timeoutMs = kDefaultTimeoutMs;
};

// "http://[fd00::1]:8080", "http://node.mesh:8080/api/storage/v1", "[fd00::1]:8080", "node:8080".
static bool parseUrl(const std::string& in, Conn& c, std::string& err)
{
    std::string s = in;
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    if (s.empty()) { err = "No url for the Storage node."; return false; }
    std::string rest = s;
    const auto sep = s.find("://");
    if (sep != std::string::npos) {
        const std::string scheme = s.substr(0, sep);
        if (scheme == "https") { err = "https is not supported: reach the node over a VPN or mesh with http."; return false; }
        if (scheme != "http") { err = "The url must start with http://"; return false; }
        rest = s.substr(sep + 3);
    }
    std::string hostport = rest, path;
    const auto slash = rest.find('/');
    if (slash != std::string::npos) { hostport = rest.substr(0, slash); path = rest.substr(slash); }
    std::string host, port;
    if (!hostport.empty() && hostport[0] == '[') {
        const auto close = hostport.find(']');
        if (close == std::string::npos) { err = "The url has an unclosed [ in its address."; return false; }
        host = hostport.substr(1, close - 1);
        if (close + 1 < hostport.size() && hostport[close + 1] == ':') port = hostport.substr(close + 2);
    } else {
        const auto colon = hostport.rfind(':');
        if (colon != std::string::npos && hostport.find(':') == colon) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }
        else host = hostport;
    }
    if (host.empty()) { err = "The url has no host."; return false; }
    int p = 8080;
    if (!port.empty()) {
        char* end = nullptr;
        const long v = std::strtol(port.c_str(), &end, 10);
        if (!end || *end || v <= 0 || v > 65535) { err = "The url's port is not a number."; return false; }
        p = (int)v;
    }
    c.url = "http://" + (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(p) + path;
    c.host = host;
    c.port = p;
    c.base = path.empty() ? kApiBase : path;
    return true;
}

static std::string configFile()
{
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.logos_storage_gateway/config.json";
}

static json readConfig()
{
    std::ifstream f(configFile());
    if (!f) return json::object();
    json j = json::parse(f, nullptr, false);
    return j.is_object() ? j : json::object();
}

static std::string jstr(const json& o, const char* k)
{
    auto it = o.find(k);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// ── small helpers ────────────────────────────────────────────────────────────

static std::string pct(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') out += (char)ch;
        else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
    }
    return out;
}

static std::string base64(const char* data, size_t len)
{
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = ((unsigned char)data[i] << 16) | ((unsigned char)data[i + 1] << 8) | (unsigned char)data[i + 2];
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i < len) {
        uint32_t v = (unsigned char)data[i] << 16;
        if (i + 1 < len) v |= (unsigned char)data[i + 1] << 8;
        out += t[v >> 18]; out += t[(v >> 12) & 63];
        out += i + 1 < len ? t[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

static std::string newId()
{
    static std::mt19937_64 rng(std::random_device{}());
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    char buf[33];
    std::snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)rng(), (unsigned long long)rng());
    return buf;
}

// What the REST API takes as the file's type (it goes into the manifest, as storage_module's does).
static std::string mimeFor(const std::string& name)
{
    static const std::map<std::string, std::string> m = {
        {".txt", "text/plain"}, {".md", "text/plain"}, {".html", "text/html"}, {".htm", "text/html"},
        {".css", "text/css"}, {".csv", "text/csv"}, {".json", "application/json"}, {".js", "text/javascript"},
        {".xml", "application/xml"}, {".pdf", "application/pdf"}, {".zip", "application/zip"},
        {".gz", "application/gzip"}, {".tar", "application/x-tar"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
        {".svg", "image/svg+xml"}, {".mp3", "audio/mpeg"}, {".ogg", "audio/ogg"}, {".wav", "audio/wav"},
        {".mp4", "video/mp4"}, {".webm", "video/webm"}, {".mkv", "video/x-matroska"}};
    std::string ext = fs::path(name).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    auto it = m.find(ext);
    return it != m.end() ? it->second : "application/octet-stream";
}

static std::string dispositionName(const std::string& name)
{
    std::string n;
    for (char ch : name) if (ch != '"' && ch != '\\' && ch != '\r' && ch != '\n') n += ch;
    return n.empty() ? "file" : n;
}

// ── state shared with the transfer threads (which may outlive the module object) ─────────────────

struct Session {
    std::atomic<bool> cancel{false};
    // uploadInit/uploadChunk: the bytes wait in a temp file until uploadFinalize.
    bool manual = false;
    std::string tmp, filename;
    bool advertise = true;
    int64_t bytes = 0;
};

struct GatewayState {
    std::mutex m;
    StorageGatewayImpl* impl = nullptr;   // null once the module is gone: no more events
    Conn conn;
    bool configured = false;
    std::atomic<bool> running{false};
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::atomic<int> workers{0};

    Conn connCopy() { std::lock_guard<std::mutex> g(m); return conn; }
    bool ready() { std::lock_guard<std::mutex> g(m); return configured; }
};

using Event = void (StorageGatewayImpl::*)(const std::string&);

static void emitEvent(const std::shared_ptr<GatewayState>& s, Event e, const json& payload)
{
    std::string data;
    try { data = payload.dump(); } catch (...) { return; }
    std::lock_guard<std::mutex> g(s->m);
    if (!s->impl) return;
    try { (s->impl->*e)(data); } catch (...) {}
}

// A thread for one transfer; it holds the state alive, and the module waits for it a little on unload.
template <typename F>
static void worker(const std::shared_ptr<GatewayState>& s, F f)
{
    s->workers++;
    std::thread([s, f]() mutable {
        try { f(); } catch (...) {}
        s->workers--;
    }).detach();
}

static std::unique_ptr<httplib::Client> client(const Conn& c, int timeoutMs)
{
    auto cl = std::make_unique<httplib::Client>(c.host, c.port);
    const auto t = std::chrono::milliseconds(timeoutMs);
    cl->set_connection_timeout(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds(std::min(timeoutMs, 15000))).count(), 0);
    cl->set_read_timeout(std::chrono::duration_cast<std::chrono::seconds>(t).count(), 0);
    cl->set_write_timeout(std::chrono::duration_cast<std::chrono::seconds>(t).count(), 0);
    if (!c.token.empty()) cl->set_bearer_token_auth(c.token);
    return cl;
}

// Why a request failed, for the person: unreachable, or the node's own answer.
static std::string failure(const Conn& c, const httplib::Result& r)
{
    if (!r) return "Could not reach the Storage node at " + c.url + " (" + httplib::to_string(r.error()) + ").";
    std::string body = r->body;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' ')) body.pop_back();
    if (body.size() > 300) body = body.substr(0, 300) + "…";
    return "The Storage node answered " + std::to_string(r->status) + (body.empty() ? "" : ": " + body);
}

static bool ok(const httplib::Result& r) { return r && r->status >= 200 && r->status < 300; }

// A short call that answers JSON: {ok, value or error}.
static StdLogosResult getJson(const Conn& c, const std::string& path)
{
    auto cl = client(c, c.timeoutMs);
    auto r = cl->Get(c.base + path);
    if (!ok(r)) return {false, {}, failure(c, r)};
    json j = json::parse(r->body, nullptr, false);
    if (j.is_discarded()) return {false, {}, "The Storage node's answer was not JSON."};
    return {true, j, ""};
}

static StdLogosResult notConfigured()
{
    return {false, {}, "No Storage node: call init() with {\"url\": \"http://…\"} first."};
}

static StdLogosResult privateRefused()
{
    return {false, {}, "Private (Mix) transfers are not offered through the gateway; use isPrivate = false."};
}

// The manifest from {"cid", "manifest": {...}}, flattened as storage_module's manifests() gives it.
static json flatManifest(const json& item)
{
    json m = item.contains("manifest") && item["manifest"].is_object() ? item["manifest"] : json::object();
    json out = {{"cid", jstr(item, "cid")}};
    for (const char* k : {"treeCid", "datasetSize", "blockSize", "filename", "mimetype"})
        if (m.contains(k)) out[k] = m[k];
    return out;
}

// ── the module ───────────────────────────────────────────────────────────────

StorageGatewayImpl::StorageGatewayImpl() : st(std::make_shared<GatewayState>())
{
    st->impl = this;
    // The last configuration, so a client that forgets init() still reaches its node.
    json saved = readConfig();
    Conn c;
    std::string err;
    if (!jstr(saved, "url").empty() && parseUrl(jstr(saved, "url"), c, err)) {
        c.token = jstr(saved, "token");
        if (saved.contains("timeout") && saved["timeout"].is_number_integer()) c.timeoutMs = std::max(1000, saved["timeout"].get<int>());
        st->conn = c;
        st->configured = true;
    }
}

StorageGatewayImpl::~StorageGatewayImpl()
{
    {
        std::lock_guard<std::mutex> g(st->m);
        st->impl = nullptr;
        for (auto& [id, s] : st->sessions) s->cancel = true;
    }
    // Transfers notice the cancel between chunks; give them a moment, the state outlives us anyway.
    for (int i = 0; i < 30 && st->workers > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

StdLogosResult StorageGatewayImpl::loadConfigOrDefault()
{
    json saved = readConfig();
    if (jstr(saved, "url").empty()) saved = {{"url", ""}};
    return {true, saved.dump(), ""};
}

bool StorageGatewayImpl::init(const std::string& cfg)
{
    json j = json::parse(cfg, nullptr, false);
    if (!j.is_object()) return false;
    const bool given = !jstr(j, "url").empty();
    if (!given) j = readConfig();
    Conn c;
    std::string err;
    if (jstr(j, "url").empty() || !parseUrl(jstr(j, "url"), c, err)) {
        if (!err.empty()) std::fprintf(stderr, "storage_gateway: %s\n", err.c_str());
        return false;
    }
    c.token = jstr(j, "token");
    if (j.contains("timeout") && j["timeout"].is_number_integer()) c.timeoutMs = std::max(1000, j["timeout"].get<int>());
    {
        std::lock_guard<std::mutex> g(st->m);
        st->conn = c;
        st->configured = true;
    }
    st->running = false;
    if (given) {
        std::error_code ec;
        fs::create_directories(fs::path(configFile()).parent_path(), ec);
        json save = {{"url", c.url}};
        if (!c.token.empty()) save["token"] = c.token;
        if (c.timeoutMs != kDefaultTimeoutMs) save["timeout"] = c.timeoutMs;
        std::ofstream f(configFile(), std::ios::trunc);
        f << save.dump(2) << "\n";
        if (!c.token.empty()) fs::permissions(configFile(), fs::perms::owner_read | fs::perms::owner_write, ec);
    }
    return true;
}

bool StorageGatewayImpl::start()
{
    if (!st->ready()) return false;
    auto s = st;
    worker(s, [s] {
        const Conn c = s->connCopy();
        auto r = getJson(c, "/debug/info");
        s->running = r.success;
        if (r.success)
            emitEvent(s, &StorageGatewayImpl::storageStart,
                      {{"success", true}, {"message", "Using the Storage node at " + c.url + " (" + jstr(r.value, "id") + ")"}});
        else
            emitEvent(s, &StorageGatewayImpl::storageStart, {{"success", false}, {"message", r.error}});
    });
    return true;
}

StdLogosResult StorageGatewayImpl::stop()
{
    st->running = false;
    emitEvent(st, &StorageGatewayImpl::storageStop, {{"success", true}, {"message", "No longer using the Storage node (it keeps running)."}});
    return {true, {}, ""};
}

StdLogosResult StorageGatewayImpl::destroy()
{
    std::lock_guard<std::mutex> g(st->m);
    for (auto& [id, s] : st->sessions) s->cancel = true;
    st->configured = false;
    st->running = false;
    return {true, {}, ""};
}

bool StorageGatewayImpl::isRunning() { return st->running; }

StdLogosResult StorageGatewayImpl::libstorageVersion()
{
    if (!st->ready()) return notConfigured();
    auto r = getJson(st->connCopy(), "/debug/info");
    if (!r.success) return r;
    json s = r.value.contains("storage") && r.value["storage"].is_object() ? r.value["storage"] : json::object();
    return {true, jstr(s, "version"), ""};
}

std::string StorageGatewayImpl::moduleVersion() { return kVersion; }

StdLogosResult StorageGatewayImpl::dataDir()
{
    if (!st->ready()) return notConfigured();
    return {true, st->connCopy().url, ""};
}

StdLogosResult StorageGatewayImpl::network()
{
    if (!st->ready()) return notConfigured();
    auto r = getJson(st->connCopy(), "/debug/info");
    if (!r.success) return r;
    return {true, jstr(r.value, "network"), ""};
}

StdLogosResult StorageGatewayImpl::peerId()
{
    if (!st->ready()) return notConfigured();
    auto r = getJson(st->connCopy(), "/peerid");
    if (!r.success) return r;
    return {true, jstr(r.value, "id"), ""};
}

StdLogosResult StorageGatewayImpl::spr()
{
    if (!st->ready()) return notConfigured();
    auto r = getJson(st->connCopy(), "/spr");
    if (!r.success) return r;
    return {true, jstr(r.value, "spr"), ""};
}

StdLogosResult StorageGatewayImpl::debug()
{
    if (!st->ready()) return notConfigured();
    return getJson(st->connCopy(), "/debug/info");
}

LogosMap StorageGatewayImpl::collectMetrics() { return json{{"metrics", json::array()}}; }

StdLogosResult StorageGatewayImpl::updateLogLevel(const std::string& logLevel)
{
    if (!st->ready()) return notConfigured();
    const Conn c = st->connCopy();
    auto cl = client(c, c.timeoutMs);
    auto r = cl->Post(c.base + "/debug/chronicles/loglevel?level=" + pct(logLevel), "", "text/plain");
    if (!ok(r)) return {false, {}, failure(c, r)};
    return {true, {}, ""};
}

StdLogosResult StorageGatewayImpl::connect(const std::string& peerId, const std::vector<std::string>& peerAddresses)
{
    if (!st->ready()) return notConfigured();
    if (peerId.empty()) return {false, {}, "No peer ID."};
    auto s = st;
    worker(s, [s, peerId, peerAddresses] {
        const Conn c = s->connCopy();
        std::string q;
        for (auto& a : peerAddresses) q += (q.empty() ? "?" : "&") + std::string("addrs=") + pct(a);
        auto cl = client(c, std::max(c.timeoutMs, 60000));
        auto r = cl->Get(c.base + "/connect/" + pct(peerId) + q);
        emitEvent(s, &StorageGatewayImpl::storageConnect,
                  ok(r) ? json{{"success", true}, {"message", "Connected to " + peerId}}
                        : json{{"success", false}, {"message", failure(c, r)}});
    });
    return {true, {}, ""};
}

// ── upload ───────────────────────────────────────────────────────────────────

// POSTs a file to /data, reporting progress per percent; returns {cid, error}.
static std::pair<std::string, std::string> postFile(const std::shared_ptr<GatewayState>& s, const Conn& c,
                                                    const std::string& sessionId, const std::shared_ptr<Session>& sess,
                                                    const std::string& path, const std::string& filename, bool advertise,
                                                    size_t chunkSize, bool reportProgress)
{
    std::error_code ec;
    const auto total = (int64_t)fs::file_size(path, ec);
    if (ec) return {"", "Cannot read " + path + ": " + ec.message()};
    std::vector<char> buf(std::max<size_t>(chunkSize, 4096));
    // The node checks the type against its own list (Nim's std/mimetypes, which lacks e.g. text/markdown)
    // and refuses one it doesn't know with 422: then once more without a type.
    httplib::Result r;
    for (const std::string& type : {mimeFor(filename), std::string()}) {
        auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
        if (!*in) return {"", "Cannot open " + path};
        httplib::Headers h = {{"Content-Disposition", "attachment; filename=\"" + dispositionName(filename) + "\""}};
        auto cl = client(c, kTransferTimeoutMs);
        int lastPct = -1;
        r = cl->Post(c.base + std::string("/data?advertise=") + (advertise ? "true" : "false"), h, (size_t)total,
            [&](size_t offset, size_t length, httplib::DataSink& sink) {
                if (sess->cancel) return false;
                const size_t n = std::min(length, buf.size());
                in->seekg((std::streamoff)offset);
                in->read(buf.data(), (std::streamsize)n);
                const auto got = (size_t)in->gcount();
                if (got == 0) return false;
                if (!sink.write(buf.data(), got)) return false;
                const int64_t done = (int64_t)(offset + got);
                const int p = total > 0 ? (int)(done * 100 / total) : 100;
                if (reportProgress && p != lastPct) {
                    lastPct = p;
                    emitEvent(s, &StorageGatewayImpl::storageUploadProgress,
                              {{"success", true}, {"sessionId", sessionId}, {"bytes", done}, {"total", total}});
                }
                return true;
            },
            type);
        const bool badType = r && r->status == 422 && r->body.find("MIME") != std::string::npos;
        if (!badType || type.empty() || sess->cancel) break;
    }
    if (sess->cancel) return {"", "Cancelled."};
    if (!ok(r)) return {"", failure(c, r)};
    std::string cid = r->body;
    while (!cid.empty() && (cid.back() == '\n' || cid.back() == '\r' || cid.back() == ' ')) cid.pop_back();
    if (cid.empty()) return {"", "The Storage node did not answer with a CID."};
    return {cid, ""};
}

StdLogosResult StorageGatewayImpl::uploadUrl(const std::string& filePath, int64_t chunkSize, bool advertise)
{
    if (!st->ready()) return notConfigured();
    std::string path = filePath;
    if (path.rfind("file://", 0) == 0) path = path.substr(7);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return {false, {}, "Not a file: " + path};
    const std::string id = newId();
    auto sess = std::make_shared<Session>();
    { std::lock_guard<std::mutex> g(st->m); st->sessions[id] = sess; }
    auto s = st;
    const size_t chunk = chunkSize > 0 ? (size_t)chunkSize : 65536;
    worker(s, [s, id, sess, path, advertise, chunk] {
        const Conn c = s->connCopy();
        auto [cid, err] = postFile(s, c, id, sess, path, fs::path(path).filename().string(), advertise, chunk, true);
        { std::lock_guard<std::mutex> g(s->m); s->sessions.erase(id); }
        emitEvent(s, &StorageGatewayImpl::storageUploadDone,
                  err.empty() ? json{{"success", true}, {"sessionId", id}, {"cid", cid}}
                              : json{{"success", false}, {"sessionId", id}, {"error", err}});
    });
    return {true, id, ""};
}

StdLogosResult StorageGatewayImpl::uploadInit(const std::string& filename, int64_t chunkSize, bool advertise)
{
    (void)chunkSize;
    if (!st->ready()) return notConfigured();
    const std::string id = newId();
    auto sess = std::make_shared<Session>();
    sess->manual = true;
    sess->filename = filename.empty() ? "file" : fs::path(filename).filename().string();
    sess->advertise = advertise;
    std::error_code ec;
    sess->tmp = (fs::temp_directory_path(ec) / ("storage-gateway-" + id)).string();
    std::ofstream f(sess->tmp, std::ios::binary | std::ios::trunc);
    if (!f) return {false, {}, "Cannot create a temporary file for the upload."};
    std::lock_guard<std::mutex> g(st->m);
    st->sessions[id] = sess;
    return {true, id, ""};
}

StdLogosResult StorageGatewayImpl::uploadChunk(const std::string& sessionId, const std::string& chunk)
{
    std::shared_ptr<Session> sess;
    {
        std::lock_guard<std::mutex> g(st->m);
        auto it = st->sessions.find(sessionId);
        if (it != st->sessions.end() && it->second->manual) sess = it->second;
    }
    if (!sess) return {false, {}, "No such upload session."};
    std::ofstream f(sess->tmp, std::ios::binary | std::ios::app);
    f.write(chunk.data(), (std::streamsize)chunk.size());
    if (!f) {
        emitEvent(st, &StorageGatewayImpl::storageUploadProgress,
                  {{"success", false}, {"sessionId", sessionId}, {"total", sess->bytes}, {"error", "Cannot write the chunk."}});
        return {false, {}, "Cannot write the chunk."};
    }
    sess->bytes += (int64_t)chunk.size();
    emitEvent(st, &StorageGatewayImpl::storageUploadProgress,
              {{"success", true}, {"sessionId", sessionId}, {"bytes", sess->bytes}, {"total", sess->bytes}});
    return {true, {}, ""};
}

StdLogosResult StorageGatewayImpl::uploadFinalize(const std::string& sessionId)
{
    std::shared_ptr<Session> sess;
    {
        std::lock_guard<std::mutex> g(st->m);
        auto it = st->sessions.find(sessionId);
        if (it != st->sessions.end() && it->second->manual) sess = it->second;
    }
    if (!sess) return {false, {}, "No such upload session."};
    auto [cid, err] = postFile(st, st->connCopy(), sessionId, sess, sess->tmp, sess->filename, sess->advertise, 65536, false);
    std::error_code ec;
    fs::remove(sess->tmp, ec);
    { std::lock_guard<std::mutex> g(st->m); st->sessions.erase(sessionId); }
    if (!err.empty()) return {false, {}, err};
    return {true, cid, ""};
}

StdLogosResult StorageGatewayImpl::uploadCancel(const std::string& sessionId)
{
    std::shared_ptr<Session> sess;
    {
        std::lock_guard<std::mutex> g(st->m);
        auto it = st->sessions.find(sessionId);
        if (it == st->sessions.end()) return {false, {}, "No such upload session."};
        sess = it->second;
        if (sess->manual) st->sessions.erase(it);
    }
    sess->cancel = true;
    if (sess->manual) { std::error_code ec; fs::remove(sess->tmp, ec); }
    return {true, {}, ""};
}

// ── download ─────────────────────────────────────────────────────────────────

// Streams a CID from the node; each piece goes to `sink`. Returns "" or why it failed.
static std::string getStream(const std::shared_ptr<GatewayState>& s, const Conn& c, const std::string& cid,
                             bool local, bool advertise, const std::shared_ptr<Session>& sess,
                             const std::function<bool(const char*, size_t, int64_t)>& sink, int64_t& total)
{
    const std::string adv = std::string("?advertise=") + (advertise ? "true" : "false");
    // The size first, for the progress (as storage_module does); it also fetches the manifest when remote.
    {
        auto cl = client(c, std::max(c.timeoutMs, 90000));
        auto r = cl->Get(c.base + "/data/" + pct(cid) + "/network/manifest" + adv);
        if (!ok(r)) return failure(c, r);
        json j = json::parse(r->body, nullptr, false);
        total = j.is_object() && j.contains("manifest") && j["manifest"].contains("datasetSize") && j["manifest"]["datasetSize"].is_number()
                    ? j["manifest"]["datasetSize"].get<int64_t>() : 0;
    }
    (void)s;
    auto cl = client(c, kTransferTimeoutMs);
    int status = 0;
    std::string errBody;
    int64_t got = 0;
    auto r = cl->Get(local ? c.base + "/data/" + pct(cid) : c.base + "/data/" + pct(cid) + "/network/stream" + adv,
        [&](const httplib::Response& resp) { status = resp.status; return true; },
        [&](const char* data, size_t len) {
            if (sess->cancel) return false;
            if (status < 200 || status >= 300) { if (errBody.size() < 300) errBody.append(data, std::min(len, (size_t)300)); return true; }
            got += (int64_t)len;
            return sink(data, len, got);
        });
    if (sess->cancel) return "Cancelled.";
    if (!r && status == 0) return failure(c, r);
    if (status < 200 || status >= 300) return "The Storage node answered " + std::to_string(status) + (errBody.empty() ? "" : ": " + errBody);
    if (!r) return failure(c, r);
    return "";
}

static std::shared_ptr<Session> addDownload(const std::shared_ptr<GatewayState>& s, const std::string& id)
{
    auto sess = std::make_shared<Session>();
    std::lock_guard<std::mutex> g(s->m);
    if (auto it = s->sessions.find(id); it != s->sessions.end()) it->second->cancel = true;
    s->sessions[id] = sess;
    return sess;
}

static void endDownload(const std::shared_ptr<GatewayState>& s, const std::string& id, const std::shared_ptr<Session>& sess, const std::string& err)
{
    {
        std::lock_guard<std::mutex> g(s->m);
        auto it = s->sessions.find(id);
        if (it != s->sessions.end() && it->second == sess) s->sessions.erase(it);
    }
    emitEvent(s, &StorageGatewayImpl::storageDownloadDone,
              err.empty() ? json{{"success", true}, {"sessionId", id}} : json{{"success", false}, {"sessionId", id}, {"error", err}});
}

StdLogosResult StorageGatewayImpl::downloadToUrl(const std::string& cid, const std::string& filePath, bool local, int64_t chunkSize,
                                                 bool isPrivate, bool advertise)
{
    (void)chunkSize;
    if (!st->ready()) return notConfigured();
    if (isPrivate) return privateRefused();
    if (cid.empty()) return {false, {}, "No CID."};
    std::string path = filePath;
    if (path.rfind("file://", 0) == 0) path = path.substr(7);
    if (path.empty()) return {false, {}, "No file to write to."};
    auto sess = addDownload(st, cid);
    auto s = st;
    worker(s, [s, cid, path, local, advertise, sess] {
        const Conn c = s->connCopy();
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) { endDownload(s, cid, sess, "Cannot write " + path); return; }
        int64_t total = 0;
        int lastPct = -1;
        const std::string err = getStream(s, c, cid, local, advertise, sess,
            [&](const char* data, size_t len, int64_t got) {
                out.write(data, (std::streamsize)len);
                if (!out) return false;
                const int p = total > 0 ? (int)std::min<int64_t>(100, got * 100 / total) : 0;
                if (p != lastPct) {
                    lastPct = p;
                    emitEvent(s, &StorageGatewayImpl::storageDownloadProgress,
                              {{"success", true}, {"sessionId", cid}, {"bytes", got}, {"total", total}});
                }
                return true;
            }, total);
        out.close();
        const bool written = !out.fail();
        if (!err.empty() || !written) { std::error_code ec; fs::remove(path, ec); }
        endDownload(s, cid, sess, !err.empty() ? err : written ? "" : "Cannot write " + path);
    });
    return {true, cid, ""};
}

StdLogosResult StorageGatewayImpl::downloadChunks(const std::string& cid, bool local, int64_t chunkSize, bool isPrivate, bool advertise)
{
    if (!st->ready()) return notConfigured();
    if (isPrivate) return privateRefused();
    if (cid.empty()) return {false, {}, "No CID."};
    auto sess = addDownload(st, cid);
    auto s = st;
    const size_t size = chunkSize > 0 ? (size_t)chunkSize : 65536;
    worker(s, [s, cid, local, advertise, sess, size] {
        const Conn c = s->connCopy();
        std::string pending;
        int64_t total = 0;
        auto flush = [&] {
            if (pending.empty()) return;
            emitEvent(s, &StorageGatewayImpl::storageDownloadProgress,
                      {{"success", true}, {"sessionId", cid}, {"chunk", base64(pending.data(), pending.size())}});
            pending.clear();
        };
        const std::string err = getStream(s, c, cid, local, advertise, sess,
            [&](const char* data, size_t len, int64_t) {
                pending.append(data, len);
                while (pending.size() >= size) {
                    emitEvent(s, &StorageGatewayImpl::storageDownloadProgress,
                              {{"success", true}, {"sessionId", cid}, {"chunk", base64(pending.data(), size)}});
                    pending.erase(0, size);
                }
                return true;
            }, total);
        if (err.empty()) flush();
        endDownload(s, cid, sess, err);
    });
    return {true, cid, ""};
}

StdLogosResult StorageGatewayImpl::downloadCancel(const std::string& sessionId)
{
    std::lock_guard<std::mutex> g(st->m);
    auto it = st->sessions.find(sessionId);
    if (it == st->sessions.end() || it->second->manual) return {false, {}, "No such download session."};
    it->second->cancel = true;
    return {true, {}, ""};
}

// ── the node's data ──────────────────────────────────────────────────────────

StdLogosResult StorageGatewayImpl::exists(const std::string& cid)
{
    if (!st->ready()) return notConfigured();
    auto r = getJson(st->connCopy(), "/data/" + pct(cid) + "/exists");
    if (!r.success) return r;
    auto it = r.value.find(cid);
    return {true, it != r.value.end() && it->is_boolean() && it->get<bool>(), ""};
}

StdLogosResult StorageGatewayImpl::fetch(const std::string& cid, bool isPrivate, bool advertise)
{
    if (!st->ready()) return notConfigured();
    if (isPrivate) return privateRefused();
    const Conn c = st->connCopy();
    auto cl = client(c, c.timeoutMs);
    auto r = cl->Post(c.base + "/data/" + pct(cid) + "/network?advertise=" + (advertise ? "true" : "false"), "", "text/plain");
    if (!ok(r)) return {false, {}, failure(c, r)};
    return {true, {}, ""};
}

StdLogosResult StorageGatewayImpl::getAdvertise(const std::string& cid)
{
    if (!st->ready()) return notConfigured();
    auto r = getJson(st->connCopy(), "/data/" + pct(cid) + "/advertise");
    if (!r.success) return r;
    auto it = r.value.find("advertise");
    return {true, it != r.value.end() && it->is_boolean() && it->get<bool>(), ""};
}

StdLogosResult StorageGatewayImpl::setAdvertise(const std::string& cid, bool advertise)
{
    if (!st->ready()) return notConfigured();
    const Conn c = st->connCopy();
    auto cl = client(c, c.timeoutMs);
    auto r = cl->Post(c.base + "/data/" + pct(cid) + "/advertise?advertise=" + (advertise ? "true" : "false"), "", "text/plain");
    if (!ok(r)) return {false, {}, failure(c, r)};
    return {true, {}, ""};
}

StdLogosResult StorageGatewayImpl::remove(const std::string& cid)
{
    if (!st->ready()) return notConfigured();
    if (cid.empty()) return {false, {}, "No CID."};
    auto s = st;
    worker(s, [s, cid] {
        const Conn c = s->connCopy();
        auto cl = client(c, std::max(c.timeoutMs, 60000));
        auto r = cl->Delete(c.base + "/data/" + pct(cid));
        emitEvent(s, &StorageGatewayImpl::storageRemoveDone,
                  ok(r) ? json{{"success", true}, {"cid", cid}} : json{{"success", false}, {"cid", cid}, {"error", failure(c, r)}});
    });
    return {true, {}, ""};
}

StdLogosResult StorageGatewayImpl::space()
{
    if (!st->ready()) return notConfigured();
    return getJson(st->connCopy(), "/space");
}

StdLogosResult StorageGatewayImpl::manifests()
{
    if (!st->ready()) return notConfigured();
    auto r = getJson(st->connCopy(), "/data");
    if (!r.success) return r;
    json out = json::array();
    if (r.value.contains("content") && r.value["content"].is_array())
        for (auto& item : r.value["content"]) if (item.is_object()) out.push_back(flatManifest(item));
    return {true, out, ""};
}

StdLogosResult StorageGatewayImpl::downloadManifest(const std::string& cid, bool isPrivate, bool advertise)
{
    if (!st->ready()) return notConfigured();
    if (isPrivate) return privateRefused();
    auto s = st;
    worker(s, [s, cid, advertise] {
        const Conn c = s->connCopy();
        auto cl = client(c, std::max(c.timeoutMs, 90000));
        auto r = cl->Get(c.base + "/data/" + pct(cid) + "/network/manifest?advertise=" + (advertise ? "true" : "false"));
        json j = ok(r) ? json::parse(r->body, nullptr, false) : json();
        if (ok(r) && j.is_object() && j.contains("manifest"))
            emitEvent(s, &StorageGatewayImpl::storageDownloadManifestDone, {{"success", true}, {"cid", cid}, {"manifest", j["manifest"]}});
        else
            emitEvent(s, &StorageGatewayImpl::storageDownloadManifestDone,
                      {{"success", false}, {"cid", cid}, {"error", ok(r) ? "The Storage node's answer had no manifest." : failure(c, r)}});
    });
    return {true, {}, ""};
}

void StorageGatewayImpl::importFiles(const std::string& path)
{
    std::error_code ec;
    for (auto& e : fs::directory_iterator(path, ec))
        if (e.is_regular_file(ec)) uploadUrl(e.path().string(), 65536, true);
}
