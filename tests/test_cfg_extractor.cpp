// test_cfg_extractor.cpp -- Integration tests: CFG node extraction and persistence.
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
    return fs::temp_directory_path() / ("test_cfg_" + std::to_string(::getpid()) + "_" + std::to_string(counter++) + ".sqlite");
}

// ============================================================
// test_cfg_python_node_counts
// Verify: analyze() produces 9 cfg_nodes, simple_branch() produces 4
// ============================================================
static bool test_cfg_python_node_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_cfg_python_node_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "cfg_sample.py";
    auto result  = analyze_file(db, reg, py_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    // Count cfg_nodes per function
    int analyze_count = 0;
    int simple_count = 0;
    {
        SQLite::Statement q(db,
            "SELECT s.name, COUNT(*) as cnt "
            "FROM cfg_nodes cn "
            "LEFT JOIN symbols s ON cn.symbol_id = s.id "
            "WHERE cn.file_id = (SELECT id FROM files WHERE path = ?) "
            "GROUP BY cn.symbol_id");
        q.bind(1, py_path.string());
        while (q.executeStep()) {
            std::string sym_name = q.getColumn(0).getText();
            int cnt = q.getColumn(1).getInt();
            if (sym_name == "analyze") analyze_count = cnt;
            if (sym_name == "simple_branch") simple_count = cnt;
        }
    }

    bool ok = true;
    if (analyze_count != 9) {
        std::cerr << "[FAIL] " << test_name << ": analyze() expected 9 cfg_nodes, got " << analyze_count << "\n";
        ok = false;
    }
    if (simple_count != 4) {
        std::cerr << "[FAIL] " << test_name << ": simple_branch() expected 4 cfg_nodes, got " << simple_count << "\n";
        ok = false;
    }

    // Verify type breakdown for analyze: 2 try_catch, 1 loop, 2 if_branch, 1 else_branch, 3 early_return
    if (ok) {
        struct TypeCount { std::string type; int expected; int actual = 0; };
        std::vector<TypeCount> types = {
            {"try_catch", 2}, {"loop", 1}, {"if_branch", 2}, {"else_branch", 1}, {"early_return", 3}
        };
        {
            SQLite::Statement q(db,
                "SELECT cn.node_type, COUNT(*) as cnt "
                "FROM cfg_nodes cn "
                "JOIN symbols s ON cn.symbol_id = s.id "
                "WHERE cn.file_id = (SELECT id FROM files WHERE path = ?) "
                "AND s.name = 'analyze' "
                "GROUP BY cn.node_type");
            q.bind(1, py_path.string());
            while (q.executeStep()) {
                std::string tp = q.getColumn(0).getText();
                int cnt = q.getColumn(1).getInt();
                for (auto& tc : types) {
                    if (tc.type == tp) tc.actual = cnt;
                }
            }
        }
        for (const auto& tc : types) {
            if (tc.actual != tc.expected) {
                std::cerr << "[FAIL] " << test_name << ": analyze() " << tc.type
                          << " expected " << tc.expected << ", got " << tc.actual << "\n";
                ok = false;
            }
        }
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (analyze=" << analyze_count << " simple_branch=" << simple_count << ")\n";
    return ok;
}

// ============================================================
// test_cfg_c_node_counts
// Verify: process() produces 8 cfg_nodes, simple() produces 2
// ============================================================
static bool test_cfg_c_node_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_cfg_c_node_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto c_path = fixture_dir() / "cfg_sample.c";
    auto result = analyze_file(db, reg, c_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int process_count = 0;
    int simple_count = 0;
    {
        SQLite::Statement q(db,
            "SELECT s.name, COUNT(*) as cnt "
            "FROM cfg_nodes cn "
            "LEFT JOIN symbols s ON cn.symbol_id = s.id "
            "WHERE cn.file_id = (SELECT id FROM files WHERE path = ?) "
            "GROUP BY cn.symbol_id");
        q.bind(1, c_path.string());
        while (q.executeStep()) {
            std::string sym_name = q.getColumn(0).getText();
            int cnt = q.getColumn(1).getInt();
            if (sym_name == "process") process_count = cnt;
            if (sym_name == "simple")  simple_count  = cnt;
        }
    }

    bool ok = true;
    if (process_count != 8) {
        std::cerr << "[FAIL] " << test_name << ": process() expected 8 cfg_nodes, got " << process_count << "\n";
        ok = false;
    }
    if (simple_count != 2) {
        std::cerr << "[FAIL] " << test_name << ": simple() expected 2 cfg_nodes, got " << simple_count << "\n";
        ok = false;
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (process=" << process_count << " simple=" << simple_count << ")\n";
    return ok;
}

// ============================================================
// test_cfg_cpp_node_counts
// Verify: analyze() produces 8 cfg_nodes (2 try_catch, 1 loop, 1 if_branch, 1 else_branch, 3 early_return)
// ============================================================
static bool test_cfg_cpp_node_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_cfg_cpp_node_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto cpp_path = fixture_dir() / "cfg_sample.cpp";
    auto result   = analyze_file(db, reg, cpp_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int analyze_count = 0;
    {
        SQLite::Statement q(db,
            "SELECT COUNT(*) FROM cfg_nodes cn "
            "JOIN symbols s ON cn.symbol_id = s.id "
            "WHERE cn.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND s.name = 'analyze'");
        q.bind(1, cpp_path.string());
        q.executeStep();
        analyze_count = q.getColumn(0).getInt();
    }

    bool ok = true;
    if (analyze_count != 8) {
        std::cerr << "[FAIL] " << test_name << ": analyze() expected 8 cfg_nodes, got " << analyze_count << "\n";
        ok = false;
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (analyze=" << analyze_count << ")\n";
    return ok;
}

// ============================================================
// test_cfg_js_node_counts
// Verify: processItems() produces 11 cfg_nodes, loopTypes() produces 5
// ============================================================
static bool test_cfg_js_node_counts(const LanguageRegistry& reg) {
    std::string test_name = "test_cfg_js_node_counts";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto js_path = fixture_dir() / "cfg_sample.js";
    auto result  = analyze_file(db, reg, js_path);

    if (!result.success) {
        std::cerr << "[FAIL] " << test_name << ": analyze_file failed: " << result.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int process_count = 0;
    int loop_count = 0;
    {
        SQLite::Statement q(db,
            "SELECT s.name, COUNT(*) as cnt "
            "FROM cfg_nodes cn "
            "LEFT JOIN symbols s ON cn.symbol_id = s.id "
            "WHERE cn.file_id = (SELECT id FROM files WHERE path = ?) "
            "GROUP BY cn.symbol_id");
        q.bind(1, js_path.string());
        while (q.executeStep()) {
            std::string sym_name = q.getColumn(0).getText();
            int cnt = q.getColumn(1).getInt();
            if (sym_name == "processItems") process_count = cnt;
            if (sym_name == "loopTypes")   loop_count    = cnt;
        }
    }

    bool ok = true;
    if (process_count != 11) {
        std::cerr << "[FAIL] " << test_name << ": processItems() expected 11 cfg_nodes, got " << process_count << "\n";
        ok = false;
    }
    if (loop_count != 5) {
        std::cerr << "[FAIL] " << test_name << ": loopTypes() expected 5 cfg_nodes, got " << loop_count << "\n";
        ok = false;
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (processItems=" << process_count << " loopTypes=" << loop_count << ")\n";
    return ok;
}

// ============================================================
// test_cfg_depth
// Verify depth values for simple_branch in Python:
//   first if_branch -> depth 0, nested if_branch -> depth 1,
//   inner return -> depth 2, outer return -> depth 0
// ============================================================
static bool test_cfg_depth(const LanguageRegistry& reg) {
    std::string test_name = "test_cfg_depth";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "cfg_sample.py";
    analyze_file(db, reg, py_path);

    // Query all cfg_nodes for simple_branch ordered by line
    struct NodeRow { std::string node_type; int line; int depth; };
    std::vector<NodeRow> rows;
    {
        SQLite::Statement q(db,
            "SELECT cn.node_type, cn.line, cn.depth "
            "FROM cfg_nodes cn "
            "JOIN symbols s ON cn.symbol_id = s.id "
            "WHERE cn.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND s.name = 'simple_branch' "
            "ORDER BY cn.line");
        q.bind(1, py_path.string());
        while (q.executeStep()) {
            rows.push_back({q.getColumn(0).getText(), q.getColumn(1).getInt(), q.getColumn(2).getInt()});
        }
    }

    // Expected: if_branch@depth0, if_branch@depth1, early_return@depth2, early_return@depth0
    // (line 18: if x>0 depth=0, line 19: if x>10 depth=1, line 20: return x depth=2, line 21: return 0 depth=0)
    bool ok = true;
    if (rows.size() != 4) {
        std::cerr << "[FAIL] " << test_name << ": expected 4 nodes for simple_branch, got " << rows.size() << "\n";
        for (const auto& r : rows) {
            std::cerr << "  " << r.node_type << " line=" << r.line << " depth=" << r.depth << "\n";
        }
        fs::remove(db_path);
        return false;
    }

    // Row 0: if_branch at depth 0
    if (rows[0].node_type != "if_branch" || rows[0].depth != 0) {
        std::cerr << "[FAIL] " << test_name << ": row0 expected if_branch@0, got "
                  << rows[0].node_type << "@" << rows[0].depth << "\n";
        ok = false;
    }
    // Row 1: if_branch at depth 1
    if (rows[1].node_type != "if_branch" || rows[1].depth != 1) {
        std::cerr << "[FAIL] " << test_name << ": row1 expected if_branch@1, got "
                  << rows[1].node_type << "@" << rows[1].depth << "\n";
        ok = false;
    }
    // Row 2: early_return at depth 2
    if (rows[2].node_type != "early_return" || rows[2].depth != 2) {
        std::cerr << "[FAIL] " << test_name << ": row2 expected early_return@2, got "
                  << rows[2].node_type << "@" << rows[2].depth << "\n";
        ok = false;
    }
    // Row 3: early_return at depth 0
    if (rows[3].node_type != "early_return" || rows[3].depth != 0) {
        std::cerr << "[FAIL] " << test_name << ": row3 expected early_return@0, got "
                  << rows[3].node_type << "@" << rows[3].depth << "\n";
        ok = false;
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << "\n";
    return ok;
}

// ============================================================
// test_cfg_condition_text
// Verify condition extraction for simple_branch in Python:
//   if_branch at depth 0 has condition containing "x > 0"
//   if_branch at depth 1 has condition containing "x > 10"
// ============================================================
static bool test_cfg_condition_text(const LanguageRegistry& reg) {
    std::string test_name = "test_cfg_condition_text";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "cfg_sample.py";
    analyze_file(db, reg, py_path);

    std::string cond_depth0, cond_depth1;
    {
        SQLite::Statement q(db,
            "SELECT cn.condition, cn.depth "
            "FROM cfg_nodes cn "
            "JOIN symbols s ON cn.symbol_id = s.id "
            "WHERE cn.file_id = (SELECT id FROM files WHERE path = ?) "
            "AND s.name = 'simple_branch' "
            "AND cn.node_type = 'if_branch' "
            "ORDER BY cn.line");
        q.bind(1, py_path.string());
        while (q.executeStep()) {
            int depth = q.getColumn(1).getInt();
            std::string cond = q.getColumn(0).isNull() ? "" : q.getColumn(0).getText();
            if (depth == 0) cond_depth0 = cond;
            if (depth == 1) cond_depth1 = cond;
        }
    }

    bool ok = true;
    if (cond_depth0.find("x > 0") == std::string::npos) {
        std::cerr << "[FAIL] " << test_name << ": depth=0 condition expected to contain 'x > 0', got: '" << cond_depth0 << "'\n";
        ok = false;
    }
    if (cond_depth1.find("x > 10") == std::string::npos) {
        std::cerr << "[FAIL] " << test_name << ": depth=1 condition expected to contain 'x > 10', got: '" << cond_depth1 << "'\n";
        ok = false;
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (cond0='" << cond_depth0 << "' cond1='" << cond_depth1 << "')\n";
    return ok;
}

// ============================================================
// test_cfg_upsert_no_duplicates
// Verify: analyzing same file twice produces identical cfg_nodes count (CFG-04)
// ============================================================
static bool test_cfg_upsert_no_duplicates(const LanguageRegistry& reg) {
    std::string test_name = "test_cfg_upsert_no_duplicates";

    auto db_path = temp_db_path();
    auto db_obj  = Database::open(db_path);
    auto& db     = db_obj.raw();

    auto py_path = fixture_dir() / "cfg_sample.py";

    auto r1 = analyze_file(db, reg, py_path);
    if (!r1.success) {
        std::cerr << "[FAIL] " << test_name << ": first analyze_file failed: " << r1.error << "\n";
        fs::remove(db_path);
        return false;
    }

    int n1 = 0;
    {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM cfg_nodes WHERE file_id = (SELECT id FROM files WHERE path = ?)");
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
        SQLite::Statement q(db, "SELECT COUNT(*) FROM cfg_nodes WHERE file_id = (SELECT id FROM files WHERE path = ?)");
        q.bind(1, py_path.string());
        q.executeStep();
        n2 = q.getColumn(0).getInt();
    }

    bool ok = (n1 == n2 && n1 > 0);
    if (!ok) {
        std::cerr << "[FAIL] " << test_name << ": cfg_nodes count changed: " << n1 << " -> " << n2 << "\n";
    }

    fs::remove(db_path);
    if (ok) std::cout << "[PASS] " << test_name << " (cfg_nodes=" << n1 << " both times)\n";
    return ok;
}

// ============================================================
// test_cfg_no_predicates
// Verify: CFG query strings contain no "#match?" or "#eq?" predicates (CFG-03)
// ============================================================
static bool test_cfg_no_predicates() {
    std::string test_name = "test_cfg_no_predicates";

    // Check all 4 CFG-enabled languages
    struct LangQuery { std::string name; const char* query; };
    std::vector<LangQuery> langs = {
        {"python",     queries::python().cfg},
        {"javascript", queries::javascript().cfg},
        {"c",          queries::c().cfg},
        {"cpp",        queries::cpp().cfg},
    };

    bool ok = true;
    for (const auto& lq : langs) {
        if (!lq.query) {
            std::cerr << "[FAIL] " << test_name << ": " << lq.name << " has null cfg query\n";
            ok = false;
            continue;
        }
        std::string qs(lq.query);
        if (qs.find("#match?") != std::string::npos) {
            std::cerr << "[FAIL] " << test_name << ": " << lq.name << " CFG query contains '#match?'\n";
            ok = false;
        }
        if (qs.find("#eq?") != std::string::npos) {
            std::cerr << "[FAIL] " << test_name << ": " << lq.name << " CFG query contains '#eq?'\n";
            ok = false;
        }
    }

    if (ok) std::cout << "[PASS] " << test_name << "\n";
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
    all_pass &= test_cfg_python_node_counts(reg);
    all_pass &= test_cfg_c_node_counts(reg);
    all_pass &= test_cfg_cpp_node_counts(reg);
    all_pass &= test_cfg_js_node_counts(reg);
    all_pass &= test_cfg_depth(reg);
    all_pass &= test_cfg_condition_text(reg);
    all_pass &= test_cfg_upsert_no_duplicates(reg);
    all_pass &= test_cfg_no_predicates();

    if (all_pass) {
        std::cout << "\nAll tests PASSED\n";
        return 0;
    } else {
        std::cerr << "\nSome tests FAILED\n";
        return 1;
    }
}
