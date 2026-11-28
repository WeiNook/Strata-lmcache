# Shared cache across engines (LMCache)

Strata already keeps a conversation in three places: the GPU (parked state, `STRATA_KV_HOST_REGISTER`), host RAM
(`ConversationCache`, sized by `STRATA_CONVERSATION_CACHE_MIB` / `--conversation-cache`), and a local SSD file per
conversation (`ConversationSpill`). All three are per process: four engines on four cards each keep their own copy of
the same long system prompt, and a request that lands on a different instance after a restart reads its whole prompt
again.

This tier is the fourth one: a **shared store on the network**, so an engine can pick up a conversation another engine
(or an earlier run of itself) put there. It is LMCache's MP ("multiprocess") server, used as an opaque block store.

```
 request ──► L1 GPU park ──► L2 RAM (ConversationCache) ──► L3 SSD file ──► L4 LMCache (this tier, shared)
                                                     hit?     hit?            hit?
```

Nothing above L4 changes shape: a session is still the same v1 byte stream (`session_stream_write`), so a conversation
restored from the store is byte-for-byte the one that was saved to disk.

## What runs where

| piece | runs | notes |
|---|---|---|
| `lmcache server` | one container per host, or one on a cache node | **CPU only**, no GPU, no CUDA context: `lmcache-cpu` image built from `python:3.11-slim` + `lmcache==0.5.7.post1` |
| Strata engine | every GPU instance, unchanged | talks to the store over TCP |

The store never touches the GPU, which is why **the engine's CUDA version does not have to match anything on the store
side**. The three places a CUDA version *does* matter are all avoided here:

* `lmcache_driven` mode uses CUDA IPC between engine and store - this tier only uses `engine_driven` (plain bytes);
* NIXL/RDMA and GDS transports need extra kernel modules - not used;
* LMCache's own `c_cops` extension must be built with the same CUDA major as torch - we run a prebuilt CPU wheel.

## Build the store image

`tools/lmcache/store.Dockerfile` builds it: `python:3.11-slim` + CPU-only torch + a pinned `lmcache` (2.7 GB).

```bash
docker build -f tools/lmcache/store.Dockerfile \
  --build-arg http_proxy=http://192.168.121.1:10808 \
  --build-arg https_proxy=http://192.168.121.1:10808 \
  -t lmcache-cpu:0.5.7 tools/lmcache
```

The build container does not inherit the docker daemon's proxy - pass it as a build argument.  On an air-gapped host,
point `--build-arg PIP_INDEX_URL=` at an internal mirror.

Why not the official `lmcache/standalone`: it is a CUDA image (8.3 GB) and is only published as nightly tags.  This tier
never touches a GPU, and the engine is written against one release's wire format (`engine/src/core/lmcache_wire.cpp`,
checked byte for byte), so a small CPU-only image of a pinned version is the safer thing to run.  The image version and
`tools/lmcache/dump_wire.py` are what to re-check before moving to a newer LMCache.

## Start the store

```bash
docker run -d --name lmcache --network host --shm-size=8g \
  -e no_proxy=localhost,127.0.0.1 \
  lmcache-cpu:0.5.7 \
  --l1-size-gb 48 --eviction-policy LRU
```

(the command is the image's entrypoint: `lmcache server --host 0.0.0.0 --port 5555 --http-host 0.0.0.0
--transport zmq --supported-transfer-mode engine_driven`)

**The HTTP report plane binds to `127.0.0.1` unless you say otherwise.** LMCache 0.5.7 has two separate address flags:
`--host` for the ZMQ data plane (5555) and `--http-host` for the HTTP report plane (8080, `/status` and `/healthcheck`).
The entrypoint sets both to `0.0.0.0`; if you launch the server another way, add `--http-host 0.0.0.0` or the monitor,
`curl` and `telnet` to the HTTP port will fail from outside the container even though the data port answers. To move
the report port, pass `--http-port <port> -e LMCACHE_HTTP_PORT=<port>` (the env var keeps the health check in sync).

The image's entrypoint already passes both of these, but if you start the server another way they are not optional:

* `--transport zmq` - the data plane this engine speaks (there is also gRPC; the engine does not use it).
* `--supported-transfer-mode engine_driven` - without it the server only assembles `lmcache_driven`, and every
  registration comes back `operation_not_enabled`.

`--shm-size` is not optional either: the L1 pool lives in shared memory, and docker's 64 MB default fails at startup.
On 麒麟 V10 (kernel 4.19) the io_uring paths are unavailable and the store falls back to `posix` I/O on its own; the
L1 tier is RAM, so this only affects optional disk tiers.

## Point an engine at it

```bash
STRATA_LMCACHE=tcp://192.168.121.134:5555        # the one switch: empty (default) = off
STRATA_LMCACHE_NAMESPACE=qwen3.8-flash-next      # what this model registers as; default: the model's own name
```

| setting | meaning | default |
|---|---|---|
| `STRATA_LMCACHE` | the store's data plane, `tcp://host:5555`. Empty = the tier is off and no other flag is read | empty |
| `STRATA_LMCACHE_HTTP` | the store's report plane (the monitor reads `/status` from it). Derived from `STRATA_LMCACHE` when unset: `http://<same host>:8080` | derived |
| `STRATA_LMCACHE_NAMESPACE` | the model name this engine registers and looks up under. Several models can share one store; each only ever touches its own | the model's name |
| `STRATA_LMCACHE_REPLACE_RAM` | `1`: the store takes the place of the in-process conversation cache - a finished conversation is published and not parked here. See "Do I still need the RAM tier?" | 0 |
| `STRATA_LMCACHE_BYTES_PER_TOKEN` | the state's size per token, which fixes the object shape. Both ends of a namespace must use the same value, so changing it invalidates what is stored under it | 19531 (~19 KB, this geometry) |
| `STRATA_LMCACHE_MIN_TOKENS` | the cost gate: a prefix shorter than this is not published (the transfer would cost more than the read it saves) | 8192 |

Two things bite when the store is not on the default ports or not on the same network:

* **The derived HTTP URL always uses port 8080.** If the store was started with `--http-port 8086`, set
  `STRATA_LMCACHE_HTTP=http://<host>:8086` explicitly. Otherwise the monitor polls `http://<host>:8080`, which is
  either closed or - inside a bridge container - the engine's own HTTP server, so the page reports "Not reachable".
* **`127.0.0.1` means the container you are in.** A store started with `--network host` listens on the host, so an
  engine in a bridge container (the usual `-p 8083:8080` deployment) must reach it by the host's address, not
  `127.0.0.1`. Either add `--add-host=host.docker.internal:host-gateway` and use `tcp://host.docker.internal:8085`,
  or use the host's LAN IP. An engine that itself runs with `--network host` may keep `127.0.0.1`.

Because these are ordinary engine settings, all three delivery paths work with no extra plumbing: a model file's
`env` block, a compose/`docker run -e`, or the settings page. `docs/SETTINGS.md` is generated from the same register, so
`python tools/settings_register.py --check` keeps code and documentation in step.

## Build an image that carries it

The tier is part of the engine binary (`engine/src/core/lmcache_wire.cpp`, `lmcache_store.cpp`, `lmcache_tier.cpp`,
linked by `engine/cmake/strata_gpu_engine.cmake`), so **any image built from this tree has it** - there is no build flag
to turn on, and no separate component to install. Build with the repository's own source Dockerfile:

```bash
docker build -f tools/docker/Dockerfile.src \
  --build-arg STRATA_COMMIT=$(git rev-parse HEAD) \
  --build-arg STRATA_VERSION=0.2.18 \
  --build-arg CUDA_ARCHITECTURES=86 \
  --build-arg HTTP_PROXY=http://<build-proxy>:8080 --build-arg HTTPS_PROXY=http://<build-proxy>:8080 \
  -t strata:0.2.18-lmcache-cuda13 -t strata:0.2.18-lmcache .
```

* `CUDA_ARCHITECTURES` defaults to `75;80;86;89;120` (every card, a fat binary, a long compile). Narrowing it to the
  card you have - A10 and A40 are `86`, RTX 40 `89`, RTX 50 `120` - builds the same image in a fraction of the time,
  and the image then runs on that architecture only.
* The build needs to reach GitHub once (the pinned CPython for the Terminal) and PyPI (the wheel sets). Where the build
  host can use a PyPI mirror, pass `--build-arg PIP_INDEX_URL=https://mirrors.aliyun.com/pypi/simple/` - it covers the
  engine stage's requirements. The Terminal's own wheels are installed hash-locked in isolation, so that step ignores
  it and needs either the network or `--wheelhouse`; and the CPython archive comes from GitHub, so a host that cannot
  reach GitHub still needs a proxy for that one download.
* The tag is `<release>-lmcache-<flavor>`: `0.2.18` is the release (the version `strata --version` prints first), the
  engine component inside the image reports its own version separately.

Checked inside the built image:

```
$ strata --version
strata 0.2.18 (build 0.1.64; python 3.12.15 at /opt/strata/terminal/python/bin/python3)
$ /opt/strata/components/engine/*/strata --help | grep -A6 'shared conversation cache'
  shared conversation cache (docs/LMCACHE.md), all by environment, off until the first is set:
    STRATA_LMCACHE=tcp://host:5555   the store's data plane; empty = the tier is off
    ...
```

The desktop app (`app/`) is not part of the image; the monitor a container serves is `terminal/serve/web`, and it
carries the same row (see "In the monitor").

## What is stored, and how

A conversation is addressed by **its token prefix**, the same identity LMCache uses for any engine
(`IPCCacheServerKey`: model, token bytes, `[start, end)`), and its state is stored as one **head record** plus one
record per `chunk_size` tokens (256 by default):

```
head       key = (namespace, tokens[0:256), [0, 256))          -> tokens, checkpoints, running state
window i   key = (namespace, tokens[0:(i+1)*256), [i*256, (i+1)*256))  -> the K/V of those 256 tokens
```

`bytes_per_token` is the engine's real K/V size per token (~19 KB/token at this geometry), registered once as the
store's `hidden_dim_size` with `dtype_str=uint8` and `use_mla=true`. LMCache validates every object against that shape,
so a truncated or foreign payload is rejected by the store rather than restored as garbage.

The layout follows from three rules of the store, each of which was found the hard way:

* **One record is one object.** A key's token range is resolved into one object per chunk and the commit must carry
  exactly that many tensors, so a record may never span two chunks. This is why the head is a separate small record and
  not a prefix of the state.
* **The first write wins.** The store never replaces an object it already has, so a key that does not change holds the
  first bytes ever written for it. The head is therefore keyed by the length it covers (a longer turn writes another
  head) and the windows are written once, by the first publish that reaches them.
* **Two records may not share a range.** The store does not reliably separate two records over the same `[start, end)`
  by `cache_salt` (two records over `[0, 256)` came back as each other on 0.5.7), so the head gets its own key stream -
  the first chunk with its opening tokens replaced by a digest of the length and of the whole prefix - and no prompt can
  ever produce it.

Consequences worth knowing:

* **A resume point floors to a chunk boundary.** Strata's continuation point is a turn boundary, which is almost never a
  multiple of 256; the tier publishes `floor(tokens/256)*256` and the first at most 255 tokens are recomputed.
* **A turn moves one head plus its new windows.** Appending 2 K tokens to a 200 K-token conversation writes one head and
  nine windows, not 780 objects.
* **The head says which conversation it belongs to.** Its tokens travel inside it and the reader compares them with the
  prompt's: a store that hands over another conversation's head (two conversations that open with the same 256 tokens)
  is refused, and the prompt is recomputed - never restored from someone else's state.
* **Pictures ride in the tokens.** An image is a run of placeholder tokens, so two requests with the same text and
  different screenshots have the same ids and different K/V. They share one cache entry (the store cannot hold two under
  one prefix), and the engine's own reuse check - same pictures, same steering - then refuses a state that was computed
  for another picture. The answer is always right; the second conversation simply does not get the reuse. Files need
  nothing here: their text is in the ids, so a different file is a different prefix.

## What it costs

| conversation | state bytes | publish/restore at 10 GbE | at 25 GbE |
|---|---|---|---|
| 8 K tokens | ~150 MB | 0.15 s | 0.06 s |
| 32 K tokens | ~600 MB | 0.6 s | 0.25 s |
| 63 K tokens (full context) | ~1.2 GB | 1.2 s | 0.5 s |

Measured on the store this feature was developed against (one virtual machine, one CPU-only server, C++ client):
**325-470 MiB/s store, ~650 MiB/s retrieve**, byte-exact both ways. Reading the same 63 K-token prefix takes 80-125 s,
so the transfer pays for itself from roughly 4 K tokens on a 10 GbE link - which is what `STRATA_LMCACHE_MIN_TOKENS`
encodes.

## How the engine uses it

The levels are **not a strict chain: the engine uses whichever source gives this prompt the longest matching prefix**,
and RAM wins ties.  That matters for what this tier does and does not do.

**On a request**, in this order:

1. what the engine already holds - the parked state, the batch slot's, the RAM tier's best entry (`parked`) - which is
   the floor every later step has to beat;
2. an image the background writer still holds in RAM (it was only on its way to disk);
3. **the shared store** (this tier): its head is read first (one small object), which says how long the session is.  A
   session no longer than the floor above is refused right there - the windows are never transferred, so a turn the RAM
   tier wins costs one round trip and a few KB instead of the whole state.  Only when it *is* longer are its windows
   read, and then compared as before: a session that does not start a longer stretch of this prompt than the RAM tier
   already does is dropped and RAM's choice stands;
4. the local SSD tier, under the same rule;
5. taking or borrowing from the RAM tier.

**Publishing is not triggered by a miss.**  It happens when a conversation is *evicted from RAM* - the moment it is
leaving anyway - on a background thread, so no request ever waits for a transfer, and a store that is down only means
the publish is skipped.

With `STRATA_LMCACHE_REPLACE_RAM=1` it also happens **as each response goes out** (see "Making the store the only
tier"): there the store has taken the place of the in-process cache, so waiting for a later request to displace the
conversation would leave it invisible to the other instances that just became able to use it.  The publish is a copy,
so this instance's next turn still finds its state live.  Publishing on every miss would spend bandwidth for nothing: a prompt that is not in the store
yet is usually the one whose state is still live in RAM.

**The SSD tier is optional.**  A host without an NVMe disk runs with it off; the shared tier then gets a writer of its
own, so evictions still reach the store (the two writers are exclusive - one image is never queued twice, and never
copied).  Conversely, running the shared tier off leaves the SSD tier exactly as it was.

## Do I still need the RAM tier?

Yes - but it can be much smaller.  Moving a conversation from one tier to the other does not by itself save memory: the
same bytes still live somewhere on the host.  What the two tiers differ in is *what a reuse costs*.

| | RAM tier (`--conversation-cache-mib`) | the store (its L1 pool) |
|---|---|---|
| where | inside the engine's process | in the store's process, on the same host's RAM |
| a restore costs | the buffers are handed back, then uploaded to VRAM: **~0.1 s** for 1.2 GB | the VRAM upload, plus serialize + hash, a TCP round trip and a validating parse: **~2-4 s** for 1.2 GB |
| who can use it | this instance only | every instance of this model, and this one after a restart |
| eviction | LRU against its byte budget and slot count | LRU against the pool, plus write/read TTLs (600 s / 300 s by default) |
| a miss costs | nothing | one object fetch, unless the cost gate refuses the attempt first |

So the RAM tier is what makes *this instance's next message* fast, and the store is what makes *another instance's*
message possible at all.  Removing the RAM tier makes every reuse - including the ordinary "same conversation, next
turn" - pay the round trip.

**Where the memory is actually saved.**  The store saves memory only where the same content would otherwise be
duplicated: four instances sharing one 63 K-token system prompt (plus the same documents) hold one 1.2 GB copy in the
store instead of four copies in their own RAM tiers.  If the instances mostly serve *different* conversations, the
store adds a copy rather than replacing one.

A layout that usually fits:

* RAM tier: `--conversation-cache-mib 2048` per instance - enough for the conversation in flight and its neighbours.
  The default is 40% of free RAM capped at 8192, which is what makes four instances hold 32 GB between them;
* store L1: `--l1-size-gb` sized for the *shared* prefixes (the prompts and documents several instances see), not for
  the sum of the instances' working sets.

### Making the store the only tier (`STRATA_LMCACHE_REPLACE_RAM=1`)

If the instances on one host would rather give the RAM back than keep their own copies, this mode publishes each
finished conversation and keeps nothing in the engine:

```bash
--conversation-cache-mib 0            # no in-process tier at all
STRATA_LMCACHE_REPLACE_RAM=1          # the store replaces it
```

Each response is published **as it goes out** (a copy, on the writer's thread, with only the windows the conversation
added), so another instance can serve the very next turn instead of reading the prompt again.

What it does *not* give up is the conversation in flight: parking never touched the live state, so the next message on
the same instance is still immediate.  What it does cost is the cross-instance case - and it is not small at a long
context: a 256 K-token conversation is roughly 4.8 GB of state, so a restore at the ~0.5 GB/s a Python store sustains is
**about ten seconds** (against minutes of recompute, but nothing like a RAM handoff).

A middle ground that usually fits better: keep one conversation's worth in the engine
(`--conversation-cache-mib 6144 --conversation-cache-slots 1`) so the active conversation is free on this instance, and
let the store hold everything else.  The eviction that keeps that single slot warm is also what feeds the store.

One hard edge, when `REPLACE_RAM` is **not** set: publishing is driven by **eviction from the RAM tier**.  Shrinking it makes evictions frequent, which
is exactly what fills the store.  Setting it to 0 (`--conversation-cache-mib 0`) removes the evictions entirely, and
then nothing is ever published and the store would only ever be read (that is what `REPLACE_RAM=1` exists for: it
publishes the capture itself, with no RAM tier involved).

## In the monitor

The cache card gains a **Shared cache (LMCache)** row once a store is configured, in both monitors: the engine's own web
page (what a container serves on `:8080`, `terminal/serve/lmcache_status.py`) and the desktop app
(`app/host/server/lmcache-status.mjs`). Both read the store's own `GET /status` (`engine.report_status()`), which
reports the L1 pool's objects, bytes and capacity, plus `non_cuda_context_meta`: the model each registered instance
belongs to.

The store is shared, so the card distinguishes the cases instead of guessing:

* **exclusive** - this model is the only one registered: the pool's totals *are* this model's, and the row shows
  `N objects · X GB`;
* **shared** - other models are registered too: the row says so and shows the whole pool, because per-model bytes are
  not something the store reports;
* **not registered** - the store is up but this engine has not stored anything yet;
* the store being switched off omits the row entirely, and one that cannot be reached says "not reachable". Nothing is
  faked either way.

The address comes from the environment the engine runs in - `STRATA_LMCACHE_HTTP`, else `STRATA_LMCACHE` with the
report port (`:8080`) substituted. The web monitor polls it on a background thread (5 s, no proxies, 1.5 s timeout), so
a monitor request never waits for the store and a store that hangs cannot make the page hang.

`tools/lmcache/http_probe.py` prints what a running store answers, and `tools/lmcache/dump_wire.py` prints the exact
bytes of every RPC payload, so both can be re-verified against a new LMCache version.

## When something is wrong

* **The engine connects but nothing ever answers.** In LMCache 0.5.7 the server's only dispatch thread dies on a
  malformed request (`assert len(msg) >= 3` in `mq.py`), and the socket keeps accepting connections afterwards. Look for
  `Exception in thread mq-server-thread` in `docker logs lmcache`; restart the store.
* **`operation_not_enabled` on registration.** The server was started without
  `--supported-transfer-mode engine_driven` (see above).
* **Startup fails inside the store on the L1 pool.** Two causes, both worth checking: `--shm-size` smaller than
  `--l1-size-gb` (the pool lives in shared memory), and `--l1-size-gb` larger than the host's RAM - the pool is
  allocated eagerly at startup, so a 48 GB pool on a 16 GB machine dies with
  `can't allocate memory: you tried to allocate 51539611648 bytes` (`Error code 12`). Size it to what the host can
  hold, or move the store to a bigger machine.
* **A model name in the monitor that is not yours.** Two engines registered with the same `STRATA_LMCACHE_NAMESPACE`, or
  the namespace defaulted differently (the model's folder name) - set it explicitly to make the grouping certain.

The tier never fails a request. A store that is unreachable, slow or full only costs the reuse, exactly as when the
feature is switched off.
