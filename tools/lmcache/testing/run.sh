#!/usr/bin/env bash
# Runs the shared cache's four suites and writes their logs into results/.
#
#   tools/lmcache/testing/run.sh <build-dir> [store-endpoint]
#
# <build-dir>: a cpu-check build (cmake --preset cpu-check), e.g. build/cpu-check
# [endpoint] : tcp://host:5555 of a running LMCache store; without it the live suite is skipped
#
# The cases are in the sources (engine/src/core/lmcache_tier_test.cpp, terminal/serve/test_lmcache_status.py) and
# described in cases/*.md; this script only runs them and keeps what they printed.
set -u

build=${1:?usage: run.sh <build-dir> [store-endpoint]}
endpoint=${2:-}
here=$(cd "$(dirname "$0")" && pwd)
stamp=$(date +%F)
out="$here/results"
mkdir -p "$out"
rc=0

run() {   # run <name> <command...>
    local name=$1; shift
    echo "== $name: $*" >"$out/$stamp-$name.log"
    "$@" >>"$out/$stamp-$name.log" 2>&1 || { echo "  FAILED (see $stamp-$name.log)"; rc=1; }
    tail -3 "$out/$stamp-$name.log"
}

run offline "$build/lmcache_tier_test"
[ -n "$endpoint" ] && run live "$build/lmcache_tier_test" "$endpoint"
run ctest ctest --test-dir "$build" -j8
( cd "$here/../../../terminal" && run monitor python3 -m unittest serve.test_lmcache_status )

echo "logs: $out/$stamp-*.log"
exit $rc
