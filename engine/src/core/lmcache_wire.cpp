#include "strata/core/lmcache_wire.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace strata::core::lmc {
namespace {

constexpr uint8_t kFlagMore = 0x01, kFlagLong = 0x02, kFlagCommand = 0x04;

void put_be16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
void put_be32(std::vector<uint8_t>& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
void put_be64(std::vector<uint8_t>& v, uint64_t x) {
    for (int s = 56; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
uint32_t get_be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | uint32_t(p[1]); }
uint32_t get_be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
uint64_t get_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = v << 8 | p[i];
    return v;
}
}  // namespace

// ---- msgpack --------------------------------------------------------------------------------------------------
namespace msgpack {
void Writer::nil() { out_.push_back(0xc0); }
void Writer::boolean(bool v) { out_.push_back(v ? 0xc3 : 0xc2); }

void Writer::integer(int64_t v) {
    if (v >= 0) { uinteger(uint64_t(v)); return; }
    if (v >= -32) { out_.push_back(uint8_t(0xe0 | (v + 32))); return; }
    if (v >= INT8_MIN) { out_.push_back(0xd0); out_.push_back(uint8_t(int8_t(v))); return; }
    if (v >= INT16_MIN) { out_.push_back(0xd1); put_be16(out_, uint16_t(int16_t(v))); return; }
    if (v >= INT32_MIN) { out_.push_back(0xd2); put_be32(out_, uint32_t(int32_t(v))); return; }
    out_.push_back(0xd3);
    put_be64(out_, uint64_t(v));
}

void Writer::uinteger(uint64_t v) {
    if (v < 0x80) { out_.push_back(uint8_t(v)); return; }
    if (v <= 0xff) { out_.push_back(0xcc); out_.push_back(uint8_t(v)); return; }
    if (v <= 0xffff) { out_.push_back(0xcd); put_be16(out_, uint16_t(v)); return; }
    if (v <= 0xffffffffull) { out_.push_back(0xce); put_be32(out_, uint32_t(v)); return; }
    out_.push_back(0xcf);
    put_be64(out_, v);
}

void Writer::real(double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    out_.push_back(0xcb);
    put_be64(out_, bits);
}

void Writer::str(const std::string& s) {
    const size_t n = s.size();
    if (n < 32) out_.push_back(uint8_t(0xa0 | n));
    else if (n <= 0xff) { out_.push_back(0xd9); out_.push_back(uint8_t(n)); }
    else if (n <= 0xffff) { out_.push_back(0xda); put_be16(out_, uint16_t(n)); }
    else { out_.push_back(0xdb); put_be32(out_, uint32_t(n)); }
    out_.insert(out_.end(), s.begin(), s.end());
}

void Writer::binary(const void* p, size_t n) {
    if (n <= 0xff) { out_.push_back(0xc4); out_.push_back(uint8_t(n)); }
    else if (n <= 0xffff) { out_.push_back(0xc5); put_be16(out_, uint16_t(n)); }
    else { out_.push_back(0xc6); put_be32(out_, uint32_t(n)); }
    const uint8_t* b = static_cast<const uint8_t*>(p);
    out_.insert(out_.end(), b, b + n);
}

void Writer::array(uint32_t n) {
    if (n < 16) out_.push_back(uint8_t(0x90 | n));
    else if (n <= 0xffff) { out_.push_back(0xdc); put_be16(out_, uint16_t(n)); }
    else { out_.push_back(0xdd); put_be32(out_, n); }
}

void Writer::map(uint32_t n) {
    if (n < 16) out_.push_back(uint8_t(0x80 | n));
    else if (n <= 0xffff) { out_.push_back(0xde); put_be16(out_, uint16_t(n)); }
    else { out_.push_back(0xdf); put_be32(out_, n); }
}

const Value* Value::find(const char* key) const {
    for (const auto& kv : obj) if (kv.first == key) return &kv.second;
    return nullptr;
}
bool Value::as_int(int64_t& v) const {
    if (kind != Kind::integer) return false;
    v = i;
    return true;
}
bool Value::as_bool(bool& v) const {
    if (kind != Kind::boolean) return false;
    v = b;
    return true;
}
bool Value::as_bytes(std::string& v) const {
    if (kind != Kind::str && kind != Kind::binary) return false;
    v = s;
    return true;
}

namespace {
// Decodes one object at `p`, moving it forward.  Depth is bounded: a hostile or corrupt message cannot recurse away.
bool decode_at(const uint8_t*& p, const uint8_t* end, Value& out, std::string& why, int depth) {
    if (depth > 32) { why = "msgpack nested too deeply"; return false; }
    if (p >= end) { why = "msgpack ends early"; return false; }
    const uint8_t tag = *p++;

    auto need = [&](size_t n) {
        if (size_t(end - p) < n) { why = "msgpack ends early"; return false; }
        return true;
    };
    auto read_str = [&](size_t n) {
        if (!need(n)) return false;
        out.s.assign(reinterpret_cast<const char*>(p), n);
        p += n;
        return true;
    };
    auto read_array = [&](size_t n) {
        if (n > 0xffffffffull) { why = "msgpack array too long"; return false; }
        out.kind = Value::Kind::array;
        out.arr.clear();
        out.arr.reserve(std::min<size_t>(n, 4096));
        for (size_t i = 0; i < n; ++i) {
            out.arr.emplace_back();
            if (!decode_at(p, end, out.arr.back(), why, depth + 1)) return false;
        }
        return true;
    };
    auto read_map = [&](size_t n) {
        out.kind = Value::Kind::map;
        out.obj.clear();
        out.obj.reserve(std::min<size_t>(n, 1024));
        for (size_t i = 0; i < n; ++i) {
            Value key;
            if (!decode_at(p, end, key, why, depth + 1)) return false;
            if (key.kind != Value::Kind::str) { why = "msgpack map key is not a string"; return false; }
            out.obj.emplace_back(std::move(key.s), Value{});
            if (!decode_at(p, end, out.obj.back().second, why, depth + 1)) return false;
        }
        return true;
    };

    if (tag <= 0x7f) { out.kind = Value::Kind::integer; out.i = tag; return true; }
    if (tag >= 0xe0) { out.kind = Value::Kind::integer; out.i = int64_t(int8_t(tag)); return true; }
    if ((tag & 0xf0) == 0x80) return read_map(tag & 0x0f);
    if ((tag & 0xf0) == 0x90) return read_array(tag & 0x0f);
    if ((tag & 0xe0) == 0xa0) { out.kind = Value::Kind::str; return read_str(tag & 0x1f); }

    switch (tag) {
        case 0xc0: out.kind = Value::Kind::nil; return true;
        case 0xc2: out.kind = Value::Kind::boolean; out.b = false; return true;
        case 0xc3: out.kind = Value::Kind::boolean; out.b = true; return true;
        case 0xc4: if (!need(1)) return false; { const size_t n = *p++; out.kind = Value::Kind::binary; return read_str(n); }
        case 0xc5: if (!need(2)) return false; { const size_t n = get_be16(p); p += 2; out.kind = Value::Kind::binary; return read_str(n); }
        case 0xc6: if (!need(4)) return false; { const size_t n = get_be32(p); p += 4; out.kind = Value::Kind::binary; return read_str(n); }
        case 0xca: if (!need(4)) return false; { const uint32_t b = get_be32(p); p += 4; float f; std::memcpy(&f, &b, 4); out.kind = Value::Kind::real; out.r = f; return true; }
        case 0xcb: if (!need(8)) return false; { const uint64_t b = get_be64(p); p += 8; double d; std::memcpy(&d, &b, 8); out.kind = Value::Kind::real; out.r = d; return true; }
        case 0xcc: if (!need(1)) return false; out.kind = Value::Kind::integer; out.i = *p++; return true;
        case 0xcd: if (!need(2)) return false; out.kind = Value::Kind::integer; out.i = get_be16(p); p += 2; return true;
        case 0xce: if (!need(4)) return false; out.kind = Value::Kind::integer; out.i = get_be32(p); p += 4; return true;
        case 0xcf: if (!need(8)) return false; { const uint64_t v = get_be64(p); p += 8; out.kind = Value::Kind::integer; out.i = int64_t(v); return true; }
        case 0xd0: if (!need(1)) return false; out.kind = Value::Kind::integer; out.i = int64_t(int8_t(*p++)); return true;
        case 0xd1: if (!need(2)) return false; out.kind = Value::Kind::integer; out.i = int64_t(int16_t(get_be16(p))); p += 2; return true;
        case 0xd2: if (!need(4)) return false; out.kind = Value::Kind::integer; out.i = int64_t(int32_t(get_be32(p))); p += 4; return true;
        case 0xd3: if (!need(8)) return false; out.kind = Value::Kind::integer; out.i = int64_t(get_be64(p)); p += 8; return true;
        case 0xd9: if (!need(1)) return false; { const size_t n = *p++; out.kind = Value::Kind::str; return read_str(n); }
        case 0xda: if (!need(2)) return false; { const size_t n = get_be16(p); p += 2; out.kind = Value::Kind::str; return read_str(n); }
        case 0xdb: if (!need(4)) return false; { const size_t n = get_be32(p); p += 4; out.kind = Value::Kind::str; return read_str(n); }
        case 0xdc: if (!need(2)) return false; { const size_t n = get_be16(p); p += 2; return read_array(n); }
        case 0xdd: if (!need(4)) return false; { const size_t n = get_be32(p); p += 4; return read_array(n); }
        case 0xde: if (!need(2)) return false; { const size_t n = get_be16(p); p += 2; return read_map(n); }
        case 0xdf: if (!need(4)) return false; { const size_t n = get_be32(p); p += 4; return read_map(n); }
        default: why = "unsupported msgpack tag " + std::to_string(int(tag)); return false;
    }
}
}  // namespace

bool decode(const uint8_t* p, size_t n, Value& out, std::string& why) {
    why.clear();
    const uint8_t* at = p;
    out = Value{};
    if (!decode_at(at, p + n, out, why, 0)) return false;
    if (at != p + n) { why = "trailing bytes after the msgpack object"; return false; }
    return true;
}
}  // namespace msgpack

// ---- ZMTP 3.1 DEALER ------------------------------------------------------------------------------------------
#ifdef _WIN32
// The engine builds on Windows too; its cache tier is a Linux deployment (docs/LMCACHE.md), so the socket end is a
// refusal there rather than a second implementation of ZMTP over Winsock.
Dealer::~Dealer() {}
void Dealer::close() {}
bool Dealer::connect(const std::string&, std::string& why) {
    why = "the LMCache client is not available on Windows in this build";
    return false;
}
bool Dealer::send(const std::vector<std::string>&, std::string& why) { why = "not connected"; return false; }
bool Dealer::recv(std::vector<std::string>&, int, std::string& why) { why = "not connected"; return false; }
bool Dealer::handshake(std::string&) { return false; }
bool Dealer::write_all(const void*, size_t, std::string&) { return false; }
bool Dealer::read_exact(void*, size_t, int, std::string&) { return false; }
#else
Dealer::~Dealer() { close(); }

void Dealer::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

namespace {
// "tcp://host:port" -> host, port.  Also accepts "host:port" and a bare "host" (port 5555, the server's default).
bool split_endpoint(const std::string& endpoint, std::string& host, std::string& port, std::string& why) {
    std::string rest = endpoint;
    const std::string scheme = "tcp://";
    if (rest.compare(0, scheme.size(), scheme) == 0) rest = rest.substr(scheme.size());
    if (rest.empty()) { why = "empty endpoint"; return false; }
    const size_t colon = rest.rfind(':');
    if (colon == std::string::npos) { host = rest; port = "5555"; return true; }
    host = rest.substr(0, colon);
    port = rest.substr(colon + 1);
    return true;
}
}  // namespace

bool Dealer::connect(const std::string& endpoint, std::string& why) {
    close();
    std::string host, port;
    if (!split_endpoint(endpoint, host, port, why)) return false;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (const int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res); rc != 0) {
        why = "cannot resolve " + host + ": " + ::gai_strerror(rc);
        return false;
    }
    std::string last;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) { last = std::strerror(errno); continue; }
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            last = std::strerror(errno);
            ::close(fd);
            continue;
        }
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fd_ = fd;
        break;
    }
    ::freeaddrinfo(res);
    if (fd_ < 0) {
        why = "cannot connect to " + endpoint + ": " + (last.empty() ? "no address" : last);
        return false;
    }
    if (!handshake(why)) {
        close();
        return false;
    }
    return true;
}

bool Dealer::write_all(const void* p, size_t n, std::string& why) {
    const uint8_t* s = static_cast<const uint8_t*>(p);
    while (n) {
        const ssize_t w = ::send(fd_, s, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) { why = std::string("send: ") + std::strerror(errno); return false; }
        s += w;
        n -= size_t(w);
    }
    return true;
}

bool Dealer::read_exact(void* p, size_t n, int timeout_ms, std::string& why) {
    uint8_t* s = static_cast<uint8_t*>(p);
    while (n) {
        // one poll per read, with the caller's budget: a slow transfer of a large frame is still bounded by the
        // caller's own deadline, which the cache tier checks around the whole call
        if (timeout_ms >= 0) {
            pollfd pfd{fd_, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, timeout_ms);
            if (rc == 0) { why = "timed out waiting for the server"; return false; }
            if (rc < 0) { why = std::string("poll: ") + std::strerror(errno); return false; }
        }
        const ssize_t r = ::recv(fd_, s, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { why = r < 0 ? std::string("recv: ") + std::strerror(errno) : "the server closed the socket"; return false; }
        s += r;
        n -= size_t(r);
    }
    return true;
}

bool Dealer::handshake(std::string& why) {
    // greeting: signature(10) version(2) mechanism(20) as-server(1) filler(31)
    uint8_t hello[64] = {};
    hello[0] = 0xff;
    hello[9] = 0x7f;
    hello[10] = 3;   // ZMTP 3
    hello[11] = 1;
    std::memcpy(hello + 12, "NULL", 4);
    if (!write_all(hello, sizeof hello, why)) return false;

    uint8_t theirs[64] = {};
    if (!read_exact(theirs, sizeof theirs, 10000, why)) return false;
    if (theirs[0] != 0xff || theirs[9] != 0x7f) { why = "the peer is not ZMTP"; return false; }
    if (theirs[10] < 3) { why = "the peer speaks ZMTP " + std::to_string(int(theirs[10])) + ", need 3"; return false; }

    // READY: a command frame carrying Socket-Type=DEALER, which the ROUTER uses to route
    std::string body;
    body.push_back(5);
    body += "READY";
    const std::string socket_type = "DEALER";
    body.push_back(uint8_t(11));                 // property name length
    body += "Socket-Type";
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);
    body.push_back(uint8_t(socket_type.size()));  // property value length, big-endian u32
    body += socket_type;
    std::string frame;
    frame.push_back(char(kFlagCommand));
    frame.push_back(char(body.size()));
    frame += body;
    if (!write_all(frame.data(), frame.size(), why)) return false;

    // the peer's READY (a socket that rejects us sends ERROR instead)
    for (;;) {
        uint8_t head[9] = {};
        if (!read_exact(head, 1, 10000, why)) return false;
        if ((head[0] & kFlagCommand) == 0) { why = "expected a ZMTP command, got a message"; return false; }
        size_t size = 0;
        if (head[0] & kFlagLong) {
            if (!read_exact(head + 1, 8, 10000, why)) return false;
            size = size_t(get_be64(head + 1));
        } else {
            if (!read_exact(head + 1, 1, 10000, why)) return false;
            size = head[1];
        }
        std::string body_bytes(size, '\0');
        if (size && !read_exact(body_bytes.data(), size, 10000, why)) return false;
        if (body_bytes.size() < 6) { why = "short ZMTP command"; return false; }
        const std::string name = body_bytes.substr(1, size_t(uint8_t(body_bytes[0])));
        if (name == "READY") return true;
        if (name == "ERROR") { why = "the server refused the connection: " + body_bytes.substr(6); return false; }
        // PING and anything else are ignored before READY
    }
}

bool Dealer::send(const std::vector<std::string>& frames, std::string& why) {
    if (fd_ < 0) { why = "not connected"; return false; }
    std::vector<uint8_t> buf;
    for (size_t i = 0; i < frames.size(); ++i) {
        const size_t n = frames[i].size();
        uint8_t flags = i + 1 < frames.size() ? kFlagMore : 0;
        if (n > 255) {
            flags |= kFlagLong;
            buf.push_back(flags);
            put_be64(buf, n);
        } else {
            buf.push_back(flags);
            buf.push_back(uint8_t(n));
        }
        buf.insert(buf.end(), frames[i].begin(), frames[i].end());
    }
    return write_all(buf.data(), buf.size(), why);
}

bool Dealer::recv(std::vector<std::string>& frames, int timeout_ms, std::string& why) {
    frames.clear();
    if (fd_ < 0) { why = "not connected"; return false; }
    for (;;) {
        uint8_t flags = 0;
        if (!read_exact(&flags, 1, timeout_ms, why)) return false;
        size_t size = 0;
        if (flags & kFlagLong) {
            uint8_t len[8];
            if (!read_exact(len, 8, timeout_ms, why)) return false;
            size = size_t(get_be64(len));
        } else {
            uint8_t len = 0;
            if (!read_exact(&len, 1, timeout_ms, why)) return false;
            size = len;
        }
        std::string body(size, '\0');
        if (size && !read_exact(body.data(), size, timeout_ms, why)) return false;
        if (flags & kFlagCommand) {   // PING, or a late READY: not part of a reply
            if (body.size() >= 6 && body.compare(1, 5, "PING") == 0) {
                // answer the heartbeat so the server keeps the connection
                std::string pong;
                pong.push_back(char(kFlagCommand));
                pong.push_back(char(body.size()));
                pong += body;
                pong[7] = 'O';   // PING -> PONG
                std::string ignored;
                write_all(pong.data(), pong.size(), ignored);
            }
            continue;
        }
        frames.push_back(std::move(body));
        if (!(flags & kFlagMore)) return true;
    }
}
#endif  // _WIN32

// ---- RPC -------------------------------------------------------------------------------------------------------
std::string pack_tokens(const int32_t* ids, size_t n) {
    std::string out;
    out.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t v = uint32_t(ids[i]);
        out[i * 4 + 0] = char(v >> 24);
        out[i * 4 + 1] = char(v >> 16);
        out[i * 4 + 2] = char(v >> 8);
        out[i * 4 + 3] = char(v);
    }
    return out;
}

msgpack::Writer CacheKey::encode() const {
    msgpack::Writer w;
    w.map(10);
    w.str("model_name"); w.str(model_name);
    w.str("world_size"); w.integer(world_size);
    w.str("worker_id"); if (worker_id < 0) w.nil(); else w.integer(worker_id);
    w.str("token_bytes"); w.binary(token_bytes.data(), token_bytes.size());
    w.str("start"); w.integer(start);
    w.str("end"); w.integer(end);
    w.str("request_id"); w.str(request_id);
    w.str("cache_salt"); w.str(cache_salt);
    w.str("request_configs"); w.nil();
    w.str("num_kv_readers"); w.integer(num_kv_readers);
    return w;
}

msgpack::Writer ContextSpec::encode() const {
    msgpack::Writer w;
    w.map(10);
    w.str("instance_id"); w.integer(instance_id);
    w.str("model_name"); w.str(model_name);
    w.str("world_size"); w.integer(world_size);
    w.str("block_size"); w.integer(block_size);
    w.str("num_layers"); w.integer(num_layers);
    w.str("hidden_dim_size"); w.integer(hidden_dim_size);
    w.str("dtype_str"); w.str(dtype_str);
    w.str("use_mla"); w.boolean(use_mla);
    w.str("num_physical_slots"); w.integer(num_physical_slots);
    w.str("group_layouts"); w.array(0);
    return w;
}

void encode_envelope(msgpack::Writer& w, const TensorView* tiles, size_t n, int64_t layers, int64_t slots,
                     int64_t hidden) {
    w.map(2);
    w.str("version"); w.integer(1);
    w.str("tensors"); w.array(uint32_t(n));
    for (size_t i = 0; i < n; ++i) {
        w.map(3);
        w.str("dtype"); w.str("uint8");
        w.str("shape"); w.array(3); w.integer(layers); w.integer(slots); w.integer(hidden);
        w.str("data"); w.binary(tiles[i].data, tiles[i].bytes);
    }
}

bool decode_envelope(const std::string& payload, std::string& out, std::string& why) {
    msgpack::Value root;
    if (!msgpack::decode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), root, why)) return false;
    const msgpack::Value* tensors = root.find("tensors");
    if (!tensors || tensors->kind != msgpack::Value::Kind::array) { why = "no tensors in the reply"; return false; }
    out.clear();
    for (const auto& t : tensors->arr) {
        const msgpack::Value* data = t.find("data");
        if (!data) { why = "a tensor has no data"; return false; }
        out += data->s;
    }
    return true;
}

bool Rpc::call(Op op, const std::vector<std::string>& payloads, msgpack::Value& reply, std::string& why,
               int timeout_ms) {
    const int64_t uid = ++uid_;
    // one msgpack object per frame: [uid][op][payload...], which is what the server asserts on
    msgpack::Writer id_bytes;
    id_bytes.integer(uid);
    msgpack::Writer op_bytes;
    op_bytes.integer(int64_t(op));
    std::vector<std::string> frames;
    frames.reserve(payloads.size() + 2);
    frames.emplace_back(reinterpret_cast<const char*>(id_bytes.bytes().data()), id_bytes.bytes().size());
    frames.emplace_back(reinterpret_cast<const char*>(op_bytes.bytes().data()), op_bytes.bytes().size());
    frames.insert(frames.end(), payloads.begin(), payloads.end());
    if (!dealer_.send(frames, why)) return false;

    std::vector<std::string> got;
    if (!dealer_.recv(got, timeout_ms < 0 ? timeout_ms_ : timeout_ms, why)) return false;
    if (got.size() < 2) { why = "the reply has " + std::to_string(got.size()) + " frames"; return false; }
    msgpack::Value id;
    if (!msgpack::decode(reinterpret_cast<const uint8_t*>(got[0].data()), got[0].size(), id, why)) return false;
    int64_t reply_uid = -1;
    if (!id.as_int(reply_uid) || reply_uid != uid) why = "the reply is for another request";

    // a reply is [uid][op][payload] (the server echoes the operation); a failure carries the reserved operation and
    // its reason in the next frame.  The payload is the last frame either way.
    for (size_t i = 1; i + 1 < got.size(); ++i) {
        msgpack::Value name;
        std::string ignored;
        if (!msgpack::decode(reinterpret_cast<const uint8_t*>(got[i].data()), got[i].size(), name, ignored)) continue;
        if (name.kind != msgpack::Value::Kind::str || name.s != "__lmcache_rpc_error__") continue;
        msgpack::Value info;
        std::string detail = got[i + 1];
        if (msgpack::decode(reinterpret_cast<const uint8_t*>(got[i + 1].data()), got[i + 1].size(), info, ignored)) {
            if (const msgpack::Value* m = info.find("message")) detail = m->s;
            if (const msgpack::Value* c = info.find("code")) detail = c->s + ": " + detail;
        }
        why = "the server refused op " + std::to_string(int64_t(op)) + ": " + detail;
        return false;
    }
    const std::string& payload = got.back();
    if (!msgpack::decode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), reply, why)) return false;
    return true;
}
}  // namespace strata::core::lmc
