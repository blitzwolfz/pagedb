#include "pagedb/wal.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "pagedb/crc32.hpp"

namespace pagedb {

WalManager::WalManager() {
    fd_ = -1;
    durable_ = true;
    lsn_ = 1;
    bytes_written_ = 0;
    commits_ = 0;
}

WalManager::~WalManager() {
    if (fd_ >= 0) {
        close();
    }
}

Status WalManager::open(const std::string& path, bool durable) {
    if (fd_ >= 0) {
        return Status::Internal("log is already open");
    }
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        std::string m = "open log (";
        m += strerror(errno);
        m += ")";
        return Status::IoError(m);
    }
    fd_ = fd;
    path_ = path;
    durable_ = durable;
    return Status::Ok();
}

Status WalManager::close() {
    if (fd_ < 0) {
        return Status::Ok();
    }
    int rc = ::fsync(fd_);
    ::close(fd_);
    fd_ = -1;
    if (rc != 0) {
        return Status::IoError("fsync log on close");
    }
    return Status::Ok();
}

Status WalManager::write_all(const uint8_t* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::write(fd_, buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::string m = "write log (";
            m += strerror(errno);
            m += ")";
            return Status::IoError(m);
        }
        done += (size_t)n;
    }
    bytes_written_ += len;
    return Status::Ok();
}

Status WalManager::log_page(page_id_t id, const uint8_t* page) {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0) {
        return Status::Internal("log is not open");
    }

    uint8_t buf[WAL_HEADER_SIZE + PAGE_SIZE];
    memset(buf, 0, WAL_HEADER_SIZE);
    put_u32(buf, WAL_MAGIC);
    put_u64(buf + 4, lsn_);
    buf[12] = WAL_PAGE_IMAGE;
    put_u32(buf + 16, id);
    put_u32(buf + 20, (uint32_t)PAGE_SIZE);
    memcpy(buf + WAL_HEADER_SIZE, page, PAGE_SIZE);
    put_u32(buf + 24, crc32(buf, WAL_HEADER_SIZE + PAGE_SIZE));

    Status s = write_all(buf, WAL_HEADER_SIZE + PAGE_SIZE);
    if (!s.ok()) {
        return s;
    }
    lsn_++;
    return Status::Ok();
}

Status WalManager::commit() {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0) {
        return Status::Internal("log is not open");
    }

    uint8_t buf[WAL_HEADER_SIZE];
    memset(buf, 0, WAL_HEADER_SIZE);
    put_u32(buf, WAL_MAGIC);
    put_u64(buf + 4, lsn_);
    buf[12] = WAL_COMMIT;
    put_u32(buf + 20, 0);
    put_u32(buf + 24, crc32(buf, WAL_HEADER_SIZE));

    Status s = write_all(buf, WAL_HEADER_SIZE);
    if (!s.ok()) {
        return s;
    }
    lsn_++;
    commits_++;

    if (durable_) {
        if (::fsync(fd_) != 0) {
            return Status::IoError("fsync log");
        }
    }
    return Status::Ok();
}

Status WalManager::truncate() {
    std::lock_guard<std::mutex> lock(mu_);
    if (fd_ < 0) {
        return Status::Internal("log is not open");
    }
    if (::ftruncate(fd_, 0) != 0) {
        return Status::IoError("truncate log");
    }
    if (::fsync(fd_) != 0) {
        return Status::IoError("fsync log after truncate");
    }
    return Status::Ok();
}

Status WalManager::recover(DiskManager* disk, uint32_t* groups_applied) {
    std::lock_guard<std::mutex> lock(mu_);
    *groups_applied = 0;
    if (fd_ < 0) {
        return Status::Internal("log is not open");
    }

    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        return Status::IoError("fstat log");
    }
    off_t size = st.st_size;
    if (size == 0) {
        return Status::Ok();
    }

    std::vector<page_id_t> group_pages;
    std::vector<std::vector<uint8_t> > group_data;
    off_t off = 0;
    uint64_t last_lsn = 0;
    bool wrote_something = false;

    while (off + (off_t)WAL_HEADER_SIZE <= size) {
        uint8_t header[WAL_HEADER_SIZE];
        ssize_t n = ::pread(fd_, header, WAL_HEADER_SIZE, off);
        if (n != (ssize_t)WAL_HEADER_SIZE) {
            break;
        }
        if (get_u32(header) != WAL_MAGIC) {
            break;
        }

        uint64_t lsn = get_u64(header + 4);
        uint8_t type = header[12];
        page_id_t page = get_u32(header + 16);
        uint32_t len = get_u32(header + 20);
        uint32_t want_crc = get_u32(header + 24);
        if (len > PAGE_SIZE) {
            break;
        }
        if (off + (off_t)WAL_HEADER_SIZE + (off_t)len > size) {
            break;  // torn record at the end
        }

        std::vector<uint8_t> record(WAL_HEADER_SIZE + len);
        n = ::pread(fd_, &record[0], record.size(), off);
        if (n != (ssize_t)record.size()) {
            break;
        }
        put_u32(&record[24], 0);
        if (crc32(&record[0], record.size()) != want_crc) {
            break;
        }

        if (type == WAL_PAGE_IMAGE) {
            if (len != PAGE_SIZE) {
                break;
            }
            group_pages.push_back(page);
            std::vector<uint8_t> copy(record.begin() + WAL_HEADER_SIZE,
                                      record.end());
            group_data.push_back(copy);
        } else if (type == WAL_COMMIT) {
            for (size_t i = 0; i < group_pages.size(); i++) {
                Status s = disk->write_raw_page(group_pages[i], &group_data[i][0]);
                if (!s.ok()) {
                    return s;
                }
                wrote_something = true;
            }
            group_pages.clear();
            group_data.clear();
            (*groups_applied)++;
        } else {
            break;
        }

        last_lsn = lsn;
        off += (off_t)WAL_HEADER_SIZE + (off_t)len;
    }

    // a group without a commit record is dropped
    group_pages.clear();
    group_data.clear();
    lsn_ = last_lsn + 1;

    if (wrote_something) {
        Status s = disk->sync();
        if (!s.ok()) {
            return s;
        }
        s = disk->reload_meta();
        if (!s.ok()) {
            return s;
        }
    }

    if (::ftruncate(fd_, 0) != 0) {
        return Status::IoError("truncate log after replay");
    }
    if (::fsync(fd_) != 0) {
        return Status::IoError("fsync log after replay");
    }
    return Status::Ok();
}

}  // namespace pagedb
