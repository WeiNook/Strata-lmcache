/**
 * LMCache's report, reduced to what Strata's monitor needs.
 *
 * The store's data plane is ZMQ (engine/src/core/lmcache_wire.cpp); it also answers plain HTTP on its own port
 * (8080 by default) with `GET /status`, the engine's `report_status()`.  That is the only endpoint a monitor needs:
 * it carries the L1 pool's objects/bytes/capacity, the hash geometry, and - in `non_cuda_context_meta` - which
 * registered instance belongs to which model, keyed by the instance id the engine registered with.
 *
 * The store is shared: several models (and several Strata processes) can be registered at once.  The pool totals are
 * therefore store-wide, and per-model bytes are only knowable when this model is the *only* one resident - which is
 * what `mine.exclusive` says.  When it is not exclusive the reducer reports the store totals and leaves `mine.bytes`
 * null instead of inventing a share.
 *
 * Nothing here throws on a malformed or missing report: a store that is down is `{available:false}`, which the
 * monitor renders as "off".
 */

const finite = (n) => typeof n === 'number' && Number.isFinite(n);

/** The default reporting endpoint; the engine's own setting (STRATA_LMCACHE_HTTP) overrides it. */
export const DEFAULT_LMCACHE_HTTP = 'http://127.0.0.1:8080';

/** The L1 pool from whichever shape this server reports it in. */
function l1Of(status) {
  const sm = status?.storage_manager ?? {};
  const managers = sm.l1_managers ?? null;
  const one = managers && typeof managers === 'object' ? Object.values(managers)[0] : sm.l1_manager;
  const pool = one && typeof one === 'object' ? one : {};
  const usage = Array.isArray(status?.l1_usage) ? status.l1_usage : null;
  const used = finite(pool.memory_used_bytes) ? pool.memory_used_bytes : finite(usage?.[0]) ? usage[0] : null;
  const total = finite(pool.memory_total_bytes) ? pool.memory_total_bytes
    : finite(pool.memory_configured_bytes) ? pool.memory_configured_bytes
      : finite(usage?.[1]) ? usage[1] : null;
  return {
    objects: finite(pool.total_object_count) ? pool.total_object_count : null,
    usedBytes: used,
    totalBytes: total,
    ratio: finite(used) && finite(total) && total > 0 ? used / total : null,
    backend: typeof pool.backend === 'string' ? pool.backend : null,
  };
}

/** `non_cuda_context_meta`: instance id -> the context that instance registered. */
function modelsOf(status) {
  const meta = status?.non_cuda_context_meta;
  if (!meta || typeof meta !== 'object') return [];
  return Object.entries(meta).map(([instanceId, c]) => ({
    instanceId,
    modelName: typeof c?.model_name === 'string' ? c.model_name : '',
    blockSize: finite(c?.block_size) ? c.block_size : null,
    worldSize: finite(c?.world_size) ? c.world_size : null,
    useMla: typeof c?.use_mla === 'boolean' ? c.use_mla : null,
  }));
}

/**
 * Reduce a `/status` body to the monitor's view.
 * @param status the parsed JSON, or null when the store could not be reached
 * @param modelName the model this engine runs, as registered with the store
 */
export function reduceLmcacheStatus(status, modelName) {
  if (!status || typeof status !== 'object') return { available: false };
  const store = l1Of(status);
  const models = modelsOf(status);
  const name = typeof modelName === 'string' ? modelName : '';
  const mineModels = name ? models.filter((m) => m.modelName === name) : [];
  const exclusive = models.length > 0 && (name ? models.every((m) => m.modelName === name) : false);
  const first = mineModels[0] ?? null;
  return {
    available: true,
    healthy: status.is_healthy !== false,
    engine: typeof status.engine_type === 'string' ? status.engine_type : null,
    chunkSize: finite(status.chunk_size) ? status.chunk_size : null,
    store,
    models,
    mine: {
      registered: mineModels.length > 0,
      instances: mineModels.length,
      blockSize: first?.blockSize ?? null,
      worldSize: first?.worldSize ?? null,
      useMla: first?.useMla ?? null,
      // the store holds only this model: its totals are this model's totals
      exclusive,
      shared: models.length > 1,
      objects: exclusive ? store.objects : null,
      bytes: exclusive ? store.usedBytes : null,
    },
    at: new Date().toISOString(),
  };
}

/** `http://host:8080` from `tcp://host:5555`, so one setting can serve both planes. */
export function httpFromRpc(rpc) {
  const m = /^(?:tcp:\/\/)?([^:/]+)(?::(\d+))?$/.exec(String(rpc ?? '').trim());
  if (!m) return '';
  return `http://${m[1]}:8080`;
}

/** GET /status with a timeout; a down store is null, never an exception. */
export async function fetchLmcacheStatus(url, { modelName = '', timeoutMs = 1500 } = {}) {
  const base = String(url ?? '').replace(/\/+$/, '');
  if (!base) return { available: false };
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), Math.max(250, Number(timeoutMs) || 1500));
  try {
    const res = await fetch(`${base}/status`, { signal: ctl.signal, headers: { accept: 'application/json' } });
    if (!res.ok) return { available: false, error: `HTTP ${res.status}` };
    return reduceLmcacheStatus(await res.json(), modelName);
  } catch (err) {
    return { available: false, error: String(err?.name === 'AbortError' ? 'timeout' : err?.message ?? err) };
  } finally {
    clearTimeout(timer);
  }
}

let snapshot = { available: false };
let poller = null;

/** The last good report (never null; `{available:false}` before the first poll). */
export function lmcacheSnapshot() {
  return snapshot;
}

/**
 * Poll the store in the background while the monitor is up.  Idempotent: calling it again with the same arguments
 * keeps the first timer; `url` empty stops polling.  The timer is unref'd so it never holds the process open.
 */
export function startLmcachePolling({ url = '', modelName = '', intervalMs = 5000 } = {}) {
  if (poller) {
    clearInterval(poller);
    poller = null;
  }
  if (!url) {
    snapshot = { available: false };
    return;
  }
  const tick = async () => {
    snapshot = await fetchLmcacheStatus(url, { modelName, timeoutMs: Math.min(4000, Math.max(500, intervalMs)) });
  };
  tick();
  poller = setInterval(tick, Math.max(1000, Number(intervalMs) || 5000));
  if (typeof poller.unref === 'function') poller.unref();
}
