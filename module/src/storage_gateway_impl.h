#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <logos_json.h>
#include <logos_module_context.h>
#include <logos_result.h>

struct GatewayState;

/// Logos Storage Gateway: the storage_module API, served by a Logos Storage node elsewhere.
///
/// Every method, argument and event of storage_module 3.0 (same names, same JSON), so an app can
/// call this module or storage_module with the same code. Instead of running a node, it talks to
/// a node's REST API (logos-storage, `--api-bindaddr`), e.g. one on a server reached over a VPN
/// or mesh. Files are read from and written to this computer, as with storage_module; the data
/// itself lives on the remote node, which also serves it to the network.
///
/// Configure with init(): {"url": "http://[fd00::1]:8080"} (the REST API's base; "/api/storage/v1"
/// is added unless the url has a path), optional "token" (sent as "Authorization: Bearer", for a
/// node behind a proxy that checks it) and "timeout" (ms, for short calls; default 20000).
/// init("{}") takes the configuration saved by the last init() that had a url.
///
/// Differences from storage_module, by nature of a remote node:
///  - isPrivate (Mix) is not offered by the REST API: a private download or fetch is refused;
///  - dataDir() returns the url, network() what the node says it runs (or "");
///  - start()/stop() do not start or stop the remote node: start() checks that it answers.
class StorageGatewayImpl : public LogosModuleContext {
public:
    StorageGatewayImpl();
    ~StorageGatewayImpl();

    /// The configuration saved by the last init() with a url, else {"url": ""}.
    /// Returns StdLogosResult::value as a JSON string.
    StdLogosResult loadConfigOrDefault();

    /// Configure the gateway: {"url", "token", "timeout"}; "{}" takes the saved configuration.
    /// Returns true when there is a url to use. The method is synchronous.
    bool init(const std::string& cfg);

    /// Check that the node answers; `storageStart` says whether it does.
    /// Returns true if there is a node to ask. The method is asynchronous.
    bool start();

    /// Stop using the node (it keeps running); emits `storageStop`.
    StdLogosResult stop();

    /// Forget the configuration in memory (not the saved one).
    StdLogosResult destroy();

    /// True after a start() that reached the node, until stop() or destroy().
    bool isRunning();

    /// The remote node's version, e.g. "v0.5.3".
    StdLogosResult libstorageVersion();

    /// This module's version, as declared in `metadata.json`.
    std::string moduleVersion();

    /// The node's url (a remote node's data directory is not known).
    StdLogosResult dataDir();

    /// The Logos network the node runs, when it says; else "".
    StdLogosResult network();

    /// The node's peer ID. Returns StdLogosResult::value as a std::string.
    StdLogosResult peerId();

    /// The node's Signed Peer Record. Returns StdLogosResult::value as a std::string.
    StdLogosResult spr();

    /// The node's debug information (id, addrs, spr, table, ...), as a JSON object.
    StdLogosResult debug();

    /// For the openmetrics module: the REST API has no metrics, so { "metrics": [] }.
    LogosMap collectMetrics();

    /// Set the node's log level: TRACE, DEBUG, INFO, NOTICE, WARN, ERROR or FATAL.
    StdLogosResult updateLogLevel(const std::string& logLevel);

    /// Ask the node to connect to a peer; `storageConnect` says how it went.
    StdLogosResult connect(const std::string& peerId, const std::vector<std::string>& peerAddresses);

    /// Upload a file on this computer to the node. Returns the session ID; progress via
    /// `storageUploadProgress`, the CID via `storageUploadDone`.
    StdLogosResult uploadUrl(const std::string& filePath, int64_t chunkSize, bool advertise);

    /// A manual upload session: uploadChunk() the bytes, then uploadFinalize() for the CID.
    StdLogosResult uploadInit(const std::string& filename, int64_t chunkSize, bool advertise);

    /// One chunk of raw bytes for a session from uploadInit(); emits `storageUploadProgress`.
    StdLogosResult uploadChunk(const std::string& sessionId, const std::string& chunk);

    /// Send the session's bytes to the node. Returns StdLogosResult::value as the CID string.
    StdLogosResult uploadFinalize(const std::string& sessionId);

    /// Cancel an upload session.
    StdLogosResult uploadCancel(const std::string& sessionId);

    /// Download a CID into a file on this computer. `local`: only what the node holds.
    /// Returns the session ID (= CID); progress via `storageDownloadProgress`, the end via
    /// `storageDownloadDone`.
    StdLogosResult downloadToUrl(const std::string& cid, const std::string& filePath, bool local, int64_t chunkSize, bool isPrivate, bool advertise);

    /// Download a CID as base64 chunks in `storageDownloadProgress` events.
    StdLogosResult downloadChunks(const std::string& cid, bool local, int64_t chunkSize, bool isPrivate, bool advertise);

    /// Cancel a download session.
    StdLogosResult downloadCancel(const std::string& sessionId);

    /// Whether the node holds the CID. Returns StdLogosResult::value as bool.
    StdLogosResult exists(const std::string& cid);

    /// Have the node fetch a CID from the network in the background.
    StdLogosResult fetch(const std::string& cid, bool isPrivate, bool advertise);

    /// Whether the node announces and serves the dataset. Returns StdLogosResult::value as bool.
    StdLogosResult getAdvertise(const std::string& cid);

    /// Start or stop announcing and serving the dataset.
    StdLogosResult setAdvertise(const std::string& cid, bool advertise);

    /// Remove a CID from the node; the outcome via `storageRemoveDone`.
    StdLogosResult remove(const std::string& cid);

    /// The node's space: {totalBlocks, quotaMaxBytes, quotaUsedBytes, quotaReservedBytes}.
    StdLogosResult space();

    /// The manifests the node holds: [{cid, treeCid, datasetSize, blockSize, filename, mimetype}].
    StdLogosResult manifests();

    /// Fetch a CID's manifest; the outcome via `storageDownloadManifestDone`.
    StdLogosResult downloadManifest(const std::string& cid, bool isPrivate, bool advertise);

    /// Upload every regular file in a directory on this computer (advertised).
    void importFiles(const std::string& path);

logos_events:
    /// {"success": bool, "message": string}
    void storageStart(const std::string& payload);

    /// {"success": bool, "message": string}
    void storageStop(const std::string& payload);

    /// {"success": bool, "message": string}
    void storageConnect(const std::string& payload);

    /// {"success": bool, "sessionId": string, "bytes": number, "total": number, "error": string}
    void storageUploadProgress(const std::string& payload);

    /// {"success": bool, "sessionId": string, "cid": string, "error": string}
    void storageUploadDone(const std::string& payload);

    /// {"success": true, "sessionId": string, "bytes": number, "total": number, "chunk": string}
    void storageDownloadProgress(const std::string& payload);

    /// {"success": bool, "sessionId": string, "error": string}
    void storageDownloadDone(const std::string& payload);

    /// {"success": bool, "cid": string, "manifest": object, "error": string}
    void storageDownloadManifestDone(const std::string& payload);

    /// {"success": bool, "cid": string, "error": string}
    void storageRemoveDone(const std::string& payload);

private:
    std::shared_ptr<GatewayState> st;
};
