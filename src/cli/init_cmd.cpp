#include "cli/init_cmd.h"
#include "config/project_dir.h"
#include "analysis/tree_sitter/language_registry.h"
#include "watcher/ignore_filter.h"
#include "daemon/daemon_client.h"
#include "daemon/daemonize.h"
#include <nlohmann/json.hpp>

#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

// Return ISO8601 UTC timestamp string
static std::string iso8601_now() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &t);
#else
    gmtime_r(&t, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// Start the daemon for the given project root.
// Returns the PID if started, or existing PID if already running, or -1 on error.
static pid_t start_daemon_for_project(const fs::path& project_root) {
    fs::path codetldr_dir = project_root / ".codetldr";
    fs::path pid_path     = codetldr_dir / "daemon.pid";
    fs::path sock_path    = codetldr_dir / "daemon.sock";

    // Check if already running
    auto existing_pid = codetldr::read_pidfile(pid_path);
    if (existing_pid && codetldr::is_process_alive(*existing_pid)) {
        std::cout << "Daemon already running (PID " << *existing_pid << ")\n";
        return *existing_pid;
    }

    // Fork + exec codetldr-daemon
    pid_t child_pid = ::fork();
    if (child_pid < 0) {
        std::cerr << "Failed to fork: " << ::strerror(errno) << "\n";
        return -1;
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
        std::cerr << "Failed to exec codetldr-daemon: " << ::strerror(errno) << "\n";
        std::exit(127);
    }

    // Parent: wait up to 3s for socket to appear
    bool socket_appeared = false;
    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (fs::exists(sock_path)) {
            socket_appeared = true;
            break;
        }
    }

    if (!socket_appeared) {
        int status = 0;
        ::waitpid(child_pid, &status, WNOHANG);
        std::cerr << "Failed to start daemon (socket did not appear within 3s)\n";
        return -1;
    }

    // Connect and get real PID
    codetldr::DaemonClient client;
    if (client.connect(sock_path)) {
        try {
            auto response = client.call("health_check");
            if (response.contains("result")) {
                int daemon_pid = response["result"].value("pid", 0);
                std::cout << "Daemon started (PID " << daemon_pid
                          << "). Initial indexing in progress.\n";
                return static_cast<pid_t>(daemon_pid);
            }
        } catch (const std::exception&) {
            // Fall through to use fork pid
        }
    }

    std::cout << "Daemon started (PID " << child_pid
              << "). Initial indexing in progress.\n";
    return child_pid;
}

// Build the codetldr section for .codetldr/CLAUDE.md
static std::string build_codetldr_section(const std::map<std::string, int>& lang_counts) {
    std::ostringstream ss;
    ss << "<!-- codetldr:start -->\n";
    ss << "## CodeTLDR\n\n";
    if (!lang_counts.empty()) {
        ss << "Detected languages: ";
        bool first = true;
        for (const auto& [lang, count] : lang_counts) {
            if (!first) ss << ", ";
            std::string display = lang;
            if (!display.empty()) display[0] = static_cast<char>(std::toupper(display[0]));
            ss << display;
            first = false;
        }
        ss << "\n\n";
    }
    ss << "### MCP Tools\n";
    ss << "- **search_symbols** — find functions/classes/methods by name\n";
    ss << "- **search_text** — keyword search over all indexed source\n";
    ss << "- **get_file_summary** — structured file overview at <10% token cost\n";
    ss << "- **get_function_detail** — signature, docs, callers, callees for a function\n";
    ss << "- **get_call_graph** — forward/backward call relationships from AST\n";
    ss << "- **get_project_overview** — language breakdown and indexing status; use first\n\n";
    ss << "Start daemon: `codetldr start`\n";
    ss << "<!-- codetldr:end -->\n";
    return ss.str();
}

// Write (or idempotently update) .codetldr/CLAUDE.md
static bool write_claude_md(const fs::path& project_root, const std::map<std::string, int>& lang_counts) {
    const std::string START = "<!-- codetldr:start -->";
    const std::string END   = "<!-- codetldr:end -->";
    fs::path path = project_root / ".codetldr" / "CLAUDE.md";

    std::string existing;
    bool existed = fs::exists(path);
    if (existed) {
        std::ifstream in(path);
        existing.assign(std::istreambuf_iterator<char>(in), {});
    }

    std::string section = build_codetldr_section(lang_counts);
    std::string output;

    auto s = existing.find(START);
    auto e = existing.find(END);
    if (s != std::string::npos && e != std::string::npos && s < e) {
        output = existing.substr(0, s) + section + existing.substr(e + END.size());
    } else {
        output = section;
    }

    std::ofstream out(path);
    if (!out) return false;
    out << output;

    if (existed) {
        std::cout << "Updated .codetldr/CLAUDE.md\n";
    } else {
        std::cout << "Created .codetldr/CLAUDE.md\n";
    }
    return true;
}


void register_init_cmd(CLI::App& app, std::string& project_root_str) {
    auto* init_cmd = app.add_subcommand("init", "Initialize codetldr for the current project");
    init_cmd->add_option("--project-root", project_root_str,
                         "Path to the project root (default: auto-detect git root or cwd)");

    init_cmd->callback([&]() {
        // Resolve project root
        fs::path project_root;
        if (!project_root_str.empty()) {
            project_root = fs::absolute(project_root_str);
        } else {
            auto git_root = codetldr::find_git_root(fs::current_path());
            if (git_root) {
                project_root = *git_root;
            } else {
                project_root = fs::current_path();
            }
        }

        // -------------------------------------------------------
        // Step 1: Create .codetldr/ directory
        // -------------------------------------------------------
        bool codetldr_dir_existed = std::filesystem::exists(project_root / ".codetldr");
        auto init_result = codetldr::init_project_dir(project_root);
        if (!codetldr_dir_existed && init_result.codetldr_created) {
            std::cout << "Created .codetldr/\n";
        } else {
            std::cout << "Already initialized (.codetldr/ exists)\n";
        }
        if (!init_result.note.empty()) {
            std::cout << "Note: " << init_result.note << "\n";
        }

        // -------------------------------------------------------
        // Step 2: Write default .codetldrignore if not present
        // -------------------------------------------------------
        fs::path ignore_path = project_root / ".codetldrignore";
        if (!fs::exists(ignore_path)) {
            std::ofstream out(ignore_path);
            if (out) {
                out << "# codetldr ignore patterns (gitignore syntax)\n";
                out << "build/\n";
                out << ".build/\n";
                out << "node_modules/\n";
                out << ".git/\n";
                out << "vendor/\n";
                out << ".codetldr/\n";
                out << "dist/\n";
                out << "out/\n";
                out << "target/\n";
                out << "*.o\n";
                out << "*.a\n";
                out << "*.so\n";
                out << "*.dylib\n";
                out.close();
                std::cout << "Created .codetldrignore with default patterns\n";
            } else {
                std::cerr << "Warning: failed to write .codetldrignore\n";
            }
        } else {
            std::cout << ".codetldrignore already exists, skipping\n";
        }

        // -------------------------------------------------------
        // Step 3: Scan project and detect languages
        // -------------------------------------------------------
        codetldr::LanguageRegistry registry;
        if (!registry.initialize()) {
            std::cerr << "Warning: language registry initialization failed\n";
        }

        codetldr::IgnoreFilter ignore_filter =
            codetldr::IgnoreFilter::from_project_root(project_root);

        std::map<std::string, int> lang_counts;
        int total_source_files = 0;

        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(project_root, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (!it->is_regular_file(ec)) {
                ec.clear();
                continue;
            }

            fs::path abs_path = it->path();
            fs::path rel_path = fs::relative(abs_path, project_root, ec);
            if (ec) {
                ec.clear();
                continue;
            }

            if (ignore_filter.should_ignore(rel_path)) {
                continue;
            }

            std::string ext = abs_path.extension().string();
            if (!ext.empty()) {
                const codetldr::LanguageEntry* entry = registry.for_extension(ext);
                if (entry) {
                    lang_counts[entry->name]++;
                    total_source_files++;
                }
            }
        }

        // Print detected languages
        if (lang_counts.empty()) {
            std::cout << "Detected languages: (none recognized)\n";
        } else {
            std::cout << "Detected languages: ";
            bool first = true;
            for (const auto& [lang, count] : lang_counts) {
                if (!first) std::cout << ", ";
                // Capitalize first letter for display
                std::string display = lang;
                if (!display.empty()) display[0] = static_cast<char>(std::toupper(display[0]));
                std::cout << display << " (" << count << " files)";
                first = false;
            }
            std::cout << "\n";
        }

        // -------------------------------------------------------
        // Step 4: Write .codetldr/config.json
        // -------------------------------------------------------
        nlohmann::json config;
        config["project_root"] = project_root.string();
        config["detected_languages"] = lang_counts;
        config["total_files"] = total_source_files;
        config["initialized_at"] = iso8601_now();

        fs::path config_path = project_root / ".codetldr" / "config.json";
        std::ofstream cfg_out(config_path);
        if (cfg_out) {
            cfg_out << config.dump(2) << "\n";
            cfg_out.close();
        } else {
            std::cerr << "Warning: failed to write .codetldr/config.json\n";
        }

        // -------------------------------------------------------
        // Step 5: Write .codetldr/CLAUDE.md
        // -------------------------------------------------------
        if (!write_claude_md(project_root, lang_counts)) {
            std::cerr << "Warning: failed to write .codetldr/CLAUDE.md\n";
        }

        // -------------------------------------------------------
        // Step 6: Migrate from CAPABILITIES.md
        // -------------------------------------------------------
        fs::path caps_path = project_root / ".codetldr" / "CAPABILITIES.md";
        if (fs::exists(caps_path)) {
            std::error_code ec;
            fs::remove(caps_path, ec);
            if (!ec) {
                std::cout << "Removed old .codetldr/CAPABILITIES.md (replaced by CLAUDE.md)\n";
            } else {
                std::cerr << "Warning: failed to remove old CAPABILITIES.md: " << ec.message() << "\n";
            }
        }

        // -------------------------------------------------------
        // Step 7: Auto-start daemon
        // -------------------------------------------------------
        start_daemon_for_project(project_root);

        // -------------------------------------------------------
        // Step 8: Print import instruction
        // -------------------------------------------------------
        const char* BOLD   = "\033[1m";
        const char* GREEN  = "\033[32m";
        const char* RESET  = "\033[0m";
        const char* YELLOW = "\033[33m";

        std::cout << "\n"
                  << GREEN << "+" << std::string(58, '-') << "+" << RESET << "\n"
                  << GREEN << "|" << RESET << BOLD
                  << "  Add to your CLAUDE.md (as a comment):                 "
                  << RESET << GREEN << "|" << RESET << "\n"
                  << GREEN << "|" << RESET << YELLOW << BOLD
                  << "  <!-- @.codetldr/CLAUDE.md -->                         "
                  << RESET << GREEN << "|" << RESET << "\n"
                  << GREEN << "+" << std::string(58, '-') << "+" << RESET << "\n\n";
    });
}
