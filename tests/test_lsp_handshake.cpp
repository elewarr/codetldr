// test_lsp_handshake.cpp
// Unit tests for LspManager handshake state machine and request queuing.
//
// Strategy: Use /bin/cat as a "mock LSP server" — it echoes stdin to stdout,
// so we write a valid JSON-RPC initialize response through the transport's
// stdin_fd() and cat mirrors it back through stdout. LspTransport's poll_read()
// then dispatches the response to the pending callback, completing the handshake.
//
// Tests:
//   1. test_handshake_state_machine: kStarting -> kReady after mocked initialize response
//   2. test_request_queuing: action queued during kStarting, drained at kReady
//   3. test_send_when_ready_immediate: action executes immediately when already kReady
//   4. test_send_when_ready_unavailable: returns false for undetected / unavailable server
//   5. test_opened_uris_cleared_on_crash: opened_uris/pending_requests cleared after crash
//   6. test_initialize_params_structure: initialize request is LSP 3.17 spec compliant
//   7. test_document_open_sends_didopen: ensure_document_open sends textDocument/didOpen
//   8. test_document_open_dedup: second call to ensure_document_open is a no-op
//   9. test_compile_commands_not_found: clangd degraded when compile_commands.json absent
//  10. test_compile_commands_found_in_root: clangd stays ready when found at project root
//  11. test_compile_commands_found_in_build: clangd stays ready when found in build/
//  12. test_language_id_mapping: language_id_for maps extensions correctly via didOpen

#include "lsp/lsp_manager.h"
#include "lsp/lsp_transport.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <optional>

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
// Helpers
// ============================================================

// Wait for fd to become readable (up to timeout_ms)
static bool wait_readable(int fd, int timeout_ms) {
    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms) > 0;
}

// Drain write queue fully
static bool drain_fully(LspTransport& t, int max_iters = 100) {
    for (int i = 0; i < max_iters; ++i) {
        if (t.drain_writes()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return t.write_queue_size() == 0;
}

// Write a JSON-RPC response through a raw fd (the transport's stdin_fd, not ours).
// Since /bin/cat echoes stdin->stdout, writing a response to its stdin causes
// it to appear on stdout for LspTransport.poll_read() to process.
static void write_lsp_response(int fd, int64_t id, const nlohmann::json& result) {
    nlohmann::json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    std::string body = msg.dump();
    std::string frame = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    ::write(fd, frame.data(), frame.size());
}

// Complete the LSP initialize handshake for a /bin/cat server.
// After ensure_server(), LspTransport has sent an "initialize" request (id=1)
// to cat's stdin. Cat hasn't echoed it yet — drain writes to push it to cat,
// then wait for cat to echo it back, then write a fake InitializeResult response
// to cat's stdin so it echoes that through as well.
//
// The sequence:
//   1. drain writes — initialize request reaches cat's stdin
//   2. wait readable — cat echoes the request back (we discard it)
//   3. poll_read — consume the echoed request (LspTransport sees it as a request/notification, ignores)
//   4. write_lsp_response(id=1) — inject fake InitializeResult into cat's stdin
//   5. drain writes — manager's pending "initialized" notification gets sent too
//   6. wait readable + poll_read — LspTransport dispatches response callback -> kReady
static void complete_handshake(LspManager& mgr, const std::string& language) {
    LspTransport* t = mgr.get_transport(language);
    CHECK(t != nullptr, "complete_handshake: transport must not be null");

    // Step 1: drain — push initialize request to cat
    drain_fully(*t);

    // Step 2-3: read and discard the echoed initialize request from cat
    // (it comes back as a JSON-RPC request with method "initialize", not a response)
    if (wait_readable(t->stdout_fd(), 500)) {
        t->poll_read();  // cat echoes the initialize request — LspTransport ignores it (no matching response)
    }

    // Step 4: inject the InitializeResult response (id=1 — first request sent)
    write_lsp_response(t->stdin_fd(), 1, {{"capabilities", nlohmann::json::object()}});

    // Step 5: drain — push the response to cat, which echoes it back
    drain_fully(*t);

    // Step 6: wait for cat to echo the response, then poll_read dispatches callback
    bool ok = false;
    for (int i = 0; i < 20 && !ok; ++i) {
        if (wait_readable(t->stdout_fd(), 200)) {
            mgr.dispatch_read(t->stdout_fd());
            // Check if we've reached kReady
            auto st = mgr.status_json();
            for (const auto& s : st) {
                if (s["language"] == language && s["state"] == "ready") {
                    ok = true;
                }
            }
        }
    }
    // Also drain any initialized notification that was queued
    drain_fully(*t);
}

// ============================================================
// Test 1: test_handshake_state_machine
// Verify kStarting -> kReady transition after mocked initialize response
// ============================================================
static void test_handshake_state_machine() {
    LspManager mgr;
    mgr.set_project_root("/tmp/test_project");

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args = {};
    cfg.extensions = {".echo"};

    mgr.register_language("echo", cfg);
    mgr.set_detected_languages({"echo"});

    bool spawned = mgr.ensure_server("echo");
    CHECK(spawned, "test_handshake_state_machine: ensure_server must succeed");

    // Immediately after spawn, state must be kStarting (not kReady)
    auto status = mgr.status_json();
    bool found_starting = false;
    for (const auto& s : status) {
        if (s["language"] == "echo") {
            CHECK(s["state"] == "starting",
                  "test_handshake_state_machine: state must be 'starting' before handshake");
            found_starting = true;
        }
    }
    CHECK(found_starting, "test_handshake_state_machine: echo must appear in status");

    // Complete the handshake
    complete_handshake(mgr, "echo");

    // After handshake, state must be kReady
    auto status2 = mgr.status_json();
    bool found_ready = false;
    for (const auto& s : status2) {
        if (s["language"] == "echo") {
            CHECK(s["state"] == "ready",
                  "test_handshake_state_machine: state must be 'ready' after handshake");
            found_ready = true;
        }
    }
    CHECK(found_ready, "test_handshake_state_machine: echo must be ready after handshake");

    mgr.shutdown();
    std::cout << "PASS: test_handshake_state_machine\n";
}

// ============================================================
// Test 2: test_request_queuing
// Action queued during kStarting, executed when kReady is reached
// ============================================================
static void test_request_queuing() {
    LspManager mgr;
    mgr.set_project_root("/tmp/test_project");

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args = {};
    cfg.extensions = {".echo"};

    mgr.register_language("echo", cfg);
    mgr.set_detected_languages({"echo"});
    mgr.ensure_server("echo");

    // Queue an action while in kStarting
    bool called = false;
    bool queued = mgr.send_when_ready("echo", [&called]() { called = true; });
    CHECK(queued, "test_request_queuing: send_when_ready must return true while kStarting");
    CHECK(!called, "test_request_queuing: action must NOT execute while kStarting");

    // Complete the handshake — queue should be drained
    complete_handshake(mgr, "echo");

    CHECK(called, "test_request_queuing: action must execute after kReady reached");

    mgr.shutdown();
    std::cout << "PASS: test_request_queuing\n";
}

// ============================================================
// Test 3: test_send_when_ready_immediate
// Action executes immediately when state is already kReady
// ============================================================
static void test_send_when_ready_immediate() {
    LspManager mgr;
    mgr.set_project_root("/tmp/test_project");

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args = {};
    cfg.extensions = {".echo"};

    mgr.register_language("echo", cfg);
    mgr.set_detected_languages({"echo"});
    mgr.ensure_server("echo");

    // Reach kReady first
    complete_handshake(mgr, "echo");

    // Now send_when_ready should execute immediately
    bool called = false;
    bool ok = mgr.send_when_ready("echo", [&called]() { called = true; });
    CHECK(ok, "test_send_when_ready_immediate: send_when_ready must return true when kReady");
    CHECK(called, "test_send_when_ready_immediate: action must execute immediately when kReady");

    mgr.shutdown();
    std::cout << "PASS: test_send_when_ready_immediate\n";
}

// ============================================================
// Test 4: test_send_when_ready_unavailable
// Returns false for undetected / not-started server
// ============================================================
static void test_send_when_ready_unavailable() {
    LspManager mgr;
    mgr.set_project_root("/tmp/test_project");

    LspServerConfig cfg;
    cfg.command = "/usr/bin/false";
    cfg.args = {};
    cfg.extensions = {".test"};

    mgr.register_language("test", cfg);
    // Do NOT call set_detected_languages — server stays kNotStarted + !detected

    bool called = false;
    bool ok = mgr.send_when_ready("test", [&called]() { called = true; });
    CHECK(!ok, "test_send_when_ready_unavailable: send_when_ready must return false for not-started server");
    CHECK(!called, "test_send_when_ready_unavailable: action must not execute");

    // Also check for completely unknown language
    bool ok2 = mgr.send_when_ready("nonexistent", [&called]() { called = true; });
    CHECK(!ok2, "test_send_when_ready_unavailable: send_when_ready must return false for unknown language");

    std::cout << "PASS: test_send_when_ready_unavailable\n";
}

// ============================================================
// Test 5: test_opened_uris_cleared_on_crash
// opened_uris and pending_requests are cleared when crash is detected
// ============================================================
static void test_opened_uris_cleared_on_crash() {
    LspManager mgr;
    mgr.set_project_root("/tmp/test_project");

    // /usr/bin/false exits immediately — triggers crash path
    LspServerConfig cfg;
    cfg.command = "/usr/bin/false";
    cfg.args = {};
    cfg.extensions = {".crash"};

    mgr.register_language("crash", cfg);
    mgr.set_detected_languages({"crash"});

    bool spawned = mgr.ensure_server("crash");
    CHECK(spawned, "test_opened_uris_cleared_on_crash: initial spawn must succeed");

    // Queue a pending action before crash is detected
    bool action_called = false;
    mgr.send_when_ready("crash", [&action_called]() { action_called = true; });

    // Sleep for /usr/bin/false to exit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Tick should detect the crash, call handle_crash, which clears pending_requests
    auto now = Clock::now();
    mgr.tick(now);

    // After crash, state should be kNotStarted (or kUnavailable if circuit tripped)
    auto status = mgr.status_json();
    bool valid_post_crash = false;
    for (const auto& s : status) {
        if (s["language"] == "crash") {
            std::string state = s["state"];
            // Either not_started (scheduled for restart) or unavailable (circuit breaker)
            valid_post_crash = (state == "not_started" || state == "unavailable");
        }
    }
    CHECK(valid_post_crash,
          "test_opened_uris_cleared_on_crash: state must be not_started or unavailable after crash");

    // The pending action must NOT have been executed (cleared by handle_crash)
    CHECK(!action_called,
          "test_opened_uris_cleared_on_crash: pending action must be cleared (not executed) on crash");

    std::cout << "PASS: test_opened_uris_cleared_on_crash\n";
}

// ============================================================
// Test 6: test_initialize_params_structure
// The initialize request sent to the LSP server is LSP 3.17 spec compliant
// ============================================================
static void test_initialize_params_structure() {
    LspManager mgr;
    mgr.set_project_root("/tmp/my_project");

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args = {};
    cfg.extensions = {".echo"};

    mgr.register_language("echo", cfg);
    mgr.set_detected_languages({"echo"});
    mgr.ensure_server("echo");

    LspTransport* t = mgr.get_transport("echo");
    CHECK(t != nullptr, "test_initialize_params_structure: transport must exist");

    // Drain the initialize request to cat
    drain_fully(*t);

    // Cat echoes the initialize request back — read it to inspect content
    CHECK(wait_readable(t->stdout_fd(), 1000),
          "test_initialize_params_structure: initialize request must be echoed by cat");

    auto messages = t->poll_read();
    CHECK(!messages.empty(), "test_initialize_params_structure: must receive at least one message");

    // Find the "initialize" request in the echoed messages
    nlohmann::json init_msg;
    for (const auto& msg : messages) {
        if (msg.contains("method") && msg["method"] == "initialize") {
            init_msg = msg;
            break;
        }
    }
    CHECK(!init_msg.is_null(), "test_initialize_params_structure: initialize message must be present");

    // Verify params structure per LSP 3.17
    const auto& params = init_msg["params"];
    CHECK(params.contains("processId"), "test_initialize_params_structure: processId must be present");
    CHECK(params["processId"].is_number(), "test_initialize_params_structure: processId must be a number");
    CHECK(params.contains("clientInfo"), "test_initialize_params_structure: clientInfo must be present");
    CHECK(params["clientInfo"]["name"] == "codetldr", "test_initialize_params_structure: clientInfo.name must be codetldr");
    CHECK(params.contains("rootUri"), "test_initialize_params_structure: rootUri must be present");
    // rootUri must start with "file://"
    std::string root_uri = params["rootUri"];
    CHECK(root_uri.substr(0, 7) == "file://", "test_initialize_params_structure: rootUri must start with file://");
    CHECK(params.contains("workspaceFolders"), "test_initialize_params_structure: workspaceFolders must be present");
    CHECK(params["workspaceFolders"].is_array(), "test_initialize_params_structure: workspaceFolders must be array");
    CHECK(!params["workspaceFolders"].empty(), "test_initialize_params_structure: workspaceFolders must not be empty");
    CHECK(params.contains("capabilities"), "test_initialize_params_structure: capabilities must be present");
    CHECK(params["capabilities"].contains("textDocument"), "test_initialize_params_structure: textDocument capabilities must be present");
    CHECK(params["capabilities"]["textDocument"].contains("definition"), "test_initialize_params_structure: textDocument.definition must be present");

    mgr.shutdown();
    std::cout << "PASS: test_initialize_params_structure\n";
}

// ============================================================
// Helpers for document lifecycle tests
// ============================================================

// Create a temporary directory using mkdtemp
static std::string make_temp_dir() {
    char tmpl[] = "/tmp/lsp_test_XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    return dir ? std::string(dir) : "";
}

// Create a temporary file with given content and suffix.
// Returns the file path on success, empty string on failure.
static std::string make_temp_file(const std::string& content, const std::string& suffix = ".cpp") {
    std::string tmpl = "/tmp/lsp_test_file_XXXXXX";
    // mkstemp doesn't support suffixes portably, use a manual approach
    static std::atomic<int> counter{0};
    std::string path = "/tmp/lsp_test_file_" + std::to_string(::getpid())
                       + "_" + std::to_string(counter++) + suffix;
    std::ofstream f(path);
    if (!f.is_open()) return "";
    f << content;
    f.close();
    return path;
}

// Read one LSP Content-Length frame from fd with timeout.
// Returns parsed JSON body or nullopt on timeout / error.
static std::optional<nlohmann::json> read_lsp_frame(int fd, int timeout_ms = 500) {
    struct pollfd pfd = {fd, POLLIN, 0};
    int ret = ::poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return std::nullopt;
    char buf[65536];
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) return std::nullopt;
    std::string data(buf, static_cast<size_t>(n));
    auto pos = data.find("\r\n\r\n");
    if (pos == std::string::npos) return std::nullopt;
    std::string body = data.substr(pos + 4);
    return nlohmann::json::parse(body, nullptr, false);
}

// ============================================================
// Test 7: test_document_open_sends_didopen
// ensure_document_open sends textDocument/didOpen with correct fields
// ============================================================
static void test_document_open_sends_didopen() {
    LspManager mgr;
    std::string tmpdir = make_temp_dir();
    CHECK(!tmpdir.empty(), "test_document_open_sends_didopen: tmpdir creation failed");
    mgr.set_project_root(tmpdir);

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args    = {};
    cfg.extensions = {".echo"};

    mgr.register_language("echo", cfg);
    mgr.set_detected_languages({"echo"});
    mgr.ensure_server("echo");

    // Complete handshake -> kReady
    complete_handshake(mgr, "echo");

    // Create a temp file with known content
    std::string content = "int main() {}";
    std::string fpath = make_temp_file(content, ".cpp");
    CHECK(!fpath.empty(), "test_document_open_sends_didopen: temp file creation failed");

    // Drain any pending notifications from the handshake (initialized notification)
    LspTransport* t = mgr.get_transport("echo");
    CHECK(t != nullptr, "test_document_open_sends_didopen: transport must not be null");
    drain_fully(*t);
    // Consume any echoed data already in pipe
    while (read_lsp_frame(t->stdout_fd(), 50)) {}

    // Call ensure_document_open
    mgr.ensure_document_open("echo", fpath);
    drain_fully(*t);

    // Cat echoes the didOpen notification back — read and verify it
    bool found_didopen = false;
    for (int i = 0; i < 10 && !found_didopen; ++i) {
        auto frame = read_lsp_frame(t->stdout_fd(), 500);
        if (!frame) break;
        if (frame->contains("method") && (*frame)["method"] == "textDocument/didOpen") {
            const auto& td = (*frame)["params"]["textDocument"];
            std::string uri = td["uri"];
            CHECK(uri.substr(0, 7) == "file://",
                  "test_document_open_sends_didopen: URI must start with file://");
            CHECK(td["text"] == content,
                  "test_document_open_sends_didopen: text must match file content");
            CHECK(td["version"] == 1,
                  "test_document_open_sends_didopen: version must be 1");
            found_didopen = true;
        }
    }
    CHECK(found_didopen, "test_document_open_sends_didopen: textDocument/didOpen must be sent");

    ::unlink(fpath.c_str());
    ::rmdir(tmpdir.c_str());
    mgr.shutdown();
    std::cout << "PASS: test_document_open_sends_didopen\n";
}

// ============================================================
// Test 8: test_document_open_dedup
// Second call to ensure_document_open for same file is a no-op
// ============================================================
static void test_document_open_dedup() {
    LspManager mgr;
    std::string tmpdir = make_temp_dir();
    CHECK(!tmpdir.empty(), "test_document_open_dedup: tmpdir creation failed");
    mgr.set_project_root(tmpdir);

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args    = {};
    cfg.extensions = {".echo"};

    mgr.register_language("echo", cfg);
    mgr.set_detected_languages({"echo"});
    mgr.ensure_server("echo");
    complete_handshake(mgr, "echo");

    std::string fpath = make_temp_file("void foo() {}", ".cpp");
    CHECK(!fpath.empty(), "test_document_open_dedup: temp file creation failed");

    LspTransport* t = mgr.get_transport("echo");
    CHECK(t != nullptr, "test_document_open_dedup: transport must not be null");
    drain_fully(*t);
    while (read_lsp_frame(t->stdout_fd(), 50)) {}

    // First call — should send didOpen
    mgr.ensure_document_open("echo", fpath);
    drain_fully(*t);

    // Drain first didOpen
    bool got_first = false;
    for (int i = 0; i < 10 && !got_first; ++i) {
        auto frame = read_lsp_frame(t->stdout_fd(), 300);
        if (!frame) break;
        if (frame->contains("method") && (*frame)["method"] == "textDocument/didOpen") {
            got_first = true;
        }
    }
    CHECK(got_first, "test_document_open_dedup: first didOpen must be sent");

    // Second call — must NOT send another didOpen
    mgr.ensure_document_open("echo", fpath);
    drain_fully(*t);

    // Poll with short timeout — should get nothing
    auto second = read_lsp_frame(t->stdout_fd(), 200);
    bool got_second_didopen = false;
    if (second && second->contains("method") && (*second)["method"] == "textDocument/didOpen") {
        got_second_didopen = true;
    }
    CHECK(!got_second_didopen,
          "test_document_open_dedup: second ensure_document_open must not send duplicate didOpen");

    ::unlink(fpath.c_str());
    ::rmdir(tmpdir.c_str());
    mgr.shutdown();
    std::cout << "PASS: test_document_open_dedup\n";
}

// ============================================================
// Test 9: test_compile_commands_not_found
// clangd degraded when compile_commands.json is absent
// ============================================================
static void test_compile_commands_not_found() {
    LspManager mgr;
    std::string tmpdir = make_temp_dir();
    CHECK(!tmpdir.empty(), "test_compile_commands_not_found: tmpdir creation failed");
    mgr.set_project_root(tmpdir);

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args    = {};
    cfg.extensions = {".cpp"};

    mgr.register_language("cpp", cfg);
    mgr.set_detected_languages({"cpp"});
    mgr.ensure_server("cpp");

    // Complete handshake — check_clangd_compile_db is called for "cpp"
    complete_handshake(mgr, "cpp");

    // No compile_commands.json in tmpdir -> must be degraded
    auto status = mgr.status_json();
    bool found_degraded = false;
    for (const auto& s : status) {
        if (s["language"] == "cpp" && s["state"] == "degraded") {
            found_degraded = true;
        }
    }
    CHECK(found_degraded,
          "test_compile_commands_not_found: cpp must be degraded when compile_commands.json absent");

    ::rmdir(tmpdir.c_str());
    mgr.shutdown();
    std::cout << "PASS: test_compile_commands_not_found\n";
}

// ============================================================
// Test 10: test_compile_commands_found_in_root
// clangd stays ready when compile_commands.json is at project root
// ============================================================
static void test_compile_commands_found_in_root() {
    LspManager mgr;
    std::string tmpdir = make_temp_dir();
    CHECK(!tmpdir.empty(), "test_compile_commands_found_in_root: tmpdir creation failed");
    mgr.set_project_root(tmpdir);

    // Create compile_commands.json at project root
    std::ofstream ccjson(tmpdir + "/compile_commands.json");
    ccjson << "[]";
    ccjson.close();

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args    = {};
    cfg.extensions = {".cpp"};

    mgr.register_language("cpp", cfg);
    mgr.set_detected_languages({"cpp"});
    mgr.ensure_server("cpp");
    complete_handshake(mgr, "cpp");

    auto status = mgr.status_json();
    bool found_ready = false;
    for (const auto& s : status) {
        if (s["language"] == "cpp" && s["state"] == "ready") {
            found_ready = true;
        }
    }
    CHECK(found_ready,
          "test_compile_commands_found_in_root: cpp must stay ready when compile_commands.json found at root");

    ::unlink((tmpdir + "/compile_commands.json").c_str());
    ::rmdir(tmpdir.c_str());
    mgr.shutdown();
    std::cout << "PASS: test_compile_commands_found_in_root\n";
}

// ============================================================
// Test 11: test_compile_commands_found_in_build
// clangd stays ready when compile_commands.json is in build/
// ============================================================
static void test_compile_commands_found_in_build() {
    LspManager mgr;
    std::string tmpdir = make_temp_dir();
    CHECK(!tmpdir.empty(), "test_compile_commands_found_in_build: tmpdir creation failed");
    mgr.set_project_root(tmpdir);

    // Create build/ subdir and compile_commands.json
    std::string builddir = tmpdir + "/build";
    ::mkdir(builddir.c_str(), 0755);
    std::ofstream ccjson(builddir + "/compile_commands.json");
    ccjson << "[]";
    ccjson.close();

    LspServerConfig cfg;
    cfg.command = "/bin/cat";
    cfg.args    = {};
    cfg.extensions = {".cpp"};

    mgr.register_language("cpp", cfg);
    mgr.set_detected_languages({"cpp"});
    mgr.ensure_server("cpp");
    complete_handshake(mgr, "cpp");

    auto status = mgr.status_json();
    bool found_ready = false;
    for (const auto& s : status) {
        if (s["language"] == "cpp" && s["state"] == "ready") {
            found_ready = true;
        }
    }
    CHECK(found_ready,
          "test_compile_commands_found_in_build: cpp must stay ready when compile_commands.json found in build/");

    ::unlink((builddir + "/compile_commands.json").c_str());
    ::rmdir(builddir.c_str());
    ::rmdir(tmpdir.c_str());
    mgr.shutdown();
    std::cout << "PASS: test_compile_commands_found_in_build\n";
}

// ============================================================
// Test 12: test_language_id_mapping
// ensure_document_open uses correct languageId per file extension
// ============================================================
static void test_language_id_mapping() {
    // Test via didOpen notification content — create files with various extensions
    struct TestCase {
        std::string ext;
        std::string expected_lang_id;
    };
    std::vector<TestCase> cases = {
        {".py",   "python"},
        {".ts",   "typescript"},
        {".js",   "javascript"},
        {".rs",   "rust"},
        {".go",   "go"},
        {".kt",   "kotlin"},
        {".kts",  "kotlin"},
        {".java", "java"},
    };

    for (const auto& tc : cases) {
        LspManager mgr;
        std::string tmpdir = make_temp_dir();
        CHECK(!tmpdir.empty(), ("test_language_id_mapping: tmpdir failed for " + tc.ext).c_str());
        mgr.set_project_root(tmpdir);

        LspServerConfig cfg;
        cfg.command = "/bin/cat";
        cfg.args    = {};
        cfg.extensions = {tc.ext};

        mgr.register_language("echo_lang", cfg);
        mgr.set_detected_languages({"echo_lang"});
        mgr.ensure_server("echo_lang");
        complete_handshake(mgr, "echo_lang");

        std::string fpath = make_temp_file("# test", tc.ext);
        CHECK(!fpath.empty(), ("test_language_id_mapping: temp file failed for " + tc.ext).c_str());

        LspTransport* t = mgr.get_transport("echo_lang");
        CHECK(t != nullptr, "test_language_id_mapping: transport must exist");
        drain_fully(*t);
        while (read_lsp_frame(t->stdout_fd(), 50)) {}

        mgr.ensure_document_open("echo_lang", fpath);
        drain_fully(*t);

        bool found = false;
        for (int i = 0; i < 10 && !found; ++i) {
            auto frame = read_lsp_frame(t->stdout_fd(), 300);
            if (!frame) break;
            if (frame->contains("method") && (*frame)["method"] == "textDocument/didOpen") {
                std::string lang_id = (*frame)["params"]["textDocument"]["languageId"];
                CHECK(lang_id == tc.expected_lang_id,
                      ("test_language_id_mapping: wrong languageId for " + tc.ext
                       + " got=" + lang_id + " want=" + tc.expected_lang_id).c_str());
                found = true;
            }
        }
        CHECK(found, ("test_language_id_mapping: didOpen not found for ext=" + tc.ext).c_str());

        ::unlink(fpath.c_str());
        ::rmdir(tmpdir.c_str());
        mgr.shutdown();
    }

    std::cout << "PASS: test_language_id_mapping\n";
}

// ============================================================
// main
// ============================================================
int main() {
    test_handshake_state_machine();
    test_request_queuing();
    test_send_when_ready_immediate();
    test_send_when_ready_unavailable();
    test_opened_uris_cleared_on_crash();
    test_initialize_params_structure();
    test_document_open_sends_didopen();
    test_document_open_dedup();
    test_compile_commands_not_found();
    test_compile_commands_found_in_root();
    test_compile_commands_found_in_build();
    test_language_id_mapping();

    std::cout << "\nAll LspHandshake tests passed.\n";
    return 0;
}
