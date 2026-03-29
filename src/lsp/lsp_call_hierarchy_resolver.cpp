#include "lsp/lsp_call_hierarchy_resolver.h"
#include "lsp/lsp_call_graph_resolver.h"
#include "lsp/lsp_manager.h"
#include "lsp/lsp_transport.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <string>
#include <unordered_set>

namespace codetldr {

LspCallHierarchyResolver::LspCallHierarchyResolver(SQLite::Database& db, LspManager& lsp_manager)
    : db_(db)
    , lsp_manager_(lsp_manager)
{}

LspCallHierarchyResolver::~LspCallHierarchyResolver() {
    cancelled_.store(true, std::memory_order_relaxed);
}

// static — delegate to LspCallGraphResolver for consistency
std::string LspCallHierarchyResolver::uri_to_path(const std::string& uri) {
    return LspCallGraphResolver::uri_to_path(uri);
}

// static — delegate to LspCallGraphResolver for consistency
std::string LspCallHierarchyResolver::path_to_uri(const std::filesystem::path& path) {
    return LspCallGraphResolver::path_to_uri(path);
}

int64_t LspCallHierarchyResolver::lookup_file_id(const std::string& path) {
    try {
        SQLite::Statement q(db_, "SELECT id FROM files WHERE path = ?");
        q.bind(1, path);
        if (q.executeStep()) {
            return q.getColumn(0).getInt64();
        }
    } catch (...) {}
    return -1;
}

void LspCallHierarchyResolver::handle_incoming_calls_response(
        const nlohmann::json& result,
        int64_t callee_file_id,
        const std::string& callee_name,
        int callee_line) {

    if (cancelled_.load(std::memory_order_relaxed)) return;
    if (!result.is_array()) return;

    for (const auto& incoming : result) {
        if (!incoming.is_object()) continue;

        // Extract caller info from "from" (CallHierarchyItem)
        const nlohmann::json* from = nullptr;
        if (incoming.contains("from") && incoming["from"].is_object()) {
            from = &incoming["from"];
        }
        if (!from) continue;

        std::string caller_name = from->value("name", "");
        // kind is an integer (SymbolKind): 12=function, 6=method, etc.
        // Store as string representation for readability
        std::string caller_kind;
        if (from->contains("kind") && (*from)["kind"].is_number()) {
            int kind_int = (*from)["kind"].get<int>();
            // Map common LSP SymbolKinds to string names
            switch (kind_int) {
                case 6:  caller_kind = "method";    break;
                case 9:  caller_kind = "constructor"; break;
                case 12: caller_kind = "function";  break;
                case 13: caller_kind = "variable";  break;
                default: caller_kind = std::to_string(kind_int); break;
            }
        } else if (from->contains("kind") && (*from)["kind"].is_string()) {
            caller_kind = (*from)["kind"].get<std::string>();
        }

        std::string caller_uri;
        if (from->contains("uri") && (*from)["uri"].is_string()) {
            caller_uri = (*from)["uri"].get<std::string>();
        }
        std::string caller_path = uri_to_path(caller_uri);

        int64_t caller_file_id = lookup_file_id(caller_path);

        // fromRanges: array of ranges where the call occurs in the caller
        const nlohmann::json* from_ranges = nullptr;
        if (incoming.contains("fromRanges") && incoming["fromRanges"].is_array()) {
            from_ranges = &incoming["fromRanges"];
        }

        if (from_ranges && !from_ranges->empty()) {
            // Insert one row per call-site range
            for (const auto& rng : *from_ranges) {
                if (!rng.is_object()) continue;
                int caller_line = 0;
                int caller_col  = 0;
                if (rng.contains("start") && rng["start"].is_object()) {
                    const auto& start = rng["start"];
                    // LSP is 0-indexed; convert to 1-indexed
                    caller_line = start.value("line", 0) + 1;
                    caller_col  = start.value("character", 0);
                }

                try {
                    SQLite::Statement ins(db_,
                        "INSERT INTO lsp_call_hierarchy_callers "
                        "(callee_file_id, callee_name, callee_line, "
                        " caller_name, caller_kind, caller_file_id, caller_file_path, "
                        " caller_line, caller_col) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
                    ins.bind(1, callee_file_id);
                    ins.bind(2, callee_name);
                    ins.bind(3, callee_line);
                    ins.bind(4, caller_name);
                    ins.bind(5, caller_kind);
                    if (caller_file_id >= 0) {
                        ins.bind(6, caller_file_id);
                    } else {
                        ins.bind(6); // NULL
                    }
                    ins.bind(7, caller_path);
                    ins.bind(8, caller_line);
                    ins.bind(9, caller_col);
                    ins.exec();
                } catch (const std::exception& e) {
                    spdlog::warn("LspCallHierarchyResolver: failed to insert caller for {}: {}",
                                 callee_name, e.what());
                }
            }
        } else {
            // No fromRanges — insert with null line/col
            try {
                SQLite::Statement ins(db_,
                    "INSERT INTO lsp_call_hierarchy_callers "
                    "(callee_file_id, callee_name, callee_line, "
                    " caller_name, caller_kind, caller_file_id, caller_file_path, "
                    " caller_line, caller_col) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, NULL, NULL)");
                ins.bind(1, callee_file_id);
                ins.bind(2, callee_name);
                ins.bind(3, callee_line);
                ins.bind(4, caller_name);
                ins.bind(5, caller_kind);
                if (caller_file_id >= 0) {
                    ins.bind(6, caller_file_id);
                } else {
                    ins.bind(6); // NULL
                }
                ins.bind(7, caller_path);
                ins.exec();
            } catch (const std::exception& e) {
                spdlog::warn("LspCallHierarchyResolver: failed to insert caller (no ranges) for {}: {}",
                             callee_name, e.what());
            }
        }
    }
}

int LspCallHierarchyResolver::resolve_incoming_callers(
        const std::filesystem::path& file_path, int64_t file_id,
        const std::string& language) {

    // KT-03: kotlin-language-server does not support callHierarchy.
    // Skip LSP path entirely — the get_incoming_callers handler in coordinator.cpp
    // falls back to Tree-sitter call_edges when LSP returns 0 dispatched queries.
    static const std::unordered_set<std::string> kNoCallHierarchy = {"kotlin", "ruby", "lua"};
    if (kNoCallHierarchy.count(language)) {
        spdlog::debug("LspCallHierarchyResolver: skipping callHierarchy for '{}' "
                      "(server does not support it)", language);
        return 0;
    }

    // Delete stale lsp_call_hierarchy_callers for this file before inserting new ones
    try {
        SQLite::Statement del(db_,
            "DELETE FROM lsp_call_hierarchy_callers WHERE callee_file_id = ?");
        del.bind(1, file_id);
        del.exec();
    } catch (const std::exception& e) {
        spdlog::warn("LspCallHierarchyResolver: failed to delete stale entries for file_id={}: {}",
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
        spdlog::warn("LspCallHierarchyResolver: failed to read symbols for file_id={}: {}",
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

                // Step 1: prepareCallHierarchy at symbol's definition position
                nlohmann::json params;
                params["textDocument"]["uri"] = file_uri;
                // LSP is 0-indexed
                params["position"]["line"]      = sym_line > 0 ? sym_line - 1 : 0;
                params["position"]["character"] = 0;

                transport->send_request("textDocument/prepareCallHierarchy", params,
                    [this, file_id, sym_name, sym_line, language](
                            const nlohmann::json& result,
                            const nlohmann::json& /*error*/) {
                        if (cancelled_.load(std::memory_order_relaxed)) return;
                        if (!result.is_array() || result.empty()) return;

                        // Take the first CallHierarchyItem (most specific match)
                        const nlohmann::json& item = result[0];

                        // Step 2: incomingCalls using the CallHierarchyItem verbatim
                        // (including opaque data field)
                        nlohmann::json incoming_params;
                        incoming_params["item"] = item;

                        // Use send_when_ready for step 2 — do NOT call transport directly
                        lsp_manager_.send_when_ready(language,
                            [this, file_id, sym_name, sym_line, incoming_params, language]() mutable {
                                if (cancelled_.load(std::memory_order_relaxed)) return;

                                LspTransport* t2 = lsp_manager_.get_transport(language);
                                if (!t2) return;

                                t2->send_request("callHierarchy/incomingCalls", incoming_params,
                                    [this, file_id, sym_name, sym_line](
                                            const nlohmann::json& calls_result,
                                            const nlohmann::json& /*error*/) {
                                        handle_incoming_calls_response(
                                            calls_result, file_id, sym_name, sym_line);
                                    });
                            });
                    });
            });

        if (sent) dispatched++;
    }

    return dispatched;
}

} // namespace codetldr
