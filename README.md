# libbufs

Zero-copy binary messages for C++17, TypeScript, C# and Python, with a schema compiler. A message is built front to back
into pooled memory or straight into a shared-memory slot, read in place with generated
accessors, and verified before an untrusted one is read. Once warm, nothing on the message path
touches the malloc heap. Made for small request/reply messages between the processes of a
long-running service, and fine with large payloads too.

* `include/bufs/`: the header-only runtime (`bufs.hpp` for messages, `shm.hpp` for the
  shared-memory transport).
* `bufsc/`: `bufsc`, which compiles a FlatBuffers-style schema into one C++ header and, on request, the same messages
  for TypeScript, C# and Python.
* `ts/`, `cs/`, `python/`: the runtimes of those languages (see "Languages").
* `docs/design.md`: the wire format, layout and evolution rules, memory and transport design.
* `examples/`: the same program in every language (see "Languages").
* `tests/`, `benchmarks/`.

## Quick start

`telemetry.bufs`, a weather station sending batches of sensor readings:

```cpp
namespace demo;

enum Unit : ubyte { None, Celsius, Pascal, Percent }

struct Vec3 { x:float; y:float; z:float; }

table Reading {
    sensor:string;
    unit:Unit = None;
    value:double;
    position:Vec3;
    samples:[float];
    raw:[ubyte];
}

table Batch {
    sequence:ulong;
    station:string;
    readings:[Reading];
}

root_type Batch;
```

`bufsc -o build/gen telemetry.bufs` writes `build/gen/telemetry.bufs.hpp`. Then:

```cpp
#include "telemetry.bufs.hpp"

// Build: front to back, into a pooled block.
bufs::MessageBuilder mb;                          // bufs::BufferPool::global()
demo::Batch::Builder batch = mb.initRoot<demo::Batch>();
batch.setSequence(7);
batch.setStation("north-ridge");
auto readings = batch.initReadings(1);
demo::Reading::Builder reading = readings.init(0);
reading.setSensor("t1");
reading.setUnit(demo::Unit::Celsius);
reading.setValue(21.5);
reading.setPosition(demo::Vec3{1.0f, 2.0f, 0.5f});
reading.setSamples(samples.data(), samples.size());
reading.setRaw(frame.data(), frame.size());
bufs::Message message = mb.release();            // owns the block; message.data(), message.size()

// Read: verify what came from outside, then read in place.
if (bufs::typeOf(data, size) == demo::Batch::typeId && bufs::verify<demo::Batch>(data, size)) {
    demo::Batch::Reader b = bufs::root<demo::Batch>(data);
    for (demo::Reading::Reader r : b.readings()) {
        std::string_view sensor = r.sensor();        // points into the message
        const float* values = r.samples().data();   // contiguous, no copy
        const demo::Vec3& position = r.position();
    }
}
```

Messages must be 8-byte aligned where they are verified and read; pool blocks and ring slots
are. Copy a message from a socket into an aligned buffer (a pool block) first.

## Schema language

A subset of the FlatBuffers schema language; a schema bufsc accepts is also a valid FlatBuffers
schema.

| Construct | Notes |
|---|---|
| `namespace a.b;` | C++ `namespace a::b`. |
| `include "other.bufs";` | Searched next to the including file, then in `-I` directories. Generates `#include "other.bufs.hpp"`. |
| `enum E : ubyte { A, B = 4, C }` | Integer underlying type. `enum class` in C++, plus `enumName(E)`. |
| `struct S { x:float; v:[float:128]; }` | Fixed layout: scalars, enums, structs, fixed arrays. No defaults, no evolution. |
| `table T { f:int = 5; s:string; v:[float]; t:T; old:int (deprecated); }` | Scalars (with defaults), enums, structs, strings, tables, vectors of all of those. |
| `root_type T;` | Optional. Any table or struct can be a message root. |
| `///` comments | Copied into the generated header. |
| `attribute`, `file_identifier`, `file_extension` | Accepted and ignored. |
| `rpc_service S { M(Req):Resp (streaming: "server"); }` | Parsed and checked (both types must be tables), then ignored by `bufsc`. `rpcc` in librpc turns services into C++, TypeScript, C# and Python clients (and C++ servers). |

Scalar types: `bool byte ubyte short ushort int uint long ulong float double` and the sized
spellings (`int8` ... `uint64`, `float32`, `float64`). Float defaults may be `nan`, `inf`,
`-inf`. Not supported: unions (dispatch on the message's type id instead), vectors of vectors,
optional scalars (`= null`; use a sentinel default), JSON.

## Generated C++

For `table T` in namespace `ns`:

| | |
|---|---|
| `ns::T::typeId`, `ns::T::typeName` | The type id in the message header. |
| `ns::T::Reader` | `x()` for every field: scalars by value, structs as `const S&`, strings as `std::string_view`, vectors as `bufs::Vector<E>` (`size()`, `operator[]`, `data()`, range-for), `bufs::BoolVector`, `bufs::StringVector`, `bufs::TableVector<U::Reader>`, tables as `U::Reader`. `hasX()` for strings, vectors and tables. A null `Reader` returns defaults. |
| `ns::T::Builder` | `setX(value)` for scalars, enums, structs and strings; `setX(values, count)` and `initX(count)` (a `bufs::ListBuilder<E>`, zeroed) and `initX(count, bufs::uninitialized)` (not zeroed: **write every element**, or pooled memory leaks into the message) for vectors of scalars and structs; `initX(count)` returning `bufs::StringListBuilder` or `bufs::TableListBuilder<U::Builder>` (`init(i)`) for vectors of strings and tables; `initX()` for a table field. |
| `ns::T::verifyTable` | Used by `bufs::verify<ns::T>()`. |

Field names become lowerCamelCase: `sample_rate` gives `sampleRate()`, `setSampleRate()`,
`hasSampleRate()`. A name that is a C++ keyword gets a trailing
underscore (`class` gives `class_()`, enum value `new` gives `E::new_`); bufsc rejects two fields
whose accessors would have the same name. A struct `S` becomes a plain C++ struct with the same
fields (`std::array` for fixed arrays, `uint8_t` for `bool`), and `bufs::StructInfo<S>::typeId`
for struct roots.

Runtime calls:

| | |
|---|---|
| `bufs::MessageBuilder mb(pool)` / `mb(memory, capacity)` | Pooled (grows) or fixed (throws `bufs::CapacityError` when full). |
| `mb.initRoot<T>()`, `mb.initRootStruct<S>()` | Starts a message. |
| `mb.finish()` → `{data, size}` | Seals it; the builder still owns the bytes. |
| `mb.release()` → `bufs::Message` | Seals a pooled message and hands its block over. |
| `mb.clear()` | Starts over in the same block. |
| `bufs::verify<T>(data, size)`, `bufs::verifyStruct<S>(data, size)` | Check an untrusted message. |
| `bufs::root<T>(data)`, `bufs::rootStruct<S>(data)` | Read a verified or self-built message. |
| `bufs::typeOf(data, size)` | The header's type id, 0 when too short. |
| `bufs::MessageStream(data, size)`, `bufs::verifyStream<T>(data, size)` | Walk or verify messages laid end to end (each starts with its own size). `next()` throws on a bad size. |
| `bufs::vectorBytes()`, `stringBytes()`, `tableBytes()` | Upper bounds on what an object adds to a message, to size a block so a builder never grows. |
| `bufs::BufferPool` | `global()`, `acquire()`, `stats()`, `trim()`. |

## Languages

One schema, one wire format, four languages. `bufsc` always writes the C++ header; add a flag for the others (`--no-cpp`
leaves the C++ out):

| Language | Flag | Writes | Runtime | Reads | Builds with |
|---|---|---|---|---|---|
| C++17 | (default) | `<stem>.bufs.hpp` | [include/bufs](include/bufs) (header-only) | in place, with a verifier | `Message`/`MessageBuilder`, pooled or shared memory |
| TypeScript | `--ts` | `<stem>.bufs.ts` | [ts/src/bufs.ts](ts/src/bufs.ts) (module `@bufs/runtime`; `--runtime MODULE` renames it) | lazy getters over a `DataView` | `T.encode({...})` |
| C# (.NET; built for net10.0) | `--cs` | `<stem>.bufs.cs` | [cs/Bufs](cs/Bufs) (project `Bufs.csproj`, namespace `Bufs`) | `readonly struct` readers over `ReadOnlyMemory<byte>`, scalar vectors as `ReadOnlySpan<T>` | `T.Encode(new TInit {...})` |
| Python 3 (tested on 3.12) | `--py` | `<stem>_bufs.py` | [python/bufs](python/bufs) (package `bufs`, no dependencies) | properties over a `memoryview` | `T.encode({...})` or keywords |

Every language reads in place and checks bounds as it reads (a malformed message raises instead of reading outside itself); only
C++ also has the full `verify<T>()` pass. All of them implement the same rules: scalars stored XOR their default, so a field
a writer left out reads as its default and new fields can be added at the end of a table, and each produces the C++ builder's
bytes exactly (`tests/languages` checks this byte for byte, both directions, on a schema with every field type).

```
bufsc --ts --cs --py -o gen telemetry.bufs         # telemetry.bufs.hpp, telemetry.bufs.ts, telemetry.bufs.cs, telemetry_bufs.py
bufsc --ts --no-cpp -o gen telemetry.bufs          # TypeScript only
```

```cmake
bufs_generate(app SCHEMAS telemetry.bufs TS CS PY)  # the flags of bufsc, as CMake options
```

How a schema maps to each language:

| Schema | TypeScript | C# | Python |
|---|---|---|---|
| `enum E : ubyte` | `enum E` | `enum E : byte` | `class E(IntEnum)` |
| `struct S` | `interface S` | `struct S` | `class S(Struct)` with keyword init |
| `table T` | `class T` (reader), `TInit` | `readonly struct T` (reader), `class TInit` | `class T` (reader), init as a dict |
| 64-bit integer | `bigint` | `long`, `ulong` | `int` |
| `string`, `[string]` | `string`, `string[]` | `string`, `string[]` | `str`, `list[str]` |
| `[ubyte]` | `Uint8Array` | `ReadOnlySpan<byte>` | `bytes` |
| other `[scalar]`, `[struct]`, `[Table]` | arrays | `ReadOnlySpan<T>` or arrays | lists |
| a missing table | `null` | `null` | `None` |

```python
from telemetry_bufs import Batch, Unit            # bufsc --py
data = Batch.encode(station="alpha", readings=[dict(unit=Unit.Celsius, value=21.5)])
batch = Batch.decode(data)                         # raises MalformedMessage for bytes that are not a Batch
print(batch.station, [r.value for r in batch.readings])
```

[examples/](examples) has one small program per language, [team.bufs](examples/team.bufs) with `cpp/`, `python/`, `ts/` and
`cs/People/`: each writes the message and reads it back, and `ctest -R bufs_examples` has every language read what every other one
wrote. The tools needed are optional: a language whose tools are missing (`node` and `esbuild`, `dotnet`, `python3`) is skipped.

## Shared memory

```cpp
#include "bufs/shm.hpp"

// Process A
bufs::shm::Channel channel = bufs::shm::Channel::create("/myapp-1", 256 << 10);
uint8_t* slot = channel.send().reserve(4096);           // nullptr: the ring is full
bufs::MessageBuilder mb(slot, 4096);                     // built in the shared memory
mb.initRoot<demo::Batch>().setSequence(7);
channel.send().commit(mb.finish().second);

// Process B
bufs::shm::Channel channel = bufs::shm::Channel::open("/myapp-1");
channel.receive().wait(std::chrono::seconds(1));        // futex, across processes
const uint8_t* data; size_t size;
if (channel.receive().front(data, size)) {
    // read in place (or copy and verify when the producer is not trusted)
    channel.receive().pop();
}
```

A ring carries messages up to half its capacity; send larger ones some other way. See
`docs/design.md` section 6 for the ring protocol and the trust model.

Without names in `/dev/shm`, the channel is anonymous and goes to the other process with a
message on a local socket (`bufs/local.hpp`), which then also carries the doorbells:

```cpp
#include "bufs/local.hpp"

// Process B (server)
bufs::local::Socket listener = bufs::local::Socket::listen("/tmp/myapp.sock");
bufs::local::Socket peer = listener.accept();            // non-blocking: empty when none waits

// Process A (client)
bufs::local::Socket socket = bufs::local::Socket::connect("/tmp/myapp.sock");
bufs::shm::Channel channel = bufs::shm::Channel::createAnonymous("myapp", 256 << 10);
const int fd = channel.fd();
socket.send(hello, helloSize, &fd, 1);                     // a first message, with the channel
// ... build in channel.send() as above, then:
socket.sendDoorbell();

// Process B
alignas(8) uint8_t inbox[4096];
bufs::local::ReceivedFds fds;
long size = peer.receive(inbox, sizeof inbox, fds);       // the first message, 1 descriptor
bufs::shm::Channel theirs = bufs::shm::Channel::adopt(fds.take(0));
size = peer.receive(inbox, sizeof inbox, fds);            // 1: a doorbell; 0: A has gone
```

`bufs::shm::Memory::create()` / `adopt()` share any other block (frames, images) the same way:
sealed against resizing, so the receiver can map it without the sender being able to shrink it
under the mapping.

## Performance

Measured against FlatBuffers 25.12.19 and a JSON DOM baseline on request/report messages of the
sizes this library is made for. The host was a Xeon Gold 5420+ with GCC 13.3, on 2026-10-22. The
full tables and the method are in `benchmarks/README.md`. Times are ns per message.

| | libbufs | FlatBuffers | JSON |
|---|---|---|---|
| Request (12 scalars + a name): size / build / verify + read | 96 B / 38 / 8.3 | 120 B / 82 / 45 | 284 B / 1,500 / 1,954 |
| Report, 1 item (strings, scalars, 128 features, part boxes): size / build / verify + read | 864 B / 152 / 95 | 920 B / 221 / 170 | 4,064 B / 24,913 / 17,806 |
| Report, 4 items with an 8 KiB blob each: size / build / verify + read | 36,128 B / 1,334 / 366 | 36,224 B / 1,726 / 591 | 59,757 B / 256,168 / 225,646 |
| Struct root (48-byte frame): build / verify + read | 19.1 / 3.0 | 22.3 / 9.4 | 1,210 / 1,486 |
| Heap allocations per message after warm-up (build, verify, read) | 0 | 0 to 6 (a new builder allocates) | 1 to 983 |
| Long run, 5 M mixed messages with 1-4 MiB outliers: throughput, RSS growth after warm-up | 1.83 M/s, +4.2 MiB | 1.06 M/s, +1.9 MiB | 0.01 M/s, +5.8 MiB |
| 8 threads, each freeing what another built | 6.6 M msg/s | 3.0 M msg/s | 0.05 M msg/s |
| 8 threads, independent, one shared pool | 9.8 M msg/s | 8.2 M msg/s | 0.05 M msg/s |

The 864-byte report between two processes, over each transport:

| Transport | Round trip, p50 | One-way stream |
|---|---|---|
| Shared-memory ring, futex wait | 18.0 us | 1.33 M msg/s |
| Shared-memory ring, `wait(timeout, 20us)` (spin, then futex) | 1.41 us | 1.64 M msg/s |
| Unix socket | 17.7 us | 0.70 M msg/s |
| Loopback TCP | 25.1 us | 0.25 M msg/s |

Where it is not ahead:
* A new builder for a tiny message costs about as much as FlatBuffers (60 ns against 39-118 ns),
  because the pool's lock costs more than glibc's per-thread cache. Reuse the builder with
  `clear()` (38 ns).
* While outliers keep coming, their idle blocks stay resident (+2.3 MiB more RSS growth than
  FlatBuffers here). They give their pages back 250 ms after the outliers stop.

## Building

```bash
cmake -S libbufs -B build && cmake --build build -j && (cd build && ctest)
```

From another CMake project:

```cmake
add_subdirectory(path/to/libbufs libbufs)
add_executable(app main.cpp)
bufs_generate(app SCHEMAS telemetry.bufs [INCLUDE_DIRS dir...] [OUTPUT_DIR dir])  # runs bufsc, adds the include dir
```

The compiler is the CMake target `bufsc`. When cross-compiling, build `bufsc` for the host and
pass `-DBUFSC_EXECUTABLE=/path/to/bufsc`.

```
bufsc [-o DIR] [-I DIR]... SCHEMA.bufs...     write DIR/<stem>.bufs.hpp (every header flat in DIR)
bufsc ... --depfile FILE SCHEMA.bufs          also list every schema read, for the build system
bufsc ... --ts --cs --py [--no-cpp]            also (or only) write the messages for TypeScript, C# and Python
bufsc --check OLD.bufs NEW.bufs                list changes that break old or new readers; exit 1 if any
bufsc --layout SCHEMA.bufs                    print every table's field offsets and type ids
```

## Tests

`ctest -R bufs`:

* `bufs_round_trip`: every field type round trip, defaults, older-schema tables, fixed buffers
  and `CapacityError`, growth, a 64 MiB message, struct roots, every truncation and 20,000
  random corruptions through the verifier, and zero heap allocations and zero new mappings over
  10,000 build + verify + read cycles.
* `bufs_shm`: region lifetime, a full ring, oversize messages, corrupted ring state, and 200,000
  messages built in place by a producer process and read in place by a consumer process across
  wrap-arounds with futex waits.
* `bufs_local`: sealed memory and its refusal of unsealed or oversized descriptors, a socket
  file left by a dead process, and a channel handed to another process over a local socket and
  driven by doorbells, with replies too large for it sent inline.
* `bufs_evolution`: two schema versions each read the other's messages.
* `bufs_languages`: C++ writes the reference messages of `tests/languages/kitchen.bufs` (every field type, with and without defaults);
  TypeScript, C# and Python read them, check every value, encode the same values and write them back, and C++ verifies and reads
  those; each encoding is also compared to C++'s bytes.
* `bufs_examples`: the example programs of every language, each reading what every one wrote.
* `bufs_check_*`, `bufs_reject_*`: `bufsc --check` and schemas bufsc must refuse.
* `tests/schemas/tricky.bufs` (in `bufs_round_trip`): keywords as names, a struct declared after
  its user, struct bools, a table shadowing an outer enum, float defaults such as `NaN` and
  `1e-9`, and a message of many tables sharing one vector of null references, which the
  verifier must reject quickly.
