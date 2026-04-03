#include "pagedb/btree.hpp"

#include "test_util.hpp"

using namespace pagedb;

static void put(BTree& t, const std::string& k, const std::string& v) {
    CHECK_OK(t.insert(k, v));
}

static void expect(BTree& t, const std::string& k, const std::string& v) {
    std::string got;
    bool found = false;
    CHECK_OK(t.get(k, &got, &found));
    CHECK(found);
    CHECK(got == v);
}

static void expect_missing(BTree& t, const std::string& k) {
    std::string got;
    bool found = true;
    CHECK_OK(t.get(k, &got, &found));
    CHECK(!found);
}

static void test_small_tree(const std::string& path) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 16);
    BTree tree(&dm, &pool);

    put(tree, "b", "two");
    put(tree, "a", "one");
    put(tree, "c", "three");

    expect(tree, "a", "one");
    expect(tree, "b", "two");
    expect(tree, "c", "three");
    expect_missing(tree, "d");
    expect_missing(tree, "");

    // insert of an existing key replaces the value
    put(tree, "b", "TWO");
    expect(tree, "b", "TWO");

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());

    DiskManager dm2;
    CHECK_OK(dm2.open(path));
    BufferPool pool2(&dm2, 16);
    BTree tree2(&dm2, &pool2);
    expect(tree2, "a", "one");
    expect(tree2, "b", "TWO");
    CHECK_OK(dm2.close());
}

static void test_bad_sizes(const std::string& path) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 16);
    BTree tree(&dm, &pool);

    std::string big_key(65, 'x');
    std::string big_value(257, 'y');
    CHECK_CODE(tree.insert(big_key, "v"), Code::InvalidArgument);
    CHECK_CODE(tree.insert("k", big_value), Code::InvalidArgument);
    CHECK_CODE(tree.insert("", "v"), Code::InvalidArgument);
    CHECK_OK(tree.insert("k", ""));
    expect(tree, "k", "");

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

int main() {
    std::string path = temp_path("btree");
    test_small_tree(path);
    remove_db(path);
    test_bad_sizes(path);
    remove_db(path);
    printf("btree_test ok\n");
    return 0;
}
