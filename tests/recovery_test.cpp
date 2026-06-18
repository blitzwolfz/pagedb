#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pagedb/database.hpp"
#include "test_util.hpp"

using namespace pagedb;

static std::string key_of(int i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%06d", i);
    return std::string(buf);
}

static std::string value_of(int i) {
    char buf[64];
    snprintf(buf, sizeof(buf), "value-%d-%d", i, i * 7);
    return std::string(buf);
}

static std::unique_ptr<Database> open_db(const std::string& path) {
    DatabaseOptions options;
    options.path = path;
    options.buffer_pool_pages = 32;
    options.durable = true;
    Result<std::unique_ptr<Database>> r = Database::open(options);
    CHECK(r.ok());
    return r.take();
}

// Writes keys and tells the parent about every one that came back ok. Never
// closes the database, the parent kills it.
static void child_writer(const std::string& path, int fd, int count,
                         int checkpoint_every) {
    DatabaseOptions options;
    options.path = path;
    options.buffer_pool_pages = 32;
    options.durable = true;
    Result<std::unique_ptr<Database>> r = Database::open(options);
    if (!r.ok()) {
        _exit(2);
    }
    std::unique_ptr<Database> db = r.take();

    for (int i = 0; i < count; i++) {
        if (!db->put(key_of(i), value_of(i)).ok()) {
            _exit(3);
        }
        char line[32];
        int len = snprintf(line, sizeof(line), "%d\n", i);
        if (write(fd, line, (size_t)len) != len) {
            _exit(4);
        }
        if (checkpoint_every > 0 && (i + 1) % checkpoint_every == 0) {
            if (!db->checkpoint().ok()) {
                _exit(5);
            }
        }
    }
    _exit(0);
}

// Runs a child that writes until the parent has seen kill_after answers, then
// kills it with SIGKILL. Returns the last key the child got an ok for.
static int run_and_kill(const std::string& path, int count, int kill_after,
                        int checkpoint_every) {
    int pipe_fd[2];
    CHECK(pipe(pipe_fd) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(pipe_fd[0]);
        child_writer(path, pipe_fd[1], count, checkpoint_every);
        _exit(0);
    }

    close(pipe_fd[1]);
    FILE* in = fdopen(pipe_fd[0], "r");
    CHECK(in != NULL);

    int acked = -1;
    int seen = 0;
    char line[64];
    while (fgets(line, sizeof(line), in) != NULL) {
        int i = atoi(line);
        if (i > acked) {
            acked = i;
        }
        seen++;
        if (seen >= kill_after) {
            kill(pid, SIGKILL);
            break;
        }
    }
    fclose(in);

    int status = 0;
    waitpid(pid, &status, 0);
    return acked;
}

static void check_acked_keys(const std::string& path, int acked) {
    std::unique_ptr<Database> db = open_db(path);
    CHECK_OK(db->verify());
    for (int i = 0; i <= acked; i++) {
        Result<std::optional<std::string>> g = db->get(key_of(i));
        CHECK(g.ok());
        if (!g.value().has_value()) {
            printf("key %d was acknowledged but is gone\n", i);
            exit(1);
        }
        CHECK(g.value().value() == value_of(i));
    }
    CHECK_OK(db->close());
}

static void test_kill_while_writing(const std::string& path) {
    int acked = run_and_kill(path, 5000, 800, 0);
    CHECK(acked >= 700);
    check_acked_keys(path, acked);
}

static void test_kill_during_checkpoints(const std::string& path) {
    int acked = run_and_kill(path, 5000, 900, 100);
    CHECK(acked >= 700);
    check_acked_keys(path, acked);
}

// Cuts bytes off the end of the log, which is what a half written record
// looks like after a crash.
static void test_broken_log_tail(const std::string& path) {
    int acked = run_and_kill(path, 2000, 500, 0);
    CHECK(acked >= 400);

    std::string wal = path + ".wal";
    struct stat st;
    CHECK(stat(wal.c_str(), &st) == 0);
    CHECK(st.st_size > 0);
    CHECK(truncate(wal.c_str(), st.st_size - 100) == 0);

    // The last operation may be lost now, everything before it must be there.
    std::unique_ptr<Database> db = open_db(path);
    CHECK_OK(db->verify());
    for (int i = 0; i < acked - 1; i++) {
        Result<std::optional<std::string>> g = db->get(key_of(i));
        CHECK(g.ok());
        CHECK(g.value().has_value());
    }
    CHECK_OK(db->close());
}

static void test_garbage_in_the_log(const std::string& path) {
    int acked = run_and_kill(path, 2000, 400, 0);
    CHECK(acked >= 300);

    std::string wal = path + ".wal";
    FILE* f = fopen(wal.c_str(), "a");
    CHECK(f != NULL);
    for (int i = 0; i < 200; i++) {
        fputc(0x7e, f);
    }
    fclose(f);

    // The garbage at the end is ignored, the database still opens.
    std::unique_ptr<Database> db = open_db(path);
    CHECK_OK(db->verify());
    Result<std::optional<std::string>> g = db->get(key_of(0));
    CHECK(g.ok());
    CHECK(g.value().has_value());
    CHECK_OK(db->close());
}

int main() {
    std::string path = temp_path("recovery");
    test_kill_while_writing(path);
    remove_db(path);
    test_kill_during_checkpoints(path);
    remove_db(path);
    test_broken_log_tail(path);
    remove_db(path);
    test_garbage_in_the_log(path);
    remove_db(path);
    printf("recovery_test ok\n");
    return 0;
}
