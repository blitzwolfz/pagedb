#ifndef PAGEDB_DATABASE_HPP
#define PAGEDB_DATABASE_HPP

#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "pagedb/btree.hpp"
#include "pagedb/buffer_pool.hpp"
#include "pagedb/disk_manager.hpp"
#include "pagedb/status.hpp"
#include "pagedb/wal.hpp"

namespace pagedb {

struct DatabaseOptions {
    std::filesystem::path path;
    std::size_t buffer_pool_pages = 1024;
    bool durable = true;
};

// One open database file. Reads can run at the same time, writes are done one
// after the other.
class Database {
public:
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    static Result<std::unique_ptr<Database>> open(const DatabaseOptions& options);

    Status put(std::string_view key, std::string_view value);
    Result<std::optional<std::string>> get(std::string_view key);
    Status remove(std::string_view key);
    Result<std::vector<KVPair>> scan(std::string_view start_inclusive,
                                     std::string_view end_exclusive);
    // Writes every dirty page into the database file, syncs it and empties
    // the log.
    Status checkpoint();
    Status close();

    const BufferPool& pool() const { return *pool_; }
    const WalManager& wal() const { return *wal_; }

private:
    Database()
        : disk_(0), pool_(0), tree_(0), wal_(0), durable_(true), closed_(false),
          damaged_(false) {}

    Status log_operation(const MetaPage& before);

    DiskManager* disk_;
    BufferPool* pool_;
    BTree* tree_;
    WalManager* wal_;
    bool durable_;
    bool closed_;
    // Set when a write failed half way. The pages in memory can not be
    // trusted after that, so the database has to be opened again.
    bool damaged_;
    std::shared_mutex mu_;
};

}  // namespace pagedb

#endif
