#include "pagedb/buffer_pool.hpp"

#include <string.h>

#include "test_util.hpp"

using namespace pagedb;

static void mark(PageGuard& g, uint8_t v) {
    uint8_t* p = g.write();
    memset(p, v, PAGE_SIZE);
}

static bool check_mark(PageGuard& g, uint8_t v) {
    const uint8_t* p = g.read();
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        if (p[i] != v) {
            return false;
        }
    }
    return true;
}

static void test_hit_and_miss(const std::string& path) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 4);

    Result<PageGuard> r = pool.new_page();
    CHECK(r.ok());
    PageGuard g = r.take();
    page_id_t id = g.page_id();
    mark(g, 0x5a);
    g.drop();

    Result<PageGuard> r2 = pool.fetch(id);
    CHECK(r2.ok());
    PageGuard g2 = r2.take();
    CHECK(check_mark(g2, 0x5a));
    g2.drop();
    CHECK(pool.hits() >= 1);

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

static void test_eviction_keeps_data(const std::string& path) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 2);

    page_id_t ids[6];
    for (int i = 0; i < 6; i++) {
        Result<PageGuard> r = pool.new_page();
        CHECK(r.ok());
        PageGuard g = r.take();
        ids[i] = g.page_id();
        mark(g, (uint8_t)(100 + i));
    }
    CHECK(pool.evictions() > 0);

    for (int i = 0; i < 6; i++) {
        Result<PageGuard> r = pool.fetch(ids[i]);
        CHECK(r.ok());
        PageGuard g = r.take();
        CHECK(check_mark(g, (uint8_t)(100 + i)));
    }

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

static void test_all_pinned(const std::string& path) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 2);

    Result<PageGuard> r1 = pool.new_page();
    CHECK(r1.ok());
    PageGuard a = r1.take();
    Result<PageGuard> r2 = pool.new_page();
    CHECK(r2.ok());
    PageGuard b = r2.take();

    Result<PageGuard> r3 = pool.new_page();
    CHECK(!r3.ok());
    CHECK(r3.status().code() == Code::PoolExhausted);

    // the failed new_page must not eat a page id
    CHECK(dm.page_count() == 3);

    a.drop();
    Result<PageGuard> r4 = pool.new_page();
    CHECK(r4.ok());

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

static void test_one_frame(const std::string& path) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 1);
    CHECK(pool.capacity() == 1);

    Result<PageGuard> r1 = pool.new_page();
    CHECK(r1.ok());
    PageGuard g1 = r1.take();
    page_id_t a = g1.page_id();
    mark(g1, 1);
    g1.drop();

    Result<PageGuard> r2 = pool.new_page();
    CHECK(r2.ok());
    PageGuard g2 = r2.take();
    page_id_t b = g2.page_id();
    mark(g2, 2);
    g2.drop();

    for (int i = 0; i < 3; i++) {
        Result<PageGuard> ra = pool.fetch(a);
        CHECK(ra.ok());
        PageGuard ga = ra.take();
        CHECK(check_mark(ga, 1));
        ga.drop();

        Result<PageGuard> rb = pool.fetch(b);
        CHECK(rb.ok());
        PageGuard gb = rb.take();
        CHECK(check_mark(gb, 2));
        gb.drop();
    }

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

static void test_free_list_reuse(const std::string& path) {
    DiskManager dm;
    CHECK_OK(dm.open(path));
    BufferPool pool(&dm, 4);

    Result<PageGuard> r = pool.new_page();
    CHECK(r.ok());
    page_id_t id = r.value().page_id();
    r.value().drop();

    uint32_t before = dm.page_count();
    CHECK_OK(pool.free_page(id));
    CHECK(dm.meta().free_list_head == id);

    Result<PageGuard> r2 = pool.new_page();
    CHECK(r2.ok());
    CHECK(r2.value().page_id() == id);
    CHECK(dm.page_count() == before);
    r2.value().drop();

    CHECK_OK(pool.flush_all());
    CHECK_OK(dm.close());
}

int main() {
    std::string path = temp_path("pool");
    test_hit_and_miss(path);
    remove_db(path);
    test_eviction_keeps_data(path);
    remove_db(path);
    test_all_pinned(path);
    remove_db(path);
    test_one_frame(path);
    remove_db(path);
    test_free_list_reuse(path);
    remove_db(path);
    printf("buffer_pool_test ok\n");
    return 0;
}
