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

static std::string make_key(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%08d", i);
    return std::string(buf);
}

static void test_many_keys(const std::string& path) {
    const int N = 20000;
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 64);
    BTree tree(&dm, &pool);

    for (int i = 0; i < N; i++) {
        put(tree, make_key(i), "value_" + std::to_string(i));
    }
    for (int i = 0; i < N; i++) {
        expect(tree, make_key(i), "value_" + std::to_string(i));
    }
    expect_missing(tree, make_key(N + 1));
    CHECK_OK(tree.check());

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());

    DiskManager dm2;
    CHECK_OK(dm2.open(path));
    BufferPool pool2(&dm2, 64);
    BTree tree2(&dm2, &pool2);
    for (int i = 0; i < N; i += 7) {
        expect(tree2, make_key(i), "value_" + std::to_string(i));
    }
    CHECK_OK(dm2.close());
}

static void test_random_order(const std::string& path) {
    const int N = 5000;
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 32);
    BTree tree(&dm, &pool);

    unsigned seed = 12345;
    std::vector<int> order;
    for (int i = 0; i < N; i++) {
        order.push_back(i);
    }
    for (int i = N - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        int j = (int)((seed >> 16) % (unsigned)(i + 1));
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }

    for (int i = 0; i < N; i++) {
        put(tree, make_key(order[i]), "v" + std::to_string(order[i]));
    }
    for (int i = 0; i < N; i++) {
        expect(tree, make_key(i), "v" + std::to_string(i));
    }
    CHECK_OK(tree.check());

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

static void test_scan(const std::string& path) {
    const int N = 3000;
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 32);
    BTree tree(&dm, &pool);

    for (int i = 0; i < N; i++) {
        put(tree, make_key(i), "v" + std::to_string(i));
    }

    std::vector<KVPair> out;
    CHECK_OK(tree.scan(make_key(10), make_key(20), &out));
    CHECK(out.size() == 10);
    for (int i = 0; i < 10; i++) {
        CHECK(out[(size_t)i].key == make_key(10 + i));
        CHECK(out[(size_t)i].value == "v" + std::to_string(10 + i));
    }

    // a range that crosses many leaves
    CHECK_OK(tree.scan(make_key(0), make_key(N), &out));
    CHECK(out.size() == (size_t)N);
    for (int i = 0; i < N; i++) {
        CHECK(out[(size_t)i].key == make_key(i));
    }

    // start after end gives nothing
    CHECK_OK(tree.scan(make_key(20), make_key(10), &out));
    CHECK(out.size() == 0);

    // start and end outside the data
    CHECK_OK(tree.scan("a", "z", &out));
    CHECK(out.size() == (size_t)N);
    CHECK_OK(tree.scan("zzz", "zzzz", &out));
    CHECK(out.size() == 0);

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

int main() {
    std::string path = temp_path("btree");
    test_small_tree(path);
    remove_db(path);
    test_bad_sizes(path);
    remove_db(path);
    test_many_keys(path);
    remove_db(path);
    test_random_order(path);
    remove_db(path);
    test_scan(path);
    remove_db(path);
    printf("btree_test ok\n");
    return 0;
}
