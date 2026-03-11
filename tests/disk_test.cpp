#include "pagedb/disk_manager.hpp"

#include <string.h>

#include "test_util.hpp"

using namespace pagedb;

static void fill(uint8_t* buf, uint8_t seed) {
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
}

static void test_create_and_reopen(const std::string& path) {
    uint8_t page[PAGE_SIZE];
    uint8_t back[PAGE_SIZE];

    DiskManager dm;
    CHECK_OK(dm.open(path));
    CHECK(dm.page_count() == 1);

    Result<page_id_t> r = dm.allocate_page();
    CHECK(r.ok());
    page_id_t id = r.value();
    CHECK(id == 1);

    fill(page, 7);
    CHECK_OK(dm.write_page(id, page));
    CHECK_OK(dm.read_page(id, back));
    CHECK(memcmp(page, back, PAGE_SIZE) == 0);
    CHECK_OK(dm.close());

    // Reopen and compare the bytes again.
    DiskManager dm2;
    CHECK_OK(dm2.open(path));
    CHECK(dm2.page_count() == 2);
    memset(back, 0, PAGE_SIZE);
    CHECK_OK(dm2.read_page(id, back));
    CHECK(memcmp(page, back, PAGE_SIZE) == 0);
    CHECK_OK(dm2.close());
}

static void test_bad_page_id(const std::string& path) {
    uint8_t page[PAGE_SIZE];
    DiskManager dm;
    CHECK_OK(dm.open(path));
    CHECK_CODE(dm.read_page(99, page), Code::InvalidArgument);
    CHECK_CODE(dm.write_page(99, page), Code::InvalidArgument);
    CHECK_OK(dm.close());
}

static void test_bad_magic(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r+");
    CHECK(f != NULL);
    fwrite("XXXX", 1, 4, f);
    fclose(f);

    DiskManager dm;
    CHECK_CODE(dm.open(path), Code::Corruption);
}

static void test_truncated_file(const std::string& path) {
    CHECK(truncate(path.c_str(), PAGE_SIZE + 10) == 0);
    DiskManager dm;
    CHECK_CODE(dm.open(path), Code::Corruption);
}

int main() {
    std::string path = temp_path("disk");
    test_create_and_reopen(path);
    test_bad_page_id(path);
    test_truncated_file(path);
    test_bad_magic(path);
    remove_db(path);
    printf("disk_test ok\n");
    return 0;
}
