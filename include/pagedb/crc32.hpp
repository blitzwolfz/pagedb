#ifndef PAGEDB_CRC32_HPP
#define PAGEDB_CRC32_HPP

#include <cstddef>
#include <cstdint>

namespace pagedb {

uint32_t crc32(const void* data, size_t len);

}

#endif
