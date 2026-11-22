#include "strata/core/lmcache_chunk.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <numeric>

namespace strata::core {
namespace {

// k, v, k_scale, v_scale, pooled - the order ConversationKv and the session format use.
constexpr size_t kBuffers = 5;
constexpr size_t kTokenMajor = 4;

// What one layer's buffers look like, in the terms the snapshot itself uses (conversation_snapshot.cpp's `layout`):
//   cells   tokens the buffers hold (page-aligned)
//   per     bytes per token, per buffer (the scales are smaller than K and V: head_dim/64 * 2)
//   row     bytes of one pooled row (one indexer block)
//   rows    pooled rows held: whole blocks + the one spare row the writer always carries
struct Shape {
    int64_t cells = 0, page = 0, block = 0;
    std::array<size_t, kTokenMajor> per{};
    size_t row = 0;
    int64_t rows = 0;
};

bool same_shape(const Shape& a, const Shape& b) {
    return a.cells == b.cells && a.page == b.page && a.block == b.block && a.per == b.per && a.row == b.row &&
           a.rows == b.rows;
}

bool copy_range(ConversationBuffer& dst, size_t dst_at, const ConversationBuffer& src, size_t src_at, size_t n) {
    if (n == 0) return true;
    if (src_at > src.size() || n > src.size() - src_at) return false;
    if (dst_at > dst.size() || n > dst.size() - dst_at) return false;
    return src.visit(src_at, n, [&](const uint8_t* p, size_t len, size_t at) {
        return dst.visit(dst_at + (at - src_at), len, [&](uint8_t* q, size_t m, size_t) {
            std::memcpy(q, p, m);
            return true;
        });
    });
}

bool shape_of_sizes(int64_t cells, int64_t page_size, int64_t idx_dim, const std::array<size_t, kBuffers>& bytes_of,
                    Shape& s, std::string& why) {
    if (cells <= 0) { why = "a layer holds no tokens"; return false; }
    if (page_size <= 0) { why = "a layer reports no page size"; return false; }
    if (idx_dim <= 0) { why = "a layer reports no indexer width"; return false; }
    s.cells = cells;
    s.page = page_size;
    bool any = false;
    for (size_t i = 0; i < kTokenMajor; ++i) {
        const size_t bytes = bytes_of[i];
        if (bytes % size_t(cells)) { why = "a K/V buffer is not a whole number of bytes per token"; return false; }
        s.per[i] = bytes / size_t(cells);
        any = any || s.per[i] != 0;
    }
    if (!any) { why = "a layer holds no K/V bytes"; return false; }
    s.row = size_t(idx_dim) * sizeof(float);
    if (bytes_of[4] % s.row) { why = "the pooled buffer is not a whole number of rows"; return false; }
    s.rows = int64_t(bytes_of[4] / s.row);
    if (s.rows < 2) { why = "the pooled buffer holds no complete block"; return false; }
    // `pooled_rows = upto / idx_block + 1`, so the block is what the spare row leaves over.  Anything that does not come
    // out whole means this image's layout is not the one this code knows: refuse rather than cut it in the wrong place.
    s.block = cells / (s.rows - 1);
    if (s.block <= 0 || s.block * (s.rows - 1) > cells || cells - s.block * (s.rows - 1) > s.block) {
        why = "the pooled rows do not divide the token count";
        return false;
    }
    return true;
}

bool shape_of(const ConversationKv& k, Shape& s, std::string& why) {
    const std::array<size_t, kBuffers> bytes_of = {k.k.size(), k.v.size(), k.k_scale.size(), k.v_scale.size(),
                                                   k.pooled.size()};
    return shape_of_sizes(k.cells, k.page_size, k.idx_dim, bytes_of, s, why);
}

bool shape_of(const SessionKvSource& k, Shape& s, std::string& why) {
    return shape_of_sizes(k.cells, k.page_size, k.idx_dim, k.sizes, s, why);
}

bool shapes_of(const SavedConversation& image, Shape& s, std::string& why) {
    if (image.kv.empty()) { why = "the state holds no K/V layers"; return false; }
    if (!image.stage_images.empty()) { why = "a layer-split state cannot be cut into chunks"; return false; }
    Shape first;
    if (!shape_of(image.kv.front(), first, why)) return false;
    for (size_t i = 1; i < image.kv.size(); ++i) {
        Shape other;
        if (!shape_of(image.kv[i], other, why)) return false;
        if (!same_shape(first, other)) { why = "the K/V layers disagree on their shape"; return false; }
    }
    s = first;
    return true;
}

bool shapes_of(const std::vector<SessionKvSource>& kv, Shape& s, std::string& why) {
    if (kv.empty()) { why = "the state holds no K/V sources"; return false; }
    Shape first;
    if (!shape_of(kv.front(), first, why)) return false;
    for (size_t i = 1; i < kv.size(); ++i) {
        Shape other;
        if (!shape_of(kv[i], other, why)) return false;
        if (!same_shape(first, other)) { why = "the K/V sources disagree on their shape"; return false; }
    }
    s = first;
    return true;
}

// The chunk-size rules, once: both forms of a state answer the same way.
bool size_ok(const Shape& s, int64_t chunk, const std::vector<int32_t>& ids, std::string& why) {
    const int64_t step = std::lcm(s.page, s.block);
    if (chunk <= 0) { why = "the chunk size is not positive"; return false; }
    if (chunk % step) {
        why = "a chunk of " + std::to_string(chunk) + " tokens is not a multiple of this state's " +
              std::to_string(step) + " token page/block";
        return false;
    }
    if (int64_t(ids.size()) < chunk) {
        why = "the prefix (" + std::to_string(ids.size()) + " tokens) is shorter than one chunk";
        return false;
    }
    if (int64_t(ids.size()) > s.cells) {
        why = "the prefix (" + std::to_string(ids.size()) + " tokens) is longer than the state (" +
              std::to_string(s.cells) + " tokens)";
        return false;
    }
    return true;
}

bool window_ok(const Shape& s, const ChunkWindow& w, std::string& why) {
    const int64_t step = std::lcm(s.page, s.block);
    if (w.tokens <= 0) { why = "the window is empty"; return false; }
    if (w.first < 0 || w.first % step || w.tokens % step) {
        why = "the window does not start and end on a " + std::to_string(step) + " token boundary";
        return false;
    }
    if (w.first + w.tokens > s.cells) { why = "the window runs past the state"; return false; }
    return true;
}

}  // namespace

int64_t chunk_granularity(const SavedConversation& image) {
    std::string why;
    Shape s;
    if (!shapes_of(image, s, why)) return 1;
    return std::lcm(s.page, s.block);
}

bool chunk_size_ok(const SavedConversation& image, int64_t chunk, const std::vector<int32_t>& ids, std::string& why) {
    Shape s;
    if (!shapes_of(image, s, why)) return false;
    return size_ok(s, chunk, ids, why);
}

bool chunk_size_ok_from(const std::vector<SessionKvSource>& kv, int64_t chunk, const std::vector<int32_t>& ids,
                        std::string& why) {
    Shape s;
    if (!shapes_of(kv, s, why)) return false;
    return size_ok(s, chunk, ids, why);
}

int64_t chunk_windows(const std::vector<int32_t>& ids, int64_t chunk) {
    if (chunk <= 0) return 0;
    return int64_t(ids.size()) / chunk;
}

bool chunk_extract(const SavedConversation& image, const ChunkWindow& window, SavedConversation& out,
                   std::string& why) {
    Shape s;
    if (!shapes_of(image, s, why)) return false;
    if (!window_ok(s, window, why)) return false;
    if (int64_t(image.live.ids.size()) < window.first + window.tokens) {
        why = "the window runs past the conversation's tokens";
        return false;
    }
    out = SavedConversation{};
    out.geometry = image.geometry;
    out.layer_lo = image.layer_lo;
    out.layer_hi = image.layer_hi;
    out.cvec = image.cvec;
    // The window's own tokens: what a reader needs to know what it is looking at, and what the file's limits apply to.
    out.live.ids.assign(image.live.ids.begin() + window.first, image.live.ids.begin() + window.first + window.tokens);
    out.kv.resize(image.kv.size());
    const int64_t first_row = window.first / s.block;
    const int64_t rows = window.tokens / s.block + 1;   // whole blocks + the spare row that follows the window
    for (size_t i = 0; i < image.kv.size(); ++i) {
        const ConversationKv& src = image.kv[i];
        ConversationKv& dst = out.kv[i];
        dst.format = src.format;
        dst.cells = window.tokens;
        dst.heads = src.heads;
        dst.head_dim = src.head_dim;
        dst.page_size = src.page_size;
        dst.pooled_rows = rows;
        dst.idx_dim = src.idx_dim;
        const std::array<const ConversationBuffer*, kBuffers> sb = {&src.k, &src.v, &src.k_scale, &src.v_scale,
                                                                    &src.pooled};
        const std::array<ConversationBuffer*, kBuffers> db = {&dst.k, &dst.v, &dst.k_scale, &dst.v_scale, &dst.pooled};
        for (size_t j = 0; j < kTokenMajor; ++j) {
            const size_t bytes = size_t(window.tokens) * s.per[j];
            db[j]->resize(bytes);
            if (!copy_range(*db[j], 0, *sb[j], size_t(window.first) * s.per[j], bytes)) {
                why = "a K/V buffer could not be copied";
                return false;
            }
        }
        if (first_row + rows > s.rows) { why = "the pooled buffer is shorter than the window"; return false; }
        db[4]->resize(size_t(rows) * s.row);
        if (!copy_range(*db[4], 0, *sb[4], size_t(first_row) * s.row, size_t(rows) * s.row)) {
            why = "the pooled buffer could not be copied";
            return false;
        }
    }
    return true;
}

bool chunk_extract_from(const SavedConversation& image, const std::vector<SessionKvSource>& kv, const ChunkWindow& window,
                        SavedConversation& out, std::string& why) {
    if (!image.stage_images.empty()) { why = "a layer-split state cannot be cut into chunks"; return false; }
    Shape s;
    if (!shapes_of(kv, s, why)) return false;
    if (!window_ok(s, window, why)) return false;
    if (int64_t(image.live.ids.size()) < window.first + window.tokens) {
        why = "the window runs past the conversation's tokens";
        return false;
    }
    out = SavedConversation{};
    out.geometry = image.geometry;
    out.layer_lo = image.layer_lo;
    out.layer_hi = image.layer_hi;
    out.cvec = image.cvec;
    out.live.ids.assign(image.live.ids.begin() + window.first, image.live.ids.begin() + window.first + window.tokens);
    out.kv.resize(kv.size());
    const int64_t first_row = window.first / s.block;
    const int64_t rows = window.tokens / s.block + 1;
    for (size_t i = 0; i < kv.size(); ++i) {
        const SessionKvSource& src = kv[i];
        ConversationKv& dst = out.kv[i];
        dst.format = src.format;
        dst.cells = window.tokens;
        dst.heads = src.heads;
        dst.head_dim = src.head_dim;
        dst.page_size = src.page_size;
        dst.pooled_rows = rows;
        dst.idx_dim = src.idx_dim;
        const std::array<ConversationBuffer*, kBuffers> db = {&dst.k, &dst.v, &dst.k_scale, &dst.v_scale, &dst.pooled};
        for (size_t j = 0; j < kTokenMajor; ++j) {
            const size_t bytes = size_t(window.tokens) * s.per[j];
            db[j]->resize(bytes);
            const size_t from = size_t(window.first) * s.per[j];
            if (!db[j]->visit(0, bytes, [&](uint8_t* q, size_t len, size_t at) {
                    return src.read(j, from + at, q, len);
                })) {
                why = "a K/V source could not be read";
                return false;
            }
        }
        if (first_row + rows > s.rows) { why = "the pooled rows are shorter than the window"; return false; }
        db[4]->resize(size_t(rows) * s.row);
        const size_t from = size_t(first_row) * s.row;
        if (!db[4]->visit(0, size_t(rows) * s.row, [&](uint8_t* q, size_t len, size_t at) {
                return src.read(4, from + at, q, len);
            })) {
            why = "the pooled source could not be read";
            return false;
        }
    }
    return true;
}

bool chunk_reserve(SavedConversation& out, const SavedConversation& chunk, int64_t tokens, std::string& why) {
    Shape s;
    if (!shapes_of(chunk, s, why)) return false;
    const int64_t step = std::lcm(s.page, s.block);
    if (tokens <= 0 || tokens % step) {
        why = "the whole state is not a multiple of its " + std::to_string(step) + " token page/block";
        return false;
    }
    out = SavedConversation{};
    out.geometry = chunk.geometry;
    out.layer_lo = chunk.layer_lo;
    out.layer_hi = chunk.layer_hi;
    out.cvec = chunk.cvec;
    out.live.ids.assign(size_t(tokens), 0);
    out.kv.resize(chunk.kv.size());
    for (size_t i = 0; i < chunk.kv.size(); ++i) {
        const ConversationKv& src = chunk.kv[i];
        ConversationKv& dst = out.kv[i];
        dst.format = src.format;
        dst.cells = tokens;
        dst.heads = src.heads;
        dst.head_dim = src.head_dim;
        dst.page_size = src.page_size;
        dst.pooled_rows = tokens / s.block + 1;
        dst.idx_dim = src.idx_dim;
        const std::array<ConversationBuffer*, kBuffers> db = {&dst.k, &dst.v, &dst.k_scale, &dst.v_scale, &dst.pooled};
        for (size_t j = 0; j < kTokenMajor; ++j) db[j]->resize(size_t(tokens) * s.per[j]);
        db[4]->resize(size_t(tokens / s.block + 1) * s.row);
    }
    return true;
}

bool chunk_merge(SavedConversation& out, const SavedConversation& chunk, const ChunkWindow& window, std::string& why) {
    Shape s;
    if (!shapes_of(out, s, why)) return false;
    Shape c;
    if (!shapes_of(chunk, c, why)) return false;
    if (!window_ok(s, window, why)) return false;
    if (c.cells != window.tokens) { why = "the chunk's tokens do not match the window"; return false; }
    if (c.per != s.per || c.block != s.block || c.row != s.row || out.kv.size() != chunk.kv.size()) {
        why = "the chunk's shape does not match this state";
        return false;
    }
    const int64_t first_row = window.first / s.block;
    const int64_t rows = window.tokens / s.block + 1;
    for (size_t i = 0; i < out.kv.size(); ++i) {
        const ConversationKv& src = chunk.kv[i];
        ConversationKv& dst = out.kv[i];
        if (src.format != dst.format || src.heads != dst.heads || src.head_dim != dst.head_dim ||
            src.page_size != dst.page_size || src.idx_dim != dst.idx_dim) {
            why = "the chunk's layer headers do not match this state";
            return false;
        }
        const std::array<const ConversationBuffer*, kBuffers> sb = {&src.k, &src.v, &src.k_scale, &src.v_scale,
                                                                    &src.pooled};
        const std::array<ConversationBuffer*, kBuffers> db = {&dst.k, &dst.v, &dst.k_scale, &dst.v_scale, &dst.pooled};
        for (size_t j = 0; j < kTokenMajor; ++j) {
            if (!copy_range(*db[j], size_t(window.first) * s.per[j], *sb[j], 0, size_t(window.tokens) * s.per[j])) {
                why = "a K/V buffer could not be merged";
                return false;
            }
        }
        if (!copy_range(*db[4], size_t(first_row) * s.row, *sb[4], 0, size_t(rows) * s.row)) {
            why = "the pooled buffer could not be merged";
            return false;
        }
    }
    if (int64_t(out.live.ids.size()) >= window.first + window.tokens) {
        std::copy(chunk.live.ids.begin(), chunk.live.ids.begin() + window.tokens, out.live.ids.begin() + window.first);
    }
    return true;
}

}  // namespace strata::core
