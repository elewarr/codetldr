#include "daemon/coordinator.h"
#include "daemon/daemonize.h"
#include "common/logging.h"
#include "config/project_dir.h"
#include "query/hybrid_search_engine.h"
#include "storage/database.h"
#include "analysis/tree_sitter/language_registry.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <toml++/toml.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    CLI::App app{"codetldr-daemon — background analysis daemon for CodeTLDR"};

    std::string project_root_str;
    bool foreground = false;
    int idle_timeout = 1800;  // 30 minutes default

    app.add_option("--project-root", project_root_str,
                   "Path to the project root directory (must contain .codetldr/)")
       ->required();

    app.add_flag("--foreground,--fg", foreground,
                 "Run in foreground (skip daemonize, useful for debugging)");

    app.add_option("--idle-timeout", idle_timeout,
                   "Seconds of inactivity before auto-shutdown (default: 1800)")
       ->default_val(1800);

    CLI11_PARSE(app, argc, argv);

    try {
        fs::path project_root = fs::absolute(project_root_str);
        fs::path codetldr_dir = project_root / ".codetldr";
        fs::path db_path      = codetldr_dir / "index.sqlite";
        fs::path sock_path    = codetldr_dir / "daemon.sock";
        fs::path pidfile_path = codetldr_dir / "daemon.pid";
        fs::path log_path     = codetldr_dir / "daemon.log";

        // Validate project root has .codetldr/
        if (!fs::exists(codetldr_dir)) {
            std::cerr << "Error: .codetldr/ not found in " << project_root.string()
                      << ". Run 'codetldr init' first.\n";
            return 1;
        }

        // Parse config.toml for optional tuning (non-fatal if missing)
        codetldr::HybridSearchConfig hybrid_cfg;
        fs::path config_path = codetldr_dir / "config.toml";
        if (fs::exists(config_path)) {
            try {
                auto config = toml::parse_file(config_path.string());
                if (auto search = config["search"].as_table()) {
                    if (auto k = (*search)["hybrid_k"].value<int>())             hybrid_cfg.rrf_k              = *k;
                    if (auto m = (*search)["candidate_multiplier"].value<int>()) hybrid_cfg.candidate_multiplier = *m;
                    if (auto b = (*search)["hybrid_bm25_limit"].value<int>())    hybrid_cfg.bm25_limit         = *b;
                    if (auto v = (*search)["hybrid_vec_limit"].value<int>())     hybrid_cfg.vec_limit          = *v;
                    if (auto r = (*search)["hybrid_return_limit"].value<int>())  hybrid_cfg.return_limit       = *r;
                }
            } catch (const std::exception& ex) {
                std::cerr << "Warning: failed to parse config.toml: " << ex.what() << "\n";
            }
        }

        // Open database before daemonizing (validates it opens cleanly)
        auto db = codetldr::Database::open(db_path);

        // Initialize language registry
        codetldr::LanguageRegistry registry;
        if (!registry.initialize()) {
            std::cerr << "Warning: some language grammars failed ABI check\n";
        }

        if (!foreground) {
            // Daemonize: double-fork, redirect stdio, write PID file
            // After this point, the parent process has exited.
            codetldr::daemonize(pidfile_path);

            // We are now the daemon child process.
            // Set up file logging AFTER fork (spdlog file sink created here).
            auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                log_path.string(), false);
            auto logger = std::make_shared<spdlog::logger>("daemon", file_sink);
            spdlog::set_default_logger(logger);
            spdlog::set_level(spdlog::level::info);
        } else {
            // Foreground mode: log to stdout
            auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
            auto logger = std::make_shared<spdlog::logger>("daemon", console_sink);
            spdlog::set_default_logger(logger);
            spdlog::set_level(spdlog::level::debug);
        }

        spdlog::info("codetldr-daemon starting for project: {}", project_root.string());
        spdlog::info("Socket: {}", sock_path.string());
        spdlog::info("PID: {}", static_cast<int>(::getpid()));

        // Create and run coordinator with parsed hybrid search config
        codetldr::Coordinator coordinator(project_root, db.raw(), registry, sock_path,
                                          std::chrono::seconds(idle_timeout), hybrid_cfg);
        coordinator.run();

        spdlog::info("codetldr-daemon stopped cleanly");
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "codetldr-daemon fatal error: " << ex.what() << "\n";
        return 1;
    }
}
