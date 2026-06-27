# Architecture

PageDB has four layers. Each layer only talks to the one below it.

    Database api        put, get, remove, scan
        |
    B+ tree             finds the leaf page for a key
        |
    Buffer pool         keeps a fixed number of pages in memory
        |
    Disk manager        reads and writes 4096 byte pages in the file

## Database

Holds one lock for the whole tree. Readers share it, writers take it alone.
It checks key and value sizes and turns tree results into the public
`Status` and `Result` types.

## B+ tree

Every node is one page. Internal nodes hold separator keys and child page
ids, leaves hold the keys and values and a pointer to the next leaf, so a
range scan is a walk along the leaves. Insert goes down to the leaf and
splits pages on the way back up. Delete takes entries from a sibling or
merges two nodes, and drops the root when it has no keys left.

## Buffer pool

A fixed number of frames. The page table says which page is in which frame.
A page that somebody is using is pinned and can not be thrown out. The least
recently used unpinned frame is taken when a new page is needed, and it is
written back to the file first when it is dirty. The caller gets a
`PageGuard` that releases the pin in its destructor.

## Disk manager

Opens the file, takes an exclusive lock so only one process writes, and does
positional reads and writes that handle short reads and interrupts. It also
keeps the meta page, which holds the root page id and the free page list.

## Locks

There are three locks and they are always taken in this order:

1. the database lock, shared for `get` and `scan`, exclusive for `put`,
   `remove` and `checkpoint`
2. the log lock inside the write ahead log
3. the buffer pool lock

A thread never takes a lock that comes earlier in this list while it holds a
later one, so there is no deadlock. Writes are serialised by the database
lock, which means more writer threads do not make writes faster.
