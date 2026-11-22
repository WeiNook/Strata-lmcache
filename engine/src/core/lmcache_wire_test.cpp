// CPU-only tests for the LMCache wire code (include/strata/core/lmcache_wire.hpp): every encoding is compared with the
// bytes lmcache itself produced for the same value (tools/lmcache/dump_wire.py prints them), so a decoder/encoder
// written here cannot drift from the server's idea of the format.  With no argument the test is offline; with an
// endpoint it also drives a live server:
//
//   lmcache_wire_test                                  # ctest: encodings only
//   lmcache_wire_test tcp://127.0.0.1:5555             # + ping and get_chunk_size
//   lmcache_wire_test tcp://127.0.0.1:5555 4           # + a 4-chunk store/retrieve round trip
#include "strata/core/lmcache_wire.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace strata::core::lmc;

namespace {
int checks = 0, failures = 0;

std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t c : v) { s.push_back(d[c >> 4]); s.push_back(d[c & 0xf]); }
    return s;
}

void same(const std::vector<uint8_t>& got, const char* want, const char* label) {
    ++checks;
    const std::string g = hex(got);
    if (g != want) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", label, g.c_str(), want);
    }
}

void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
}

// the encodings msgpack uses for the values an RPC actually carries
void golden_scalars() {
    { msgpack::Writer w; w.nil(); same(w.bytes(), "c0", "nil"); }
    { msgpack::Writer w; w.boolean(true); same(w.bytes(), "c3", "true"); }
    { msgpack::Writer w; w.boolean(false); same(w.bytes(), "c2", "false"); }
    { msgpack::Writer w; w.integer(0); same(w.bytes(), "00", "int 0"); }
    { msgpack::Writer w; w.integer(1); same(w.bytes(), "01", "int 1"); }
    { msgpack::Writer w; w.integer(127); same(w.bytes(), "7f", "int 127"); }
    { msgpack::Writer w; w.integer(128); same(w.bytes(), "cc80", "int 128"); }
    { msgpack::Writer w; w.integer(255); same(w.bytes(), "ccff", "int 255"); }
    { msgpack::Writer w; w.integer(256); same(w.bytes(), "cd0100", "int 256"); }
    { msgpack::Writer w; w.integer(64768); same(w.bytes(), "cdfd00", "int 64768"); }
    { msgpack::Writer w; w.integer(-1); same(w.bytes(), "ff", "int -1"); }
    { msgpack::Writer w; w.str("abc"); same(w.bytes(), "a3616263", "str abc"); }
    { msgpack::Writer w; w.binary("abc", 3); same(w.bytes(), "c403616263", "bin abc"); }
    { msgpack::Writer w; w.array(0); same(w.bytes(), "90", "empty array"); }
}

// IPCCacheServerKey.from_token_ids(model_name="m", world_size=1, worker_id=0, token_ids=(1,2,3), start=0, end=256,
//                                  request_id="r1", num_kv_readers=1)
void golden_key() {
    const int32_t ids[3] = {1, 2, 3};
    CacheKey k;
    k.model_name = "m";
    k.world_size = 1;
    k.worker_id = 0;
    k.token_bytes = pack_tokens(ids, 3);
    k.start = 0;
    k.end = 256;
    k.request_id = "r1";
    k.num_kv_readers = 1;
    msgpack::Writer w = k.encode();
    same(w.bytes(),
         "8aaa6d6f64656c5f6e616d65a16daa776f726c645f73697a6501a9776f726b65725f696400ab746f6b656e5f6279746573"
         "c40c000000010000000200000003a5737461727400a3656e64cd0100aa726571756573745f6964a27231aa63616368655f"
         "73616c74a0af726571756573745f636f6e66696773c0ae6e756d5f6b765f7265616465727301",
         "IPCCacheServerKey");
}

// RegisterEngineDrivenContextPayload(instance_id=4242, model_name="m", world_size=1, block_size=256, num_layers=1,
//                                    hidden_dim_size=19531, dtype_str="uint8", use_mla=True, num_physical_slots=256)
void golden_context() {
    ContextSpec c;
    c.instance_id = 4242;
    c.model_name = "m";
    c.block_size = 256;
    c.num_layers = 1;
    c.hidden_dim_size = 19531;
    c.num_physical_slots = 256;
    msgpack::Writer w = c.encode();
    same(w.bytes(),
         "8aab696e7374616e63655f6964cd1092aa6d6f64656c5f6e616d65a16daa776f726c645f73697a6501aa626c6f636b5f73697a"
         "65cd0100aa6e756d5f6c617965727301af68696464656e5f64696d5f73697a65cd4c4ba964747970655f737472a575696e74"
         "38a77573655f6d6c61c3b26e756d5f706879736963616c5f736c6f7473cd0100ad67726f75705f6c61796f75747390",
         "RegisterEngineDrivenContextPayload");
}

// encode_tensors([torch.zeros((1,1,4), uint8)])
void golden_envelope() {
    const uint8_t zeros[4] = {0, 0, 0, 0};
    const TensorView tile{zeros, sizeof zeros};
    msgpack::Writer w;
    encode_envelope(w, &tile, 1, 1, 1, 4);
    same(w.bytes(),
         "82a776657273696f6e01a774656e736f72739183a56474797065a575696e7438a5736861706593010104a464617461c40400"
         "000000",
         "TensorEnvelope");

    // and back out again
    std::string out, why;
    const std::string bytes(reinterpret_cast<const char*>(w.bytes().data()), w.bytes().size());
    check(decode_envelope(bytes, out, why) && out.size() == 4, "envelope decodes to the tile");
}

void roundtrip_values() {
    msgpack::Writer w;
    w.map(2);
    w.str("a"); w.integer(-70000);
    w.str("b"); w.binary("\x00\x01\x02", 3);
    msgpack::Value v;
    std::string why;
    check(msgpack::decode(w.bytes().data(), w.bytes().size(), v, why), "decode a map");
    const msgpack::Value* a = v.find("a");
    int64_t i = 0;
    check(a && a->as_int(i) && i == -70000, "int round trips");
    const msgpack::Value* b = v.find("b");
    check(b && b->kind == msgpack::Value::Kind::binary && b->s.size() == 3, "binary round trips");

    // a truncated object is refused, not read past
    msgpack::Value bad;
    check(!msgpack::decode(w.bytes().data(), w.bytes().size() - 1, bad, why), "a truncated object is refused");
}

int live(const std::string& endpoint) {
    Dealer d;
    std::string why;
    if (!d.connect(endpoint, why)) {
        std::fprintf(stderr, "connect %s: %s\n", endpoint.c_str(), why.c_str());
        return 2;
    }
    std::printf("connected to %s\n", endpoint.c_str());
    Rpc rpc(d);

    msgpack::Writer none;           // ping's only argument is `int | None`
    none.nil();
    std::vector<std::string> args{std::string(none.bytes().begin(), none.bytes().end())};
    msgpack::Value reply;
    check(rpc.call(Op::ping, args, reply, why), "ping");
    bool alive = false;
    check(reply.as_bool(alive) && alive, "ping answered true");

    check(rpc.call(Op::get_chunk_size, {}, reply, why), "get_chunk_size");
    int64_t chunk = 0;
    check(reply.as_int(chunk) && chunk > 0, "get_chunk_size answered");
    std::printf("server chunk size: %lld\n", (long long) chunk);

    // NOT tested here on purpose: a request with the wrong number of payloads.  In 0.5.7 the server's only dispatch
    // thread raises on it (`assert len(msg) >= 3`) and dies, so the server keeps accepting connections and answering
    // nothing.  Every call below therefore builds its payload list from the contract, never by guesswork.
    d.close();
    return 0;
}
// The whole data path from C++: register an opaque context, store a blob as N chunks, look it up, fetch it back.
// `chunks` chunks of 256 tokens x `hidden` bytes = an opaque blob of that many bytes, the shape P0 established.
int live_roundtrip(const std::string& endpoint, int chunks) {
    const int64_t kChunk = 256;         // the server's chunk size (get_chunk_size)
    const int64_t kHidden = 19531;      // bytes per token: ~19 KB/token is what this model's state costs
    const size_t per_chunk = size_t(kChunk) * size_t(kHidden);

    Dealer d;
    std::string why;
    if (!d.connect(endpoint, why)) {
        std::fprintf(stderr, "connect %s: %s\n", endpoint.c_str(), why.c_str());
        return 2;
    }
    Rpc rpc(d);
    rpc.set_timeout(120000);

    // 1. register the context: one layer, uint8, one physical slot per token in a chunk
    ContextSpec spec;
    spec.instance_id = 4242;
    spec.model_name = "strata/selftest";
    spec.block_size = kChunk;
    spec.num_layers = 1;
    spec.hidden_dim_size = kHidden;
    spec.num_physical_slots = kChunk;
    {
        msgpack::Writer w = spec.encode();
        std::vector<std::string> args{std::string(w.bytes().begin(), w.bytes().end())};
        msgpack::Value reply;
        check(rpc.call(Op::register_context, args, reply, why), "register_context");
        if (!reply.obj.empty()) {
            std::printf("registered: shm=%s pool=%lld\n",
                        reply.find("shm_name") ? reply.find("shm_name")->s.c_str() : "?",
                        reply.find("pool_size") ? (long long) reply.find("pool_size")->i : -1);
        }
        if (!why.empty()) std::printf("  (%s)\n", why.c_str());
    }

    // 2. the blob: chunk-aligned tokens, and bytes that cannot compress or repeat
    const int64_t tokens = int64_t(chunks) * kChunk;
    std::vector<int32_t> ids(static_cast<size_t>(tokens));
    for (int64_t i = 0; i < tokens; ++i) ids[size_t(i)] = int32_t((uint64_t(i) * 2654435761u) & 0x7fffffffu);
    CacheKey key;
    key.model_name = spec.model_name;
    key.token_bytes = pack_tokens(ids.data(), ids.size());
    key.start = 0;
    key.end = tokens;
    key.request_id = "cpp-selftest";
    key.num_kv_readers = 1;

    std::vector<uint8_t> blob(per_chunk * size_t(chunks));
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = uint8_t((i * 131u + (i >> 13)) & 0xff);

    // 3. the envelope: one tile per chunk
    std::vector<TensorView> tiles(static_cast<size_t>(chunks));
    for (int c = 0; c < chunks; ++c) tiles[size_t(c)] = TensorView{blob.data() + per_chunk * size_t(c), per_chunk};
    msgpack::Writer env;
    encode_envelope(env, tiles.data(), tiles.size(), 1, kChunk, kHidden);

    {
        msgpack::Writer skipped;
        skipped.array(0);                            // tuple[tuple[int, ...], ...]: nothing skipped
        msgpack::Writer inst;
        inst.integer(spec.instance_id);
        msgpack::Writer k = key.encode();
        std::vector<std::string> args{
            std::string(k.bytes().begin(), k.bytes().end()),
            std::string(inst.bytes().begin(), inst.bytes().end()),
            std::string(skipped.bytes().begin(), skipped.bytes().end()),
        };
        msgpack::Value reply;
        check(rpc.call(Op::prepare_store, args, reply, why), "prepare_store");
        if (!why.empty()) std::printf("  prepare_store: %s\n", why.c_str());
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        msgpack::Writer inst;
        inst.integer(spec.instance_id);
        msgpack::Writer k = key.encode();
        msgpack::Writer data;
        data.binary(env.bytes().data(), env.bytes().size());
        std::vector<std::string> args{
            std::string(k.bytes().begin(), k.bytes().end()),
            std::string(inst.bytes().begin(), inst.bytes().end()),
            std::string(data.bytes().begin(), data.bytes().end()),
        };
        msgpack::Value reply;
        const bool ok = rpc.call(Op::commit_store, args, reply, why);
        bool stored = false;
        reply.as_bool(stored);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("store: %zu MiB in %.2f s = %.0f MiB/s (ok=%d)\n", blob.size() >> 20, s,
                    double(blob.size() >> 20) / (s > 0 ? s : 1), int(stored));
        check(ok && stored, "commit_store");
        if (!why.empty()) std::printf("  commit_store: %s\n", why.c_str());
    }

    // 4. lookup, then fetch it back byte for byte
    int64_t hit_chunks = -1;
    {
        msgpack::Writer tp;
        tp.integer(1);
        msgpack::Writer k = key.encode();
        std::vector<std::string> args{std::string(k.bytes().begin(), k.bytes().end()),
                                      std::string(tp.bytes().begin(), tp.bytes().end())};
        msgpack::Value reply;
        if (rpc.call(Op::lookup, args, reply, why)) {
            if (reply.kind == msgpack::Value::Kind::map) {
                for (const auto& kv : reply.obj) std::printf("  lookup.%s = %lld\n", kv.first.c_str(), (long long) kv.second.i);
                if (const msgpack::Value* h = reply.find("hit_chunks")) hit_chunks = h->i;
            } else {
                std::printf("  lookup reply kind %d\n", int(reply.kind));
            }
        } else {
            std::printf("  lookup: %s\n", why.c_str());
        }
    }
    {
        msgpack::Writer inst;
        inst.integer(spec.instance_id);
        msgpack::Writer k = key.encode();
        std::vector<std::string> args{std::string(k.bytes().begin(), k.bytes().end()),
                                      std::string(inst.bytes().begin(), inst.bytes().end())};
        msgpack::Value reply;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = rpc.call(Op::prepare_retrieve, args, reply, why);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        check(ok, "prepare_retrieve");
        if (!ok) { std::printf("  prepare_retrieve: %s\n", why.c_str()); return 2; }
        const msgpack::Value* okv = reply.find("success");
        const msgpack::Value* datav = reply.find("data");
        const bool success = okv && okv->b;
        std::printf("retrieve: success=%d data=%zu bytes in %.2f s\n", int(success),
                    datav ? datav->s.size() : 0, s);
        check(success, "prepare_retrieve says success");
        if (datav && !datav->s.empty()) {
            std::string out;
            check(decode_envelope(datav->s, out, why), "the reply envelope decodes");
            check(out.size() == blob.size() && std::memcmp(out.data(), blob.data(), blob.size()) == 0,
                  "the bytes came back identical");
            std::printf("byte-exact: %d (%zu of %zu bytes)\n", int(out == std::string(blob.begin(), blob.end())),
                        out.size(), blob.size());
        }
    }
    {
        msgpack::Writer inst;
        inst.integer(spec.instance_id);
        msgpack::Writer k = key.encode();
        std::vector<std::string> args{std::string(k.bytes().begin(), k.bytes().end()),
                                      std::string(inst.bytes().begin(), inst.bytes().end())};
        msgpack::Value reply;
        check(rpc.call(Op::commit_retrieve, args, reply, why), "commit_retrieve");
        if (!why.empty()) std::printf("  commit_retrieve: %s\n", why.c_str());
    }
    (void) hit_chunks;
    d.close();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    golden_scalars();
    golden_key();
    golden_context();
    golden_envelope();
    roundtrip_values();
    int rc = 0;
    if (argc > 1) rc = live(argv[1]);
    if (rc == 0 && argc > 2) rc = live_roundtrip(argv[1], std::atoi(argv[2]));
    std::printf("wire_selftest: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : rc;
}
