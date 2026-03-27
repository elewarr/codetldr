// Test: LspProcess posix_spawn wrapper
// Tests:
//   1. spawn_cat: spawn /bin/cat, verify fds >= 0 and pid > 0
//   2. write_read_cat: write "hello\n" to stdin, read "hello\n" from stdout
//   3. kill_and_wait: kill, wait returns, stdin_fd returns -1
//   4. nonblocking_stdout: stdout_fd has O_NONBLOCK, read returns EAGAIN
//   5. spawn_failure: spawn non-existent binary, pid == -1

#include "lsp/lsp_process.h"

#include <cassert>
#include <iostream>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace {

// Helper: wait for data to be available on fd (blocking up to timeout_ms)
static bool wait_readable(int fd, int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) > 0;
}

// ============================================================
// Test 1: spawn_cat
// ============================================================
static void test_1_spawn_cat() {
    std::cout << "Test 1: spawn_cat..." << std::flush;

    codetldr::LspProcess proc;
    int err = proc.spawn("/bin/cat", {});
    assert(err == 0 && "Test 1: spawn should succeed");
    assert(proc.stdin_fd() >= 0  && "Test 1: stdin_fd should be valid");
    assert(proc.stdout_fd() >= 0 && "Test 1: stdout_fd should be valid");
    assert(proc.stderr_fd() >= 0 && "Test 1: stderr_fd should be valid");
    assert(proc.pid() > 0        && "Test 1: pid should be > 0");
    assert(proc.is_running()     && "Test 1: is_running should be true");

    proc.kill();
    proc.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 2: write_read_cat
// ============================================================
static void test_2_write_read_cat() {
    std::cout << "Test 2: write_read_cat..." << std::flush;

    codetldr::LspProcess proc;
    int err = proc.spawn("/bin/cat", {});
    assert(err == 0 && "Test 2: spawn should succeed");

    const char* msg = "hello\n";
    ssize_t written = ::write(proc.stdin_fd(), msg, strlen(msg));
    assert(written == static_cast<ssize_t>(strlen(msg)) && "Test 2: write should succeed");

    // Wait for cat to echo the data back
    bool readable = wait_readable(proc.stdout_fd(), 1000);
    assert(readable && "Test 2: stdout should become readable");

    char buf[16] = {};
    ssize_t nread = ::read(proc.stdout_fd(), buf, sizeof(buf) - 1);
    assert(nread > 0 && "Test 2: read should return data");
    assert(std::string(buf, nread) == "hello\n" && "Test 2: echoed data should match");

    proc.kill();
    proc.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 3: kill_and_wait
// ============================================================
static void test_3_kill_and_wait() {
    std::cout << "Test 3: kill_and_wait..." << std::flush;

    codetldr::LspProcess proc;
    int err = proc.spawn("/bin/cat", {});
    assert(err == 0 && "Test 3: spawn should succeed");
    assert(proc.is_running() && "Test 3: should be running after spawn");

    proc.kill();
    int exit_code = proc.wait();

    // After wait(), fds should be closed (-1)
    assert(proc.stdin_fd()  == -1 && "Test 3: stdin_fd should be -1 after wait");
    assert(proc.stdout_fd() == -1 && "Test 3: stdout_fd should be -1 after wait");
    assert(proc.stderr_fd() == -1 && "Test 3: stderr_fd should be -1 after wait");
    assert(!proc.is_running() && "Test 3: is_running should be false after wait");
    // exit_code may be -1 (signal termination) or 0 depending on platform — just check it completed
    (void)exit_code;

    std::cout << " PASS\n";
}

// ============================================================
// Test 4: nonblocking_stdout
// ============================================================
static void test_4_nonblocking_stdout() {
    std::cout << "Test 4: nonblocking_stdout..." << std::flush;

    codetldr::LspProcess proc;
    int err = proc.spawn("/bin/cat", {});
    assert(err == 0 && "Test 4: spawn should succeed");

    // Verify O_NONBLOCK is set on stdout_fd
    int flags = ::fcntl(proc.stdout_fd(), F_GETFL, 0);
    assert(flags != -1 && "Test 4: fcntl should succeed");
    assert((flags & O_NONBLOCK) != 0 && "Test 4: stdout_fd should have O_NONBLOCK");

    // Read when no data available — should return EAGAIN
    char buf[16];
    ssize_t nread = ::read(proc.stdout_fd(), buf, sizeof(buf));
    assert(nread == -1 && "Test 4: read should return -1 with no data");
    assert((errno == EAGAIN || errno == EWOULDBLOCK) &&
           "Test 4: errno should be EAGAIN or EWOULDBLOCK");

    proc.kill();
    proc.wait();
    std::cout << " PASS\n";
}

// ============================================================
// Test 5: spawn_failure
// ============================================================
static void test_5_spawn_failure() {
    std::cout << "Test 5: spawn_failure..." << std::flush;

    codetldr::LspProcess proc;
    int err = proc.spawn("/nonexistent/binary", {});
    assert(err != 0 && "Test 5: spawn should fail for non-existent binary");
    assert(proc.pid() == -1 && "Test 5: pid should be -1 after failed spawn");
    assert(!proc.is_running() && "Test 5: is_running should be false after failed spawn");
    assert(proc.stdin_fd()  == -1 && "Test 5: stdin_fd should be -1 after failed spawn");
    assert(proc.stdout_fd() == -1 && "Test 5: stdout_fd should be -1 after failed spawn");
    assert(proc.stderr_fd() == -1 && "Test 5: stderr_fd should be -1 after failed spawn");

    std::cout << " PASS\n";
}

} // anonymous namespace

int main() {
    std::cout << "=== LspProcess Tests ===\n";

    try {
        test_1_spawn_cat();
        test_2_write_read_cat();
        test_3_kill_and_wait();
        test_4_nonblocking_stdout();
        test_5_spawn_failure();

        std::cout << "\nAll tests passed!\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nFATAL: " << ex.what() << "\n";
        return 1;
    }
}
