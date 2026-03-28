#include "lsp/lsp_manager.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

namespace codetldr {

std::string to_string(LspServerState s) {
    switch (s) {
        case LspServerState::kNotStarted:  return "not_started";
        case LspServerState::kStarting:    return "starting";
        case LspServerState::kReady:       return "ready";
        case LspServerState::kIndexing:    return "indexing";
        case LspServerState::kUnavailable: return "unavailable";
        case LspServerState::kDegraded:    return "degraded";
    }
    return "unknown";
}

LspManager::~LspManager() {
    shutdown();
}

void LspManager::register_language(const std::string& language, LspServerConfig config) {
    // ServerEntry contains LspTransport which is non-copyable, so we must
    // default-construct in-place via operator[] then assign fields.
    auto& entry = servers_[language];
    entry.config = std::move(config);
    entry.state = LspServerState::kNotStarted;
    entry.detected = false;
}

void LspManager::register_unavailable_language(
        const std::string& language, const std::string& reason) {
    // LspTransport is non-copyable/non-movable — use operator[] like register_language().
    auto& entry = servers_[language];
    entry.state = LspServerState::kUnavailable;
    entry.detected = true;
    entry.unavailable_reason = reason;
    spdlog::info("LspManager: registered '{}' as unavailable: {}", language, reason);
}

void LspManager::set_detected_languages(const std::vector<std::string>& languages) {
    // First, clear all detected flags
    for (auto& [lang, entry] : servers_) {
        entry.detected = false;
    }
    // Then set detected for each language in the input
    for (const auto& lang : languages) {
        auto it = servers_.find(lang);
        if (it != servers_.end()) {
            it->second.detected = true;
        }
    }
}

bool LspManager::ensure_server(const std::string& language) {
    auto it = servers_.find(language);
    if (it == servers_.end()) {
        return false;
    }
    ServerEntry& entry = it->second;

    if (!entry.detected) {
        return false;
    }

    if (entry.state == LspServerState::kUnavailable) {
        return false;
    }

    // Already running in some capacity
    if (entry.state == LspServerState::kStarting ||
        entry.state == LspServerState::kReady ||
        entry.state == LspServerState::kIndexing ||
        entry.state == LspServerState::kDegraded) {
        return true;
    }

    // kNotStarted + first start (restart_count == 0)
    if (entry.state == LspServerState::kNotStarted && entry.restart_count == 0) {
        return try_spawn(entry, language);
    }

    return false;
}

LspTransport* LspManager::get_transport(const std::string& language) {
    auto it = servers_.find(language);
    if (it == servers_.end()) {
        return nullptr;
    }
    ServerEntry& entry = it->second;
    if (entry.state == LspServerState::kStarting ||
        entry.state == LspServerState::kReady ||
        entry.state == LspServerState::kIndexing ||
        entry.state == LspServerState::kDegraded) {
        return &entry.transport;
    }
    return nullptr;
}

void LspManager::set_project_root(const std::filesystem::path& root) {
    project_root_ = root;
}

nlohmann::json LspManager::make_initialize_params(const std::string& language,
                                                    const std::filesystem::path& project_root,
                                                    const nlohmann::json& extra_init_options) {
    (void)language;  // reserved for language-specific capability tweaks
    std::string root_uri = "file://" + project_root.string();
    nlohmann::json params = {
        {"processId", static_cast<int>(::getpid())},
        {"clientInfo", {{"name", "codetldr"}, {"version", "2.0"}}},
        {"rootUri", root_uri},
        {"workspaceFolders", {{{"uri", root_uri},
                               {"name", project_root.filename().string()}}}},
        {"capabilities", {
            {"textDocument", {
                {"synchronization", {
                    {"didOpen", true},
                    {"didClose", true},
                    {"didChange", true}
                }},
                {"definition", {{"dynamicRegistration", false}}},
                {"references", {{"dynamicRegistration", false}}}
            }},
            {"workspace", {
                {"workspaceFolders", true},
                {"configuration", false}
            }}
        }}
    };
    // JDT-05: merge language-specific initializationOptions when provided
    if (!extra_init_options.is_null()) {
        params["initializationOptions"] = extra_init_options;
    }
    return params;
}

bool LspManager::try_spawn(ServerEntry& entry, const std::string& language) {
    int rc = entry.transport.spawn(entry.config.command, entry.config.args);
    if (rc != 0) {
        spdlog::error("LspManager: failed to spawn LSP server for '{}': error {}", language, rc);
        return false;
    }
    entry.state = LspServerState::kStarting;
    // INFRA-03: Apply per-language handshake timeout when configured
    if (entry.config.handshake_timeout_s > 0) {
        entry.transport.set_timeout(
            std::chrono::seconds(entry.config.handshake_timeout_s));
        spdlog::info("LspManager: set handshake timeout for '{}' to {}s",
                     language, entry.config.handshake_timeout_s);
    }
    int fd = entry.transport.stdout_fd();
    if (fd >= 0) {
        fd_to_language_[fd] = language;
    }
    spdlog::info("LspManager: spawned LSP server for '{}' (pid={}, stdout_fd={})",
                 language, entry.transport.pid(), fd);

    // Send LSP initialize request immediately after spawn
    auto params = make_initialize_params(language, project_root_, entry.config.extra_init_options);
    entry.transport.send_request("initialize", params,
        [this, language](const nlohmann::json& result, const nlohmann::json& error) {
            (void)result;
            auto it = servers_.find(language);
            if (it == servers_.end()) return;
            ServerEntry& e = it->second;
            // Stale callback guard: ignore if we've already moved past kStarting
            if (e.state != LspServerState::kStarting) return;

            if (!error.is_null()) {
                spdlog::error("LspManager: initialize failed for '{}': {}", language, error.dump());
                // Treat as crash for backoff/circuit-breaker logic
                handle_crash(e, language, Clock::now());
                return;
            }

            // Send initialized notification — LSP spec requires this before any requests
            e.transport.send_notification("initialized", nlohmann::json::object());
            e.state = LspServerState::kReady;
            spdlog::info("LspManager: '{}' reached kReady", language);

            // Drain queued requests first (before potential downgrade to kDegraded)
            auto queued = std::move(e.pending_requests);
            e.pending_requests.clear();
            for (auto& fn : queued) {
                fn();
            }

            // Check compile_commands.json for clangd after queue drain
            if (language == "cpp") {
                check_clangd_compile_db(e);
            }
            if (language == "rust") {
                check_cargo_toml(e);
            }
            if (language == "go") {
                check_go_mod(e);
            }
            if (language == "kotlin") {
                check_kotlin_build(e);
            }
            if (language == "java") {
                check_java_build(e);
            }
        });

    return true;
}

void LspManager::handle_crash(ServerEntry& entry, const std::string& language,
                               Clock::time_point now) {
    // Clear per-session state so the next spawn gets fresh state
    entry.opened_uris.clear();
    entry.pending_requests.clear();

    // Add crash time
    entry.crash_times.push_back(now);

    // Evict crashes older than kCrashWindow from the front
    while (!entry.crash_times.empty() &&
           (now - entry.crash_times.front()) > kCrashWindow) {
        entry.crash_times.pop_front();
    }

    if (static_cast<int>(entry.crash_times.size()) >= kMaxCrashesInWindow) {
        entry.state = LspServerState::kUnavailable;
        spdlog::error("LspManager: circuit breaker tripped for '{}': {} crashes in {}s window",
                      language, entry.crash_times.size(),
                      kCrashWindow.count());
        return;
    }

    // Schedule restart with exponential backoff
    auto delay = backoff_for(entry.restart_count);
    entry.restart_after = now + delay;
    entry.restart_count++;
    entry.state = LspServerState::kNotStarted;
    spdlog::warn("LspManager: LSP server '{}' crashed (attempt {}), restarting in {}s",
                 language, entry.restart_count, delay.count());
}

std::chrono::seconds LspManager::backoff_for(int restart_count) const {
    // 1 << restart_count seconds, capped at 60
    long delay = 1L << restart_count;
    return std::chrono::seconds(std::min(delay, static_cast<long>(kBackoffMax.count())));
}

bool LspManager::all_backends_ready() const {
    for (const auto& [lang, entry] : servers_) {
        if (!entry.detected) continue;
        if (entry.state == LspServerState::kUnavailable) continue;
        if (entry.state == LspServerState::kNotStarted ||
            entry.state == LspServerState::kStarting) {
            return false;
        }
    }
    return true;
}

bool LspManager::tick(Clock::time_point now) {
    bool changed = false;

    for (auto& [language, entry] : servers_) {
        if (!entry.detected) {
            continue;
        }

        // Check for crashed running processes
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {

            // Use WNOHANG to non-blocking check if the process has exited.
            // transport.is_running() only returns false after wait() is called,
            // so we need waitpid(WNOHANG) to detect crashes without blocking.
            bool crashed = false;
            pid_t pid = entry.transport.pid();
            if (pid > 0) {
                int status = 0;
                pid_t result = ::waitpid(pid, &status, WNOHANG);
                if (result > 0) {
                    // Process has exited
                    crashed = true;
                } else if (result < 0) {
                    // ECHILD: process already reaped — also treat as crash
                    crashed = true;
                }
                // result == 0: still running
            } else {
                // No pid — treat as not running
                crashed = !entry.transport.is_running();
            }

            if (crashed) {
                // Remove old fd from map
                int old_fd = entry.transport.stdout_fd();
                if (old_fd >= 0) {
                    fd_to_language_.erase(old_fd);
                }
                // Don't call entry.transport.wait() since we already waited with WNOHANG above
                // But we need to mark it as waited in the transport — call wait() with knowledge
                // that it won't block (process is already reaped)
                entry.transport.wait();
                handle_crash(entry, language, now);
                changed = true;
            }
        }

        // Schedule pending restarts
        if (entry.state == LspServerState::kNotStarted && entry.restart_count > 0) {
            if (now >= entry.restart_after) {
                if (try_spawn(entry, language)) {
                    changed = true;
                }
            }
        }
    }

    return changed;
}

void LspManager::append_pollfds(std::vector<pollfd>& fds) const {
    for (const auto& [language, entry] : servers_) {
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {
            int fd = entry.transport.stdout_fd();
            if (fd >= 0) {
                fds.push_back({fd, POLLIN, 0});
            }
        }
    }
}

void LspManager::dispatch_read(int fd) {
    auto it = fd_to_language_.find(fd);
    if (it == fd_to_language_.end()) {
        return;
    }
    const std::string& language = it->second;
    auto sit = servers_.find(language);
    if (sit == servers_.end()) {
        return;
    }
    sit->second.transport.poll_read();
}

nlohmann::json LspManager::status_json() const {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& [language, entry] : servers_) {
        if (!entry.detected) {
            continue;
        }
        nlohmann::json item = {
            {"language", language},
            {"state", to_string(entry.state)}
        };
        if (!entry.unavailable_reason.empty()) {
            item["message"] = entry.unavailable_reason;
        }
        result.push_back(item);
    }
    return result;
}

void LspManager::shutdown() {
    for (auto& [language, entry] : servers_) {
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {
            entry.transport.kill();
            entry.transport.wait();
        }
    }
}

void LspManager::check_timeouts() {
    for (auto& [language, entry] : servers_) {
        if (entry.state == LspServerState::kStarting ||
            entry.state == LspServerState::kReady ||
            entry.state == LspServerState::kIndexing ||
            entry.state == LspServerState::kDegraded) {
            entry.transport.check_timeouts();
        }
    }
}

std::string LspManager::file_uri(const std::filesystem::path& path) {
    return "file://" + std::filesystem::absolute(path).string();
}

std::string LspManager::language_id_for(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
        ext == ".h"   || ext == ".hpp") return "cpp";
    if (ext == ".c")   return "c";
    if (ext == ".py")  return "python";
    if (ext == ".ts")  return "typescript";
    if (ext == ".tsx") return "typescriptreact";
    if (ext == ".js")  return "javascript";
    if (ext == ".jsx") return "javascriptreact";
    if (ext == ".rs")                     return "rust";
    if (ext == ".go")                     return "go";
    if (ext == ".kt" || ext == ".kts")    return "kotlin";
    if (ext == ".java")                   return "java";
    if (ext == ".swift")                  return "swift";
    if (ext == ".m")                      return "objective-c";
    if (ext == ".rb" || ext == ".rake" || ext == ".gemspec" || ext == ".ru") return "ruby";
    return "plaintext";
}

void LspManager::check_clangd_compile_db(ServerEntry& entry) {
    // Search in priority order:
    // 1. project_root_ / compile_commands.json
    // 2. project_root_ / build / compile_commands.json
    // 3. project_root_ / cmake-build-* / compile_commands.json
    auto check_path = [&](const std::filesystem::path& p) -> bool {
        if (std::filesystem::exists(p)) {
            spdlog::info("LspManager: clangd found compile_commands.json at {}",
                         p.parent_path().string());
            return true;
        }
        return false;
    };

    if (check_path(project_root_ / "compile_commands.json")) return;
    if (check_path(project_root_ / "build" / "compile_commands.json")) return;

    // Scan for cmake-build-* directories
    std::error_code ec;
    for (const auto& dir_entry : std::filesystem::directory_iterator(project_root_, ec)) {
        if (!dir_entry.is_directory()) continue;
        const std::string dirname = dir_entry.path().filename().string();
        if (dirname.substr(0, 12) == "cmake-build-") {
            if (check_path(dir_entry.path() / "compile_commands.json")) return;
        }
    }

    // Not found — set degraded
    entry.state = LspServerState::kDegraded;
    spdlog::warn("LspManager: clangd degraded -- compile_commands.json not found. "
                 "Expected at: {}/compile_commands.json (or build/ or cmake-build-*/)",
                 project_root_.string());
}

void LspManager::check_cargo_toml(ServerEntry& entry) {
    (void)entry;  // No state change — rust-analyzer does not degrade without Cargo.toml
    if (std::filesystem::exists(project_root_ / "Cargo.toml")) {
        spdlog::info("LspManager: rust-analyzer workspace root confirmed (Cargo.toml at {})",
                     project_root_.string());
    } else {
        spdlog::warn("LspManager: rust-analyzer: no Cargo.toml found at {} — "
                     "single-file mode (workspace features limited)", project_root_.string());
    }
    // Do NOT set kDegraded: rust-analyzer supports single-file Rust analysis.
}

void LspManager::check_go_mod(ServerEntry& entry) {
    (void)entry;  // No state change — gopls does not degrade without go.mod

    // GO-03: Walk up from project_root_ to find nearest go.mod
    std::filesystem::path search = project_root_;
    bool found_go_mod = false;
    while (search.has_parent_path() && search != search.parent_path()) {
        if (std::filesystem::exists(search / "go.mod")) {
            spdlog::info("LspManager: gopls workspace root: go.mod found at {}",
                         search.string());
            found_go_mod = true;
            break;
        }
        search = search.parent_path();
    }
    if (!found_go_mod) {
        spdlog::warn("LspManager: gopls: no go.mod found in {} or any parent — "
                     "single-file/GOPATH mode (module features limited)",
                     project_root_.string());
    }

    // GO-04: Count go.mod files under project_root_ for multi-module warning
    // Skip vendor/ to avoid false positives from vendored dependencies
    int go_mod_count = 0;
    bool has_go_work = std::filesystem::exists(project_root_ / "go.work");
    std::error_code ec;
    for (const auto& dir_entry : std::filesystem::recursive_directory_iterator(
             project_root_,
             std::filesystem::directory_options::skip_permission_denied, ec)) {
        // Skip vendor directory to avoid false positives
        const auto& p = dir_entry.path();
        bool in_vendor = false;
        for (const auto& component : p) {
            if (component == "vendor") { in_vendor = true; break; }
        }
        if (in_vendor) continue;
        if (dir_entry.is_regular_file() && p.filename() == "go.mod") {
            ++go_mod_count;
            if (go_mod_count > 10) break;  // Early exit: only need to know if count > 1
        }
    }
    if (go_mod_count > 1 && !has_go_work) {
        spdlog::warn("LspManager: gopls: {} go.mod files detected without go.work — "
                     "multi-module workspace not fully supported by single gopls instance "
                     "(ADV-02). Cross-module references may be incomplete.",
                     go_mod_count);
    }
    // Do NOT set kDegraded: gopls handles single-file and GOPATH mode without go.mod.
}

void LspManager::check_kotlin_build(ServerEntry& entry) {
    (void)entry;  // No state change — kotlin-language-server does not require build files

    // KT-05: Check for Gradle or Maven build files
    static const std::vector<std::string> kBuildFiles = {
        "build.gradle.kts", "build.gradle", "pom.xml"
    };

    for (const auto& name : kBuildFiles) {
        if (std::filesystem::exists(project_root_ / name)) {
            spdlog::info("LspManager: kotlin-language-server workspace root confirmed ({} at {})",
                         name, project_root_.string());
            return;
        }
    }

    spdlog::warn("LspManager: kotlin-language-server: no build.gradle, build.gradle.kts, "
                 "or pom.xml found at {} — workspace features may be limited",
                 project_root_.string());
    // Do NOT set kDegraded: kotlin-language-server can operate without build files.
}

void LspManager::check_java_build(ServerEntry& entry) {
    (void)entry;  // No state change — jdtls does not require build files
    static const std::array<const char*, 3> kJavaBuildFiles = {
        "pom.xml",
        "build.gradle",
        "build.gradle.kts"
    };
    for (const char* fname : kJavaBuildFiles) {
        if (std::filesystem::exists(project_root_ / fname)) {
            spdlog::info("LspManager: jdtls workspace root confirmed ({} at {})",
                         fname, project_root_.string());
            return;
        }
    }
    spdlog::warn("LspManager: jdtls: no pom.xml, build.gradle, or build.gradle.kts found "
                 "at {} — workspace-level features may be limited", project_root_.string());
    // Do NOT set kDegraded: jdtls can still serve single-file Java analysis.
}

void LspManager::ensure_document_open(const std::string& language,
                                       const std::filesystem::path& file_path) {
    auto it = servers_.find(language);
    if (it == servers_.end()) return;
    ServerEntry& entry = it->second;

    // Only send didOpen when server is running
    if (entry.state != LspServerState::kReady &&
        entry.state != LspServerState::kIndexing &&
        entry.state != LspServerState::kDegraded) {
        return;
    }

    std::string uri = file_uri(file_path);

    // Idempotent per session — skip if already opened
    if (entry.opened_uris.count(uri)) return;
    entry.opened_uris.insert(uri);

    // Read file content
    std::ifstream ifs(file_path);
    if (!ifs.is_open()) {
        entry.opened_uris.erase(uri);
        return;
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();

    std::string lang_id = language_id_for(file_path);

    entry.transport.send_notification("textDocument/didOpen", {
        {"textDocument", {
            {"uri",        uri},
            {"languageId", lang_id},
            {"version",    1},
            {"text",       content}
        }}
    });

    spdlog::debug("LspManager: didOpen '{}' for '{}'", uri, language);
}

bool LspManager::send_when_ready(const std::string& language, std::function<void()> action) {
    auto it = servers_.find(language);
    if (it == servers_.end()) return false;
    ServerEntry& e = it->second;
    if (e.state == LspServerState::kReady ||
        e.state == LspServerState::kIndexing ||
        e.state == LspServerState::kDegraded) {
        action();
        return true;
    }
    if (e.state == LspServerState::kStarting) {
        e.pending_requests.push_back(std::move(action));
        return true;
    }
    return false;
}

} // namespace codetldr
