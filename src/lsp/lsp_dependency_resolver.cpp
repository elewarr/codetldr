#include "lsp/lsp_dependency_resolver.h"
#include "lsp/lsp_call_graph_resolver.h"
#include "lsp/lsp_manager.h"
#include "lsp/lsp_transport.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <string>

// Tree-sitter C API
#include <tree_sitter/api.h>

// Language grammar declarations (C linkage, defined in grammar libs)
extern "C" {
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_javascript(void);
const TSLanguage* tree_sitter_typescript(void);
}

namespace codetldr {

LspDependencyResolver::LspDependencyResolver(SQLite::Database& db, LspManager& lsp_manager)
    : db_(db)
    , lsp_manager_(lsp_manager)
{}

LspDependencyResolver::~LspDependencyResolver() {
    cancelled_.store(true, std::memory_order_relaxed);
}

// static
std::string LspDependencyResolver::uri_to_path(const std::string& uri) {
    return LspCallGraphResolver::uri_to_path(uri);
}

// static
std::string LspDependencyResolver::path_to_uri(const std::filesystem::path& path) {
    return LspCallGraphResolver::path_to_uri(path);
}

// static
std::string LspDependencyResolver::import_kind_for_language(const std::string& language) {
    if (language == "cpp" || language == "c") {
        return "include";
    }
    // python, javascript, typescript, and others use "import"
    return "import";
}

int64_t LspDependencyResolver::lookup_file_id(const std::string& path) {
    try {
        SQLite::Statement q(db_, "SELECT id FROM files WHERE path = ?");
        q.bind(1, path);
        if (q.executeStep()) {
            return q.getColumn(0).getInt64();
        }
    } catch (...) {}
    return -1;
}

void LspDependencyResolver::handle_dependency_response(
        const nlohmann::json& result,
        int64_t importer_file_id,
        int import_line,
        const std::string& import_kind) {

    if (cancelled_.load(std::memory_order_relaxed)) return;
    if (result.is_null()) return;

    // Handle both Location and Location[] — take first element
    const nlohmann::json* loc = nullptr;
    if (result.is_array()) {
        if (result.empty()) return;
        loc = &result[0];
    } else if (result.is_object()) {
        loc = &result;
    } else {
        return;
    }

    // Extract uri
    if (!loc->contains("uri")) return;
    std::string uri = (*loc)["uri"].get<std::string>();
    std::string target_path = uri_to_path(uri);

    if (target_path.empty()) return;

    // Look up target_file_id (may be -1 if not indexed)
    int64_t target_file_id = lookup_file_id(target_path);

    try {
        SQLite::Statement ins(db_,
            "INSERT INTO lsp_dependencies "
            "(importer_file_id, import_line, import_kind, target_file_id, target_file_path) "
            "VALUES (?, ?, ?, ?, ?)");
        ins.bind(1, importer_file_id);
        ins.bind(2, import_line);
        ins.bind(3, import_kind);
        if (target_file_id >= 0) {
            ins.bind(4, target_file_id);
        } else {
            ins.bind(4); // NULL
        }
        ins.bind(5, target_path);
        ins.exec();
    } catch (const std::exception& e) {
        spdlog::warn("LspDependencyResolver: failed to insert lsp_dependency for line {}: {}",
                     import_line, e.what());
    }
}

int LspDependencyResolver::resolve_dependencies(
        const std::filesystem::path& file_path, int64_t file_id,
        const std::string& language) {

    if (cancelled_.load(std::memory_order_relaxed)) return 0;

    // Delete stale lsp_dependencies for this importer_file_id before inserting
    try {
        SQLite::Statement del(db_,
            "DELETE FROM lsp_dependencies WHERE importer_file_id = ?");
        del.bind(1, file_id);
        del.exec();
    } catch (const std::exception& e) {
        spdlog::warn("LspDependencyResolver: failed to delete stale deps for file_id={}: {}",
                     file_id, e.what());
    }

    // Read the file content for Tree-sitter parsing
    std::string source;
    try {
        std::ifstream fs(file_path);
        if (!fs.is_open()) return 0;
        std::ostringstream ss;
        ss << fs.rdbuf();
        source = ss.str();
    } catch (...) {
        return 0;
    }

    if (source.empty()) return 0;

    // Select Tree-sitter language and query string based on language
    const TSLanguage* ts_lang = nullptr;
    std::string query_string;

    if (language == "cpp" || language == "c") {
        ts_lang = (language == "cpp") ? tree_sitter_cpp() : tree_sitter_c();
        query_string = "(preproc_include path: [(string_literal) (system_lib_string)] @import_path)";
    } else if (language == "python") {
        ts_lang = tree_sitter_python();
        // Two patterns for python imports
        query_string =
            "(import_statement name: (dotted_name) @import_path) "
            "(import_from_statement module_name: (dotted_name) @import_path)";
    } else if (language == "javascript" || language == "typescript") {
        ts_lang = (language == "javascript") ? tree_sitter_javascript() : tree_sitter_typescript();
        query_string = "(import_statement source: (string) @import_path)";
    } else {
        // Language not supported for import resolution
        return 0;
    }

    // Parse the source with Tree-sitter
    TSParser* parser = ts_parser_new();
    if (!parser) return 0;

    if (!ts_parser_set_language(parser, ts_lang)) {
        ts_parser_delete(parser);
        return 0;
    }

    TSTree* tree = ts_parser_parse_string(parser, nullptr, source.c_str(),
                                          static_cast<uint32_t>(source.size()));
    if (!tree) {
        ts_parser_delete(parser);
        return 0;
    }

    // Compile Tree-sitter query
    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    TSQuery* query = ts_query_new(ts_lang,
                                  query_string.c_str(),
                                  static_cast<uint32_t>(query_string.size()),
                                  &error_offset, &error_type);

    if (!query || error_type != TSQueryErrorNone) {
        spdlog::debug("LspDependencyResolver: query compile failed for language={} error_offset={}",
                      language, error_offset);
        if (query) ts_query_delete(query);
        ts_tree_delete(tree);
        ts_parser_delete(parser);
        return 0;
    }

    // Run the query and collect import line numbers and raw text
    struct ImportInfo {
        int line;          // 1-indexed
        std::string text;  // raw import path text from Tree-sitter node
    };
    std::vector<ImportInfo> imports;

    TSNode root_node = ts_tree_root_node(tree);
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, query, root_node);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        for (uint32_t i = 0; i < match.capture_count; ++i) {
            const TSQueryCapture& cap = match.captures[i];
            // Get capture name to check it's @import_path
            uint32_t cap_name_len = 0;
            const char* cap_name = ts_query_capture_name_for_id(query, cap.index, &cap_name_len);
            if (cap_name && std::string(cap_name, cap_name_len) == "import_path") {
                TSPoint start = ts_node_start_point(cap.node);
                // Tree-sitter is 0-indexed; convert to 1-indexed
                int line = static_cast<int>(start.row) + 1;
                // Extract the raw import path text from the node
                uint32_t start_byte = ts_node_start_byte(cap.node);
                uint32_t end_byte = ts_node_end_byte(cap.node);
                std::string text = source.substr(start_byte, end_byte - start_byte);
                // Strip surrounding quotes if present (string literals include them)
                if (text.size() >= 2 &&
                    (text.front() == '"' || text.front() == '\'' || text.front() == '<')) {
                    text = text.substr(1, text.size() - 2);
                }
                imports.push_back({line, std::move(text)});
            }
        }
    }

    ts_query_cursor_delete(cursor);
    ts_query_delete(query);
    ts_tree_delete(tree);
    ts_parser_delete(parser);

    if (imports.empty()) return 0;

    std::string file_uri = path_to_uri(file_path);
    std::string import_kind = import_kind_for_language(language);
    int dispatched = 0;

    for (const auto& imp : imports) {
        int import_line = imp.line;
        std::string import_text = imp.text;

        bool sent = lsp_manager_.send_when_ready(language,
            [this, file_path, file_uri, file_id, import_line, import_kind, language]() mutable {
                if (cancelled_.load(std::memory_order_relaxed)) return;

                lsp_manager_.ensure_document_open(language, file_path);

                LspTransport* transport = lsp_manager_.get_transport(language);
                if (!transport) return;

                nlohmann::json params;
                params["textDocument"]["uri"] = file_uri;
                // LSP is 0-indexed
                params["position"]["line"]      = import_line > 0 ? import_line - 1 : 0;
                params["position"]["character"] = 0;

                transport->send_request("textDocument/definition", params,
                    [this, file_id, import_line, import_kind](
                            const nlohmann::json& result,
                            const nlohmann::json& /*error*/) {
                        handle_dependency_response(result, file_id, import_line, import_kind);
                    });
            });

        if (sent) {
            dispatched++;
        } else {
            // LSP unavailable — persist Tree-sitter-parsed import as fallback
            // target_file_id is NULL (unresolved), target_file_path is the raw import text
            try {
                SQLite::Statement ins(db_,
                    "INSERT INTO lsp_dependencies "
                    "(importer_file_id, import_line, import_kind, target_file_id, target_file_path) "
                    "VALUES (?, ?, ?, NULL, ?)");
                ins.bind(1, file_id);
                ins.bind(2, import_line);
                ins.bind(3, import_kind);
                ins.bind(4, import_text);
                ins.exec();
            } catch (const std::exception& e) {
                spdlog::warn("LspDependencyResolver: fallback insert failed for line {}: {}",
                             import_line, e.what());
            }
        }
    }

    return dispatched;
}

} // namespace codetldr
