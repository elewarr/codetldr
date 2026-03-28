#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <atomic>

namespace codetldr {
class LspManager;

// Resolves import/include/require nodes in a file to their cross-file definition
// targets via LSP textDocument/definition, and persists results to lsp_dependencies.
//
// Workflow:
//   1. Use Tree-sitter queries to find import nodes in the source file
//   2. For each import node, send textDocument/definition at the import line position
//   3. Parse the definition response URI, look up file_id, insert into lsp_dependencies
//
// Tree-sitter import query patterns by language:
//   C/C++:  (preproc_include path: [(string_literal) (system_lib_string)] @import_path) @import
//   Python: (import_statement name: (dotted_name) @import_path) @import
//           (import_from_statement module_name: (dotted_name) @import_path) @import
//   JS/TS:  (import_statement source: (string) @import_path) @import
//           (call_expression function: (identifier) @fn (#eq? @fn "require")
//            arguments: (arguments (string) @import_path)) @import
//
// All LSP queries are async via LspManager::send_when_ready().
// Stale lsp_dependencies rows for importer_file_id are deleted before inserting new ones.
class LspDependencyResolver {
public:
    LspDependencyResolver(SQLite::Database& db, LspManager& lsp_manager);
    ~LspDependencyResolver();

    // Non-copyable, non-movable
    LspDependencyResolver(const LspDependencyResolver&) = delete;
    LspDependencyResolver& operator=(const LspDependencyResolver&) = delete;

    // Resolve imports in a source file via Tree-sitter + LSP definition.
    // Returns number of LSP queries dispatched (0 if LSP unavailable or no imports found).
    // Deletes stale lsp_dependencies rows for importer_file_id before inserting.
    int resolve_dependencies(const std::filesystem::path& file_path, int64_t file_id,
                             const std::string& language);

    // Strip "file://" prefix from LSP URI to get absolute path (delegates to LspCallGraphResolver)
    static std::string uri_to_path(const std::string& uri);

    // Construct "file://" URI from absolute path (delegates to LspCallGraphResolver)
    static std::string path_to_uri(const std::filesystem::path& path);

private:
    // Look up file_id for a path, or return -1 if not in index
    int64_t lookup_file_id(const std::string& path);

    // Handle Location or Location[] from textDocument/definition response for an import
    void handle_dependency_response(const nlohmann::json& result,
                                    int64_t importer_file_id,
                                    int import_line,
                                    const std::string& import_kind);

    // Returns import kind string for the given language
    static std::string import_kind_for_language(const std::string& language);

    SQLite::Database& db_;
    LspManager& lsp_manager_;
    std::atomic<bool> cancelled_{false};
};

} // namespace codetldr
