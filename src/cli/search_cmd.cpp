#include "cli/search_cmd.h"
#include "daemon/daemon_client.h"
#include "config/project_dir.h"
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

void register_search_cmd(CLI::App& app, std::string& project_root_str) {
    auto* search_cmd = app.add_subcommand("search", "Search for symbols or text in the index");

    // Positional query argument
    static std::string query_str;
    static std::string kind_str;
    static int limit = 20;
    static bool json_output = false;
    static bool text_mode = false;
    static std::string search_root_str;

    search_cmd->add_option("query", query_str, "Search query")->required();
    search_cmd->add_option("--kind", kind_str,
                           "Filter by symbol kind: function, class, method, struct, enum");
    search_cmd->add_option("--limit", limit, "Maximum results")->default_val(20);
    search_cmd->add_flag("--json", json_output, "Output as JSON");
    search_cmd->add_flag("--text", text_mode, "Full-text search (instead of symbol search)");
    search_cmd->add_option("--project-root", project_root_str,
                           "Path to the project root (default: auto-detect git root or cwd)");

    search_cmd->callback([&]() {
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

        fs::path sock_path = project_root / ".codetldr" / "daemon.sock";

        // Connect to daemon (fresh connection per call — one-request-per-connection design)
        codetldr::DaemonClient client;
        if (!client.connect(sock_path)) {
            std::cerr << "Daemon not running. Run: codetldr start\n";
            std::exit(1);
        }

        // Build params
        nlohmann::json params;
        params["query"] = query_str;
        params["limit"] = limit;
        if (!kind_str.empty()) {
            params["kind"] = kind_str;
        }

        // Call appropriate method
        nlohmann::json resp;
        try {
            if (text_mode) {
                resp = client.call("search_text", params);
            } else {
                resp = client.call("search_symbols", params);
            }
        } catch (const std::exception& ex) {
            std::cerr << "Search failed: " << ex.what() << "\n";
            std::exit(1);
        }

        // Handle error in response
        if (resp.contains("error")) {
            std::string msg = resp["error"].value("message", "Unknown error");
            std::cerr << "Error: " << msg << "\n";
            std::exit(1);
        }

        // Output results
        if (!resp.contains("result")) {
            std::cerr << "Error: unexpected response format\n";
            std::exit(1);
        }

        if (json_output) {
            std::cout << resp["result"].dump(2) << "\n";
        } else {
            // Human-readable: "name (kind) -- file:line"
            const auto& results = resp["result"];
            if (results.is_array()) {
                if (results.empty()) {
                    std::cout << "No results found.\n";
                }
                for (const auto& item : results) {
                    std::string name = item.value("name", "?");
                    std::string kind = item.value("kind", "");
                    std::string file = item.value("file", "");
                    int line = item.value("line", 0);

                    if (!kind.empty()) {
                        std::cout << name << " (" << kind << ")";
                    } else {
                        std::cout << name;
                    }
                    if (!file.empty()) {
                        std::cout << " -- " << file;
                        if (line > 0) {
                            std::cout << ":" << line;
                        }
                    }
                    std::cout << "\n";
                }
            } else {
                // Fallback: dump as JSON if result is not an array
                std::cout << results.dump(2) << "\n";
            }
        }
    });
}
