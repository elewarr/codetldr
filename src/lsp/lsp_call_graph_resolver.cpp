#include "lsp/lsp_call_graph_resolver.h"
#include "lsp/lsp_manager.h"
#include "lsp/lsp_transport.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <string>

namespace codetldr {

LspCallGraphResolver::LspCallGraphResolver(SQLite::Database& db, LspManager& lsp_manager)
    : db_(db)
    , lsp_manager_(lsp_manager)
{}

LspCallGraphResolver::~LspCallGraphResolver() {
    cancelled_.store(true, std::memory_order_relaxed);
}

// static
std::string LspCallGraphResolver::uri_to_path(const std::string& uri) {
    if (uri.size() >= 7 && uri.substr(0, 7) == "file://") {
        return uri.substr(7);
    }
    return uri;
}

// static
std::string LspCallGraphResolver::path_to_uri(const std::filesystem::path& path) {
    return "file://" + path.string();
}

int64_t LspCallGraphResolver::lookup_file_id(const std::string& path) {
    try {
        SQLite::Statement q(db_, "SELECT id FROM files WHERE path = ?");
        q.bind(1, path);
        if (q.executeStep()) {
            return q.getColumn(0).getInt64();
        }
    } catch (...) {}
    return -1;
}

void LspCallGraphResolver::handle_definition_response(
        const nlohmann::json& result,
        int64_t caller_file_id, int call_line,
        int call_col, const std::string& callee_name) {

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

    // Extract uri and range
    if (!loc->contains("uri") || !loc->contains("range")) return;
    std::string uri = (*loc)["uri"].get<std::string>();
    std::string def_path = uri_to_path(uri);

    int def_line = 0;
    int def_col = 0;
    if ((*loc)["range"].contains("start")) {
        const auto& start = (*loc)["range"]["start"];
        // LSP is 0-indexed; convert to 1-indexed
        def_line = start.value("line", 0) + 1;
        def_col  = start.value("character", 0);
    }

    // Look up def_file_id (may be -1 if not indexed)
    int64_t def_file_id = lookup_file_id(def_path);

    try {
        SQLite::Statement ins(db_,
            "INSERT INTO lsp_definitions "
            "(caller_file_id, call_line, call_col, callee_name, "
            " def_file_id, def_file_path, def_line, def_col, source) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'lsp')");
        ins.bind(1, caller_file_id);
        ins.bind(2, call_line);
        ins.bind(3, call_col);
        ins.bind(4, callee_name);
        if (def_file_id >= 0) {
            ins.bind(5, def_file_id);
        } else {
            ins.bind(5); // NULL
        }
        ins.bind(6, def_path);
        ins.bind(7, def_line);
        ins.bind(8, def_col);
        ins.exec();
    } catch (const std::exception& e) {
        spdlog::warn("LspCallGraphResolver: failed to insert lsp_definition for {}: {}",
                     callee_name, e.what());
    }
}

void LspCallGraphResolver::handle_references_response(
        const nlohmann::json& result,
        int64_t callee_file_id,
        const std::string& callee_name, int def_line) {

    if (cancelled_.load(std::memory_order_relaxed)) return;
    if (!result.is_array()) return;

    for (const auto& loc : result) {
        if (!loc.is_object()) continue;
        if (!loc.contains("uri") || !loc.contains("range")) continue;

        std::string uri = loc["uri"].get<std::string>();
        std::string caller_path = uri_to_path(uri);

        int caller_line = 0;
        int caller_col = 0;
        if (loc["range"].contains("start")) {
            const auto& start = loc["range"]["start"];
            // LSP is 0-indexed; convert to 1-indexed
            caller_line = start.value("line", 0) + 1;
            caller_col  = start.value("character", 0);
        }

        int64_t caller_file_id = lookup_file_id(caller_path);

        try {
            SQLite::Statement ins(db_,
                "INSERT INTO lsp_references "
                "(callee_file_id, callee_name, def_line, "
                " caller_file_id, caller_file_path, caller_line, caller_col, source) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, 'lsp')");
            ins.bind(1, callee_file_id);
            ins.bind(2, callee_name);
            ins.bind(3, def_line);
            if (caller_file_id >= 0) {
                ins.bind(4, caller_file_id);
            } else {
                ins.bind(4); // NULL
            }
            ins.bind(5, caller_path);
            ins.bind(6, caller_line);
            ins.bind(7, caller_col);
            ins.exec();
        } catch (const std::exception& e) {
            spdlog::warn("LspCallGraphResolver: failed to insert lsp_reference for {}: {}",
                         callee_name, e.what());
        }
    }
}

int LspCallGraphResolver::resolve_callees(
        const std::filesystem::path& file_path, int64_t file_id,
        const std::string& language) {

    // Delete stale lsp_definitions for this file before inserting new ones
    try {
        SQLite::Statement del(db_,
            "DELETE FROM lsp_definitions WHERE caller_file_id = ?");
        del.bind(1, file_id);
        del.exec();
    } catch (const std::exception& e) {
        spdlog::warn("LspCallGraphResolver: failed to delete stale lsp_definitions for file_id={}: {}",
                     file_id, e.what());
    }

    // Read call sites from the calls table
    struct CallSite {
        std::string callee_name;
        int line;
    };
    std::vector<CallSite> call_sites;

    try {
        SQLite::Statement q(db_,
            "SELECT callee_name, line FROM calls WHERE file_id = ?");
        q.bind(1, file_id);
        while (q.executeStep()) {
            call_sites.push_back({
                q.getColumn(0).getString(),
                q.getColumn(1).getInt()
            });
        }
    } catch (const std::exception& e) {
        spdlog::warn("LspCallGraphResolver: failed to read call sites for file_id={}: {}",
                     file_id, e.what());
        return 0;
    }

    if (call_sites.empty()) return 0;

    std::string file_uri = path_to_uri(file_path);
    int dispatched = 0;

    for (const auto& cs : call_sites) {
        std::string callee_name = cs.callee_name;
        int call_line = cs.line;

        bool sent = lsp_manager_.send_when_ready(language,
            [this, file_path, file_uri, file_id, call_line, callee_name, language]() mutable {
                if (cancelled_.load(std::memory_order_relaxed)) return;

                lsp_manager_.ensure_document_open(language, file_path);

                LspTransport* transport = lsp_manager_.get_transport(language);
                if (!transport) return;

                nlohmann::json params;
                params["textDocument"]["uri"] = file_uri;
                // LSP is 0-indexed
                params["position"]["line"]      = call_line > 0 ? call_line - 1 : 0;
                params["position"]["character"] = 0;

                transport->send_request("textDocument/definition", params,
                    [this, file_id, call_line, callee_name](
                            const nlohmann::json& result,
                            const nlohmann::json& /*error*/) {
                        handle_definition_response(result, file_id, call_line, 0, callee_name);
                    });
            });

        if (sent) dispatched++;
    }

    return dispatched;
}

int LspCallGraphResolver::resolve_callers(
        const std::filesystem::path& file_path, int64_t file_id,
        const std::string& language) {

    // Delete stale lsp_references for this file before inserting new ones
    try {
        SQLite::Statement del(db_,
            "DELETE FROM lsp_references WHERE callee_file_id = ?");
        del.bind(1, file_id);
        del.exec();
    } catch (const std::exception& e) {
        spdlog::warn("LspCallGraphResolver: failed to delete stale lsp_references for file_id={}: {}",
                     file_id, e.what());
    }

    // Read function/method symbols from this file
    struct SymbolDef {
        std::string name;
        int line_start;
    };
    std::vector<SymbolDef> symbols;

    try {
        SQLite::Statement q(db_,
            "SELECT s.name, s.line_start "
            "FROM symbols s "
            "WHERE s.file_id = ? AND s.kind IN ('function', 'method')");
        q.bind(1, file_id);
        while (q.executeStep()) {
            symbols.push_back({
                q.getColumn(0).getString(),
                q.getColumn(1).getInt()
            });
        }
    } catch (const std::exception& e) {
        spdlog::warn("LspCallGraphResolver: failed to read symbols for file_id={}: {}",
                     file_id, e.what());
        return 0;
    }

    if (symbols.empty()) return 0;

    std::string file_uri = path_to_uri(file_path);
    int dispatched = 0;

    for (const auto& sym : symbols) {
        std::string sym_name = sym.name;
        int sym_line = sym.line_start;

        bool sent = lsp_manager_.send_when_ready(language,
            [this, file_path, file_uri, file_id, sym_name, sym_line, language]() mutable {
                if (cancelled_.load(std::memory_order_relaxed)) return;

                lsp_manager_.ensure_document_open(language, file_path);

                LspTransport* transport = lsp_manager_.get_transport(language);
                if (!transport) return;

                nlohmann::json params;
                params["textDocument"]["uri"] = file_uri;
                // LSP is 0-indexed
                params["position"]["line"]      = sym_line > 0 ? sym_line - 1 : 0;
                params["position"]["character"] = 0;
                params["context"]["includeDeclaration"] = false;

                transport->send_request("textDocument/references", params,
                    [this, file_id, sym_name, sym_line](
                            const nlohmann::json& result,
                            const nlohmann::json& /*error*/) {
                        handle_references_response(result, file_id, sym_name, sym_line);
                    });
            });

        if (sent) dispatched++;
    }

    return dispatched;
}

void LspCallGraphResolver::resolve_file(
        const std::filesystem::path& file_path, int64_t file_id,
        const std::string& language) {
    resolve_callees(file_path, file_id, language);
    resolve_callers(file_path, file_id, language);
}

} // namespace codetldr
