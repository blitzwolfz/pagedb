#ifndef PAGEDB_BUFFER_POOL_HPP
#define PAGEDB_BUFFER_POOL_HPP

#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "pagedb/disk_manager.hpp"
#include "pagedb/page.hpp"
#include "pagedb/status.hpp"

namespace pagedb {

class BufferPool;

// Holds a pin on one frame. The pin is released in the destructor, so a page
// can not be evicted while somebody still has a guard for it.
class PageGuard {
public:
    PageGuard() : pool_(0), frame_(0), id_(NO_PAGE), dirty_(false) {}
    PageGuard(BufferPool* pool, size_t frame, page_id_t id)
        : pool_(pool), frame_(frame), id_(id), dirty_(false) {}
    ~PageGuard();

    PageGuard(const PageGuard&) = delete;
    PageGuard& operator=(const PageGuard&) = delete;
    PageGuard(PageGuard&& other);
    PageGuard& operator=(PageGuard&& other);

    bool valid() const { return pool_ != 0; }
    page_id_t page_id() const { return id_; }

    const uint8_t* read() const;
    uint8_t* write();

    void drop();

private:
    BufferPool* pool_;
    size_t frame_;
    page_id_t id_;
    bool dirty_;
};

struct Frame {
    page_id_t page_id;
    uint32_t pin_count;
    bool dirty;
    uint64_t page_lsn;

    Frame() {
        page_id = NO_PAGE;
        pin_count = 0;
        dirty = false;
        page_lsn = 0;
    }
};

class BufferPool {
public:
    BufferPool(DiskManager* disk, size_t capacity);
    ~BufferPool();

    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    Result<PageGuard> fetch(page_id_t id);
    Result<PageGuard> new_page();
    Status free_page(page_id_t id);

    Status flush_page(page_id_t id);
    Status flush_all();

    size_t capacity() const { return capacity_; }
    uint64_t hits() const { return hits_; }
    uint64_t misses() const { return misses_; }
    uint64_t evictions() const { return evictions_; }
    uint64_t writes() const { return writes_; }

private:
    friend class PageGuard;

    uint8_t* frame_data(size_t frame) { return mem_ + frame * PAGE_SIZE; }
    void unpin(size_t frame, bool dirty);
    void set_dirty(size_t frame);
    Status pick_victim(size_t* frame_out);
    Status flush_frame(size_t frame);
    void lru_remove(size_t frame);
    void lru_add(size_t frame);

    std::mutex mu_;
    DiskManager* disk_;
    size_t capacity_;
    uint8_t* mem_;
    std::vector<Frame> frames_;
    std::unordered_map<page_id_t, size_t> table_;
    std::list<size_t> lru_;
    std::unordered_map<size_t, std::list<size_t>::iterator> lru_pos_;
    std::vector<size_t> free_frames_;

    uint64_t hits_;
    uint64_t misses_;
    uint64_t evictions_;
    uint64_t writes_;
};

}  // namespace pagedb

#endif
