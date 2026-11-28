// CPU-only tests for the shared cache tier (include/strata/core/lmcache_tier.hpp): its settings, its INFO line, and the
// whole publish/restore path against an in-memory store that behaves like the real one (objects, misses, failures).
//
// The store's wire format is pinned separately (lmcache_wire_test); what is proved here is that a conversation
// survives a round trip through it, and that the objects really carry the same bytes the file layer writes.
#include "strata/core/lmcache_tier.hpp"

#include <array>
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace strata::core;
namespace fs = std::filesystem;

namespace {
int checks = 0, failures = 0;

void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

void same(const std::string& got, const std::string& want, const char* label) {
    ++checks;
    if (got != want) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  got  '%s'\n  want '%s'\n", label, got.c_str(), want.c_str());
    }
}

bool has(const std::string& text, const char* part, const char* label) {
    ++checks;
    if (text.find(part) == std::string::npos) {
        ++failures;
        std::fprintf(stderr, "FAIL %s: '%s' missing from '%s'\n", label, part, text.c_str());
        return false;
    }
    return true;
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

void clear_all() {
    for (const char* n : {"STRATA_LMCACHE", "STRATA_LMCACHE_HTTP", "STRATA_LMCACHE_NAMESPACE",
                          "STRATA_LMCACHE_MIN_TOKENS", "STRATA_LMCACHE_BYTES_PER_TOKEN",
                          "STRATA_LMCACHE_REPLACE_RAM"})
        set_env(n, nullptr);
}

// ---- settings --------------------------------------------------------------------------------------------------

// One setting would be a hidden feature: this is the whole configuration surface, so it is pinned here.
void settings_surface() {
    clear_all();
    {
        const LmcacheSettings s = lmcache_settings();
        check(!s.enabled, "no STRATA_LMCACHE: the tier is off");
        check(s.rpc.empty() && s.http.empty(), "off: no endpoints to speak of");
    }
    set_env("STRATA_LMCACHE", "tcp://192.168.121.134:5555");
    {
        const LmcacheSettings s = lmcache_settings();
        check(s.enabled, "STRATA_LMCACHE turns it on");
        same(s.rpc, "tcp://192.168.121.134:5555", "the data plane is the flag's value");
        same(s.http, "http://192.168.121.134:8080", "the report plane is derived from it");
        check(s.min_tokens == 8192, "the documented cost gate");
        check(s.bytes_per_token == 19531, "~19 KB per token at this geometry, as the default");
        check(!s.replace_ram, "the in-process cache stays the primary tier by default");
        check(s.model.empty(), "no namespace given: the engine's model name is used later");
    }
    set_env("STRATA_LMCACHE_HTTP", "http://cache-host:9090");
    {
        const LmcacheSettings s = lmcache_settings();
        same(s.http, "http://cache-host:9090", "an explicit report plane wins");
    }
    set_env("STRATA_LMCACHE_NAMESPACE", "qwen3.8-flash-next");
    set_env("STRATA_LMCACHE_MIN_TOKENS", "2048");
    set_env("STRATA_LMCACHE_BYTES_PER_TOKEN", "64");
    set_env("STRATA_LMCACHE_REPLACE_RAM", "1");
    {
        const LmcacheSettings s = lmcache_settings();
        same(s.model, "qwen3.8-flash-next", "the namespace is what the store is told");
        check(s.replace_ram, "REPLACE_RAM hands the finished conversation to the store instead");
        check(s.min_tokens == 2048, "the cost gate is configurable");
        check(s.bytes_per_token == 64, "the object shape is configurable (a test geometry, here)");
    }
    // a hand-edited file must not be able to produce a nonsensical tier
    set_env("STRATA_LMCACHE_MIN_TOKENS", "12");
    set_env("STRATA_LMCACHE_BYTES_PER_TOKEN", "0");
    {
        const LmcacheSettings s = lmcache_settings();
        check(s.min_tokens == 256, "below one chunk the value is raised to it");
        check(s.bytes_per_token == 16, "a zero object shape is raised to the floor");
    }
    clear_all();
    set_env("STRATA_LMCACHE", "tcp://nas");
    same(lmcache_settings().http, "http://nas:8080", "a bare host gets the server's ports");
    clear_all();
}

// The one INFO line the engine prints at start-up and per transfer: an operator reads the tier's whole configuration
// and effect there, so its shape is pinned.
void info_lines() {
    LmcacheSettings s;
    LmcacheStats st;
    same(lmcache_info_line(s, st), "lmcache=off\n", "off is one key, not a wall of empty ones");

    s.enabled = true;
    s.rpc = "tcp://nas:5555";
    s.http = "http://nas:8080";
    s.model = "qwen38";
    const std::string line = lmcache_info_line(s, st);
    has(line, "lmcache=on", "INFO says the tier is on");
    has(line, "store=tcp://nas:5555", "INFO carries the store");
    has(line, "model=qwen38", "INFO carries the namespace");
    has(line, "min_tokens=8192", "INFO carries the cost gate");
    has(line, "per_token=19531", "INFO carries the object shape");
    has(line, "replace_ram=0", "INFO says which tier is primary");
    s.replace_ram = true;
    has(lmcache_info_line(s, st), "replace_ram=1", "INFO follows the mode");
    has(line, "http=http://nas:8080", "INFO carries the report plane");

    st.connected = true;
    st.published = 3;
    st.published_bytes = int64_t(3600) << 20;
    st.restored = 1;
    st.restored_bytes = int64_t(1200) << 20;
    st.probes = 3;
    st.hits = 1;
    st.rejected = 4;
    st.failed = 2;
    st.last_ms = 4120;
    const std::string stats = lmcache_info_line(s, st);
    has(stats, "connected=1", "INFO says whether the store answered");
    has(stats, "published=3", "INFO carries the publish count");
    has(stats, "published_mib=3600", "INFO carries the bytes, in MiB");
    has(stats, "restored=1", "INFO carries the restore count");
    has(stats, "hit=1", "INFO carries the hits");
    has(stats, "miss=2", "the misses are derived from the probes, not counted twice");
    has(stats, "last_ms=4120", "INFO carries the last transfer's cost");
    has(stats, "failed=2", "INFO carries the failures: a broken store must be visible");
}

// ---- a store that behaves like the real one, in memory ----------------------------------------------------------

// LMCache keeps the FIRST object it was given for a key: a second put of the same key is accepted and ignored.  That
// is not a detail - it is why a record's key has to be right before anything is written, and why this fixture keeps
// the first too (an overwriting store would hide exactly the bug the picture key fixes).  The key itself is the salt,
// the start and a hash of the prefix up to the record's end - the same three things lmcache hashes - so two
// conversations never share an object here either.
class FakeStore : public LmcacheStore {
public:
    explicit FakeStore(int64_t per_token) : per_token_(per_token) {}

    static uint64_t prefix_digest(const std::vector<int32_t>& tokens, int64_t end) {
        const size_t up_to = size_t(std::min<int64_t>(end, int64_t(tokens.size())));
        return session_hash64(tokens.data(), up_to * sizeof(int32_t), 0x9e3779b97f4a7c15ull);
    }

    bool open(std::string& why) override {
        if (fail_open) { why = "the store is unreachable"; return false; }
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool opened() const override { return open_; }
    size_t object_bytes() const override { return size_t(256 * per_token_); }
    int64_t chunk_tokens() const override { return 256; }

    bool put_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, const void* data, size_t bytes,
                    std::string& why, const char* salt) override {
        ++puts;
        if (fail_put) { why = "the store refused the object"; return false; }
        if (bytes > object_bytes()) { why = "a record is larger than its object"; return false; }
        const std::string kind(salt ? salt : "");
        // The store resolves a key's token range into one object per chunk and demands one tensor for each: a record
        // that spans more than one chunk is refused (the deployment's own store raises on the count mismatch).
        if (end - start != chunk_tokens()) { why = "one record must be one object"; return false; }
        if (end > int64_t(tokens.size())) { why = "object past the prefix"; return false; }
        ++offered;
        objects.emplace(ObjectKey{kind, start, prefix_digest(tokens, end)},
                        std::string(static_cast<const char*>(data), bytes));   // first wins
        if (end > max_end) max_end = end;
        return true;
    }

    bool get_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, std::string& out, bool& present,
                    std::string& why, const char* salt) override {
        ++gets;
        if (fail_get) { why = "the store is unreachable"; return false; }
        const std::string kind(salt ? salt : "");
        const auto it = objects.find(ObjectKey{kind, start, prefix_digest(tokens, end)});
        if (it == objects.end()) { present = false; return true; }
        if (end > int64_t(tokens.size())) { why = "the prefix is shorter than the stored session"; return false; }
        present = true;
        out = it->second;
        return true;
    }

    // test knobs
    bool fail_open = false, fail_put = false, fail_get = false;
    int puts = 0, gets = 0, offered = 0;
    int64_t max_end = 0;
    // (salt, start, the prefix the object belongs to)
    using ObjectKey = std::tuple<std::string, int64_t, uint64_t>;
    std::map<ObjectKey, std::string> objects;

private:
    int64_t per_token_;
    bool open_ = false;
};

// A second engine's view of one store: several LmcacheTier objects, each owning its own connection, all reaching the
// same objects - which is what four containers on one host do.
class ProxyStore : public LmcacheStore {
public:
    explicit ProxyStore(std::shared_ptr<FakeStore> backend) : backend_(std::move(backend)) {}
    bool open(std::string& why) override { return backend_->open(why); }
    void close() override { backend_->close(); }
    bool opened() const override { return backend_->opened(); }
    size_t object_bytes() const override { return backend_->object_bytes(); }
    int64_t chunk_tokens() const override { return backend_->chunk_tokens(); }
    bool put_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, const void* data, size_t bytes,
                    std::string& why, const char* salt) override {
        return backend_->put_object(tokens, start, end, data, bytes, why, salt);
    }
    bool get_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, std::string& out, bool& present,
                    std::string& why, const char* salt) override {
        return backend_->get_object(tokens, start, end, out, present, why, salt);
    }

private:
    std::shared_ptr<FakeStore> backend_;
};

// ---- fixtures --------------------------------------------------------------------------------------------------

std::vector<uint8_t> bytes_of(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 7u + seed);
    return v;
}

ConversationCheckpoint checkpoint(size_t tokens, uint8_t seed) {
    ConversationCheckpoint c;
    for (size_t i = 0; i < tokens; ++i) c.ids.push_back(int32_t(1000 + i + int(seed) * 100000));
    c.imgs = {{3, 0x1234567890abcdefull ^ seed}};
    c.gdn = bytes_of(4099, seed);
    c.ple = bytes_of(77, seed + 1);
    c.tails = bytes_of(301, seed + 2);
    c.dead = bytes_of(64, seed + 3);
    c.block_pos = bytes_of(8, seed + 4);
    c.used = 42 + seed;
    return c;
}

// A session whose K/V comes from sources, which is the form the engine publishes in (the in-place path).
struct Session {
    SavedConversation image;
    std::vector<std::array<std::vector<uint8_t>, 5>> parts;
    std::vector<SessionKvSource> kv;
    std::vector<int32_t> tokens;
};

// The fixture's K/V sources read the session's own buffers through a reference to it, so a Session that was moved
// (into a vector, out of a helper) leaves them pointing at the moved-from object.  A test that keeps several sessions
// calls this after they are in their final place.
void rebind(Session& s) {
    s.kv.clear();
    const int64_t cells = int64_t(s.parts.empty() ? 0 : s.parts[0][0].size() / 32);
    for (size_t layer = 0; layer < s.parts.size(); ++layer) {
        SessionKvSource src;
        src.format = 2 + int(layer);
        src.cells = cells;
        src.heads = 2;
        src.head_dim = 256;
        src.page_size = 4;
        src.pooled_rows = cells / 4 + 1;
        src.idx_dim = 128;
        for (size_t p = 0; p < 5; ++p) src.sizes[p] = s.parts[layer][p].size();
        src.read = [&s, layer](size_t part, size_t off, void* dst, size_t n) {
            std::memcpy(dst, s.parts[layer][part].data() + off, n);
            return true;
        };
        s.kv.push_back(std::move(src));
    }
}

//: A state whose K/V can be cut at 256 tokens: 4-cell pages, one indexer row per 4 cells, and each buffer a whole
//: number of bytes per token (32 + 32 + 4 + 4, plus the pooled row's 128) - 200 bytes a token, which is what
//: test_settings() registers and what FakeStore shapes its objects to.
constexpr int64_t kPerToken = 400;   // both layers: 2 x (32 + 32 + 4 + 4) plus the two pooled row streams (2 x 128)
constexpr int64_t kCells = 1024;

// A conversation of `ids`, with buffers derived from `seed`: two sessions of the same length and different seeds are
// two users' conversations (different tokens, different K/V), which is what the interleaving tests need.
Session make_session_from(std::vector<int32_t> ids, size_t seed = 0) {
    Session s;
    const size_t tokens = ids.size();
    s.image.geometry = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    s.image.layer_lo = 0;
    s.image.layer_hi = 48;
    s.image.cvec = false;
    s.image.live = checkpoint(tokens, uint8_t(seed + 1));   // the engine passes img.live.ids as the conversation's tokens
    s.image.checkpoints.push_back(checkpoint(8, uint8_t(seed + 2)));
    // A text-only conversation: the picture tests set the image keys they mean (a picture is part of a record's key, so
    // a fixture that invented one would only ever be read back by a caller that passed it too).
    s.image.live.imgs.clear();
    s.image.checkpoints.back().imgs.clear();
    s.image.live.ids = ids;   // (checkpoint() numbers them; the ids are the conversation's own)
    s.tokens = std::move(ids);

    const int64_t cells = ((int64_t(tokens) + 255) / 256) * 256;   // whole chunks
    s.parts.resize(2);
    for (size_t layer = 0; layer < s.parts.size(); ++layer) {
        const std::array<size_t, 5> sizes = {size_t(cells) * 32, size_t(cells) * 32, size_t(cells) * 4,
                                             size_t(cells) * 4, size_t(cells / 4 + 1) * 128 * sizeof(float)};
        for (size_t p = 0; p < 5; ++p) s.parts[layer][p] = bytes_of(sizes[p], uint8_t(layer * 5 + p + seed * 13));
    }
    rebind(s);
    return s;
}

Session make_session(size_t tokens, size_t seed = 0) {
    std::vector<int32_t> ids;
    for (size_t i = 0; i < tokens; ++i) ids.push_back(int32_t((i * 2654435761u + seed * 40503u) & 0x7fffffffu));
    return make_session_from(std::move(ids), seed);
}

bool same_checkpoint(const ConversationCheckpoint& a, const ConversationCheckpoint& b) {
    return a.ids == b.ids && a.imgs == b.imgs && a.gdn == b.gdn && a.ple == b.ple && a.tails == b.tails &&
           a.dead == b.dead && a.block_pos == b.block_pos && a.used == b.used;
}

bool same_state(const SavedConversation& a, const SavedConversation& b) {
    if (a.geometry != b.geometry || a.layer_lo != b.layer_lo || a.layer_hi != b.layer_hi || a.cvec != b.cvec ||
        !same_checkpoint(a.live, b.live) || a.checkpoints.size() != b.checkpoints.size())
        return false;
    for (size_t i = 0; i < a.checkpoints.size(); ++i)
        if (!same_checkpoint(a.checkpoints[i], b.checkpoints[i])) return false;
    return true;
}

bool buffer_equals(const ConversationBuffer& b, const std::vector<uint8_t>& v) {
    if (b.size() != v.size()) return false;
    bool ok = true;
    b.visit(0, b.size(), [&](const uint8_t* p, size_t c, size_t at) {
        if (std::memcmp(p, v.data() + at, c) != 0) ok = false;
        return ok;
    });
    return ok;
}

// The first `n` bytes of a buffer against the first `n` of a vector: a restored prefix of a longer conversation.
bool buffer_prefix_equals(const ConversationBuffer& b, const std::vector<uint8_t>& v, size_t n) {
    if (b.size() < n || v.size() < n) return false;
    bool ok = true;
    b.visit(0, n, [&](const uint8_t* p, size_t c, size_t at) {
        if (std::memcmp(p, v.data() + at, c) != 0) ok = false;
        return ok;
    });
    return ok;
}

LmcacheSettings test_settings() {
    LmcacheSettings s;
    s.enabled = true;
    s.rpc = "fake://store";
    s.model = "test-model";
    s.bytes_per_token = kPerToken;   // a 67 KB object and a 256-token chunk: small enough to test fast, same shape as production
    s.min_tokens = 512;
    return s;
}

// ---- the round trip --------------------------------------------------------------------------------------------

bool publish_ok(LmcacheTier& tier, const Session& s, const SessionFileIdentity& id, const char* what,
                std::string& why) {
    const bool ok = tier.publish(s.image, s.kv, s.tokens, id, why);
    if (!ok) std::fprintf(stderr, "  publish refused (%s): %s\n", what, why.c_str());
    return ok;
}

// The base promise: a conversation published to the store comes back as the same state, K/V included.
void publish_restore_roundtrip() {
    Session s = make_session(1024);
    LmcacheTier tier(test_settings(), std::make_unique<FakeStore>(kPerToken + 64));
    const SessionFileIdentity id{0x1111222233334444ull, 0x5555666677778888ull};

    std::string why;
    check(publish_ok(tier, s, id, "round trip", why), "publish succeeds");
    check(tier.stats().published == 1, "one publish counted");
    check(tier.stats().published_bytes > 0, "the published bytes are counted");

    SavedConversation back;
    check(tier.restore(s.tokens, id, back, {}, why), "restore succeeds");
    check(same_state(s.image, back), "the running state came back identical");
    check(back.kv.size() == s.kv.size(), "the K/V layers came back");
    bool kv_ok = back.kv.size() == s.kv.size();
    for (size_t layer = 0; kv_ok && layer < s.kv.size(); ++layer)
        kv_ok = back.kv[layer].format == s.kv[layer].format && back.kv[layer].cells == s.kv[layer].cells &&
                buffer_equals(back.kv[layer].k, s.parts[layer][0]) && buffer_equals(back.kv[layer].v, s.parts[layer][1]) &&
                buffer_equals(back.kv[layer].k_scale, s.parts[layer][2]) &&
                buffer_equals(back.kv[layer].v_scale, s.parts[layer][3]) &&
                buffer_equals(back.kv[layer].pooled, s.parts[layer][4]);
    check(kv_ok, "every K/V part came back byte for byte");
    check(tier.stats().hits == 1 && tier.stats().restored == 1, "the hit is counted");
}

// A conversation only grows, so the windows an earlier publish wrote are still in the store: sending them again would
// move the whole state on every turn.  This is the whole point of cutting it up.
void only_new_windows_are_sent() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{0x0f0f0f0f0f0f0f0full, 0x0101010101010101ull};
    std::string why;

    Session first = make_session(1024);
    check(publish_ok(tier, first, id, "first", why), "the first publish sends the whole state");
    check(raw->puts == 1 + 4, "one head record and four windows of 256 tokens");

    Session grown = make_session(1280);   // the same conversation, two turns later
    const int before = raw->puts;
    check(publish_ok(tier, grown, id, "grown", why), "the grown conversation publishes");
    check(raw->puts - before == 1 + 1, "the head (rewritten every turn) and the new window, not the four already there");

    SavedConversation back;
    check(tier.restore(grown.tokens, id, back, {}, why), "the grown conversation restores");
    check(int64_t(back.live.ids.size()) == 1280, "the state comes back with all of its tokens");
    check(back.kv.size() == grown.kv.size() && buffer_equals(back.kv[0].k, grown.parts[0][0]),
          "the K/V of the grown conversation came back");

    // and publishing the same conversation again (nothing new) sends the record alone
    const int steady = raw->puts;
    check(tier.publish(grown.image, grown.kv, grown.tokens, id, why), "an unchanged conversation publishes");
    check(raw->puts - steady == 1, "with nothing new, only the record is rewritten");
}

// A prefix too short to pay for its own transfer is not published: below the gate nothing is written at all.
void cost_gate_refuses_short_prefixes() {
    Session s = make_session(300);   // below the 512-token gate
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{1, 2};
    std::string why;
    check(!tier.publish(s.image, s.kv, s.tokens, id, why), "a short prefix is not published");
    check(raw->objects.empty(), "nothing reached the store");
    check(tier.stats().rejected == 1, "the refusal is counted as a cost-gate rejection");
    check(raw->puts == 0, "not a single object was transferred");
}

// The restore side takes the same gate the publish side has.  Without it every request - a two-line question
// included - probes the store, and a store that accepts a lookup without answering holds the prefill for the whole
// lookup timeout before anything is computed (30 s, measured on the deployment this test was written for).
void restore_gate_refuses_short_prefixes() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{13, 14};
    SavedConversation back;
    std::string why;

    const Session short_s = make_session(53);   // below the 512-token gate
    check(!tier.restore(short_s.tokens, id, back, {}, why), "a short prefix is not restored");
    check(raw->gets == 0, "the store is not asked below the cost gate");
    check(tier.stats().probes == 1, "the skipped probe is still counted");
    check(why.find("cost gate") != std::string::npos, "the reason names the cost gate");

    // Above the gate the store is asked, and this one holds nothing: a miss, not an error.
    const Session long_s = make_session(600);
    check(!tier.restore(long_s.tokens, id, back, {}, why), "a prefix above the gate is looked up and misses");
    check(raw->gets > 0, "the store is asked above the cost gate");
    check(tier.stats().probes == 2, "both probes are counted");
}

// The gate is a floor, not a fudge: one token below it the store is never asked, one token at it is.
void restore_gate_boundary() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));   // min_tokens = 512
    const SessionFileIdentity id{15, 16};
    SavedConversation back;
    std::string why;

    const Session below = make_session(511);
    check(!tier.restore(below.tokens, id, back, {}, why), "511 tokens is below the gate");
    check(raw->gets == 0, "511 tokens never reaches the store");

    const Session at = make_session(512);
    check(!tier.restore(at.tokens, id, back, {}, why), "512 tokens is at the gate (this store has nothing: a miss)");
    check(raw->gets > 0, "512 tokens does reach the store");
}

// The stored prefix floors to a whole chunk, and what comes back is exactly that prefix - the tail that never filled
// a window is recomputed, not invented.
void prefix_is_cut_at_chunk_boundaries() {
    Session s = make_session(1000);   // 3 whole chunks of 256, 232 tokens left over
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{3, 4};
    std::string why;
    check(publish_ok(tier, s, id, "1000 tokens", why), "a 1000-token session publishes");
    check(raw->max_end == 768, "the stored prefix floors to 768 tokens, never past the last whole chunk");
    check(raw->objects.size() == 1 + 3, "one head record and three windows of 256 tokens");

    SavedConversation back;
    check(tier.restore(s.tokens, id, back, {}, why), "and it restores");
    // A window is a whole chunk, so the state comes back as the 768-token prefix it stored - not the 232 tokens that
    // never filled one.  The engine reads those again, which is what the chunk floor has always meant.
    check(back.live.ids.size() == 768, "the restored state covers the whole chunks that were stored");
    check(std::equal(back.live.ids.begin(), back.live.ids.end(), s.image.live.ids.begin()),
          "its tokens are the conversation's own prefix");
    std::vector<uint8_t> k_head(768 * 32);
    check(back.kv[0].k.read(k_head.data(), 0, k_head.size()), "the restored K is readable");
    check(std::memcmp(k_head.data(), s.parts[0][0].data(), k_head.size()) == 0,
          "the restored K is the prefix the store was given");
}

// A refusing or unreachable store only costs the reuse: publish reports it, restore leaves the caller's image as it
// was, and the request goes on.
void a_store_failure_is_never_fatal() {
    Session s = make_session(1024);
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{5, 6};
    std::string why;

    raw->fail_put = true;
    check(!tier.publish(s.image, s.kv, s.tokens, id, why), "a refusing store makes publish fail, quietly");
    check(tier.stats().failed >= 1, "the failure is counted");
    check(!why.empty(), "and explained");

    raw->fail_put = false;
    raw->fail_open = true;
    check(!tier.publish(s.image, s.kv, s.tokens, id, why), "an unreachable store makes publish fail too");

    raw->fail_open = false;
    SavedConversation keep;
    keep.layer_hi = 7;
    const SavedConversation before = keep;
    raw->fail_get = true;
    check(!tier.restore(s.tokens, id, keep, {}, why), "an unreachable store makes restore fail");
    check(same_state(before, keep), "a failed restore leaves the caller's image untouched");
}

// Nothing stored is a miss (a probe, no hit, no failure) - the shape the engine's counters depend on.
void a_miss_is_not_an_error() {
    Session s = make_session(1024);
    LmcacheTier tier(test_settings(), std::make_unique<FakeStore>(kPerToken + 64));
    const SessionFileIdentity id{7, 8};
    SavedConversation out;
    std::string why;
    check(!tier.restore(s.tokens, id, out, {}, why), "nothing stored: restore reports a miss");
    check(tier.stats().probes == 1 && tier.stats().hits == 0, "the probe was counted, the hit was not");
    check(tier.stats().failed == 0, "a miss is not a failure");
}

// A session saved under another model or engine configuration is refused before any of its payload is parsed.
void another_identity_is_refused() {
    Session s = make_session(1024);
    LmcacheTier tier(test_settings(), std::make_unique<FakeStore>(kPerToken + 64));
    const SessionFileIdentity id{9, 10};
    std::string why;
    check(tier.publish(s.image, s.kv, s.tokens, id, why), "publish under one identity");

    SavedConversation out;
    const SessionFileIdentity other{9, 11};   // same model, another engine configuration
    check(!tier.restore(s.tokens, other, out, {}, why), "another configuration is refused");
    check(why.find("configuration") != std::string::npos, "and says why");
    check(out.geometry == std::array<int64_t, 18>{}, "the caller's image stays empty");
}

// No STRATA_LMCACHE: publish and restore are no-ops and INFO says the tier is off - a model that never configured it
// behaves exactly as before this feature.
void disabled_tier_does_nothing() {
    LmcacheSettings off;   // enabled = false
    LmcacheTier tier(off, std::make_unique<FakeStore>(kPerToken + 64));
    Session s = make_session(4096);
    const SessionFileIdentity id{11, 12};
    std::string why;
    check(!tier.enabled(), "no settings, no tier");
    check(!tier.publish(s.image, s.kv, s.tokens, id, why), "publish is a no-op");
    SavedConversation out;
    check(!tier.restore(s.tokens, id, out, {}, why), "restore is a no-op");
    same(lmcache_info_line(off, tier.stats()), "lmcache=off\n", "and INFO says so");
}
// ---- several users, several conversations, several engines --------------------------------------------------------

bool same_bytes(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) { return a == b; }

// A real instance serves several clients at once: an agent's main conversation, a subagent's, a side question, another
// person's chat.  Their publishes interleave turn by turn, and every turn must read back its OWN state - a shared tier
// that mixed two conversations up would be worse than no tier at all.
void users_interleave_on_one_engine() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{21, 22};
    std::string why;

    const int users = 4;
    std::vector<Session> live;
    live.reserve(users);
    for (int u = 0; u < users; ++u) live.push_back(make_session(1024, size_t(u + 1)));
    for (auto& s : live) rebind(s);

    for (int turn = 0; turn <= 3; ++turn) {
        for (int u = 0; u < users; ++u) {
            const size_t len = 1024 + size_t(turn) * 256;
            Session grown = make_session(len, size_t(u + 1));   // the same conversation, one turn longer
            live[size_t(u)] = std::move(grown);
            rebind(live[size_t(u)]);
            const Session& now = live[size_t(u)];
            if (!publish_ok(tier, now, id, "an interleaved turn", why)) {
                check(false, "every user's turn publishes");
                continue;
            }
            SavedConversation back;
            const bool ok = tier.restore(now.tokens, id, back, {}, why);
            check(ok, "every user's turn is read back");
            if (!ok) continue;
            check(int64_t(back.live.ids.size()) == int64_t(len), "the turn comes back at its own length");
            check(back.live.ids == now.tokens, "with that user's tokens");
            check(back.live.gdn == now.image.live.gdn, "and that user's running state");
            check(buffer_equals(back.kv[0].k, now.parts[0][0]), "and that user's K/V");
        }
    }
    // four conversations, four turns each: every one of them reached the store, and each window was written once - the
    // windows of the turns before are still there (4 -> 7 windows per user, not 4+5+6+7)
    check(tier.stats().published == users * 4, "every turn of every user was published");
    // every window once, and one head per turn (the head is keyed by the length it covers, so a turn adds one and the
    // turn before it stays as a shorter, stale head that the TTL takes away)
    check(raw->objects.size() == size_t(users) * (7 + 4), "each new window once, and one head per turn");
}

// The deployment runs four engines against one store, and a turn does not come back to the engine that computed it:
// that is the whole point.  Every publish lands on one instance and every read is made on the other.
void two_instances_serve_their_own_users() {
    auto backend = std::make_shared<FakeStore>(kPerToken + 64);
    LmcacheTier a(test_settings(), std::make_unique<ProxyStore>(backend));
    LmcacheTier b(test_settings(), std::make_unique<ProxyStore>(backend));
    const SessionFileIdentity id{31, 32};
    std::string why;

    std::vector<Session> live;
    live.reserve(4);
    for (int u = 0; u < 4; ++u) live.push_back(make_session(1024, size_t(u + 11)));
    for (auto& s : live) rebind(s);

    for (int turn = 0; turn < 3; ++turn) {
        for (int u = 0; u < 4; ++u) {
            const size_t len = 1024 + size_t(turn) * 256;
            Session grown = make_session(len, size_t(u + 11));
            live[size_t(u)] = std::move(grown);
            rebind(live[size_t(u)]);
            const Session& now = live[size_t(u)];
            LmcacheTier& owner = (u % 2 == 0) ? a : b;
            LmcacheTier& other = (u % 2 == 0) ? b : a;
            if (!owner.publish(now.image, now.kv, now.tokens, id, why)) {
                check(false, "a turn publishes on the instance that computed it");
                continue;
            }
            SavedConversation back;
            const bool ok = other.restore(now.tokens, id, back, {}, why);
            check(ok, "the other instance reads the turn back");
            if (!ok) continue;
            check(same_state(now.image, back), "and gets that user's state, not another's");
            check(buffer_equals(back.kv[0].k, now.parts[0][0]), "with that user's K/V");
        }
    }
}

// A picture is a run of placeholder tokens: two users who send the same words with different screenshots produce the
// SAME token ids and DIFFERENT K/V.  One key cannot hold both (the store keeps the first object it was given), so the
// second conversation's state is simply not stored - and every reader must then refuse it.  That refusal is the
// caller's check (conversation_prefix: same pictures, same steering), which this exercises end to end.
void a_picture_is_never_served_for_another_picture() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{41, 42};
    std::string why;

    Session mine = make_session(1024, 1);
    Session yours = make_session(1024, 1);
    rebind(mine);
    rebind(yours);
    mine.image.live.imgs = {{3, 0xaaaa0001ull}};
    yours.image.live.imgs = {{3, 0xbbbb0002ull}};
    for (size_t p = 0; p < 5; ++p) yours.parts[0][p] = bytes_of(yours.parts[0][p].size(), uint8_t(0x60 + p));
    rebind(yours);
    check(mine.tokens == yours.tokens, "the two prompts have the same token ids");
    check(!same_bytes(mine.parts[0][0], yours.parts[0][0]), "and the pictures make their K/V differ");

    check(publish_ok(tier, mine, id, "the first picture", why), "the first picture's conversation publishes");
    check(publish_ok(tier, yours, id, "the second picture", why), "the second publishes over it (the store keeps the first)");
    check(raw->objects.size() == 1 + 4, "one head and four windows: the store holds one state for these ids, not two");

    SavedConversation back;
    check(tier.restore(yours.tokens, id, back, {}, why), "the ids are in the store, so the read succeeds");
    check(back.live.imgs == mine.image.live.imgs, "and it says which picture it was computed for");
    // The next turn of each conversation: the restored prefix is shorter than its prompt, as it always is in practice.
    std::vector<int32_t> next = yours.tokens;
    next.push_back(777);
    check(conversation_prefix(back.live, next, mine.image.live.imgs) == 1024,
          "the first picture's caller accepts it");
    check(conversation_prefix(back.live, next, yours.image.live.imgs) == 0,
          "the second picture's caller refuses it - recomputes instead of using another picture's state");
}

// The stored prefix cuts at a whole chunk, so a picture before that cut is part of the state and one after it is not:
// the caller's own check (conversation_prefix, which this exercises) must follow the same line.
void a_picture_inside_the_prefix_gates_the_match() {
    Session s = make_session(1200, 3);   // 4 windows stored: the state covers 1024 tokens
    s.image.live.imgs = {{3, 0x1111ull}, {900, 0x2222ull}, {1100, 0x3333ull}};
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{43, 44};
    std::string why;
    check(publish_ok(tier, s, id, "a session with pictures", why), "a session with pictures publishes");

    SavedConversation back;
    check(tier.restore(s.tokens, id, back, {}, why), "and restores");
    check(back.live.ids.size() == 1024, "as the 1024-token prefix it stored");
    check(back.live.imgs == std::vector<ConversationImageKey>{{3, 0x1111ull}, {900, 0x2222ull}},
          "carrying the pictures inside that prefix (and not the one past it)");
    check(conversation_prefix(back.live, s.tokens, {{3, 0x1111ull}, {900, 0x2222ull}}) == 1024,
          "the state matches a prompt with the same pictures");

    // Another picture at 900 - inside the restored state - means the K/V there is not the caller's: no match.
    std::vector<ConversationImageKey> other_inside = {{3, 0x1111ull}, {900, 0x9999ull}};
    check(conversation_prefix(back.live, s.tokens, other_inside) == 0,
          "a different picture inside the prefix refuses the match");

    // A different picture at 1100 - past the restored prefix - changes nothing: that state never covered it.
    std::vector<ConversationImageKey> other_after = {{3, 0x1111ull}, {900, 0x2222ull}, {1100, 0x7777ull}};
    check(conversation_prefix(back.live, s.tokens, other_after) == 1024,
          "a picture past the stored prefix does not");
}

// An agent pastes the same long file twice with two different questions.  The file's windows are the same objects, so
// the second question must restore them instead of reading the file again - and never send them a second time.
void a_long_file_prefix_is_shared_by_two_questions() {
    std::vector<int32_t> file;
    file.reserve(4136);
    for (int i = 0; i < 4096; ++i) file.push_back(int32_t((i * 2654435761u) & 0x7fffffff));
    std::vector<int32_t> q1 = file, q2 = file;
    for (int i = 0; i < 40; ++i) {
        q1.push_back(int32_t(900000 + i));
        q2.push_back(int32_t(800000 + i));
    }
    Session a = make_session_from(q1, 5);
    Session b = make_session_from(q2, 5);
    rebind(a);
    rebind(b);

    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{45, 46};
    std::string why;
    check(publish_ok(tier, a, id, "the first question", why), "the first question publishes the file's windows");
    check(raw->objects.size() == 1 + 16, "one head and sixteen windows of the file");

    const int before = raw->puts;
    check(publish_ok(tier, b, id, "the second question", why), "the second question publishes");
    check(raw->puts - before == 1, "only the head it shares with the file: its windows are already there");

    SavedConversation back;
    check(tier.restore(b.tokens, id, back, {}, why), "the second question restores");
    check(back.live.ids == file, "as the file's own prefix, not the first question's tail");
    check(conversation_prefix(back.live, b.tokens, {}) == 4096, "which is a prefix of the second prompt");
    check(buffer_prefix_equals(back.kv[0].k, a.parts[0][0], 4096 * 32),
          "with the K/V of the file the first turn computed");
}



// The engine's writer publishes an image that ALREADY carries its K/V, so the tier cuts it with chunk_extract (a copy
// out of the image's buffers).  Every other test here publishes the source form, which the engine only uses when the
// K/V is pulled from its layers as it goes - two different cuts, and the one the deployment takes was untested.
SavedConversation image_with_kv(const Session& s) {
    SavedConversation image = s.image;
    image.kv.resize(s.parts.size());
    for (size_t layer = 0; layer < s.parts.size(); ++layer) {
        ConversationKv& k = image.kv[layer];
        const int64_t cells = int64_t(s.parts[layer][0].size() / 32);
        k.format = 2 + int(layer);
        k.cells = cells;
        k.heads = 2;
        k.head_dim = 256;
        k.page_size = 4;
        k.pooled_rows = cells / 4 + 1;
        k.idx_dim = 128;
        std::array<ConversationBuffer*, 5> dst = {&k.k, &k.v, &k.k_scale, &k.v_scale, &k.pooled};
        for (size_t p = 0; p < 5; ++p) {
            const std::vector<uint8_t>& src = s.parts[layer][p];
            dst[p]->resize(src.size());
            dst[p]->visit(0, src.size(), [&](uint8_t* q, size_t n, size_t at) {
                std::memcpy(q, src.data() + at, n);
                return true;
            });
        }
    }
    return image;
}

// The form the writer really publishes: the K/V already inside the image, cut by chunk_extract.
void the_image_form_the_engine_publishes() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{71, 72};
    std::string why;

    Session first = make_session(1024);
    check(tier.publish(image_with_kv(first), id, first.tokens, why), "an image that carries its K/V publishes");
    check(raw->puts == 1 + 4, "one head and four windows");
    SavedConversation back;
    check(tier.restore(first.tokens, id, back, {}, why), "and restores");
    check(back.kv.size() == first.parts.size() && buffer_equals(back.kv[0].k, first.parts[0][0]),
          "the K/V that came out of the image is byte for byte what went in");

    Session grown = make_session(1280);
    const int before = raw->puts;
    check(tier.publish(image_with_kv(grown), id, grown.tokens, why), "the grown image publishes");
    check(raw->puts - before == 1 + 1, "only the head and the new window, as with the source form");
    check(tier.restore(grown.tokens, id, back, {}, why), "and the grown state restores");
    check(buffer_equals(back.kv[1].pooled, grown.parts[1][4]), "including the second layer's pooled rows");
}

// One window lost (the store evicted it, or the publish was cut short) makes the chain unusable: the state's running
// arrays belong to the whole conversation, so a shorter prefix assembled from the rest would be wrong.
void a_missing_window_refuses_the_whole_chain() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{73, 74};
    std::string why;
    Session s = make_session(1024);
    check(publish_ok(tier, s, id, "the store's copy", why), "the state publishes");
    for (auto it = raw->objects.begin(); it != raw->objects.end(); ++it)
        if (std::get<1>(it->first) == 512) { raw->objects.erase(it); break; }   // window 2 is gone

    SavedConversation keep;
    keep.layer_hi = 3;
    const SavedConversation untouched = keep;
    check(!tier.restore(s.tokens, id, keep, {}, why), "a chain missing a window is refused");
    check(why.find("missing window 2") != std::string::npos, "and names the window");
    check(same_state(untouched, keep), "the caller's image is untouched");
    check(tier.stats().restored == 0, "nothing is counted as restored");
}

// What the store hands back is never trusted: every record carries its own hash and the session format's checks.  A
// torn write, a reused key from another tool, a truncated object - all of them end as "not restored", never as a state
// the engine would continue from.
void a_corrupt_object_in_the_store_is_refused() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{83, 84};
    std::string why;
    Session s = make_session(1024);
    check(publish_ok(tier, s, id, "the store's copy", why), "the state publishes");

    for (auto& kv : raw->objects) {
        if (std::get<1>(kv.first) == 512) { kv.second.resize(100); kv.second[40] ^= 0xff; break; }   // window 2 is torn
    }
    SavedConversation keep;
    keep.layer_hi = 2;
    const SavedConversation untouched = keep;
    check(!tier.restore(s.tokens, id, keep, {}, why), "a torn record is refused");
    check(!why.empty(), "with a reason");
    check(same_state(untouched, keep), "and the caller's image is untouched");
    check(tier.stats().restored == 0 && tier.stats().hits == 0, "nothing is counted as restored or hit");

    // The head itself corrupted: the same.
    for (auto& kv : raw->objects) {
        if (std::get<1>(kv.first) == 0) { kv.second.assign(kv.second.size(), 0); break; }   // the head, zeroed
    }
    check(!tier.restore(s.tokens, id, keep, {}, why), "a zeroed head is refused too");
    check(same_state(untouched, keep), "and the caller's image stays as it was");
}

// The store was down and comes back: the next call reconnects instead of staying broken for the process's life.
void a_store_that_comes_back_is_used_again() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{75, 76};
    std::string why;
    Session s = make_session(1024);

    raw->fail_open = true;
    check(!tier.publish(s.image, s.kv, s.tokens, id, why), "a store that is down refuses the publish");
    check(tier.stats().connected == false, "and says it is not connected");
    raw->fail_open = false;
    check(tier.publish(s.image, s.kv, s.tokens, id, why), "the same tier publishes once the store answers again");
    check(tier.stats().connected, "and is connected again");
    SavedConversation back;
    check(tier.restore(s.tokens, id, back, {}, why), "and reads back");
}

// A client that rewinds history (an edited turn, a compacted conversation) asks for a SHORTER prefix than the last
// publish: the windows the store holds beyond it are not this conversation's any more, so they are written again.
void a_rewound_conversation_is_published_again() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{77, 78};
    std::string why;
    Session long_s = make_session(1280);
    check(publish_ok(tier, long_s, id, "the long turn", why), "a 1280-token conversation publishes");
    Session short_s = make_session(1024);   // its own prefix, one window shorter
    const int before = raw->puts;
    check(publish_ok(tier, short_s, id, "the rewind", why), "the rewound one publishes");
    check(raw->puts - before == 1 + 4, "all four windows again, not the ones past the new end");
    SavedConversation back;
    check(tier.restore(short_s.tokens, id, back, {}, why), "and restores at the shorter length");
    check(back.live.ids == short_s.tokens, "with the rewound tokens");
}

// A layer split (several cards, one image per stage) cannot be cut into windows: the shared tier stays out of it, and
// says so instead of writing something a reader could not use.
void a_layer_split_state_is_not_published() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{79, 80};
    std::string why;
    Session s = make_session(1024);
    SavedConversation split = image_with_kv(s);
    split.stage_images.push_back(s.image);   // the later stage's own image

    check(!tier.publish(split, id, s.tokens, why), "a layer-split state is not published");
    check(raw->puts == 0, "nothing reaches the store");
    check(why.find("layer-split") != std::string::npos, "and the reason says why");
}

// The engine refuses a read its RAM preflight cannot afford before anything is parsed; the tier must pass that through
// without touching the caller's image.
void a_read_the_ram_preflight_refuses_leaves_the_caller_alone() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{81, 82};
    std::string why;
    Session s = make_session(1024);
    check(publish_ok(tier, s, id, "the store's copy", why), "the state publishes");

    SessionReadLimits limits;
    limits.admit = [](uint64_t, std::string& w) { w = "no room for it"; return false; };
    SavedConversation keep;
    keep.layer_hi = 5;
    const SavedConversation untouched = keep;
    check(!tier.restore(s.tokens, id, keep, limits, why), "a read the RAM preflight refuses fails");
    check(why.find("no room") != std::string::npos, "with the preflight's reason");
    check(same_state(untouched, keep), "and the caller's image is untouched");
}

// One instance keeps a conversation in its own RAM tier and in the store at the same time.  The rule is "the longer
// prefix wins" (generate.cpp: the store is only used when it beats `resume`, the batch slot and the parked
// conversation), and a turn the RAM tier wins must not pay for the store's state: the head alone says how long it is.
void the_longer_of_the_ram_tier_and_the_store_wins() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{53, 54};
    std::string why;

    Session store_side = make_session(1280);            // what another engine published
    check(publish_ok(tier, store_side, id, "the store's copy", why), "the store holds a 1280-token state");
    std::vector<int32_t> prompt = store_side.tokens;    // the next turn starts with it and goes on
    prompt.push_back(4242);

    ConversationCache ram(8u << 20, 2);                 // the RAM tier, as --serve builds it
    Session ram_side = make_session(768);
    check(ram.put(SavedConversation(ram_side.image)), "the RAM tier holds a 768-token state");
    const auto parked = ram.best(prompt, {}, false);
    check(parked.tokens == 768, "the RAM tier offers this prompt 768 tokens");

    SavedConversation back;
    check(tier.restore(prompt, id, parked.tokens, back, {}, why), "the store's longer state is read");
    const int64_t from_store = conversation_prefix(back.live, prompt, {});
    check(from_store == 1280, "and it gives this prompt 1280 tokens");
    check(from_store > parked.tokens, "more than the RAM tier's 768, so the store wins");

    // The other way round: the RAM tier holds the longer conversation.
    Session longer_ram = make_session(1536);
    ConversationCache ram2(8u << 20, 2);
    check(ram2.put(SavedConversation(longer_ram.image)), "the RAM tier holds a 1536-token state");
    std::vector<int32_t> next = longer_ram.tokens;
    next.push_back(99);
    const auto best = ram2.best(next, {}, false);
    check(best.tokens == 1536, "which offers this prompt 1536 tokens");

    const int before = raw->gets;
    check(!tier.restore(next, id, best.tokens, back, {}, why), "a store state shorter than the RAM tier's is not used");
    // one lookup for the prompt's own length (a miss), one for the head that is there - and no window at all
    check(raw->gets - before == 2, "only heads were asked for: the five windows were never transferred");
    check(why.find("not longer than the RAM tier") != std::string::npos, "and the reason names the RAM tier");

    // A store state one window longer than the RAM tier's is read in full.
    const int before_read = raw->gets;
    check(tier.restore(prompt, id, 768, back, {}, why), "one window longer than the RAM tier is read");
    check(raw->gets - before_read == 1 + 5, "its head and its five windows");
}

// The same rule, at the tier's own boundary: a state no longer than the caller's prefix is refused after its head.
void a_state_the_caller_already_beats_is_not_transferred() {
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{51, 52};
    std::string why;
    Session s = make_session(1024);
    check(publish_ok(tier, s, id, "the store's copy", why), "a 1024-token state publishes");

    const int before = raw->gets;
    SavedConversation back;
    check(!tier.restore(s.tokens, id, 1024, back, {}, why), "a state as long as the caller's prefix is refused");
    check(raw->gets - before == 1, "only its head was read");
    check(tier.stats().restored == 0, "nothing was counted as restored");
    check(tier.stats().hits == 0, "nor as a hit");
    check(back.live.ids.empty(), "and the caller's image stays empty");
}

// ---- against a real store, when an endpoint is given ------------------------------------------------------------

int live(const std::string& endpoint) {
    LmcacheSettings s = test_settings();
    s.rpc = endpoint;
    s.model = "strata/tier-selftest";
    s.bytes_per_token = kPerToken;   // the fixture's shape, so one window is one object
    s.min_tokens = 256;
    LmcacheTier tier(s, lmcache_zmq_store(s.rpc, s.model, s.bytes_per_token, 9911));

    Session session = make_session(1024);
    const SessionFileIdentity id{0xabcdef0123456789ull, 0x0fedcba987654321ull};
    std::string why;
    if (!tier.publish(session.image, session.kv, session.tokens, id, why)) {
        std::fprintf(stderr, "live publish: %s\n", why.c_str());
        return 2;
    }
    SavedConversation back;
    if (!tier.restore(session.tokens, id, back, {}, why)) {
        std::fprintf(stderr, "live restore: %s\n", why.c_str());
        return 2;
    }
    check(same_state(session.image, back), "live: the state came back identical");
    bool kv_ok = back.kv.size() == session.kv.size();
    for (size_t layer = 0; kv_ok && layer < session.kv.size(); ++layer)
        kv_ok = buffer_equals(back.kv[layer].k, session.parts[layer][0]) &&
                buffer_equals(back.kv[layer].pooled, session.parts[layer][4]);
    check(kv_ok, "live: the K/V came back byte for byte");

    // The form the engine's writer really publishes: the K/V inside the image, cut by chunk_extract.  The round trip
    // above used the source form (chunk_extract_from), which the engine only uses when the layers are read in place.
    {
        Session in_image = make_session(1280, 41);
        SavedConversation picture = image_with_kv(in_image);
        const SessionFileIdentity iid{0x2222333344445555ull, 0x6666777788889999ull};
        if (!tier.publish(picture, iid, in_image.tokens, why)) {
            std::fprintf(stderr, "live image publish: %s\n", why.c_str());
            return 2;
        }
        SavedConversation got;
        if (!tier.restore(in_image.tokens, iid, got, {}, why)) {
            std::fprintf(stderr, "live image restore: %s\n", why.c_str());
            return 2;
        }
        check(same_state(picture, got), "live: an image that carries its K/V round-trips");
        check(got.kv.size() == in_image.parts.size() && buffer_equals(got.kv[0].k, in_image.parts[0][0]) &&
                  buffer_equals(got.kv[1].pooled, in_image.parts[1][4]),
              "live: with both layers byte for byte");
    }

    // The deployment this was written for runs four engines against one store: what one publishes, another reads.
    // A second instance (another instance_id, same model and geometry) must find the first one's session.
    {
        LmcacheTier second(s, lmcache_zmq_store(s.rpc, s.model, s.bytes_per_token, 9912));
        SavedConversation other;
        if (!second.restore(session.tokens, id, other, {}, why)) {
            std::fprintf(stderr, "live second engine restore: %s\n", why.c_str());
            return 2;
        }
        check(same_state(session.image, other), "live: a second engine reads what the first published");
    }

    // Both at once, as two requests arriving on two instances.  The key's request_id is the same on every engine,
    // so this is the case where a store that keys its prefetch state by it would make the two collide: every read
    // must still hit.
    {
        LmcacheTier a(s, lmcache_zmq_store(s.rpc, s.model, s.bytes_per_token, 9913));
        LmcacheTier b(s, lmcache_zmq_store(s.rpc, s.model, s.bytes_per_token, 9914));
        std::atomic<int> hits{0};
        auto hammer = [&](LmcacheTier& t, int rounds) {
            for (int i = 0; i < rounds; ++i) {
                SavedConversation got;
                std::string w;
                if (t.restore(session.tokens, id, got, {}, w)) ++hits;
            }
        };
        std::thread t1(hammer, std::ref(a), 8), t2(hammer, std::ref(b), 8);
        t1.join();
        t2.join();
        check(hits.load() == 16, "live: 16 concurrent reads on two engines all hit");
    }

    // The production gate is 8192, not the fixture's 256, and a real conversation is far longer than 1024 tokens:
    // 9000 tokens is 36 windows and ~3.6 MB through the store, which is the size class the deployment moves.
    {
        LmcacheSettings g = s;
        g.min_tokens = 8192;
        LmcacheTier gated(g, lmcache_zmq_store(g.rpc, g.model, g.bytes_per_token, 9920));
        const SessionFileIdentity bid{0xaaaabbbbccccddddull, 0x1111222233334444ull};
        Session big = make_session(9216, 7);   // 36 whole chunks (its own opening, so no other fixture shadows it)
        if (!gated.publish(big.image, big.kv, big.tokens, bid, why)) {
            std::fprintf(stderr, "live gated publish: %s\n", why.c_str());
            return 2;
        }
        SavedConversation got;
        const bool restored = gated.restore(big.tokens, bid, got, {}, why);
        if (!restored) std::fprintf(stderr, "live gated restore: %s\n", why.c_str());
        check(restored, "live: a 9216-token session at the 8192 gate restores");
        check(same_state(big.image, got), "live: the 9216-token state came back identical");

        Session small = make_session(8000);   // below the 8192 gate
        SavedConversation none;
        check(!gated.restore(small.tokens, bid, none, {}, why), "live: 8000 tokens is below the 8192 gate");
        check(why.find("cost gate") != std::string::npos, "live: the gate names itself");
    }
    // Two users, the same words, two different screenshots: the same token ids, another K/V, and therefore another
    // salt.  The store must keep them apart - this is the case the picture key exists for.
    {
        LmcacheSettings ls = s;
        ls.min_tokens = 512;
        LmcacheTier apt(ls, lmcache_zmq_store(ls.rpc, ls.model, ls.bytes_per_token, 9940));
        LmcacheTier bpt(ls, lmcache_zmq_store(ls.rpc, ls.model, ls.bytes_per_token, 9941));
        LmcacheTier cpt(ls, lmcache_zmq_store(ls.rpc, ls.model, ls.bytes_per_token, 9942));
        const SessionFileIdentity pid{0x1111222233334444ull, 0x5555666677778888ull};
        Session first = make_session(1024, 31);
        Session second = make_session(1024, 31);
        rebind(first);
        rebind(second);
        first.image.live.imgs = {{3, 0xa1a1a1a1a1a1a1a1ull}};
        second.image.live.imgs = {{3, 0xb2b2b2b2b2b2b2b2ull}};
        for (size_t p = 0; p < 5; ++p) second.parts[0][p] = bytes_of(second.parts[0][p].size(), uint8_t(0x70 + p));
        rebind(second);
        if (!apt.publish(first.image, first.kv, first.tokens, pid, why)) {
            std::fprintf(stderr, "live pictures publish 1: %s\n", why.c_str());
            return 2;
        }
        if (!bpt.publish(second.image, second.kv, second.tokens, pid, why)) {
            std::fprintf(stderr, "live pictures publish 2: %s\n", why.c_str());
            return 2;
        }
        SavedConversation got;
        if (!cpt.restore(first.tokens, pid, got, {}, why)) {   // a third engine reads what the two wrote
            std::fprintf(stderr, "live pictures restore: %s\n", why.c_str());
            return 2;
        }
        // One of the two states is in the store (the store keeps the first object it was given) and the state says
        // which picture it was computed for; the other caller's check refuses it, so no one is ever served the wrong
        // picture's K/V.
        const bool is_first = got.live.imgs == first.image.live.imgs;
        check(is_first || got.live.imgs == second.image.live.imgs, "live: the state names the picture it was made for");
        check(is_first ? buffer_equals(got.kv[0].k, first.parts[0][0])
                       : buffer_equals(got.kv[0].k, second.parts[0][0]),
              "live: and its bytes are that picture's");
        const Session& other_pic = is_first ? second : first;
        check(conversation_prefix(got.live, other_pic.tokens, other_pic.image.live.imgs) == 0,
              "live: the other picture's caller refuses it");
    }

    // The deployment's real shape: more than one engine, more than one user per engine, and pictures among the turns.
    // A turn computed on one engine is read on another, with the request's own pictures in the key.
    {
        LmcacheSettings ls = s;
        ls.min_tokens = 512;
        LmcacheTier e0(ls, lmcache_zmq_store(ls.rpc, ls.model, ls.bytes_per_token, 9930));
        LmcacheTier e1(ls, lmcache_zmq_store(ls.rpc, ls.model, ls.bytes_per_token, 9931));
        const SessionFileIdentity uid{0x0102030405060708ull, 0x090a0b0c0d0e0f10ull};
        std::vector<Session> users;
        users.reserve(3);
        for (int u = 0; u < 3; ++u) users.push_back(make_session(1024, size_t(u + 21)));
        for (int u = 0; u < 3; ++u) {
            rebind(users[size_t(u)]);
            users[size_t(u)].image.live.imgs = {{3, 0x5000000000000000ull + uint64_t(u)}};   // one screenshot each
        }
        int read_ok = 0;
        for (int turn = 0; turn < 2; ++turn) {
            for (int u = 0; u < 3; ++u) {
                const size_t len = 1024 + size_t(turn) * 256;
                Session grown = make_session(len, size_t(u + 21));
                grown.image.live.imgs = {{3, 0x5000000000000000ull + uint64_t(u)}};
                users[size_t(u)] = std::move(grown);
                rebind(users[size_t(u)]);
                const Session& now = users[size_t(u)];
                LmcacheTier& owner = (u % 2 == 0) ? e0 : e1;
                LmcacheTier& other = (u % 2 == 0) ? e1 : e0;
                if (!owner.publish(now.image, now.kv, now.tokens, uid, why)) {
                    std::fprintf(stderr, "live interleaved publish (user %d, turn %d): %s\n", u, turn, why.c_str());
                    return 2;
                }
                SavedConversation got;
                if (!other.restore(now.tokens, uid, got, {}, why)) {
                    std::fprintf(stderr, "live interleaved restore (user %d, turn %d): %s\n", u, turn, why.c_str());
                    return 2;
                }
                if (!got.live.ids.empty() && got.live.ids == now.tokens && buffer_equals(got.kv[0].k, now.parts[0][0]))
                    ++read_ok;
            }
        }
        check(read_ok == 6, "live: six turns of three users on two engines all came back on the other engine");

        // A conversation nobody published is a miss, never another user's state.
        Session stranger = make_session(1024, 99);
        SavedConversation none;
        check(!e0.restore(stranger.tokens, uid, none, {}, why), "live: an unpublished conversation misses");
    }

    const auto& st = tier.stats();
    std::printf("live: published=%lld (%lld MiB) restored=%lld (%lld MiB) hit=%lld failed=%lld last_ms=%lld\n",
                (long long) st.published, (long long) (st.published_bytes >> 20), (long long) st.restored,
                (long long) (st.restored_bytes >> 20), (long long) st.hits, (long long) st.failed, (long long) st.last_ms);
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    settings_surface();
    info_lines();
    publish_restore_roundtrip();
    only_new_windows_are_sent();
    cost_gate_refuses_short_prefixes();
    restore_gate_refuses_short_prefixes();
    restore_gate_boundary();
    prefix_is_cut_at_chunk_boundaries();
    a_store_failure_is_never_fatal();
    a_miss_is_not_an_error();
    another_identity_is_refused();
    disabled_tier_does_nothing();
    users_interleave_on_one_engine();
    two_instances_serve_their_own_users();
    a_picture_is_never_served_for_another_picture();
    a_picture_inside_the_prefix_gates_the_match();
    a_long_file_prefix_is_shared_by_two_questions();
    a_state_the_caller_already_beats_is_not_transferred();
    the_image_form_the_engine_publishes();
    a_missing_window_refuses_the_whole_chain();
    a_corrupt_object_in_the_store_is_refused();
    a_store_that_comes_back_is_used_again();
    a_rewound_conversation_is_published_again();
    a_layer_split_state_is_not_published();
    a_read_the_ram_preflight_refuses_leaves_the_caller_alone();
    the_longer_of_the_ram_tier_and_the_store_wins();
    int rc = 0;
    if (argc > 1) rc = live(argv[1]);
    std::printf("lmcache_tier_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : rc;
}
