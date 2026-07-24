#include "pagedb/btree.hpp"

#include <map>
#include <string>

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

static void test_delete(const std::string& path) {
    const int N = 2000;
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 32);
    BTree tree(&dm, &pool);

    for (int i = 0; i < N; i++) {
        put(tree, make_key(i), "v" + std::to_string(i));
    }
    for (int i = 0; i < N; i += 2) {
        CHECK_OK(tree.remove(make_key(i)));
    }
    CHECK_CODE(tree.remove(make_key(0)), Code::NotFound);
    CHECK_CODE(tree.remove("nothing"), Code::NotFound);

    for (int i = 0; i < N; i++) {
        if (i % 2 == 0) {
            expect_missing(tree, make_key(i));
        } else {
            expect(tree, make_key(i), "v" + std::to_string(i));
        }
    }
    CHECK_OK(tree.check());

    std::vector<KVPair> out;
    CHECK_OK(tree.scan("", "zzz", &out));
    CHECK(out.size() == (size_t)N / 2);

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

// Simple repeatable random numbers so a failing run can be replayed.
struct Rng {
    unsigned state;
    explicit Rng(unsigned seed) : state(seed) {}
    unsigned next() {
        state = state * 1103515245u + 12345u;
        return (state >> 8) & 0x7fffffu;
    }
};

// Does the same operations on the tree and on a std::map and compares them.
static void test_against_map(const std::string& path, unsigned seed) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 32);
    BTree tree(&dm, &pool);

    std::map<std::string, std::string> model;
    Rng rng(seed);

    for (int step = 0; step < 20000; step++) {
        unsigned op = rng.next() % 100;
        char kbuf[16];
        snprintf(kbuf, sizeof(kbuf), "k%05u", rng.next() % 1500);
        std::string key = kbuf;

        if (op < 45) {
            std::string value(rng.next() % 60, 'v');
            CHECK_OK(tree.insert(key, value));
            model[key] = value;
        } else if (op < 80) {
            std::string got;
            bool found = false;
            CHECK_OK(tree.get(key, &got, &found));
            std::map<std::string, std::string>::iterator it = model.find(key);
            if (it == model.end()) {
                CHECK(!found);
            } else {
                CHECK(found);
                CHECK(got == it->second);
            }
        } else if (op < 97) {
            Status s = tree.remove(key);
            if (model.erase(key) == 1) {
                CHECK_OK(s);
            } else {
                CHECK(s.code() == Code::NotFound);
            }
        } else {
            char sbuf[16];
            char ebuf[16];
            snprintf(sbuf, sizeof(sbuf), "k%05u", rng.next() % 1500);
            snprintf(ebuf, sizeof(ebuf), "k%05u", rng.next() % 1500);
            std::vector<KVPair> out;
            CHECK_OK(tree.scan(sbuf, ebuf, &out));

            std::vector<KVPair> want;
            std::map<std::string, std::string>::iterator it =
                model.lower_bound(sbuf);
            while (it != model.end() && it->first < std::string(ebuf)) {
                KVPair kv;
                kv.key = it->first;
                kv.value = it->second;
                want.push_back(kv);
                ++it;
            }
            CHECK(out.size() == want.size());
            for (size_t i = 0; i < out.size(); i++) {
                CHECK(out[i].key == want[i].key);
                CHECK(out[i].value == want[i].value);
            }
        }

        if (step % 2000 == 0) {
            CHECK_OK(tree.check());
        }
    }

    CHECK_OK(tree.check());

    // everything the model has must be in the tree and nothing more
    std::vector<KVPair> all;
    CHECK_OK(tree.scan("", "zzzzzz", &all));
    CHECK(all.size() == model.size());
    size_t i = 0;
    for (std::map<std::string, std::string>::iterator it = model.begin();
         it != model.end(); ++it) {
        CHECK(all[i].key == it->first);
        CHECK(all[i].value == it->second);
        i++;
    }

    // close, reopen and compare again
    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());

    DiskManager dm2;
    CHECK_OK(dm2.open(path));
    BufferPool pool2(&dm2, 32);
    BTree tree2(&dm2, &pool2);
    CHECK_OK(tree2.check());
    std::vector<KVPair> all2;
    CHECK_OK(tree2.scan("", "zzzzzz", &all2));
    CHECK(all2.size() == model.size());
    CHECK_OK(dm2.close());
}

static void test_delete_everything(const std::string& path) {
    const int N = 4000;
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 32);
    BTree tree(&dm, &pool);

    for (int i = 0; i < N; i++) {
        put(tree, make_key(i), std::string(50, 'x'));
    }
    uint32_t pages_used = dm.page_count();

    for (int i = 0; i < N; i++) {
        CHECK_OK(tree.remove(make_key(i)));
    }
    CHECK_OK(tree.check());
    CHECK(tree.root() == NO_PAGE);
    CHECK(dm.meta().free_page_count > 0);

    // the freed pages are used again instead of growing the file
    for (int i = 0; i < N; i++) {
        put(tree, make_key(i), std::string(50, 'x'));
    }
    CHECK(dm.page_count() <= pages_used + 4);
    CHECK_OK(tree.check());

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
    test_delete(path);
    remove_db(path);
    test_delete_everything(path);
    remove_db(path);
    for (unsigned seed = 1; seed <= 3; seed++) {
        test_against_map(path, seed);
        remove_db(path);
    }
    printf("btree_test ok\n");
    return 0;
}
