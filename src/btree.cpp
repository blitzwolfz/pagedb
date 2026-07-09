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

// Walks down to the leaf that would hold the key.
Status BTree::find_leaf(std::string_view key, PageGuard* leaf_out) {
    page_id_t pid = disk_->meta().root_page;
    if (pid == NO_PAGE) {
        return Status::NotFound("tree is empty");
    }

    while (true) {
        Result<PageGuard> r = pool_->fetch(pid);
        if (!r.ok()) {
            return r.status();
        }
        PageGuard guard = r.take();
        Node node((uint8_t*)guard.read());
        if (node.is_leaf()) {
            *leaf_out = std::move(guard);
            return Status::Ok();
        }

        bool exact = false;
        int idx = node.lower_bound(key, &exact);
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

Status BTree::scan(std::string_view start, std::string_view end,
                   std::vector<KVPair>* out) {
    out->clear();
    if (compare_keys(start, end) >= 0) {
        return Status::Ok();
    }
    if (disk_->meta().root_page == NO_PAGE) {
        return Status::Ok();
    }

    PageGuard leaf;
    Status s = find_leaf(start, &leaf);
    if (!s.ok()) {
        if (s.code() == Code::NotFound) {
            return Status::Ok();
        }
        return s;
    }

    bool exact = false;
    Node node((uint8_t*)leaf.read());
    int idx = node.lower_bound(start, &exact);

    while (true) {
        Node cur((uint8_t*)leaf.read());
        int n = cur.count();
        for (int i = idx; i < n; i++) {
            std::string_view k = cur.key_at(i);
            if (compare_keys(k, end) >= 0) {
                return Status::Ok();
            }
            KVPair kv;
            kv.key = std::string(k);
            std::string_view v = cur.value_at(i);
            kv.value = std::string(v);
            out->push_back(kv);
        }

        page_id_t next = cur.extra();
        if (next == NO_PAGE) {
            return Status::Ok();
        }
        leaf.drop();
        Result<PageGuard> r = pool_->fetch(next);
        if (!r.ok()) {
            return r.status();
        }
        leaf = r.take();
        idx = 0;
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
        // The old root was split in two, so we need a new root above it.
        Result<PageGuard> r = pool_->new_page();
        if (!r.ok()) {
            return r.status();
        }
        PageGuard guard = r.take();
        Node root(guard.write());
        root.init(PAGE_TYPE_INTERNAL);
        root.insert_internal_cell(0, sep_key, meta.root_page);
        root.set_extra(right);
        meta.root_page = guard.page_id();
    }
    return Status::Ok();
}

// A node that holds less than this many bytes is too empty and is either
// filled up from a sibling or merged with one.
static const size_t NODE_CAPACITY = PAGE_SIZE - NODE_HEADER_SIZE;
static const size_t MIN_FILL = NODE_CAPACITY / 2;

static page_id_t child_ptr(Node& n, int i) {
    if (i == n.count()) {
        return n.extra();
    }
    return n.child_at(i);
}

static void set_child_ptr(Node& n, int i, page_id_t v) {
    if (i == n.count()) {
        n.set_extra(v);
    } else {
        n.set_child_at(i, v);
    }
}

// A separator key can only be replaced when the new key still fits.
static bool can_replace_key(Node& parent, int i, std::string_view new_key) {
    size_t old_size = 6 + parent.key_at(i).size();
    size_t new_size = 6 + new_key.size();
    if (new_size <= old_size) {
        return true;
    }
    return new_size <= parent.free_space() + parent.dead_bytes() + old_size;
}

static bool replace_key(Node& parent, int i, std::string_view new_key) {
    page_id_t child = parent.child_at(i);
    std::string keep = std::string(parent.key_at(i));
    parent.remove_cell(i);
    if (parent.insert_internal_cell(i, new_key, child)) {
        return true;
    }
    parent.insert_internal_cell(i, keep, child);
    return false;
}

Status BTree::remove(std::string_view key) {
    if (key.size() < 1 || key.size() > MAX_KEY_SIZE) {
        return Status::InvalidArgument("key length must be 1 to 64 bytes");
    }
    page_id_t root = disk_->meta().root_page;
    if (root == NO_PAGE) {
        return Status::NotFound("key does not exist");
    }

    bool underflow = false;
    Status s = remove_at(root, key, &underflow);
    if (!s.ok()) {
        return s;
    }
    return shrink_root();
}

Status BTree::remove_at(page_id_t pid, std::string_view key, bool* underflow) {
    *underflow = false;

    Result<PageGuard> r = pool_->fetch(pid);
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();
    Node node((uint8_t*)guard.read());

    bool exact = false;
    int idx = node.lower_bound(key, &exact);

    if (node.is_leaf()) {
        if (!exact) {
            return Status::NotFound("key does not exist");
        }
        Node w(guard.write());
        w.remove_cell(idx);
        *underflow = w.used_space() < MIN_FILL;
        return Status::Ok();
    }

    if (exact) {
        idx++;
    }
    page_id_t child = child_ptr(node, idx);
    if (child == NO_PAGE) {
        return Status::Corruption("internal node points at page 0");
    }
    guard.drop();

    bool child_underflow = false;
    Status s = remove_at(child, key, &child_underflow);
    if (!s.ok()) {
        return s;
    }
    if (!child_underflow) {
        return Status::Ok();
    }

    Result<PageGuard> r2 = pool_->fetch(pid);
    if (!r2.ok()) {
        return r2.status();
    }
    PageGuard parent_guard = r2.take();
    Node parent(parent_guard.write());

    bool exact2 = false;
    idx = parent.lower_bound(key, &exact2);
    if (exact2) {
        idx++;
    }

    s = fix_child(parent, idx);
    if (!s.ok()) {
        return s;
    }
    *underflow = parent.used_space() < MIN_FILL;
    return Status::Ok();
}

// Takes one entry from a sibling, or merges with it when no sibling can give
// anything away.
Status BTree::fix_child(Node& parent, int idx) {
    if (idx > 0) {
        bool done = false;
        Status s = borrow_left(parent, idx, &done);
        if (!s.ok()) {
            return s;
        }
        if (done) {
            return Status::Ok();
        }
    }
    if (idx < parent.count()) {
        bool done = false;
        Status s = borrow_right(parent, idx, &done);
        if (!s.ok()) {
            return s;
        }
        if (done) {
            return Status::Ok();
        }
    }
    if (idx > 0) {
        return merge_children(parent, idx - 1);
    }
    if (parent.count() > 0) {
        return merge_children(parent, idx);
    }
    return Status::Ok();
}

Status BTree::borrow_left(Node& parent, int idx, bool* done) {
    *done = false;
    page_id_t left_id = child_ptr(parent, idx - 1);
    page_id_t child_id = child_ptr(parent, idx);

    Result<PageGuard> rl = pool_->fetch(left_id);
    if (!rl.ok()) {
        return rl.status();
    }
    PageGuard lg = rl.take();
    Result<PageGuard> rc = pool_->fetch(child_id);
    if (!rc.ok()) {
        return rc.status();
    }
    PageGuard cg = rc.take();

    Node left(lg.write());
    Node child(cg.write());
    int last = left.count() - 1;
    if (last < 0) {
        return Status::Ok();
    }

    if (left.is_leaf()) {
        std::string k = std::string(left.key_at(last));
        std::string v = std::string(left.value_at(last));
        if (left.used_space() - (6 + k.size() + v.size()) < MIN_FILL) {
            return Status::Ok();
        }
        if (!can_replace_key(parent, idx - 1, k)) {
            return Status::Ok();
        }
        if (!child.insert_leaf_cell(0, k, v)) {
            return Status::Ok();
        }
        left.remove_cell(last);
        if (!replace_key(parent, idx - 1, k)) {
            return Status::Internal("could not update the separator key");
        }
    } else {
        std::string k = std::string(left.key_at(last));
        page_id_t moved = left.extra();
        std::string sep = std::string(parent.key_at(idx - 1));
        if (left.used_space() - (6 + k.size()) < MIN_FILL) {
            return Status::Ok();
        }
        if (!can_replace_key(parent, idx - 1, k)) {
            return Status::Ok();
        }
        if (!child.insert_internal_cell(0, sep, moved)) {
            return Status::Ok();
        }
        left.set_extra(left.child_at(last));
        left.remove_cell(last);
        if (!replace_key(parent, idx - 1, k)) {
            return Status::Internal("could not update the separator key");
        }
    }

    *done = true;
    return Status::Ok();
}

Status BTree::borrow_right(Node& parent, int idx, bool* done) {
    *done = false;
    page_id_t child_id = child_ptr(parent, idx);
    page_id_t right_id = child_ptr(parent, idx + 1);

    Result<PageGuard> rc = pool_->fetch(child_id);
    if (!rc.ok()) {
        return rc.status();
    }
    PageGuard cg = rc.take();
    Result<PageGuard> rr = pool_->fetch(right_id);
    if (!rr.ok()) {
        return rr.status();
    }
    PageGuard rg = rr.take();

    Node child(cg.write());
    Node right(rg.write());
    if (right.count() < 1) {
        return Status::Ok();
    }

    if (right.is_leaf()) {
        std::string k = std::string(right.key_at(0));
        std::string v = std::string(right.value_at(0));
        if (right.used_space() - (6 + k.size() + v.size()) < MIN_FILL) {
            return Status::Ok();
        }
        if (right.count() < 2) {
            return Status::Ok();
        }
        std::string next_key = std::string(right.key_at(1));
        if (!can_replace_key(parent, idx, next_key)) {
            return Status::Ok();
        }
        if (!child.insert_leaf_cell(child.count(), k, v)) {
            return Status::Ok();
        }
        right.remove_cell(0);
        if (!replace_key(parent, idx, next_key)) {
            return Status::Internal("could not update the separator key");
        }
    } else {
        std::string k = std::string(right.key_at(0));
        page_id_t moved = right.child_at(0);
        std::string sep = std::string(parent.key_at(idx));
        if (right.used_space() - (6 + k.size()) < MIN_FILL) {
            return Status::Ok();
        }
        if (!can_replace_key(parent, idx, k)) {
            return Status::Ok();
        }
        if (!child.insert_internal_cell(child.count(), sep, child.extra())) {
            return Status::Ok();
        }
        child.set_extra(moved);
        right.remove_cell(0);
        if (!replace_key(parent, idx, k)) {
            return Status::Internal("could not update the separator key");
        }
    }

    *done = true;
    return Status::Ok();
}

// Moves everything from the right node into the left one and drops the
// separator key from the parent.
Status BTree::merge_children(Node& parent, int j) {
    page_id_t left_id = child_ptr(parent, j);
    page_id_t right_id = child_ptr(parent, j + 1);
    std::string sep = std::string(parent.key_at(j));

    Result<PageGuard> rl = pool_->fetch(left_id);
    if (!rl.ok()) {
        return rl.status();
    }
    PageGuard lg = rl.take();
    Result<PageGuard> rr = pool_->fetch(right_id);
    if (!rr.ok()) {
        return rr.status();
    }
    PageGuard rg = rr.take();

    Node left(lg.write());
    Node right((uint8_t*)rg.read());

    size_t extra_bytes = left.is_leaf() ? 0 : 6 + sep.size();
    if (left.used_space() + right.used_space() + extra_bytes > NODE_CAPACITY) {
        // Does not fit, leave the node half empty instead.
        return Status::Ok();
    }

    if (left.is_leaf()) {
        int n = right.count();
        for (int i = 0; i < n; i++) {
            std::string k = std::string(right.key_at(i));
            std::string v = std::string(right.value_at(i));
            if (!left.insert_leaf_cell(left.count(), k, v)) {
                return Status::Internal("merge did not fit after all");
            }
        }
        left.set_extra(right.extra());
    } else {
        if (!left.insert_internal_cell(left.count(), sep, left.extra())) {
            return Status::Internal("merge did not fit after all");
        }
        int n = right.count();
        for (int i = 0; i < n; i++) {
            std::string k = std::string(right.key_at(i));
            page_id_t c = right.child_at(i);
            if (!left.insert_internal_cell(left.count(), k, c)) {
                return Status::Internal("merge did not fit after all");
            }
        }
        left.set_extra(right.extra());
    }

    parent.remove_cell(j);
    set_child_ptr(parent, j, left_id);

    lg.drop();
    rg.drop();
    return pool_->free_page(right_id);
}

// The root is allowed to be almost empty, but when it has no keys left it is
// replaced by its only child.
Status BTree::shrink_root() {
    MetaPage& meta = disk_->meta();
    page_id_t root = meta.root_page;
    if (root == NO_PAGE) {
        return Status::Ok();
    }

    Result<PageGuard> r = pool_->fetch(root);
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();
    Node node((uint8_t*)guard.read());
    if (node.count() > 0) {
        return Status::Ok();
    }

    page_id_t new_root = NO_PAGE;
    if (!node.is_leaf()) {
        new_root = node.extra();
    }
    guard.drop();

    meta.root_page = new_root;
    return pool_->free_page(root);
}

Status BTree::insert_at(page_id_t pid, std::string_view key, std::string_view value,
                        bool* split, std::string* sep_key, page_id_t* right_page) {
    *split = false;

    Result<PageGuard> r = pool_->fetch(pid);
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();
    // Only ask for a writable page when this node really changes, a page that
    // is only read on the way down does not have to go into the log.
    Node node((uint8_t*)guard.read());

    bool exact = false;
    int idx = node.lower_bound(key, &exact);

    if (node.is_leaf()) {
        Node leaf(guard.write());
        if (exact) {
            leaf.remove_cell(idx);
        }
        if (!leaf.insert_leaf_cell(idx, key, value)) {
            *split = true;
            return split_leaf(leaf, idx, key, value, sep_key, right_page);
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

    // The child was split, so this node gets one more separator key.
    Result<PageGuard> r2 = pool_->fetch(pid);
    if (!r2.ok()) {
        return r2.status();
    }
    PageGuard parent_guard = r2.take();
    Node parent(parent_guard.write());

    bool exact2 = false;
    idx = parent.lower_bound(key, &exact2);
    if (exact2) {
        idx++;
    }

    bool was_extra = (idx == parent.count());
    if (parent.insert_internal_cell(idx, child_sep, child)) {
        if (was_extra) {
            parent.set_extra(child_right);
        } else {
            parent.set_child_at(idx + 1, child_right);
        }
        return Status::Ok();
    }

    *split = true;
    return split_internal(parent, idx, child_sep, child, child_right, sep_key,
                          right_page);
}

Status BTree::check() {
    page_id_t root = disk_->meta().root_page;
    if (root == NO_PAGE) {
        return Status::Ok();
    }
    int leaf_level = -1;
    Status s = check_node(root, 0, 0, 0, &leaf_level);
    if (!s.ok()) {
        return s;
    }
    return check_leaf_chain();
}

Status BTree::check_node(page_id_t pid, int level, const std::string* lo,
                         const std::string* hi, int* leaf_level) {
    Result<PageGuard> r = pool_->fetch(pid);
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();
    Node node((uint8_t*)guard.read());

    if (node.type() != PAGE_TYPE_LEAF && node.type() != PAGE_TYPE_INTERNAL) {
        return Status::Corruption("page " + std::to_string(pid) + " is not a node");
    }

    int n = node.count();
    for (int i = 0; i < n; i++) {
        std::string_view k = node.key_at(i);
        if (i > 0 && compare_keys(node.key_at(i - 1), k) >= 0) {
            return Status::Corruption("keys are not sorted in page " +
                                      std::to_string(pid));
        }
        if (lo != 0 && compare_keys(k, *lo) < 0) {
            return Status::Corruption("key is smaller than the separator above it");
        }
        if (hi != 0 && compare_keys(k, *hi) >= 0) {
            return Status::Corruption("key is bigger than the separator above it");
        }
    }

    if (node.is_leaf()) {
        if (*leaf_level == -1) {
            *leaf_level = level;
        } else if (*leaf_level != level) {
            return Status::Corruption("leaves are not all at the same depth");
        }
        return Status::Ok();
    }

    if (n == 0) {
        return Status::Corruption("internal page " + std::to_string(pid) +
                                  " has no keys");
    }

    std::vector<std::string> keys;
    std::vector<page_id_t> children;
    for (int i = 0; i < n; i++) {
        keys.push_back(std::string(node.key_at(i)));
        children.push_back(node.child_at(i));
    }
    page_id_t last = node.extra();
    guard.drop();

    for (int i = 0; i < n; i++) {
        const std::string* child_lo = (i == 0) ? lo : &keys[(size_t)i - 1];
        Status s = check_node(children[(size_t)i], level + 1, child_lo, &keys[(size_t)i],
                              leaf_level);
        if (!s.ok()) {
            return s;
        }
    }
    if (last == NO_PAGE) {
        return Status::Corruption("internal page has no right child");
    }
    return check_node(last, level + 1, &keys[(size_t)n - 1], hi, leaf_level);
}

// Walks the leaves from left to right and checks that the keys only go up.
Status BTree::check_leaf_chain() {
    PageGuard leaf;
    Status s = find_leaf("", &leaf);
    if (!s.ok()) {
        return s;
    }

    std::string prev;
    bool has_prev = false;
    while (true) {
        Node node((uint8_t*)leaf.read());
        int n = node.count();
        for (int i = 0; i < n; i++) {
            std::string k = std::string(node.key_at(i));
            if (has_prev && compare_keys(prev, k) >= 0) {
                return Status::Corruption("leaf chain is out of order at " + k);
            }
            prev = k;
            has_prev = true;
        }
        page_id_t next = node.extra();
        if (next == NO_PAGE) {
            return Status::Ok();
        }
        leaf.drop();
        Result<PageGuard> r = pool_->fetch(next);
        if (!r.ok()) {
            return r.status();
        }
        leaf = r.take();
    }
}

Status BTree::split_leaf(Node& node, int idx, std::string_view key,
                         std::string_view value, std::string* sep_key,
                         page_id_t* right_page) {
    int n = node.count();
    std::vector<KVPair> all;
    all.reserve((size_t)n + 1);
    for (int i = 0; i < n; i++) {
        if (i == idx) {
            KVPair nw;
            nw.key = std::string(key);
            nw.value = std::string(value);
            all.push_back(nw);
        }
        KVPair kv;
        std::string_view k = node.key_at(i);
        std::string_view v = node.value_at(i);
        kv.key = std::string(k);
        kv.value = std::string(v);
        all.push_back(kv);
    }
    if (idx >= n) {
        KVPair nw;
        nw.key = std::string(key);
        nw.value = std::string(value);
        all.push_back(nw);
    }

    size_t total = 0;
    for (size_t i = 0; i < all.size(); i++) {
        total += 6 + all[i].key.size() + all[i].value.size();
    }

    size_t acc = 0;
    size_t mid = 0;
    for (size_t i = 0; i < all.size(); i++) {
        acc += 6 + all[i].key.size() + all[i].value.size();
        if (acc * 2 >= total) {
            mid = i + 1;
            break;
        }
    }
    if (mid < 1) {
        mid = 1;
    }
    if (mid >= all.size()) {
        mid = all.size() - 1;
    }

    Result<PageGuard> r = pool_->new_page();
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();
    Node right(guard.write());
    right.init(PAGE_TYPE_LEAF);
    right.set_extra(node.extra());
    for (size_t i = mid; i < all.size(); i++) {
        if (!right.insert_leaf_cell((int)(i - mid), all[i].key, all[i].value)) {
            return Status::Internal("right half does not fit after a leaf split");
        }
    }

    node.init(PAGE_TYPE_LEAF);
    node.set_extra(guard.page_id());
    for (size_t i = 0; i < mid; i++) {
        if (!node.insert_leaf_cell((int)i, all[i].key, all[i].value)) {
            return Status::Internal("left half does not fit after a leaf split");
        }
    }

    *sep_key = all[mid].key;
    *right_page = guard.page_id();
    return Status::Ok();
}

struct InternalEntry {
    std::string key;
    page_id_t child;
};

Status BTree::split_internal(Node& node, int idx, std::string_view key,
                             page_id_t left_child, page_id_t right_child,
                             std::string* sep_key, page_id_t* right_page) {
    int n = node.count();
    std::vector<InternalEntry> all;
    all.reserve((size_t)n + 1);
    for (int i = 0; i < n; i++) {
        InternalEntry e;
        e.key = std::string(node.key_at(i));
        e.child = node.child_at(i);
        all.push_back(e);
    }
    page_id_t last_child = node.extra();

    InternalEntry added;
    added.key = std::string(key);
    added.child = left_child;
    all.insert(all.begin() + idx, added);
    if ((size_t)idx + 1 < all.size()) {
        all[idx + 1].child = right_child;
    } else {
        last_child = right_child;
    }

    size_t mid = all.size() / 2;
    std::string up_key = all[mid].key;
    page_id_t left_last = all[mid].child;

    Result<PageGuard> r = pool_->new_page();
    if (!r.ok()) {
        return r.status();
    }
    PageGuard guard = r.take();
    Node right(guard.write());
    right.init(PAGE_TYPE_INTERNAL);
    right.set_extra(last_child);
    for (size_t i = mid + 1; i < all.size(); i++) {
        if (!right.insert_internal_cell((int)(i - mid - 1), all[i].key, all[i].child)) {
            return Status::Internal("right half does not fit after an internal split");
        }
    }

    node.init(PAGE_TYPE_INTERNAL);
    node.set_extra(left_last);
    for (size_t i = 0; i < mid; i++) {
        if (!node.insert_internal_cell((int)i, all[i].key, all[i].child)) {
            return Status::Internal("left half does not fit after an internal split");
        }
    }

    *sep_key = up_key;
    *right_page = guard.page_id();
    return Status::Ok();
}

}  // namespace pagedb
