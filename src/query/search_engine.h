#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <string>
#include <vector>

namespace codetldr {

/// A single ranked search result from the FTS5 index.
struct SearchResult {
    int64_t     symbol_id;
    std::string name;
    std::string kind;
    std::string signature;
    std::string documentation;
    std::string file_path;
    int         line_start;
    double      rank;
};

/// Full-text search engine over the symbols_fts FTS5 virtual table.
class SearchEngine {
public:
    /// Construct with a reference to an open SQLite::Database.
    /// The database must have migration v4 applied (symbols_fts table exists).
    explicit SearchEngine(SQLite::Database& db);

    /// Search across all symbols (name, signature, documentation) using FTS5 BM25 ranking.
    /// - Single-token queries without FTS operators get "*" appended for prefix search.
    /// - FTS metacharacters (", (, ), ^) are stripped from user input before querying.
    /// - Syntax errors in FTS queries return empty results (no crash).
    /// @param query  User-supplied search string.
    /// @param limit  Maximum number of results to return (default 20).
    /// @return Ranked results, best match first.
    /// @param query    User-supplied search string.
    /// @param language Optional language filter (e.g. "cpp", "python"). Empty = no filter.
    /// @param limit    Maximum number of results to return (default 20).
    /// @return Ranked results, best match first.
    std::vector<SearchResult> search_text(const std::string& query,
                                           const std::string& language = "",
                                           int limit = 20);

    /// Same as search_text but additionally filters by symbol kind and/or language.
    /// If kind is empty, no kind filter is applied.
    /// If language is empty, no language filter is applied.
    /// @param query    User-supplied search string (may be empty to match all of a kind).
    /// @param kind     Symbol kind to filter on (e.g. "function", "class", "method").
    /// @param language Optional language filter (e.g. "cpp", "python"). Empty = no filter.
    /// @param limit    Maximum number of results to return (default 20).
    /// @return Ranked results, best match first.
    std::vector<SearchResult> search_symbols(const std::string& query,
                                             const std::string& kind,
                                             const std::string& language = "",
                                             int limit = 20);

private:
    SQLite::Database& db_;

    /// Sanitize a user query for safe FTS5 input.
    /// - Strips metacharacters: " ( ) ^
    /// - Appends "*" for single-token queries that lack FTS operators
    std::string prepare_fts_query(const std::string& query);
};

} // namespace codetldr
