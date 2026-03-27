// MCP provider protocol round-trip tests
//
// Spawns codetldr-mcp as a subprocess via pipe+fork+exec, sends NDJSON
// messages on stdin, reads NDJSON responses from stdout, and validates
// the MCP 2025-11-25 protocol behavior.
//
// Tests:
//   1. initialize returns correct protocolVersion, serverInfo, capabilities
//   2. tools/list returns 9 tools with valid inputSchema
//   3. tools/call with no daemon running returns isError:true
//   4. Unknown method returns JSON-RPC -32601 error
//   5. ping returns empty result
//   6. Notification produces no response (only ping response returned)
//   7. get_control_flow tool dispatches (isError:true when no daemon)
//   8. get_data_flow tool dispatches (isError:true when no daemon)

#include <nlohmann/json.hpp>

#include <cassert>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// MCP subprocess handle
// ---------------------------------------------------------------------------
struct McpProcess {
    FILE* write_pipe = nullptr;  // stdin of codetldr-mcp
    FILE* read_pipe  = nullptr;  // stdout of codetldr-mcp
    pid_t pid        = -1;
};

// Spawn codetldr-mcp with the given project root.
// Returns an McpProcess with open pipes.
static McpProcess spawn_mcp(const fs::path& binary_path,
                             const fs::path& project_root) {
    // Two pipes: parent->child (stdin) and child->parent (stdout)
    int stdin_pipe[2];   // [0]=read end (child stdin), [1]=write end (parent writes)
    int stdout_pipe[2];  // [0]=read end (parent reads), [1]=write end (child stdout)

    if (::pipe(stdin_pipe) != 0 || ::pipe(stdout_pipe) != 0) {
        throw std::runtime_error("spawn_mcp: pipe() failed");
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("spawn_mcp: fork() failed");
    }

    if (pid == 0) {
        // Child process
        // Redirect stdin from stdin_pipe[0]
        ::dup2(stdin_pipe[0], STDIN_FILENO);
        // Redirect stdout to stdout_pipe[1]
        ::dup2(stdout_pipe[1], STDOUT_FILENO);
        // Redirect stderr to /dev/null to suppress spdlog output in tests
        int dev_null = ::open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            ::dup2(dev_null, STDERR_FILENO);
            ::close(dev_null);
        }
        // Close unused ends
        ::close(stdin_pipe[1]);
        ::close(stdout_pipe[0]);

        std::string bin_str    = binary_path.string();
        std::string root_str   = project_root.string();

        const char* args[] = {
            bin_str.c_str(),
            "--project-root",
            root_str.c_str(),
            nullptr
        };

        ::execv(bin_str.c_str(), const_cast<char* const*>(args));
        // execv failed
        ::_exit(1);
    }

    // Parent process
    ::close(stdin_pipe[0]);   // Close child's end
    ::close(stdout_pipe[1]);  // Close child's end

    FILE* write_f = ::fdopen(stdin_pipe[1], "w");
    FILE* read_f  = ::fdopen(stdout_pipe[0], "r");

    if (!write_f || !read_f) {
        throw std::runtime_error("spawn_mcp: fdopen() failed");
    }

    return McpProcess{write_f, read_f, pid};
}

static void close_mcp(McpProcess& proc) {
    if (proc.write_pipe) {
        ::fclose(proc.write_pipe);
        proc.write_pipe = nullptr;
    }
    if (proc.read_pipe) {
        ::fclose(proc.read_pipe);
        proc.read_pipe = nullptr;
    }
    if (proc.pid > 0) {
        // Give it a moment to exit cleanly after stdin is closed
        int status = 0;
        ::waitpid(proc.pid, &status, 0);
        proc.pid = -1;
    }
}

static void send_request(FILE* write_pipe, const nlohmann::json& msg) {
    std::string line = msg.dump() + "\n";
    ::fputs(line.c_str(), write_pipe);
    ::fflush(write_pipe);
}

static nlohmann::json read_response(FILE* read_pipe) {
    char buf[65536];
    if (::fgets(buf, sizeof(buf), read_pipe) == nullptr) {
        throw std::runtime_error("read_response: EOF or read error");
    }
    return nlohmann::json::parse(std::string(buf));
}

// ---------------------------------------------------------------------------
// Find the codetldr-mcp binary relative to argv[0]
// ---------------------------------------------------------------------------
static fs::path find_binary(const char* argv0) {
    fs::path self(argv0);
    fs::path dir = self.parent_path();
    fs::path candidate = dir / "codetldr-mcp";
    if (fs::exists(candidate)) return candidate;
    // Fallback: look in cwd
    candidate = fs::current_path() / "codetldr-mcp";
    if (fs::exists(candidate)) return candidate;
    throw std::runtime_error("Cannot find codetldr-mcp binary near " + std::string(argv0));
}

// ---------------------------------------------------------------------------
// Test 1: initialize
// ---------------------------------------------------------------------------
static void test_1_initialize(const fs::path& binary, const fs::path& project_root) {
    std::cout << "Test 1: initialize returns correct protocolVersion and serverInfo..." << std::flush;

    auto proc = spawn_mcp(binary, project_root);

    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"id",      1},
        {"method",  "initialize"}
    });

    auto resp = read_response(proc.read_pipe);

    assert(resp.value("id", -1) == 1 && "Test 1: id should be 1");
    assert(resp.contains("result") && "Test 1: response should have result");
    assert(resp["result"].value("protocolVersion", "") == "2025-11-25" &&
           "Test 1: protocolVersion should be 2025-11-25");
    assert(resp["result"].contains("serverInfo") && "Test 1: result should have serverInfo");
    assert(resp["result"]["serverInfo"].value("name", "") == "codetldr" &&
           "Test 1: serverInfo.name should be codetldr");
    assert(resp["result"].contains("capabilities") && "Test 1: result should have capabilities");
    assert(resp["result"]["capabilities"].contains("tools") &&
           "Test 1: capabilities should have tools");

    close_mcp(proc);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 2: tools/list returns 8 tools with valid inputSchema
// ---------------------------------------------------------------------------
static void test_2_tools_list(const fs::path& binary, const fs::path& project_root) {
    std::cout << "Test 2: tools/list returns 9 tools with valid inputSchema..." << std::flush;

    auto proc = spawn_mcp(binary, project_root);

    // Send initialize first
    send_request(proc.write_pipe, {{"jsonrpc","2.0"},{"id",1},{"method","initialize"}});
    read_response(proc.read_pipe);  // consume initialize response

    // Send tools/list
    send_request(proc.write_pipe, {{"jsonrpc","2.0"},{"id",2},{"method","tools/list"}});
    auto resp = read_response(proc.read_pipe);

    assert(resp.value("id", -1) == 2 && "Test 2: id should be 2");
    assert(resp.contains("result") && "Test 2: response should have result");
    assert(resp["result"].contains("tools") && "Test 2: result should have tools");

    auto& tools = resp["result"]["tools"];
    assert(tools.is_array() && "Test 2: tools should be array");
    assert(tools.size() == 9 && "Test 2: should have exactly 9 tools");

    // Verify all expected tool names are present
    std::vector<std::string> expected_names = {
        "search_symbols", "search_text", "get_file_summary",
        "get_function_detail", "get_call_graph", "get_project_overview",
        "get_control_flow", "get_data_flow", "get_embedding_stats"
    };
    for (const auto& name : expected_names) {
        bool found = false;
        for (const auto& tool : tools) {
            if (tool.value("name", "") == name) {
                found = true;
                break;
            }
        }
        assert(found && ("Test 2: missing tool: " + name).c_str());
    }

    // Verify each tool has inputSchema with type == "object"
    for (const auto& tool : tools) {
        assert(tool.contains("inputSchema") &&
               ("Test 2: tool missing inputSchema: " + tool.value("name","?")).c_str());
        assert(tool["inputSchema"].value("type", "") == "object" &&
               ("Test 2: inputSchema.type != object for: " + tool.value("name","?")).c_str());
    }

    close_mcp(proc);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 3: tools/call with no daemon running returns isError:true
// ---------------------------------------------------------------------------
static void test_3_tools_call_daemon_not_running(const fs::path& binary,
                                                  const fs::path& project_root) {
    std::cout << "Test 3: tools/call with no daemon returns isError:true..." << std::flush;

    // Use a temp project root where no daemon socket exists
    fs::path temp_root = fs::temp_directory_path() / "codetldr_mcp_test_t3";
    fs::create_directories(temp_root / ".codetldr");

    auto proc = spawn_mcp(binary, temp_root);

    // Send initialize
    send_request(proc.write_pipe, {{"jsonrpc","2.0"},{"id",1},{"method","initialize"}});
    read_response(proc.read_pipe);  // consume

    // Send tools/call — daemon is not running, socket does not exist
    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"id",      3},
        {"method",  "tools/call"},
        {"params",  {
            {"name",      "search_symbols"},
            {"arguments", {{"query", "test"}}}
        }}
    });

    auto resp = read_response(proc.read_pipe);

    assert(resp.value("id", -1) == 3 && "Test 3: id should be 3");
    assert(resp.contains("result") && "Test 3: response should have result");
    assert(resp["result"].value("isError", false) == true &&
           "Test 3: isError should be true when daemon not running");
    assert(resp["result"].contains("content") && "Test 3: result should have content");
    assert(!resp["result"]["content"].empty() && "Test 3: content should not be empty");
    std::string text = resp["result"]["content"][0].value("text", "");
    assert(text.find("Daemon") != std::string::npos || text.find("codetldr") != std::string::npos
           && "Test 3: error message should mention Daemon or codetldr");

    close_mcp(proc);
    fs::remove_all(temp_root);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 4: Unknown method returns JSON-RPC -32601 error
// ---------------------------------------------------------------------------
static void test_4_unknown_method(const fs::path& binary, const fs::path& project_root) {
    std::cout << "Test 4: unknown method returns -32601 error..." << std::flush;

    auto proc = spawn_mcp(binary, project_root);

    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"id",      4},
        {"method",  "nonexistent"}
    });

    auto resp = read_response(proc.read_pipe);

    assert(resp.value("id", -1) == 4 && "Test 4: id should be 4");
    assert(resp.contains("error") && "Test 4: response should have error");
    assert(resp["error"].value("code", 0) == -32601 &&
           "Test 4: error.code should be -32601");
    assert(!resp["error"].value("message", "").empty() &&
           "Test 4: error.message should not be empty");

    close_mcp(proc);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 5: ping returns empty result
// ---------------------------------------------------------------------------
static void test_5_ping(const fs::path& binary, const fs::path& project_root) {
    std::cout << "Test 5: ping returns empty result {}..." << std::flush;

    auto proc = spawn_mcp(binary, project_root);

    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"id",      5},
        {"method",  "ping"}
    });

    auto resp = read_response(proc.read_pipe);

    assert(resp.value("id", -1) == 5 && "Test 5: id should be 5");
    assert(resp.contains("result") && "Test 5: response should have result");
    assert(resp["result"].is_object() && "Test 5: result should be an object");
    assert(resp["result"].empty() && "Test 5: result should be empty {}");

    close_mcp(proc);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 6: Notification produces no response (only ping response returned)
// ---------------------------------------------------------------------------
static void test_6_notification_no_response(const fs::path& binary,
                                              const fs::path& project_root) {
    std::cout << "Test 6: notification produces no response..." << std::flush;

    auto proc = spawn_mcp(binary, project_root);

    // Send notification (no id) — MCP server must NOT respond
    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"method",  "notifications/initialized"}
    });

    // Immediately send a ping with id — we should only get ONE response (the ping)
    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"id",      6},
        {"method",  "ping"}
    });

    // Read the one response we expect — should be the ping
    auto resp = read_response(proc.read_pipe);

    // Must be the ping response (id=6), not a spurious notification response
    assert(resp.value("id", -1) == 6 && "Test 6: only ping response should arrive (id=6)");
    assert(resp.contains("result") && "Test 6: ping response should have result");
    assert(resp["result"].empty() && "Test 6: ping result should be {}");

    // Verify no second response arrives within 200ms
    // Close write pipe to signal EOF, then read — should get EOF immediately (no second response)
    ::fclose(proc.write_pipe);
    proc.write_pipe = nullptr;

    // Try to read with a timeout using select
    int fd = ::fileno(proc.read_pipe);
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval tv{0, 200000};  // 200ms

    int ready = ::select(fd + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready > 0) {
        // Something is available — check if it's EOF (from binary exiting) or unexpected data
        char buf[4096];
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            // Got unexpected data — fail
            std::string unexpected(buf, n);
            assert(false && "Test 6: received unexpected second response after notification");
        }
        // n == 0 means EOF (binary exited cleanly after stdin closed) — that's fine
    }
    // ready == 0 means timeout (no data) — also fine

    close_mcp(proc);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 7: get_control_flow tool dispatches (isError:true when no daemon)
// ---------------------------------------------------------------------------
static void test_7_get_control_flow(const fs::path& binary, const fs::path& project_root) {
    std::cout << "Test 7: get_control_flow dispatches correctly (isError:true when no daemon)..." << std::flush;

    fs::path temp_root = fs::temp_directory_path() / "codetldr_mcp_test_t7";
    fs::create_directories(temp_root / ".codetldr");

    auto proc = spawn_mcp(binary, temp_root);

    // Send initialize
    send_request(proc.write_pipe, {{"jsonrpc","2.0"},{"id",1},{"method","initialize"}});
    read_response(proc.read_pipe);  // consume

    // Send tools/call for get_control_flow — daemon is not running
    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"id",      7},
        {"method",  "tools/call"},
        {"params",  {
            {"name",      "get_control_flow"},
            {"arguments", {{"name", "main"}}}
        }}
    });

    auto resp = read_response(proc.read_pipe);

    assert(resp.value("id", -1) == 7 && "Test 7: id should be 7");
    assert(resp.contains("result") && "Test 7: response should have result");
    assert(resp["result"].value("isError", false) == true &&
           "Test 7: isError should be true when daemon not running");

    close_mcp(proc);
    fs::remove_all(temp_root);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// Test 8: get_data_flow tool dispatches (isError:true when no daemon)
// ---------------------------------------------------------------------------
static void test_8_get_data_flow(const fs::path& binary, const fs::path& project_root) {
    std::cout << "Test 8: get_data_flow dispatches correctly (isError:true when no daemon)..." << std::flush;

    fs::path temp_root = fs::temp_directory_path() / "codetldr_mcp_test_t8";
    fs::create_directories(temp_root / ".codetldr");

    auto proc = spawn_mcp(binary, temp_root);

    // Send initialize
    send_request(proc.write_pipe, {{"jsonrpc","2.0"},{"id",1},{"method","initialize"}});
    read_response(proc.read_pipe);  // consume

    // Send tools/call for get_data_flow — daemon is not running
    send_request(proc.write_pipe, {
        {"jsonrpc", "2.0"},
        {"id",      8},
        {"method",  "tools/call"},
        {"params",  {
            {"name",      "get_data_flow"},
            {"arguments", {{"name", "main"}}}
        }}
    });

    auto resp = read_response(proc.read_pipe);

    assert(resp.value("id", -1) == 8 && "Test 8: id should be 8");
    assert(resp.contains("result") && "Test 8: response should have result");
    assert(resp["result"].value("isError", false) == true &&
           "Test 8: isError should be true when daemon not running");

    close_mcp(proc);
    fs::remove_all(temp_root);
    std::cout << " PASS\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    // Ignore SIGPIPE: occurs if child process exits while parent is writing
    ::signal(SIGPIPE, SIG_IGN);

    std::cout << "=== MCP Provider Protocol Tests ===\n";

    // Find the codetldr-mcp binary
    fs::path binary;
    try {
        binary = find_binary(argv[0]);
    } catch (const std::exception& ex) {
        std::cerr << "FATAL: " << ex.what() << "\n";
        return 1;
    }
    std::cout << "Using binary: " << binary << "\n";

    // Use a temp project root for tests (no actual daemon)
    fs::path project_root = fs::temp_directory_path() / "codetldr_mcp_tests";
    fs::create_directories(project_root / ".codetldr");

    try {
        test_1_initialize(binary, project_root);
        test_2_tools_list(binary, project_root);
        test_3_tools_call_daemon_not_running(binary, project_root);
        test_4_unknown_method(binary, project_root);
        test_5_ping(binary, project_root);
        test_6_notification_no_response(binary, project_root);
        test_7_get_control_flow(binary, project_root);
        test_8_get_data_flow(binary, project_root);

        fs::remove_all(project_root);
        std::cout << "\nAll 8 tests passed.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\nFATAL: " << ex.what() << "\n";
        fs::remove_all(project_root);
        return 1;
    } catch (...) {
        std::cerr << "\nFATAL: unknown exception\n";
        fs::remove_all(project_root);
        return 1;
    }
}
