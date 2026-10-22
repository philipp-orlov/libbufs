# libbufs benchmarks

libbufs against FlatBuffers and against a JSON baseline, on a small set of typical messages (a
request, a report of items with feature vectors and optional blobs, a fixed-size struct, a vector
of structs); and the shared-memory ring against sockets.

* `bench.bufs`: the messages, written in the subset both `bufsc` and `flatc` accept. Both formats
  are generated from this one file, so they carry the same fields.
* `bufs_bench` (`bench_main.cpp`, one `bench_<format>.cpp` per format): sizes, build / verify /
  read times, heap allocations, a long-run memory test and a multi-threaded test.
* `bufs_bench_shm` (`bench_shm.cpp`): round trips and one-way streams between two processes
  over `bufs::shm::Channel`, a unix socket and loopback TCP.

## Messages

| case | content |
|---|---|
| request | `Query`: 12 scalars (u64 id, ints, bool, floats, enum) and the `source` shm name |
| report 1 item / 4 items | `Report` with 1 or 4 `Item`s: `label` (7-8 chars), `category`, `subcategory`, 12 scalars, `part_scores` [float] (8), `part_boxes` [Box] (8), `part_rows` [ubyte] (8), `features` [float] (128), `bounds` |
| report 1 / 4 + 8K blob | the same with an 8 KiB `blob` [ubyte] per item |
| frame (struct) | `FrameReady`, a 48-byte struct. libbufs sends it as a struct root; FlatBuffers, whose root must be a table, as a table holding the struct |
| 16 detections | `DetectionList`: a u64 and a vector of 16 `Detection` structs |

The JSON rows are a baseline: they carry the same values under camelCase versions of the schema's
field names, with blobs as base64 data URIs. They build a libhttp `json::Json` tree and `dump()`
it; `json (libhttp)` parses with libhttp's `Json::parse`, `json/jsonlite` with `JsonLite`, a
minimal standalone JSON DOM parser kept in `baseline/` (sharing the libhttp encoder row). Both
need the surrounding tree to have `external/libhttp`.

## What is measured

* **Build**: one builder reused (`MessageBuilder::clear()`, `FlatBufferBuilder::Clear()`, a
  recycled `std::string` for `dump`), and a new builder per message with the finished message
  kept until the next (`MessageBuilder(pool)` + `release()`, `FlatBufferBuilder` + `Release()`,
  `dump()`).
* **Verify**: `bufs::verify<T>` / `verifyStruct<S>`, `flatbuffers::Verifier::VerifyBuffer<T>`,
  JSON parse.
* **Read**: every field, every vector element (all 128 feature floats); strings by size and
  first byte; the blob by size and first/last byte for the zero-copy formats, while JSON has to
  base64-decode it. **Verify + read** together.
* Every cell is ns per message: the median over batches of ~20 ms (`--quick`: 10 ms) with the
  fastest and slowest batch in parentheses.
* **Heap**: malloc, calloc, realloc and the memalign family are interposed and `operator new`
  replaced (`bench_alloc.cpp`), so every heap allocation counts, counted after a warm-up.
  libbufs' pool maps its memory with `mmap`, which the long-run notes report instead.
* **Long run**: one process per format builds a random mix (requests, 0-4 item reports with
  1-16 KiB blobs on 30% of items, 0-64 detections, frames, and about one 1-4 MiB blob report
  per 2000) into a FIFO of 8 live messages, verifying, reading and freeing each as its slot comes
  round; a new builder per message, as a server handing messages to a socket would. RSS
  (`/proc/self/statm`) at start, after 1% (warm), at half, at the end, after draining, and the
  peak (`VmHWM`). 5 M cycles (`--cycles`); JSON runs a tenth of that.
* **Multi-threaded**: `--threads` (8) threads each running the long-run mix, either
  independently or handing every message to the next thread, which verifies, reads and frees it.
  libbufs runs with one pool shared by all threads (as `BufferPool::global()` is) and with a
  pool per thread; FlatBuffers with its default allocator (glibc malloc).
* **Shared memory**: `bufs_bench_shm` forks a server. Ping-pong: the client builds a request
  straight into its send slot (`Ring::reserve` + `MessageBuilder(slot, size)`), the server
  verifies and reads it and builds the report into its own send slot; the client verifies and reads
  the report. Round-trip percentiles and round trips per second. The ring is waited on with
  `Ring::wait` (futex), with 20 µs of spinning before `Ring::wait`, or by spinning only. The
  socket rows send the same bytes (built the same way into a staging buffer) over a unix
  socketpair and over loopback TCP with `TCP_NODELAY`. Stream: the server sends reports one way
  as fast as the client verifies and reads them.

## Building

FlatBuffers is not a dependency of this repository. Fetch and build it in a scratch directory
outside the tree:

```
mkdir -p /tmp/bufs-bench && cd /tmp/bufs-bench
git clone --depth 1 --branch v25.12.19 https://github.com/google/flatbuffers.git
cmake -S flatbuffers -B flatbuffers/build -DCMAKE_BUILD_TYPE=Release -DFLATBUFFERS_BUILD_TESTS=OFF
cmake --build flatbuffers/build -j8 --target flatc
```

Then, from the root of the surrounding tree, either libbufs alone (the JSON rows compile
libhttp's `json.cpp` and `base64.cpp` from `external/libhttp` and `JsonLite.cpp` from
`baseline/`):

```
cmake -S libbufs -B build-bench -DCMAKE_BUILD_TYPE=Release -DBUFS_BUILD_TESTS=OFF \
      -DBUFS_BUILD_BENCHMARKS=ON -DFLATBUFFERS_DIR=/tmp/bufs-bench/flatbuffers
cmake --build build-bench -j8 --target bufs_bench bufs_bench_shm
```

or the whole tree (`cmake -S . -B build -DBUFS_BUILD_BENCHMARKS=ON -DFLATBUFFERS_DIR=...`; the
programs land in `build/bin/`). `FLATBUFFERS_DIR` must hold `include/` and `build/flatc`
(`-DFLATBUFFERS_FLATC=` names another flatc). Without it the FlatBuffers rows are left out; without
the libhttp sources the JSON rows are.

## Running

Pin the programs: single-threaded parts run on the first CPU `taskset` allows, the
multi-threaded part on the first `--threads`, and `bufs_bench_shm` puts the client on the first
and the server on the second. Prefer distinct physical cores (on the 56-vCPU Xeon, hyperthread
siblings are adjacent numbers, so use even ones):

```
taskset -c 28,30,32,34,36,38,40,42 build-bench/benchmarks/bufs_bench
taskset -c 28,30 build-bench/benchmarks/bufs_bench_shm
```

`--quick` runs everything in well under a minute with fewer batches and cycles. `bufs_bench`
also takes `--only sizes,speed,heap,longrun,mt`, `--formats libbufs,flatbuffers,json`,
`--cycles N`, `--threads N`, `--mt-cycles N`, `--batches N`, `--batch-ms N`;
`bufs_bench_shm` takes `--iterations N` and `--only pingpong|stream`. Run each a few times on a
quiet host and compare runs: the in-run spread only covers batch-to-batch noise.

## Sample output

One run each on a development host (2x Xeon Gold 5420+, 56 vCPUs, Hyper-V; load average about
1.5-2), 2026-10-22, GCC 13.3 `-O3`, FlatBuffers 25.12.19, `taskset -c 28,30,32,34,36,38,40,42`
and `taskset -c 28,30`. Earlier runs agreed within a few percent.

```
bufs_bench  2026-10-22 05:14 UTC
host: Intel(R) Xeon(R) Gold 5420+, 56 online CPUs, Linux 6.8.0-147-generic; compiler: 13.3.0
allowed CPUs: 28 30 32 34 36 38 40 42  (single-threaded parts pinned to CPU 28)
FlatBuffers 25.12.19
mode: full, 15 batches of ~20 ms per cell; cells are median (min-max) over batches

Encoded size, bytes
format                          request          report 1 item         report 4 items     report 1 + 8K blob     report 4 + 8K blob         frame (struct)          16 detections
libbufs                              96                    864                   3344                   9064                  36128                     56                    416
flatbuffers                         120                    920                   3424                   9128                  36224                     64                    424
json (libhttp)                      284                   4064                  15873                  15035                  59757                    230                   2263
json/jsonlite                       284                   4064                  15873                  15035                  59757                    230                   2263

Build, ns/message, one builder reused (bufs: clear(); flatbuffers: Clear(); json: tree + dump into a recycled string)
format                          request          report 1 item         report 4 items     report 1 + 8K blob     report 4 + 8K blob         frame (struct)          16 detections
libbufs                38.2 (37.8-38.5)          152 (150-153)          505 (493-701)          210 (206-221)       1334 (1324-1359)       19.1 (18.9-19.5)       37.6 (37.3-38.2)
flatbuffers            82.4 (79.8-90.1)          221 (213-290)          776 (767-826)          280 (276-286)       1726 (1703-1737)       22.3 (22.2-22.6)       33.6 (33.4-34.2)
json (libhttp)         1500 (1493-1515)    24913 (24664-32336) 103892 (103423-106566)    63751 (63344-64486) 256168 (250418-260258)       1210 (1205-1288)    14845 (14690-15152)

Build, ns/message, a new builder per message, finished message kept (bufs: MessageBuilder(pool) + release(); flatbuffers: FlatBufferBuilder + Release(); json: tree + dump())
format                          request          report 1 item         report 4 items     report 1 + 8K blob     report 4 + 8K blob         frame (struct)          16 detections
libbufs                60.2 (60.0-60.6)          181 (177-184)          535 (532-541)          247 (245-248)       1377 (1372-1404)       41.5 (41.2-42.3)       58.2 (57.9-58.9)
flatbuffers               118 (116-119)          252 (248-260)       1085 (1080-1116)          403 (400-410)       3589 (3575-3599)       39.1 (38.7-39.9)       58.0 (55.3-59.4)
json (libhttp)         1561 (1544-1573)    24884 (24801-25010) 104475 (103162-105493)    64435 (63029-65718) 253660 (249917-259171)       1257 (1251-1281)    15075 (15024-15181)

Verify, ns/message (json: parse)
format                          request          report 1 item         report 4 items     report 1 + 8K blob     report 4 + 8K blob         frame (struct)          16 detections
libbufs                   3.4 (3.4-3.4)       40.8 (40.7-40.9)          147 (145-149)       44.7 (44.7-45.2)          162 (161-163)          1.9 (1.9-1.9)          3.4 (3.4-3.4)
flatbuffers            40.3 (39.7-41.1)          114 (108-116)          358 (352-364)          119 (117-122)          375 (365-380)          6.6 (6.5-7.0)       11.6 (11.2-12.2)
json (libhttp)         1640 (1632-1658)    16254 (16152-16583)    65626 (65381-66031)    32908 (32768-37187) 134278 (131895-136914)       1289 (1281-1464)    12079 (12044-12114)
json/jsonlite          2253 (2232-2301)    39344 (39102-40006) 158883 (158400-159814)    58500 (58240-59076) 237954 (234745-240832)       1904 (1894-1916)    23241 (23099-23527)

Read every field, ns/message (json: walk the parsed tree, base64-decode blobs)
format                          request          report 1 item         report 4 items     report 1 + 8K blob     report 4 + 8K blob         frame (struct)          16 detections
libbufs                   3.9 (3.8-4.0)       53.0 (52.8-54.5)          199 (198-209)       54.3 (54.1-54.5)          204 (204-205)          4.1 (4.1-4.2)       18.7 (18.4-18.9)
flatbuffers               7.1 (6.9-7.2)       58.3 (57.5-65.7)          212 (210-216)       60.5 (59.8-62.9)          217 (215-219)          3.4 (3.4-3.5)       22.5 (22.5-22.9)
json (libhttp)            287 (283-405)       1263 (1255-1520)       4810 (4751-4928)    19853 (19622-20283)    79336 (78366-79630)          190 (187-191)       1512 (1501-1532)
json/jsonlite             319 (313-334)       1254 (1248-1259)       4782 (4751-4810)    19850 (19778-19993)    79980 (79591-81186)          232 (229-239)       1812 (1809-1816)

Verify + read, ns/message (json: parse + walk)
format                          request          report 1 item         report 4 items     report 1 + 8K blob     report 4 + 8K blob         frame (struct)          16 detections
libbufs                   8.3 (8.3-8.3)       95.2 (94.7-96.6)          346 (345-348)      99.8 (99.5-100.4)          366 (365-367)          3.0 (3.0-3.0)       22.6 (22.1-23.0)
flatbuffers            45.2 (44.9-46.6)          170 (169-172)          563 (561-570)          175 (173-177)          591 (582-595)          9.4 (9.1-9.9)       34.5 (34.2-35.8)
json (libhttp)         1954 (1910-2001)    17806 (17494-19006)    72343 (72065-72737)    56626 (55745-57350) 225646 (224459-231498)       1486 (1475-1506)    13769 (13647-13980)
json/jsonlite          2601 (2570-2713)    40791 (40648-41062) 162828 (161074-164398)    83905 (82796-84453) 339389 (337064-341543)       2187 (2153-2209)    25479 (25071-25802)

Heap allocations per message (malloc-level, includes operator new), after warm-up; 'n/B' = calls/bytes
format           operation               request      report 1 item     report 4 items report 1 + 8K blob report 4 + 8K blob     frame (struct)      16 detections
libbufs          build reuse                 0/0                0/0                0/0                0/0                0/0                0/0                0/0
libbufs          build fresh                 0/0                0/0                0/0                0/0                0/0                0/0                0/0
libbufs          verify+read                 0/0                0/0                0/0                0/0                0/0                0/0                0/0
flatbuffers      build reuse                 0/0                0/0                0/0                0/0                0/0                0/0                0/0
flatbuffers      build fresh              1/1024             1/1024            5/13504            2/10240           6/128640             1/1024             1/1024
flatbuffers      verify+read                 0/0                0/0                0/0                0/0                0/0                0/0                0/0
json (libhttp)   build reuse                1/18            8/10200            8/10200            4/21965           20/97900                0/0             5/1240
json (libhttp)   build fresh              3/1383            9/14265            9/26074          8.5/42081          21/157658              1/231             6/3504
json (libhttp)   verify+read                 0/0                0/0                0/0            2/19156           10/76686                0/0                0/0
json/jsonlite    verify+read              9/4054          238/50502         939/196483          249/89396         983/352059             8/4023          152/36131
(libbufs takes message memory from its mmap'd pool, which malloc counting does not see; after warm-up the pool makes no mmap calls either)

Long run: build into a FIFO of 8 live messages, verify+read+free each as its slot is reused; mix of requests, 0-4 item reports (30% of items with a 1-16 KiB blob), 0-64 detections, frames, and one 1-4 MiB blob report per ~2000. One process per format. RSS in MiB.
format               cycles outliers  Mcycle/s   start    warm     mid     end end-warm drained    peak  notes
libbufs             5000000     2535     1.833     7.7    14.3    14.6    18.5     +4.2    18.5    18.5  pool: 21.4 MiB mapped (20.1 MiB idle large), 11 mmap, 0 munmap
flatbuffers         5000000     2535     1.059     7.8    11.8    12.9    13.7     +1.9    13.7    15.2  
json (libhttp)       500000      252     0.010     7.8     8.7    16.8    14.4     +5.8    14.4    24.3  
(json runs 1/10 of the cycles: it is that much slower)

Multi-threaded: 8 threads at once, each running the long-run mix (400000 cycles each); 'handoff' passes every message to the next thread to verify, read and free. 8 CPUs allowed. RSS in MiB.
format           mode                         messages   M msg/s  RSS end     peak  notes
libbufs          independent, shared pool      3200000     9.753     43.6     43.7  pool: 40.0 MiB mapped (34.0 MiB idle large), 33 mmap, 0 munmap
libbufs          independent, pool/thread      3200000    13.955     68.9     69.0  pool: 69.2 MiB mapped (60.0 MiB idle large), 55 mmap, 0 munmap
libbufs          handoff, shared pool          3200000     6.647     56.4     56.6  pool: 57.0 MiB mapped (30.0 MiB idle large), 116 mmap, 0 munmap
libbufs          handoff, pool/thread          3200000     6.540    127.0    127.1  pool: 148.2 MiB mapped (60.0 MiB idle large), 371 mmap, 0 munmap
flatbuffers      independent                   3200000     8.212     45.5     45.6  
flatbuffers      handoff                       3200000     3.013     53.2     53.3  
json (libhttp)   independent                    320000     0.046     52.8     85.0  
json (libhttp)   handoff                        320000     0.048     82.1    105.2
```

```
bufs_bench_shm: client on CPU 28, server on CPU 30; 200000 iterations

Ping-pong round trip (query -> report, both verified and read), ns
message            transport               bytes      mean      p50      p90      p99    p99.9       max       rt/s
report 1 item      shm futex                 864     18917    17981    18723    31161    88217   5336893      52759
report 1 item      shm spin 20us+futex       864      1491     1405     1543     1684    15631   3387127     656105
report 1 item      shm spin                  864      1370     1318     1474     1630    13561     92437     711504
report 1 item      unix socket               864     18553    17730    18318    31501    57534   5026523      53772
report 1 item      tcp loopback              864     26511    25091    26568    38866    91716   5159722      37660
report 4 + 8K blob shm futex               36128     21628    20702    21646    34870   106659   9476490      46149
report 4 + 8K blob shm spin 20us+futex     36128      3797     3580     3923     5736    38847    671167     260797
report 4 + 8K blob shm spin                36128      3805     3549     3864    10642    34645   4556063     259259
report 4 + 8K blob unix socket             36128     30659    29779    31062    43566    91222   6141549      32566
report 4 + 8K blob tcp loopback            36128     33934    32763    35142    46960    88242   6951207      29430

One-way stream of reports (server builds, client verifies and reads)
message            transport               bytes   messages        msg/s       MB/s
report 1 item      shm futex                 864    2000000      1330870       1150
report 1 item      shm spin 20us+futex       864    2000000      1637042       1414
report 1 item      shm spin                  864    2000000      1633233       1411
report 1 item      unix socket               864    2000000       697308        602
report 1 item      tcp loopback              864    2000000       247171        214
report 4 + 8K blob shm futex               36128     400000       389156      14059
report 4 + 8K blob shm spin 20us+futex     36128     400000       490904      17735
report 4 + 8K blob shm spin                36128     400000       481704      17403
report 4 + 8K blob unix socket             36128     400000       145752       5266
report 4 + 8K blob tcp loopback            36128     400000        93091       3363
```
