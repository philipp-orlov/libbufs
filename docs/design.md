# libbufs design

libbufs is a binary message format with a schema compiler (`bufsc`) and a header-only C++17
runtime. It plays the role FlatBuffers plays (schema, generated accessors, zero-copy reads), with
three goals:

1. **No heap traffic.** Building, verifying and reading a message allocate nothing from the
   malloc heap once a process is warm, so memory stays flat over weeks and nothing fragments.
2. **Small messages first.** Requests and replies of tens of bytes to tens of kilobytes, the
   usual traffic between the processes of one service, cost as little as possible: small headers,
   32-bit offsets, no vtables, no back-patching. Large payloads (up to 4 GiB per message) still work.
3. **Shared memory as the transport.** A message is built in place in a shared-memory ring slot
   and read in place by the other process: no serialization step and no copy on either side.

## 1. Wire format

Little-endian. A message is 8-byte aligned and its size is a multiple of 8.

```
message  = header root other-objects...
header   = u32 size        whole message, bytes
           u32 typeId      FNV-1a (32-bit) of the root type's qualified name, "ns.Type"
```

The root object starts at byte 8. A table's type id comes from its name, so renaming a table or
moving it to another namespace changes its id; `bufsc` refuses a schema in which two ids collide.

### Tables

```
table    = u16 dataBytes   header + data section, bytes (>= 4)
           u16 refCount
           data section    dataBytes - 4 bytes of fields at fixed offsets
           pad to 4
           refCount x u32  references
```

* A table starts 8-aligned. Offsets inside the data section count from the table's first byte,
  so the 4-byte header is the start of the data section and an 8-byte field sits at offset 8.
* **Placement.** `bufsc` walks the fields in declaration order. Each data field takes the lowest
  free position aligned to its own size: bools are single bits, scalars 1/2/4/8 bytes, inline
  structs their size at their alignment. A field added later only fills a hole or extends the
  section, so no existing field ever moves. Reference fields (strings, vectors, tables) take the
  next reference slot.
* **Defaults.** A scalar is stored as its bits XOR the bits of its schema default. A zeroed table
  therefore reads as all defaults, and setting a field to its default writes zeros.
* **Missing fields.** A field whose bytes lie past `dataBytes`, or a reference at or past
  `refCount`, was not in the writer's schema: it reads as its default, an empty string or vector,
  or a null table. A reader uses the message's `dataBytes`/`refCount`, not its own, to find the
  reference section, so it skips fields it does not know.
* **References.** A u32 offset from the slot's own address to the object, always forward.
  0 is null. Because every reference points forward, a message is a DAG with no cycles.

### Vectors and strings

```
vector   = u32 count, then count elements
string   = u32 length, bytes, NUL
```

* Elements are scalars, enums (as their integer type), structs (at their size), bools (one byte
  each), or u32 references (strings and tables; each relative to its own slot). Vectors of
  vectors are not supported; wrap the inner one in a table.
* A vector whose elements need 8-byte alignment starts at 4 mod 8, so its elements are aligned.
  Every object starts 4-aligned.
* The NUL after a string makes `data()` usable as a C string; it is not counted in `length`.

### Structs

A struct holds scalars, enums, structs and fixed arrays (`[float:128]`) only. It is laid out
like C: declaration order, natural alignment (at most 8), size rounded to the alignment. The
generated C++ struct has the same layout with the padding spelled out, and `static_assert`s
check every offset and the size. Structs cannot evolve; put fields that may change in a table.
A struct can be a table field (inline in the data section), a vector element, or a message root.
A `bool` in a struct is one byte, and its generated C++ member is a `uint8_t` (non-zero is true):
readers hand out references into the message, and a `bool` holding a byte other than 0 or 1
would be undefined behavior.

## 2. Evolution

Writers and readers on different schema versions interoperate when the newer schema only:

* adds fields at the end of a table;
* marks fields `(deprecated)` (the slot stays; no accessor is generated);
* adds enum values;
* adds tables, structs and enums.

Renaming a field is allowed: names are not on the wire. Everything else changes the meaning of
existing bytes: changing a field's type, reordering fields, changing a default (values are stored
XOR the default), changing a struct, renaming a table (its type id). `bufsc --check old.bufs new.bufs`
lists every such change and exits 1. Run it in CI against the last released schema.

## 3. Building

* `MessageBuilder` appends objects at the end of one contiguous block. Allocation zeroes the
  object and any alignment padding before it, so pooled memory never leaks into a message.
* Builder handles (`T::Builder`, `ListBuilder`, ...) hold offsets, not pointers. A pooled block
  that fills up is replaced by a larger one (one `memcpy`), and the handles stay valid. A raw
  pointer from `ListBuilder::data()` is valid until the next allocation in the same message.
* Each root type remembers the size of its largest message so far, capped at 64 KiB, and the
  next builder of that type starts with a block that size. After warm-up a small message never
  grows; one outlier does not make later messages start with a huge block.
* A builder over caller memory (`MessageBuilder(memory, capacity)`), such as a ring slot, never
  moves and throws `CapacityError` when the message does not fit.
* Setting a string, vector or table field twice leaves the first object in the message unused.
  Set each field once.

## 4. Reading and verifying

* `bufs::root<T>(data)` returns a `Reader`: a pointer to the table. Accessors compute an address
  and load; strings are `std::string_view`, vectors of scalars and structs are contiguous views
  with `data()`, vectors of tables and strings yield views. Nothing is copied or allocated.
* A `Reader` on a null table is valid: every accessor returns the default.
* Read only messages this process built, or that `bufs::verify<T>(data, size)` accepted. After
  verification every accessor of `T` stays inside the message: object bounds, element counts
  (overflow-checked), string NUL terminators, alignment of tables (8) and 8-byte elements, and
  forward 4-aligned references are all checked. Depth and object count are bounded
  (`VerifyOptions`): every table, vector and reference-vector element counts, null or not, so a
  small message that shares objects cannot make verification expensive.
* The verifier does not check enum values: an unknown value (from a newer writer) reads as its
  number.
* `bufs::typeOf(data, size)` reads the header's type id: the way to dispatch on a received
  message, in place of FlatBuffers unions.

## 5. Memory

`BufferPool` hands out power-of-two blocks from 256 B to 64 MiB, 64-byte aligned, and never uses
the malloc heap for them.

| Class | Source | Kept |
|---|---|---|
| 256 B .. 64 KiB | carved from 256 KiB slabs that hold one class each | for the life of the pool; bounded by the peak number of blocks of that class in use at once |
| 128 KiB .. 64 MiB | one `mmap` per block | up to 16 idle blocks per class and 64 MiB in total (`Options`); a block idle for 250 ms gives its pages back (`MADV_DONTNEED`) and keeps only its address range; more are unmapped at once; `trim()` unmaps all idle ones |
| above 64 MiB | one `mmap` per use | never |

A slab only ever holds blocks of one size, so a free block is always exactly the size of the next
request for its class: the pool cannot fragment. Its footprint is the high-water mark of messages
in flight, not the history of the process, and an outlier message leaves no resident memory
behind.

Every class is split into 16 shards, each with its own mutex, free list and in-use count on its
own cache line; a thread uses one shard (assigned round robin). When a shard runs dry it takes up
to 32 idle blocks from another in one go, so a pattern where one thread builds and another frees
(a worker building replies that the I/O thread sends and frees) moves blocks in batches, not one
lock round trip per message. After warm-up, `acquire()`/`release()` take one uncontended mutex
per call and make no system call (except `madvise` when an outlier of 1 MiB or more is released).

The rule for a long-running process: messages come from the pool (or from a ring slot), a
`Message` returns its block when it is destroyed, and nothing else allocates on the message path.
`tests/test_bufs.cpp` checks that 10,000 build + verify + read cycles allocate nothing and map
nothing new.

## 6. Shared-memory transport

### Memory

Anonymous shared memory: a `memfd` sealed with `F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL`, so
neither process can resize it. `Memory::create()` makes one; its descriptor goes to the other
process over a local socket (below), where `Memory::adopt()` maps it after checking the seals and
an upper bound on its size. Reading past the end of a shared file that its owner has shrunk kills
the reader with `SIGBUS`; the seals rule that out, which is why `adopt()` refuses unsealed memory.
Nothing has a name: there is nothing in `/dev/shm` to confine, guess or leak, and the kernel frees
the memory when the last process closes it.

### Region

A block with a 64-byte header (magic, total size): anonymous (`createAnonymous()` / `adopt()`,
on `Memory`), or a named POSIX shared-memory object (`create()` / `open()`, `shm_open`), which the
creator unlinks on destruction. Both check the header against the size before use.

### Ring

A single-producer, single-consumer queue of variable-size messages in any shared memory:

```
control  256 bytes: magic and capacity | writePos, writeSeq | readPos, readSeq | waiting flags
data     capacity bytes (a power of two)
record   = u32 length, u32 kind (1 message, 2 padding), payload padded to 8
```

* Positions are 64-bit counters that never wrap; `pos & (capacity - 1)` is the byte index. The
  producer owns `writePos`, the consumer `readPos`; each sits on its own cache line.
* A message is always contiguous. When it does not fit before the end of the data area, the
  producer writes a padding record over the rest and starts at 0. So the largest message is half
  the capacity (less 8 bytes), and `reserve()` throws `CapacityError` above that.
* `reserve(n)` returns an 8-aligned slot (or nullptr when the ring is full), a `MessageBuilder`
  builds the message in it, and `commit(size)` publishes it with one release store. The consumer's
  `front()` returns the message in place; `pop()` frees it.
* Waiting: `wait(timeout, spin)` (consumer) and `reserve(n, timeout, spin)` (producer) poll for up
  to `spin` (default 0), then sleep on a futex word in the shared memory (not `FUTEX_PRIVATE`, so
  it works across processes), with a waiting flag so the other side calls `FUTEX_WAKE` only when
  someone sleeps. A futex wake-up across processes costs about as much as a unix socket round
  trip; a spin of tens of microseconds is an order of magnitude faster when the peer answers
  within it, at the cost of a busy core while polling. Leave it 0 on core-starved devices.

### Channel

One region holding two rings: the creator sends on the first and receives on the second, the
process that opens (or adopts) it the other way round. One channel per connection between two
processes; `createAnonymous()` and `adopt()` make it anonymous.

### Local socket

`bufs/local.hpp`: a `SOCK_SEQPACKET` Unix socket, which keeps message boundaries and order, for
the control side. It carries file descriptors with a message (`SCM_RIGHTS`, at most 4), which is
how `Memory` and anonymous channels change hands, and `kDoorbell`, a one-byte message meaning "a
message waits in the channel". A doorbell cannot be an empty message: on a `SOCK_SEQPACKET` socket
an empty message reads exactly like the peer closing. The socket is also what tells each side
that the other has gone, so a server can drop everything a dead client shared. `listen()`
replaces a socket file a dead process left behind and refuses one a live process still listens
on.

### Trust

The other process can write anything into the shared memory at any time. The consumer reads
`writePos` once, checks that it is ahead of `readPos` by at most the capacity, and checks every
record header (kind, length within the data area and within the published range) before using
it; a violation throws `CorruptRing`. The message bytes themselves can still change while they
are read, so a consumer that does not trust its producer copies the message out of the ring (it
is small) and verifies the copy. A consumer that trusts its producer reads in place.

### When the reply does not fit

A ring is sized for the common case. A message larger than half the ring, or one that arrives
while the ring is full, goes by another path (for example inline on the socket the two processes
already share); the format is the same either way.

## 7. Limits

* A message is at most 4 GiB less 8 bytes (u32 offsets and size).
* A table's data section is at most 65,535 bytes and it has at most 65,535 references.
* Struct alignment is at most 8; fixed arrays hold at most 65,535 elements.
* Little-endian targets only (x86-64, ARM64). The shared-memory transport is Linux only (futex).
* No unions, no vectors of vectors, no JSON, no reflection, no optional (`= null`) scalars: use a
  sentinel default (e.g. `-1` for "use the server's value").
