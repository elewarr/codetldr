// test_fts5_search.cpp -- FTS5 full-text search integration tests.
// Uses assert() + return 0/1 pattern consistent with existing tests (no test framework).

#include "storage/database.h"
#include "query/search_engine.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace codetldr;

static void insert_test_data(SQLite::Database& db) {
    // Insert a test file
    db.exec("INSERT INTO files(path, language, mtime_ns) VALUES('/test/foo.cpp', 'cpp', 0)");
    int64_t file_id = db.getLastInsertRowid();

    // Insert several symbols with varied names/kinds/signatures/docs
    SQLite::Statement ins(db,
        "INSERT INTO symbols(file_id, kind, name, signature, line_start, line_end, documentation) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");

    auto insert_sym = [&](const std::string& kind, const std::string& name,
                          const std::string& sig, const std::string& doc,
                          int line) {
        ins.reset();
        ins.bind(1, file_id);
        ins.bind(2, kind);
        ins.bind(3, name);
        ins.bind(4, sig);
        ins.bind(5, line);
        ins.bind(6, line + 10);
        ins.bind(7, doc);
        ins.exec();
    };

    insert_sym("function", "analyze_file",   "AnalysisResult analyze_file(db, registry, path)", "Analyzes a source file and upserts symbols.", 1);
    insert_sym("function", "parse_source",   "Tree* parse_source(lang, content)",                "Parses source code using Tree-sitter.", 20);
    insert_sym("class",    "SearchEngine",   "class SearchEngine",                               "Full-text search over indexed symbols.", 40);
    insert_sym("method",   "search_text",    "vector<SearchResult> search_text(query, limit)",   "Executes FTS5 MATCH query with BM25 ranking.", 55);
    insert_sym("function", "extract_calls",  "vector<Call> extract_calls(tree, query, src, syms)", "Extracts function call sites from AST.", 80);
    insert_sym("class",    "LanguageRegistry","class LanguageRegistry",                          "Maps file extensions to language definitions.", 100);
}

static void populate_fts(SQLite::Database& db) {
    db.exec(
        "INSERT INTO symbols_fts(rowid, name, signature, documentation) "
        "SELECT id, name, COALESCE(signature,''), COALESCE(documentation,'') FROM symbols"
    );
}

int main() {
    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_fts5";
    fs::remove_all(test_dir);

    const fs::path db_path = test_dir / "index.sqlite";
    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    // Test 1: After Database::open(), symbols_fts table exists (migration v4 applied)
    {
        SQLite::Statement q(db, "SELECT 1 FROM symbols_fts LIMIT 0");
        bool ok = true;  // no throw means table exists
        (void)ok;
        assert(db_obj.schema_version() >= 4);
        std::cout << "PASS: symbols_fts table exists (migration v4)\n";
    }

    // Insert test data
    insert_test_data(db);
    populate_fts(db);

    SearchEngine engine(db);

    // Test 2: Search by exact name returns the symbol
    {
        auto results = engine.search_text("analyze_file");
        assert(!results.empty());
        assert(results[0].name == "analyze_file");
        std::cout << "PASS: exact name search returns correct symbol\n";
    }

    // Test 3: Prefix query "ana*" matches "analyze_file"
    {
        // "ana" prefix should match "analyze_file" (prefix='2 3' covers 2 and 3 char prefixes)
        // But we use "ana*" explicit prefix in search_text for single-token queries
        auto results = engine.search_text("ana");
        // single token without FTS operators gets "*" appended for prefix search
        bool found_analyze = false;
        for (const auto& r : results) {
            if (r.name == "analyze_file") { found_analyze = true; break; }
        }
        assert(found_analyze);
        std::cout << "PASS: prefix query 'ana' matches 'analyze_file'\n";
    }

    // Test 4: BM25 ranking: more specific match ranks higher
    // "SearchEngine" appears in name (most specific), so it should rank above a less-specific hit
    {
        auto results = engine.search_text("SearchEngine");
        assert(!results.empty());
        // The symbol named exactly "SearchEngine" should be first or very near top
        assert(results[0].name == "SearchEngine");
        std::cout << "PASS: BM25 ranking puts exact name match first\n";
    }

    // Test 5: Search with limit=1 returns exactly 1 result
    {
        auto results = engine.search_text("search", "", 1);
        assert(results.size() == 1);
        std::cout << "PASS: limit=1 returns exactly 1 result\n";
    }

    // Test 6: Search for nonexistent term returns empty vector
    {
        auto results = engine.search_text("nonexistent_xyz_term_12345");
        assert(results.empty());
        std::cout << "PASS: nonexistent term returns empty results\n";
    }

    // Test 7: FTS metacharacters in query do not crash
    {
        // "operator+" contains a '+' which is not a metachar in FTS5, but '(' ')' '"' '^' are stripped
        auto results = engine.search_text("operator+");
        // should return empty or partial, not crash
        std::cout << "PASS: FTS metacharacters do not crash (got " << results.size() << " results)\n";
    }

    // Test 7b: Parentheses and quotes in query do not crash
    {
        auto results = engine.search_text("\"analyze(file)\"");
        std::cout << "PASS: parens/quotes in query sanitized without crash (got " << results.size() << " results)\n";
    }

    // Test 8: search_symbols with kind filter returns only symbols of that kind
    {
        auto results = engine.search_symbols("", "class");
        assert(!results.empty());
        for (const auto& r : results) {
            assert(r.kind == "class");
        }
        std::cout << "PASS: kind filter returns only 'class' symbols (" << results.size() << " found)\n";
    }

    // Test 8b: search_symbols with query + kind filter
    {
        auto results = engine.search_symbols("search", "method");
        assert(!results.empty());
        for (const auto& r : results) {
            assert(r.kind == "method");
        }
        std::cout << "PASS: search_symbols with query+kind filter works\n";
    }

    // Timing test: search_text completes in under 50ms
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; ++i) {
            engine.search_text("analyze");
        }
        auto t1 = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        // 10 searches under 500ms means each is < 50ms on average
        assert(ms < 500);
        std::cout << "PASS: 10 searches completed in " << ms << "ms (avg < 50ms)\n";
    }

    // --- Task 2: FTS sync test ---
    // Test: Simulate re-analysis (delete old FTS+symbols, insert new symbols+FTS)
    // Verify stale entries gone, new entries found.
    {
        // Insert a new file for sync test
        db.exec("INSERT INTO files(path, language, mtime_ns) VALUES('/test/sync_test.cpp', 'cpp', 0)");
        int64_t sync_file_id = db.getLastInsertRowid();

        // Insert initial symbol "old_function"
        {
            SQLite::Statement ins(db,
                "INSERT INTO symbols(file_id, kind, name, signature, line_start, line_end) "
                "VALUES (?, 'function', 'old_function', 'void old_function()', 1, 10)");
            ins.bind(1, sync_file_id);
            ins.exec();
        }
        // Populate FTS for this file
        {
            SQLite::Statement ins_fts(db,
                "INSERT INTO symbols_fts(rowid, name, signature, documentation) "
                "SELECT id, name, COALESCE(signature,''), COALESCE(documentation,'') "
                "FROM symbols WHERE file_id = ?");
            ins_fts.bind(1, sync_file_id);
            ins_fts.exec();
        }

        // Verify "old_function" is searchable
        {
            auto results = engine.search_text("old_function");
            bool found = false;
            for (const auto& r : results) {
                if (r.name == "old_function") { found = true; break; }
            }
            assert(found);
            std::cout << "PASS: initial 'old_function' is searchable\n";
        }

        // Simulate re-analysis: delete old FTS entries first, then delete symbols, then re-insert
        SQLite::Transaction txn(db);

        // Delete stale FTS entries (must happen BEFORE symbols delete)
        {
            SQLite::Statement del_fts(db,
                "DELETE FROM symbols_fts WHERE rowid IN "
                "(SELECT id FROM symbols WHERE file_id = ?)");
            del_fts.bind(1, sync_file_id);
            del_fts.exec();
        }

        // Delete old symbols
        {
            SQLite::Statement del_syms(db, "DELETE FROM symbols WHERE file_id = ?");
            del_syms.bind(1, sync_file_id);
            del_syms.exec();
        }

        // Insert new symbol "new_function"
        {
            SQLite::Statement ins(db,
                "INSERT INTO symbols(file_id, kind, name, signature, line_start, line_end) "
                "VALUES (?, 'function', 'new_function', 'void new_function()', 1, 10)");
            ins.bind(1, sync_file_id);
            ins.exec();
        }

        // Populate FTS with new symbols
        {
            SQLite::Statement ins_fts(db,
                "INSERT INTO symbols_fts(rowid, name, signature, documentation) "
                "SELECT id, name, COALESCE(signature,''), COALESCE(documentation,'') "
                "FROM symbols WHERE file_id = ?");
            ins_fts.bind(1, sync_file_id);
            ins_fts.exec();
        }

        txn.commit();

        // Verify "new_function" is found, "old_function" is not
        {
            auto new_results = engine.search_text("new_function");
            bool found_new = false;
            for (const auto& r : new_results) {
                if (r.name == "new_function") { found_new = true; break; }
            }
            assert(found_new);
            std::cout << "PASS: 'new_function' is searchable after sync\n";
        }
        {
            auto old_results = engine.search_text("old_function");
            bool found_old = false;
            for (const auto& r : old_results) {
                if (r.name == "old_function") { found_old = true; break; }
            }
            assert(!found_old);
            std::cout << "PASS: stale 'old_function' not found after FTS sync\n";
        }
    }

    fs::remove_all(test_dir);
    std::cout << "All FTS5 search tests passed.\n";
    return 0;
}
