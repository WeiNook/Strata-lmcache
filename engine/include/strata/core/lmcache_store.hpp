// What the shared tier needs from the store, and the ZMQ client that provides it (docs/LMCACHE.md).
//
// The unit the store understands is a CHUNK: `chunk_size` tokens (256 by default) of one model, held as one opaque
// object whose bytes are `256 x bytes_per_token`.  A session is therefore a CHAIN of objects, each addressed by its
// own token range - which is how LMCache addresses any engine's K/V, and why a partial session is usable: the objects
// that are still resident can be read, and only the missing tail is recomputed.
//
// The interface is deliberately whole-object and copy-based: the store is a network peer, so an object is either
// here or it is not.  `LmcacheSink`/`LmcacheSource` in lmcache_tier.cpp cut the byte stream into these objects, so the
// caller never sees them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace strata::core {

class LmcacheStore {
public:
    virtual ~LmcacheStore() = default;

    // Connect and register this process's context (one layer, uint8, `block_size` slots of `bytes_per_token`).
    // Idempotent: a second call after a failure retries the whole handshake.
    virtual bool open(std::string& why) = 0;
    virtual void close() = 0;
    virtual bool opened() const = 0;

    // Object [start, end) of `tokens` (end - start == chunk size).  `data` is exactly the object's bytes.  A `false`
    // return means the store refused or is unreachable: the caller's request goes on without the cache.
    // `salt` separates the kinds of object one prefix has ("" = the K/V window at that range, "meta" = the
    // conversation's own record).  `bytes` may be smaller than `object_bytes()`: the store pads it.
    virtual bool put_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, const void* data,
                            size_t bytes, std::string& why, const char* salt = "") = 0;
    // Fetches an object into `out`.  `present` is false when the store simply does not have it (a miss, not an
    // error); `false` as the return value is an error.
    virtual bool get_object(const std::vector<int32_t>& tokens, int64_t start, int64_t end, std::string& out,
                            bool& present, std::string& why, const char* salt = "") = 0;

    // The bytes one object holds, fixed for the life of the store connection.  It carries a little more than the
    // state's own `chunk x bytes_per_token`, so one window's record (its headers included) fits in one object.
    virtual size_t object_bytes() const = 0;
    virtual int64_t chunk_tokens() const = 0;
};

// The real one: a DEALER to the LMCache MP server (lmcache_wire.hpp), one registered context, objects chunked as above.
// Construction does not touch the network; `open()` does, and a failed open is retried by the next call that needs it.
std::unique_ptr<LmcacheStore> lmcache_zmq_store(std::string endpoint, std::string model_name, int64_t bytes_per_token,
                                               int64_t instance_id);

}  // namespace strata::core
