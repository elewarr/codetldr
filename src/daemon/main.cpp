#include "daemon/coordinator.h"
#include "daemon/daemonize.h"
#include "common/logging.h"
#include "common/sha256.h"
#include "config/paths.h"
#include "config/project_dir.h"
#include "storage/database.h"
#include "analysis/tree_sitter/language_registry.h"
#include "lsp/lsp_manager.h"
#include "lsp/lsp_call_graph_resolver.h"
#include "lsp/lsp_dependency_resolver.h"
#include "lsp/lsp_call_hierarchy_resolver.h"

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

// JDT-03: Detect Java 21+ runtime. Returns java_home path if found, empty string if not.
// Checks JAVA_HOME/bin/java first, then java on PATH.
// IMPORTANT: java -version writes to stderr, not stdout — must use 2>&1 redirect.
static std::string detect_java_21_plus() {
    std::vector<std::string> candidates;
    const char* java_home = ::getenv("JAVA_HOME");
    if (java_home && java_home[0] != '\0') {
        candidates.push_back(std::string(java_home) + "/bin/java");
    }
    candidates.push_back("java");  // PATH fallback

    for (const auto& java_bin : candidates) {
        // java -version writes to stderr — redirect to stdout
        std::string cmd = java_bin + " -version 2>&1";
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) continue;
        char buf[512];
        std::string output;
        while (::fgets(buf, sizeof(buf), pipe)) output += buf;
        int rc = ::pclose(pipe);
        if (rc != 0) continue;

        // Parse version: find quoted string like "21.0.3" or "1.8.0_391"
        auto q1 = output.find('"');
        auto q2 = (q1 != std::string::npos) ? output.find('"', q1 + 1) : std::string::npos;
        if (q1 == std::string::npos || q2 == std::string::npos) continue;
        std::string ver = output.substr(q1 + 1, q2 - q1 - 1);

        // Handle legacy "1.X" format (Java 8 and below)
        int major = 0;
        try {
            if (ver.size() > 2 && ver[0] == '1' && ver[1] == '.') {
                major = std::stoi(ver.substr(2));  // "1.8.0" -> 8
            } else {
                major = std::stoi(ver);  // "21.0.3" -> 21
            }
        } catch (...) {
            continue;  // Unparseable version string
        }

        if (major >= 21) {
            // Return java_home: prefer JAVA_HOME if that's the binary we used
            if (java_home && java_home[0] != '\0' &&
                java_bin.find(java_home) == 0) {
                return std::string(java_home);
            }
            // For PATH java, derive home from which java minus /bin/java
            std::string which_cmd = "which " + java_bin + " 2>/dev/null";
            FILE* wp = ::popen(which_cmd.c_str(), "r");
            if (!wp) return "";
            char wbuf[512] = {};
            ::fgets(wbuf, sizeof(wbuf), wp);
            ::pclose(wp);
            std::string java_path(wbuf);
            while (!java_path.empty() &&
                   (java_path.back() == '\n' || java_path.back() == '\r'))
                java_path.pop_back();
            std::filesystem::path jp(java_path);
            if (jp.has_parent_path() && jp.parent_path().has_parent_path()) {
                return jp.parent_path().parent_path().string();
            }
            return "";
        }
    }
    return "";  // No Java 21+ found
}

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

        // Environment probe for language toolchains (INFRA-04)
        {
            auto log_env = [](const char* var) {
                const char* val = ::getenv(var);
                if (val && val[0] != '\0') {
                    spdlog::info("Env: {}={}", var, val);
                } else {
                    spdlog::warn("Env: {} not set", var);
                }
            };
            log_env("CARGO_HOME");
            log_env("GOPATH");
            log_env("JAVA_HOME");
        }

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

        // rust-analyzer for Rust (RUST-01, RUST-02)
        std::string ra_path = find_binary("rust-analyzer");
        if (!ra_path.empty()) {
            // Version probe: distinguishes real rust-analyzer from rustup proxy stub.
            // Proxy stub exits non-zero with "error: Unknown binary..." — real binary exits 0.
            std::string ra_cmd = ra_path + " --version 2>&1";
            FILE* ra_pipe = ::popen(ra_cmd.c_str(), "r");
            std::string ra_version;
            if (ra_pipe) {
                char ra_buf[256];
                while (::fgets(ra_buf, sizeof(ra_buf), ra_pipe)) ra_version += ra_buf;
                int ra_rc = ::pclose(ra_pipe);
                while (!ra_version.empty() &&
                       (ra_version.back() == '\n' || ra_version.back() == '\r')) {
                    ra_version.pop_back();
                }
                if (ra_rc != 0) ra_version.clear();  // proxy stub failure
            }
            if (!ra_version.empty()) {
                lsp_manager.register_language("rust", {ra_path, {}, {".rs"}});
                spdlog::info("LSP: registered rust-analyzer {} at {}", ra_version, ra_path);
            } else {
                spdlog::warn("LSP: rust-analyzer found at {} but --version failed "
                             "(rustup component not installed?) — Rust LSP disabled", ra_path);
            }
        }

        // gopls for Go (GO-01)
        std::string gopls_path = find_binary("gopls");
        if (!gopls_path.empty()) {
            // Version probe: 'gopls version' (subcommand, NOT --version flag)
            // exits 0 with "golang.org/x/tools/gopls vX.Y.Z" on success
            std::string gopls_cmd = gopls_path + " version 2>&1";
            FILE* gopls_pipe = ::popen(gopls_cmd.c_str(), "r");
            std::string gopls_version;
            if (gopls_pipe) {
                char gopls_buf[256];
                while (::fgets(gopls_buf, sizeof(gopls_buf), gopls_pipe))
                    gopls_version += gopls_buf;
                int gopls_rc = ::pclose(gopls_pipe);
                while (!gopls_version.empty() &&
                       (gopls_version.back() == '\n' || gopls_version.back() == '\r')) {
                    gopls_version.pop_back();
                }
                if (gopls_rc != 0) gopls_version.clear();
            }
            if (!gopls_version.empty()) {
                lsp_manager.register_language("go", {gopls_path, {}, {".go"}});
                spdlog::info("LSP: registered gopls {} at {}", gopls_version, gopls_path);
            } else {
                spdlog::warn("LSP: gopls found at {} but version check failed "
                             "— Go LSP disabled", gopls_path);
            }
        }

        // kotlin-language-server for Kotlin (KT-01, KT-04)
        std::string kls_path = find_binary("kotlin-language-server");
        if (!kls_path.empty()) {
            // Version probe: kotlin-language-server --version
            std::string kls_cmd = kls_path + " --version 2>&1";
            FILE* kls_pipe = ::popen(kls_cmd.c_str(), "r");
            std::string kls_version;
            if (kls_pipe) {
                char kls_buf[256];
                while (::fgets(kls_buf, sizeof(kls_buf), kls_pipe)) kls_version += kls_buf;
                int kls_rc = ::pclose(kls_pipe);
                while (!kls_version.empty() &&
                       (kls_version.back() == '\n' || kls_version.back() == '\r')) {
                    kls_version.pop_back();
                }
                if (kls_rc != 0) kls_version.clear();
            }
            if (!kls_version.empty()) {
                codetldr::LspServerConfig kls_config;
                kls_config.command = kls_path;
                kls_config.args = {};  // kotlin-language-server uses stdio by default
                kls_config.extensions = {".kt", ".kts"};
                kls_config.handshake_timeout_s = 120;  // KT-04: JVM cold-start needs 120s
                lsp_manager.register_language("kotlin", kls_config);
                spdlog::info("LSP: registered kotlin-language-server {} at {} (timeout=120s)",
                             kls_version, kls_path);
            } else {
                spdlog::warn("LSP: kotlin-language-server found at {} but --version failed "
                             "— Kotlin LSP disabled", kls_path);
            }
        }

        // jdtls for Java (JDT-01 through JDT-06)
        std::string detected_java_home = detect_java_21_plus();
        if (detected_java_home.empty()) {
            // JDT-03: Java 21+ not found — register as unavailable with message
            spdlog::warn("LSP: Java 21+ not detected (JAVA_HOME={}) — "
                         "Java LSP (jdtls) disabled",
                         (::getenv("JAVA_HOME") ? ::getenv("JAVA_HOME") : "not set"));
            lsp_manager.register_unavailable_language(
                "java",
                "Java 21+ required (JAVA_HOME=" +
                std::string(::getenv("JAVA_HOME") ? ::getenv("JAVA_HOME") : "not set") + ")");
        } else {
            std::string jdtls_path = find_binary("jdtls");
            if (!jdtls_path.empty()) {
                // Version probe: jdtls --version
                std::string jdtls_cmd = jdtls_path + " --version 2>&1";
                FILE* jdtls_pipe = ::popen(jdtls_cmd.c_str(), "r");
                std::string jdtls_version;
                if (jdtls_pipe) {
                    char jdtls_buf[256];
                    while (::fgets(jdtls_buf, sizeof(jdtls_buf), jdtls_pipe))
                        jdtls_version += jdtls_buf;
                    int jdtls_rc = ::pclose(jdtls_pipe);
                    while (!jdtls_version.empty() &&
                           (jdtls_version.back() == '\n' || jdtls_version.back() == '\r'))
                        jdtls_version.pop_back();
                    if (jdtls_rc != 0) jdtls_version.clear();
                }
                if (!jdtls_version.empty()) {
                    // JDT-02: Compute per-project -data dir
                    auto xdg = codetldr::resolve_xdg_paths();
                    std::filesystem::path data_dir =
                        xdg.cache_home / "jdtls-data" /
                        sha256_string(project_root.string()).substr(0, 12);
                    std::filesystem::create_directories(data_dir);

                    codetldr::LspServerConfig jdtls_config;
                    jdtls_config.command = jdtls_path;
                    jdtls_config.args = {"-data", data_dir.string()};  // JDT-02
                    jdtls_config.extensions = {".java"};
                    jdtls_config.handshake_timeout_s = 180;             // JDT-06
                    jdtls_config.extra_init_options =                    // JDT-05
                        {{"java", {{"home", detected_java_home}}}};
                    lsp_manager.register_language("java", jdtls_config);
                    spdlog::info("LSP: registered jdtls {} at {} "
                                 "(java_home={}, data_dir={}, timeout=180s)",
                                 jdtls_version, jdtls_path,
                                 detected_java_home, data_dir.string());
                } else {
                    spdlog::warn("LSP: jdtls found at {} but --version failed "
                                 "— Java LSP disabled", jdtls_path);
                }
            }
        }

        lsp_manager.set_project_root(project_root);
        coordinator.set_lsp_manager(&lsp_manager);

        // Phase 26: LSP call graph resolver
        auto lsp_resolver = std::make_unique<codetldr::LspCallGraphResolver>(
            db.raw(), lsp_manager);
        coordinator.set_lsp_resolver(std::move(lsp_resolver));

        // Phase 27: LSP dependency resolver
        auto lsp_dep_resolver = std::make_unique<codetldr::LspDependencyResolver>(
            db.raw(), lsp_manager);
        coordinator.set_lsp_dependency_resolver(std::move(lsp_dep_resolver));

        // Phase 27: LSP call hierarchy resolver
        auto lsp_ch_resolver = std::make_unique<codetldr::LspCallHierarchyResolver>(
            db.raw(), lsp_manager);
        coordinator.set_lsp_call_hierarchy_resolver(std::move(lsp_ch_resolver));

        coordinator.run();

        spdlog::info("codetldr-daemon stopped cleanly");
        return 0;

    } catch (const std::exception& ex) {
        std::cerr << "codetldr-daemon fatal error: " << ex.what() << "\n";
        return 1;
    }
}
