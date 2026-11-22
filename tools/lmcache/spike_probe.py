#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""P0 spike: can LMCache's MP server store an OPAQUE whole-conversation state blob?

Strata's cache unit is one serialized `SavedConversation` (~1.2 GB at 63K tokens:
GDN recurrence + conv history + QSA K/V pages + draft layer), NOT a vLLM
per-chunk KV tensor.  Before writing any C++ we must know whether LMCache's
engine-driven path takes arbitrary bytes.

What this measures
------------------
1. the real RPC surface of the installed lmcache (no guessing: it prints
   `get_rpc_specs()`);
2. whether a non-vLLM client can register an engine-driven context
   (`dtype_str="uint8"`);
3. store -> retrieve round-trip of N x 16 MiB opaque blocks, with a byte-exact
   check, and MB/s both ways;
4. whether a NON chunk-aligned `end` is accepted (Strata's resume points sit at
   turn boundaries, which are not multiples of any chunk size -- if this fails
   we fall back to the gateway design);
5. the cost of a miss (the probe ladder does ~tens of these per request).

Run on the server (CPU only, no GPU needed):

    lmcache server --host 0.0.0.0 --port 5555 --l1-size-gb 48 --eviction-policy LRU

    pip install lmcache
    python tools/lmcache/spike_probe.py inspect
    python tools/lmcache/spike_probe.py roundtrip --blob-mib 1200
    python tools/lmcache/spike_probe.py roundtrip --blob-mib 1200 --url tcp://10.0.0.7:5555

Exit code 0 = the native path works, 2 = it does not (go to the gateway plan).
"""

from __future__ import annotations

import argparse
import inspect
import os
import sys
import time
import traceback

MiB = 1 << 20


def _settle(value, timeout: float = 120.0):
    """Resolve whatever the transport handed back: value, future, or coroutine."""
    if value is None:
        return None
    if inspect.isawaitable(value):
        import asyncio

        async def _run(aw):
            return await aw

        try:
            asyncio.get_running_loop()
        except RuntimeError:
            return asyncio.run(_run(value))
        raise SystemExit("this tool is not meant to run inside a live event loop")
    for attr in ("result", "wait", "get"):
        fn = getattr(value, attr, None)
        if callable(fn):
            try:
                return fn(timeout) if attr == "wait" else fn()
            except TypeError:
                try:
                    return fn()
                except Exception:
                    continue
            except Exception:
                raise
    return value


def _client(url: str):
    """0.5.x: lmcache.v1.multiprocess.transport.factory (the dev branch used v1.client.transport)."""
    try:
        from lmcache.v1.multiprocess.transport.factory import RequestClientFactory

        return RequestClientFactory.create(url)
    except ImportError:
        from lmcache.v1.client.transport.factory import RequestClientFactory  # older layout

        return RequestClientFactory.create(url)


def _imports():
    """Everything we need, resolved by name so a rename shows up as a clear error."""
    mod = {}
    try:
        from lmcache.v1.multiprocess import custom_types as ct
    except ImportError:  # older layout
        from lmcache.v1.common import custom_types as ct

    mod["ct"] = ct
    mod["pack"] = None
    for path in (
        "lmcache.v1.multiprocess.token_codec",
        "lmcache.v1.multiprocess.custom_types",
        "lmcache.v1.utils.token_codec",
        "lmcache.v1.common.token_codec",
    ):
        try:
            m = __import__(path, fromlist=["pack_tokens"])
        except Exception:
            continue
        if hasattr(m, "pack_tokens"):
            mod["pack"] = m.pack_tokens
            break
    return mod


def cmd_inspect(args) -> int:
    import lmcache

    print(f"lmcache: {getattr(lmcache, '__version__', 'unknown')} ({lmcache.__file__})")
    print(f"torch:   {_torch_version()}")

    # --- discovery: the layout differs between dev branch and releases --------
    print("\n--- lmcache.v1.transport members ---")
    try:
        import lmcache.v1.transport as tr

        names = sorted(n for n in dir(tr) if not n.startswith("_"))
        print("  " + ", ".join(names))
    except Exception as exc:
        print(f"  import failed: {type(exc).__name__}: {exc}")

    specs = None
    for modpath in ("lmcache.v1.multiprocess.transport.rpc", "lmcache.v1.multiprocess.transport.base",
                    "lmcache.v1.multiprocess.transport.zmq_impl", "lmcache.v1.multiprocess.rpc",
                    "lmcache.v1.transport.rpc", "lmcache.v1.transport.rpc_specs"):
        try:
            m = __import__(modpath, fromlist=["get_rpc_specs"])
            fn = getattr(m, "get_rpc_specs", None)
            if fn is None:
                print(f"  {modpath}: imported, but no get_rpc_specs "
                      f"(members: {[n for n in dir(m) if not n.startswith('_')][:12]})")
                continue
            specs = fn()
            print(f"\n--- RPC surface from {modpath} (the contract to bind to) ---")
            for name, spec in sorted(specs.items()):
                try:
                    params = ", ".join(
                        f"{p.name}: {p.annotation}" for p in spec.signature.parameters.values())
                    print(f"  {name}({params}) -> {spec.response_type}")
                except Exception:
                    print(f"  {name}: {spec}")
            break
        except Exception as exc:
            print(f"  {modpath}: {type(exc).__name__}: {exc}")

    if specs is None:
        print("\n--- no get_rpc_specs; dumping the client's own methods instead ---")
        cls = None
        for modpath in ("lmcache.v1.multiprocess.transport.base", "lmcache.v1.multiprocess.transport.factory"):
            try:
                m = __import__(modpath, fromlist=["RequestClient"])
                cls = getattr(m, "RequestClient", None) or getattr(m, "RequestClientFactory", None)
                if cls is not None:
                    print(f"  from {modpath}: {cls}")
                    break
            except Exception as exc:
                print(f"  {modpath}: {type(exc).__name__}: {exc}")
        if cls is not None:
            for n in sorted(dir(cls)):
                if n.startswith("_"):
                    continue
                attr = getattr(cls, n, None)
                if not callable(attr):
                    continue
                try:
                    print(f"  {n}{inspect.signature(attr)}")
                except Exception:
                    print(f"  {n}(...)")

    ct = _imports()["ct"]
    print("\n--- key / payload structs ---")
    for cls_name in (
        "IPCCacheServerKey",
        "RegisterEngineDrivenContextPayload",
        "RegisterEngineDrivenContextResponse",
        "PrepareStoreResponse",
        "PrepareRetrieveResponse",
    ):
        obj = getattr(ct, cls_name, None)
        if obj is None:
            print(f"  {cls_name}: MISSING")
            continue
        fields = getattr(obj, "__struct_fields__", None) or getattr(obj, "__dataclass_fields__", None)
        print(f"  {cls_name}: {fields}")

    try:
        c = _client(args.url)
        print(f"\nping:            {_settle(c.ping(None))}")
        print(f"get_chunk_size:  {_settle(c.get_chunk_size())}")
    except Exception as exc:
        print(f"\nclient connect FAILED: {exc}")
        traceback.print_exc()
        return 2
    return 0


def _torch_version() -> str:
    try:
        import torch

        return torch.__version__
    except Exception as exc:  # pragma: no cover
        return f"unavailable ({exc})"


def _tokens(n: int):
    """A deterministic token run, with NO period inside n.

    The ids only have to be stable and distinct: lmcache derives each chunk's
    object key from the token hashes, so a repeating token sequence would make
    two different payload chunks collide on one object and corrupt the compare.
    """
    return tuple((i * 2654435761) & 0xFFFFFF for i in range(n))


def _pack(codec, tokens):
    if codec["pack"] is not None:
        return codec["pack"](list(tokens))
    # documented layout: packed big-endian uint32, one word per token
    import struct

    return b"".join(struct.pack(">I", t) for t in tokens)


def _make_key(codec, model: str, tokens, start: int, end: int, req: str):
    ct = codec["ct"]
    key_cls = ct.IPCCacheServerKey
    # the official helper: packs the token ids the way the server's hasher expects
    from_ids = getattr(key_cls, "from_token_ids", None)
    if from_ids is not None:
        try:
            return from_ids(model_name=model, world_size=1, worker_id=0, num_kv_readers=1,
                            token_ids=tuple(tokens), start=start, end=end, request_id=req)
        except TypeError as exc:
            print(f"(from_token_ids fallback: {exc})")
    kw = dict(
        model_name=model,
        world_size=1,
        worker_id=0,
        num_kv_readers=1,          # 0.5.7 rejects a lookup whose key does not carry it
        token_bytes=_pack(codec, tokens),
        start=start,
        end=end,
        request_id=req,
    )
    try:
        return key_cls(**kw)
    except TypeError:
        # cache_salt and friends have defaults; anything else is a real surprise
        fields = getattr(key_cls, "__struct_fields__", ())
        kw = {k: v for k, v in kw.items() if k in fields}
        return key_cls(**kw)


def _poll_prefetch(c, request_id: str, max_polls: int = 200, interval: float = 0.05):
    """Hit-chunk count of a lookup, or None.  The server keys jobs by request_id."""
    for _ in range(max_polls):
        v = _settle(c.query_prefetch_status(request_id))
        if v is not None:
            return v
        time.sleep(interval)
    return None


def _tensor_wire():
    from lmcache.v1.multiprocess.transfer_context.tensor_wire import (
        decode_tensors,
        encode_tensors,
    )

    return encode_tensors, decode_tensors


def cmd_roundtrip(args) -> int:
    """Store our bytes as N opaque uint8 tiles under one token-range key.

    lmcache 0.5.7 stores PER CHUNK, not per blob: `commit_store` takes a msgpack
    ``TensorEnvelope`` whose tensors are validated against the shape derived at
    registration time,
        shape = (num_layers, num_physical_slots, hidden_dim_size)   # use_mla=True
    so one object == `chunk_size` tokens, and a whole conversation is a chain of
    objects.  We therefore pick hidden_dim_size = bytes per token, which makes a
    chunk exactly `chunk_size * bytes_per_token` bytes -- i.e. an arbitrary byte
    blob, cut into tiles, with no assumption about what those bytes mean.
    """
    import torch

    codec = _imports()
    ct = codec["ct"]
    encode_tensors, decode_tensors = _tensor_wire()
    blob_bytes = args.blob_mib * MiB
    model = args.model
    per_token = args.bytes_per_token

    c = _client(args.url)
    print(f"ping: {_settle(c.ping(None))}")
    chunk = int(_settle(c.get_chunk_size()) or 0)
    print(f"server chunk size: {chunk} tokens")
    try:
        print(f"negotiate_chunk_size(1): {_settle(c.negotiate_chunk_size(1))}")
    except Exception as exc:
        print(f"negotiate_chunk_size(1): {type(exc).__name__}: {exc}")

    per_chunk = chunk * per_token
    nchunks = max(1, (blob_bytes + per_chunk - 1) // per_chunk)
    total = nchunks * per_chunk
    tokens = _tokens(nchunks * chunk + 64)
    print(f"blob {blob_bytes / MiB:.1f} MiB -> {nchunks} chunks x "
          f"{per_chunk / MiB:.2f} MiB (tile = [{1}, {chunk}, {per_token}] uint8)")

    # ---- 1. register a non-vLLM, opaque, engine-driven context -----------------
    ctx_id = os.getpid()
    payload = ct.RegisterEngineDrivenContextPayload(
        instance_id=ctx_id,
        model_name=model,
        world_size=1,
        block_size=chunk,
        num_layers=1,
        hidden_dim_size=per_token,
        dtype_str="uint8",
        use_mla=True,                       # shape without the K/V=2 dimension
        num_physical_slots=chunk,           # one slot per token in a chunk
    )
    try:
        resp = _settle(c.register_kv_cache_engine_driven_context(payload))
        print(f"REGISTER: ok  shm_name={getattr(resp, 'shm_name', '')!r} "
              f"pool_size={getattr(resp, 'pool_size', 0)}")
    except Exception as exc:
        print(f"REGISTER FAILED: {type(exc).__name__}: {exc}")
        return 2

    # ---- 2. store it ---------------------------------------------------------
    blob = os.urandom(total)
    tiles = [
        torch.frombuffer(bytearray(blob[i * per_chunk:(i + 1) * per_chunk]), dtype=torch.uint8)
        .reshape(1, chunk, per_token)
        for i in range(nchunks)
    ]
    wire = encode_tensors(tiles)
    print(f"wire envelope: {len(wire) / MiB:.1f} MiB (overhead "
          f"{len(wire) - total} bytes)")
    end = nchunks * chunk
    key = _make_key(codec, model, tokens, 0, end, "spike-aligned")

    t0 = time.perf_counter()
    try:
        _settle(c.prepare_store(key, ctx_id))
        ok = bool(_settle(c.commit_store(key, ctx_id, wire)))
    except Exception as exc:
        print(f"STORE FAILED: {type(exc).__name__}: {exc}")
        return 2
    store_s = time.perf_counter() - t0
    print(f"STORE  {total / MiB:.1f} MiB in {store_s * 1e3:.1f} ms "
          f"= {total / MiB / store_s:.0f} MiB/s  (ok={ok})")
    if not ok:
        print("commit_store returned False -> the server refused the payload")
        return 2

    # ---- 3. retrieve + byte-exact check -------------------------------------
    # the real flow is lookup (reserves read locks + triggers the prefetch) ->
    # poll query_prefetch_status -> prepare_retrieve -> commit_retrieve
    t0 = time.perf_counter()
    hit = None
    try:
        _settle(c.lookup(key, 1))
        hit = _poll_prefetch(c, key.request_id)
        print(f"LOOKUP: hit_chunks={hit} in {(time.perf_counter() - t0) * 1e3:.1f} ms "
              f"(asked for {nchunks})")
    except Exception as exc:
        print(f"LOOKUP FAILED: {type(exc).__name__}: {exc}")

    t0 = time.perf_counter()
    got = _settle(c.prepare_retrieve(key, ctx_id))
    get_s = time.perf_counter() - t0
    data = getattr(got, "data", b"")
    print(f"RETRIEVE success={getattr(got, 'success', None)} in {get_s * 1e3:.1f} ms "
          f"(wire {len(data) / MiB:.1f} MiB)")
    exact = False
    if data:
        try:
            back = decode_tensors(data, expected_shape=(1, chunk, per_token),
                                  expected_dtype=torch.uint8, expected_count=nchunks)
            joined = b"".join(t.reshape(-1).numpy().tobytes() for t in back)
            exact = joined == blob
            print(f"decoded {len(back)} tiles, {len(joined) / MiB:.1f} MiB")
        except Exception as exc:
            print(f"decode FAILED: {type(exc).__name__}: {exc}")
    print(f"byte-exact: {exact}")
    try:
        _settle(c.commit_retrieve(key, ctx_id))
    except Exception as exc:
        print(f"(commit_retrieve: {exc})")

    # ---- 4. does a NON-aligned end work?  (Strata resumes at turn boundaries) --
    ragged_end = max(1, end - 137)
    rkey = _make_key(codec, model, tokens, 0, ragged_end, "spike-ragged")
    ragged_ok, ragged_note = False, ""
    try:
        _settle(c.prepare_store(rkey, ctx_id))
        rn = len(_settle(c.prepare_store(rkey, ctx_id)).context or {})
        ragged_ok = rn >= 0
        ragged_note = f"prepare_store ok (context keys={rn})"
    except Exception as exc:
        ragged_note = f"{type(exc).__name__}: {exc}"
    print(f"non-aligned end={ragged_end} accepted: {ragged_ok}  {ragged_note}")

    # ---- 5. what a miss costs ----------------------------------------------
    miss_key = _make_key(codec, model + "/no-such-namespace", tokens, 0, end, "spike-miss")
    t0 = time.perf_counter()
    try:
        miss = _settle(c.prepare_retrieve(miss_key, ctx_id))
        miss_ok = getattr(miss, "success", None)
    except Exception as exc:
        miss_ok = f"raised {type(exc).__name__}"
    print(f"MISS probe: {miss_ok} in {(time.perf_counter() - t0) * 1e3:.2f} ms")

    print("\n--- verdict ---")
    print(f"opaque uint8 tiles accepted : {'yes' if ok else 'NO'}")
    print(f"byte-exact round trip       : {'yes' if exact else 'NO'}")
    print(f"store throughput            : {total / MiB / max(store_s, 1e-9):.0f} MiB/s")
    print(f"retrieve throughput (wire)  : {len(data) / MiB / max(get_s, 1e-9):.0f} MiB/s")
    try:
        _settle(c.unregister_kv_cache_engine_driven_context(ctx_id))
    except Exception:
        pass

    return 0 if (ok and exact) else 2


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--url", default=os.getenv("LMCACHE_MP_URL", "tcp://127.0.0.1:5555"),
                   help="lmcache server URL (or LMCACHE_MP_URL)")
    sub = p.add_subparsers(dest="cmd", required=True)

    # --url may also be given AFTER the subcommand: SUPPRESS keeps the global default
    def _url(sp):
        sp.add_argument("--url", default=argparse.SUPPRESS, help="same as the top-level --url")

    pi = sub.add_parser("inspect", help="dump the installed API surface (safe, changes nothing)")
    pi.set_defaults(func=cmd_inspect)
    _url(pi)

    pr = sub.add_parser("roundtrip", help="store+retrieve an opaque blob and report")
    pr.add_argument("--blob-mib", type=int, default=1200, help="blob size in MiB (1200 ~= a 63K-token conversation)")
    pr.add_argument("--model", default="strata/spike/v1", help="namespace; use model_name=namespace")
    pr.add_argument("--bytes-per-token", type=int, default=19456,
                    help="payload bytes per token; a chunk tile is chunk_size * this bytes "
                         "(19456 = the ~19KB/token of a real Strata session state)")
    pr.set_defaults(func=cmd_roundtrip)
    _url(pr)

    args = p.parse_args()
    if os.name != "nt":
        import signal

        signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(
            SystemExit("probe deadline: the server did not answer (see its log)")))
        signal.alarm(int(os.getenv("PROBE_DEADLINE_S", "600")))
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
