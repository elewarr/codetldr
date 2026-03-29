// search_engine.cpp -- FTS5 search over indexed symbols using BM25 ranking.

#include "query/search_engine.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace codetldr {

SearchEngine::SearchEngine(SQLite::Database& db) : db_(db) {}

std::string SearchEngine::prepare_fts_query(const std::string& query) {
    if (query.empty()) {
        return query;
    }

    // Strip FTS5 metacharacters that could cause syntax errors:  "  (  )  ^
    std::string sanitized;
    sanitized.reserve(query.size());
    for (char c : query) {
        if (c != '"' && c != '(' && c != ')' && c != '^') {
            sanitized += c;
        }
    }

    // Trim leading/trailing whitespace
    auto start = sanitized.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    auto end = sanitized.find_last_not_of(" \t\n\r");
    sanitized = sanitized.substr(start, end - start + 1);

    if (sanitized.empty()) return "";

    // Determine if the sanitized query is a single token without FTS operators.
    // FTS5 operators that indicate a compound query: AND, OR, NOT, *, { }
    bool has_spaces      = sanitized.find(' ')  != std::string::npos;
    bool has_fts_ops     = sanitized.find('*')  != std::string::npos
                        || sanitized.find('{')  != std::string::npos
                        || sanitized.find('}')  != std::string::npos;

    // Detect keyword operators (case-insensitive) only when separated by spaces
    bool has_kw_ops = false;
    if (has_spaces) {
        std::string upper = sanitized;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        has_kw_ops = (upper.find(" AND ") != std::string::npos)
                  || (upper.find(" OR ")  != std::string::npos)
                  || (upper.find(" NOT ") != std::string::npos);
    }

    // For single-token queries without any operator, append "*" for prefix search.
    if (!has_spaces && !has_fts_ops) {
        sanitized += "*";
    } else if (has_spaces && !has_fts_ops && !has_kw_ops) {
        // Multi-word with no operators: treat as a phrase with prefix on last token
        // Append "*" to allow prefix on the last word
        sanitized += "*";
    }

    return sanitized;
}

std::vector<SearchResult> SearchEngine::search_text(const std::string& query, int limit) {
    // First try FTS5 symbol search
    auto results = search_symbols(query, "", limit);
    if (!results.empty()) return results;

    // FTS fallback: substring match on symbol names and documentation
    if (query.empty()) return results;

    try {
        std::string like_pattern = "%" + query + "%";
        SQLite::Statement stmt(db_, R"sql(
            SELECT s.id, s.name, s.kind, COALESCE(s.signature,''),
                   COALESCE(s.documentation,''), f.path, s.line_start, 0.0
            FROM symbols s
            JOIN files f ON f.id = s.file_id
            WHERE s.name LIKE ? OR COALESCE(s.documentation,'') LIKE ?
            ORDER BY s.name
            LIMIT ?
        )sql");
        stmt.bind(1, like_pattern);
        stmt.bind(2, like_pattern);
        stmt.bind(3, limit);
        while (stmt.executeStep()) {
            SearchResult r;
            r.symbol_id    = stmt.getColumn(0).getInt64();
            r.name         = stmt.getColumn(1).getText();
            r.kind         = stmt.getColumn(2).getText();
            r.signature    = stmt.getColumn(3).getText();
            r.documentation= stmt.getColumn(4).getText();
            r.file_path    = stmt.getColumn(5).getText();
            r.line_start   = stmt.getColumn(6).getInt();
            r.rank         = stmt.getColumn(7).getDouble();
            results.push_back(std::move(r));
        }
    } catch (const SQLite::Exception&) {
        return {};
    }

    return results;
}

std::vector<SearchResult> SearchEngine::search_symbols(const std::string& query,
                                                        const std::string& kind,
                                                        int limit) {
    std::vector<SearchResult> results;

    std::string fts_query = prepare_fts_query(query);

    // If no query and no kind filter, nothing to search.
    if (fts_query.empty() && kind.empty()) {
        return results;
    }

    try {
        if (!fts_query.empty() && !kind.empty()) {
            // FTS MATCH + kind filter
            SQLite::Statement stmt(db_, R"sql(
                SELECT s.id, s.name, s.kind, COALESCE(s.signature,''),
                       COALESCE(s.documentation,''), f.path, s.line_start, sf.rank
                FROM symbols_fts sf
                JOIN symbols s ON s.id = sf.rowid
                JOIN files   f ON f.id = s.file_id
                WHERE symbols_fts MATCH ?
                  AND s.kind = ?
                ORDER BY rank
                LIMIT ?
            )sql");
            stmt.bind(1, fts_query);
            stmt.bind(2, kind);
            stmt.bind(3, limit);
            while (stmt.executeStep()) {
                SearchResult r;
                r.symbol_id    = stmt.getColumn(0).getInt64();
                r.name         = stmt.getColumn(1).getText();
                r.kind         = stmt.getColumn(2).getText();
                r.signature    = stmt.getColumn(3).getText();
                r.documentation= stmt.getColumn(4).getText();
                r.file_path    = stmt.getColumn(5).getText();
                r.line_start   = stmt.getColumn(6).getInt();
                r.rank         = stmt.getColumn(7).getDouble();
                results.push_back(std::move(r));
            }
        } else if (!fts_query.empty()) {
            // FTS MATCH only
            SQLite::Statement stmt(db_, R"sql(
                SELECT s.id, s.name, s.kind, COALESCE(s.signature,''),
                       COALESCE(s.documentation,''), f.path, s.line_start, sf.rank
                FROM symbols_fts sf
                JOIN symbols s ON s.id = sf.rowid
                JOIN files   f ON f.id = s.file_id
                WHERE symbols_fts MATCH ?
                ORDER BY rank
                LIMIT ?
            )sql");
            stmt.bind(1, fts_query);
            stmt.bind(2, limit);
            while (stmt.executeStep()) {
                SearchResult r;
                r.symbol_id    = stmt.getColumn(0).getInt64();
                r.name         = stmt.getColumn(1).getText();
                r.kind         = stmt.getColumn(2).getText();
                r.signature    = stmt.getColumn(3).getText();
                r.documentation= stmt.getColumn(4).getText();
                r.file_path    = stmt.getColumn(5).getText();
                r.line_start   = stmt.getColumn(6).getInt();
                r.rank         = stmt.getColumn(7).getDouble();
                results.push_back(std::move(r));
            }
        } else {
            // kind filter only — no FTS, just a regular query
            SQLite::Statement stmt(db_, R"sql(
                SELECT s.id, s.name, s.kind, COALESCE(s.signature,''),
                       COALESCE(s.documentation,''), f.path, s.line_start, 0.0
                FROM symbols s
                JOIN files   f ON f.id = s.file_id
                WHERE s.kind = ?
                LIMIT ?
            )sql");
            stmt.bind(1, kind);
            stmt.bind(2, limit);
            while (stmt.executeStep()) {
                SearchResult r;
                r.symbol_id    = stmt.getColumn(0).getInt64();
                r.name         = stmt.getColumn(1).getText();
                r.kind         = stmt.getColumn(2).getText();
                r.signature    = stmt.getColumn(3).getText();
                r.documentation= stmt.getColumn(4).getText();
                r.file_path    = stmt.getColumn(5).getText();
                r.line_start   = stmt.getColumn(6).getInt();
                r.rank         = stmt.getColumn(7).getDouble();
                results.push_back(std::move(r));
            }
        }
    } catch (const SQLite::Exception&) {
        // FTS syntax error or other SQLite error — return empty results gracefully
        return {};
    }

    // LIKE fallback: when FTS finds nothing and query is non-empty, try substring
    // match. This helps with camelCase components (e.g., "ViewController" finding
    // "HomeViewController") that FTS5's unicode61 tokenizer can't split.
    if (results.empty() && !query.empty() && !kind.empty()) {
        try {
            std::string like_pattern = "%" + query + "%";
            SQLite::Statement stmt(db_, R"sql(
                SELECT s.id, s.name, s.kind, COALESCE(s.signature,''),
                       COALESCE(s.documentation,''), f.path, s.line_start, 0.0
                FROM symbols s
                JOIN files f ON f.id = s.file_id
                WHERE s.kind = ? AND s.name LIKE ?
                ORDER BY s.name
                LIMIT ?
            )sql");
            stmt.bind(1, kind);
            stmt.bind(2, like_pattern);
            stmt.bind(3, limit);
            while (stmt.executeStep()) {
                SearchResult r;
                r.symbol_id    = stmt.getColumn(0).getInt64();
                r.name         = stmt.getColumn(1).getText();
                r.kind         = stmt.getColumn(2).getText();
                r.signature    = stmt.getColumn(3).getText();
                r.documentation= stmt.getColumn(4).getText();
                r.file_path    = stmt.getColumn(5).getText();
                r.line_start   = stmt.getColumn(6).getInt();
                r.rank         = stmt.getColumn(7).getDouble();
                results.push_back(std::move(r));
            }
        } catch (...) {}
    } else if (results.empty() && !query.empty()) {
        try {
            std::string like_pattern = "%" + query + "%";
            SQLite::Statement stmt(db_, R"sql(
                SELECT s.id, s.name, s.kind, COALESCE(s.signature,''),
                       COALESCE(s.documentation,''), f.path, s.line_start, 0.0
                FROM symbols s
                JOIN files f ON f.id = s.file_id
                WHERE s.name LIKE ?
                ORDER BY s.name
                LIMIT ?
            )sql");
            stmt.bind(1, like_pattern);
            stmt.bind(2, limit);
            while (stmt.executeStep()) {
                SearchResult r;
                r.symbol_id    = stmt.getColumn(0).getInt64();
                r.name         = stmt.getColumn(1).getText();
                r.kind         = stmt.getColumn(2).getText();
                r.signature    = stmt.getColumn(3).getText();
                r.documentation= stmt.getColumn(4).getText();
                r.file_path    = stmt.getColumn(5).getText();
                r.line_start   = stmt.getColumn(6).getInt();
                r.rank         = stmt.getColumn(7).getDouble();
                results.push_back(std::move(r));
            }
        } catch (...) {}
    }

    return results;
}

} // namespace codetldr
