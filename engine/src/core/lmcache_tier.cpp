#include "strata/core/lmcache_tier.hpp"

#include "strata/core/lmcache_chunk.hpp"

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

// The head is its own object, and it must not share a key with any window: the store resolves a key's token range into
// one object per chunk (a record can never span two) and keeps the first object it was given for a key, so two records
// over one range are one record - the second is dropped and the first is what everybody reads.  Its range is therefore
// [0, chunk) like the first window, and the two are told apart by the only thing this store certainly hashes: the
// tokens.  The head's key stream is the conversation's first chunk with its opening tokens replaced by a digest of the
// length and of the whole prefix; no prompt produces that stream, so the head can never be confused with a window or
// with another turn's head.
std::vector<int32_t> head_key_tokens(const std::vector<int32_t>& tokens, int64_t end, int64_t chunk) {
    const size_t n = std::min<size_t>(size_t(std::max<int64_t>(end, 0)), tokens.size());
    std::vector<int32_t> key(size_t(std::max<int64_t>(chunk, 0)), 0);
    std::copy(tokens.begin(), tokens.begin() + (std::ptrdiff_t) std::min<size_t>(n, key.size()), key.begin());
    SessionHasher h(0x686561646b6579ull);   // "headkey"
    h.update(&end, sizeof end);
    h.update(tokens.data(), n * sizeof(int32_t));
    const uint64_t d = h.digest();
    if (key.size() > 0) key[0] = int32_t(uint32_t(d));
    if (key.size() > 1) key[1] = int32_t(uint32_t(d >> 32));
    if (key.size() > 2) key[2] = int32_t(uint32_t(d * 0x9e3779b1u));
    if (key.size() > 3) key[3] = 0x48454144;   // "HEAD": a head's key, never a prompt's own stream
    return key;
}

// One record = one image serialized in the session format.  A published conversation is a handful of them: a HEAD
// record (its tokens, checkpoints and running state, together with the first window's K/V) and one record per further
// 256-token window of K/V.
//
// The head rides in the first window's record because of a hard rule of the store: a key's token range is resolved
// into one object per chunk and the commit must carry exactly that many tensors, so a record can never span more than
// one chunk - and two records may never share one, either.  The store keeps the first object it was given for a key
// and its lookup does not reliably separate two records over the same range by their cache_salt (two records over
// [0, 256) came back as each other in testing).  Giving the head its own range is therefore impossible, and folding it
// into the first window's record costs one object per turn: the head changes on every turn, so the first window is
// written again while the others are not.
//
// Each record is the same format a session file has, so the hash, the identity and the limits all come for free; what
// changes is the composition, and that is what makes a growing conversation cheap: its earlier windows stay in the
// store.
class RamSink : public SessionSink {
public:
    bool ok() const override { return true; }
    std::string error() const override { return std::string(); }
    SessionError kind() const override { return SessionError::none; }
    bool begin(uint64_t total) override {
        buf_.clear();
        buf_.reserve(size_t(total));
        return true;
    }
    uint8_t* span(size_t& room) override {
        room = kSessionBlock;
        const size_t at = buf_.size();
        buf_.resize(at + room);
        return reinterpret_cast<uint8_t*>(&buf_[at]);
    }
    bool commit(size_t n) override {
        buf_.resize(buf_.size() - (kSessionBlock - n));
        return true;
    }
    bool write(const void* p, size_t n) override {
        buf_.append(static_cast<const char*>(p), n);
        return true;
    }
    bool finish(bool) override { return true; }
    const std::string& bytes() const { return buf_; }

private:
    std::string buf_;
};

class RamSource : public SessionSource {
public:
    explicit RamSource(std::string bytes) : bytes_(std::move(bytes)) {
        // An object is padded to the shape the store was registered with, so the bytes are longer than the record.
        // The record's own header says how long it is (the payload length sits at offset 32 of the header).
        if (bytes_.size() >= kSessionHeaderBytes) {
            uint64_t payload = 0;
            for (int i = 7; i >= 0; --i) payload = (payload << 8) | uint8_t(bytes_[32 + i]);
            const uint64_t total = kSessionHeaderBytes + payload + kSessionTrailerBytes;
            if (total <= bytes_.size()) bytes_.resize(size_t(total));
        }
    }
    bool ok() const override { return true; }
    std::string error() const override { return std::string(); }
    SessionError kind() const override { return SessionError::none; }
    bool begin(uint64_t& total) override {
        total = bytes_.size();
        return true;
    }
    bool read(void* p, size_t n) override {
        if (at_ + n > bytes_.size()) return false;
        std::memcpy(p, bytes_.data() + at_, n);
        at_ += n;
        return true;
    }

private:
    std::string bytes_;
    size_t at_ = 0;
};

// Serializes one record and hands it to the store; `salt` says which kind it is ("" = a K/V window).
bool put_record(LmcacheStore& store, const SavedConversation& image, const std::vector<SessionKvSource>* kv,
                const std::vector<int32_t>& tokens, int64_t start, int64_t end, const char* salt,
                const SessionFileIdentity& id, std::string& why) {
    RamSink sink;
    SessionStatus st;
    size_t bytes = 0;
    const bool wrote = kv ? session_stream_write(sink, image, *kv, id, bytes, why, &st)
                          : session_stream_write(sink, image, id, bytes, why, &st);
    if (!wrote) return false;
    return store.put_object(tokens, start, end, sink.bytes().data(), sink.bytes().size(), why, salt);
}

// Reads one record back, or reports a miss.
bool get_record(LmcacheStore& store, const std::vector<int32_t>& tokens, int64_t start, int64_t end, const char* salt,
                const SessionFileIdentity& id, const SessionReadLimits& limits, SavedConversation& out, bool& present,
                std::string& why) {
    std::string bytes;
    if (!store.get_object(tokens, start, end, bytes, present, why, salt)) return false;
    if (!present) return true;
    RamSource source(std::move(bytes));
    size_t read = 0;
    SessionStatus st;
    if (!session_stream_read(source, id, out, read, why, limits, &st)) return false;
    return true;
}

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
    const int64_t chunk = store_->chunk_tokens();
    const bool shaped = kv ? chunk_size_ok_from(*kv, chunk, tokens, why) : chunk_size_ok(image, chunk, tokens, why);
    if (!shaped) {
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    const int64_t total = chunk_windows(tokens, chunk);
    const auto t0 = std::chrono::steady_clock::now();
    // 1. the head: the conversation's tokens, checkpoints and running state, in one small object of its own (see
    //    head_key_tokens).  It is rewritten on every publish, because it grows with the conversation.
    {
        SavedConversation head = image;
        for (ConversationKv& k : head.kv) {
            k.k = ConversationBuffer{};
            k.v = ConversationBuffer{};
            k.k_scale = ConversationBuffer{};
            k.v_scale = ConversationBuffer{};
            k.pooled = ConversationBuffer{};
            k.cells = 0;
            k.pooled_rows = 0;
        }
        head.live.ids = image.live.ids;
        head.live.ids.resize(size_t(total * chunk));   // what this publish covers, whole windows only
        for (auto it = head.live.imgs.begin(); it != head.live.imgs.end();)
            it = it->start >= total * chunk ? head.live.imgs.erase(it) : it + 1;
        const std::vector<int32_t> hkey = head_key_tokens(tokens, total * chunk, chunk);
        if (!put_record(*store_, head, nullptr, hkey, 0, chunk, nullptr, id, why)) {
            ++stats_.failed;
            stats_.connected = false;
            return false;
        }
    }
    // 2. the windows the store does not hold yet.  Since a conversation only grows, the ones written by an earlier
    //    publish are still there: writing them again would send the whole state on every turn.
    const int64_t already = published_windows(tokens, chunk);
    int64_t written = 0;
    for (int64_t i = already; i < total; ++i) {
        ChunkWindow window{i * chunk, chunk};
        SavedConversation piece;
        const bool cut = kv ? chunk_extract_from(image, *kv, window, piece, why)
                            : chunk_extract(image, window, piece, why);
        if (!cut) {
            ++stats_.failed;
            stats_.connected = false;
            return false;
        }
        if (!put_record(*store_, piece, nullptr, tokens, window.first, window.first + chunk, nullptr, id, why)) {
            ++stats_.failed;
            stats_.connected = false;
            return false;
        }
        ++written;
    }
    remember_published(tokens, total, chunk);
    if (written > 0) {
        ++stats_.published;
        stats_.published_bytes += int64_t(written) * int64_t(store_->object_bytes());
    }
    stats_.connected = true;
    stats_.last_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

int64_t LmcacheTier::published_windows(const std::vector<int32_t>& tokens, int64_t chunk) const {
    for (auto it = published_.rbegin(); it != published_.rend(); ++it) {
        if (it->ids.size() > tokens.size() || !std::equal(it->ids.begin(), it->ids.end(), tokens.begin())) continue;
        return std::min(it->windows, int64_t(tokens.size()) / chunk);
    }
    return 0;
}

void LmcacheTier::remember_published(const std::vector<int32_t>& tokens, int64_t windows, int64_t chunk) {
    const size_t held = std::min(tokens.size(), size_t(std::max<int64_t>(windows, 0) * chunk));
    for (auto it = published_.begin(); it != published_.end(); ++it) {
        // The same conversation, now as long or longer: the entry follows it rather than piling a second one up, so
        // one conversation keeps one entry no matter how many turns it takes.
        if (it->ids.size() <= held && std::equal(it->ids.begin(), it->ids.end(), tokens.begin())) {
            if (held > it->ids.size()) it->ids.assign(tokens.begin(), tokens.begin() + (std::ptrdiff_t) held);
            it->windows = std::max(it->windows, windows);
            return;
        }
    }
    published_.push_back(Published{std::vector<int32_t>(tokens.begin(), tokens.begin() + (std::ptrdiff_t) held), windows});
    // One instance serves several conversations at once (an agent's main thread, its subagents, another person's chat),
    // and a conversation only grows: forgetting one after two others means its next turn re-sends every window it
    // already has - the whole state, on every turn, which is what the cutting up exists to avoid.  Eight is more than
    // the RAM tier's four parked slots, and each entry is only the tokens of the windows it covers (a 200 K-token
    // conversation: 800 KB).
    while (published_.size() > 8) published_.erase(published_.begin());
}

bool LmcacheTier::restore(const std::vector<int32_t>& tokens, const SessionFileIdentity& id, SavedConversation& out,
                          const SessionReadLimits& limits, std::string& why) {
    return restore_impl(tokens, id, 0, out, limits, why);   // no RAM prefix to beat
}

bool LmcacheTier::restore(const std::vector<int32_t>& tokens, const SessionFileIdentity& id, int64_t floor_tokens,
                          SavedConversation& out, const SessionReadLimits& limits, std::string& why) {
    return restore_impl(tokens, id, floor_tokens, out, limits, why);
}

bool LmcacheTier::restore_impl(const std::vector<int32_t>& tokens, const SessionFileIdentity& id, int64_t floor_tokens,
                               SavedConversation& out, const SessionReadLimits& limits, std::string& why) {
    why.clear();
    if (!enabled()) {
        why = "the shared cache is off";
        return false;
    }
    ++stats_.probes;
    // The same cost gate the publish side has.  Without it every request - including a two-line question whose prefix
    // is far below the gate - probes the store, and a store that does not answer costs the caller the whole lookup
    // timeout before the prefill can start.  Below the gate a transfer could never pay for the read it saves anyway.
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
    const int64_t chunk = store_->chunk_tokens();
    const auto t0 = std::chrono::steady_clock::now();
    // 1. the head: the conversation's tokens, checkpoints and running state (see head_key_tokens).  Its length says how
    //    many windows follow, so one lookup decides both "is it there" and "how much of it" - after walking the lengths
    //    down, since the head of a shorter turn has another key.
    SavedConversation head;
    bool present = false;
    int64_t total = 0;
    const int64_t want = (int64_t(tokens.size()) / chunk) * chunk;
    for (int64_t end = want, tries = 0; end >= chunk && tries < 8; end -= chunk, ++tries) {
        const std::vector<int32_t> hkey = head_key_tokens(tokens, end, chunk);
        if (!get_record(*store_, hkey, 0, chunk, nullptr, id, limits, head, present, why)) {
            ++stats_.failed;
            stats_.connected = false;
            return false;
        }
        if (present) {
            total = end / chunk;
            break;
        }
    }
    if (!present || total <= 0) {
        stats_.connected = true;   // the store answered: it simply does not have this prefix
        why = "no session for this prefix";
        return false;
    }
    if (int64_t(head.live.ids.size()) != total * chunk) {
        stats_.connected = true;
        why = "the head does not cover the length it was found under";
        return false;
    }
    // The store's own key is the prompt's chunk hashes, and a head covers the whole conversation while its key covers
    // one chunk: two conversations that open with the same 256 tokens can therefore be handed each other's head.  The
    // head carries the tokens it belongs to, so this is where that is refused - a miss (recompute) and never another
    // conversation's state.
    if (!std::equal(head.live.ids.begin(), head.live.ids.end(), tokens.begin())) {
        stats_.connected = true;
        why = "the head belongs to another conversation with the same opening tokens";
        return false;
    }
    // Not longer than what the caller already holds in its own RAM tier: it could never be used, and a turn that the
    // RAM tier wins must not pay for the store's whole state.  The head alone was enough to know.
    if (total * chunk <= floor_tokens) {
        stats_.connected = true;
        why = "the store's state is not longer than the RAM tier's (" + std::to_string(total * chunk) + " <= " +
              std::to_string(floor_tokens) + " tokens)";
        return false;
    }
    // 2. the first window: it gives the image its shape, and loses the head's running state onto it.
    SavedConversation first;
    if (!get_record(*store_, tokens, 0, chunk, nullptr, id, limits, first, present, why)) {
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    if (!present) {
        stats_.connected = true;
        why = "the store is missing window 0 of " + std::to_string(total);
        return false;
    }
    SavedConversation image;
    if (!chunk_reserve(image, first, total * chunk, why)) {
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    image.live = head.live;
    image.checkpoints = head.checkpoints;
    if (!chunk_merge(image, first, ChunkWindow{0, chunk}, why)) {
        ++stats_.failed;
        stats_.connected = false;
        return false;
    }
    // 3. every further window.  A chain that lost one is not a shorter session - it is an unusable one, because the
    //    head's running arrays belong to the whole conversation.
    for (int64_t i = 1; i < total; ++i) {
        SavedConversation piece;
        if (!get_record(*store_, tokens, i * chunk, (i + 1) * chunk, nullptr, id, limits, piece, present, why)) {
            ++stats_.failed;
            stats_.connected = false;
            return false;
        }
        if (!present) {
            stats_.connected = true;
            why = "the store is missing window " + std::to_string(i) + " of " + std::to_string(total);
            return false;
        }
        if (!chunk_merge(image, piece, ChunkWindow{i * chunk, chunk}, why)) {
            ++stats_.failed;
            stats_.connected = false;
            return false;
        }
    }
    out = std::move(image);
    remember_published(tokens, total, chunk);   // what is in the store is now known to be there
    ++stats_.hits;
    stats_.restored += 1;
    stats_.restored_bytes += int64_t(out.bytes());
    stats_.connected = true;
    stats_.last_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace strata::core
