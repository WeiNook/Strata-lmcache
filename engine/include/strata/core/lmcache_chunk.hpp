#pragma once

// A parked conversation, cut into token windows that each stand on their own.
//
// Why: the store is keyed per 256-token chunk, but a parked conversation is one image whose buffers are laid out
// [token][per-token bytes] (that is what `conversation_kv_save` writes: a prefix of N tokens is a prefix of N*per bytes,
// see its `keep`).  One image therefore cannot be published piece by piece - growing the conversation changes every
// byte's offset.  Cutting the image into windows first makes each window self-contained: adding 2 K tokens to a 200 K
// conversation then writes 8 new objects instead of all of them, and a chain that lost its tail still restores the
// prefix it kept.
//
// The cut is legal only on a boundary the KV itself respects: one page (`page_size`, in cells) and one indexer block
// (the pooled buffer holds one row per block).  Both are read from the image, never assumed; in this engine they are 4,
// so a 256-token window is 64 whole pages and 64 whole blocks.

#include "strata/core/conversation_buffer.hpp"
#include "strata/core/conversation_cache.hpp"

#include <cstdint>
#include <string>

namespace strata::core {

//: Tokens [first, first + tokens) of a conversation.
struct ChunkWindow {
    int64_t first = 0;
    int64_t tokens = 0;
};

//: The smallest window the image can be cut at: the widest page and indexer block across its layers (1 when unknown).
int64_t chunk_granularity(const SavedConversation& image);

//: Whether `chunk` tokens is a usable window size for `image`: a multiple of the granularity, at least one window, and
//: no wider than what the conversation holds.  `why` says which rule failed.
bool chunk_size_ok(const SavedConversation& image, int64_t chunk, const std::vector<int32_t>& ids, std::string& why);

//: How many whole windows of `chunk` tokens `ids` holds (the tail that does not fill one is not published).
int64_t chunk_windows(const std::vector<int32_t>& ids, int64_t chunk);

//: The image of one window: same geometry, same layers, `ids` and every buffer cut to [first, first + tokens).
//: Serializing it with `session_stream_write` gives one object that a store can hold - and lose - on its own.
bool chunk_extract(const SavedConversation& image, const ChunkWindow& window, SavedConversation& out, std::string& why);

//: An empty image shaped like `chunk` but `tokens` long, ready for `chunk_merge` (buffers allocated, ids sized).
bool chunk_reserve(SavedConversation& out, const SavedConversation& chunk, int64_t tokens, std::string& why);

//: Writes one window's bytes back into `out` at the offsets they came from, and its ids into `out.live.ids`.
bool chunk_merge(SavedConversation& out, const SavedConversation& chunk, const ChunkWindow& window, std::string& why);

}  // namespace strata::core
