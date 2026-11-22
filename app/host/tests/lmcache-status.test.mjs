import test from 'node:test';
import assert from 'node:assert/strict';
import { reduceLmcacheStatus, httpFromRpc, fetchLmcacheStatus } from '../server/lmcache-status.mjs';

// A /status body as this server really answers it (trimmed to the fields the monitor reads), captured from
// lmcache 0.5.7.post1 after two Strata-style contexts were registered.
const status = (over = {}) => ({
  is_healthy: true,
  engine_type: 'MPCacheServer',
  chunk_size: 256,
  hash_algorithm: 'blake3',
  active_sessions: 1,
  registered_non_cuda_instance_ids: [4242],
  non_cuda_context_meta: { 4242: { model_name: 'qwen3.8-flash-next', world_size: 1, block_size: 256, use_mla: true } },
  storage_manager: {
    is_healthy: true,
    l1_managers: {
      _default: {
        is_healthy: true,
        total_object_count: 253,
        memory_used_bytes: 1288490188,
        memory_total_bytes: 51539607552,
        memory_configured_bytes: 51539607552,
        backend: 'dram',
        memory_usage_ratio: 0.025,
      },
    },
  },
  ...over,
});

test('a missing or unreachable store is simply unavailable', () => {
  assert.deepEqual(reduceLmcacheStatus(null, 'm'), { available: false });
  assert.deepEqual(reduceLmcacheStatus(undefined, 'm'), { available: false });
  assert.deepEqual(reduceLmcacheStatus('nope', 'm'), { available: false });
});

test('one registered model: the pool totals ARE this model\'s totals', () => {
  const v = reduceLmcacheStatus(status(), 'qwen3.8-flash-next');
  assert.equal(v.available, true);
  assert.equal(v.healthy, true);
  assert.equal(v.engine, 'MPCacheServer');
  assert.equal(v.chunkSize, 256);
  assert.equal(v.store.objects, 253);
  assert.equal(v.store.usedBytes, 1288490188);
  assert.equal(v.store.totalBytes, 51539607552);
  assert.equal(v.mine.registered, true);
  assert.equal(v.mine.exclusive, true);
  assert.equal(v.mine.shared, false);
  assert.equal(v.mine.objects, 253, 'exclusive: the store\'s objects are ours');
  assert.equal(v.mine.bytes, 1288490188, 'exclusive: the store\'s bytes are ours');
  assert.equal(v.mine.blockSize, 256);
  assert.equal(v.mine.useMla, true);
});

test('another model in the store: totals are shared, this model\'s bytes are not invented', () => {
  const two = status({
    non_cuda_context_meta: {
      4242: { model_name: 'qwen3.8-flash-next', world_size: 1, block_size: 256, use_mla: true },
      5151: { model_name: 'some-other-model', world_size: 1, block_size: 256, use_mla: true },
    },
    registered_non_cuda_instance_ids: [4242, 5151],
  });
  const v = reduceLmcacheStatus(two, 'qwen3.8-flash-next');
  assert.equal(v.mine.registered, true);
  assert.equal(v.mine.shared, true);
  assert.equal(v.mine.exclusive, false);
  assert.equal(v.mine.objects, null, 'a shared pool cannot attribute objects');
  assert.equal(v.mine.bytes, null, 'a shared pool cannot attribute bytes');
  assert.equal(v.store.objects, 253, 'the pool totals are still reported');
  assert.deepEqual(v.models.map((m) => m.modelName).sort(), ['qwen3.8-flash-next', 'some-other-model']);
});

test('only another model in the store: not registered here, nothing shown as ours', () => {
  const other = status({
    non_cuda_context_meta: { 5151: { model_name: 'some-other-model', world_size: 1, block_size: 256, use_mla: true } },
  });
  const v = reduceLmcacheStatus(other, 'qwen3.8-flash-next');
  assert.equal(v.available, true);
  assert.equal(v.mine.registered, false);
  assert.equal(v.mine.exclusive, false);
  assert.equal(v.mine.bytes, null);
  assert.equal(v.store.objects, 253, 'the store itself is still up and full');
});

test('the pool is read from whichever shape the server uses', () => {
  const flat = reduceLmcacheStatus(status({ storage_manager: { l1_manager: { total_object_count: 7, memory_used_bytes: 100, memory_total_bytes: 400 } } }), 'qwen3.8-flash-next');
  assert.equal(flat.store.objects, 7);
  assert.equal(flat.store.usedBytes, 100);
  assert.equal(flat.store.ratio, 0.25);
  const usage = reduceLmcacheStatus(status({ storage_manager: {}, l1_usage: [200, 800] }), 'qwen3.8-flash-next');
  assert.equal(usage.store.usedBytes, 200);
  assert.equal(usage.store.totalBytes, 800);
  const empty = reduceLmcacheStatus(status({ storage_manager: {} }), 'qwen3.8-flash-next');
  assert.equal(empty.store.objects, null);
  assert.equal(empty.store.usedBytes, null);
  assert.equal(empty.store.ratio, null);
});

test('a store that reports itself unhealthy is passed through as such', () => {
  const v = reduceLmcacheStatus(status({ is_healthy: false }), 'qwen3.8-flash-next');
  assert.equal(v.available, true);
  assert.equal(v.healthy, false);
});

test('one setting serves both planes: tcp://host:port becomes http://host:8080', () => {
  assert.equal(httpFromRpc('tcp://192.168.121.134:5555'), 'http://192.168.121.134:8080');
  assert.equal(httpFromRpc('192.168.121.134:5555'), 'http://192.168.121.134:8080');
  assert.equal(httpFromRpc('cache-host'), 'http://cache-host:8080');
  assert.equal(httpFromRpc(''), '');
  assert.equal(httpFromRpc(null), '');
});

test('fetch: a down store is {available:false}, never an exception', async () => {
  const real = globalThis.fetch;
  try {
    globalThis.fetch = async () => { throw new Error('ECONNREFUSED'); };
    const down = await fetchLmcacheStatus('http://127.0.0.1:9');
    assert.equal(down.available, false);
    assert.match(down.error, /ECONNREFUSED/);

    globalThis.fetch = async () => ({ ok: false, status: 503 });
    assert.equal((await fetchLmcacheStatus('http://127.0.0.1:9')).available, false);

    globalThis.fetch = async () => ({ ok: true, status: 200, json: async () => status() });
    const up = await fetchLmcacheStatus('http://127.0.0.1:8080', { modelName: 'qwen3.8-flash-next' });
    assert.equal(up.available, true);
    assert.equal(up.mine.objects, 253);

    assert.equal((await fetchLmcacheStatus('')).available, false, 'no endpoint configured: off');
  } finally {
    globalThis.fetch = real;
  }
});
