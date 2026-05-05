#ifndef PAGEDB_BTREE_HPP
#define PAGEDB_BTREE_HPP

#include <string>
#include <string_view>
#include <vector>

#include "pagedb/buffer_pool.hpp"
#include "pagedb/page.hpp"
#include "pagedb/status.hpp"

namespace pagedb {

const uint8_t PAGE_TYPE_META = 1;
const uint8_t PAGE_TYPE_INTERNAL = 2;
const uint8_t PAGE_TYPE_LEAF = 3;
const uint8_t PAGE_TYPE_FREE_PAGE = 4;

const size_t NODE_HEADER_SIZE = 24;
const size_t MAX_KEY_SIZE = 64;
const size_t MAX_VALUE_SIZE = 256;

// Node header (all little endian):
//   0  u8  page type
//   1  u8  unused
//   2  u16 number of cells
//   4  u16 start of the cell area
//   6  u16 bytes lost to removed cells
//   8  u32 right child (internal) or next leaf (leaf)
//   12 u64 page lsn
//   20 u32 unused
// After the header there is one u16 slot per cell. Cells are added from the
// end of the page downwards.
class Node {
public:
    explicit Node(uint8_t* data) : p_(data) {}

    void init(uint8_t type);

    uint8_t type() const { return p_[0]; }
    bool is_leaf() const { return p_[0] == PAGE_TYPE_LEAF; }
    int count() const { return (int)get_u16(p_ + 2); }
    uint16_t cell_area() const { return get_u16(p_ + 4); }
    uint16_t dead_bytes() const { return get_u16(p_ + 6); }
    page_id_t extra() const { return get_u32(p_ + 8); }
    uint64_t lsn() const { return get_u64(p_ + 12); }

    void set_count(int n) { put_u16(p_ + 2, (uint16_t)n); }
    void set_cell_area(uint16_t v) { put_u16(p_ + 4, v); }
    void set_dead_bytes(uint16_t v) { put_u16(p_ + 6, v); }
    void set_extra(page_id_t v) { put_u32(p_ + 8, v); }
    void set_lsn(uint64_t v) { put_u64(p_ + 12, v); }

    uint16_t slot(int i) const { return get_u16(p_ + NODE_HEADER_SIZE + 2 * i); }
    void set_slot(int i, uint16_t off) { put_u16(p_ + NODE_HEADER_SIZE + 2 * i, off); }

    std::string_view key_at(int i) const;
    std::string_view value_at(int i) const;
    page_id_t child_at(int i) const;
    void set_child_at(int i, page_id_t child);

    // Index of the first key that is >= the given key. exact is set when the
    // key at that index is the same key.
    int lower_bound(std::string_view key, bool* exact) const;

    size_t free_space() const;
    size_t used_space() const;
    void compact();

    bool insert_leaf_cell(int idx, std::string_view key, std::string_view value);
    bool insert_internal_cell(int idx, std::string_view key, page_id_t child);
    void remove_cell(int idx);

    uint8_t* data() { return p_; }

private:
    size_t cell_size(int i) const;
    uint8_t* cell(int i) const { return p_ + slot(i); }

    uint8_t* p_;
};

int compare_keys(std::string_view a, std::string_view b);

struct KVPair {
    std::string key;
    std::string value;
};

// The tree itself. The root page id lives in the meta page so it survives a
// restart. All pages come from the buffer pool.
class BTree {
public:
    BTree(DiskManager* disk, BufferPool* pool) : disk_(disk), pool_(pool) {}

    Status get(std::string_view key, std::string* out, bool* found);
    Status insert(std::string_view key, std::string_view value);

    Status remove(std::string_view key);
    Status scan(std::string_view start, std::string_view end,
                std::vector<KVPair>* out);

    // Walks the whole tree and checks the rules a B+ tree has to follow.
    // Only used by the tests.
    Status check();

    page_id_t root() const { return disk_->meta().root_page; }

private:
    Status insert_at(page_id_t pid, std::string_view key, std::string_view value,
                     bool* split, std::string* sep_key, page_id_t* right_page);
    Status split_leaf(Node& node, int idx, std::string_view key,
                      std::string_view value, std::string* sep_key,
                      page_id_t* right_page);
    Status find_leaf(std::string_view key, PageGuard* leaf_out);
    Status remove_at(page_id_t pid, std::string_view key, bool* underflow);
    Status fix_child(Node& parent, int idx);
    Status borrow_left(Node& parent, int idx, bool* done);
    Status borrow_right(Node& parent, int idx, bool* done);
    Status merge_children(Node& parent, int j);
    Status shrink_root();
    Status check_node(page_id_t pid, int level, const std::string* lo,
                      const std::string* hi, int* leaf_level);
    Status check_leaf_chain();
    Status split_internal(Node& node, int idx, std::string_view key,
                          page_id_t left_child, page_id_t right_child,
                          std::string* sep_key, page_id_t* right_page);

    DiskManager* disk_;
    BufferPool* pool_;
};

}  // namespace pagedb

#endif
