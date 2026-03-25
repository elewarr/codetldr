#include "daemon/daemon_client.h"
#include "daemon/daemonize.h"
#include "daemon/status.h"
#include "config/project_dir.h"

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
                    print_status(response["result"]);
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
                print_status(j, /*offline=*/true);
                return;
            } catch (...) {
                // Fall through
            }
        }

        // No info available
        std::cout << "Daemon not running\n";
    });

    CLI11_PARSE(app, argc, argv);
    return 0;
}
