"""Golden wire vectors for the C++ client: exact msgpack bytes the server expects, so an encoder written in another
language can be checked byte for byte instead of by trial and error.

    python dump_wire.py
"""
import msgspec


def hx(b):
    return b.hex()


def main():
    from lmcache.v1.multiprocess import custom_types as ct
    from lmcache.v1.multiprocess.transport.zmq_impl.wire import encode_operation
    from lmcache.v1.multiprocess.transport.zmq_impl.mq import RequestUID

    print("### RequestUID =", RequestUID)
    print("  sample uid=12345 ->", hx(msgspec.msgpack.encode(12345)))

    print("\n### operation encoding")
    for op in ("ping", "get_chunk_size", "prepare_store", "commit_store", "prepare_retrieve", "commit_retrieve",
               "lookup", "query_prefetch_status", "register_kv_cache_engine_driven_context",
               "unregister_kv_cache_engine_driven_context"):
        print(f"  {op}: {hx(encode_operation(op))}")

    print("\n### raw small values")
    for v in (None, True, False, 0, 1, 127, 128, 255, 256, 64768, -1):
        print(f"  {v!r}: {hx(msgspec.msgpack.encode(v))}")
    print("  b'abc':", hx(msgspec.msgpack.encode(b"abc")))
    print("  'abc':", hx(msgspec.msgpack.encode("abc")))
    print("  [] :", hx(msgspec.msgpack.encode([])))
    print("  ():", hx(msgspec.msgpack.encode(())))

    print("\n### IPCCacheServerKey")
    print("  array_like:", ct.IPCCacheServerKey.__struct_config__.array_like)
    print("  fields:", [(f.name, str(f.type)) for f in msgspec.structs.fields(ct.IPCCacheServerKey)])
    key = ct.IPCCacheServerKey.from_token_ids(
        model_name="m", world_size=1, worker_id=0, token_ids=(1, 2, 3), start=0, end=256,
        request_id="r1", num_kv_readers=1)
    enc = msgspec.msgpack.encode(key)
    print("  encoded:", hx(enc))

    print("\n### RegisterEngineDrivenContextPayload")
    p = ct.RegisterEngineDrivenContextPayload(
        instance_id=4242, model_name="m", world_size=1, block_size=256, num_layers=1,
        hidden_dim_size=19531, dtype_str="uint8", use_mla=True, num_physical_slots=256)
    print("  encoded:", hx(msgspec.msgpack.encode(p)))

    print("\n### response structs")
    for name in ("RegisterEngineDrivenContextResponse", "PrepareStoreResponse", "PrepareRetrieveResponse"):
        cls = getattr(ct, name, None)
        if cls is None:
            continue
        print(f"  {name}: array_like={cls.__struct_config__.array_like} "
              f"{[(f.name, str(f.type)) for f in msgspec.structs.fields(cls)]}")

    print("\n### TensorEnvelope (commit_store's bytes payload, one 4-byte tile)")
    from lmcache.v1.multiprocess.transfer_context import tensor_wire
    print("  members:", [n for n in dir(tensor_wire) if not n.startswith("_")])
    try:
        import torch
        env = tensor_wire.encode_tensors([torch.zeros((1, 1, 4), dtype=torch.uint8)])
        print("  encoded:", hx(env))
    except Exception as exc:
        print("  encode_tensors:", type(exc).__name__, exc)


if __name__ == "__main__":
    main()
