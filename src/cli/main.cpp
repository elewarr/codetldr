#include "daemon/daemon_client.h"
#include "daemon/daemonize.h"
#include "daemon/status.h"
#include "config/project_dir.h"
#include "cli/search_cmd.h"
#include "cli/init_cmd.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// ANSI color helpers
static const char* GREEN  = "\033[32m";
static const char* YELLOW = "\033[33m";
static const char* RED    = "\033[31m";
static const char* BOLD   = "\033[1m";
static const char* RESET  = "\033[0m";

// Format uptime_seconds into "Xh Ym Zs" or "Xm Zs"
static std::string format_uptime(int seconds) {
    if (seconds < 0) seconds = 0;
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    std::string result;
    if (h > 0) result += std::to_string(h) + "h ";
    if (h > 0 || m > 0) result += std::to_string(m) + "m ";
    result += std::to_string(s) + "s";
    return result;
}

// Print embedding pipeline stats with color-coded health indicators
static void print_stats(const nlohmann::json& result) {
    // Print warnings first (OBS-03, OBS-04)
    std::string health   = result.value("health", "ok");
    std::string degraded_str = result.contains("degraded") && result["degraded"].is_string()
                             ? result["degraded"].get<std::string>() : "";

    if (!degraded_str.empty()) {
        std::cout << RED << BOLD << "[WARNING] " << RESET << RED << degraded_str << RESET << "\n\n";
    }

    // Header
    std::cout << BOLD << "Embedding Pipeline Stats\n" << RESET;
    std::cout << std::string(40, '-') << "\n";

    // Model
    std::string model_status = result.value("model_status", "unknown");
    const char* model_color = (model_status == "loaded") ? GREEN : YELLOW;
    std::cout << "Model:        " << model_color << model_status << RESET << "\n";

    // Health indicator
    const char* health_color = (health == "ok") ? GREEN : RED;
    std::cout << "Health:       " << health_color << health << RESET << "\n";

    // Latency
    size_t sample_count = result.value("sample_count", static_cast<size_t>(0));
    if (sample_count > 0) {
        double p50 = result.value("latency_p50_ms", 0.0);
        double p95 = result.value("latency_p95_ms", 0.0);
        double p99 = result.value("latency_p99_ms", 0.0);
        double thr = result.value("throughput_chunks_per_sec", 0.0);

        // Color latency: green <10ms, yellow 10-20ms, red >20ms
        auto latency_color = [](double ms) -> const char* {
            if (ms < 10.0) return GREEN;
            if (ms < 20.0) return YELLOW;
            return RED;
        };

        std::cout << "Latency p50:  " << latency_color(p50)
                  << p50 << "ms" << RESET << "\n";
        std::cout << "Latency p95:  " << latency_color(p95)
                  << p95 << "ms" << RESET << "\n";
        std::cout << "Latency p99:  " << latency_color(p99)
                  << p99 << "ms" << RESET << "\n";
        std::cout << "Throughput:   " << thr << " chunks/sec\n";
        std::cout << "(n=" << sample_count << " samples)\n";
    } else {
        std::cout << YELLOW << "Latency:      no data yet\n" << RESET;
    }

    // Queue
    uint64_t queue = result.value("queue_depth", static_cast<uint64_t>(0));
    const char* queue_color = (queue == 0) ? GREEN : (queue < 100 ? YELLOW : RED);
    std::cout << "Queue depth:  " << queue_color << queue << RESET << " pending\n";

    // Index sizes
    int64_t faiss  = result.value("faiss_vector_count", static_cast<int64_t>(0));
    int64_t sqlite = result.value("sqlite_embedded_count", static_cast<int64_t>(0));
    std::cout << "FAISS index:  " << faiss  << " vectors\n";
    std::cout << "SQLite index: " << sqlite << " symbols embedded\n";

    // Total
    uint64_t total = result.value("chunks_embedded_total", static_cast<uint64_t>(0));
    std::cout << "Total chunks: " << total << " embedded (all time)\n";
}

// Print daemon status in human-readable format
static void print_status(const nlohmann::json& result, bool offline = false) {
    std::string state = result.value("state", "unknown");
    int pid           = result.value("pid", 0);
    int files_indexed = result.value("files_indexed", 0);
    int files_total   = result.value("files_total", 0);
    int uptime_sec    = result.value("uptime_seconds", 0);
    std::string sock  = result.value("socket_path", "");

    std::cout << "State:     " << state << "\n";
    if (pid > 0) {
        std::cout << "PID:       " << pid << "\n";
    }
    std::cout << "Files:     " << files_indexed << "/" << files_total << " indexed\n";
    if (!offline) {
        std::cout << "Uptime:    " << format_uptime(uptime_sec) << "\n";
    }
    if (!sock.empty()) {
        std::cout << "Socket:    " << sock << "\n";
    }
    if (offline) {
        std::cout << "(daemon not running — last known state)\n";
    }

    // Display LSP server status (Phase 24)
    if (result.contains("lsp_servers") && result["lsp_servers"].is_array()) {
        const auto& servers = result["lsp_servers"];
        if (!servers.empty()) {
            std::cout << "\nLSP Servers:\n";
            for (const auto& srv : servers) {
                std::string lang  = srv.value("language", "?");
                std::string state = srv.value("state", "unknown");
                // Color-code state: green=ready, yellow=starting/indexing/degraded, red=unavailable
                const char* color = RESET;
                if (state == "ready") color = GREEN;
                else if (state == "unavailable") color = RED;
                else if (state == "starting" || state == "indexing" || state == "degraded") color = YELLOW;
                std::cout << "  " << lang << ": " << color << state << RESET << "\n";
            }
        }
    }
}

int main(int argc, char* argv[]) {
    CLI::App app{"codetldr - code analysis daemon"};
    app.require_subcommand(1);

    // Common option: --project-root (global, falls through to subcommands)
    std::string project_root_str;
    app.add_option("--project-root", project_root_str,
                   "Path to the project root (default: auto-detect git root or cwd)");
    app.fallthrough();

    // Resolve project root (used by subcommand callbacks)
    auto resolve_project_root = [&]() -> fs::path {
        if (!project_root_str.empty()) {
            return fs::absolute(project_root_str);
        }
        auto git_root = codetldr::find_git_root(fs::current_path());
        if (git_root) {
            return *git_root;
        }
        return fs::current_path();
    };

    // =========================================================
    // Subcommand: start
    // =========================================================
    auto* start_cmd = app.add_subcommand("start", "Start the codetldr daemon");
    start_cmd->add_option("--project-root", project_root_str,
                          "Path to the project root (default: auto-detect git root or cwd)");

    start_cmd->callback([&]() {
        fs::path project_root = resolve_project_root();
        fs::path codetldr_dir = project_root / ".codetldr";
        fs::path pid_path     = codetldr_dir / "daemon.pid";
        fs::path sock_path    = codetldr_dir / "daemon.sock";

        // Check if daemon is already running
        auto existing_pid = codetldr::read_pidfile(pid_path);
        if (existing_pid && codetldr::is_process_alive(*existing_pid)) {
            std::cout << "Daemon already running (PID " << *existing_pid << ")\n";
            return;
        }

        // Ensure .codetldr/ exists
        codetldr::init_project_dir(project_root);

        // Launch codetldr-daemon as a child process
        pid_t child_pid = ::fork();
        if (child_pid < 0) {
            std::cerr << "Failed to fork: " << ::strerror(errno) << "\n";
            std::exit(1);
        }

        if (child_pid == 0) {
            // Child: exec codetldr-daemon
            const char* daemon_bin = "codetldr-daemon";
            std::string root_str = project_root.string();
            char* args[] = {
                const_cast<char*>(daemon_bin),
                const_cast<char*>("--project-root"),
                const_cast<char*>(root_str.c_str()),
                nullptr
            };
            ::execvp(daemon_bin, args);
            // If exec fails:
            std::cerr << "Failed to exec codetldr-daemon: " << ::strerror(errno) << "\n";
            std::exit(127);
        }

        // Parent: wait up to 3s for socket file to appear (poll every 100ms)
        bool socket_appeared = false;
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (fs::exists(sock_path)) {
                socket_appeared = true;
                break;
            }
        }

        if (!socket_appeared) {
            // Check if child exited with an error
            int status = 0;
            ::waitpid(child_pid, &status, WNOHANG);
            std::cerr << "Failed to start daemon (socket did not appear within 3s)\n";
            std::exit(1);
        }

        // Connect and send health_check
        codetldr::DaemonClient client;
        if (client.connect(sock_path)) {
            try {
                auto response = client.call("health_check");
                if (response.contains("result")) {
                    int daemon_pid = response["result"].value("pid", 0);
                    std::cout << "Daemon started (PID " << daemon_pid << ")\n";
                } else {
                    std::cout << "Daemon started\n";
                }
            } catch (const std::exception& ex) {
                std::cerr << "Warning: health_check failed: " << ex.what() << "\n";
            }
        } else {
            std::cout << "Daemon started (PID " << child_pid << ")\n";
        }
    });

    // =========================================================
    // Subcommand: stop
    // =========================================================
    auto* stop_cmd = app.add_subcommand("stop", "Stop the codetldr daemon");
    stop_cmd->add_option("--project-root", project_root_str,
                         "Path to the project root (default: auto-detect git root or cwd)");

    stop_cmd->callback([&]() {
        fs::path project_root = resolve_project_root();
        fs::path codetldr_dir = project_root / ".codetldr";
        fs::path pid_path     = codetldr_dir / "daemon.pid";
        fs::path sock_path    = codetldr_dir / "daemon.sock";

        // Try to connect via socket first
        codetldr::DaemonClient client;
        if (client.connect(sock_path)) {
            try {
                client.call("stop");
                std::cout << "Daemon stopped\n";
            } catch (const std::exception& ex) {
                std::cerr << "Warning: stop request failed: " << ex.what() << "\n";
            }
            return;
        }

        // Socket not available — try PID file fallback
        auto pid = codetldr::read_pidfile(pid_path);
        if (pid && codetldr::is_process_alive(*pid)) {
            if (::kill(*pid, SIGTERM) == 0) {
                std::cout << "Sent SIGTERM to daemon (PID " << *pid << ")\n";
            } else {
                std::cerr << "Failed to send SIGTERM: " << ::strerror(errno) << "\n";
            }
            return;
        }

        std::cout << "Daemon not running\n";
    });

    // =========================================================
    // Subcommand: status
    // =========================================================
    auto* status_cmd = app.add_subcommand("status", "Show daemon status");
    status_cmd->add_option("--project-root", project_root_str,
                           "Path to the project root (default: auto-detect git root or cwd)");
    bool status_json  = false;
    bool status_stats = false;
    status_cmd->add_flag("--json",  status_json,  "Output as JSON");
    status_cmd->add_flag("--stats", status_stats, "Output embedding pipeline metrics and health indicators");

    status_cmd->callback([&]() {
        fs::path project_root = resolve_project_root();
        fs::path codetldr_dir = project_root / ".codetldr";
        fs::path sock_path    = codetldr_dir / "daemon.sock";
        fs::path status_path  = codetldr_dir / "status.json";

        // Try live connection first
        codetldr::DaemonClient client;
        if (client.connect(sock_path)) {
            try {
                auto response = client.call("get_status");
                if (response.contains("result")) {
                    if (status_stats) {
                        // Fetch embedding stats from daemon
                        auto stats_resp = client.call("get_embedding_stats");
                        print_status(response["result"]);
                        std::cout << "\n";
                        if (stats_resp.contains("result")) {
                            print_stats(stats_resp["result"]);
                        } else {
                            std::cout << "Embedding stats unavailable\n";
                        }
                    } else if (status_json) {
                        std::cout << response["result"].dump(2) << "\n";
                    } else {
                        print_status(response["result"]);
                    }
                } else {
                    std::cout << "Daemon running (status unavailable)\n";
                }
            } catch (const std::exception& ex) {
                std::cerr << "Status query failed: " << ex.what() << "\n";
            }
            return;
        }

        // Offline: try status.json
        if (fs::exists(status_path)) {
            try {
                auto ds = codetldr::StatusWriter::read(status_path);
                nlohmann::json j;
                j["state"]         = codetldr::to_string(ds.state);
                j["pid"]           = ds.pid;
                j["socket_path"]   = ds.socket_path;
                j["files_indexed"] = ds.files_indexed;
                j["files_total"]   = ds.files_total;
                j["uptime_seconds"] = ds.uptime_seconds;
                if (status_json) {
                    j["offline"] = true;
                    std::cout << j.dump(2) << "\n";
                } else {
                    print_status(j, /*offline=*/true);
                }
                return;
            } catch (...) {
                // Fall through
            }
        }

        // No info available
        if (status_json) {
            nlohmann::json j;
            j["state"] = "not_running";
            std::cout << j.dump(2) << "\n";
        } else {
            std::cout << "Daemon not running\n";
        }
    });

    // =========================================================
    // Subcommand: search (registered from search_cmd.cpp)
    // =========================================================
    register_search_cmd(app, project_root_str);

    // =========================================================
    // Subcommand: init (registered from init_cmd.cpp)
    // =========================================================
    register_init_cmd(app, project_root_str);

    CLI11_PARSE(app, argc, argv);
    return 0;
}
