// pipeline.cpp -- File-to-SQLite analysis pipeline.

#include "analysis/pipeline.h"
#include "analysis/tree_sitter/extractor.h"
#include "analysis/tree_sitter/language_registry.h"
#include "analysis/tree_sitter/parser.h"
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
        return {0, 0, false, "file not found: " + file_path.string()};
    }

    std::ifstream f(file_path);
    if (!f.is_open()) {
        return {0, 0, false, "cannot open file: " + file_path.string()};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    // b. Detect language from file extension
    std::string ext = file_path.extension().string();
    const auto* entry = registry.for_extension(ext);
    if (!entry) {
        return {0, 0, false, "unsupported extension: " + ext};
    }

    // c. Upsert the file into the `files` table
    {
        SQLite::Statement upsert_file(db, R"sql(
            INSERT INTO files (path, language, mtime_ns, indexed_at)
            VALUES (?, ?, 0, datetime('now'))
            ON CONFLICT(path) DO UPDATE SET
                language   = excluded.language,
                mtime_ns   = excluded.mtime_ns,
                indexed_at = excluded.indexed_at
        )sql");
        upsert_file.bind(1, file_path.string());
        upsert_file.bind(2, entry->name);
        upsert_file.exec();
    }

    // Get file_id
    int64_t file_id = 0;
    {
        SQLite::Statement get_id(db, "SELECT id FROM files WHERE path = ?");
        get_id.bind(1, file_path.string());
        if (!get_id.executeStep()) {
            return {0, 0, false, "failed to get file_id for: " + file_path.string()};
        }
        file_id = get_id.getColumn(0).getInt64();
    }

    // d. Parse source with Tree-sitter
    auto tree = parse_source(entry->language, content);
    if (!tree) {
        return {0, 0, false, "parse failed for " + file_path.string()};
    }

    // e. Extract symbols and calls
    auto symbols = extract_symbols(tree.get(), entry->symbol_query.get(), content);
    auto calls   = extract_calls(tree.get(), entry->call_query.get(), content, symbols);

    // f. Persist to SQLite inside a transaction (delete + re-insert = upsert semantics)
    try {
        SQLite::Transaction txn(db);

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

        txn.commit();
    } catch (const SQLite::Exception& e) {
        return {0, 0, false, std::string("SQLite error: ") + e.what()};
    }

    return {static_cast<int>(symbols.size()), static_cast<int>(calls.size()), true, {}};
}

} // namespace codetldr
