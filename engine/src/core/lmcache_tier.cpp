#include "strata/core/lmcache_tier.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace strata::core {
namespace {
// The environment is the engine's settings contract (tools/settings_register.py collects every name): a value that is
// absent, empty or unparsable keeps the default, so a typo degrades to the documented behaviour instead of failing a
// launch that has nothing to do with the cache.
std::string env_text(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return {};
    std::string s(v);
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

int64_t env_int(const char* name, int64_t fallback, int64_t lo, int64_t hi) {
    const std::string t = env_text(name);
    if (t.empty()) return fallback;
    char* end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, 10);
    if (end == t.c_str() || (end && *end != '\0')) return fallback;   // "8k", "yes": keep the default
    return std::min(hi, std::max(lo, (int64_t) v));
}
}  // namespace

std::string lmcache_http_from_rpc(const std::string& rpc) {
    std::string rest = rpc;
    const std::string scheme = "tcp://";
    if (rest.compare(0, scheme.size(), scheme) == 0) rest = rest.substr(scheme.size());
    if (rest.empty()) return {};
    const size_t colon = rest.rfind(':');
    const std::string host = colon == std::string::npos ? rest : rest.substr(0, colon);
    if (host.empty()) return {};
    // the server's HTTP port is fixed next to the data port, so one setting names the store
    return "http://" + host + ":8080";
}

LmcacheSettings lmcache_settings() {
    LmcacheSettings s;
    s.rpc = env_text("STRATA_LMCACHE");
    s.enabled = !s.rpc.empty();
    if (!s.enabled) return s;   // off: every other flag is ignored, as if it were not there
    s.http = env_text("STRATA_LMCACHE_HTTP");
    if (s.http.empty()) s.http = lmcache_http_from_rpc(s.rpc);
    s.model = env_text("STRATA_LMCACHE_NAMESPACE");
    // The state's size per token fixes the object shape, and both ends must agree on it: it is the namespace's
    // geometry, so it is a setting rather than something derived per session.
    s.bytes_per_token = env_int("STRATA_LMCACHE_BYTES_PER_TOKEN", 19531, 16, int64_t(1) << 24);
    // below 256 tokens there is no chunk to store, and a transfer of a few MB never pays for itself
    s.min_tokens = env_int("STRATA_LMCACHE_MIN_TOKENS", 8192, 256, int64_t(1) << 22);
    s.replace_ram = env_int("STRATA_LMCACHE_REPLACE_RAM", 0, 0, 1) != 0;
    return s;
}

std::string lmcache_info_line(const LmcacheSettings& s, const LmcacheStats& st) {
    char b[512];
    if (!s.enabled) {
        std::snprintf(b, sizeof b, "lmcache=off\n");
        return b;
    }
    std::snprintf(b, sizeof b,
                  "lmcache=on store=%s model=%s min_tokens=%lld per_token=%lld replace_ram=%d http=%s\n", s.rpc.c_str(),
                  s.model.empty() ? "(model)" : s.model.c_str(), (long long) s.min_tokens,
                  (long long) s.bytes_per_token, s.replace_ram ? 1 : 0, s.http.c_str());
    std::string out(b);
    std::snprintf(b, sizeof b,
                  "lmcache_state connected=%d published=%lld published_mib=%lld restored=%lld restored_mib=%lld "
                  "hit=%lld miss=%lld rejected=%lld failed=%lld last_ms=%lld\n",
                  st.connected ? 1 : 0, (long long) st.published, (long long) (st.published_bytes >> 20),
                  (long long) st.restored, (long long) (st.restored_bytes >> 20), (long long) st.hits,
                  (long long) std::max<int64_t>(0, st.probes - st.hits), (long long) st.rejected, (long long) st.failed,
                  (long long) st.last_ms);
    out += b;
    return out;
}



// ---- the tier: one conversation <-> a chain of chunk objects ----------------------------------------------------
namespace {
// Serializes into the store: the same 16 MiB-blocked walk the file layer uses, cut again at the store's object size,
// so a 1.2 GB state is handed over object by object and never held twice.
class StoreSink : public SessionSink {
public:
    StoreSink(LmcacheStore& store, const std::vector<int32_t>& tokens, std::string& why)
        : store_(store), tokens_(tokens), why_(why) {}

    bool ok() const override { return ok_; }
    std::string error() const override { return why_; }
    SessionError kind() const override { return SessionError::io; }

    bool begin(uint64_t total) override {
        total_ = total;
        object_ = store_.object_bytes();
        chunk_ = store_.chunk_tokens();
        if (object_ == 0 || chunk_ <= 0) {
            ok_ = false;
            why_ = "the store reports no object shape";
            return false;
        }
        // only whole chunks of the prefix can be addressed, and the state has to fit in them
        const int64_t usable_tokens = (int64_t(tokens_.size()) / chunk_) * chunk_;
        const int64_t available = usable_tokens / chunk_;
        objects_ = int64_t((total + object_ - 1) / object_);
        if (available <= 0) {
            ok_ = false;
            why_ = "the conversation is shorter than one chunk (" + std::to_string(tokens_.size()) + " tokens)";
            return false;
        }
        if (objects_ > available) {
            ok_ = false;
            why_ = "the state needs " + std::to_string(objects_) + " objects but the prefix holds " +
                   std::to_string(available) + " (raise STRATA_LMCACHE_BYTES_PER_TOKEN?)";
            return false;
        }
        buf_.assign(object_, 0);
        return true;
    }

    uint8_t* span(size_t& room) override {
        room = object_ - used_;   // one whole object at a time: commit() can hand it over without a copy
        return buf_.data() + used_;
    }

    bool commit(size_t n) override {
        used_ += n;
        if (used_ < object_) return true;
        return flush();
    }

    bool write(const void* p, size_t n) override {
        const uint8_t* s = static_cast<const uint8_t*>(p);
        while (n) {
            size_t room = 0;
            uint8_t* d = span(room);
            const size_t c = n < room ? n : room;
            std::memcpy(d, s, c);
            if (!commit(c)) return false;
            s += c;
            n -= c;
        }
        return true;
    }

    bool finish(bool) override {
        if (!ok_) return false;
        if (used_ > 0 || index_ == 0) return flush();   // the padded tail, or a state smaller than one object
        return true;
    }

    int64_t objects() const { return index_; }

private:
    bool flush() {
        const int64_t start = index_ * chunk_;
        const int64_t end = start + chunk_;
        if (end > int64_t(tokens_.size())) {
            ok_ = false;
            why_ = "the prefix ended before the state did";
            return false;
        }
        if (used_ < object_) std::memset(buf_.data() + used_, 0, object_ - used_);   // the tail is padding, not data
        if (!store_.put_object(tokens_, start, end, buf_.data(), object_, why_)) {
            ok_ = false;
            return false;
        }
        ++index_;
        used_ = 0;
        return true;
    }

    LmcacheStore& store_;
    const std::vector<int32_t>& tokens_;
    std::string& why_;
    std::vector<uint8_t> buf_;
    size_t object_ = 0, used_ = 0;
    int64_t chunk_ = 0, objects_ = 0;
    uint64_t total_ = 0;
    int64_t index_ = 0;
    bool ok_ = true;   // "nothing has gone wrong yet": begin() is what decides whether this sink can work
};

// Reads the chain back, one object at a time.  The first object carries the stream's header, which says how long the
// stream is - that is what lets this answer begin() without a separate manifest.
class StoreSource : public SessionSource {
public:
    StoreSource(LmcacheStore& store, const std::vector<int32_t>& tokens, std::string& why)
        : store_(store), tokens_(tokens), why_(why) {}

    bool ok() const override { return ok_; }
    std::string error() const override { return why_; }
    SessionError kind() const override { return SessionError::io; }
    bool missed() const { return missed_; }

    bool begin(uint64_t& total) override {
        object_ = store_.object_bytes();
        chunk_ = store_.chunk_tokens();
        if (object_ == 0 || chunk_ <= 0) {
            why_ = "the store reports no object shape";
            return false;
        }
        if (!fetch(0)) return false;
        if (cur_.size() < kSessionHeaderBytes) {
            why_ = "the first object is shorter than a session header";
            return false;
        }
        // header v1, little-endian: payload length at offset 32
        uint64_t payload = 0;
        for (int i = 7; i >= 0; --i) payload = payload << 8 | uint8_t(cur_[32 + i]);
        total_ = kSessionHeaderBytes + payload + kSessionTrailerBytes;
        objects_ = int64_t((total_ + object_ - 1) / object_);
        available_ = cur_.size();
        at_ = 0;
        served_ = 0;
        total = total_;
        return true;
    }

    bool read(void* p, size_t n) override {
        uint8_t* d = static_cast<uint8_t*>(p);
        while (n) {
            if (at_ == available_) {
                if (!fetch(++index_)) return false;
                available_ = cur_.size();
                at_ = 0;
            }
            const size_t c = std::min(n, available_ - at_);
            std::memcpy(d, cur_.data() + at_, c);
            at_ += c;
            d += c;
            n -= c;
            served_ += c;
        }
        return true;
    }

private:
    bool fetch(int64_t index) {
        const int64_t start = index * chunk_;
        const int64_t end = start + chunk_;
        if (end > int64_t(tokens_.size())) {
            why_ = "the prefix is shorter than the stored session";
            return false;
        }
        bool present = false;
        if (!store_.get_object(tokens_, start, end, cur_, present, why_)) return false;
        if (!present) {
            missed_ = index == 0;   // nothing there at all; a later gap is a store that lost an object
            why_ = index == 0 ? "no session for this prefix" : "the store is missing object " + std::to_string(index);
            return false;
        }
        return true;
    }

    LmcacheStore& store_;
    const std::vector<int32_t>& tokens_;
    std::string& why_;
    std::string cur_;
    size_t object_ = 0, available_ = 0, at_ = 0;
    int64_t chunk_ = 0, objects_ = 0, index_ = 0;
    uint64_t total_ = 0, served_ = 0;
    bool ok_ = true, missed_ = false;
};
}  // namespace

LmcacheTier::LmcacheTier(LmcacheSettings settings, std::unique_ptr<LmcacheStore> store)
    : settings_(std::move(settings)), store_(std::move(store)) {
    stats_.enabled = enabled();
}

LmcacheTier::~LmcacheTier() = default;

bool LmcacheTier::publish(const SavedConversation& image, const SessionFileIdentity& id,
                          const std::vector<int32_t>& tokens, std::string& why) {
    return publish_impl(image, nullptr, tokens, id, why);   // the K/V is in the image (a parked conversation)
}

bool LmcacheTier::publish(const SavedConversation& image, const std::vector<SessionKvSource>& kv,
                          const std::vector<int32_t>& tokens, const SessionFileIdentity& id, std::string& why) {
    return publish_impl(image, &kv, tokens, id, why);       // the K/V is pulled from its owners as it goes
}

bool LmcacheTier::publish_impl(const SavedConversation& image, const std::vector<SessionKvSource>* kv,
                               const std::vector<int32_t>& tokens, const SessionFileIdentity& id, std::string& why) {
    why.clear();
    if (!enabled()) {
        why = "the shared cache is off";
        return false;
    }
    // the cost gate: below this a transfer costs more than the recompute it saves
    if (int64_t(tokens.size()) < settings_.min_tokens) {
        ++stats_.rejected;
        why = "prefix of " + std::to_string(tokens.size()) + " tokens is below the cost gate (" +
              std::to_string(settings_.min_tokens) + ")";
        return false;
    }
    if (!store_->open(why)) {
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    StoreSink sink(*store_, tokens, why);
    SessionStatus st;
    size_t bytes = 0;
    const bool wrote = kv ? session_stream_write(sink, image, *kv, id, bytes, why, &st)
                          : session_stream_write(sink, image, id, bytes, why, &st);
    if (!wrote) {
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    ++stats_.published;
    stats_.published_bytes += int64_t(bytes);
    stats_.connected = true;
    stats_.last_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

bool LmcacheTier::restore(const std::vector<int32_t>& tokens, const SessionFileIdentity& id, SavedConversation& out,
                          const SessionReadLimits& limits, std::string& why) {
    why.clear();
    if (!enabled()) {
        why = "the shared cache is off";
        return false;
    }
    ++stats_.probes;
    // the same cost gate as publishing: a short prefix is not worth an object fetch (which is a miss anyway)
    if (int64_t(tokens.size()) < settings_.min_tokens) {
        why = "prefix of " + std::to_string(tokens.size()) + " tokens is below the cost gate (" +
              std::to_string(settings_.min_tokens) + ")";
        return false;
    }
    if (!store_->open(why)) {
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    StoreSource source(*store_, tokens, why);
    SessionStatus st;
    size_t bytes = 0;
    // the read validates the header hash, the identity, every object's length, the payload hash and the end marker:
    // a session that was written by another model or configuration is refused here, before `out` is touched
    if (!session_stream_read(source, id, out, bytes, why, limits, &st)) {
        if (source.missed()) {
            stats_.connected = true;   // the store answered: it simply does not have this prefix
            return false;
        }
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    ++stats_.hits;
    stats_.restored += 1;
    stats_.restored_bytes += int64_t(bytes);
    stats_.connected = true;
    stats_.last_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace strata::core
