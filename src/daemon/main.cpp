#include "daemon/coordinator.h"
#include "daemon/daemonize.h"
#include "common/logging.h"
#include "config/project_dir.h"
#include "storage/database.h"
#include "analysis/tree_sitter/language_registry.h"
#include "lsp/lsp_manager.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <unistd.h>

#include <filesystem>
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

        // Create coordinator
        codetldr::Coordinator coordinator(project_root, db.raw(), registry, sock_path);

        // Set up LSP manager with known language servers
        codetldr::LspManager lsp_manager;

        // Helper: find a binary on PATH using popen("which ...")
        auto find_binary = [](const std::string& name) -> std::string {
            std::string cmd = "which " + name + " 2>/dev/null";
            FILE* pipe = ::popen(cmd.c_str(), "r");
            if (!pipe) return "";
            char buf[512];
            std::string result;
            while (::fgets(buf, sizeof(buf), pipe)) {
                result += buf;
            }
            ::pclose(pipe);
            // Trim trailing newline/CR
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
                result.pop_back();
            }
            return result;
        };

        // clangd for C/C++
        std::string clangd_path = find_binary("clangd");
        if (!clangd_path.empty()) {
            lsp_manager.register_language("cpp", {clangd_path, {"--background-index"},
                {".cpp", ".cc", ".cxx", ".h", ".hpp", ".c"}});
            spdlog::info("LSP: registered clangd at {}", clangd_path);
        }

        // pyright for Python (try pyright-langserver, then basedpyright-langserver)
        std::string pyright_path = find_binary("pyright-langserver");
        if (pyright_path.empty()) pyright_path = find_binary("basedpyright-langserver");
        if (!pyright_path.empty()) {
            lsp_manager.register_language("python", {pyright_path, {"--stdio"}, {".py"}});
            spdlog::info("LSP: registered pyright at {}", pyright_path);
        }

        // typescript-language-server for TypeScript/JavaScript
        std::string tsserver_path = find_binary("typescript-language-server");
        if (tsserver_path.empty()) {
            auto local = project_root / "node_modules" / ".bin" / "typescript-language-server";
            if (fs::exists(local)) {
                tsserver_path = local.string();
            }
        }
        if (!tsserver_path.empty()) {
            lsp_manager.register_language("typescript", {tsserver_path, {"--stdio"},
                {".ts", ".tsx", ".js", ".jsx"}});
            spdlog::info("LSP: registered typescript-language-server at {}", tsserver_path);
        }

        lsp_manager.set_project_root(project_root);
        coordinator.set_lsp_manager(&lsp_manager);

        coordinator.run();

        spdlog::info("codetldr-daemon stopped cleanly");
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "codetldr-daemon fatal error: " << ex.what() << "\n";
        return 1;
    }
}
