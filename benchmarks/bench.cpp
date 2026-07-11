#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "pagedb/database.hpp"

using namespace pagedb;

typedef std::chrono::steady_clock Clock;

struct Options {
    int ops = 200000;
    size_t pool = 1024;
    bool durable = true;
    int threads = 1;
    std::string only = "";
    unsigned seed = 42;
    int key_size = 16;
    int value_size = 100;
};

struct Rng {
    unsigned state;
    explicit Rng(unsigned seed) : state(seed) {}
    unsigned next() {
        state = state * 1103515245u + 12345u;
        return (state >> 8) & 0xffffffu;
    }
};

static std::string make_key(int i, int size) {
    char buf[80];
    snprintf(buf, sizeof(buf), "key%013d", i);
    std::string s(buf);
    if ((int)s.size() > size) {
        s.resize((size_t)size);
    }
    while ((int)s.size() < size) {
        s += "0";
    }
    return s;
}

// Prints one result line. Latencies are microseconds.
static void report(const char* name, long ops, double seconds,
                   std::vector<double>& lat) {
    if (lat.empty()) {
        printf("%-22s %9ld ops %8.2f s %12.0f ops/s\n", name, ops, seconds,
               (double)ops / seconds);
        return;
    }
    std::sort(lat.begin(), lat.end());
    double p50 = lat[lat.size() / 2];
    double p99 = lat[(size_t)((double)lat.size() * 0.99)];
    printf("%-22s %9ld ops %8.2f s %12.0f ops/s  p50 %7.2f us  p99 %8.2f us\n",
           name, ops, seconds, (double)ops / seconds, p50, p99);
}

static std::unique_ptr<Database> open_db(const std::string& path, const Options& o) {
    DatabaseOptions options;
    options.path = path;
    options.buffer_pool_pages = o.pool;
    options.durable = o.durable;
    Result<std::unique_ptr<Database>> r = Database::open(options);
    if (!r.ok()) {
        printf("open failed: %s\n", r.status().to_string().c_str());
        exit(1);
    }
    return r.take();
}

static void fresh(const std::string& path) {
    ::unlink(path.c_str());
    ::unlink((path + ".wal").c_str());
}

static void bench_insert(const std::string& path, const Options& o, bool random) {
    fresh(path);
    std::unique_ptr<Database> db = open_db(path, o);
    std::string value((size_t)o.value_size, 'v');
    std::vector<double> lat;
    lat.reserve((size_t)o.ops);
    Rng rng(o.seed);

    Clock::time_point start = Clock::now();
    for (int i = 0; i < o.ops; i++) {
        int n = random ? (int)(rng.next() % (unsigned)o.ops) : i;
        Clock::time_point t0 = Clock::now();
        Status s = db->put(make_key(n, o.key_size), value);
        Clock::time_point t1 = Clock::now();
        if (!s.ok()) {
            printf("put failed: %s\n", s.to_string().c_str());
            exit(1);
        }
        lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    report(random ? "random insert" : "sequential insert", o.ops, seconds, lat);
    db->close();
}

static void fill(Database* db, const Options& o) {
    std::string value((size_t)o.value_size, 'v');
    for (int i = 0; i < o.ops; i++) {
        Status s = db->put(make_key(i, o.key_size), value);
        if (!s.ok()) {
            printf("fill failed: %s\n", s.to_string().c_str());
            exit(1);
        }
    }
}

static void bench_get(const std::string& path, const Options& o) {
    fresh(path);
    std::unique_ptr<Database> db = open_db(path, o);
    fill(db.get(), o);

    std::vector<double> lat;
    lat.reserve((size_t)o.ops);
    Rng rng(o.seed + 1);
    Clock::time_point start = Clock::now();
    for (int i = 0; i < o.ops; i++) {
        int n = (int)(rng.next() % (unsigned)o.ops);
        Clock::time_point t0 = Clock::now();
        Result<std::optional<std::string>> r = db->get(make_key(n, o.key_size));
        Clock::time_point t1 = Clock::now();
        if (!r.ok()) {
            printf("get failed: %s\n", r.status().to_string().c_str());
            exit(1);
        }
        lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    report("random get", o.ops, seconds, lat);
    printf("    buffer pool: %llu hits, %llu misses, %llu evictions\n",
           (unsigned long long)db->pool().hits(),
           (unsigned long long)db->pool().misses(),
           (unsigned long long)db->pool().evictions());
    db->close();
}

static void bench_scan(const std::string& path, const Options& o) {
    fresh(path);
    std::unique_ptr<Database> db = open_db(path, o);
    fill(db.get(), o);

    const int RANGE = 100;
    int rounds = o.ops / 100;
    std::vector<double> lat;
    lat.reserve((size_t)rounds);
    Rng rng(o.seed + 2);
    long rows = 0;

    Clock::time_point start = Clock::now();
    for (int i = 0; i < rounds; i++) {
        int n = (int)(rng.next() % (unsigned)(o.ops - RANGE));
        Clock::time_point t0 = Clock::now();
        Result<std::vector<KVPair>> r =
            db->scan(make_key(n, o.key_size), make_key(n + RANGE, o.key_size));
        Clock::time_point t1 = Clock::now();
        if (!r.ok()) {
            printf("scan failed: %s\n", r.status().to_string().c_str());
            exit(1);
        }
        rows += (long)r.value().size();
        lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    report("range scan x100", rounds, seconds, lat);
    printf("    %ld rows read\n", rows);
    db->close();
}

static void bench_mixed(const std::string& path, const Options& o) {
    fresh(path);
    std::unique_ptr<Database> db = open_db(path, o);
    fill(db.get(), o);

    std::string value((size_t)o.value_size, 'w');
    std::vector<double> lat;
    lat.reserve((size_t)o.ops);
    Rng rng(o.seed + 3);
    Clock::time_point start = Clock::now();
    for (int i = 0; i < o.ops; i++) {
        unsigned pick = rng.next() % 100;
        int n = (int)(rng.next() % (unsigned)o.ops);
        Clock::time_point t0 = Clock::now();
        if (pick < 80) {
            Result<std::optional<std::string>> r = db->get(make_key(n, o.key_size));
            if (!r.ok()) {
                exit(1);
            }
        } else {
            Status s = db->put(make_key(n, o.key_size), value);
            if (!s.ok()) {
                exit(1);
            }
        }
        Clock::time_point t1 = Clock::now();
        lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    report("80 read 20 write", o.ops, seconds, lat);
    db->close();
}

static void reader_work(Database* db, const Options* o, int id, long* done) {
    Rng rng((unsigned)(o->seed + 100 + id));
    long count = 0;
    for (int i = 0; i < o->ops / o->threads; i++) {
        int n = (int)(rng.next() % (unsigned)o->ops);
        Result<std::optional<std::string>> r = db->get(make_key(n, o->key_size));
        if (!r.ok()) {
            exit(1);
        }
        count++;
    }
    *done = count;
}

static void bench_threads(const std::string& path, const Options& o) {
    fresh(path);
    std::unique_ptr<Database> db = open_db(path, o);
    fill(db.get(), o);

    for (int t = 1; t <= o.threads; t *= 2) {
        Options local = o;
        local.threads = t;
        std::vector<std::thread> threads;
        std::vector<long> done((size_t)t, 0);
        Clock::time_point start = Clock::now();
        for (int i = 0; i < t; i++) {
            threads.push_back(std::thread(reader_work, db.get(), &local, i, &done[(size_t)i]));
        }
        long total = 0;
        for (int i = 0; i < t; i++) {
            threads[(size_t)i].join();
            total += done[(size_t)i];
        }
        double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        char name[64];
        snprintf(name, sizeof(name), "get with %d threads", t);
        std::vector<double> empty;
        report(name, total, seconds, empty);
    }
    db->close();
}

static void bench_cache_sweep(const std::string& path, const Options& o) {
    size_t sizes[5] = {32, 128, 512, 2048, 8192};
    for (int i = 0; i < 5; i++) {
        Options local = o;
        local.pool = sizes[i];
        fresh(path);
        std::unique_ptr<Database> db = open_db(path, local);
        fill(db.get(), local);

        Rng rng(local.seed + 4);
        Clock::time_point start = Clock::now();
        for (int k = 0; k < local.ops; k++) {
            int n = (int)(rng.next() % (unsigned)local.ops);
            Result<std::optional<std::string>> r = db->get(make_key(n, local.key_size));
            if (!r.ok()) {
                exit(1);
            }
        }
        double seconds = std::chrono::duration<double>(Clock::now() - start).count();
        double hit_rate = 100.0 * (double)db->pool().hits() /
                          (double)(db->pool().hits() + db->pool().misses());
        char name[64];
        snprintf(name, sizeof(name), "get, pool %zu pages", sizes[i]);
        std::vector<double> empty;
        report(name, local.ops, seconds, empty);
        printf("    hit rate %.1f%%\n", hit_rate);
        db->close();
    }
}

// Writes without closing the database, then measures how long the next open
// takes, which is the log replay.
static void bench_recovery(const std::string& path, const Options& o) {
    fresh(path);
    int n = o.ops / 10;

    // The writer has to die without closing the file, otherwise the lock is
    // still held and the log is already empty.
    pid_t pid = fork();
    if (pid == 0) {
        std::unique_ptr<Database> db = open_db(path, o);
        std::string value((size_t)o.value_size, 'v');
        for (int i = 0; i < n; i++) {
            if (!db->put(make_key(i, o.key_size), value).ok()) {
                _exit(1);
            }
        }
        db.release();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);

    Clock::time_point start = Clock::now();
    std::unique_ptr<Database> db = open_db(path, o);
    double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    printf("%-22s %9d ops %8.2f s replay\n", "recovery", n, seconds);
    db->close();
}

int main(int argc, char** argv) {
    Options o;
    std::string path = "/tmp/pagedb_bench.db";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
            o.ops = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--pool") == 0 && i + 1 < argc) {
            o.pool = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--durable") == 0 && i + 1 < argc) {
            o.durable = atoi(argv[++i]) != 0;
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            o.threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--value-size") == 0 && i + 1 < argc) {
            o.value_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else if (strcmp(argv[i], "--only") == 0 && i + 1 < argc) {
            o.only = argv[++i];
        } else {
            printf("usage: bench [--ops n] [--pool n] [--durable 0|1] "
                   "[--threads n] [--value-size n] [--path file] [--only name]\n");
            return 1;
        }
    }

    printf("ops %d, pool %zu pages, durable %d, key %d bytes, value %d bytes\n",
           o.ops, o.pool, o.durable ? 1 : 0, o.key_size, o.value_size);

    if (o.only.empty() || o.only == "seq") {
        bench_insert(path, o, false);
    }
    if (o.only.empty() || o.only == "rand") {
        bench_insert(path, o, true);
    }
    if (o.only.empty() || o.only == "get") {
        bench_get(path, o);
    }
    if (o.only.empty() || o.only == "scan") {
        bench_scan(path, o);
    }
    if (o.only.empty() || o.only == "mixed") {
        bench_mixed(path, o);
    }
    if (o.only == "cache") {
        bench_cache_sweep(path, o);
    }
    if (o.only == "threads") {
        bench_threads(path, o);
    }
    if (o.only == "recovery") {
        bench_recovery(path, o);
    }
    fresh(path);
    return 0;
}
