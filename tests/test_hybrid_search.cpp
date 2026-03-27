// test_hybrid_search.cpp -- Unit tests for HybridSearchEngine.
// Tests FTS5-only path (model=nullptr, store=nullptr) and rrf_merge() directly.
// Uses assert() + stdout pattern consistent with other tests (no framework).

#include "storage/database.h"
#include "query/hybrid_search_engine.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace codetldr;

// Insert N symbols into a test database.
static int64_t insert_file(SQLite::Database& db, const std::string& path,
                            const std::string& language) {
    SQLite::Statement q(db,
        "INSERT INTO files(path, language, mtime_ns) VALUES(?, ?, 0)");
    q.bind(1, path);
    q.bind(2, language);
    q.exec();
    return db.getLastInsertRowid();
}

static int64_t insert_symbol(SQLite::Database& db, int64_t file_id,
                              const std::string& kind, const std::string& name,
                              const std::string& sig, const std::string& doc,
                              int line) {
    SQLite::Statement q(db,
        "INSERT INTO symbols(file_id, kind, name, signature, line_start, line_end, documentation) "
        "VALUES(?, ?, ?, ?, ?, ?, ?)");
    q.bind(1, file_id);
    q.bind(2, kind);
    q.bind(3, name);
    q.bind(4, sig);
    q.bind(5, line);
    q.bind(6, line + 10);
    q.bind(7, doc);
    q.exec();
    return db.getLastInsertRowid();
}

static void populate_fts(SQLite::Database& db) {
    db.exec(
        "INSERT INTO symbols_fts(rowid, name, signature, documentation) "
        "SELECT id, name, COALESCE(signature,''), COALESCE(documentation,'') FROM symbols"
    );
}

static void insert_test_data(SQLite::Database& db) {
    int64_t cpp_file = insert_file(db, "/src/analyzer.cpp", "cpp");
    int64_t py_file  = insert_file(db, "/src/analyzer.py",  "python");

    // C++ symbols
    insert_symbol(db, cpp_file, "function", "analyze_file",
                  "AnalysisResult analyze_file(db, path)",
                  "Analyzes a source file and indexes symbols.", 10);
    insert_symbol(db, cpp_file, "class", "SearchEngine",
                  "class SearchEngine",
                  "Full-text search over symbols.", 50);
    insert_symbol(db, cpp_file, "function", "extract_calls",
                  "vector<Call> extract_calls(tree, src)",
                  "Extracts function call sites from AST.", 100);

    // Python symbols
    insert_symbol(db, py_file, "function", "analyze_python",
                  "def analyze_python(path)",
                  "Analyzes a Python source file.", 5);
    insert_symbol(db, py_file, "class", "QueryEngine",
                  "class QueryEngine",
                  "Query engine for Python symbol index.", 30);
}

int main() {
    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_hybrid";
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    const fs::path db_path = test_dir / "index.sqlite";
    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    insert_test_data(db);
    populate_fts(db);

    // -----------------------------------------------------------------------
    // Test 1: FTS5-only fallback when model=nullptr and store=nullptr
    // HYB-03: must return results with provenance == "fts5", no crash
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto hybrid = eng.search_text("analyze", "", 10);
        assert(hybrid.search_mode == "fts5_only");
        assert(!hybrid.results.empty());
        for (const auto& r : hybrid.results) {
            assert(r.provenance == "fts5");
        }
        std::cout << "PASS: Test 1 - FTS5-only fallback (null model/store), provenance=fts5\n";
    }

    // -----------------------------------------------------------------------
    // Test 2: search_symbols kind filter works in FTS5-only mode
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto hybrid = eng.search_symbols("", "function", "", 10);
        assert(hybrid.search_mode == "fts5_only");
        assert(!hybrid.results.empty());
        for (const auto& r : hybrid.results) {
            assert(r.kind == "function");
            assert(r.provenance == "fts5");
        }
        std::cout << "PASS: Test 2 - kind filter in FTS5-only mode returns only functions\n";
    }

    // -----------------------------------------------------------------------
    // Test 3: Language filter works in FTS5-only mode
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto hybrid = eng.search_symbols("", "function", "python", 10);
        assert(hybrid.search_mode == "fts5_only");
        assert(!hybrid.results.empty());
        for (const auto& r : hybrid.results) {
            assert(r.kind == "function");
            // Python file path should contain "py"
            assert(r.file_path.find("py") != std::string::npos);
        }
        // Should not return C++ symbols
        for (const auto& r : hybrid.results) {
            assert(r.name != "analyze_file");
            assert(r.name != "extract_calls");
        }
        std::cout << "PASS: Test 3 - language filter python restricts to Python symbols\n";
    }

    // -----------------------------------------------------------------------
    // Test 4: Result limit is respected
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto hybrid = eng.search_text("analyze", "", 1);
        assert(hybrid.results.size() <= 1);
        std::cout << "PASS: Test 4 - limit=1 returns at most 1 result (got "
                  << hybrid.results.size() << ")\n";
    }

    // -----------------------------------------------------------------------
    // Test 5: No crash on empty query with kind filter
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto hybrid = eng.search_symbols("", "class", "", 10);
        assert(!hybrid.results.empty());
        for (const auto& r : hybrid.results) {
            assert(r.kind == "class");
        }
        std::cout << "PASS: Test 5 - empty query + kind filter returns symbols of that kind\n";
    }

    // -----------------------------------------------------------------------
    // Test 6: No duplicates in output
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto hybrid = eng.search_text("analyze", "", 20);
        // Verify no duplicate symbol_ids
        std::unordered_map<int64_t, int> counts;
        for (const auto& r : hybrid.results) {
            counts[r.symbol_id]++;
        }
        for (const auto& [id, cnt] : counts) {
            assert(cnt == 1);
        }
        std::cout << "PASS: Test 6 - no duplicate symbol_ids in results\n";
    }

    // -----------------------------------------------------------------------
    // Test 7: Nonexistent query returns empty (no crash)
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto hybrid = eng.search_text("xyzzy_nonexistent_12345", "", 10);
        assert(hybrid.results.empty());
        std::cout << "PASS: Test 7 - nonexistent query returns empty vector\n";
    }

    // -----------------------------------------------------------------------
    // Test 8: HybridSearchConfig with different rrf_k creates engine without crash
    // (FTS5-only path does not use k, but constructor must succeed)
    // -----------------------------------------------------------------------
    {
        HybridSearchConfig cfg;
        cfg.rrf_k = 10;
        cfg.candidate_multiplier = 5;
        HybridSearchEngine eng(db_path, nullptr, nullptr, cfg);
        auto hybrid = eng.search_text("analyze", "", 10);
        assert(!hybrid.results.empty());
        std::cout << "PASS: Test 8 - HybridSearchConfig{rrf_k=10, mult=5} accepted, FTS5 path works\n";
    }

    // -----------------------------------------------------------------------
    // Test 9: search_text with language filter
    // -----------------------------------------------------------------------
    {
        HybridSearchEngine eng(db_path, nullptr, nullptr);
        auto cpp_hybrid = eng.search_text("analyze", "cpp", 10);
        auto py_hybrid  = eng.search_text("analyze", "python", 10);

        // C++ results should contain analyze_file (not analyze_python)
        bool found_analyze_file = false;
        for (const auto& r : cpp_hybrid.results) {
            assert(r.file_path.find("cpp") != std::string::npos
                || r.file_path.find(".cpp") != std::string::npos);
            if (r.name == "analyze_file") found_analyze_file = true;
        }
        assert(found_analyze_file);

        // Python results should contain analyze_python (not analyze_file from cpp)
        bool found_analyze_python = false;
        for (const auto& r : py_hybrid.results) {
            if (r.name == "analyze_python") found_analyze_python = true;
            // Must not contain cpp symbols
            assert(r.name != "analyze_file");
        }
        assert(found_analyze_python);

        std::cout << "PASS: Test 9 - language filter in search_text separates cpp and python\n";
    }

    // -----------------------------------------------------------------------
    // RRF merge unit tests — call rrf_merge() directly, no database needed
    // -----------------------------------------------------------------------

    auto make_result = [](int64_t id, const std::string& name) -> SearchResult {
        SearchResult r;
        r.symbol_id    = id;
        r.name         = name;
        r.kind         = "function";
        r.signature    = "";
        r.documentation = "";
        r.file_path    = "/test.cpp";
        r.line_start   = 1;
        r.rank         = 0.0;
        r.provenance   = "";
        return r;
    };

    // -----------------------------------------------------------------------
    // Test 10: rrf_merge with FTS5-only (empty faiss list) — all provenance fts5
    // -----------------------------------------------------------------------
    {
        std::vector<SearchResult> fts5 = {
            make_result(1, "alpha"),
            make_result(2, "beta"),
            make_result(3, "gamma"),
        };
        std::vector<std::pair<int64_t, float>> faiss = {};
        std::unordered_map<int64_t, SearchResult> lookup = {
            {1, make_result(1, "alpha")},
            {2, make_result(2, "beta")},
            {3, make_result(3, "gamma")},
        };
        auto results = codetldr::rrf_merge(fts5, faiss, lookup, 10);
        assert(results.size() == 3);
        for (const auto& r : results) {
            assert(r.provenance == "fts5");
        }
        std::cout << "PASS: Test 10 - rrf_merge FTS5-only: all provenance=fts5\n";
    }

    // -----------------------------------------------------------------------
    // Test 11: rrf_merge with FAISS-only (empty fts5 list) — all provenance vector
    // -----------------------------------------------------------------------
    {
        std::vector<SearchResult> fts5 = {};
        std::vector<std::pair<int64_t, float>> faiss = {
            {10, 0.1f}, {11, 0.2f}, {12, 0.3f}
        };
        std::unordered_map<int64_t, SearchResult> lookup = {
            {10, make_result(10, "vec_a")},
            {11, make_result(11, "vec_b")},
            {12, make_result(12, "vec_c")},
        };
        auto results = codetldr::rrf_merge(fts5, faiss, lookup, 10);
        assert(results.size() == 3);
        for (const auto& r : results) {
            assert(r.provenance == "vector");
        }
        std::cout << "PASS: Test 11 - rrf_merge FAISS-only: all provenance=vector\n";
    }

    // -----------------------------------------------------------------------
    // Test 12: rrf_merge with overlap — id=1 in both, id=2 fts5-only, id=3 faiss-only
    // -----------------------------------------------------------------------
    {
        std::vector<SearchResult> fts5 = {
            make_result(1, "overlap"),
            make_result(2, "fts5_only"),
        };
        std::vector<std::pair<int64_t, float>> faiss = {
            {1, 0.1f}, {3, 0.2f}
        };
        std::unordered_map<int64_t, SearchResult> lookup = {
            {1, make_result(1, "overlap")},
            {2, make_result(2, "fts5_only")},
            {3, make_result(3, "faiss_only")},
        };
        auto results = codetldr::rrf_merge(fts5, faiss, lookup, 10);
        assert(results.size() == 3);
        // Verify provenance assignment for each symbol
        for (const auto& r : results) {
            if (r.symbol_id == 1) assert(r.provenance == "both");   // provenance "both" for overlap
            if (r.symbol_id == 2) assert(r.provenance == "fts5");
            if (r.symbol_id == 3) assert(r.provenance == "vector");
        }
        std::cout << "PASS: Test 12 - rrf_merge overlap: id=1 both, id=2 fts5, id=3 vector\n";
    }

    // -----------------------------------------------------------------------
    // Test 13: Symbol ranked #1 in both lists has higher RRF score than #1 in one list
    // -----------------------------------------------------------------------
    {
        // id=1 is #1 in both fts5 and faiss
        // id=2 is #1 in fts5 only
        // id=3 is #1 in faiss only (but placed after id=1)
        std::vector<SearchResult> fts5 = {
            make_result(1, "both_top"),
            make_result(2, "fts5_top"),
        };
        std::vector<std::pair<int64_t, float>> faiss = {
            {1, 0.1f}, {3, 0.2f}
        };
        std::unordered_map<int64_t, SearchResult> lookup = {
            {1, make_result(1, "both_top")},
            {2, make_result(2, "fts5_top")},
            {3, make_result(3, "faiss_only")},
        };
        auto results = codetldr::rrf_merge(fts5, faiss, lookup, 10);
        // id=1 should have highest rank (scored in both lists at position 0)
        double rank_both_top = -1.0, rank_fts5_top = -1.0;
        for (const auto& r : results) {
            if (r.symbol_id == 1) rank_both_top = r.rank;
            if (r.symbol_id == 2) rank_fts5_top = r.rank;
        }
        assert(rank_both_top > rank_fts5_top);
        assert(results[0].symbol_id == 1);
        std::cout << "PASS: Test 13 - id ranked #1 in both lists has higher RRF score than #1 in one\n";
    }

    // -----------------------------------------------------------------------
    // Test 14: FAISS sentinel id=-1 entries are skipped
    // -----------------------------------------------------------------------
    {
        std::vector<SearchResult> fts5 = {};
        std::vector<std::pair<int64_t, float>> faiss = {
            {-1, 0.0f}, {5, 0.5f}, {-1, 0.0f}
        };
        std::unordered_map<int64_t, SearchResult> lookup = {
            {5, make_result(5, "valid")},
        };
        auto results = codetldr::rrf_merge(fts5, faiss, lookup, 10);
        assert(results.size() == 1);
        assert(results[0].symbol_id == 5);
        for (const auto& r : results) {
            assert(r.symbol_id >= 0);
        }
        std::cout << "PASS: Test 14 - sentinel id=-1 entries skipped, no crash\n";
    }

    // -----------------------------------------------------------------------
    // Test 15: limit truncates output
    // -----------------------------------------------------------------------
    {
        std::vector<SearchResult> fts5;
        std::unordered_map<int64_t, SearchResult> lookup;
        for (int i = 1; i <= 10; ++i) {
            fts5.push_back(make_result(i, "sym" + std::to_string(i)));
            lookup[i] = make_result(i, "sym" + std::to_string(i));
        }
        std::vector<std::pair<int64_t, float>> faiss = {};
        auto results = codetldr::rrf_merge(fts5, faiss, lookup, 3);
        assert(results.size() <= 3);
        std::cout << "PASS: Test 15 - limit=3 truncates 10 results to " << results.size() << "\n";
    }

    // -----------------------------------------------------------------------
    // Test 16: Dedup — symbol in both lists appears exactly once
    // -----------------------------------------------------------------------
    {
        std::vector<SearchResult> fts5 = {
            make_result(1, "a"), make_result(2, "b")
        };
        std::vector<std::pair<int64_t, float>> faiss = {
            {1, 0.1f}, {2, 0.2f}
        };
        std::unordered_map<int64_t, SearchResult> lookup = {
            {1, make_result(1, "a")},
            {2, make_result(2, "b")},
        };
        auto results = codetldr::rrf_merge(fts5, faiss, lookup, 10);
        // Should have exactly 2 results (not 4)
        assert(results.size() == 2);
        std::unordered_map<int64_t, int> id_count;
        for (const auto& r : results) id_count[r.symbol_id]++;
        for (const auto& [id, cnt] : id_count) {
            assert(cnt == 1);
        }
        std::cout << "PASS: Test 16 - dedup: 2 symbols each in both lists → exactly 2 results\n";
    }

    // -----------------------------------------------------------------------
    // Test 17: Different rrf_k values produce same ordering but different scores
    // -----------------------------------------------------------------------
    {
        std::vector<SearchResult> fts5 = {
            make_result(1, "top"), make_result(2, "second"), make_result(3, "third")
        };
        std::vector<std::pair<int64_t, float>> faiss = {
            {1, 0.1f}, {3, 0.3f}
        };
        std::unordered_map<int64_t, SearchResult> lookup = {
            {1, make_result(1, "top")},
            {2, make_result(2, "second")},
            {3, make_result(3, "third")},
        };
        auto results_k10  = codetldr::rrf_merge(fts5, faiss, lookup, 10, /*rrf_k=*/10);
        auto results_k100 = codetldr::rrf_merge(fts5, faiss, lookup, 10, /*rrf_k=*/100);

        // Same ordering
        assert(results_k10.size() == results_k100.size());
        for (size_t i = 0; i < results_k10.size(); ++i) {
            assert(results_k10[i].symbol_id == results_k100[i].symbol_id);
        }

        // k=10 produces larger scores than k=100 (denominator is smaller)
        // Compare the top-ranked symbol
        assert(results_k10[0].rank > results_k100[0].rank);

        std::cout << "PASS: Test 17 - rrf_k=10 and rrf_k=100 same ordering, k=10 has larger scores\n";
    }

    std::cout << "\nAll hybrid_search tests passed.\n";
    return 0;
}
