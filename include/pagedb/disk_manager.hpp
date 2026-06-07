#ifndef PAGEDB_DISK_MANAGER_HPP
#define PAGEDB_DISK_MANAGER_HPP

#include <string>

#include "pagedb/page.hpp"
#include "pagedb/status.hpp"

namespace pagedb {

const uint32_t FORMAT_VERSION = 1;

// Page 0 of the file. Fixed offsets, not a slotted page.
struct MetaPage {
    uint32_t format_version;
    uint32_t page_size;
    page_id_t root_page;
    uint32_t page_count;
    page_id_t free_list_head;
    uint32_t free_page_count;

    MetaPage() {
        format_version = FORMAT_VERSION;
        page_size = (uint32_t)PAGE_SIZE;
        root_page = NO_PAGE;
        page_count = 1;
        free_list_head = NO_PAGE;
        free_page_count = 0;
    }
};

// Owns the database file. All reads and writes go through here.
class DiskManager {
public:
    DiskManager();
    ~DiskManager();

    DiskManager(const DiskManager&) = delete;
    DiskManager& operator=(const DiskManager&) = delete;

    Status open(const std::string& path);
    Status close();
    bool is_open() const { return fd_ >= 0; }

    Status read_page(page_id_t id, uint8_t* out);
    Status write_page(page_id_t id, const uint8_t* in);

    // Grows the file by one page and returns the new page id.
    Result<page_id_t> allocate_page();

    Status sync();

    // Used by log replay: writes a page even when the meta page does not know
    // about it yet, and grows the file if it has to.
    Status write_raw_page(page_id_t id, const uint8_t* in);
    Status reload_meta();

    Status write_meta();
    MetaPage& meta() { return meta_; }
    uint32_t page_count() const { return meta_.page_count; }

private:
    Status read_at(off_t off, uint8_t* buf, size_t len);
    Status write_at(off_t off, const uint8_t* buf, size_t len);
    Status init_new_file();
    Status load_meta();

    int fd_;
    std::string path_;
    MetaPage meta_;
};

}  // namespace pagedb

#endif
