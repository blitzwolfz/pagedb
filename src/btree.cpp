#include "pagedb/btree.hpp"

#include <string.h>

namespace pagedb {

int compare_keys(std::string_view a, std::string_view b) {
    size_t n = a.size() < b.size() ? a.size() : b.size();
    int c = 0;
    if (n > 0) {
        c = memcmp(a.data(), b.data(), n);
    }
    if (c != 0) {
        return c;
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

void Node::init(uint8_t type) {
    memset(p_, 0, PAGE_SIZE);
    p_[0] = type;
    set_count(0);
    set_cell_area((uint16_t)PAGE_SIZE);
    set_dead_bytes(0);
    set_extra(NO_PAGE);
    set_lsn(0);
}

size_t Node::cell_size(int i) const {
    uint8_t* c = cell(i);
    if (is_leaf()) {
        return 4 + (size_t)get_u16(c) + (size_t)get_u16(c + 2);
    }
    return 6 + (size_t)get_u16(c);
}

std::string_view Node::key_at(int i) const {
    uint8_t* c = cell(i);
    size_t klen = get_u16(c);
    size_t off = is_leaf() ? 4 : 6;
    return std::string_view((const char*)c + off, klen);
}

std::string_view Node::value_at(int i) const {
    uint8_t* c = cell(i);
    size_t klen = get_u16(c);
    size_t vlen = get_u16(c + 2);
    return std::string_view((const char*)c + 4 + klen, vlen);
}

page_id_t Node::child_at(int i) const {
    return get_u32(cell(i) + 2);
}

void Node::set_child_at(int i, page_id_t child) {
    put_u32(cell(i) + 2, child);
}

int Node::lower_bound(std::string_view key, bool* exact) const {
    *exact = false;
    int lo = 0;
    int hi = count();
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = compare_keys(key_at(mid), key);
        if (c < 0) {
            lo = mid + 1;
        } else if (c > 0) {
            hi = mid;
        } else {
            *exact = true;
            return mid;
        }
    }
    return lo;
}

size_t Node::free_space() const {
    size_t slots_end = NODE_HEADER_SIZE + 2 * (size_t)count();
    if (cell_area() <= slots_end) {
        return 0;
    }
    return cell_area() - slots_end;
}

size_t Node::used_space() const {
    return PAGE_SIZE - NODE_HEADER_SIZE - free_space() - dead_bytes();
}

// Moves all live cells back to the end of the page so the dead bytes from
// removed cells can be used again.
void Node::compact() {
    uint8_t tmp[PAGE_SIZE];
    memcpy(tmp, p_, PAGE_SIZE);
    Node old(tmp);

    int n = count();
    uint16_t area = (uint16_t)PAGE_SIZE;
    for (int i = 0; i < n; i++) {
        size_t sz = old.cell_size(i);
        area = (uint16_t)(area - sz);
        memcpy(p_ + area, tmp + old.slot(i), sz);
        set_slot(i, area);
    }
    set_cell_area(area);
    set_dead_bytes(0);
}

bool Node::insert_leaf_cell(int idx, std::string_view key, std::string_view value) {
    size_t need = 4 + key.size() + value.size();
    if (free_space() < need + 2) {
        if (free_space() + dead_bytes() < need + 2) {
            return false;
        }
        compact();
    }

    int n = count();
    uint16_t off = (uint16_t)(cell_area() - need);
    uint8_t* c = p_ + off;
    put_u16(c, (uint16_t)key.size());
    put_u16(c + 2, (uint16_t)value.size());
    memcpy(c + 4, key.data(), key.size());
    if (value.size() > 0) {
        memcpy(c + 4 + key.size(), value.data(), value.size());
    }

    for (int i = n; i > idx; i--) {
        set_slot(i, slot(i - 1));
    }
    set_slot(idx, off);
    set_cell_area(off);
    set_count(n + 1);
    return true;
}

bool Node::insert_internal_cell(int idx, std::string_view key, page_id_t child) {
    size_t need = 6 + key.size();
    if (free_space() < need + 2) {
        if (free_space() + dead_bytes() < need + 2) {
            return false;
        }
        compact();
    }

    int n = count();
    uint16_t off = (uint16_t)(cell_area() - need);
    uint8_t* c = p_ + off;
    put_u16(c, (uint16_t)key.size());
    put_u32(c + 2, child);
    memcpy(c + 6, key.data(), key.size());

    for (int i = n; i > idx; i--) {
        set_slot(i, slot(i - 1));
    }
    set_slot(idx, off);
    set_cell_area(off);
    set_count(n + 1);
    return true;
}

void Node::remove_cell(int idx) {
    int n = count();
    size_t sz = cell_size(idx);
    for (int i = idx; i < n - 1; i++) {
        set_slot(i, slot(i + 1));
    }
    set_count(n - 1);
    set_dead_bytes((uint16_t)(dead_bytes() + sz));
}

Status BTree::get(std::string_view key, std::string* out, bool* found) {
    *found = false;
    page_id_t pid = disk_->meta().root_page;
    if (pid == NO_PAGE) {
        return Status::Ok();
    }

    while (true) {
        Result<PageGuard> r = pool_->fetch(pid);
        if (!r.ok()) {
            return r.status();
        }
        PageGuard guard = r.take();
        Node node((uint8_t*)guard.read());

        bool exact = false;
        int idx = node.lower_bound(key, &exact);
        if (node.is_leaf()) {
            if (exact) {
                std::string_view v = node.value_at(idx);
                out->assign(v.data(), v.size());
                *found = true;
            }
            return Status::Ok();
        }

        // Keys that are equal to a separator live in the right subtree.
        if (exact) {
            idx++;
        }
        if (idx == node.count()) {
            pid = node.extra();
        } else {
            pid = node.child_at(idx);
        }
        if (pid == NO_PAGE) {
            return Status::Corruption("internal node points at page 0");
        }
    }
}

Status BTree::insert(std::string_view key, std::string_view value) {
    if (key.size() < 1 || key.size() > MAX_KEY_SIZE) {
        return Status::InvalidArgument("key length must be 1 to 64 bytes");
    }
    if (value.size() > MAX_VALUE_SIZE) {
        return Status::InvalidArgument("value length must be 0 to 256 bytes");
    }

    MetaPage& meta = disk_->meta();
    if (meta.root_page == NO_PAGE) {
        Result<PageGuard> r = pool_->new_page();
        if (!r.ok()) {
            return r.status();
        }
        PageGuard guard = r.take();
        Node node(guard.write());
        node.init(PAGE_TYPE_LEAF);
        meta.root_page = guard.page_id();
    }

    bool split = false;
    std::string sep_key;
    page_id_t right = NO_PAGE;
    Status s = insert_at(meta.root_page, key, value, &split, &sep_key, &right);
    if (!s.ok()) {
        return s;
    }
    if (split) {
        return Status::Internal("root split is not implemented yet");
    }
    return Status::Ok();
}

Status BTree::insert_at(page_id_t pid, std::string_view key, std::string_view value,
                        bool* split, std::string* sep_key, page_id_t* right_page) {
    *split = false;

    Result<PageGuard> r = pool_->fetch(pid);
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();
    Node node(guard.write());

    bool exact = false;
    int idx = node.lower_bound(key, &exact);

    if (node.is_leaf()) {
        if (exact) {
            node.remove_cell(idx);
        }
        if (!node.insert_leaf_cell(idx, key, value)) {
            return Status::Internal("leaf split is not implemented yet");
        }
        return Status::Ok();
    }

    if (exact) {
        idx++;
    }
    page_id_t child = (idx == node.count()) ? node.extra() : node.child_at(idx);

    bool child_split = false;
    std::string child_sep;
    page_id_t child_right = NO_PAGE;
    guard.drop();
    Status s = insert_at(child, key, value, &child_split, &child_sep, &child_right);
    if (!s.ok()) {
        return s;
    }
    if (!child_split) {
        return Status::Ok();
    }
    return Status::Internal("internal split is not implemented yet");
}

}  // namespace pagedb
