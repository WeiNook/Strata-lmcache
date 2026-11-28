#!/usr/bin/env python3
"""Writes the inventory of the shared cache's test cases from the sources themselves.

The cases live where they are executed - `engine/src/core/lmcache_tier_test.cpp` (the tier, offline and against a real
store) and `terminal/serve/test_lmcache_status.py` (the monitor's card).  This script reads those files and writes what
they contain, so the inventory cannot claim a case the code does not have, or miss one it does.

    python tools/lmcache/testing/list_cases.py            # rewrites cases/*.md

Each case's one-line description is the comment block directly above it, which is where the reason it exists is written.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CPP = ROOT / "engine/src/core/lmcache_tier_test.cpp"
PY = ROOT / "terminal/serve/test_lmcache_status.py"
OUT = Path(__file__).resolve().parent / "cases"

CASE = re.compile(r"^void (\w+)\(\) \{", re.M)
LIVE = re.compile(r"^int live\(const std::string& endpoint\) \{", re.M)
CLASS = re.compile(r"^class (\w+)\(unittest\.TestCase\):", re.M)
METHOD = re.compile(r"^    def (test_\w+)\(self\):", re.M)


def description_above(text: str, at: int) -> str:
    """The comment lines directly above `at`, joined; empty when there is none."""
    lines = text[:at].splitlines()
    block: list[str] = []
    for line in reversed(lines):
        stripped = line.strip()
        if not stripped:
            if block:
                break
            continue
        if stripped.startswith("//"):
            block.append(stripped[2:].strip())
            continue
        break
    return " ".join(reversed(block))


def body(text: str, start: int) -> str:
    """The text from `start` to the matching closing brace at column 0."""
    end = text.find("\n}\n", start)
    return text[start:] if end < 0 else text[start:end]


def cpp_cases() -> tuple[list[tuple[str, str, int]], int]:
    text = CPP.read_text(encoding="utf-8")
    out = []
    for m in CASE.finditer(text):
        chunk = body(text, m.end())
        sites = sum(chunk.count(a) for a in ("check(", "same(", "has("))
        if sites:                                   # a helper (clear_all, rebind, ...) is not a case
            out.append((m.group(1), description_above(text, m.start()), sites))
    live = LIVE.search(text)
    live_body = body(text, live.end()) if live else ""
    live_checks = sum(live_body.count(a) for a in ("check(", "same(", "has("))
    return out, live_checks


def py_cases() -> list[tuple[str, str, str]]:
    lines = PY.read_text(encoding="utf-8").splitlines()
    out, cls = [], ""
    for i, line in enumerate(lines):
        m = CLASS.match(line)
        if m:
            cls = m.group(1)
            continue
        m = METHOD.match(line)
        if m:
            doc = ""
            if i + 1 < len(lines) and lines[i + 1].strip().startswith('"""'):
                doc = lines[i + 1].strip().strip('"').strip()
            out.append((m.group(1), cls, doc))
    return out


def write_cpp(cases: list[tuple[str, str, int]], live_checks: int) -> None:
    total = sum(n for _, _, n in cases) + live_checks
    out = [
        "# Shared cache - tier cases (C++)\n",
        "\n",
        "Generated from `engine/src/core/lmcache_tier_test.cpp` by `tools/lmcache/testing/list_cases.py`; edit the test\n",
        "file, not this one.  Run them with:\n",
        "\n",
        "```\n",
        "cmake --preset cpu-check && cmake --build build/cpu-check -j8 --target lmcache_tier_test\n",
        "build/cpu-check/lmcache_tier_test                          # offline: the in-memory store\n",
        "build/cpu-check/lmcache_tier_test tcp://127.0.0.1:5555      # and the whole thing again, against a real store\n",
        "```\n",
        "\n",
        f"{len(cases)} cases, {total} assertion sites ({live_checks} of them in the `live` section below).  The binary\n",
        "reports the assertions it executed per run; the count here is the call sites in the source (a loop runs its\n",
        "site once per iteration).\n",
        "\n",
        "| case | what it is for | asserts |\n",
        "|---|---|---|\n",
    ]
    for name, desc, checks in cases:
        out.append(f"| `{name}` | {desc or '-'} | {checks} |\n")
    out += [
        "\n",
        "## `live(endpoint)` - the same shapes against a real LMCache store\n",
        "\n",
        f"Only run when an endpoint is given ({live_checks} assertions).  It publishes and reads back:\n",
        "\n",
        "1. a 1024-token session, and the same state from a second engine on the same store;\n",
        "2. 16 concurrent reads from two engines (the store's per-session prefetch state, the shared `request_id` bug);\n",
        "3. a 9216-token session at the production 8192 cost gate, and an 8000-token one below it;\n",
        "4. **the image form the engine's writer publishes** (the K/V inside the image, `chunk_extract`), both layers;\n",
        "5. two users with the same text and different pictures, read by a third engine;\n",
        "6. three users over two engines, interleaved, two turns each - a turn published on one engine read on the other;\n",
        "7. a conversation nobody published (a miss, never another user's state).\n",
    ]
    (OUT / "lmcache_tier_cases.md").write_text("".join(out), encoding="utf-8")


def write_py(cases: list[tuple[str, str, str]]) -> None:
    out = [
        "# Shared cache - monitor cases (Python)\n",
        "\n",
        "Generated from `terminal/serve/test_lmcache_status.py` by `tools/lmcache/testing/list_cases.py`.  Run them with\n",
        "(no pytest needed):\n",
        "\n",
        "```\n",
        "cd terminal && python3 -m unittest serve.test_lmcache_status -v\n",
        "```\n",
        "\n",
        f"{len(cases)} cases.\n",
        "\n",
        "| class | case | what it checks |\n",
        "|---|---|---|\n",
    ]
    for name, cls, doc in cases:
        out.append(f"| `{cls}` | `{name}` | {doc or '-'} |\n")
    out += [
        "\n",
        "The card reads the store's HTTP report plane only; it is polled off the request path and never raises, which is\n",
        "what the \"unreachable store\" and \"no store configured\" cases pin.\n",
    ]
    (OUT / "monitor_cases.md").write_text("".join(out), encoding="utf-8")


def main() -> int:
    if not CPP.is_file() or not PY.is_file():
        print(f"missing sources: {CPP} / {PY}", file=sys.stderr)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    cases, live_checks = cpp_cases()
    write_cpp(cases, live_checks)
    write_py(py_cases())
    print(f"{OUT / 'lmcache_tier_cases.md'}: {len(cases)} cases")
    print(f"{OUT / 'monitor_cases.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
