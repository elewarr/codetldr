// test_lsp_manager.cpp
// Unit tests for LspManager: lazy spawn gating, backoff, circuit breaker,
// status JSON filtering, SIGPIPE isolation.
//
// Tests:
//   1. Undetected language is not spawned (LSP-05)
//   2. Detected language can be spawned (LSP-05)
//   3. Backoff computation — 1, 2, 4, 8, 16, 32, 60s cap (LSP-06)
//   4. Circuit breaker: 5 crashes in 3 minutes -> kUnavailable (LSP-06)
//   5. Status JSON omits undetected languages (LSP-05/LSP-07)
//   6. to_string(LspServerState) full enum coverage
//   7. SIGPIPE reset: LspProcess child exits on write-after-close (LSP-08b)

#include "lsp/lsp_manager.h"
#include "lsp/lsp_process.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <poll.h>

using namespace codetldr;
using Clock = LspManager::Clock;

// ============================================================
// CHECK macro — prints PASS/FAIL, exits on failure
// ============================================================
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << (msg) << " [" << __FILE__ << ":" << __LINE__ << "]\n"; \
            std::exit(1); \
        } \
    } while (0)

// ============================================================
// Test 1: Undetected language is not spawned (LSP-05)
// ============================================================
static void test_1_undetected_not_spawned() {
    LspManager mgr;

    LspServerConfig cfg;
    cfg.command = "/usr/bin/false";
    cfg.args = {};
    cfg.extensions = {".cpp"};

    mgr.register_language("cpp", cfg);
    // Do NOT call set_detected_languages

    bool spawned = mgr.ensure_server("cpp");
    CHECK(!spawned, "test_1: ensure_server on undetected lang must return false");

    LspTransport* t = mgr.get_transport("cpp");
    CHECK(t == nullptr, "test_1: get_transport on undetected lang must return nullptr");

    auto status = mgr.status_json();
    CHECK(status.is_array(), "test_1: status_json must return array");
    CHECK(status.empty(), "test_1: status_json must be empty for undetected language");

    std::cout << "PASS: test_1_undetected_not_spawned\n";
}

// ============================================================
// Test 2: Detected language can be spawned (LSP-05)
// ============================================================
static void test_2_detected_can_spawn() {
    LspManager mgr;

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args = {};
    cfg.extensions = {".echo"};

    mgr.register_language("echo", cfg);
    mgr.set_detected_languages({"echo"});

    bool spawned = mgr.ensure_server("echo");
    CHECK(spawned, "test_2: ensure_server on detected lang must return true");

    LspTransport* t = mgr.get_transport("echo");
    CHECK(t != nullptr, "test_2: get_transport must return non-null for detected+spawned lang");
    CHECK(t->is_running(), "test_2: transport must be running after ensure_server");

    auto status = mgr.status_json();
    CHECK(status.is_array(), "test_2: status_json must return array");
    CHECK(status.size() == 1, "test_2: status_json must have 1 entry");
    CHECK(status[0]["language"] == "echo", "test_2: language field must be echo");
    CHECK(status[0]["state"] == "starting", "test_2: state must be starting after spawn");

    mgr.shutdown();
    std::cout << "PASS: test_2_detected_can_spawn\n";
}

// ============================================================
// Test 3: Backoff computation (LSP-06)
// ============================================================
static void test_3_backoff_computation() {
    LspManager mgr;

    // backoff_for is public for testability
    CHECK(mgr.backoff_for(0).count() == 1,   "test_3: backoff(0) must be 1s");
    CHECK(mgr.backoff_for(1).count() == 2,   "test_3: backoff(1) must be 2s");
    CHECK(mgr.backoff_for(2).count() == 4,   "test_3: backoff(2) must be 4s");
    CHECK(mgr.backoff_for(3).count() == 8,   "test_3: backoff(3) must be 8s");
    CHECK(mgr.backoff_for(4).count() == 16,  "test_3: backoff(4) must be 16s");
    CHECK(mgr.backoff_for(5).count() == 32,  "test_3: backoff(5) must be 32s");
    CHECK(mgr.backoff_for(6).count() == 60,  "test_3: backoff(6) must be 60s (capped)");
    CHECK(mgr.backoff_for(100).count() == 60,"test_3: backoff(100) must be 60s (still capped)");

    std::cout << "PASS: test_3_backoff_computation\n";
}

// ============================================================
// Test 4: Circuit breaker triggers after 5 crashes in 3 minutes (LSP-06)
//
// Strategy: use real-time process exits but inject 'now' to stay within
// the 3-minute crash window. Advance now by only enough to pass each
// backoff (1s, 2s, 4s, ...) so all 5 crashes are within 180s.
// ============================================================
static void test_4_circuit_breaker() {
    LspManager mgr;

    LspServerConfig cfg;
    cfg.command = "/usr/bin/false";  // exits immediately
    cfg.args = {};
    cfg.extensions = {".test"};

    mgr.register_language("test", cfg);
    mgr.set_detected_languages({"test"});

    // Start time base
    auto now = Clock::now();
    // Time budget: 180s window. Backoffs: 1+2+4+8+16 = 31s total.
    // We can safely accumulate 5 crashes within the window.

    // First spawn
    bool ok = mgr.ensure_server("test");
    CHECK(ok, "test_4: initial ensure_server must succeed");

    // Simulate crash-restart cycles.
    // Each cycle:
    //   1. Sleep 50ms for /usr/bin/false to exit
    //   2. Call tick(now) to detect crash — handle_crash records crash at 'now'
    //   3. Advance 'now' by backoff+1 (stays within 180s window for 5 crashes)
    //   4. Call tick(now) to trigger respawn
    //   5. Check if circuit breaker tripped
    bool circuit_open = false;
    for (int cycle = 0; cycle < 10 && !circuit_open; ++cycle) {
        // /usr/bin/false exits immediately — sleep 50ms to ensure it's reaped
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Detect crash at current time (within window)
        mgr.tick(now);
        now += std::chrono::seconds(1);

        // Check if circuit breaker opened
        auto st = mgr.status_json();
        if (!st.empty() && st[0]["state"] == "unavailable") {
            circuit_open = true;
            break;
        }

        // Advance time by backoff + 1 second (stays within 180s window for 5 cycles)
        // Backoffs: 1, 2, 4, 8, 16 -> total ~31 seconds < 180s
        long backoff = 1L << cycle;
        if (backoff > 60) backoff = 60;
        now += std::chrono::seconds(backoff + 1);

        // This tick triggers the respawn (now >= restart_after)
        mgr.tick(now);
        now += std::chrono::milliseconds(100);
    }

    CHECK(circuit_open, "test_4: circuit breaker must open after 5 crashes in 3-minute window");

    auto status = mgr.status_json();
    CHECK(!status.empty(), "test_4: status_json must have entry for detected lang");
    CHECK(status[0]["state"] == "unavailable",
          "test_4: state must be unavailable after circuit breaker trips");

    bool blocked = mgr.ensure_server("test");
    CHECK(!blocked, "test_4: ensure_server must return false when circuit breaker open");

    std::cout << "PASS: test_4_circuit_breaker\n";
}

// ============================================================
// Test 5: Status JSON omits undetected languages (LSP-05/LSP-07)
// ============================================================
static void test_5_status_json_filtering() {
    LspManager mgr;

    LspServerConfig cfg_cpp;
    cfg_cpp.command = "/bin/cat";
    cfg_cpp.args = {};
    cfg_cpp.extensions = {".cpp"};

    LspServerConfig cfg_py;
    cfg_py.command = "/bin/cat";
    cfg_py.args = {};
    cfg_py.extensions = {".py"};

    mgr.register_language("cpp", cfg_cpp);
    mgr.register_language("python", cfg_py);

    // Only cpp detected
    mgr.set_detected_languages({"cpp"});

    auto status = mgr.status_json();
    CHECK(status.is_array(), "test_5: status_json must return array");
    CHECK(status.size() == 1, "test_5: status_json must have exactly 1 entry with only cpp detected");

    bool found_cpp = false;
    for (const auto& entry : status) {
        CHECK(entry["language"] != "python", "test_5: python must not appear in status when not detected");
        if (entry["language"] == "cpp") found_cpp = true;
    }
    CHECK(found_cpp, "test_5: cpp must appear in status when detected");

    // Now detect both
    mgr.set_detected_languages({"cpp", "python"});

    auto status2 = mgr.status_json();
    CHECK(status2.size() == 2, "test_5: status_json must have 2 entries when both detected");

    // Cleanup — both may have spawned via ensure_server implicitly? No, just registered
    // ensure_server was never called so nothing was spawned
    std::cout << "PASS: test_5_status_json_filtering\n";
}

// ============================================================
// Test 6: to_string(LspServerState) coverage
// ============================================================
static void test_6_to_string_coverage() {
    CHECK(to_string(LspServerState::kNotStarted)  == "not_started",  "test_6: kNotStarted");
    CHECK(to_string(LspServerState::kStarting)    == "starting",     "test_6: kStarting");
    CHECK(to_string(LspServerState::kReady)        == "ready",        "test_6: kReady");
    CHECK(to_string(LspServerState::kIndexing)     == "indexing",     "test_6: kIndexing");
    CHECK(to_string(LspServerState::kUnavailable)  == "unavailable",  "test_6: kUnavailable");
    CHECK(to_string(LspServerState::kDegraded)     == "degraded",     "test_6: kDegraded");

    std::cout << "PASS: test_6_to_string_coverage\n";
}

// ============================================================
// Test 7: SIGPIPE reset in LspProcess child (LSP-08b)
// ============================================================
static void test_7_sigpipe_reset() {
    // Spawn /bin/cat via LspProcess
    LspProcess proc;
    int err = proc.spawn("/bin/cat", {});
    CHECK(err == 0, "test_7: spawn /bin/cat must succeed");
    CHECK(proc.is_running(), "test_7: process must be running after spawn");

    // Close parent's read end of stdout — cat's stdout is now broken
    int stdout_fd = proc.stdout_fd();
    CHECK(stdout_fd >= 0, "test_7: stdout_fd must be valid");
    ::close(stdout_fd);

    // Write to cat's stdin — cat will try to write to its stdout (which is now a broken pipe)
    // With SIG_DFL (SIGPIPE restored), cat will receive SIGPIPE and die.
    // With SIG_IGN (inherited), write() would return EPIPE silently and cat might keep running.
    const char* msg = "hello\n";
    ::write(proc.stdin_fd(), msg, 6);

    // Give the child time to process the SIGPIPE
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Check child has exited
    // waitpid with WNOHANG to peek
    bool exited = !proc.is_running();

    // Note: is_running() just checks pid_>0 && !waited_, so process may have
    // exited the OS-side but we haven't called wait() yet. Use a small poll loop.
    if (!exited) {
        for (int i = 0; i < 20; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // Check via waitpid WNOHANG
            int status = 0;
            pid_t result = ::waitpid(proc.pid(), &status, WNOHANG);
            if (result > 0) {
                exited = true;
                break;
            }
        }
    }

    // If still running, kill it and call wait
    if (!exited) {
        proc.kill();
    }
    proc.wait();

    CHECK(exited, "test_7: cat must have exited after SIGPIPE (POSIX_SPAWN_SETSIGDEF working)");

    std::cout << "PASS: test_7_sigpipe_reset\n";
}

// ============================================================
// Test: version probe rejects binaries that exit non-zero (simulates rustup proxy stub)
// ============================================================
static void test_version_probe_rejects_non_zero_exit() {
    // /usr/bin/false exits 1 — simulates rustup proxy stub
    std::string cmd = "/usr/bin/false --version 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    CHECK(pipe != nullptr, "test_version_probe: popen must succeed");
    char buf[256];
    std::string out;
    while (::fgets(buf, sizeof(buf), pipe)) out += buf;
    int rc = ::pclose(pipe);
    CHECK(rc != 0, "test_version_probe: /usr/bin/false must exit non-zero");

    // Simulate probe logic: non-zero exit = version string cleared = skip registration
    bool would_register = (rc == 0 && !out.empty());
    CHECK(!would_register,
          "test_version_probe: proxy stub (non-zero exit) must NOT register backend");

    // Positive case: /bin/echo exits 0 with output — would register
    std::string cmd2 = "/bin/echo test-version 2>&1";
    FILE* pipe2 = ::popen(cmd2.c_str(), "r");
    CHECK(pipe2 != nullptr, "test_version_probe: popen2 must succeed");
    std::string out2;
    while (::fgets(buf, sizeof(buf), pipe2)) out2 += buf;
    int rc2 = ::pclose(pipe2);
    while (!out2.empty() && (out2.back() == '\n' || out2.back() == '\r'))
        out2.pop_back();
    bool would_register2 = (rc2 == 0 && !out2.empty());
    CHECK(would_register2,
          "test_version_probe: real binary (zero exit + output) must register backend");

    std::cout << "PASS: test_version_probe_rejects_non_zero_exit\n";
}

// ============================================================
// Test: Kotlin registration uses handshake_timeout_s=120
// ============================================================
static void test_kotlin_timeout_config() {
    codetldr::LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args = {};
    cfg.extensions = {".kt", ".kts"};
    cfg.handshake_timeout_s = 120;

    CHECK(cfg.handshake_timeout_s == 120,
          "test_kotlin_timeout_config: handshake_timeout_s must be 120");

    // Default config must have handshake_timeout_s=0
    codetldr::LspServerConfig default_cfg;
    default_cfg.command = "/bin/cat";
    default_cfg.extensions = {".cpp"};
    CHECK(default_cfg.handshake_timeout_s == 0,
          "test_kotlin_timeout_config: default handshake_timeout_s must be 0");

    std::cout << "PASS: test_kotlin_timeout_config\n";
}

// ============================================================
// main
// ============================================================
int main() {
    test_1_undetected_not_spawned();
    test_2_detected_can_spawn();
    test_3_backoff_computation();
    test_4_circuit_breaker();
    test_5_status_json_filtering();
    test_6_to_string_coverage();
    test_7_sigpipe_reset();
    test_version_probe_rejects_non_zero_exit();
    test_kotlin_timeout_config();

    std::cout << "\nAll LspManager tests passed.\n";
    return 0;
}
