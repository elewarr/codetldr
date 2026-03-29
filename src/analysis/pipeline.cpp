// pipeline.cpp -- File-to-SQLite analysis pipeline.

#include "analysis/pipeline.h"
#include "analysis/tree_sitter/extractor.h"
#include "analysis/tree_sitter/language_registry.h"
#include "analysis/tree_sitter/parser.h"
#include "common/sha256.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace codetldr {

AnalysisResult analyze_file(SQLite::Database& db,
                              const LanguageRegistry& registry,
                              const std::filesystem::path& file_path) {
    // a. Read file content from disk
    if (!std::filesystem::exists(file_path)) {
        return {0, 0, 0, 0, false, "file not found: " + file_path.string()};
    }

    std::ifstream f(file_path);
    if (!f.is_open()) {
        return {0, 0, 0, 0, false, "cannot open file: " + file_path.string()};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    // b. Detect language from file extension
    std::string ext = file_path.extension().string();
    const auto* entry = registry.for_extension(ext);
    if (!entry) {
        return {0, 0, 0, 0, false, "unsupported extension: " + ext};
    }

    // b2. Content-based override: .h files may be Objective-C, not C.
    // Check for ObjC indicators in the first 4KB of content.
    if (entry->name == "c" && (ext == ".h" || ext == ".hpp")) {
        std::string head = content.substr(0, 4096);
        bool has_objc = false;
        // Check for common ObjC patterns
        if (head.find("@interface") != std::string::npos ||
            head.find("@implementation") != std::string::npos ||
            head.find("@property") != std::string::npos ||
            head.find("@protocol") != std::string::npos ||
            head.find("@class") != std::string::npos ||
            head.find("NSObject") != std::string::npos ||
            head.find("NSString") != std::string::npos) {
            // Verify with #import (ObjC uses #import, C uses #include)
            if (head.find("#import") != std::string::npos) {
                const auto* objc_entry = registry.for_name("objc");
                if (objc_entry) {
                    entry = objc_entry;
                }
            }
        }
    }

    // c. Upsert the file into the `files` table
    std::string hash = sha256_file(file_path);
    {
        SQLite::Statement upsert_file(db, R"sql(
            INSERT INTO files (path, language, mtime_ns, content_hash, indexed_at)
            VALUES (?, ?, 0, ?, datetime('now'))
            ON CONFLICT(path) DO UPDATE SET
                language     = excluded.language,
                mtime_ns     = excluded.mtime_ns,
                content_hash = excluded.content_hash,
                indexed_at   = excluded.indexed_at
        )sql");
        upsert_file.bind(1, file_path.string());
        upsert_file.bind(2, entry->name);
        if (!hash.empty()) upsert_file.bind(3, hash);
        else               upsert_file.bind(3);  // NULL if hash fails
        upsert_file.exec();
    }

    // Get file_id
    int64_t file_id = 0;
    {
        SQLite::Statement get_id(db, "SELECT id FROM files WHERE path = ?");
        get_id.bind(1, file_path.string());
        if (!get_id.executeStep()) {
            return {0, 0, 0, 0, false, "failed to get file_id for: " + file_path.string()};
        }
        file_id = get_id.getColumn(0).getInt64();
    }

    // d. Parse source with Tree-sitter
    auto tree = parse_source(entry->language, content);
    if (!tree) {
        return {0, 0, 0, 0, false, "parse failed for " + file_path.string()};
    }

    // e. Extract symbols, calls, and CFG nodes
    auto symbols = extract_symbols(tree.get(), entry->symbol_query.get(), content);
    auto calls   = extract_calls(tree.get(), entry->call_query.get(), content, symbols);

    // f2. Extract CFG nodes (if language supports it)
    std::vector<CfgNode> cfg_nodes_vec;
    if (entry->cfg_query) {
        cfg_nodes_vec = extract_cfg_nodes(tree.get(), entry->cfg_query.get(), content, symbols);
    }

    // f3. Extract DFG edges (if language supports it)
    std::vector<DfgEdge> dfg_edges_vec;
    if (entry->dfg_query) {
        dfg_edges_vec = extract_dfg_edges(tree.get(), entry->dfg_query.get(), content, symbols);
    }

    // f. Persist to SQLite inside a transaction (delete + re-insert = upsert semantics)
    try {
        SQLite::Transaction txn(db);

        // Check whether the symbols_fts FTS5 table exists (migration v4).
        // Guard so this code is safe on databases that haven't yet been migrated.
        bool has_fts = false;
        try {
            SQLite::Statement check(db, "SELECT 1 FROM symbols_fts LIMIT 0");
            (void)check;
            has_fts = true;
        } catch (...) {}

        // Delete stale FTS entries BEFORE deleting symbols (FTS content= does not auto-cascade).
        if (has_fts) {
            SQLite::Statement del_fts(db,
                "DELETE FROM symbols_fts WHERE rowid IN "
                "(SELECT id FROM symbols WHERE file_id = ?)");
            del_fts.bind(1, file_id);
            del_fts.exec();
        }

        // Delete existing calls for this file first (FK may not cascade on non-WAL connections)
        {
            SQLite::Statement del_calls(db, "DELETE FROM calls WHERE file_id = ?");
            del_calls.bind(1, file_id);
            del_calls.exec();
        }

        // Delete existing symbols (FK cascade on calls if FK enabled; belt+suspenders above)
        {
            SQLite::Statement del_syms(db, "DELETE FROM symbols WHERE file_id = ?");
            del_syms.bind(1, file_id);
            del_syms.exec();
        }

        // Insert symbols and build name -> id mapping for call resolution
        std::unordered_map<std::string, int64_t> sym_name_to_id;

        SQLite::Statement ins_sym(db, R"sql(
            INSERT INTO symbols(file_id, kind, name, signature, line_start, line_end, documentation)
            VALUES (?, ?, ?, ?, ?, ?, ?)
        )sql");

        for (const auto& sym : symbols) {
            ins_sym.reset();
            ins_sym.bind(1, file_id);
            ins_sym.bind(2, sym.kind);
            ins_sym.bind(3, sym.name);
            if (!sym.signature.empty()) ins_sym.bind(4, sym.signature);
            else                        ins_sym.bind(4);  // NULL
            ins_sym.bind(5, sym.line_start);
            ins_sym.bind(6, sym.line_end);
            if (!sym.documentation.empty()) ins_sym.bind(7, sym.documentation);
            else                            ins_sym.bind(7);  // NULL
            ins_sym.exec();
            sym_name_to_id[sym.name] = db.getLastInsertRowid();
        }

        // Sync FTS5 index for this file's new symbols (after all symbols inserted).
        if (has_fts) {
            SQLite::Statement ins_fts(db,
                "INSERT INTO symbols_fts(rowid, name, signature, documentation) "
                "SELECT id, name, COALESCE(signature,''), COALESCE(documentation,'') "
                "FROM symbols WHERE file_id = ?");
            ins_fts.bind(1, file_id);
            ins_fts.exec();
        }

        // Insert calls with resolved IDs where possible
        SQLite::Statement ins_call(db, R"sql(
            INSERT INTO calls(caller_id, callee_id, callee_name, file_id, line)
            VALUES (?, ?, ?, ?, ?)
        )sql");

        for (const auto& call : calls) {
            ins_call.reset();

            auto caller_it = sym_name_to_id.find(call.caller_name);
            if (caller_it != sym_name_to_id.end())
                ins_call.bind(1, caller_it->second);
            else
                ins_call.bind(1);  // NULL caller (top-level call)

            auto callee_it = sym_name_to_id.find(call.callee_name);
            if (callee_it != sym_name_to_id.end())
                ins_call.bind(2, callee_it->second);
            else
                ins_call.bind(2);  // NULL (unresolved / cross-file)

            ins_call.bind(3, call.callee_name);
            ins_call.bind(4, file_id);
            ins_call.bind(5, call.line);
            ins_call.exec();
        }

        // Delete stale CFG nodes for this file
        {
            SQLite::Statement del_cfg(db, "DELETE FROM cfg_nodes WHERE file_id = ?");
            del_cfg.bind(1, file_id);
            del_cfg.exec();
        }

        // Insert CFG nodes
        if (!cfg_nodes_vec.empty()) {
            SQLite::Statement ins_cfg(db, R"sql(
                INSERT INTO cfg_nodes(file_id, symbol_id, node_type, condition, line, depth)
                VALUES (?, ?, ?, ?, ?, ?)
            )sql");

            for (const auto& node : cfg_nodes_vec) {
                ins_cfg.reset();
                ins_cfg.bind(1, file_id);
                auto sym_it = sym_name_to_id.find(node.symbol_name);
                if (sym_it != sym_name_to_id.end())
                    ins_cfg.bind(2, sym_it->second);
                else
                    ins_cfg.bind(2);  // NULL (top-level control flow)
                ins_cfg.bind(3, node.node_type);
                if (!node.condition.empty()) ins_cfg.bind(4, node.condition);
                else                         ins_cfg.bind(4);  // NULL
                ins_cfg.bind(5, node.line);
                ins_cfg.bind(6, node.depth);
                ins_cfg.exec();
            }
        }

        // Delete stale DFG edges for this file
        {
            SQLite::Statement del_dfg(db, "DELETE FROM dfg_edges WHERE file_id = ?");
            del_dfg.bind(1, file_id);
            del_dfg.exec();
        }

        // Insert DFG edges
        if (!dfg_edges_vec.empty()) {
            SQLite::Statement ins_dfg(db, R"sql(
                INSERT INTO dfg_edges(file_id, symbol_id, edge_type, lhs, rhs_snippet, line)
                VALUES (?, ?, ?, ?, ?, ?)
            )sql");

            for (const auto& edge : dfg_edges_vec) {
                ins_dfg.reset();
                ins_dfg.bind(1, file_id);
                auto sym_it = sym_name_to_id.find(edge.symbol_name);
                if (sym_it != sym_name_to_id.end()) ins_dfg.bind(2, sym_it->second);
                else                                ins_dfg.bind(2);  // NULL
                ins_dfg.bind(3, edge.edge_type);
                ins_dfg.bind(4, edge.lhs);
                if (!edge.rhs_snippet.empty()) ins_dfg.bind(5, edge.rhs_snippet);
                else                           ins_dfg.bind(5);  // NULL for parameter edges
                ins_dfg.bind(6, edge.line);
                ins_dfg.exec();
            }
        }

        txn.commit();
    } catch (const SQLite::Exception& e) {
        return {0, 0, 0, 0, false, std::string("SQLite error: ") + e.what()};
    }

    return {static_cast<int>(symbols.size()), static_cast<int>(calls.size()),
            static_cast<int>(cfg_nodes_vec.size()), static_cast<int>(dfg_edges_vec.size()),
            true, {}, hash};
}

} // namespace codetldr
