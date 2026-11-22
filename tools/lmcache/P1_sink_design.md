# P1（已完成）：会话序列化层与文件解耦

目的：同一份 `SavedConversation` 序列化代码，既能写本地文件（现有 L3/SSD 层），也能交给网络层
（L4/LMCache），**格式 v1 的字节完全不变**。

## 1. 落地后的接口

`engine/include/strata/core/conversation_file.hpp`

```cpp
constexpr size_t kSessionBlock = size_t(16) << 20;   // 两端一次搬运的单位（文件层缓冲区的同一数值）

class SessionSink {
    virtual bool ok() const;                      // 起步所需的资源拿到了
    virtual std::string error() const;
    virtual SessionError kind() const;
    virtual bool begin(uint64_t total_bytes);      // 首字节之前告知流的精确长度
    virtual uint8_t* span(size_t& room);           // 当前块剩余空间，就地生产（K/V 从属主直读）
    virtual bool commit(size_t n);
    virtual bool write(const void* p, size_t n);
    virtual bool finish(bool durable);             // 尾块 + 落地（文件：flush+close；网络：等存储确认）
};

class SessionSource {
    virtual bool ok() const;
    virtual std::string error() const;
    virtual SessionError kind() const;
    virtual bool begin(uint64_t& total_bytes);     // 首读之前给出流长度
    virtual bool read(void* p, size_t n);          // 只向前，读满 n 字节或 false
};

bool session_stream_write(SessionSink&, const SavedConversation& image, const SessionFileIdentity&,
                          size_t& bytes, std::string& error, SessionStatus* = nullptr);
bool session_stream_write(SessionSink&, const SavedConversation& meta, const std::vector<SessionKvSource>&,
                          const SessionFileIdentity&, size_t& bytes, std::string& error, SessionStatus* = nullptr);
bool session_stream_read(SessionSource&, const SessionFileIdentity&, SavedConversation& image, size_t& bytes,
                         std::string& error, const SessionReadLimits& = {}, SessionStatus* = nullptr);
```

与原草案的两处修正（实现时发现的，均为必要）：

1. **加了 `begin()`**。`FileSink` 原本靠公开字段 `expected` 拿总长（进度上报和"块数"都要它），网络侧
   更需要：一个 publish 型 sink 要在**任何数据移动之前**算好要传多少、能不能传。所以总长上移到
   接口里，`FileSink`/`FileSource` 的 `expected` 改为私有、由 `begin()` 设置，语义反而更干净。
2. **两个 `session_stream_write` 重载**，与 `session_file_write` 对称。`SavedConversation` 有两种合法
   形态：K/V 在镜像里（`kv` 缓冲），或按 `SessionKvSource` 从属主缓冲区直读。单重载会在
   "镜像自带 K/V"这条最常走的路上被 `stream_length` 直接判为非法（"K/V given both in the image and as
   sources"），这是我在测试里第一次就踩到的。

## 2. 改动点（实际 diff）

| 位置 | 改动 |
|---|---|
| `conversation_file.hpp` | 新增两个接口类、`kSessionBlock`、两个流式读写函数；文件开头的说明补一句"流 == 文件" |
| `:83` `kBlock` | 改为 `= kSessionBlock`（单一数值来源） |
| `:528` `FileSink` | 继承 `SessionSink`，方法加 `override`，新增 `begin()`（设置 `expected` 并上报 0，原 `write_impl` 里那行搬进来）；逻辑一行未动 |
| `:589` `FileSource` | 继承 `SessionSource`，构造函数加 `uint64_t size`，`begin()` 返回它；`expected` → `expected_` 私有 |
| `:629` `struct Out` | `FileSink* f` → `SessionSink* f`（唯一改动） |
| `:711` `struct In` | `FileSource* f` → `SessionSource* f` |
| `write_impl` | 拆成 `stream_length()`（拒绝条件 + 定长）+ `stream_write()`（header/payload/trailer + finish）+ 薄封装（预检、临时名、rename、fsync） |
| `read_impl` | 拆成 `stream_read()`（长度→头哈希→身份→长度校验→RAM 预检→受限解析→payload 哈希→end 标记）+ 薄封装（不跟随链接地打开、单链接检查） |

行为差异只有一条，且无害：读一个**过短**的文件时，16 MiB 读缓冲现在会先分配再被拒（原来是先拒后分配）。

## 3. 验收证据

- `conversation_file_test`：**222 项通过**（原 201 项 + 21 项新增）。
- **字节不变**：测试里本来就有一道 golden 断言（`kGoldenSize=17878535` + 文件全字节 `session_hash64`），
  它同时通过 → 重构没有改动格式 v1 的任何一个字节。
- **流 == 文件**（新增用例）：同一个样本分别走 `session_file_write` 与 `session_stream_write`，
  逐字节 `memcmp` 相等；再从流读回，与原始镜像 `same()` 全字段一致。
- **K/V 源路径**（新增用例）：2 层 × 5 个 part（首 part 17 MiB，跨块），文件与流两条路字节相同，
  且每个源字节只被读一次（`read_bytes == sum(sizes)`）。
- **失败面**（新增用例）：流中翻一个 bit → payload 哈希拒绝；截断 1 字节 → 长度与头不符；
  换 config 指纹 → 身份拒绝，且**调用方的 image 保持不变**。
- `cpu-check` 全量：320 目标构建通过，`ctest` **141/141** 通过。

## 4. LMCache 侧两条实现（P2）

```cpp
class LmcacheSink : public SessionSink {
    // begin(total)  -> 算 token 数、建 namespace、定 key 区间（对齐 256）
    // span/commit   -> 16 MiB 一块，攒够就切 256-token 对象发出去
    // finish(true)  -> 尾块；全部成功后写清单（读者永远看不到"有清单无数据"）
};
class LmcacheSource : public SessionSource {
    // begin(total)  -> 长度来自清单
    // read(p, n)    -> 顺序读；块边界处向预取队列等待
};
```

`In::read` 只顺序读，所以 source 只需"当前块 + 预取队列"，预取深度 `STRATA_LMCACHE_STREAMS`（默认 8）。
