#include "pagedb/database.hpp"

#include "test_util.hpp"

using namespace pagedb;

static std::unique_ptr<Database> open_db(const std::string& path) {
    DatabaseOptions options;
    options.path = path;
    options.buffer_pool_pages = 64;
    Result<std::unique_ptr<Database>> r = Database::open(options);
    CHECK(r.ok());
    return r.take();
}

static void test_api(const std::string& path) {
    std::unique_ptr<Database> db = open_db(path);

    CHECK_OK(db->put("apple", "red"));
    CHECK_OK(db->put("banana", "yellow"));
    CHECK_OK(db->put("cherry", "dark red"));

    Result<std::optional<std::string>> g = db->get("banana");
    CHECK(g.ok());
    CHECK(g.value().has_value());
    CHECK(g.value().value() == "yellow");

    // a missing key is not an error
    Result<std::optional<std::string>> g2 = db->get("durian");
    CHECK(g2.ok());
    CHECK(!g2.value().has_value());

    // put replaces
    CHECK_OK(db->put("banana", "green"));
    Result<std::optional<std::string>> g3 = db->get("banana");
    CHECK(g3.ok());
    CHECK(g3.value().value() == "green");

    // remove of a missing key is NotFound
    CHECK_CODE(db->remove("durian"), Code::NotFound);
    CHECK_OK(db->remove("banana"));
    Result<std::optional<std::string>> g4 = db->get("banana");
    CHECK(g4.ok());
    CHECK(!g4.value().has_value());

    // bad sizes
    CHECK_CODE(db->put("", "x"), Code::InvalidArgument);
    CHECK_CODE(db->put(std::string(65, 'k'), "x"), Code::InvalidArgument);
    CHECK_CODE(db->put("k", std::string(257, 'v')), Code::InvalidArgument);

    Result<std::vector<KVPair>> s = db->scan("a", "z");
    CHECK(s.ok());
    CHECK(s.value().size() == 2);
    CHECK(s.value()[0].key == "apple");
    CHECK(s.value()[1].key == "cherry");

    // start not smaller than end gives nothing
    Result<std::vector<KVPair>> s2 = db->scan("z", "a");
    CHECK(s2.ok());
    CHECK(s2.value().size() == 0);

    CHECK_OK(db->close());
}

static void test_reopen(const std::string& path) {
    {
        std::unique_ptr<Database> db = open_db(path);
        for (int i = 0; i < 500; i++) {
            CHECK_OK(db->put("k" + std::to_string(i), "v" + std::to_string(i)));
        }
        CHECK_OK(db->close());
    }

    std::unique_ptr<Database> db = open_db(path);
    for (int i = 0; i < 500; i++) {
        Result<std::optional<std::string>> g = db->get("k" + std::to_string(i));
        CHECK(g.ok());
        CHECK(g.value().has_value());
        CHECK(g.value().value() == "v" + std::to_string(i));
    }
    CHECK_OK(db->close());
}

static void test_second_opener_is_rejected(const std::string& path) {
    std::unique_ptr<Database> db = open_db(path);

    DatabaseOptions options;
    options.path = path;
    Result<std::unique_ptr<Database>> r = Database::open(options);
    CHECK(!r.ok());
    CHECK(r.status().code() == Code::Locked);

    CHECK_OK(db->close());
}

// With a small checkpoint limit the log has to be emptied while the writes
// are still running, and the data must still be there afterwards.
static void test_auto_checkpoint(const std::string& path) {
    DatabaseOptions options;
    options.path = path;
    options.buffer_pool_pages = 64;
    options.durable = false;
    options.checkpoint_bytes = 256 * 1024;
    Result<std::unique_ptr<Database>> r = Database::open(options);
    CHECK(r.ok());
    std::unique_ptr<Database> db = r.take();

    for (int i = 0; i < 3000; i++) {
        CHECK_OK(db->put("key" + std::to_string(i), "value" + std::to_string(i)));
    }
    CHECK(db->wal().commits() == 3000);

    struct stat st;
    CHECK(stat((path + ".wal").c_str(), &st) == 0);
    CHECK(st.st_size < 4 * 1024 * 1024);

    for (int i = 0; i < 3000; i++) {
        Result<std::optional<std::string>> g = db->get("key" + std::to_string(i));
        CHECK(g.ok());
        CHECK(g.value().has_value());
        CHECK(g.value().value() == "value" + std::to_string(i));
    }
    CHECK_OK(db->verify());
    CHECK_OK(db->close());
}

int main() {
    std::string path = temp_path("database");
    test_api(path);
    remove_db(path);
    test_reopen(path);
    remove_db(path);
    test_second_opener_is_rejected(path);
    remove_db(path);
    test_auto_checkpoint(path);
    remove_db(path);
    printf("database_test ok\n");
    return 0;
}
