#include "pagedb/database.hpp"

namespace pagedb {

// The tree needs a few pages pinned at the same time while it splits or
// merges nodes, so a very small pool can not work.
static const size_t MIN_POOL_PAGES = 16;

Database::~Database() {
    close();
    delete tree_;
    delete pool_;
    delete wal_;
    delete disk_;
}

static bool meta_changed(const MetaPage& a, const MetaPage& b) {
    return a.root_page != b.root_page || a.page_count != b.page_count ||
           a.free_list_head != b.free_list_head ||
           a.free_page_count != b.free_page_count;
}

Result<std::unique_ptr<Database>> Database::open(const DatabaseOptions& options) {
    if (options.path.empty()) {
        return Result<std::unique_ptr<Database>>(
            Status::InvalidArgument("database path is empty"));
    }
    size_t frames = options.buffer_pool_pages;
    if (frames < MIN_POOL_PAGES) {
        frames = MIN_POOL_PAGES;
    }

    std::unique_ptr<Database> db(new Database());
    db->disk_ = new DiskManager();
    Status s = db->disk_->open(options.path.string());
    if (!s.ok()) {
        return Result<std::unique_ptr<Database>>(s);
    }
    db->durable_ = options.durable;
    db->checkpoint_bytes_ = options.checkpoint_bytes;
    db->logged_since_checkpoint_ = 0;
    db->wal_ = new WalManager();
    std::string wal_path = options.path.string() + ".wal";
    s = db->wal_->open(wal_path, options.durable);
    if (!s.ok()) {
        return Result<std::unique_ptr<Database>>(s);
    }

    // Anything the log holds from a crashed run goes into the file first.
    uint32_t groups = 0;
    s = db->wal_->recover(db->disk_, &groups);
    if (!s.ok()) {
        return Result<std::unique_ptr<Database>>(s);
    }

    db->pool_ = new BufferPool(db->disk_, frames);
    db->tree_ = new BTree(db->disk_, db->pool_);
    return Result<std::unique_ptr<Database>>(std::move(db));
}

Status Database::put(std::string_view key, std::string_view value) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (closed_) {
        return Status::Internal("database is closed");
    }
    if (key.size() < 1 || key.size() > MAX_KEY_SIZE) {
        return Status::InvalidArgument("key length must be 1 to 64 bytes");
    }
    if (value.size() > MAX_VALUE_SIZE) {
        return Status::InvalidArgument("value length must be 0 to 256 bytes");
    }
    if (damaged_) {
        return Status::Internal("a write failed before, open the database again");
    }

    MetaPage before = disk_->meta();
    pool_->begin_operation();
    Status s = tree_->insert(key, value);
    if (!s.ok()) {
        pool_->end_operation();
        damaged_ = true;
        return s;
    }
    return log_operation(before);
}

// Writes the new content of every page the operation touched into the log and
// then the commit record. After that the pages may go to the database file.
Status Database::log_operation(const MetaPage& before) {
    std::vector<page_id_t> pages;
    pool_->operation_pages(&pages);

    uint8_t buf[PAGE_SIZE];
    for (size_t i = 0; i < pages.size(); i++) {
        Status s = pool_->copy_page(pages[i], buf);
        if (!s.ok()) {
            pool_->end_operation();
            damaged_ = true;
            return s;
        }
        s = wal_->log_page(pages[i], buf);
        if (!s.ok()) {
            pool_->end_operation();
            damaged_ = true;
            return s;
        }
    }

    if (meta_changed(before, disk_->meta())) {
        disk_->encode_meta(buf);
        Status s = wal_->log_page(META_PAGE_ID, buf);
        if (!s.ok()) {
            pool_->end_operation();
            damaged_ = true;
            return s;
        }
    }

    Status s = wal_->commit();
    pool_->end_operation();
    if (!s.ok()) {
        damaged_ = true;
        return s;
    }

    // The log would grow forever otherwise.
    if (checkpoint_bytes_ > 0 &&
        wal_->bytes_written() - logged_since_checkpoint_ > checkpoint_bytes_) {
        s = checkpoint_locked();
        if (!s.ok()) {
            damaged_ = true;
        }
    }
    return s;
}

Result<std::optional<std::string>> Database::get(std::string_view key) {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (closed_) {
        return Result<std::optional<std::string>>(
            Status::Internal("database is closed"));
    }
    if (key.size() < 1 || key.size() > MAX_KEY_SIZE) {
        return Result<std::optional<std::string>>(
            Status::InvalidArgument("key length must be 1 to 64 bytes"));
    }

    std::string value;
    bool found = false;
    Status s = tree_->get(key, &value, &found);
    if (!s.ok()) {
        return Result<std::optional<std::string>>(s);
    }
    if (!found) {
        return Result<std::optional<std::string>>(std::optional<std::string>());
    }
    return Result<std::optional<std::string>>(std::optional<std::string>(value));
}

Status Database::remove(std::string_view key) {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (closed_) {
        return Status::Internal("database is closed");
    }
    if (key.size() < 1 || key.size() > MAX_KEY_SIZE) {
        return Status::InvalidArgument("key length must be 1 to 64 bytes");
    }
    if (damaged_) {
        return Status::Internal("a write failed before, open the database again");
    }

    MetaPage before = disk_->meta();
    pool_->begin_operation();
    Status s = tree_->remove(key);
    if (!s.ok()) {
        pool_->end_operation();
        if (s.code() != Code::NotFound) {
            damaged_ = true;
        }
        return s;
    }
    return log_operation(before);
}

Result<std::vector<KVPair>> Database::scan(std::string_view start_inclusive,
                                           std::string_view end_exclusive) {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (closed_) {
        return Result<std::vector<KVPair>>(Status::Internal("database is closed"));
    }
    if (start_inclusive.size() > MAX_KEY_SIZE ||
        end_exclusive.size() > MAX_KEY_SIZE) {
        return Result<std::vector<KVPair>>(
            Status::InvalidArgument("key length must be 1 to 64 bytes"));
    }

    std::vector<KVPair> out;
    Status s = tree_->scan(start_inclusive, end_exclusive, &out);
    if (!s.ok()) {
        return Result<std::vector<KVPair>>(s);
    }
    return Result<std::vector<KVPair>>(out);
}

Status Database::checkpoint() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (closed_ || pool_ == 0) {
        return Status::Internal("database is closed");
    }
    if (damaged_) {
        return Status::Internal("a write failed before, open the database again");
    }
    return checkpoint_locked();
}

// The caller already holds the database lock.
Status Database::checkpoint_locked() {
    Status s = pool_->flush_all();
    if (!s.ok()) {
        return s;
    }
    s = disk_->write_meta();
    if (!s.ok()) {
        return s;
    }
    s = disk_->sync();
    if (!s.ok()) {
        return s;
    }
    // Only now, when the file has everything, the log can go away.
    s = wal_->truncate();
    if (s.ok()) {
        logged_since_checkpoint_ = wal_->bytes_written();
    }
    return s;
}

Status Database::verify() {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (closed_ || tree_ == 0) {
        return Status::Internal("database is closed");
    }
    return tree_->check();
}

Status Database::close() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (closed_) {
        return Status::Ok();
    }
    closed_ = true;
    if (pool_ == 0 || disk_ == 0) {
        if (wal_ != 0) {
            wal_->close();
        }
        return Status::Ok();
    }

    Status s;
    if (damaged_) {
        // The pages in memory may hold half of a failed write, so they are
        // dropped. The meta page in memory can be half way as well, so the
        // one from the file is put back before closing. The log still has
        // every operation that was committed.
        disk_->reload_meta();
        s = disk_->close();
        wal_->close();
        return s;
    }

    s = pool_->flush_all();
    if (s.ok()) {
        s = disk_->write_meta();
    }
    if (s.ok()) {
        s = disk_->sync();
    }
    if (s.ok()) {
        s = wal_->truncate();
    }
    Status s2 = disk_->close();
    Status s3 = wal_->close();
    if (!s.ok()) {
        return s;
    }
    if (!s2.ok()) {
        return s2;
    }
    return s3;
}

}  // namespace pagedb
