// Cutting a parked conversation into token windows and putting it back together, with no GPU, no model and no store:
// what matters is that the bytes of token t land at the same offset they came from, for every layer and every buffer.

#include "strata/core/lmcache_chunk.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace strata::core;

int checks = 0, failures = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what);
    }
}

// A byte that depends on the layer, the buffer and the absolute offset, so a slice taken at the wrong offset cannot match.
uint8_t pattern(int layer, int buffer, size_t at) {
    return uint8_t((at * 7 + size_t(layer) * 31 + size_t(buffer) * 53) & 0xff);
}

void fill(ConversationBuffer& b, int layer, int buffer) {
    for (size_t at = 0; at < b.size();) {
        const size_t chunk = std::min<size_t>(4096, b.size() - at);
        std::vector<uint8_t> p(chunk);
        for (size_t i = 0; i < chunk; ++i) p[i] = pattern(layer, buffer, at + i);
        b.visit(at, chunk, [&](uint8_t* q, size_t n, size_t) {
            std::memcpy(q, p.data(), n);
            return true;
        });
        at += chunk;
    }
}

bool same(const ConversationBuffer& a, const ConversationBuffer& b) {
    if (a.size() != b.size()) return false;
    std::vector<uint8_t> x(a.size()), y(b.size());
    if (a.size() && (!a.read(x.data(), 0, a.size()) || !b.read(y.data(), 0, b.size()))) return false;
    return x == y;
}

// The shapes conversation_snapshot.cpp's `layout` produces for 8-bit K/V: 2 heads of 64, one scale per 64, a 4-cell page
// and a 4-cell indexer block.
SavedConversation make_image(int64_t tokens, int layers = 2) {
    SavedConversation image;
    image.geometry = {tokens, layers, 256, 4096, 1, 2, 64, 128, 4};
    image.layer_lo = 0;
    image.layer_hi = layers;
    image.cvec = true;
    image.live.ids.resize(size_t(tokens));
    for (int64_t t = 0; t < tokens; ++t) image.live.ids[size_t(t)] = int32_t(1000 + t);
    image.kv.resize(size_t(layers));
    for (int l = 0; l < layers; ++l) {
        ConversationKv& k = image.kv[size_t(l)];
        k.format = 1;   // int8
        k.cells = tokens;
        k.heads = 2;
        k.head_dim = 64;
        k.page_size = 4;
        k.pooled_rows = tokens / 4 + 1;   // whole blocks + the spare row
        k.idx_dim = 128;
        k.k.resize(size_t(tokens) * 128);
        k.v.resize(size_t(tokens) * 128);
        k.k_scale.resize(size_t(tokens) * 4);
        k.v_scale.resize(size_t(tokens) * 4);
        k.pooled.resize(size_t(k.pooled_rows) * 128 * sizeof(float));
        fill(k.k, l, 0);
        fill(k.v, l, 1);
        fill(k.k_scale, l, 2);
        fill(k.v_scale, l, 3);
        fill(k.pooled, l, 4);
    }
    return image;
}

bool same_ids(const std::vector<int32_t>& a, const std::vector<int32_t>& b) { return a == b; }

void round_trip() {
    const int64_t tokens = 768, chunk = 256;
    const SavedConversation image = make_image(tokens);
    check(chunk_granularity(image) == 4, "the granularity is the page and block (4 cells)");

    std::string why;
    check(chunk_size_ok(image, chunk, image.live.ids, why), "a 256-token chunk fits this state");
    check(!chunk_size_ok(image, 250, image.live.ids, why), "a chunk off the page boundary is refused");
    check(!chunk_size_ok(image, 1024, image.live.ids, why), "a chunk wider than the state is refused");
    check(chunk_windows(image.live.ids, chunk) == 3, "the state holds three whole chunks");

    std::vector<SavedConversation> chunks;
    for (int64_t i = 0; i < 3; ++i) {
        SavedConversation piece;
        const ChunkWindow w{i * chunk, chunk};
        check(chunk_extract(image, w, piece, why), "a window comes out of the image");
        check(piece.live.ids.size() == size_t(chunk), "the window carries its own tokens");
        check(piece.live.ids[0] == image.live.ids[size_t(i * chunk)], "the window starts at its own token");
        check(piece.kv.size() == image.kv.size(), "the window carries every layer");
        check(piece.kv[0].cells == chunk, "the window holds one chunk of cells");
        check(piece.kv[0].pooled_rows == chunk / 4 + 1, "the window carries the spare pooled row");
        check(piece.kv[0].k.size() == size_t(chunk) * 128, "the window's K is one chunk long");
        chunks.push_back(std::move(piece));
    }

    SavedConversation back;
    check(chunk_reserve(back, chunks[0], tokens, why), "a state of the full length is reserved");
    check(back.kv[0].pooled_rows == tokens / 4 + 1, "the reserved state carries one spare row, not one per chunk");
    for (int64_t i = 0; i < 3; ++i)
        check(chunk_merge(back, chunks[size_t(i)], ChunkWindow{i * chunk, chunk}, why), "a window merges back");
    check(same_ids(back.live.ids, image.live.ids), "the tokens come back in order");
    bool bytes = true;
    for (size_t l = 0; l < image.kv.size(); ++l) {
        bytes = bytes && same(back.kv[l].k, image.kv[l].k) && same(back.kv[l].v, image.kv[l].v) &&
                same(back.kv[l].k_scale, image.kv[l].k_scale) && same(back.kv[l].v_scale, image.kv[l].v_scale) &&
                same(back.kv[l].pooled, image.kv[l].pooled);
    }
    check(bytes, "every layer's five buffers come back byte for byte");
    check(back.live.ids.size() == size_t(tokens), "the state holds as many tokens as it did");
}

void a_middle_window() {
    // Publishing only what is new means cutting at an offset that is not zero: the pooled rows must be taken from the
    // block that covers the first token of the window, not from row 0.
    const SavedConversation image = make_image(1024);
    std::string why;
    SavedConversation piece;
    check(chunk_extract(image, ChunkWindow{512, 256}, piece, why), "a middle window comes out of the image");
    check(piece.live.ids[0] == image.live.ids[512], "the middle window starts where it should");
    std::vector<uint8_t> a(128), b(128);
    check(piece.kv[0].k.read(a.data(), 0, 128) && image.kv[0].k.read(b.data(), size_t(512) * 128, 128),
          "the window's first K byte is the image's byte at its own token");
    check(a == b, "a middle window's K starts at the right offset");
    check(piece.kv[0].k.read(a.data(), 100, 128) && image.kv[0].k.read(b.data(), size_t(512) * 128 + 100, 128),
          "a middle window's K stays aligned inside the buffer");
    check(a == b, "a middle window's K bytes line up");
    std::vector<uint8_t> pa(512), pb(512);
    check(piece.kv[0].pooled.read(pa.data(), 0, 512) &&
              image.kv[0].pooled.read(pb.data(), size_t(512 / 4) * 512, 512),
          "a middle window's pooled row is the block at its own token");
    check(pa == pb, "a middle window's pooled row lines up");
}

void refusals() {
    const SavedConversation image = make_image(768);
    std::string why;
    SavedConversation piece;
    check(!chunk_extract(image, ChunkWindow{3, 256}, piece, why), "a window off the page boundary is refused");
    check(!chunk_extract(image, ChunkWindow{0, 768 + 4}, piece, why), "a window past the state is refused");
    check(!chunk_extract(image, ChunkWindow{0, 0}, piece, why), "an empty window is refused");
    check(!why.empty(), "a refusal carries a reason");

    SavedConversation no_kv;
    no_kv.live.ids.assign(1024, 0);
    check(!chunk_extract(no_kv, ChunkWindow{0, 256}, piece, why), "a state without K/V cannot be cut");

    SavedConversation split = image;
    split.stage_images.resize(1);
    check(!chunk_extract(split, ChunkWindow{0, 256}, piece, why), "a layer-split state cannot be cut");

    SavedConversation odd = make_image(768);
    odd.kv[1].cells = 512;   // layers that disagree
    check(!chunk_extract(odd, ChunkWindow{0, 256}, piece, why), "layers that disagree on their shape are refused");

    SavedConversation short_ids = make_image(768);
    short_ids.live.ids.resize(100);
    check(!chunk_extract(short_ids, ChunkWindow{0, 256}, piece, why), "a window past the tokens is refused");
}

void shapes_the_code_does_not_know() {
    // A pooled buffer that is not whole rows means the layout is not the one this code slices: refuse, so the caller can
    // fall back to publishing the state whole instead of writing bytes nothing can restore.
    SavedConversation image = make_image(768);
    image.kv[0].pooled.resize(image.kv[0].pooled.size() - 3);
    std::string why;
    SavedConversation piece;
    check(!chunk_extract(image, ChunkWindow{0, 256}, piece, why), "a pooled buffer off its row size is refused");
    check(why.find("row") != std::string::npos, "the reason names the pooled rows");
}

}  // namespace

int main() {
    round_trip();
    a_middle_window();
    refusals();
    shapes_the_code_does_not_know();
    std::printf("lmcache_chunk_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
