#include "strata/core/lmcache_store.hpp"

#include "strata/core/lmcache_wire.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace strata::core {
namespace {
// the wire layer's names, kept explicit so this file reads as "the store over the wire" and nothing else
using lmc::CacheKey;
using lmc::ContextSpec;
using lmc::Dealer;
using lmc::Op;
using lmc::Rpc;
using lmc::TensorView;
using lmc::decode_envelope;
using lmc::encode_envelope;
using lmc::pack_tokens;
namespace msgpack = lmc::msgpack;

// lmcache's chunk size.  The server reports it (get_chunk_size), but the object shape has to be known before the first
// call, so it is fixed here and verified on open: a server with another chunk size is refused rather than silently
// addressed wrongly.
constexpr int64_t kChunkTokens = 256;
// Reserved on top of the caller's bytes-per-token: one object must hold one window's record *and* its headers
// (a header per layer and per buffer, plus the trailer), or a window would not fit in the object it is written to.
constexpr int64_t kObjectSlackBytesPerToken = 64;

class ZmqStore : public LmcacheStore {
public:
    ZmqStore(std::string endpoint, std::string model_name, int64_t bytes_per_token, int64_t instance_id)
        : endpoint_(std::move(endpoint)), model_(std::move(model_name)),
          per_token_(std::max<int64_t>(1, bytes_per_token) + kObjectSlackBytesPerToken),
          instance_(instance_id), rpc_(dealer_) {}

    bool open(std::string& why) override {
        if (open_) return true;
        if (opening_) return false;   // never re-enter the handshake from inside a call
        opening_ = true;
        struct Guard {
            bool& f;
            ~Guard() { f = false; }
        } guard{opening_};
        if (!dealer_.connect(endpoint_, why)) {
            error_ = why;
            return false;
        }
        // the server's chunk size must be the one the objects are shaped for
        msgpack::Value reply;
        if (rpc_.call(Op::get_chunk_size, {}, reply, why, 10000)) {
            int64_t chunk = 0;
            if (reply.as_int(chunk) && chunk != kChunkTokens) {
                why = "the store's chunk size is " + std::to_string(chunk) + ", this engine writes " +
                      std::to_string(kChunkTokens);
                dealer_.close();
                error_ = why;
                return false;
            }
        }
        ContextSpec spec;
        spec.instance_id = instance_;
        spec.model_name = model_;
        spec.block_size = kChunkTokens;
        spec.num_layers = 1;
        spec.hidden_dim_size = per_token_;
        spec.num_physical_slots = kChunkTokens;
        msgpack::Writer w = spec.encode();
        std::vector<std::string> args{std::string(w.bytes().begin(), w.bytes().end())};
        if (!rpc_.call(Op::register_context, args, reply, why, 20000)) {
            dealer_.close();
            error_ = why;
            return false;
        }
        open_ = true;
        error_.clear();
        return true;
    }

    void close() override {
        dealer_.close();
        open_ = false;
    }
    bool opened() const override { return open_; }
    size_t object_bytes() const override { return size_t(kChunkTokens) * size_t(per_token_); }
    int64_t chunk_tokens() const override { return kChunkTokens; }

    bool put_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, const void* data, size_t bytes,
                    std::string& why, const char* salt) override {
        if (!open_ && !open(why)) return false;
        const size_t want = object_bytes();
        if (bytes > want) {
            why = "a record is " + std::to_string(bytes) + " bytes, more than the " + std::to_string(want) +
                  "-byte object: raise STRATA_LMCACHE_BYTES_PER_TOKEN by " +
                  std::to_string((bytes - want + size_t(kChunkTokens) - 1) / size_t(kChunkTokens)) + " or more";
            return false;
        }
        // a record shorter than the object is padded: the store holds one shape, whatever the record's own length
        std::vector<uint8_t> padded;
        if (bytes < want) {
            padded.assign(want, 0);
            if (bytes) std::memcpy(padded.data(), data, bytes);
            data = padded.data();
        }
        const CacheKey key = make_key(tokens, start, end, salt);

        // prepare_store takes a tuple of skipped (layer, ...) groups; nothing is skipped here
        msgpack::Writer skipped;
        skipped.array(0);
        msgpack::Writer inst;
        inst.integer(instance_);
        msgpack::Writer k = key.encode();
        std::vector<std::string> args{
            std::string(k.bytes().begin(), k.bytes().end()),
            std::string(inst.bytes().begin(), inst.bytes().end()),
            std::string(skipped.bytes().begin(), skipped.bytes().end()),
        };
        msgpack::Value reply;
        if (!rpc_.call(Op::prepare_store, args, reply, why, 60000)) return false;

        // the object itself: one tile shaped exactly as the context that was registered
        msgpack::Writer env;
        const TensorView tile{data, want};
        encode_envelope(env, &tile, 1, 1, kChunkTokens, per_token_);
        msgpack::Writer data_wire;
        data_wire.binary(env.bytes().data(), env.bytes().size());
        std::vector<std::string> commit_args{
            std::string(k.bytes().begin(), k.bytes().end()),
            std::string(inst.bytes().begin(), inst.bytes().end()),
            std::string(data_wire.bytes().begin(), data_wire.bytes().end()),
        };
        if (!rpc_.call(Op::commit_store, commit_args, reply, why, 120000)) return false;
        bool stored = false;
        if (!reply.as_bool(stored) || !stored) {
            why = "the store refused the object";
            return false;
        }
        return true;
    }

    bool get_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, std::string& out, bool& present,
                    std::string& why, const char* salt) override {
        present = false;
        out.clear();
        if (!open_ && !open(why)) return false;
        const CacheKey key = make_key(tokens, start, end, salt);
        msgpack::Writer inst;
        inst.integer(instance_);
        msgpack::Writer k = key.encode();

        // three steps, in this order: lookup takes the read lock (and starts a prefetch when the object is only in a
        // slower tier), prepare_retrieve hands the bytes over, commit_retrieve releases the lock.  Skipping the lookup
        // makes prepare_retrieve answer success=false forever - the store needs the lock before it will serve.
        msgpack::Writer tp;
        tp.integer(1);
        std::vector<std::string> lookup_args{
            std::string(k.bytes().begin(), k.bytes().end()),
            std::string(tp.bytes().begin(), tp.bytes().end()),
        };
        msgpack::Value hit;
        // A lookup is on the request's critical path: it runs before the prefill can start.  A store that accepts the
        // request but never answers must not cost more than a moment - the caller then treats it as a miss and
        // prefills.  Four seconds covers a busy store; thirty froze the first token of every request.
        if (!rpc_.call(Op::lookup, lookup_args, hit, why, 4000)) return false;
        bool found = false;
        if (hit.kind == msgpack::Value::Kind::array) {   // per-chunk flags: any non-zero entry is a hit
            for (const auto& v : hit.arr) {
                int64_t n = 0;
                if (v.as_int(n) && n > 0) found = true;
            }
        } else if (hit.kind == msgpack::Value::Kind::map) {
            if (const msgpack::Value* h = hit.find("hit_chunks")) {
                int64_t n = 0;
                if (h->as_int(n) && n > 0) found = true;
            }
        } else if (hit.kind == msgpack::Value::Kind::boolean) {
            found = hit.b;
        } else if (hit.kind == msgpack::Value::Kind::integer) {
            found = hit.i > 0;
        }
        if (!found) {
            present = false;   // a miss, not an error
            return true;
        }

        std::vector<std::string> args{
            std::string(k.bytes().begin(), k.bytes().end()),
            std::string(inst.bytes().begin(), inst.bytes().end()),
        };
        msgpack::Value reply;
        msgpack::Value ignored;
        std::string release_why;
        bool success = false;
        for (int attempt = 0; attempt < 20 && !success; ++attempt) {
            if (!rpc_.call(Op::prepare_retrieve, args, reply, why, 10000)) return false;
            const msgpack::Value* ok = reply.find("success");
            success = ok && ok->b;
            if (!success) {
                // the object is in a slower tier: the lookup started its prefetch, give it a moment
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (success) {
            const msgpack::Value* data = reply.find("data");
            if (data && !data->s.empty()) {
                const bool decoded = decode_envelope(data->s, out, why);
                rpc_.call(Op::commit_retrieve, args, ignored, release_why, 30000);
                if (!decoded) return false;
                present = !out.empty();
                if (present && out.size() != object_bytes()) {
                    why = "the store returned " + std::to_string(out.size()) + " bytes for a " +
                          std::to_string(object_bytes()) + "-byte object";
                    present = false;
                    out.clear();
                    return false;
                }
                return true;
            }
        }
        rpc_.call(Op::commit_retrieve, args, ignored, release_why, 30000);   // releases the read lock either way
        if (!success) {
            why = "the store has the object but would not serve it";
            return false;
        }
        present = false;
        return true;
    }

private:
    CacheKey make_key(const std::vector<int32_t>& tokens, int64_t start, int64_t end, const char* salt = "") const {
        CacheKey key;
        key.model_name = model_;
        key.cache_salt = salt ? salt : "";
        key.start = start;
        key.end = end;
        key.request_id = "strata";
        key.num_kv_readers = 1;
        // lmcache hashes the whole prefix up to `end` and selects [start, end) from it: the token bytes are the prefix,
        // not just this chunk (IPCCacheServerKey.from_token_ids does the same)
        const size_t up_to = size_t(std::min<int64_t>(end, int64_t(tokens.size())));
        key.token_bytes = pack_tokens(tokens.data(), up_to);
        return key;
    }

    std::string endpoint_, model_;
    int64_t per_token_;
    int64_t instance_;
    Dealer dealer_;
    Rpc rpc_;
    bool open_ = false, opening_ = false;
    std::string error_;
};

}  // namespace

std::unique_ptr<LmcacheStore> lmcache_zmq_store(std::string endpoint, std::string model_name, int64_t bytes_per_token,
                                               int64_t instance_id) {
    return std::make_unique<ZmqStore>(std::move(endpoint), std::move(model_name), bytes_per_token, instance_id);
}

}  // namespace strata::core
