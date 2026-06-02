# Recovery design

This is written before the log code, as the plan the code has to follow.

## What is promised

One `put` or `remove` call is one operation. When the call returns `Ok` and
the database was opened with `durable = true`, the operation is in the log
file and `fsync` on the log file has returned. After a process crash the
database comes back with that operation applied.

A crash while an operation is still running may keep it or lose it. It must
never leave half of it, so a page split can not be replayed in part.

`SIGKILL` is the crash that is tested. Power loss is not tested and is not
promised, because that depends on the disk honouring `fsync`.

## Log file

The log is `<database>.wal`. It is only appended to. A record is:

| offset | size | field |
|---|---|---|
| 0 | 4 | magic `WAL1` |
| 4 | 8 | log sequence number |
| 12 | 1 | record type: 1 page image, 2 commit |
| 13 | 3 | unused |
| 16 | 4 | page id, 0 for a commit record |
| 20 | 4 | payload length |
| 24 | 4 | crc32 of the first 24 header bytes with this field as 0, plus the payload |
| 28 | n | payload, a full 4096 byte page for a page image |

A group is one or more page image records followed by one commit record. The
page images are the new content of every page the operation changed, the meta
page included when the root page id or the free list changed.

## Rules

1. Page images are written before the commit record. The commit record is the
   point where the operation counts as done.
2. A dirty page that belongs to an operation that has not committed yet is
   never written to the database file. The buffer pool keeps those frames and
   does not pick them for eviction, so the database file never holds half of
   an operation. When no other frame can be freed the operation fails with
   `PoolExhausted` and nothing is changed.
3. Because of rule 2 replay only ever writes whole operations, and a page
   image is the full new page, so replaying the same log twice gives the same
   result.
4. A new root page and the split pages under it are in the same group, so the
   tree and the root page id are always recovered together.
5. A checkpoint writes every dirty page to the database file, syncs the file,
   and only then truncates the log. A crash during a checkpoint leaves the log
   in place and the log is replayed again, which is safe because of rule 3.

## Reading the log after a crash

Records are read from the start of the file. A record is used when the magic
is right, the length fits in the file, and the crc matches. Page images are
kept in memory until the commit record of their group is read, then they are
written to the database file. The last group is thrown away when its commit
record is missing or broken, which is what a torn write at the end of the log
looks like. A broken record in the middle stops the replay at that point, and
everything before it is still applied.

After the replay the database file is synced, the meta page is read again,
and the log is truncated.

## Not covered

- Two processes writing the same file. The file lock stops that.
- Groups from more than one operation at a time. Writes are serialised by the
  database lock, so there is only ever one open group.
