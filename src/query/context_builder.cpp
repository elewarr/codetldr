#include "query/context_builder.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <map>
#include <sstream>

namespace codetldr {

ContextBuilder::ContextBuilder(SQLite::Database& db) : db_(db) {}

ContextResponse ContextBuilder::build(const ContextRequest& req) {
    // Collect symbols grouped by file path
    // Key: file_path, Value: list of symbol rows
    std::map<std::string, std::vector<SymbolRow>> file_symbols;

    if (req.format == ContextFormat::kDiffAware) {
        // Only include symbols from changed_paths
        if (req.changed_paths.empty()) {
            ContextResponse resp;
            resp.estimated_tokens = 0;
            return resp;
        }

        for (const auto& changed_path : req.changed_paths) {
            SQLite::Statement q(db_,
                "SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), "
                "       COALESCE(s.documentation,''), s.line_start, s.line_end "
                "FROM symbols s "
                "JOIN files f ON f.id = s.file_id "
                "WHERE f.path = ? "
                "ORDER BY s.line_start");
            q.bind(1, changed_path);

            while (q.executeStep()) {
                SymbolRow row;
                row.id            = q.getColumn(0).getInt64();
                row.name          = q.getColumn(1).getString();
                row.kind          = q.getColumn(2).getString();
                row.signature     = q.getColumn(3).getString();
                row.documentation = q.getColumn(4).getString();
                row.line_start    = q.getColumn(5).getInt();
                row.line_end      = q.getColumn(6).getInt();
                file_symbols[changed_path].push_back(std::move(row));
            }
        }
    } else if (!req.file_paths.empty()) {
        // Explicit file list
        for (const auto& file_path : req.file_paths) {
            SQLite::Statement q(db_,
                "SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), "
                "       COALESCE(s.documentation,''), s.line_start, s.line_end "
                "FROM symbols s "
                "JOIN files f ON f.id = s.file_id "
                "WHERE f.path = ? "
                "ORDER BY s.line_start");
            q.bind(1, file_path);

            while (q.executeStep()) {
                SymbolRow row;
                row.id            = q.getColumn(0).getInt64();
                row.name          = q.getColumn(1).getString();
                row.kind          = q.getColumn(2).getString();
                row.signature     = q.getColumn(3).getString();
                row.documentation = q.getColumn(4).getString();
                row.line_start    = q.getColumn(5).getInt();
                row.line_end      = q.getColumn(6).getInt();
                file_symbols[file_path].push_back(std::move(row));
            }
        }
    } else {
        // All files, capped by max_symbols
        SQLite::Statement q(db_,
            "SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), "
            "       COALESCE(s.documentation,''), s.line_start, s.line_end, f.path "
            "FROM symbols s "
            "JOIN files f ON f.id = s.file_id "
            "ORDER BY f.path, s.line_start "
            "LIMIT ?");
        q.bind(1, req.max_symbols);

        while (q.executeStep()) {
            SymbolRow row;
            row.id            = q.getColumn(0).getInt64();
            row.name          = q.getColumn(1).getString();
            row.kind          = q.getColumn(2).getString();
            row.signature     = q.getColumn(3).getString();
            row.documentation = q.getColumn(4).getString();
            row.line_start    = q.getColumn(5).getInt();
            row.line_end      = q.getColumn(6).getInt();
            std::string path  = q.getColumn(7).getString();
            file_symbols[path].push_back(std::move(row));
        }
    }

    // Handle explicit file_paths that had no symbols — still output their header
    if (!req.file_paths.empty() && req.format != ContextFormat::kDiffAware) {
        for (const auto& fp : req.file_paths) {
            if (file_symbols.find(fp) == file_symbols.end()) {
                // Verify file exists in DB
                SQLite::Statement check(db_, "SELECT 1 FROM files WHERE path = ? LIMIT 1");
                check.bind(1, fp);
                if (check.executeStep()) {
                    file_symbols[fp] = {};  // empty symbol list
                }
            }
        }
    }

    // Build the output text
    std::string text;
    int total_symbols = 0;
    int file_count = 0;

    for (const auto& [file_path, symbols] : file_symbols) {
        ++file_count;
        total_symbols += static_cast<int>(symbols.size());

        if (req.format == ContextFormat::kDetailed) {
            text += format_detailed(file_path, symbols);
        } else {
            // kCondensed and kDiffAware both use condensed format
            text += format_condensed(file_path, symbols);
        }
    }

    ContextResponse resp;
    resp.text             = std::move(text);
    resp.symbol_count     = total_symbols;
    resp.file_count       = file_count;
    resp.estimated_tokens = static_cast<int>(resp.text.size()) / 4;
    return resp;
}

std::string ContextBuilder::format_condensed(const std::string& file_path,
                                              const std::vector<SymbolRow>& symbols) {
    std::string out;
    out += "[FILE] " + file_path + " (" + std::to_string(symbols.size()) + " symbols)\n";
    for (const auto& sym : symbols) {
        out += "  " + sym.kind + " " + sym.name;
        if (!sym.signature.empty() && sym.signature != sym.name) {
            // Append signature after name if it provides more info
            out += " -- " + sym.signature;
        }
        out += "\n";
    }
    return out;
}

std::string ContextBuilder::format_detailed(const std::string& file_path,
                                             const std::vector<SymbolRow>& symbols) {
    std::string out;
    out += "[FILE] " + file_path + "\n";
    for (const auto& sym : symbols) {
        out += "  [" + sym.kind + "] " + sym.name + "\n";
        out += "    signature: " + sym.signature + "\n";

        // Truncate documentation to first 200 chars
        std::string doc = sym.documentation;
        if (doc.size() > 200) {
            doc = doc.substr(0, 200) + "...";
        }
        out += "    doc: " + doc + "\n";

        // Callees
        auto callees = get_callee_names(sym.id);
        out += "    calls: ";
        for (size_t i = 0; i < callees.size(); ++i) {
            if (i > 0) out += ", ";
            out += callees[i];
        }
        out += "\n";

        // Callers
        auto callers = get_caller_names(sym.id);
        out += "    called_by: ";
        for (size_t i = 0; i < callers.size(); ++i) {
            if (i > 0) out += ", ";
            out += callers[i];
        }
        out += "\n";

        out += "    lines: " + std::to_string(sym.line_start) + "-" + std::to_string(sym.line_end) + "\n";
    }
    return out;
}

SymbolInfo ContextBuilder::find_symbol(const std::string& name,
                                        const std::string& file_path) {
    SymbolInfo info;
    info.found = false;

    if (file_path.empty()) {
        SQLite::Statement q(db_,
            "SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), "
            "       COALESCE(s.documentation,''), f.path, s.line_start, s.line_end "
            "FROM symbols s "
            "JOIN files f ON f.id = s.file_id "
            "WHERE s.name = ? "
            "LIMIT 1");
        q.bind(1, name);
        if (q.executeStep()) {
            info.id            = q.getColumn(0).getInt64();
            info.name          = q.getColumn(1).getString();
            info.kind          = q.getColumn(2).getString();
            info.signature     = q.getColumn(3).getString();
            info.documentation = q.getColumn(4).getString();
            info.file_path     = q.getColumn(5).getString();
            info.line_start    = q.getColumn(6).getInt();
            info.line_end      = q.getColumn(7).getInt();
            info.found = true;
        }
    } else {
        SQLite::Statement q(db_,
            "SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), "
            "       COALESCE(s.documentation,''), f.path, s.line_start, s.line_end "
            "FROM symbols s "
            "JOIN files f ON f.id = s.file_id "
            "WHERE s.name = ? AND f.path = ? "
            "LIMIT 1");
        q.bind(1, name);
        q.bind(2, file_path);
        if (q.executeStep()) {
            info.id            = q.getColumn(0).getInt64();
            info.name          = q.getColumn(1).getString();
            info.kind          = q.getColumn(2).getString();
            info.signature     = q.getColumn(3).getString();
            info.documentation = q.getColumn(4).getString();
            info.file_path     = q.getColumn(5).getString();
            info.line_start    = q.getColumn(6).getInt();
            info.line_end      = q.getColumn(7).getInt();
            info.found = true;
        }
    }

    return info;
}

std::vector<std::string> ContextBuilder::get_callee_names(int64_t symbol_id) {
    std::vector<std::string> names;
    SQLite::Statement q(db_,
        "SELECT callee_name FROM calls WHERE caller_id = ?");
    q.bind(1, symbol_id);
    while (q.executeStep()) {
        names.push_back(q.getColumn(0).getString());
    }
    return names;
}

std::vector<std::string> ContextBuilder::get_caller_names(int64_t symbol_id) {
    std::vector<std::string> names;
    SQLite::Statement q(db_,
        "SELECT s.name FROM calls c JOIN symbols s ON s.id = c.caller_id "
        "WHERE c.callee_name = (SELECT name FROM symbols WHERE id = ?)");
    q.bind(1, symbol_id);
    while (q.executeStep()) {
        names.push_back(q.getColumn(0).getString());
    }
    return names;
}

} // namespace codetldr
