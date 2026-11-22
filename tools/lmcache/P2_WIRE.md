# P2（进行中）：C++ 直连 LMCache 的线格式契约

结论：**不需要 Python 网关，也不需要 libzmq。** C++ 侧自己实现 ZMTP 3.1 DEALER + msgpack 即可，
已经用黄金字节和真实服务端双向验证。

代码：`engine/include/strata/core/lmcache_wire.hpp`、`engine/src/core/lmcache_wire.cpp`
测试：`engine/src/core/lmcache_wire_test.cpp` —— 已进 `cpu-check` 与 ctest（**22 项离线编码校验**，不连网）；
      给出 endpoint 时再跑真实 RPC 与往返：`lmcache_wire_test tcp://127.0.0.1:5555 4`

## 1. 契约（从 lmcache 0.5.7.post1 实测，`tools/lmcache/dump_wire.py` 可复现）

| 项 | 事实 |
|---|---|
| 套接字 | 服务端 bind **ROUTER**，客户端 connect **DEALER**，ZMTP 3.1，机制 NULL |
| 请求帧 | `[msgpack int uid][msgpack int op][msgpack 载荷…]`，**一个载荷一帧** |
| 回复帧 | `[uid][op][载荷]`（op 被回显），载荷在**最后一帧** |
| 失败 | op 帧换成保留名 `"__lmcache_rpc_error__"`，下一帧是 `{code, operation, message}` |
| 载荷结构 | msgpack **map，键是字段名**（msgspec 默认），所以字段顺序无关 |
| 大载荷 | `commit_store` 第三参与 `prepare_retrieve` 的 `data` 都是**裸 bin**，不是嵌套结构 |
| uid | 就是 `int`（`RequestUID = int`），客户端自增 |

操作 id（`encode_operation` 发的就是这些整数）：

```
lookup 8   query_prefetch_status 9   register_context 14   unregister_context 15
prepare_store 16   commit_store 17   prepare_retrieve 18   commit_retrieve 19
get_chunk_size 21  ping 22
```

各操作的载荷：

- `register_kv_cache_engine_driven_context(RegisterEngineDrivenContextPayload)` → map：
  `instance_id, model_name, world_size, block_size, num_layers, hidden_dim_size, dtype_str, use_mla,
  num_physical_slots, group_layouts[]`
- `prepare_store(key, instance_id, skipped_chunks: tuple[tuple[int,...],...])` → 空跳过表就是 `90`
- `commit_store(key, instance_id, data: bytes)`，`data` = msgpack `{version:1, tensors:[{dtype,shape,data}]}`
- `prepare_retrieve(key, instance_id)` → `{success: bool, data: bytes, context: dict}`
- `commit_retrieve(key, instance_id)` → bool
- `lookup(key, tp_size)` → **数组**（每块的命中标记），不是 map
- `query_prefetch_status(request_id: str)` → int | null

`IPCCacheServerKey` 是 **dataclass（10 个键）**：
`model_name, world_size, worker_id(int|null), token_bytes(bin), start, end, request_id, cache_salt,
request_configs(map|null), num_kv_readers`。`token_bytes` = 每 token 4 字节大端（`pack_tokens`）。

## 2. 实测结果（虚拟机，CPU-only 服务端，C++ 客户端）

```
# 4 块（19 MiB）
server chunk size: 256      register_context: ok
store            : 19 MiB / 0.06 s = 325 MiB/s
prepare_retrieve : success=1, 19,999,911 bytes / 0.03 s（≈ 650 MiB/s 回传）
byte-exact       : 1  (19999744 / 19999744 字节一致)      checks: 34, failed 0

# 2 块（9.5 MiB，全链路回归时跑的）
store            : 9 MiB / 0.02 s = 468 MiB/s
byte-exact       : 1  (9999872 / 9999872 字节一致)        checks: 34, failed 0
```

对比 P0 的 Python 路径（store 464 MiB/s / retrieve 322 MiB/s），C++ 侧回传更快、发送略慢但同量级。
1.2 GB 会话量级的结论不变：**往返远快于重算（80–125 s）**。

## 3. 两条会咬人的坑（务必记住）

1. **一个畸形请求会打死服务端的唯一分发线程。** `mq.py:781` 用 `assert len(msg) >= 3` 校验帧数，
   断言失败抛在 `mq-server-thread` 里，线程直接死掉：之后服务端仍在监听、仍能握手，但**什么都不回**，
   客户端表现为超时。所以载荷数量必须严格按契约给，不能靠猜；调试期一旦出现"能连上但不回"，
   先看 `docker logs` 里有没有 `Exception in thread mq-server-thread`，重启服务端。
2. **我第一版把 uid 和 op 编进了同一帧**（`msgpack::Writer` 里连续写两个对象），服务端读到
   `operation == None`。一帧一对象是硬约束。

## 4. 已进构建

`engine/CMakeLists.txt` 增加了 `lmcache_wire_test`（`src/core/lmcache_wire_test.cpp` + `src/core/lmcache_wire.cpp`），
ctest 现有 **142 项，100% 通过**。注意主引擎 target 还未链接 `lmcache_wire.cpp`：等 tier 真正被
`generate.cpp` 调用时一起挂上，避免链接一个没人用的 TU。

## 5. 下一步（P2 剩余）

1. `LmcacheSink` / `LmcacheSource`：把 `session_stream_write/read` 接上（P1 已完成接口）。
   - `begin(total)` → 定 token 数（`ceil(total / 19531)` 向上对齐 256）、建 key、注册 context
   - `commit(16 MiB)` → 切片成 256-token 对象、攒 envelope、`prepare_store` + `commit_store`
   - `finish()` → 尾块 + 清单（清单用独立的 key 区间，读者永远看不到"有清单无数据"）
2. 策略层 `lmcache_tier`：命名空间、探测阶梯（先查清单再逐块）、成本闸门（低于 N token 不发布）、
   后台发布队列（debounce）、服务不可用时的静默降级。
3. `generate.cpp` 四处定点接入（`:13467` 初始化、`:13584` evict sink、`:13924` park 发布、`:18717` 恢复）。
4. `docs/LMCACHE.md` + settings 注册表（仓库有 `test_settings_register.py` 会校验文档↔代码）、compose 与
   `--shm-size` 要求。
