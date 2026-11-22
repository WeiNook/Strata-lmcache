# The shared cache store for Strata (docs/LMCACHE.md): LMCache in MP mode, CPU only.
#
# Why not the official image: lmcache/standalone is a CUDA image - 8.3 GB, nightly tags only.  This tier never touches a
# GPU (it stores opaque bytes and reports over HTTP), so a CPU-only image is 2.7 GB, carries no CUDA runtime that could
# drift from the engine's own, and pins the exact release whose ZMQ/msgpack contract the engine was written against
# (engine/src/core/lmcache_wire.cpp, verified against this version's bytes).
#
#   docker build -f tools/lmcache/store.Dockerfile \
#     --build-arg http_proxy=http://<proxy>:10808 \
#     --build-arg https_proxy=http://<proxy>:10808 \
#     -t lmcache-cpu:0.5.7 tools/lmcache
#
# The build container does not inherit the docker daemon's proxy, so pass it as a build argument (or use a mirror).
# Behind an internal mirror: --build-arg PIP_INDEX_URL=http://mirror/simple
FROM python:3.11-slim

# Pinned on purpose: the engine speaks this release's wire format.  A newer server is a code change, not a rebuild.
ARG LMCACHE_VERSION=0.5.7.post1
ARG PIP_INDEX_URL=https://pypi.org/simple
ARG TORCH_INDEX_URL=https://download.pytorch.org/whl/cpu

ENV PYTHONDONTWRITEBYTECODE=1 \
    PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1

# torch from the CPU wheel index first: the default wheel pulls in ~2 GB of CUDA the store cannot use.
RUN pip install --index-url "${TORCH_INDEX_URL}" torch \
 && pip install --index-url "${PIP_INDEX_URL}" "lmcache==${LMCACHE_VERSION}" \
 && python -c "import lmcache, torch; print('lmcache', lmcache.__version__, 'torch', torch.__version__)"

# 5555: the data plane (ZMTP/msgpack, what the engine writes sessions through)
# 8080: the report plane (GET /status, what the monitor reads)
EXPOSE 5555 8080

# The report plane's port is its own flag (--http-port, 8080 by default); set LMCACHE_HTTP_PORT when you move it so the
# health check follows.
HEALTHCHECK --interval=30s --timeout=5s --start-period=90s \
    CMD python -c "import os,sys,urllib.request; p=os.environ.get('LMCACHE_HTTP_PORT','8080'); sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:'+p+'/healthcheck', timeout=4).status == 200 else 1)"

# The L1 pool is shared memory: docker's 64 MB default makes the server fail at startup, so always run with
#   --shm-size=8g   (size it above --l1-size-gb)
#
# The entrypoint passes --transport zmq --supported-transfer-mode engine_driven itself, because forgetting the second
# one makes every registration fail; append arguments to change anything else (the last value of a flag wins):
#   docker run ... <image> --l1-size-gb 48 --port 5555 --http-port 8080
COPY store-entrypoint.sh /usr/local/bin/store-entrypoint.sh
RUN chmod +x /usr/local/bin/store-entrypoint.sh
ENTRYPOINT ["/usr/local/bin/store-entrypoint.sh"]
