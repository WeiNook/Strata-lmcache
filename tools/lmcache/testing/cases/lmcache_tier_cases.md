# Shared cache - tier cases (C++)

Generated from `engine/src/core/lmcache_tier_test.cpp` by `tools/lmcache/testing/list_cases.py`; edit the test
file, not this one.  Run them with:

```
cmake --preset cpu-check && cmake --build build/cpu-check -j8 --target lmcache_tier_test
build/cpu-check/lmcache_tier_test                          # offline: the in-memory store
build/cpu-check/lmcache_tier_test tcp://127.0.0.1:5555      # and the whole thing again, against a real store
```

26 cases, 198 assertion sites (15 of them in the `live` section below).  The binary
reports the assertions it executed per run; the count here is the call sites in the source (a loop runs its
site once per iteration).

| case | what it is for | asserts |
|---|---|---|
| `settings_surface` | One setting would be a hidden feature: this is the whole configuration surface, so it is pinned here. | 17 |
| `info_lines` | The one INFO line the engine prints at start-up and per transfer: an operator reads the tier's whole configuration and effect there, so its shape is pinned. | 17 |
| `publish_restore_roundtrip` | The base promise: a conversation published to the store comes back as the same state, K/V included. | 8 |
| `only_new_windows_are_sent` | A conversation only grows, so the windows an earlier publish wrote are still in the store: sending them again would move the whole state on every turn.  This is the whole point of cutting it up. | 9 |
| `cost_gate_refuses_short_prefixes` | A prefix too short to pay for its own transfer is not published: below the gate nothing is written at all. | 4 |
| `restore_gate_refuses_short_prefixes` | The restore side takes the same gate the publish side has.  Without it every request - a two-line question included - probes the store, and a store that accepts a lookup without answering holds the prefill for the whole lookup timeout before anything is computed (30 s, measured on the deployment this test was written for). | 7 |
| `restore_gate_boundary` | The gate is a floor, not a fudge: one token below it the store is never asked, one token at it is. | 4 |
| `prefix_is_cut_at_chunk_boundaries` | The stored prefix floors to a whole chunk, and what comes back is exactly that prefix - the tail that never filled a window is recomputed, not invented. | 8 |
| `a_store_failure_is_never_fatal` | A refusing or unreachable store only costs the reuse: publish reports it, restore leaves the caller's image as it was, and the request goes on. | 6 |
| `a_miss_is_not_an_error` | Nothing stored is a miss (a probe, no hit, no failure) - the shape the engine's counters depend on. | 3 |
| `another_identity_is_refused` | A session saved under another model or engine configuration is refused before any of its payload is parsed. | 4 |
| `disabled_tier_does_nothing` | No STRATA_LMCACHE: publish and restore are no-ops and INFO says the tier is off - a model that never configured it behaves exactly as before this feature. | 4 |
| `users_interleave_on_one_engine` | A real instance serves several clients at once: an agent's main conversation, a subagent's, a side question, another person's chat.  Their publishes interleave turn by turn, and every turn must read back its OWN state - a shared tier that mixed two conversations up would be worse than no tier at all. | 8 |
| `two_instances_serve_their_own_users` | The deployment runs four engines against one store, and a turn does not come back to the engine that computed it: that is the whole point.  Every publish lands on one instance and every read is made on the other. | 4 |
| `a_picture_is_never_served_for_another_picture` | A picture is a run of placeholder tokens: two users who send the same words with different screenshots produce the SAME token ids and DIFFERENT K/V.  One key cannot hold both (the store keeps the first object it was given), so the second conversation's state is simply not stored - and every reader must then refuse it.  That refusal is the caller's check (conversation_prefix: same pictures, same steering), which this exercises end to end. | 9 |
| `a_picture_inside_the_prefix_gates_the_match` | The stored prefix cuts at a whole chunk, so a picture before that cut is part of the state and one after it is not: the caller's own check (conversation_prefix, which this exercises) must follow the same line. | 7 |
| `a_long_file_prefix_is_shared_by_two_questions` | An agent pastes the same long file twice with two different questions.  The file's windows are the same objects, so the second question must restore them instead of reading the file again - and never send them a second time. | 8 |
| `the_image_form_the_engine_publishes` | The form the writer really publishes: the K/V already inside the image, cut by chunk_extract. | 8 |
| `a_missing_window_refuses_the_whole_chain` | One window lost (the store evicted it, or the publish was cut short) makes the chain unusable: the state's running arrays belong to the whole conversation, so a shorter prefix assembled from the rest would be wrong. | 5 |
| `a_corrupt_object_in_the_store_is_refused` | What the store hands back is never trusted: every record carries its own hash and the session format's checks.  A torn write, a reused key from another tool, a truncated object - all of them end as "not restored", never as a state the engine would continue from. | 7 |
| `a_store_that_comes_back_is_used_again` | The store was down and comes back: the next call reconnects instead of staying broken for the process's life. | 5 |
| `a_rewound_conversation_is_published_again` | A client that rewinds history (an edited turn, a compacted conversation) asks for a SHORTER prefix than the last publish: the windows the store holds beyond it are not this conversation's any more, so they are written again. | 5 |
| `a_layer_split_state_is_not_published` | A layer split (several cards, one image per stage) cannot be cut into windows: the shared tier stays out of it, and says so instead of writing something a reader could not use. | 3 |
| `a_read_the_ram_preflight_refuses_leaves_the_caller_alone` | The engine refuses a read its RAM preflight cannot afford before anything is parsed; the tier must pass that through without touching the caller's image. | 4 |
| `the_longer_of_the_ram_tier_and_the_store_wins` | One instance keeps a conversation in its own RAM tier and in the store at the same time.  The rule is "the longer prefix wins" (generate.cpp: the store is only used when it beats `resume`, the batch slot and the parked conversation), and a turn the RAM tier wins must not pay for the store's state: the head alone says how long it is. | 13 |
| `a_state_the_caller_already_beats_is_not_transferred` | The same rule, at the tier's own boundary: a state no longer than the caller's prefix is refused after its head. | 6 |

## `live(endpoint)` - the same shapes against a real LMCache store

Only run when an endpoint is given (15 assertions).  It publishes and reads back:

1. a 1024-token session, and the same state from a second engine on the same store;
2. 16 concurrent reads from two engines (the store's per-session prefetch state, the shared `request_id` bug);
3. a 9216-token session at the production 8192 cost gate, and an 8000-token one below it;
4. **the image form the engine's writer publishes** (the K/V inside the image, `chunk_extract`), both layers;
5. two users with the same text and different pictures, read by a third engine;
6. three users over two engines, interleaved, two turns each - a turn published on one engine read on the other;
7. a conversation nobody published (a miss, never another user's state).
