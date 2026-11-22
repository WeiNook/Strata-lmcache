// The wire the LMCache server speaks, in C++: msgpack (the subset its RPC uses) and a ZMTP 3.1 DEALER socket, plus
// the handful of RPC payloads a cache tier needs.  Nothing here knows about conversations: this is transport only.
//
// Contract (read off lmcache 0.5.7.post1 with tools/lmcache/dump_wire.py, which prints the golden bytes the tests
// below assert):
//   * the server binds a ROUTER on tcp://host:PORT; we connect as a DEALER, so messages are multipart frames both ways;
//   * a request is [msgpack int uid][msgpack int op][msgpack payload...], one payload per RPC parameter;
//   * a reply is [msgpack int uid][msgpack reply]; operations report failures as the reserved op
//     "__lmcache_rpc_error__" whose payload is {code, operation, message};
//   * payload structs are msgpack MAPS keyed by field name (msgspec's default), so field order does not matter;
//   * the big payload of commit_store / prepare_retrieve is a raw msgpack bin, not a nested structure.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace strata::core::lmc {

namespace msgpack {
// Appends one msgpack object at a time; the caller names the shape (map(n) first, then 2n objects, ...).
class Writer {
public:
    void nil();
    void boolean(bool v);
    void integer(int64_t v);
    void uinteger(uint64_t v);
    void real(double v);
    void str(const std::string& s);
    void binary(const void* p, size_t n);
    void array(uint32_t n);
    void map(uint32_t n);
    const std::vector<uint8_t>& bytes() const { return out_; }
    std::vector<uint8_t>& bytes() { return out_; }
    void clear() { out_.clear(); }
private:
    std::vector<uint8_t> out_;
};

// One decoded msgpack object.  A map keeps its keys as strings and its values in order.
struct Value {
    enum class Kind { nil, boolean, integer, real, str, binary, array, map } kind = Kind::nil;
    bool b = false;
    int64_t i = 0;
    double r = 0;
    std::string s;                                  // the bytes of a str or a binary
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    const Value* find(const char* key) const;
    bool as_int(int64_t& v) const;
    bool as_bool(bool& v) const;
    bool as_bytes(std::string& v) const;            // str or binary
};

// Exactly one object, and nothing after it.  `why` says what was wrong (truncated, bad tag, trailing bytes).
bool decode(const uint8_t* p, size_t n, Value& out, std::string& why);
}  // namespace msgpack

// The client end of the socket lmcache server binds.  ZMTP 3.1, NULL mechanism, Socket-Type DEALER.
class Dealer {
public:
    Dealer() = default;
    ~Dealer();
    Dealer(const Dealer&) = delete;
    Dealer& operator=(const Dealer&) = delete;

    // "tcp://host:port" (the form the server prints).  A refused connection is reported, not fatal: the caller
    // retries when its cache tier wants the store again.
    bool connect(const std::string& endpoint, std::string& why);
    void close();
    bool connected() const { return fd_ >= 0; }

    bool send(const std::vector<std::string>& frames, std::string& why);
    // timeout_ms < 0 waits forever; 0 polls.  A closed socket or a broken frame is an error in `why`.
    bool recv(std::vector<std::string>& frames, int timeout_ms, std::string& why);

private:
    bool handshake(std::string& why);
    bool write_all(const void* p, size_t n, std::string& why);
    bool read_exact(void* p, size_t n, int timeout_ms, std::string& why);
    int fd_ = -1;
    std::string peer_error_;   // a socket error found while reading; kept for the next failed call
};

// The operation ids the server routes on (lmc/v1/multiprocess/transport/zmq_impl/wire.py's LEGACY_OPERATION_IDS:
// these integers are what a client sends, and what the server still accepts for every operation it has).
enum class Op : int32_t {
    lookup = 8,
    query_prefetch_status = 9,
    register_context = 14,
    unregister_context = 15,
    prepare_store = 16,
    commit_store = 17,
    prepare_retrieve = 18,
    commit_retrieve = 19,
    get_chunk_size = 21,
    ping = 22,
};

// IPCCacheServerKey: the address of one token range of one model.  `token_bytes` is lmcache's pack_tokens: 4 bytes
// big-endian per token id.  `worker_id` < 0 is encoded as null (the server accepts either).
struct CacheKey {
    std::string model_name;
    int64_t world_size = 1;
    int64_t worker_id = 0;
    std::string token_bytes;
    int64_t start = 0, end = 0;
    std::string request_id;
    std::string cache_salt;
    int64_t num_kv_readers = 1;
    msgpack::Writer encode() const;   // a map of the ten fields, request_configs: nil
};

// lmcache's pack_tokens: one big-endian uint32 per id, which is what the server hashes.
std::string pack_tokens(const int32_t* ids, size_t n);

// The context registration a non-vLLM engine must send once before it stores anything.
struct ContextSpec {
    int64_t instance_id = 0;
    std::string model_name;
    int64_t world_size = 1;
    int64_t block_size = 256;
    int64_t num_layers = 1;
    int64_t hidden_dim_size = 0;    // bytes per token, flattened
    std::string dtype_str = "uint8";
    bool use_mla = true;            // the shape without a K/V=2 dimension
    int64_t num_physical_slots = 256;
    msgpack::Writer encode() const;
};

// One opaque tensor of a chunk payload: what commit_store carries.
struct TensorView {
    const void* data = nullptr;
    size_t bytes = 0;
};
// commit_store's argument: msgpack {version:1, tensors:[{dtype, shape, data}]}.  One tensor per chunk object, and its
// shape is the (layers, slots, hidden) the server validated at registration - the same numbers for every chunk.
void encode_envelope(msgpack::Writer& w, const TensorView* tiles, size_t n, int64_t layers, int64_t slots,
                     int64_t hidden);
// prepare_retrieve's reply payload: the same envelope, decoded back into the caller's buffer.
bool decode_envelope(const std::string& payload, std::string& out, std::string& why);

// One RPC call: request frames, reply frames, and the decoded reply value.  `uid` is ours (any increasing number).
class Rpc {
public:
    explicit Rpc(Dealer& d) : dealer_(d) {}
    // `payloads` are objects already appended to `w` — the caller keeps one Writer per call so a payload can be huge.
    bool call(Op op, const std::vector<std::string>& payloads, msgpack::Value& reply, std::string& why,
              int timeout_ms = 30000);
    void set_timeout(int timeout_ms) { timeout_ms_ = timeout_ms; }
private:
    Dealer& dealer_;
    int64_t uid_ = 0;
    int timeout_ms_ = 30000;
};
}  // namespace strata::core::lmc
