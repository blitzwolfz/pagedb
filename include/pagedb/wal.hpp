#ifndef PAGEDB_WAL_HPP
#define PAGEDB_WAL_HPP

#include <mutex>
#include <string>

#include "pagedb/disk_manager.hpp"
#include "pagedb/page.hpp"
#include "pagedb/status.hpp"

namespace pagedb {

const uint32_t WAL_MAGIC = 0x314C4157u;  // "WAL1"
const size_t WAL_HEADER_SIZE = 28;

const uint8_t WAL_PAGE_IMAGE = 1;
const uint8_t WAL_COMMIT = 2;

// Append only log. One operation writes the new content of every page it
// changed and then a commit record.
class WalManager {
public:
    WalManager();
    ~WalManager();

    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

    Status open(const std::string& path, bool durable);
    Status close();

    // Reads the log and writes every committed page image into the database
    // file. Has to run before the database is used.
    Status recover(DiskManager* disk, uint32_t* groups_applied);

    Status log_page(page_id_t id, const uint8_t* page);
    Status commit();
    Status truncate();

    uint64_t next_lsn() const { return lsn_; }
    uint64_t bytes_written() const { return bytes_written_; }
    uint64_t commits() const { return commits_; }

private:
    Status write_all(const uint8_t* buf, size_t len);

    int fd_;
    bool durable_;
    uint64_t lsn_;
    uint64_t bytes_written_;
    uint64_t commits_;
    std::string path_;
    std::mutex mu_;
};

}  // namespace pagedb

#endif
