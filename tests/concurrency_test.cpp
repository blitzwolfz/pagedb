#include <atomic>
#include <thread>
#include <vector>

#include "pagedb/database.hpp"
#include "test_util.hpp"

using namespace pagedb;

// Each writer owns its own range of keys, so the value of a key is always
// known: either it is missing or it is exactly this string.
static std::string key_of(int writer, int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "w%02d-k%06d", writer, i);
    return std::string(buf);
}

static std::string value_of(int writer, int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "w%02d-v%06d", writer, i);
    return std::string(buf);
}

static std::atomic<bool> g_stop(false);
static std::atomic<long> g_reads(0);
static std::atomic<long> g_scans(0);

static void writer_thread(Database* db, int id, int count) {
    for (int i = 0; i < count; i++) {
        Status s = db->put(key_of(id, i), value_of(id, i));
        if (!s.ok()) {
            printf("put failed: %s\n", s.to_string().c_str());
            exit(1);
        }
        if (i % 5 == 4) {
            Status r = db->remove(key_of(id, i - 4));
            if (!r.ok() && r.code() != Code::NotFound) {
                printf("remove failed: %s\n", r.to_string().c_str());
                exit(1);
            }
        }
    }
}

static void reader_thread(Database* db, int writers, int count) {
    int i = 0;
    while (!g_stop.load()) {
        int w = i % writers;
        int n = i % count;
        Result<std::optional<std::string>> g = db->get(key_of(w, n));
        if (!g.ok()) {
            printf("get failed: %s\n", g.status().to_string().c_str());
            exit(1);
        }
        if (g.value().has_value() && g.value().value() != value_of(w, n)) {
            printf("reader saw a value that was never written\n");
            exit(1);
        }
        g_reads.fetch_add(1);

        if (i % 50 == 0) {
            Result<std::vector<KVPair>> s = db->scan("w", "x");
            if (!s.ok()) {
                printf("scan failed: %s\n", s.status().to_string().c_str());
                exit(1);
            }
            const std::vector<KVPair>& rows = s.value();
            for (size_t j = 1; j < rows.size(); j++) {
                if (rows[j - 1].key >= rows[j].key) {
                    printf("scan is not sorted\n");
                    exit(1);
                }
            }
            g_scans.fetch_add(1);
        }
        i++;
    }
}

static void test_readers_and_writers(const std::string& path) {
    const int WRITERS = 3;
    const int READERS = 4;
    const int COUNT = 1500;

    DatabaseOptions options;
    options.path = path;
    options.buffer_pool_pages = 128;
    options.durable = false;
    Result<std::unique_ptr<Database>> r = Database::open(options);
    CHECK(r.ok());
    std::unique_ptr<Database> db = r.take();

    std::vector<std::thread> threads;
    for (int i = 0; i < READERS; i++) {
        threads.push_back(std::thread(reader_thread, db.get(), WRITERS, COUNT));
    }
    for (int i = 0; i < WRITERS; i++) {
        threads.push_back(std::thread(writer_thread, db.get(), i, COUNT));
    }

    for (size_t i = READERS; i < threads.size(); i++) {
        threads[i].join();
    }
    g_stop.store(true);
    for (int i = 0; i < READERS; i++) {
        threads[(size_t)i].join();
    }

    CHECK(g_reads.load() > 0);
    CHECK(g_scans.load() > 0);
    CHECK_OK(db->verify());

    // every key that was not removed has to be there
    for (int w = 0; w < WRITERS; w++) {
        for (int i = 0; i < COUNT; i++) {
            bool removed = (i % 5 == 0) && (i + 4 < COUNT);
            Result<std::optional<std::string>> g = db->get(key_of(w, i));
            CHECK(g.ok());
            if (removed) {
                CHECK(!g.value().has_value());
            } else {
                CHECK(g.value().has_value());
                CHECK(g.value().value() == value_of(w, i));
            }
        }
    }

    CHECK_OK(db->close());
}

int main() {
    std::string path = temp_path("concurrency");
    test_readers_and_writers(path);
    remove_db(path);
    printf("concurrency_test ok\n");
    return 0;
}
