# Shared cache - how it is tested

Two things live here, and they are kept apart on purpose:

```
testing/
  cases/          what is tested - generated from the test sources, never edited by hand
  results/        what a run produced - dated, one file per run plus its raw logs
```

The cases themselves are executable code in the tree (`engine/src/core/lmcache_tier_test.cpp`,
`terminal/serve/test_lmcache_status.py`); `cases/*.md` is an inventory of them, regenerated from those files so it
cannot claim a case the code does not have:

```
python tools/lmcache/testing/list_cases.py          # rewrites cases/*.md from the sources
```

## Running them

```
cmake --preset cpu-check
cmake --build build/cpu-check -j8 --target lmcache_tier_test

build/cpu-check/lmcache_tier_test                        # offline: an in-memory store
build/cpu-check/lmcache_tier_test tcp://127.0.0.1:5555    # and again against a real LMCache store
ctest --test-dir build/cpu-check -j8                     # the whole engine's CPU suite
cd terminal && python3 -m unittest serve.test_lmcache_status -v
```

`run.sh` does all four and writes the logs into `results/`:

```
tools/lmcache/testing/run.sh build/cpu-check [tcp://127.0.0.1:5555]
```

## Reading a result

The C++ binary prints failures only - one `FAIL: <label>` line each - and a final `N checks, M failed`.  So a run's log
is short on purpose: no failures and `0 failed` is the whole story.  A `results/<date>.md` records the counts, the
commit, the environment and what each suite ran against; the `.log` files beside it are the commands' own output.

## What the two layers answer

* **offline** (`lmcache_tier_test`, no store): the tier against a store fixture that behaves like LMCache - one object
  per chunk, first write wins, no replacement.  This is where the format, the gate, the cutting into windows, the
  bookkeeping of what is already published, the picture and file rules, and every failure path are pinned.
* **live** (`lmcache_tier_test tcp://...`): the same shapes over the real ZMQ data plane and a real store, including
  several engines on one store, interleaved users, and the image form the engine's writer really publishes.
* **monitor** (`serve.test_lmcache_status`): the card's HTTP report plane - a healthy store, one that is down, and no
  store configured at all.
