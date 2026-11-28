// CPU-only tests for the shared cache tier (include/strata/core/lmcache_tier.hpp): its settings, its INFO line, and the
// whole publish/restore path against an in-memory store that behaves like the real one (objects, misses, failures).
//
// The store's wire format is pinned separately (lmcache_wire_test); what is proved here is that a conversation
// survives a round trip through it, and that the objects really carry the same bytes the file layer writes.
#include "strata/core/lmcache_tier.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
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

class FakeStore : public LmcacheStore {
public:
    explicit FakeStore(int64_t per_token) : per_token_(per_token) {}

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
        if (kind.empty() && end - start != chunk_tokens()) { why = "wrong chunk range"; return false; }
        if (end > int64_t(tokens.size())) { why = "object past the prefix"; return false; }
        objects[{kind, start}].assign(static_cast<const char*>(data), bytes);
        if (kind.empty() && end > max_end) max_end = end;
        return true;
    }

    bool get_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, std::string& out, bool& present,
                    std::string& why, const char* salt) override {
        ++gets;
        if (fail_get) { why = "the store is unreachable"; return false; }
        const std::string kind(salt ? salt : "");
        const auto it = objects.find({kind, start});
        if (it == objects.end()) { present = false; return true; }
        if (end > int64_t(tokens.size())) { why = "the prefix is shorter than the stored session"; return false; }
        present = true;
        out = it->second;
        return true;
    }

    // test knobs
    bool fail_open = false, fail_put = false, fail_get = false;
    int puts = 0, gets = 0;
    int64_t max_end = 0;
    std::map<std::pair<std::string, int64_t>, std::string> objects;   // (kind, start): "" = a K/V window

private:
    int64_t per_token_;
    bool open_ = false;
};

// ---- fixtures --------------------------------------------------------------------------------------------------

std::vector<uint8_t> bytes_of(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = uint8_t(i * 7u + seed);
    return v;
}

ConversationCheckpoint checkpoint(size_t tokens, uint8_t seed) {
    ConversationCheckpoint c;
    for (size_t i = 0; i < tokens; ++i) c.ids.push_back(int32_t(1000 + i));
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

//: A state whose K/V can be cut at 256 tokens: 4-cell pages, one indexer row per 4 cells, and each buffer a whole
//: number of bytes per token (32 + 32 + 4 + 4, plus the pooled row's 128) - 200 bytes a token, which is what
//: test_settings() registers and what FakeStore shapes its objects to.
constexpr int64_t kPerToken = 400;   // both layers: 2 x (32 + 32 + 4 + 4) plus the two pooled row streams (2 x 128)
constexpr int64_t kCells = 1024;

Session make_session(size_t tokens, size_t = 0) {
    Session s;
    s.image.geometry = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17};
    s.image.layer_lo = 0;
    s.image.layer_hi = 48;
    s.image.cvec = false;
    s.image.live = checkpoint(int(tokens), 1);   // the engine passes img.live.ids as the conversation's tokens
    s.image.checkpoints.push_back(checkpoint(8, 2));
    for (size_t i = 0; i < tokens; ++i) s.tokens.push_back(int32_t((i * 2654435761u) & 0x7fffffffu));

    const int64_t cells = ((int64_t(tokens) + 255) / 256) * 256;   // whole chunks
    s.parts.resize(2);
    for (size_t layer = 0; layer < s.parts.size(); ++layer) {
        const std::array<size_t, 5> sizes = {size_t(cells) * 32, size_t(cells) * 32, size_t(cells) * 4,
                                             size_t(cells) * 4, size_t(cells / 4 + 1) * 128 * sizeof(float)};
        for (size_t p = 0; p < 5; ++p) s.parts[layer][p] = bytes_of(sizes[p], uint8_t(layer * 5 + p));
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
    return s;
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
    check(raw->puts == 1 + 4, "one record plus four windows of 256 tokens");

    Session grown = make_session(1280);   // the same conversation, two turns later
    const int before = raw->puts;
    check(publish_ok(tier, grown, id, "grown", why), "the grown conversation publishes");
    check(raw->puts - before == 1 + 1, "only the record and the new window are sent, not the five already there");

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

void prefix_is_cut_at_chunk_boundaries() {
    Session s = make_session(1000);   // 3 whole chunks of 256, 232 tokens left over
    auto store = std::make_unique<FakeStore>(kPerToken + 64);
    FakeStore* raw = store.get();
    LmcacheTier tier(test_settings(), std::move(store));
    const SessionFileIdentity id{3, 4};
    std::string why;
    check(publish_ok(tier, s, id, "1000 tokens", why), "a 1000-token session publishes");
    check(raw->max_end == 768, "the stored prefix floors to 768 tokens, never past the last whole chunk");
    int64_t windows = 0;
    for (const auto& kv : raw->objects) windows += kv.first.first.empty() ? 1 : 0;
    check(windows == 3, "three windows of 256 tokens");
    check(raw->objects.size() == 4, "the record and three windows reached the store");

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
        Session big = make_session(9000);
        if (!gated.publish(big.image, big.kv, big.tokens, bid, why)) {
            std::fprintf(stderr, "live gated publish: %s\n", why.c_str());
            return 2;
        }
        SavedConversation got;
        check(gated.restore(big.tokens, bid, got, {}, why), "live: a 9000-token session at the 8192 gate restores");
        check(same_state(big.image, got), "live: the 9000-token state came back identical");

        Session small = make_session(8000);   // below the 8192 gate
        SavedConversation none;
        check(!gated.restore(small.tokens, bid, none, {}, why), "live: 8000 tokens is below the 8192 gate");
        check(why.find("cost gate") != std::string::npos, "live: the gate names itself");
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
    int rc = 0;
    if (argc > 1) rc = live(argv[1]);
    std::printf("lmcache_tier_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : rc;
}
