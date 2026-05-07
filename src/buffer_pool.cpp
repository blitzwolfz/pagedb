#include "pagedb/buffer_pool.hpp"

#include <string.h>

namespace pagedb {

// A page on the free list keeps the next free page id at offset 8, which is
// the same place the node header keeps its extra page id.
static const size_t FREE_NEXT_OFFSET = 8;
static const uint8_t PAGE_TYPE_FREE = 4;

PageGuard::~PageGuard() {
    drop();
}

PageGuard::PageGuard(PageGuard&& other) {
    pool_ = other.pool_;
    frame_ = other.frame_;
    id_ = other.id_;
    dirty_ = other.dirty_;
    other.pool_ = 0;
    other.id_ = NO_PAGE;
    other.dirty_ = false;
}

PageGuard& PageGuard::operator=(PageGuard&& other) {
    if (this != &other) {
        drop();
        pool_ = other.pool_;
        frame_ = other.frame_;
        id_ = other.id_;
        dirty_ = other.dirty_;
        other.pool_ = 0;
        other.id_ = NO_PAGE;
        other.dirty_ = false;
    }
    return *this;
}

void PageGuard::drop() {
    if (pool_ != 0) {
        pool_->unpin(frame_, dirty_);
        pool_ = 0;
        id_ = NO_PAGE;
        dirty_ = false;
    }
}

const uint8_t* PageGuard::read() const {
    if (pool_ == 0) {
        return 0;
    }
    return pool_->frame_data(frame_);
}

uint8_t* PageGuard::write() {
    if (pool_ == 0) {
        return 0;
    }
    dirty_ = true;
    pool_->set_dirty(frame_);
    return pool_->frame_data(frame_);
}

BufferPool::BufferPool(DiskManager* disk, size_t capacity) {
    disk_ = disk;
    capacity_ = capacity;
    if (capacity_ < 1) {
        capacity_ = 1;
    }
    mem_ = new uint8_t[capacity_ * PAGE_SIZE];
    memset(mem_, 0, capacity_ * PAGE_SIZE);
    frames_.resize(capacity_);
    for (size_t i = 0; i < capacity_; i++) {
        free_frames_.push_back(capacity_ - 1 - i);
    }
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
    writes_ = 0;
}

BufferPool::~BufferPool() {
    delete[] mem_;
}

void BufferPool::lru_remove(size_t frame) {
    std::unordered_map<size_t, std::list<size_t>::iterator>::iterator it =
        lru_pos_.find(frame);
    if (it != lru_pos_.end()) {
        lru_.erase(it->second);
        lru_pos_.erase(it);
    }
}

void BufferPool::lru_add(size_t frame) {
    lru_remove(frame);
    lru_.push_back(frame);
    std::list<size_t>::iterator it = lru_.end();
    --it;
    lru_pos_[frame] = it;
}

Status BufferPool::flush_frame(size_t frame) {
    Frame& f = frames_[frame];
    if (!f.dirty || f.page_id == NO_PAGE) {
        return Status::Ok();
    }
    Status s = disk_->write_page(f.page_id, frame_data(frame));
    if (!s.ok()) {
        return s;
    }
    f.dirty = false;
    writes_++;
    return Status::Ok();
}

// Least recently used frame that nobody is using right now.
Status BufferPool::pick_victim(size_t* frame_out) {
    if (!free_frames_.empty()) {
        *frame_out = free_frames_.back();
        free_frames_.pop_back();
        return Status::Ok();
    }
    if (lru_.empty()) {
        return Status::PoolExhausted("every frame in the buffer pool is pinned");
    }

    size_t victim = lru_.front();
    lru_remove(victim);

    Frame& f = frames_[victim];
    Status s = flush_frame(victim);
    if (!s.ok()) {
        lru_add(victim);
        return s;
    }
    if (f.page_id != NO_PAGE) {
        table_.erase(f.page_id);
    }
    f.page_id = NO_PAGE;
    f.pin_count = 0;
    f.dirty = false;
    f.page_lsn = 0;
    evictions_++;
    *frame_out = victim;
    return Status::Ok();
}

Result<PageGuard> BufferPool::fetch(page_id_t id) {
    std::lock_guard<std::mutex> lock(mu_);

    std::unordered_map<page_id_t, size_t>::iterator it = table_.find(id);
    if (it != table_.end()) {
        size_t frame = it->second;
        frames_[frame].pin_count++;
        lru_remove(frame);
        hits_++;
        return Result<PageGuard>(PageGuard(this, frame, id));
    }

    size_t frame = 0;
    Status s = pick_victim(&frame);
    if (!s.ok()) {
        return Result<PageGuard>(s);
    }

    s = disk_->read_page(id, frame_data(frame));
    if (!s.ok()) {
        free_frames_.push_back(frame);
        return Result<PageGuard>(s);
    }

    Frame& f = frames_[frame];
    f.page_id = id;
    f.pin_count = 1;
    f.dirty = false;
    f.page_lsn = 0;
    table_[id] = frame;
    misses_++;
    return Result<PageGuard>(PageGuard(this, frame, id));
}

Result<PageGuard> BufferPool::new_page() {
    std::lock_guard<std::mutex> lock(mu_);

    page_id_t id = NO_PAGE;
    MetaPage& meta = disk_->meta();

    if (meta.free_list_head != NO_PAGE) {
        id = meta.free_list_head;
        page_id_t next = NO_PAGE;
        std::unordered_map<page_id_t, size_t>::iterator cached = table_.find(id);
        if (cached != table_.end()) {
            // The freed page may still be dirty in a frame, the copy on disk
            // is then older than the one we have here.
            next = get_u32(frame_data(cached->second) + FREE_NEXT_OFFSET);
        } else {
            uint8_t buf[PAGE_SIZE];
            Status s = disk_->read_page(id, buf);
            if (!s.ok()) {
                return Result<PageGuard>(s);
            }
            next = get_u32(buf + FREE_NEXT_OFFSET);
        }
        meta.free_list_head = next;
        if (meta.free_page_count > 0) {
            meta.free_page_count--;
        }
    } else {
        Result<page_id_t> r = disk_->allocate_page();
        if (!r.ok()) {
            return Result<PageGuard>(r.status());
        }
        id = r.value();
    }

    // The page may still be in a frame from an older life, reuse that frame.
    size_t frame = 0;
    std::unordered_map<page_id_t, size_t>::iterator it = table_.find(id);
    if (it != table_.end()) {
        frame = it->second;
        lru_remove(frame);
    } else {
        Status s = pick_victim(&frame);
        if (!s.ok()) {
            return Result<PageGuard>(s);
        }
        table_[id] = frame;
    }

    memset(frame_data(frame), 0, PAGE_SIZE);
    Frame& f = frames_[frame];
    f.page_id = id;
    f.pin_count = 1;
    f.dirty = true;
    f.page_lsn = 0;
    return Result<PageGuard>(PageGuard(this, frame, id));
}

Status BufferPool::free_page(page_id_t id) {
    if (id == META_PAGE_ID) {
        return Status::Internal("tried to free the meta page");
    }

    Result<PageGuard> r = fetch(id);
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();

    // write() takes the lock itself, so call it before locking here.
    uint8_t* data = guard.write();

    std::lock_guard<std::mutex> lock(mu_);
    MetaPage& meta = disk_->meta();
    memset(data, 0, PAGE_SIZE);
    data[0] = PAGE_TYPE_FREE;
    put_u32(data + FREE_NEXT_OFFSET, meta.free_list_head);
    meta.free_list_head = id;
    meta.free_page_count++;
    return Status::Ok();
}

void BufferPool::unpin(size_t frame, bool dirty) {
    std::lock_guard<std::mutex> lock(mu_);
    Frame& f = frames_[frame];
    if (dirty) {
        f.dirty = true;
    }
    if (f.pin_count > 0) {
        f.pin_count--;
    }
    if (f.pin_count == 0) {
        lru_add(frame);
    }
}

void BufferPool::set_dirty(size_t frame) {
    std::lock_guard<std::mutex> lock(mu_);
    frames_[frame].dirty = true;
}

Status BufferPool::flush_page(page_id_t id) {
    std::lock_guard<std::mutex> lock(mu_);
    std::unordered_map<page_id_t, size_t>::iterator it = table_.find(id);
    if (it == table_.end()) {
        return Status::Ok();
    }
    return flush_frame(it->second);
}

Status BufferPool::flush_all() {
    std::lock_guard<std::mutex> lock(mu_);
    for (size_t i = 0; i < frames_.size(); i++) {
        Status s = flush_frame(i);
        if (!s.ok()) {
            return s;
        }
    }
    return Status::Ok();
}

}  // namespace pagedb
