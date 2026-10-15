# Logos Storage Gateway

`storage_gateway` is a Basecamp module with the **same API as `storage_module`** (3.0): every
method, argument and event, the same JSON. But it doesn't run a Storage node on this computer:
it uses one that runs somewhere else, through that node's REST API.

Why: a Storage node on a laptop sits behind NAT and sleeps with the laptop, so what it holds is
often unreachable for others. A node on an always-on server with a public address serves your
files to the network all the time. With the gateway, an app on the laptop uses that node as if
it were its own: uploads go from files on this computer to the node, downloads come back into
files on this computer.

Because the API is identical, an app can offer both: its own `storage_module`, or the gateway
when the person gives a node's address. The calls are the same; only the module differs.
[Dufs](https://github.com/vpavlin/dufs-basecamp) does this.

## Use

```cpp
auto& sg = modules().storage_gateway;
sg.init(R"({"url": "http://[fd00::1]:8080"})");   // the node's REST API
sg.onStorageUploadDone([](const std::string& p) { /* {"success", "sessionId", "cid"} */ });
sg.start();                                        // storageStart: does the node answer?
sg.uploadUrlAsyncResult("/home/me/photo.jpg", 65536, true, cb, 20000);
```

`init()` takes `{"url", "token", "timeout"}`:

- `url` is the REST API's address, `http://host:port`; `/api/storage/v1` is added unless the
  url has a path;
- `token` is optional, sent as `Authorization: Bearer`, for a node behind a proxy that checks it;
- `timeout` is for short calls, in ms (default 20000).

The last configuration is saved in `~/.logos_storage_gateway/config.json`, and `init("{}")` takes it.

### What differs from storage_module

- **Mix privacy.** `isPrivate` is not offered by the REST API, so a private download, fetch or
  manifest request is refused.
- **The node itself.** `start()`/`stop()` don't start or stop the remote node: `start()` checks
  that it answers.
- **dataDir() and network().** `dataDir()` returns the url, and `network()` returns what the
  node reports (usually "").
- **Metrics.** `collectMetrics()` returns no metrics.

## The node

Any `logos-storage` node (v0.5.x, the same libstorage generation as storage_module 3.0) with
its REST API on:

```sh
logos-storage --data-dir=/srv/storage --listen-port=8499 --nat=extip:<public IP> \
  --network=logos.test --api-bindaddr=<VPN/mesh address> --api-port=8480 \
  --autonat-server=true --relay-server=true
```

**The REST API has no authentication.** Bind it only to an address that your own devices
reach: a WireGuard/VPN/mesh address, or localhost behind an authenticating proxy. Never bind
it to a public address. The peer-to-peer port (`--listen-port`) is the one to make public, so
the node can serve its data to the network. The flags `--api-*` are hidden from `--help` but
present.

## Build

```sh
cd module && nix build .#lgx-portable
```

HTTP is [cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT, vendored in
`module/src/vendor`). It speaks plain http: run it over an encrypted VPN or mesh.

## License

MIT or Apache-2.0, at your option.
