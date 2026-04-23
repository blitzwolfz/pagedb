# PageDB

PageDB is a small embedded key value store written in C++. It keeps all data
in one file that is split into 4096 byte pages. It is a learning project about
how storage engines work, not a production database.

## Parts

- disk manager, reads and writes pages with pread and pwrite
- buffer pool with a fixed number of frames and LRU eviction
- B+ tree with variable length keys and values
- write ahead log (not written yet)

## Build

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build

## State

Insert, lookup, delete and range scan work. Deletes do not give pages back to
the file yet. There is no crash recovery yet.
