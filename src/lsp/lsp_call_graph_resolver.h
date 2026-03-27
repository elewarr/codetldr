#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <atomic>

namespace codetldr {
class LspManager;

// Resolves call sites in a file to their cross-file definitions via LSP,
// and discovers callers of symbols defined in a file via LSP references.
// All LSP queries are async via LspManager::send_when_ready().
// Results are persisted to lsp_definitions and lsp_references tables.
class LspCallGraphResolver {
public:
    LspCallGraphResolver(SQLite::Database& db, LspManager& lsp_manager);
    ~LspCallGraphResolver();

    // Resolve all outgoing call sites in file_path to their definition locations.
    // Reads call sites from the `calls` table for this file, sends textDocument/definition
    // for each, and persists results to lsp_definitions.
    // Deletes stale lsp_definitions for this file before inserting.
    // Returns number of LSP queries dispatched (0 if LSP unavailable).
    int resolve_callees(const std::filesystem::path& file_path, int64_t file_id,
                        const std::string& language);

    // Discover all callers of symbols defined in file_path via textDocument/references.
    // Reads symbol definitions from the `symbols` table for this file, sends
    // textDocument/references for each, and persists results to lsp_references.
    // Deletes stale lsp_references for this file before inserting.
    // Returns number of LSP queries dispatched (0 if LSP unavailable).
    int resolve_callers(const std::filesystem::path& file_path, int64_t file_id,
                        const std::string& language);

    // Convenience: resolve both callees and callers for a file.
    // Called from coordinator after analyze_file() when content_hash changed.
    void resolve_file(const std::filesystem::path& file_path, int64_t file_id,
                      const std::string& language);

    // Strip "file://" prefix from LSP URI to get absolute path (exposed for testing)
    static std::string uri_to_path(const std::string& uri);

    // Construct "file://" URI from absolute path (exposed for testing)
    static std::string path_to_uri(const std::filesystem::path& path);

private:
    // Look up file_id for a path, or return -1 if not in index
    int64_t lookup_file_id(const std::string& path);

    // Handle Location or Location[] from textDocument/definition response
    void handle_definition_response(const nlohmann::json& result,
                                     int64_t caller_file_id, int call_line,
                                     int call_col, const std::string& callee_name);

    // Handle Location[] from textDocument/references response
    void handle_references_response(const nlohmann::json& result,
                                     int64_t callee_file_id,
                                     const std::string& callee_name, int def_line);

    SQLite::Database& db_;
    LspManager& lsp_manager_;
    std::atomic<bool> cancelled_{false};
};

} // namespace codetldr
