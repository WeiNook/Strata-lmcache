#!/bin/sh
# The store's entrypoint.  Two flags are required by the engine's contract and are easy to forget:
#   --transport zmq                    the data plane the engine speaks (there is also gRPC; the engine does not use it)
#   --supported-transfer-mode engine_driven
# Without the second one the server only assembles lmcache_driven and every registration comes back
# operation_not_enabled, so the image passes both itself.
#
# Everything the caller passes is appended, and the server's parser takes the LAST value of a repeated flag, so
#   docker run <image> --l1-size-gb 48 --port 6000 --http-port 6001
# changes exactly what it names and keeps the rest.  LMCACHE_HOST / LMCACHE_PORT do the same from the environment.
set -e
exec lmcache server \
  --host "${LMCACHE_HOST:-0.0.0.0}" \
  --port "${LMCACHE_PORT:-5555}" \
  --transport zmq \
  --supported-transfer-mode engine_driven \
  "$@"
