#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <atomic>

namespace codetldr {
class LspManager;

// Resolves call hierarchy (incoming callers) for symbols defined in a file
// using the LSP callHierarchy/prepareCallHierarchy + callHierarchy/incomingCalls
// two-step protocol. Results are persisted to lsp_call_hierarchy_callers table.
//
// Protocol:
//   Step 1: textDocument/prepareCallHierarchy at symbol definition position
//   Step 2: callHierarchy/incomingCalls on the returned CallHierarchyItem
//   Result: insert CallHierarchyIncomingCall[] into lsp_call_hierarchy_callers
//
// Falls back gracefully when LSP is unavailable (returns 0 queries dispatched).
// All LSP queries are async via LspManager::send_when_ready().
class LspCallHierarchyResolver {
public:
    LspCallHierarchyResolver(SQLite::Database& db, LspManager& lsp_manager);
    ~LspCallHierarchyResolver();

    // Resolve incoming callers for all function/method symbols defined in file_path.
    // Deletes stale lsp_call_hierarchy_callers rows for callee_file_id before inserting.
    // Returns number of LSP queries dispatched (0 if LSP unavailable or no symbols).
    int resolve_incoming_callers(const std::filesystem::path& file_path,
                                  int64_t file_id,
                                  const std::string& language);

    // Strip "file://" prefix from LSP URI to get absolute path (delegates to LspCallGraphResolver)
    static std::string uri_to_path(const std::string& uri);

    // Construct "file://" URI from absolute path (delegates to LspCallGraphResolver)
    static std::string path_to_uri(const std::filesystem::path& path);

private:
    // Handle CallHierarchyIncomingCall[] from callHierarchy/incomingCalls response.
    // Extracts from.name, from.kind, from.uri, fromRanges[*].start.line/character.
    // Inserts into lsp_call_hierarchy_callers.
    void handle_incoming_calls_response(const nlohmann::json& result,
                                         int64_t callee_file_id,
                                         const std::string& callee_name,
                                         int callee_line);

    // Look up file_id for a path, or return -1 if not in index
    int64_t lookup_file_id(const std::string& path);

    SQLite::Database& db_;
    LspManager& lsp_manager_;
    std::atomic<bool> cancelled_{false};
};

} // namespace codetldr
