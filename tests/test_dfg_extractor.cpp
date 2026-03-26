// test_dfg_extractor.cpp -- Integration tests: DFG edge extraction and persistence.
// No test framework: returns 0 on pass, 1 on failure.

#include "analysis/pipeline.h"
#include "analysis/tree_sitter/language_registry.h"
#include "analysis/tree_sitter/queries.h"
#include "storage/database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace codetldr;

static fs::path fixture_dir() {
    return fs::path(__FILE__).parent_path() / "fixtures";
}

static fs::path temp_db_path() {
    static int counter = 0;
    return fs::temp_directory_path() / ("test_dfg_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".sqlite");
}

// ============================================================
// test_dfg_python_edge_counts
// Python fixture: transform(x, y, z=10)
//   parameters:   3  (x, y, z)
//   assignments:  3  (result=, total=, ratio=)
//   return_value: 1  (return ratio)
// Total: 7 dfg_edges inside transform()
// ============================================================
static bool test_dfg_python_edge_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_dfg_python_edge_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "dfg_sample.py";
    auto result  = analyze_file(db, reg, py_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    // Count dfg_edges per edge_type for all edges inside transform()
    struct TypeCount { std::string type; int expected; int actual = 0; };
    std::vector<TypeCount> types = {
        {"assignment",   3},
        {"parameter",    3},
        {"return_value", 1},
    };
    {
        SQLite::Statement q(db,
            "SELECT de.edge_type, COUNT(*) as cnt "
            "FROM dfg_edges de "
            "JOIN symbols s ON de.symbol_id = s.id "
            "WHERE de.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND s.name = 'transform' "
            "GROUP BY de.edge_type");
        q.bind(1, py_path.string());
        while (q.executeStep()) {
            std::string tp  = q.getColumn(0).getText();
            int         cnt = q.getColumn(1).getInt();
            for (auto& tc : types) {
                if (tc.type == tp) tc.actual = cnt;
            }
        }
    }

    bool ok = true;
    for (const auto& tc : types) {
        if (tc.actual != tc.expected) {
            std::cerr << "[FAIL] " << test_name << ": transform() " << tc.type
                      << " expected " << tc.expected << ", got " << tc.actual << "\n";
            ok = false;
        }
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_dfg_c_edge_counts
// C fixture: compute(int a, int b)
//   parameters:   2  (a, b)
//   assignments:  3  (sum=, product=, sum=)
//   return_value: 1  (return product)
// Total: 6 dfg_edges inside compute()
// ============================================================
static bool test_dfg_c_edge_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_dfg_c_edge_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto c_path = fixture_dir() / "dfg_sample.c";
    auto result = analyze_file(db, reg, c_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    struct TypeCount { std::string type; int expected; int actual = 0; };
    std::vector<TypeCount> types = {
        {"assignment",   3},
        {"parameter",    2},
        {"return_value", 1},
    };
    {
        SQLite::Statement q(db,
            "SELECT de.edge_type, COUNT(*) as cnt "
            "FROM dfg_edges de "
            "JOIN symbols s ON de.symbol_id = s.id "
            "WHERE de.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND s.name = 'compute' "
            "GROUP BY de.edge_type");
        q.bind(1, c_path.string());
        while (q.executeStep()) {
            std::string tp  = q.getColumn(0).getText();
            int         cnt = q.getColumn(1).getInt();
            for (auto& tc : types) {
                if (tc.type == tp) tc.actual = cnt;
            }
        }
    }

    bool ok = true;
    for (const auto& tc : types) {
        if (tc.actual != tc.expected) {
            std::cerr << "[FAIL] " << test_name << ": compute() " << tc.type
                      << " expected " << tc.expected << ", got " << tc.actual << "\n";
            ok = false;
        }
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_dfg_cpp_edge_counts
// C++ fixture: analyze(int x, int& y)
//   parameters:   2  (x, y)
//   assignments:  3  (result=, result=, final_val=)
//   return_value: 1  (return final_val)
// Total: 6 dfg_edges inside analyze()
// ============================================================
static bool test_dfg_cpp_edge_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_dfg_cpp_edge_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto cpp_path = fixture_dir() / "dfg_sample.cpp";
    auto result   = analyze_file(db, reg, cpp_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    struct TypeCount { std::string type; int expected; int actual = 0; };
    std::vector<TypeCount> types = {
        {"assignment",   3},
        {"parameter",    2},
        {"return_value", 1},
    };
    {
        SQLite::Statement q(db,
            "SELECT de.edge_type, COUNT(*) as cnt "
            "FROM dfg_edges de "
            "JOIN symbols s ON de.symbol_id = s.id "
            "WHERE de.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND s.name = 'analyze' "
            "GROUP BY de.edge_type");
        q.bind(1, cpp_path.string());
        while (q.executeStep()) {
            std::string tp  = q.getColumn(0).getText();
            int         cnt = q.getColumn(1).getInt();
            for (auto& tc : types) {
                if (tc.type == tp) tc.actual = cnt;
            }
        }
    }

    bool ok = true;
    for (const auto& tc : types) {
        if (tc.actual != tc.expected) {
            std::cerr << "[FAIL] " << test_name << ": analyze() " << tc.type
                      << " expected " << tc.expected << ", got " << tc.actual << "\n";
            ok = false;
        }
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_dfg_js_edge_counts
// JS fixture: calculate(a, b, c)
//   parameters:   3  (a, b, c)
//   assignments:  3  (sum=, product=, product=)
//   return_value: 1  (return product)
// Total: 7 dfg_edges inside calculate()
// ============================================================
static bool test_dfg_js_edge_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_dfg_js_edge_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto js_path = fixture_dir() / "dfg_sample.js";
    auto result  = analyze_file(db, reg, js_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    struct TypeCount { std::string type; int expected; int actual = 0; };
    std::vector<TypeCount> types = {
        {"assignment",   3},
        {"parameter",    3},
        {"return_value", 1},
    };
    {
        SQLite::Statement q(db,
            "SELECT de.edge_type, COUNT(*) as cnt "
            "FROM dfg_edges de "
            "JOIN symbols s ON de.symbol_id = s.id "
            "WHERE de.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND s.name = 'calculate' "
            "GROUP BY de.edge_type");
        q.bind(1, js_path.string());
        while (q.executeStep()) {
            std::string tp  = q.getColumn(0).getText();
            int         cnt = q.getColumn(1).getInt();
            for (auto& tc : types) {
                if (tc.type == tp) tc.actual = cnt;
            }
        }
    }

    bool ok = true;
    for (const auto& tc : types) {
        if (tc.actual != tc.expected) {
            std::cerr << "[FAIL] " << test_name << ": calculate() " << tc.type
                      << " expected " << tc.expected << ", got " << tc.actual << "\n";
            ok = false;
        }
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_dfg_function_local
// Verify: DFG extraction is function-local only.
// The Python fixture has no top-level assignments, so all edges
// belong to transform(). Check symbol_id is not NULL for any edge.
// ============================================================
static bool test_dfg_function_local(const LanguageRegistry& reg) {
    std::string test_name = "test_dfg_function_local";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "dfg_sample.py";
    auto result  = analyze_file(db, reg, py_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    // Count edges with NULL symbol_id (these would be top-level / function-local violation)
    int null_symbol_count = 0;
    {
        SQLite::Statement q(db,
            "SELECT COUNT(*) FROM dfg_edges "
            "WHERE file_id = (SELECT id FROM files WHERE path = ?) "
            "AND symbol_id IS NULL");
        q.bind(1, py_path.string());
        q.executeStep();
        null_symbol_count = q.getColumn(0).getInt();
    }

    bool ok = (null_symbol_count == 0);
    if (!ok) {
        std::cerr << "[FAIL] " << test_name << ": found " << null_symbol_count
                  << " dfg_edges with NULL symbol_id (function-local scoping violated)\n";
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (all edges have enclosing function)\n";
    return ok;
}

// ============================================================
// test_dfg_upsert_no_duplicates
// Verify: analyzing same file twice produces identical dfg_edges count (DFG-04)
// ============================================================
static bool test_dfg_upsert_no_duplicates(const LanguageRegistry& reg) {
    std::string test_name = "test_dfg_upsert_no_duplicates";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "dfg_sample.py";

    auto r1 = analyze_file(db, reg, py_path);
    if (!r1.success) {
        std::cerr << "[FAIL] " << test_name << ": first analyze_file failed: " << r1.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int n1 = 0;
    {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM dfg_edges WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        n1 = q.getColumn(0).getInt();
    }

    auto r2 = analyze_file(db, reg, py_path);
    if (!r2.success) {
        std::cerr << "[FAIL] " << test_name << ": second analyze_file failed: " << r2.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int n2 = 0;
    {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM dfg_edges WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        n2 = q.getColumn(0).getInt();
    }

    bool ok = (n1 == n2 && n1 > 0);
    if (!ok) {
        std::cerr << "[FAIL] " << test_name << ": dfg_edges count changed: " << n1 << " -> " << n2 << "\n";
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (dfg_edges=" << n1 << " both times)\n";
    return ok;
}

// ============================================================
// main
// ============================================================
int main() {
    LanguageRegistry reg;
    if (!reg.initialize()) {
        std::cerr << "ERROR: LanguageRegistry::initialize() returned false\n";
        return 1;
    }

    bool all_pass = true;
    all_pass &= test_dfg_python_edge_counts(reg);
    all_pass &= test_dfg_c_edge_counts(reg);
    all_pass &= test_dfg_cpp_edge_counts(reg);
    all_pass &= test_dfg_js_edge_counts(reg);
    all_pass &= test_dfg_function_local(reg);
    all_pass &= test_dfg_upsert_no_duplicates(reg);

    if (all_pass) {
        std::cout << "\nAll tests PASSED\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED\n";
        return 1;
    }
}
