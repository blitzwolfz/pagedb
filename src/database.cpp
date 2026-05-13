#include "pagedb/database.hpp"

namespace pagedb {

// The tree needs a few pages pinned at the same time while it splits or
// merges nodes, so a very small pool can not work.
static const size_t MIN_POOL_PAGES = 16;

Database::~Database() {
    close();
    delete tree_;
    delete pool_;
    delete disk_;
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
    return tree_->insert(key, value);
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
    return tree_->remove(key);
}

Result<std::vector<KVPair>> Database::scan(std::string_view start_inclusive,
                                           std::string_view end_exclusive) {
    std::shared_lock<std::shared_mutex> lock(mu_);
    if (closed_) {
        return Result<std::vector<KVPair>>(Status::Internal("database is closed"));
    }
    if (start_inclusive.size() > MAX_KEY_SIZE || end_exclusive.size() > MAX_KEY_SIZE) {
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

Status Database::close() {
    std::unique_lock<std::shared_mutex> lock(mu_);
    if (closed_) {
        return Status::Ok();
    }
    closed_ = true;

    Status s = pool_->flush_all();
    Status s2 = disk_->close();
    if (!s.ok()) {
        return s;
    }
    return s2;
}

}  // namespace pagedb
