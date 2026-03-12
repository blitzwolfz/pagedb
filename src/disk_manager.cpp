#include "pagedb/disk_manager.hpp"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "pagedb/crc32.hpp"

namespace pagedb {

static const char MAGIC[8] = {'P', 'A', 'G', 'E', 'D', 'B', '0', '1'};

static std::string errno_msg(const std::string& what) {
    std::string s = what;
    s += " (";
    s += strerror(errno);
    s += ")";
    return s;
}

DiskManager::DiskManager() {
    fd_ = -1;
}

DiskManager::~DiskManager() {
    if (fd_ >= 0) {
        close();
    }
}

Status DiskManager::open(const std::string& path) {
    if (fd_ >= 0) {
        return Status::Internal("disk manager already open");
    }

    bool created = false;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        created = true;
    }

    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return Status::IoError(errno_msg("open " + path));
    }

    // Only one writer process at a time.
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return Status::Locked("database file is locked by another process");
    }

    fd_ = fd;
    path_ = path;

    if (::fstat(fd_, &st) != 0) {
        ::close(fd_);
        fd_ = -1;
        return Status::IoError(errno_msg("fstat"));
    }

    Status s;
    if (st.st_size == 0) {
        s = init_new_file();
        if (s.ok() && created) {
            // Make sure the new file entry itself is on disk.
            std::string dir = path;
            size_t slash = dir.find_last_of('/');
            if (slash == std::string::npos) {
                dir = ".";
            } else {
                dir = dir.substr(0, slash);
            }
            int dfd = ::open(dir.c_str(), O_RDONLY);
            if (dfd >= 0) {
                ::fsync(dfd);
                ::close(dfd);
            }
        }
    } else {
        s = load_meta();
    }

    if (!s.ok()) {
        ::close(fd_);
        fd_ = -1;
    }
    return s;
}

Status DiskManager::init_new_file() {
    meta_ = MetaPage();
    Status s = write_meta();
    if (!s.ok()) {
        return s;
    }
    return sync();
}

Status DiskManager::load_meta() {
    uint8_t buf[PAGE_SIZE];
    Status s = read_at(0, buf, PAGE_SIZE);
    if (!s.ok()) {
        return s;
    }

    if (memcmp(buf, MAGIC, 8) != 0) {
        return Status::Corruption("bad magic, not a pagedb file");
    }

    uint32_t stored_crc = get_u32(buf + PAGE_SIZE - 4);
    uint32_t real_crc = crc32(buf, PAGE_SIZE - 4);
    if (stored_crc != real_crc) {
        return Status::Corruption("meta page checksum mismatch");
    }

    meta_.format_version = get_u32(buf + 8);
    meta_.page_size = get_u32(buf + 12);
    meta_.root_page = get_u32(buf + 16);
    meta_.page_count = get_u32(buf + 20);

    if (meta_.format_version != FORMAT_VERSION) {
        return Status::Corruption("unsupported format version");
    }
    if (meta_.page_size != PAGE_SIZE) {
        return Status::Corruption("page size does not match this build");
    }
    if (meta_.page_count < 1) {
        return Status::Corruption("page count is zero");
    }

    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        return Status::IoError(errno_msg("fstat"));
    }
    if ((uint64_t)st.st_size < (uint64_t)meta_.page_count * PAGE_SIZE) {
        return Status::Corruption("file is shorter than page count says");
    }
    return Status::Ok();
}

Status DiskManager::write_meta() {
    uint8_t buf[PAGE_SIZE];
    memset(buf, 0, PAGE_SIZE);
    memcpy(buf, MAGIC, 8);
    put_u32(buf + 8, meta_.format_version);
    put_u32(buf + 12, meta_.page_size);
    put_u32(buf + 16, meta_.root_page);
    put_u32(buf + 20, meta_.page_count);
    put_u32(buf + PAGE_SIZE - 4, crc32(buf, PAGE_SIZE - 4));
    return write_at(0, buf, PAGE_SIZE);
}

Status DiskManager::read_page(page_id_t id, uint8_t* out) {
    if (fd_ < 0) {
        return Status::Internal("read on closed file");
    }
    if (id >= meta_.page_count) {
        return Status::InvalidArgument("page id out of range");
    }
    return read_at((off_t)id * (off_t)PAGE_SIZE, out, PAGE_SIZE);
}

Status DiskManager::write_page(page_id_t id, const uint8_t* in) {
    if (fd_ < 0) {
        return Status::Internal("write on closed file");
    }
    if (id >= meta_.page_count) {
        return Status::InvalidArgument("page id out of range");
    }
    return write_at((off_t)id * (off_t)PAGE_SIZE, in, PAGE_SIZE);
}

Result<page_id_t> DiskManager::allocate_page() {
    if (fd_ < 0) {
        return Result<page_id_t>(Status::Internal("allocate on closed file"));
    }
    if (meta_.page_count == 0xFFFFFFFFu) {
        return Result<page_id_t>(Status::Internal("out of page ids"));
    }

    page_id_t id = meta_.page_count;
    meta_.page_count++;

    uint8_t zero[PAGE_SIZE];
    memset(zero, 0, PAGE_SIZE);
    Status s = write_at((off_t)id * (off_t)PAGE_SIZE, zero, PAGE_SIZE);
    if (!s.ok()) {
        meta_.page_count--;
        return Result<page_id_t>(s);
    }
    return Result<page_id_t>(id);
}

Status DiskManager::sync() {
    if (fd_ < 0) {
        return Status::Internal("sync on closed file");
    }
    if (::fsync(fd_) != 0) {
        return Status::IoError(errno_msg("fsync"));
    }
    return Status::Ok();
}

Status DiskManager::close() {
    if (fd_ < 0) {
        return Status::Ok();
    }
    Status s = write_meta();
    if (s.ok()) {
        s = sync();
    }
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
    return s;
}

Status DiskManager::read_at(off_t off, uint8_t* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pread(fd_, buf + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status::IoError(errno_msg("pread"));
        }
        if (n == 0) {
            return Status::Corruption("short read, file is truncated");
        }
        done += (size_t)n;
    }
    return Status::Ok();
}

Status DiskManager::write_at(off_t off, const uint8_t* buf, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pwrite(fd_, buf + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status::IoError(errno_msg("pwrite"));
        }
        done += (size_t)n;
    }
    return Status::Ok();
}

}  // namespace pagedb
