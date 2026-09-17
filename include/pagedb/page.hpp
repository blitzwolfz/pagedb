#ifndef PAGEDB_PAGE_HPP
#define PAGEDB_PAGE_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pagedb {

typedef uint32_t page_id_t;

const size_t PAGE_SIZE = 4096;

const uint8_t PAGE_TYPE_META = 1;
const uint8_t PAGE_TYPE_INTERNAL = 2;
const uint8_t PAGE_TYPE_LEAF = 3;
const uint8_t PAGE_TYPE_FREE = 4;

const page_id_t META_PAGE_ID = 0;
const page_id_t NO_PAGE = 0;

// Everything on disk is little endian, we never write a struct directly.
inline void put_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

inline void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

inline void put_u64(uint8_t* p, uint64_t v) {
    put_u32(p, (uint32_t)(v & 0xffffffffull));
    put_u32(p + 4, (uint32_t)(v >> 32));
}

inline uint16_t get_u16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

inline uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

inline uint64_t get_u64(const uint8_t* p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

}  // namespace pagedb

#endif
