#ifndef PAGEDB_TEST_UTIL_HPP
#define PAGEDB_TEST_UTIL_HPP

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>

#include "pagedb/status.hpp"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define CHECK_OK(st)                                                         \
    do {                                                                     \
        pagedb::Status _s = (st);                                            \
        if (!_s.ok()) {                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__,                   \
                   _s.to_string().c_str());                                  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define CHECK_CODE(st, expected)                                             \
    do {                                                                     \
        pagedb::Status _s = (st);                                            \
        if (_s.code() != (expected)) {                                       \
            printf("FAIL %s:%d  got %s\n", __FILE__, __LINE__,               \
                   _s.to_string().c_str());                                  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

// Unique file name per test process so tests can run in parallel.
inline std::string temp_path(const char* name) {
    std::string p = "/tmp/pagedb_test_";
    p += name;
    p += "_";
    p += std::to_string((long)getpid());
    p += ".db";
    ::unlink(p.c_str());
    return p;
}

inline void remove_db(const std::string& path) {
    ::unlink(path.c_str());
    ::unlink((path + ".wal").c_str());
}

#endif
