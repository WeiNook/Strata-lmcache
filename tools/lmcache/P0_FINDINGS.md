# P0 结论：LMCache 0.5.7 能否承载 Strata 的整份会话状态

> 环境：麒麟 V10 虚拟机（192.168.121.134，8 vCPU / 15GB / 内核 4.19，**无 GPU**）
> 服务端：`lmcache-cpu:0.5.7`（pip `lmcache==0.5.7.post1` + torch 2.15.0+cpu），`--transport zmq --supported-transfer-mode engine_driven --l1-size-gb 4`
> 客户端：`lmcache.v1.multiprocess` 官方 Python 客户端（`spike_probe.py`）
> 日期：2026-11-22

## 1. 结论

**Plan A 成立**：LMCache 原生通道可以按"不透明 uint8 字节"存取 Strata 的会话状态，**1.2GB 端到端字节精确往返通过**。

| 指标 | 16 MiB（4 块） | 1201.8 MiB（253 块） |
|---|---|---|
| register（非 GPU、uint8 上下文） | ok | ok |
| store | 667 MiB/s | **464 MiB/s（2.59 s）** |
| lookup（命中块数） | 4/4，2.7 ms | 253/253，**27.5 ms** |
| retrieve | 345 MiB/s | **322 MiB/s（3.73 s）** |
| 字节精确 | ✅ | ✅ |
| miss 探测耗时 | 1.3 ms | 12.4 ms |
| 线包开销 | 167 B | 9382 B（≈37 B/块） |

对照：1.2GB 恢复 3.7 s vs 重读 63K token 需 80–125 s（A10 单卡 prefill）。**收益成立。**
注：上述吞吐含 Python 侧 encode/decode 与两次拷贝，C++ 直连可更高；且这是虚拟机 + loopback，生产数字需重测。

## 2. 三个必须写进设计的修正（与最初方案不同）

### 2.1 粒度是"每 256 token 一个对象"，不是"整份会话一个 blob"

`commit_store` 收的不是任意字节，而是 **msgpack 编码的 `TensorEnvelope`**：

```
TensorEnvelope { version: 1, tensors: [ TensorFrame { dtype: str, shape: tuple[int,...], data: bytes } ] }
```

服务端按注册时推导的形状逐块校验（`decode_tensors`：count / dtype / shape / 字节长度全对才收）：

```
shape = (num_layers, num_physical_slots, hidden_dim_size)      # use_mla=True
      = (2, num_layers, num_physical_slots, hidden_dim_size)   # use_mla=False
```

所以：
- 注册时取 `use_mla=True, num_layers=1, num_physical_slots=chunk_size, hidden_dim_size=bytes_per_token, dtype_str="uint8"`；
- 一块 = `chunk_size × bytes_per_token` 字节；`bytes_per_token=19456`（≈19KB/token）时，一块 = 4.75 MiB；
- 一份 63K token 的会话 = 253 块 ≈ 1.2GB。

→ **C++ 侧要把序列化流按 4.75MiB（或按 chunk_size 对齐）切片发送**，不能一次发整包。
→ 好处：天然支持前缀部分命中（`lookup` 直接返回命中块数），比"整包有/无"更细。

### 2.2 检索是三段式，不是一次 RPC

```
lookup(key, 1)                    # 占读锁 + 触发预取
query_prefetch_status(request_id) # 轮询到非 None，返回命中块数
prepare_retrieve(key, ctx)        # -> PrepareRetrieveResponse(success, data, context)
commit_retrieve(key, ctx)         # 释放读锁
```
漏掉 `lookup` 时 `prepare_retrieve` 直接 `success=False`（我第一版就踩了这个）。

### 2.3 key 必须带 `num_kv_readers=1`

`IPCCacheServerKey` 的 `num_kv_readers` 默认 0，服务端会直接报
`ValueError: num_kv_readers=0: this server requires clients that send IPCCacheServerKey.num_kv_readers`。
且要用官方工厂 `IPCCacheServerKey.from_token_ids(model_name, world_size, worker_id, ...)`，
`worker_id` 是必填位置参数，`token_bytes` 是大端 uint32 逐 token 打包。

## 3. 对齐问题：必须在 256 的整数倍上存取

- `get_chunk_size()` = 256，`negotiate_chunk_size(1)` 仍返回 **256** → 对齐**不可协商**。
- `end=64631` 这类非对齐值 `prepare_store` 不报错，但块数是 `ceil(end/256)`，意味着**按块边界保存，不是按 token 精确保存**。

→ **设计决定**：发布/恢复点一律向下取整到 `floor(n/256)*256`。Strata 从该前缀继续重读剩余 token 即可，正确性无损，代价是最多 255 个 token 的重算（可忽略）。

## 4. 其它实测事实

- 非 GPU 环境下 LMCache 自动用 `StubCPUDevice`，**进程内无 CUDA context** → 与 CUDA 13.1 彻底解耦，架构判断成立。
- 服务端必须用 `--supported-transfer-mode engine_driven` 启动，否则 `register_kv_cache_engine_driven_context` 直接返回 `operation_not_enabled`。默认只装配 `lmcache_driven_transfer`。
- CPU-only 时 register 返回 `shm_name='' / pool_size=0`，走 **inline** 传输策略（"safe inline engine-driven transfer strategy"），不需要映射共享内存 → C++ 侧实现负担小。
- L1 容量不足时会**淘汰刚写入的对象**（第一次 1.2GB 测试复用了上一轮残留数据，`lookup` 直接 0 命中）。生产上 `--l1-size-gb` 必须显著大于"4 实例 × 单会话状态"的常驻量。
- 模块布局是 0.5.7 自己的：`lmcache.v1.multiprocess.*`（`transport/`、`transfer_context/`、`custom_types.py`），与 dev 分支的 `v1.client.transport` / `v1.common` 不同。协议签名以 `lmcache.v1.rpc.get_rpc_specs()` 为准。
- 官方 bench 客户端可作为线协议的参考实现：
  `lmcache/cli/commands/bench/server_bench/{client,helpers}.py`

## 5. 对 P1/P2 的影响

- `SessionSink`/`SessionSource` 抽象仍然正确且必要（P1 不变）：网络 sink 只需按 4.75MiB 切片收字节。
- P2 的 key 推导要改：不再是"整份会话一个 key"，而是"token 区间 → 253 个块 key"，块 key 由服务端 `TokenHasher`（blake3, chunk=256）生成，**Strata 侧不需要自己算块哈希**，只需给出 `token_bytes + start + end`。
- 探测阶梯简化：`lookup(key, 1)` 一次就能拿到"命中多少块"，不必逐候选探测。

## 6. 复现

```bash
# 服务端
docker run -d --name lmc-server --network host --shm-size=6g \
  -e no_proxy=localhost,127.0.0.1 lmcache-cpu:0.5.7 \
  lmcache server --host 0.0.0.0 --port 5555 --l1-size-gb 4 \
  --eviction-policy LRU --transport zmq --supported-transfer-mode engine_driven

# 探针（inspect 先 dump 真实签名，roundtrip 做往返）
docker cp tools/lmcache/spike_probe.py lmc-server:/tmp/probe.py
docker exec lmc-server python /tmp/probe.py inspect   --url tcp://127.0.0.1:5555
docker exec lmc-server python /tmp/probe.py roundtrip --blob-mib 1200 --url tcp://127.0.0.1:5555
```
