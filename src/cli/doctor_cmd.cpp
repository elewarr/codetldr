#include "cli/doctor_cmd.h"
#include "daemon/daemon_client.h"
#include "daemon/status.h"
#include "config/project_dir.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

// Check if a file exists and is executable using access(2)
static bool is_executable(const fs::path& p) {
    return fs::exists(p) && ::access(p.c_str(), X_OK) == 0;
}

// Find a binary in PATH by checking common installation directories
static std::string find_in_path(const std::string& binary_name) {
    // Check common macOS/Linux installation paths first
    static const std::vector<std::string> common_dirs = {
        "/opt/homebrew/bin",
        "/usr/local/bin",
        "/usr/bin",
        "/bin"
    };
    for (const auto& dir : common_dirs) {
        fs::path candidate = fs::path(dir) / binary_name;
        if (is_executable(candidate)) {
            return candidate.string();
        }
    }

    // Check PATH environment variable
    const char* path_env = ::getenv("PATH");
    if (path_env) {
        std::string path_str(path_env);
        std::string::size_type start = 0;
        std::string::size_type end;
        while ((end = path_str.find(':', start)) != std::string::npos) {
            std::string dir = path_str.substr(start, end - start);
            if (!dir.empty()) {
                fs::path candidate = fs::path(dir) / binary_name;
                if (is_executable(candidate)) {
                    return candidate.string();
                }
            }
            start = end + 1;
        }
        // Last segment
        if (start < path_str.size()) {
            std::string dir = path_str.substr(start);
            fs::path candidate = fs::path(dir) / binary_name;
            if (is_executable(candidate)) {
                return candidate.string();
            }
        }
    }
    return {};
}

// Find the MCP binary alongside the current executable
static std::string find_mcp_binary_beside_self() {
    // Try /proc/self/exe (Linux)
#if defined(__linux__)
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (!ec && !self.empty()) {
        fs::path candidate = self.parent_path() / "codetldr-mcp";
        if (is_executable(candidate)) {
            return candidate.string();
        }
    }
#endif
    return {};
}

void register_doctor_cmd(CLI::App& app, std::string& project_root_str) {
    auto* doctor = app.add_subcommand("doctor", "Verify CodeTLDR installation");
    doctor->add_option("--project-root", project_root_str,
                       "Path to the project root (default: auto-detect git root or cwd)");

    doctor->callback([&]() {
        // Resolve project root
        fs::path project_root;
        if (!project_root_str.empty()) {
            project_root = fs::absolute(project_root_str);
        } else {
            auto git_root = codetldr::find_git_root(fs::current_path());
            project_root = git_root ? *git_root : fs::current_path();
        }

        int pass_count = 0;
        int fail_count = 0;
        int skip_count = 0;

        auto report = [&](bool ok, const std::string& msg) {
            if (ok) {
                std::cout << "[PASS] " << msg << "\n";
                pass_count++;
            } else {
                std::cout << "[FAIL] " << msg << "\n";
                fail_count++;
            }
        };

        auto report_skip = [&](const std::string& msg) {
            std::cout << "[SKIP] " << msg << "\n";
            skip_count++;
        };

        // ----------------------------------------------------------------
        // Check 1: Binary found and version reported
        // ----------------------------------------------------------------
#ifndef CODETLDR_VERSION
#define CODETLDR_VERSION "dev"
#endif
        report(true, std::string("Binary found: codetldr (v") + CODETLDR_VERSION + ")");

        // ----------------------------------------------------------------
        // Check 2: Daemon reachable
        // ----------------------------------------------------------------
        fs::path codetldr_dir = project_root / ".codetldr";
        fs::path sock_path    = codetldr_dir / "daemon.sock";

        bool daemon_ok = false;
        {
            codetldr::DaemonClient client;
            if (client.connect(sock_path)) {
                try {
                    auto response = client.call("health_check");
                    if (response.contains("result") && response["result"].contains("pid")) {
                        int pid = response["result"]["pid"].get<int>();
                        report(true, "Daemon reachable (PID " + std::to_string(pid) + ")");
                        daemon_ok = true;
                    } else {
                        report(true, "Daemon reachable");
                        daemon_ok = true;
                    }
                } catch (...) {
                    report(false, "Daemon not running -- run: codetldr start");
                }
            } else {
                report(false, "Daemon not running -- run: codetldr start");
            }
        }
        (void)daemon_ok; // Reserved for future checks that depend on daemon state

        // ----------------------------------------------------------------
        // Check 3: MCP server binary present
        // ----------------------------------------------------------------
        {
            std::string mcp_path = find_in_path("codetldr-mcp");
            if (mcp_path.empty()) {
                mcp_path = find_mcp_binary_beside_self();
            }
            if (!mcp_path.empty()) {
                report(true, "MCP server binary found: " + mcp_path);
            } else {
                report(false, "MCP server binary not found (codetldr-mcp)");
            }
        }

        // ----------------------------------------------------------------
        // Check 4: Hook scripts present and executable
        // ----------------------------------------------------------------
        {
            // Resolve plugin root: try env var, then common locations
            fs::path plugin_root;
            const char* plugin_root_env = ::getenv("CLAUDE_PLUGIN_ROOT");
            if (plugin_root_env && plugin_root_env[0] != '\0') {
                plugin_root = fs::path(plugin_root_env);
            } else {
                // Try development layout: cwd/codetldr-plugin
                fs::path dev_candidate = fs::current_path() / "codetldr-plugin";
                if (fs::exists(dev_candidate / "hooks")) {
                    plugin_root = dev_candidate;
                }
            }

            if (plugin_root.empty()) {
                report_skip("Hook scripts (plugin root not found -- set CLAUDE_PLUGIN_ROOT or run from project dir)");
            } else {
                static const std::vector<std::string> hook_files = {
                    "hooks/session-start.sh",
                    "hooks/pre-tool-use.sh",
                    "hooks/stop.sh"
                };
                int found = 0;
                std::vector<std::string> missing;
                for (const auto& hook : hook_files) {
                    fs::path hook_path = plugin_root / hook;
                    if (is_executable(hook_path)) {
                        found++;
                    } else {
                        missing.push_back(hook);
                    }
                }
                if (found == static_cast<int>(hook_files.size())) {
                    report(true, "Hook scripts present and executable (" +
                           std::to_string(found) + "/" +
                           std::to_string(hook_files.size()) + ")");
                } else {
                    std::string detail;
                    for (size_t i = 0; i < missing.size(); ++i) {
                        if (i > 0) detail += ", ";
                        detail += missing[i];
                    }
                    report(false, "Hook scripts: " + std::to_string(found) + "/" +
                           std::to_string(hook_files.size()) + " found (missing: " + detail + ")");
                }
            }
        }

        // ----------------------------------------------------------------
        // Summary
        // ----------------------------------------------------------------
        std::cout << "\n";
        if (fail_count == 0) {
            std::cout << "All checks passed.\n";
        } else {
            std::cout << fail_count << " check(s) failed.\n";
        }

        if (fail_count > 0) {
            std::exit(1);
        }
    });
}
