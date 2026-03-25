// test_call_extractor.cpp -- Integration tests: symbol+call extraction + SQLite persistence.
// No test framework: returns 0 on pass, 1 on failure.

#include "analysis/pipeline.h"
#include "analysis/tree_sitter/language_registry.h"
#include "storage/database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace codetldr;

static fs::path fixture_dir() {
    return fs::path(__FILE__).parent_path() / "fixtures";
}

// Create a unique temp database path
static fs::path temp_db_path() {
    static int counter = 0;
    return fs::temp_directory_path() / ("test_call_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".sqlite");
}

// ============================================================
// test_analyze_python_symbols_persisted
// ============================================================
static bool test_analyze_python_symbols_persisted(const LanguageRegistry& reg) {
    std::string test_name = "test_analyze_python_symbols_persisted";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "sample.py";
    auto result  = analyze_file(db, reg, py_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    if (result.symbols_count < 6) {
        std::cerr << "[FAIL] " << test_name << ": expected >= 6 symbols_count, got " << result.symbols_count << "\n";
        fs::remove(db_path);
        return false;
    }

    // Verify in SQLite
    {
        SQLite::Statement q(db, "SELECT count(*) FROM symbols WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        int count = q.getColumn(0).getInt();
        if (count < 6) {
            std::cerr << "[FAIL] " << test_name << ": DB has " << count << " symbols, expected >= 6\n";
            fs::remove(db_path);
            return false;
        }
    }

    // Check "greet" has kind "function" and "Calculator" has kind "class"
    {
        SQLite::Statement q(db, "SELECT name, kind FROM symbols WHERE file_id = (SELECT id FROM files WHERE path = ?) ORDER BY line_start");
        q.bind(1, py_path.string());
        bool found_greet = false;
        bool found_calc  = false;
        while (q.executeStep()) {
            std::string name = q.getColumn(0).getText();
            std::string kind = q.getColumn(1).getText();
            if (name == "greet"      && kind == "function") found_greet = true;
            if (name == "Calculator" && kind == "class")    found_calc  = true;
        }
        if (!found_greet || !found_calc) {
            std::cerr << "[FAIL] " << test_name << ": greet/Calculator not found with correct kinds\n";
            fs::remove(db_path);
            return false;
        }
    }

    fs::remove(db_path);
    std::cout << "[PASS] " << test_name << " (" << result.symbols_count << " symbols)\n";
    return true;
}

// ============================================================
// test_analyze_python_calls_persisted
// ============================================================
static bool test_analyze_python_calls_persisted(const LanguageRegistry& reg) {
    std::string test_name = "test_analyze_python_calls_persisted";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "sample.py";
    auto result  = analyze_file(db, reg, py_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    // Check callee_name "greet" and "farewell" are in calls
    {
        SQLite::Statement q(db, "SELECT callee_name FROM calls WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        bool has_greet   = false;
        bool has_farewell = false;
        while (q.executeStep()) {
            std::string callee = q.getColumn(0).getText();
            if (callee == "greet")   has_greet   = true;
            if (callee == "farewell") has_farewell = true;
        }
        if (!has_greet || !has_farewell) {
            std::cerr << "[FAIL] " << test_name << ": calls to greet/farewell not found in DB\n";
            fs::remove(db_path);
            return false;
        }
    }

    // Check that at least one call has caller = "main" via JOIN
    {
        SQLite::Statement q(db,
            "SELECT c.callee_name, s.name as caller "
            "FROM calls c JOIN symbols s ON c.caller_id = s.id "
            "WHERE c.file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        bool found_main_caller = false;
        while (q.executeStep()) {
            std::string caller = q.getColumn(1).getText();
            if (caller == "main") { found_main_caller = true; break; }
        }
        if (!found_main_caller) {
            std::cerr << "[FAIL] " << test_name << ": no call with caller = 'main' in DB\n";
            fs::remove(db_path);
            return false;
        }
    }

    fs::remove(db_path);
    std::cout << "[PASS] " << test_name << " (" << result.calls_count << " calls)\n";
    return true;
}

// ============================================================
// test_upsert_no_duplicates
// ============================================================
static bool test_upsert_no_duplicates(const LanguageRegistry& reg) {
    std::string test_name = "test_upsert_no_duplicates";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "sample.py";

    // First analysis
    auto r1 = analyze_file(db, reg, py_path);
    if (!r1.success) {
        std::cerr << "[FAIL] " << test_name << ": first analyze_file failed: " << r1.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int n1_sym = 0, n1_calls = 0;
    {
        SQLite::Statement q(db, "SELECT count(*) FROM symbols WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        n1_sym = q.getColumn(0).getInt();
    }
    {
        SQLite::Statement q(db, "SELECT count(*) FROM calls WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        n1_calls = q.getColumn(0).getInt();
    }

    // Second analysis (same file)
    auto r2 = analyze_file(db, reg, py_path);
    if (!r2.success) {
        std::cerr << "[FAIL] " << test_name << ": second analyze_file failed: " << r2.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int n2_sym = 0, n2_calls = 0;
    {
        SQLite::Statement q(db, "SELECT count(*) FROM symbols WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        n2_sym = q.getColumn(0).getInt();
    }
    {
        SQLite::Statement q(db, "SELECT count(*) FROM calls WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        n2_calls = q.getColumn(0).getInt();
    }

    if (n1_sym != n2_sym) {
        std::cerr << "[FAIL] " << test_name << ": symbol count changed after re-parse: " << n1_sym << " -> " << n2_sym << "\n";
        fs::remove(db_path);
        return false;
    }
    if (n1_calls != n2_calls) {
        std::cerr << "[FAIL] " << test_name << ": calls count changed after re-parse: " << n1_calls << " -> " << n2_calls << "\n";
        fs::remove(db_path);
        return false;
    }

    fs::remove(db_path);
    std::cout << "[PASS] " << test_name << " (symbols=" << n1_sym << " calls=" << n1_calls << ")\n";
    return true;
}

// ============================================================
// test_intra_file_call_resolution
// ============================================================
static bool test_intra_file_call_resolution(const LanguageRegistry& reg) {
    std::string test_name = "test_intra_file_call_resolution";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "sample.py";
    auto result  = analyze_file(db, reg, py_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    // Calls to greet/farewell should have non-NULL callee_id (resolved)
    {
        SQLite::Statement q(db,
            "SELECT c.callee_name, c.callee_id "
            "FROM calls c "
            "WHERE c.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND c.callee_name IN ('greet', 'farewell')");
        q.bind(1, py_path.string());
        bool found_greet_resolved   = false;
        bool found_farewell_resolved = false;
        while (q.executeStep()) {
            std::string callee_name = q.getColumn(0).getText();
            bool callee_id_null = q.getColumn(1).isNull();
            if (callee_name == "greet"    && !callee_id_null) found_greet_resolved   = true;
            if (callee_name == "farewell" && !callee_id_null) found_farewell_resolved = true;
        }
        if (!found_greet_resolved) {
            std::cerr << "[FAIL] " << test_name << ": call to 'greet' has NULL callee_id (not resolved)\n";
            fs::remove(db_path);
            return false;
        }
        if (!found_farewell_resolved) {
            std::cerr << "[FAIL] " << test_name << ": call to 'farewell' has NULL callee_id (not resolved)\n";
            fs::remove(db_path);
            return false;
        }
    }

    fs::remove(db_path);
    std::cout << "[PASS] " << test_name << "\n";
    return true;
}

// ============================================================
// test_all_languages_analyze
// ============================================================
static bool test_all_languages_analyze(const LanguageRegistry& reg) {
    std::string test_name = "test_all_languages_analyze";
    bool ok = true;

    struct Fixture { std::string file; };
    std::vector<Fixture> fixtures = {
        {"sample.py"},
        {"sample.js"},
        {"sample.ts"},
        {"sample.rs"},
        {"sample.c"},
        {"sample.cpp"},
        {"sample.java"},
        {"sample.kt"},
        {"sample.swift"},
        {"sample.m"},
    };

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    for (const auto& fix : fixtures) {
        auto path   = fixture_dir() / fix.file;
        auto result = analyze_file(db, reg, path);

        if (!result.success) {
            std::cerr << "[FAIL] " << test_name << ": " << fix.file << " failed: " << result.error << "\n";
            ok = false;
            continue;
        }
        if (result.symbols_count < 3) {
            std::cerr << "[FAIL] " << test_name << ": " << fix.file
                      << " expected >= 3 symbols, got " << result.symbols_count << "\n";
            ok = false;
        }
        if (result.calls_count < 2) {
            std::cerr << "[FAIL] " << test_name << ": " << fix.file
                      << " expected >= 2 calls, got " << result.calls_count << "\n";
            ok = false;
        }
        std::cout << "  " << fix.file << ": symbols=" << result.symbols_count
                  << " calls=" << result.calls_count << "\n";
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_call_graph_coverage_python
// ============================================================
static bool test_call_graph_coverage_python(const LanguageRegistry& reg) {
    std::string test_name = "test_call_graph_coverage_python";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "sample.py";
    auto result  = analyze_file(db, reg, py_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    // The sample.py main() has 5 calls: greet, farewell, Calculator, calc.add, calc.multiply
    // Require at least 3 out of 5 (>= 60% coverage)
    // Known calls: greet("world"), farewell("world"), Calculator(), calc.add(2,3), calc.multiply(result,2)
    int main_calls = 0;
    {
        SQLite::Statement q(db,
            "SELECT count(*) FROM calls c "
            "JOIN symbols s ON c.caller_id = s.id "
            "WHERE s.name = 'main' "
            "AND c.file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        main_calls = q.getColumn(0).getInt();
    }

    if (main_calls < 3) {
        std::cerr << "[FAIL] " << test_name << ": expected >= 3 calls from main, got " << main_calls << "\n";
        fs::remove(db_path);
        return false;
    }

    fs::remove(db_path);
    std::cout << "[PASS] " << test_name << " (" << main_calls << " calls from main)\n";
    return true;
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
    all_pass &= test_analyze_python_symbols_persisted(reg);
    all_pass &= test_analyze_python_calls_persisted(reg);
    all_pass &= test_upsert_no_duplicates(reg);
    all_pass &= test_intra_file_call_resolution(reg);
    all_pass &= test_all_languages_analyze(reg);
    all_pass &= test_call_graph_coverage_python(reg);

    if (all_pass) {
        std::cout << "\nAll tests PASSED\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED\n";
        return 1;
    }
}
