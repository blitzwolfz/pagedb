# PageDB — AI Implementation PRD

**Project:** PageDB  
**Type:** Embedded, persistent key-value storage engine  
**Language:** C++20  
**Target:** Linux x86-64  
**Build:** CMake  
**Status:** Implementation specification, v1.0

> **For the coding agent:** This is a build specification, not a request for another plan or a mock implementation. Implement the smallest correct, testable increment in milestone order. Do not claim a feature works until its tests pass. Report blockers and deviations explicitly.

## 1. Mission

Build a single-process, disk-backed, ordered key-value database in C++20 **without using an existing database or storage-engine library**. Implement a custom disk manager, bounded buffer pool, persistent B+ tree, write-ahead logging (WAL), process-crash recovery, and thread-safe access. Produce reproducible benchmarks and engineering documentation.

The intended outcome is a technically defensible systems-programming portfolio project. Prioritize correctness and explainable design over feature count or invented performance claims.

## 2. Scope and versioning

### v0.1 — persistent page storage
- Create/open/close one database file; fixed 4,096-byte pages.
- Define versioned on-disk metadata, page IDs, and explicit little-endian serialization.
- Implement robust `pread`/`pwrite` loops, error handling, page allocation, and synchronization.
- Reopen the database and verify persisted pages.

### v0.2 — bounded buffer pool
- Implement page table, fixed-capacity frames, pin/unpin, dirty tracking, and LRU eviction.
- Expose move-only RAII page guards; never evict pinned pages.
- Define behavior when all frames are pinned: return a specific error, not undefined behavior.

### v0.3 — working ordered key-value database
- Persist a B+ tree with lookup, upsert, and ordered inclusive-exclusive range scan `[start, end)`.
- Handle leaf/internal splits, root creation, and persistence of the root page ID.
- Add deletion, underflow redistribution/merge, and root shrinking after insertion/lookup/scan tests pass.
- Differential-test behavior against `std::map` with reproducible random seeds.

### v0.4 — durability and recovery
- Implement WAL for complete logical operations, checksums, log sequence numbers, and explicit commit boundaries.
- Define atomicity for multi-page tree updates. Do **not** add a superficially working WAL that can replay half of a split.
- Recover acknowledged operations after `SIGKILL`; handle incomplete trailing records.
- Add safe checkpointing only after basic replay is correct.

### v0.5 — concurrency
- Start with a coarse `std::shared_mutex` for tree reads/writes; synchronize buffer pool and WAL metadata separately.
- Define and document lock acquisition order; run concurrency tests and ThreadSanitizer where available.
- Fine-grained latch coupling is **not** required for v1.0.

### v1.0 — release
- Reproducible benchmarks and profiler-driven investigation.
- Clear README, on-disk format, architecture, recovery guarantees, known limitations, and a clean CMake build.

**Non-goals:** SQL, networking, multi-process concurrent writers, distributed replication, sharding, MVCC, multi-statement transactions, query planning, production-readiness claims.

## 3. Public API contract

Use an idiomatic C++20 API; the following is the target behavior, not an instruction to invent unnecessary generic frameworks:

```cpp
struct DatabaseOptions {
    std::filesystem::path path;
    std::size_t buffer_pool_pages = 1024;
    bool durable = true;
};

class Database {
public:
    static Result<std::unique_ptr<Database>> open(const DatabaseOptions&);

    Status put(std::string_view key, std::string_view value);
    Result<std::optional<std::string>> get(std::string_view key);
    Status remove(std::string_view key);
    Result<std::vector<KVPair>> scan(
        std::string_view start_inclusive,
        std::string_view end_exclusive);
    Status close();
};
```

Implement a small, explicit `Status`/`Result<T>` error model; errors must not be confused with missing keys. `put` replaces existing values; `remove` of a missing key returns `NotFound`. `get` of a missing key returns an empty optional. Scans return sorted keys in `[start, end)`; if start >= end, return an empty result.

**Data limits for v1:** binary keys of 1–64 bytes; binary values of 0–256 bytes. Compare keys lexicographically by **unsigned byte**, not locale or signed `char`. Invalid lengths return `InvalidArgument`. No pointer/reference to cached page contents escapes the guard's lifetime. At most one process may open the database for writing; acquire an appropriate exclusive file lock or reject a conflicting opener.

## 4. Architecture

```text
Application / Database API
           |
     B+ Tree Index ----- operation-level concurrency control
           |
     Buffer Pool -------- frame/page synchronization
           |
     Disk Manager ------- pagedb.db
           |
     WAL Manager -------- pagedb.wal
```

WAL is part of the **write and flush protocol**, not merely a background logging sidecar. The buffer pool must not write a dirty page whose required WAL records are not durable. Define the WAL/page ordering contract before integrating recovery.

### 4.1 Disk manager

- Page size: 4,096 bytes; reserve page 0 for a header with magic, format version, page size, root page ID, and allocator/recovery metadata as needed.
- Page offset = page ID × page size; validate overflow and invalid IDs.
- Store integer fields using explicit endianness; avoid dumping C++ structs or native pointers to disk.
- Handle `EINTR`, short reads/writes, I/O failures, truncation, and incompatible formats.
- Document metadata synchronization and new-file directory-sync behavior where applicable.
- Page IDs are persistent identifiers; in-memory frame indices are not.

### 4.2 Buffer pool

- Fixed frame budget; a page table maps each cached page ID to exactly one frame.
- Pin counts prevent eviction. Dirty pages are persisted safely before reuse.
- Implement O(1)-amortized LRU bookkeeping where practical.
- Use move-only guards with deterministic unpin; prevent double-unpin and stale guard access.
- Test a one-frame pool, a pool smaller than the tree's working set, repeated eviction, dirty eviction, and all-pinned exhaustion.

### 4.3 B+ tree

- All nodes occupy disk pages. Internal nodes store separators and child page IDs; leaves store bounded key/value records and next-leaf links.
- Define an explicit page header and node encoding in `docs/on_disk_format.md` **before** relying on it.
- Never persist process virtual addresses.
- Preserve sorted node contents, valid child links, equal leaf depth, correct separators, and a durable root identifier.
- Handle duplicate inserts as replacement; test splits propagating through several levels, deletion/merge, root shrinkage, empty tree, and scans across leaf boundaries.
- Initially serialize mutating operations. Do not introduce concurrent splits before single-threaded recovery correctness.

### 4.4 WAL and recovery — mandatory design gate

Before coding WAL, write `docs/recovery_design.md` specifying:

1. WAL record format, length framing, checksums, LSNs, and operation commit marker.
2. Exact durability/linearization boundary of an acknowledged API operation.
3. How complete logical operations are replayed idempotently or how page-level redo is made safe.
4. How root changes and multi-page B+ tree splits are recovered atomically.
5. When dirty pages may be flushed relative to WAL durability.
6. How checkpoints are made durable before WAL truncation.
7. Behavior for interrupted operations, torn/truncated log tail, and detected corruption.

**Correctness constraint:** Redo-only logical logging is not safe if partially applied operations can reach the database file and then be blindly replayed. Choose and implement a coherent recovery protocol (for example, no-steal with a well-defined atomic checkpoint/rebuild scheme, or a documented page-level WAL protocol with appropriate transaction semantics). Do not quietly assert ACID guarantees that the design does not establish. If the chosen protocol cannot satisfy the v0.4 acceptance tests, stop and revise the design instead of weakening the tests.

A successful durable write must survive tested abrupt **process** termination, assuming successful sync operations are honored by the OS/storage stack. `SIGKILL` does not establish power-loss durability. For an in-flight operation at crash time, the recovered result may include or exclude it, but must remain structurally valid and consistent with the declared contract.

### 4.5 Concurrency

- Coarse-grained tree read/write locking is acceptable for v1.
- Guard page-table/frame state and logging state against races.
- Document lock order and avoid holding inappropriate locks across unbounded waits.
- Do not imply per-key parallel write scalability if writes are serialized.

## 5. Required project layout

```text
pagedb/
├── CMakeLists.txt
├── README.md
├── include/pagedb/
│   ├── database.hpp
│   ├── status.hpp
│   ├── disk_manager.hpp
│   ├── buffer_pool.hpp
│   ├── page.hpp
│   ├── btree.hpp
│   └── wal.hpp
├── src/
├── tests/
│   ├── disk_test.cpp
│   ├── buffer_pool_test.cpp
│   ├── btree_test.cpp
│   ├── recovery_test.cpp
│   └── concurrency_test.cpp
├── benchmarks/
└── docs/
    ├── architecture.md
    ├── on_disk_format.md
    ├── recovery_design.md
    ├── benchmarks.md
    └── limitations.md
```

Use CMake + CTest. GoogleTest or Catch2 is acceptable for tests; they are not database engines. Keep benchmark dependencies minimal and document their setup. No dependency should replace the required storage components.

## 6. Verification and acceptance gates

| Gate | Required proof |
|---|---|
| v0.1 | Write pages, close, reopen, compare bytes; reject bad magic/version/truncation. |
| v0.2 | Cache hit/miss/eviction/dirty flush/pin tests with very small capacities. |
| v0.3 | Seeded differential tests against `std::map`; randomized insert/update/delete/get/scan; reopen and repeat; check B+ tree invariants. |
| v0.4 | Fork/launch a child, acknowledge operations to an external test oracle, kill child at injected points, reopen, check acknowledged operations and index invariants. Include WAL-tail and checkpoint interruption tests. |
| v0.5 | Multithreaded read/write tests; no observed data races under sanitizer-supported workloads. |
| v1.0 | Clean build, all applicable tests, documented benchmarks and known limitations. |

Do not claim that passing finite tests proves absence of bugs. Keep an explicit limitations section.

**Build quality:** compile with high warning levels (`-Wall -Wextra -Wpedantic` with compiler-appropriate variants), run AddressSanitizer/UBSan test builds, and use ThreadSanitizer separately when supported. Treat sanitizer reports as defects to investigate, not warnings to suppress casually.

## 7. Benchmarks

Implement reproducible sequential insert, random insert, random get, range scan, 80/20 read/write, cache-size sweep, worker-count sweep, and recovery-time runs.

Report operation count, key/value sizes, key distribution, dataset size, cache budget, durability mode, thread count, hardware, OS, compiler/build flags, iterations, ops/sec, p50/p99 latency, and relevant I/O/cache/WAL counters. Compare like-for-like durability configurations. Never fabricate results or claim a speedup without a repeatable baseline.

Use `perf` or equivalent to profile at least one measurable bottleneck; document hypothesis, change, and before/after data. Comparing against SQLite/RocksDB is optional and must clearly state differences in semantics and setup.

## 8. Coding-agent execution rules

1. **Implement milestone by milestone.** Do not jump to WAL, concurrency, SQL, or optimizations before prior correctness gates pass.
2. At the start of a milestone, inspect existing code and documents; identify the minimum files and tests needed.
3. Make code changes and tests together. Prefer small, reviewable changes over a giant one-shot rewrite.
4. State assumptions explicitly when the spec leaves an engineering choice open; record persistent-format and recovery choices in documentation.
5. Compile and run relevant tests after every increment. Do not report a command as run unless it actually ran; include failing tests and blockers.
6. Never silently drop errors, use unbounded caches, expose invalid references, or write raw C++ object memory to a persistent format.
7. Preserve existing public API behavior and on-disk compatibility unless a migration/version change is documented.
8. If a requested feature conflicts with data integrity, explain the conflict and implement a correct smaller scope instead.
9. Do not create fake benchmark numbers, fake screenshots, or resume claims.
10. Finish each milestone with: files changed, behavior implemented, commands/tests run and outcomes, outstanding risks, and the next milestone.

## 9. First task for the coding agent

**Implement only v0.1.** Create the CMake project, a minimal status/result abstraction, a versioned page-0 header, and a Linux disk manager with robust positional I/O. Add CTest coverage for creating a database, allocating/writing/reading pages, closing/reopening, rejecting invalid/truncated files, and surfacing I/O errors. Write `docs/on_disk_format.md` for the initial header and page-ID conventions. Do not create placeholder B+ tree, WAL, or concurrency classes just to make the repository look complete.

## 10. Definition of done for v1.0

PageDB can be built from a clean checkout, used via the documented C++ API, persist and retrieve records, perform ordered scans, recover acknowledged writes under its explicitly stated process-crash model, safely support documented concurrent access, and reproduce its published benchmarks. Its architecture, storage format, durability contract, test evidence, and limitations are documented. No feature is described as complete solely because an interface or stub exists.
