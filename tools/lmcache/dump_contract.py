"""Dump the exact RPC contract this LMCache install speaks: operations, their payload types, and each payload's
msgpack shape.  Read-only: it inspects types, it does not connect or change anything.

    python dump_contract.py            # the table
"""
import msgspec


def fields_of(cls):
    try:
        fs = msgspec.structs.fields(cls)
    except Exception:
        return None
    return [(f.name, str(f.type)) for f in fs]


def shape(cls):
    cfg = getattr(cls, "__struct_config__", None)
    array_like = getattr(cfg, "array_like", None)
    fs = fields_of(cls)
    return array_like, fs


def main():
    import lmcache
    print(f"lmcache {getattr(lmcache, '__version__', '?')}")

    from lmcache.v1.multiprocess import rpc as rpc_mod
    print("\n### rpc module members")
    print([n for n in dir(rpc_mod) if not n.startswith("_")])

    RpcSpec = getattr(rpc_mod, "RpcSpec", None)
    print("\n### RpcSpec", RpcSpec)
    if RpcSpec is not None:
        print("  fields:", fields_of(RpcSpec))
        print("  slots :", [n for n in dir(RpcSpec) if not n.startswith("_")])

    # the registry the client builds its methods from
    reg = None
    for name in ("get_rpc_specs", "RPC_SPECS", "SPECS", "_RPC_SPECS", "OPERATIONS"):
        if hasattr(rpc_mod, name):
            reg = getattr(rpc_mod, name)
            print(f"\n### registry: rpc.{name}")
            break
    if callable(reg):
        reg = reg()

    names = [
        "ping",
        "get_chunk_size",
        "register_kv_cache_engine_driven_context",
        "unregister_kv_cache_engine_driven_context",
        "prepare_store",
        "commit_store",
        "prepare_retrieve",
        "commit_retrieve",
        "lookup",
        "query_prefetch_status",
    ]
    print("\n### operation -> payload types")
    if reg is not None:
        try:
            items = reg.items()
        except Exception:
            items = [(getattr(s, "operation", i), s) for i, s in enumerate(reg)]
        for op, spec in items:
            opname = getattr(op, "value", op)
            if opname not in names:
                continue
            print(f"  {opname}:")
            print(f"    request : {[getattr(c, '__name__', str(c)) for c in getattr(spec, 'payload_types', [])]}")
            print(f"    response: {getattr(spec, 'response_type', None)}")
            sig = getattr(spec, "signature", None)
            if sig is not None:
                for pname, p in sig.parameters.items():
                    print(f"    param {pname}: {p.annotation}")
            for c in getattr(spec, "payload_types", []):
                if isinstance(c, type):
                    print(f"      {c.__name__}: array_like={shape(c)[0]} {shape(c)[1]}")

    print("\n### IPCCacheServerKey source")
    import inspect
    print(inspect.getsource(ct.IPCCacheServerKey)[:2600])
    print("\n### response / key structs")
    from lmcache.v1.multiprocess import custom_types as ct
    for n in [x for x in dir(ct) if not x.startswith("_")]:
        o = getattr(ct, n)
        if isinstance(o, type) and issubclass(o, msgspec.Struct):
            al, fs = shape(o)
            if fs is None:
                continue
            print(f"  {n}: array_like={al}")
            for fname, ftype in fs:
                print(f"      {fname}: {ftype}")

    print("\n### RequestUID")
    for modname in ("lmcache.v1.multiprocess.custom_types",
                    "lmcache.v1.multiprocess.rpc",
                    "lmcache.v1.multiprocess.transport.base"):
        try:
            mod = __import__(modname, fromlist=["x"])
        except Exception:
            continue
        uid = getattr(mod, "RequestUID", None)
        if uid is not None:
            print(f"  from {modname}: array_like={shape(uid)[0]} {shape(uid)[1]}")
            break


if __name__ == "__main__":
    main()
