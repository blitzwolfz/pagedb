# Limitations

What PageDB does not do, or does badly. Everything here is known, not hidden.

## Data

- Keys are 1 to 64 bytes, values 0 to 256 bytes. Bigger values are rejected
  because there are no overflow pages.
- `scan` builds the whole result in memory, so a range over a million rows
  needs the memory for a million rows. There is no cursor.
- Only one key order, byte by byte. No secondary index, no sorting rules.

## Concurrency

- One process may open the file for writing. The file lock rejects a second
  one. Two processes sharing a database do not work.
- All writes take one lock, so more writer threads do not make writing
  faster.
- Reads get slower with more threads, see `benchmarks.md`. Every page lookup
  takes the single buffer pool lock. A pool split into parts, or a read path
  that does not touch the LRU list on every hit, would fix that. It is not
  done.

## Durability

- The crash that is tested is `SIGKILL` of the process. Power loss is not
  tested. On macOS `fsync` does not push the disk cache out, `F_FULLFSYNC`
  would be needed for that, so the durable benchmark numbers are better than
  a real power safe write would be.
- One `put` or `remove` is one operation. There are no transactions over
  several operations.
- The log stores whole 4096 byte pages, about 4.9 KB per insert of a 116 byte
  record. A log that stores only the changed bytes would be much smaller.
- Recovery reads the log from the beginning. There is no checkpoint record in
  the log that lets it start later, the log is simply emptied by a checkpoint.

## Errors

- When a write fails half way, for example because every frame in the pool is
  taken, the database is marked as damaged. Reads still work, writes are
  refused, and the file has to be opened again. The pages in memory are
  dropped so the file keeps its last good state.
- Only the meta page has a checksum. A node page that is damaged on disk is
  not detected and will be used as if it were fine.

## Other

- Pages that are freed go on a free list and are used again, but the file
  never gets smaller.
- The page lsn field in the page header is always 0. The recovery protocol
  does not need it, it is only reserved for later.
- Built and tested on macOS on an arm64 machine. Only POSIX calls are used
  (`pread`, `pwrite`, `fsync`, `flock`, `ftruncate`), so Linux should work,
  but it was not tested there.
- The tests that pass do not prove there are no bugs left. The random test
  against `std::map` uses three fixed seeds and 20000 steps each.
