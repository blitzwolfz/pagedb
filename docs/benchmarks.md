# Benchmarks

All numbers below come from the `bench` program in this repository. Nothing
here is estimated or copied from somewhere else.

## Setup

- machine: Apple M3, 8 cores, 24 GB memory, built in SSD
- system: macOS 27.0
- compiler: Apple clang 21, `-O3` (CMake Release)
- database: 200000 keys, key 16 bytes, value 100 bytes, about 30 MB of data
- buffer pool: 1024 pages (4 MB) if not written differently
- one run each, single thread if not written differently

How to repeat:

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ./build/bench --ops 200000 --durable 1
    ./build/bench --ops 200000 --durable 0
    ./build/bench --ops 200000 --durable 0 --only cache
    ./build/bench --ops 400000 --durable 0 --threads 8 --only threads
    ./build/bench --ops 200000 --durable 1 --only recovery

## Durable writes, fsync after every operation

| benchmark | ops/s | p50 | p99 |
|---|---|---|---|
| sequential insert | 22451 | 38.6 us | 108.7 us |
| random insert | 21297 | 41.3 us | 107.3 us |
| random get | 708813 | 1.4 us | 2.0 us |
| range scan, 100 rows each | 64105 | 13.7 us | 30.5 us |
| 80 read 20 write | 90860 | 2.0 us | 51.3 us |

## Same run without fsync

| benchmark | ops/s | p50 | p99 |
|---|---|---|---|
| sequential insert | 49633 | 16.0 us | 79.1 us |
| random insert | 49720 | 18.0 us | 79.3 us |
| random get | 666171 | 1.5 us | 2.3 us |
| range scan, 100 rows each | 70289 | 12.7 us | 27.7 us |
| 80 read 20 write | 198432 | 1.7 us | 19.6 us |

Writes are about twice as fast without fsync, reads are the same, which is
what one would expect. `fsync` on macOS does not force the disk cache, so the
durable numbers would be lower on hardware that does.

## Buffer pool size, random get

| pool pages | memory | hit rate | ops/s |
|---|---|---|---|
| 32 | 128 KB | 68.7 % | 619910 |
| 128 | 512 KB | 73.8 % | 655515 |
| 512 | 2 MB | 83.0 % | 735628 |
| 2048 | 8 MB | 86.1 % | 652458 |
| 8192 | 32 MB | 94.8 % | 816617 |

The hit rate goes up with the pool, the speed almost does not. The data set
is small and the operating system caches the file anyway, so a miss in the
pool is still a hit in the page cache of the system.

## Reader threads, random get, 400000 operations in total

| threads | ops/s |
|---|---|
| 1 | 634203 |
| 2 | 395545 |
| 4 | 242258 |
| 8 | 129948 |

Reads get slower with more threads. Every lookup takes the one buffer pool
lock about five times, once per page on the way down, so the threads spend
their time waiting for that lock and moving frames in the LRU list. This is a
known limit of the current design, see `limitations.md`.

## Recovery

20000 operations were written by a child process that was not allowed to
close the database. The log was about 95 MB. The next open replayed it in
0.36 seconds, which is around 55000 operations per second.

## One thing that was measured and then fixed

The first version marked every page it walked over as changed, because the
tree asked the page guard for a writable pointer before it knew whether it
was going to write anything. Every insert therefore put the root, the
internal node and the leaf into the log.

- before: 12767 log bytes per insert, 25294 inserts per second
- after: 4885 log bytes per insert, 57102 inserts per second

The change was to read pages read only while walking down and only ask for a
writable pointer in the node that really changes. The same measurement was
run before and after with `--ops 100000 --durable 0` on the same machine.
