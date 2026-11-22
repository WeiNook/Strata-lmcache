"""What an LMCache server tells about itself over HTTP: the observability surface a monitor page can read.

The store's data plane is ZMQ (lmcache_wire.cpp); its *reporting* plane is a plain HTTP API on the same process
(port 8080 by default), plus optional Prometheus metrics.  This script prints what each endpoint answers on the
running server, so the fields the monitor shows are read from real output, not from documentation.

    python http_probe.py                        # http://127.0.0.1:8080
    python http_probe.py http://192.168.121.134:8080
    python http_probe.py --model strata/qwen38-flash-next   # also list that model's objects
"""
import argparse
import json
import sys
import urllib.error
import urllib.request


def get(url, timeout=10):
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")
    except Exception as exc:                                  # noqa: BLE001 - a probe reports, it does not fail
        return None, f"{type(exc).__name__}: {exc}"


def show(base, path, note=""):
    url = base + path
    status, body = get(url)
    print(f"\n=== GET {path}   {note}")
    print(f"  status: {status}")
    try:
        parsed = json.loads(body)
    except Exception:
        print(f"  body:   {body[:800]}")
        return None
    print(f"  keys:   {list(parsed)[:20] if isinstance(parsed, dict) else type(parsed).__name__}")
    print("  body:   " + json.dumps(parsed, ensure_ascii=False)[:1800])
    return parsed


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("base", nargs="?", default="http://127.0.0.1:8080")
    p.add_argument("--model", default="", help="also list this model's objects in L1")
    a = p.parse_args()
    base = a.base.rstrip("/")

    show(base, "/", "is this the API?")
    show(base, "/healthcheck")
    # report_status(): the server's own view of its pools - the numbers a monitor wants
    show(base, "/status", "engine.report_status()")
    show(base, "/lmc_version")
    show(base, "/config/adapters", "which L2 adapters exist (L1 has none)")
    if a.model:
        show(base, f"/cache/objects?tier=l1&model_name={urllib.request.quote(a.model)}&page_size=5",
             "this model's L1 objects")
    # Prometheus: the counters, if the server started its metrics port
    show(base, "/metrics", "may 404: a separate metrics server serves this")

    print("\n=== notes")
    print("  /status is the one to poll for 'how much of my model is in the store'.")
    print("  /cache/objects is L2/adapter listing; with L1-only it may answer 503 (no adapters).")
    print("  everything here is read-only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
