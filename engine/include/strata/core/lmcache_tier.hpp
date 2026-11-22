// The shared cache tier above the conversation cache: what it is configured with, and what it reports (docs/LMCACHE.md).
//
// The store is LMCache in its "MP" mode, run as a container on the same host or on a cache node.  Two planes:
//   * data   - a ZMQ ROUTER we connect to as a DEALER (lmcache_wire.hpp), where a session's bytes are stored and read;
//   * report - a plain HTTP API on its own port, where the monitor reads "how full is the store" (/status).
// One setting names the store; the rest tune the policy, and every one of them is also a STRATA_* environment flag, so
// a launcher's model file (its `args` + `env`), a compose file and a plain `docker run -e` all reach the same place.
//
// Nothing here talks to the store: this is the settings and the accounting the engine's INFO line carries, so an
// operator can see the tier's configuration and its effect without reading the store's logs.
#pragma once

#include "strata/core/conversation_file.hpp"
#include "strata/core/lmcache_store.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace strata::core {

// What the tier counts while it runs.  All of it is per process and per model: the store may hold other models'
// entries, and these numbers never include them.
struct LmcacheStats {
    bool enabled = false;
    bool connected = false;         // the data plane answered since the last reconnect attempt
    int64_t published = 0;          // sessions written to the store
    int64_t published_bytes = 0;
    int64_t restored = 0;           // sessions read back from it
    int64_t restored_bytes = 0;
    int64_t probes = 0;             // lookups that asked the store whether it has a session
    int64_t hits = 0;
    int64_t rejected = 0;           // refused by the cost gate (too short to be worth a transfer)
    int64_t failed = 0;             // store errors; the request went on without it
    int64_t last_ms = 0;            // how long the last publish or restore took
};

struct LmcacheSettings {
    bool enabled = false;
    std::string rpc;                // STRATA_LMCACHE:        tcp://host:5555 - the data plane
    std::string http;               // STRATA_LMCACHE_HTTP:   http://host:8080 - the report plane (empty: off)
    std::string model;              // STRATA_LMCACHE_NAMESPACE: the name this engine registers under (empty: from the model)
    int64_t bytes_per_token = 19531;  // STRATA_LMCACHE_BYTES_PER_TOKEN: the state's size per token, fixed per namespace
    int64_t min_tokens = 8192;      // STRATA_LMCACHE_MIN_TOKENS: shorter prefixes are not worth a transfer
    // STRATA_LMCACHE_REPLACE_RAM: the store takes the place of the in-process conversation cache.  A finished
    // conversation is published and NOT kept in this process's RAM, so several engines on one host stop each holding
    // their own copy.  The price is that every reuse - including the same instance's next turn - pays a store round
    // trip instead of a buffer handoff, so this is for hosts that would rather have the RAM back.
    bool replace_ram = false;
};

// Reads the environment once.  `STRATA_LMCACHE` empty (the default) turns the tier off, and nothing else is looked at:
// a model that never sets it behaves exactly as before this feature.
LmcacheSettings lmcache_settings();

// `http://host:8080` for a `tcp://host:5555` store, so one flag configures both planes (the default port is the
// server's).  Empty in, empty out.
std::string lmcache_http_from_rpc(const std::string& rpc);

// The tier's configuration and counters as one INFO line fragment, e.g.
//   lmcache=on store=tcp://nas:5555 model=qwen38 min_tokens=8192 per_token=19531 http=http://nas:8080
// and, once it has run, published=3 published_mib=3600 restored=1 hit=1 miss=2 failed=0 last_ms=4120.
// The engine appends it to its INFO line, so /metrics, the monitor and the logs all carry it without a second channel.
std::string lmcache_info_line(const LmcacheSettings& settings, const LmcacheStats& stats);

// The tier itself.  It owns the store connection and turns one conversation into a chain of chunk objects, and back.
//
//   publish(): serialize with session_stream_write into a sink that cuts the stream at the store's object size, so the
//             1.2 GB state is never held twice: the K/V layers are read from their owners straight into the object
//             buffer (P1's SessionSink).
//   restore(): session_stream_read over a source that fetches one object at a time.  A miss costs one object; the
//             caller's image is only replaced once the whole stream, its hashes and its identity check out.
//
// Neither call throws and neither can fail a request: an unreachable, slow or full store only means no reuse, which is
// exactly the behaviour of the tier being off.
class LmcacheTier {
public:
    LmcacheTier(LmcacheSettings settings, std::unique_ptr<LmcacheStore> store);
    ~LmcacheTier();
    LmcacheTier(const LmcacheTier&) = delete;
    LmcacheTier& operator=(const LmcacheTier&) = delete;

    bool enabled() const { return settings_.enabled && store_ != nullptr; }
    const LmcacheSettings& settings() const { return settings_; }
    const LmcacheStats& stats() const { return stats_; }

    // Publish `image` under `tokens` (the conversation's own ids, oldest first).  The prefix that is stored is
    // floor(tokens / chunk) chunks; false + `why` means "not published" for any reason, never a broken caller.
    // The first form carries the K/V inside the image (a parked conversation), the second pulls it from its owners.
    bool publish(const SavedConversation& image, const SessionFileIdentity& id, const std::vector<int32_t>& tokens,
                 std::string& why);
    bool publish(const SavedConversation& image, const std::vector<SessionKvSource>& kv,
                 const std::vector<int32_t>& tokens, const SessionFileIdentity& id, std::string& why);

    // Read a session back.  False + `why` for "the store has no session for this prefix" as well as for a real error;
    // `out` is untouched unless the whole stream was restored and validated.
    bool restore(const std::vector<int32_t>& tokens, const SessionFileIdentity& id, SavedConversation& out,
                 const SessionReadLimits& limits, std::string& why);

private:
    bool publish_impl(const SavedConversation& image, const std::vector<SessionKvSource>* kv,
                      const std::vector<int32_t>& tokens, const SessionFileIdentity& id, std::string& why);

    LmcacheSettings settings_;
    std::unique_ptr<LmcacheStore> store_;
    LmcacheStats stats_;
};

}  // namespace strata::core
