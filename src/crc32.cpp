#include "pagedb/crc32.hpp"

namespace pagedb {

namespace {

struct Crc32Table {
    uint32_t v[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                if (c & 1) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            v[i] = c;
        }
    }
};

const Crc32Table& table() {
    static Crc32Table t;
    return t;
}

}  // namespace

uint32_t crc32(const void* data, size_t len) {
    const Crc32Table& t = table();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c = t.v[(c ^ p[i]) & 0xff] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace pagedb
