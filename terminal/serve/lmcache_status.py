"""LMCache's report, reduced to what the Monitor's Conversation cache card needs.

The engine talks to the store over ZMQ (``engine/src/core/lmcache_wire.cpp``); the store also answers plain HTTP on
its own port (``--http-port``, 8080 by default).  ``GET /status`` is the only endpoint a monitor needs: the L1 pool's
objects / bytes / capacity, the chunk geometry, and - in ``non_cuda_context_meta`` - which registered instance belongs
to which model, keyed by the instance id the engine registered with.

The store is shared, so its totals are store-wide.  Per-model bytes are only knowable when this model is the *only*
one resident, which is what ``mine.exclusive`` says; when it is not, the view reports the store totals and leaves
``mine.bytes`` ``None`` instead of inventing a share.

Nothing here raises: a store that is down is ``{"available": False}``, which the card renders as unreachable.  The
polling runs on a daemon thread so a monitor poll never waits for the store, and the URL is fetched with proxies
bypassed (a container with ``HTTP_PROXY`` set must still reach a store on the same host).
"""
from __future__ import annotations

import json
import math
import os
import threading
import time
import urllib.request

#: How often the background thread refreshes the view, and how long one fetch may take.
INTERVAL_S = 5.0
TIMEOUT_S = 1.5

#: The store's own default report port; ``STRATA_LMCACHE_HTTP`` overrides the whole URL.
DEFAULT_HTTP_PORT = 8080

_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def http_from_rpc(rpc) -> str:
    """``http://host:8080`` from ``tcp://host:5555``, so one setting can serve both planes ("" when there is none)."""
    text = str(rpc or "").strip()
    if not text:
        return ""
    if "://" in text:
        text = text.split("://", 1)[1]
    text = text.split("/", 1)[0]
    if text.startswith("["):                     # an IPv6 literal keeps its brackets
        end = text.find("]")
        if end < 0:
            return ""
        return f"http://{text[:end + 1]}:{DEFAULT_HTTP_PORT}"
    if ":" in text:
        host, _, port = text.rpartition(":")
        if not host or not port.isdigit():
            return ""
        text = host
    return f"http://{text}:{DEFAULT_HTTP_PORT}" if text else ""


def store_url(environ=None) -> str:
    """The store's report plane: ``STRATA_LMCACHE_HTTP``, else derived from ``STRATA_LMCACHE``, else "" (off)."""
    env = os.environ if environ is None else environ
    url = (env.get("STRATA_LMCACHE_HTTP") or "").strip()
    if url:
        return url.rstrip("/")
    return http_from_rpc(env.get("STRATA_LMCACHE"))


def model_name(environ=None) -> str:
    """The name this engine registered under: ``STRATA_LMCACHE_NAMESPACE``, else the engine's own default."""
    env = os.environ if environ is None else environ
    return (env.get("STRATA_LMCACHE_NAMESPACE") or "").strip() or "strata"


def _number(value):
    """A finite number, or None: JSON booleans and strings are not measurements."""
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return value if math.isfinite(value) else None


def _l1_of(status: dict) -> dict:
    """The L1 pool, from whichever shape this store reports it in (``storage_manager`` or ``l1_usage``)."""
    manager = status.get("storage_manager")
    manager = manager if isinstance(manager, dict) else {}
    managers = manager.get("l1_managers")
    if isinstance(managers, dict) and managers:
        first = next(iter(managers.values()))
    else:
        first = manager.get("l1_manager")
    pool = first if isinstance(first, dict) else {}
    usage = status.get("l1_usage")
    usage = usage if isinstance(usage, list) else None
    used = _number(pool.get("memory_used_bytes"))
    if used is None and usage:
        used = _number(usage[0])
    total = _number(pool.get("memory_total_bytes"))
    if total is None:
        total = _number(pool.get("memory_configured_bytes"))
    if total is None and usage and len(usage) > 1:
        total = _number(usage[1])
    ratio = used / total if used is not None and total else None
    return {"objects": _number(pool.get("total_object_count")), "used_bytes": used, "total_bytes": total,
            "ratio": ratio, "backend": pool.get("backend") if isinstance(pool.get("backend"), str) else None}


def _models_of(status: dict) -> list[dict]:
    """``non_cuda_context_meta``: instance id -> the context that instance registered."""
    meta = status.get("non_cuda_context_meta")
    if not isinstance(meta, dict):
        return []
    out = []
    for instance_id, context in meta.items():
        context = context if isinstance(context, dict) else {}
        out.append({"instance_id": str(instance_id),
                    "model_name": context.get("model_name") if isinstance(context.get("model_name"), str) else "",
                    "block_size": _number(context.get("block_size")),
                    "world_size": _number(context.get("world_size")),
                    "use_mla": context.get("use_mla") if isinstance(context.get("use_mla"), bool) else None})
    return out


#: Any one of these makes a body a report rather than some other JSON on that port.
_REPORT_KEYS = ("is_healthy", "engine_type", "chunk_size", "storage_manager", "l1_usage", "non_cuda_context_meta")


def reduce_status(status, name: str = "strata") -> dict:
    """Reduce a ``/status`` body to the card's view; anything that is not a report is "unreachable"."""
    if not isinstance(status, dict) or not any(key in status for key in _REPORT_KEYS):
        return {"available": False}
    store = _l1_of(status)
    models = _models_of(status)
    mine = [m for m in models if name and m["model_name"] == name]
    exclusive = bool(models) and bool(name) and all(m["model_name"] == name for m in models)
    first = mine[0] if mine else None
    return {"available": True,
            "healthy": status.get("is_healthy") is not False,
            "chunk_size": _number(status.get("chunk_size")),
            "store": store,
            "models": models,
            "model_count": len({m["model_name"] for m in models}),
            "mine": {"registered": bool(mine), "instances": len(mine),
                     "block_size": first["block_size"] if first else None,
                     "world_size": first["world_size"] if first else None,
                     "use_mla": first["use_mla"] if first else None,
                     "exclusive": exclusive,
                     "shared": len({m["model_name"] for m in models}) > 1,
                     # the store holds only this model: its totals are this model's totals
                     "objects": store["objects"] if exclusive else None,
                     "bytes": store["used_bytes"] if exclusive else None}}


_state: dict = {"available": False}
_lock = threading.Lock()
_thread: threading.Thread | None = None
_watching: tuple[str, str] | None = None
_generation = 0


def _poll(url: str, name: str) -> dict:
    try:
        with _opener.open(url + "/status", timeout=TIMEOUT_S) as reply:
            body = reply.read(4 << 20)
        return reduce_status(json.loads(body.decode("utf-8", "replace")), name)
    except Exception as exc:                     # a monitor never fails the page it reports on
        return {"available": False, "error": type(exc).__name__}


def _loop(url: str, name: str, generation: int) -> None:
    global _state
    while True:
        report = _poll(url, name)
        with _lock:
            if generation != _generation:
                return                           # the endpoint changed: this thread is stale
            _state = report
        time.sleep(INTERVAL_S)


def _watch(url: str, name: str) -> None:
    """Start the background poller once per endpoint (a reload of the setting starts a new one)."""
    global _thread, _watching, _generation
    with _lock:
        if _watching == (url, name) and _thread is not None and _thread.is_alive():
            return
        _generation += 1
        _watching = (url, name)
        _thread = threading.Thread(target=_loop, args=(url, name, _generation), daemon=True,
                                   name="lmcache-monitor")
        _thread.start()


def view(environ=None) -> dict:
    """The last report, for the Monitor's card.  Empty environment = the tier is off, and nothing is polled."""
    url = store_url(environ)
    if not url:
        return {"available": False, "configured": False}
    name = model_name(environ)
    _watch(url, name)
    with _lock:
        report = dict(_state)
    report["configured"] = True
    report["url"] = url
    report["model"] = name
    return report


def reset() -> None:
    """Forget the poller and the last report (tests, and a settings reload)."""
    global _state, _thread, _watching, _generation
    with _lock:
        _generation += 1
        _state = {"available": False}
        _thread = None
        _watching = None
