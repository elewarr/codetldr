// test_lsp_call_graph.cpp
// Tests for LspCallGraphResolver and upgraded get_call_graph MCP tool.
// Tests do NOT require a real LSP server — they test DB query layer and MCP
// response shape by inserting test data directly into the tables.

#include "storage/schema.h"
#include "storage/migrations.h"
#include "storage/database.h"
#include "lsp/lsp_call_graph_resolver.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>

// ============================================================
// Simple test harness (same style as existing tests)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& msg) {
    if (cond) {
        std::cout << "PASS: " << msg << "\n";
        ++g_pass;
    } else {
        std::cerr << "FAIL: " << msg << "\n";
        ++g_fail;
    }
}

// ============================================================
// Helper: open in-memory SQLite DB with all migrations applied
// ============================================================
static SQLite::Database make_test_db() {
    SQLite::Database db(":memory:",
        SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    db.exec("PRAGMA foreign_keys = ON");
    codetldr::run_migrations(db);
    return db;
}

// ============================================================
// Helper: insert a file row, return file id
// ============================================================
static int64_t insert_file(SQLite::Database& db,
                            const std::string& path,
                            const std::string& language = "cpp") {
    SQLite::Statement ins(db,
        "INSERT INTO files (path, language, mtime_ns) VALUES (?, ?, 0)");
    ins.bind(1, path);
    ins.bind(2, language);
    ins.exec();
    return db.getLastInsertRowid();
}

// ============================================================
// Helper: insert a symbol row, return symbol id
// ============================================================
static int64_t insert_symbol(SQLite::Database& db,
                              int64_t file_id,
                              const std::string& name,
                              const std::string& kind = "function",
                              int line_start = 1, int line_end = 10) {
    SQLite::Statement ins(db,
        "INSERT INTO symbols (file_id, kind, name, line_start, line_end) "
        "VALUES (?, ?, ?, ?, ?)");
    ins.bind(1, file_id);
    ins.bind(2, kind);
    ins.bind(3, name);
    ins.bind(4, line_start);
    ins.bind(5, line_end);
    ins.exec();
    return db.getLastInsertRowid();
}

// ============================================================
// Helper: insert a call row
// ============================================================
static void insert_call(SQLite::Database& db,
                         int64_t file_id, int64_t caller_id,
                         const std::string& callee_name, int line) {
    SQLite::Statement ins(db,
        "INSERT INTO calls (caller_id, callee_name, file_id, line) "
        "VALUES (?, ?, ?, ?)");
    ins.bind(1, caller_id);
    ins.bind(2, callee_name);
    ins.bind(3, file_id);
    ins.bind(4, line);
    ins.exec();
}

// ============================================================
// Helper: build a get_call_graph JSON-RPC request
// ============================================================
static nlohmann::json make_cg_request(const std::string& name,
                                       const std::string& direction = "both",
                                       int depth = 1) {
    nlohmann::json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 1;
    req["method"]  = "get_call_graph";
    req["params"]["name"]      = name;
    req["params"]["direction"] = direction;
    req["params"]["depth"]     = depth;
    return req;
}

// ============================================================
// Minimal Coordinator stub for RequestRouter construction
// ============================================================
// We use the real RequestRouter but with a minimal Coordinator facade.
// Since tests don't run the event loop, we only need the Coordinator's
// db_ reference and a non-crashing get_status_json / get_language_support.
//
// Strategy: test the SQL queries directly without going through RequestRouter.
// This avoids linking the heavy Coordinator and efsw dependencies in test.
// The plan explicitly says: "prefer the direct SQL approach for simplicity".
// ============================================================

// ============================================================
// Helper: query get_call_graph directly using the same SQL logic
// as request_router.cpp, for testing without RequestRouter overhead.
// ============================================================

struct CallEdge {
    std::string name;
    std::string file;
    int line = 0;
    std::string source;
};

// Replicate the upgraded get_call_graph SQL logic for testing
static nlohmann::json query_call_graph_direct(
        SQLite::Database& db,
        const std::string& symbol_name,
        const std::string& direction = "both") {

    nlohmann::json result;
    result["name"]    = symbol_name;
    result["found"]   = false;
    result["callers"] = nlohmann::json::array();
    result["callees"] = nlohmann::json::array();

    // Find symbol
    SQLite::Statement sym_q(db,
        "SELECT s.id, s.file_id, f.path, s.line_start, s.line_end "
        "FROM symbols s JOIN files f ON s.file_id = f.id "
        "WHERE s.name = ? LIMIT 1");
    sym_q.bind(1, symbol_name);
    if (!sym_q.executeStep()) return result;

    result["found"]   = true;
    int64_t sym_id    = sym_q.getColumn(0).getInt64();
    int64_t file_id   = sym_q.getColumn(1).getInt64();
    std::string fpath = sym_q.getColumn(2).getString();
    int line_start    = sym_q.getColumn(3).getInt();
    int line_end      = sym_q.getColumn(4).getInt();

    (void)sym_id; // suppress unused warning for depth>1 case (not tested here)

    // Callees: try LSP-resolved first
    if (direction == "callees" || direction == "both") {
        try {
            SQLite::Statement q(db, R"sql(
                SELECT d.callee_name, d.def_file_path, d.def_line, d.source
                FROM lsp_definitions d
                WHERE d.caller_file_id = (SELECT id FROM files WHERE path = ?)
                  AND d.call_line BETWEEN ? AND ?
            )sql");
            q.bind(1, fpath);
            q.bind(2, line_start);
            q.bind(3, line_end);

            bool has_lsp_data = false;
            while (q.executeStep()) {
                has_lsp_data = true;
                nlohmann::json edge;
                edge["name"]   = q.getColumn(0).getString();
                edge["file"]   = q.getColumn(1).isNull() ? "" : q.getColumn(1).getString();
                edge["line"]   = q.getColumn(2).isNull() ? 0 : q.getColumn(2).getInt();
                edge["source"] = q.getColumn(3).getString();
                result["callees"].push_back(std::move(edge));
            }

            // Fallback to Tree-sitter if no LSP data
            if (!has_lsp_data) {
                SQLite::Statement fb(db,
                    "SELECT DISTINCT callee_name FROM calls WHERE file_id = ? "
                    "AND line BETWEEN ? AND ?");
                fb.bind(1, file_id);
                fb.bind(2, line_start);
                fb.bind(3, line_end);
                while (fb.executeStep()) {
                    nlohmann::json edge;
                    edge["name"]   = fb.getColumn(0).getString();
                    edge["file"]   = "";
                    edge["line"]   = 0;
                    edge["source"] = "tree-sitter-approximate";
                    result["callees"].push_back(std::move(edge));
                }
            }
        } catch (...) {
            // fallback to tree-sitter-approximate
            SQLite::Statement fb(db,
                "SELECT DISTINCT callee_name FROM calls WHERE file_id = ? "
                "AND line BETWEEN ? AND ?");
            fb.bind(1, file_id);
            fb.bind(2, line_start);
            fb.bind(3, line_end);
            while (fb.executeStep()) {
                nlohmann::json edge;
                edge["name"]   = fb.getColumn(0).getString();
                edge["file"]   = "";
                edge["line"]   = 0;
                edge["source"] = "tree-sitter-approximate";
                result["callees"].push_back(std::move(edge));
            }
        }
    }

    // Callers: try LSP-resolved first
    if (direction == "callers" || direction == "both") {
        try {
            SQLite::Statement q(db, R"sql(
                SELECT r.caller_file_path, r.caller_line, r.source,
                       COALESCE(
                           (SELECT s.name FROM symbols s
                            JOIN files f ON s.file_id = f.id
                            WHERE f.path = r.caller_file_path
                              AND s.line_start <= r.caller_line
                              AND s.line_end >= r.caller_line
                            LIMIT 1),
                           'unknown'
                       ) as caller_name
                FROM lsp_references r
                WHERE r.callee_file_id = (SELECT id FROM files WHERE path = ?)
                  AND r.callee_name = ?
            )sql");
            q.bind(1, fpath);
            q.bind(2, symbol_name);

            bool has_lsp_data = false;
            while (q.executeStep()) {
                has_lsp_data = true;
                nlohmann::json edge;
                edge["name"]   = q.getColumn(3).getString();
                edge["file"]   = q.getColumn(0).isNull() ? "" : q.getColumn(0).getString();
                edge["line"]   = q.getColumn(1).isNull() ? 0 : q.getColumn(1).getInt();
                edge["source"] = q.getColumn(2).getString();
                result["callers"].push_back(std::move(edge));
            }

            // Fallback to Tree-sitter if no LSP data
            if (!has_lsp_data) {
                SQLite::Statement fb(db,
                    "SELECT DISTINCT s.name FROM calls c "
                    "JOIN symbols s ON s.id = c.caller_id "
                    "WHERE c.callee_name = ?");
                fb.bind(1, symbol_name);
                while (fb.executeStep()) {
                    nlohmann::json edge;
                    edge["name"]   = fb.getColumn(0).getString();
                    edge["file"]   = "";
                    edge["line"]   = 0;
                    edge["source"] = "tree-sitter-approximate";
                    result["callers"].push_back(std::move(edge));
                }
            }
        } catch (...) {
            // tree-sitter-approximate fallback
            SQLite::Statement fb(db,
                "SELECT DISTINCT s.name FROM calls c "
                "JOIN symbols s ON s.id = c.caller_id "
                "WHERE c.callee_name = ?");
            fb.bind(1, symbol_name);
            while (fb.executeStep()) {
                nlohmann::json edge;
                edge["name"]   = fb.getColumn(0).getString();
                edge["file"]   = "";
                edge["line"]   = 0;
                edge["source"] = "tree-sitter-approximate";
                result["callers"].push_back(std::move(edge));
            }
        }
    }

    return result;
}

// ============================================================
// Test 1: LSP callees with source="lsp"
// ============================================================
static void test_lsp_callees() {
    auto db = make_test_db();

    int64_t fid_a = insert_file(db, "/test/a.cpp");
    int64_t fid_b = insert_file(db, "/test/b.cpp");
    insert_symbol(db, fid_a, "main", "function", 1, 10);

    // Insert LSP definition: main at line 5 calls helper in b.cpp line 20
    {
        SQLite::Statement ins(db,
            "INSERT INTO lsp_definitions "
            "(caller_file_id, call_line, call_col, callee_name, "
            " def_file_id, def_file_path, def_line, def_col, source) "
            "VALUES (?, 5, 0, 'helper', ?, '/test/b.cpp', 20, 0, 'lsp')");
        ins.bind(1, fid_a);
        ins.bind(2, fid_b);
        ins.exec();
    }

    auto result = query_call_graph_direct(db, "main", "callees");
    check(result["found"].get<bool>(), "Test1: symbol 'main' found");
    check(result["callees"].size() == 1, "Test1: exactly 1 callee");
    if (result["callees"].size() >= 1) {
        auto& edge = result["callees"][0];
        check(edge["name"].get<std::string>() == "helper", "Test1: callee name is 'helper'");
        check(edge["file"].get<std::string>() == "/test/b.cpp", "Test1: callee file is '/test/b.cpp'");
        check(edge["line"].get<int>() == 20, "Test1: callee line is 20");
        check(edge["source"].get<std::string>() == "lsp", "Test1: callee source is 'lsp'");
    }
}

// ============================================================
// Test 2: LSP callers with source="lsp"
// ============================================================
static void test_lsp_callers() {
    auto db = make_test_db();

    int64_t fid_a = insert_file(db, "/test/a.cpp");
    int64_t fid_b = insert_file(db, "/test/b.cpp");
    int64_t sym_b = insert_symbol(db, fid_b, "helper", "function", 1, 5);
    (void)sym_b;

    // Insert caller symbol in a.cpp
    insert_symbol(db, fid_a, "main", "function", 1, 20);

    // Insert LSP reference: helper in b.cpp is called from a.cpp line 10
    {
        SQLite::Statement ins(db,
            "INSERT INTO lsp_references "
            "(callee_file_id, callee_name, def_line, "
            " caller_file_id, caller_file_path, caller_line, caller_col, source) "
            "VALUES (?, 'helper', 1, ?, '/test/a.cpp', 10, 0, 'lsp')");
        ins.bind(1, fid_b);
        ins.bind(2, fid_a);
        ins.exec();
    }

    auto result = query_call_graph_direct(db, "helper", "callers");
    check(result["found"].get<bool>(), "Test2: symbol 'helper' found");
    check(result["callers"].size() == 1, "Test2: exactly 1 caller");
    if (result["callers"].size() >= 1) {
        auto& edge = result["callers"][0];
        check(edge["file"].get<std::string>() == "/test/a.cpp", "Test2: caller file is '/test/a.cpp'");
        check(edge["line"].get<int>() == 10, "Test2: caller line is 10");
        check(edge["source"].get<std::string>() == "lsp", "Test2: caller source is 'lsp'");
    }
}

// ============================================================
// Test 3: Fallback to tree-sitter-approximate when no LSP data
// ============================================================
static void test_fallback_tree_sitter() {
    auto db = make_test_db();

    int64_t fid_a = insert_file(db, "/test/a.cpp");
    int64_t sym_a = insert_symbol(db, fid_a, "doWork", "function", 1, 15);

    // Insert call via tree-sitter (calls table only, no lsp_definitions)
    insert_call(db, fid_a, sym_a, "helperFunc", 5);

    auto result = query_call_graph_direct(db, "doWork", "callees");
    check(result["found"].get<bool>(), "Test3: symbol 'doWork' found");
    check(result["callees"].size() >= 1, "Test3: at least 1 callee from tree-sitter");
    bool found_helper = false;
    for (const auto& edge : result["callees"]) {
        if (edge["name"].get<std::string>() == "helperFunc") {
            found_helper = true;
            check(edge["source"].get<std::string>() == "tree-sitter-approximate",
                  "Test3: fallback source is 'tree-sitter-approximate'");
            check(edge["file"].get<std::string>().empty(),
                  "Test3: fallback file is empty string");
            check(edge["line"].get<int>() == 0,
                  "Test3: fallback line is 0");
        }
    }
    check(found_helper, "Test3: 'helperFunc' found in callees via tree-sitter fallback");
}

// ============================================================
// Test 4: Empty tables return empty arrays (not error)
// ============================================================
static void test_empty_tables() {
    auto db = make_test_db();

    int64_t fid = insert_file(db, "/test/empty.cpp");
    insert_symbol(db, fid, "emptyFn", "function", 1, 5);

    // No calls, no lsp_definitions, no lsp_references
    auto result = query_call_graph_direct(db, "emptyFn", "both");
    check(result["found"].get<bool>(), "Test4: symbol 'emptyFn' found");
    check(result["callees"].is_array(), "Test4: callees is array");
    check(result["callers"].is_array(), "Test4: callers is array");
    check(result["callees"].size() == 0, "Test4: callees is empty");
    check(result["callers"].size() == 0, "Test4: callers is empty");
}

// ============================================================
// Test 5: direction parameter respected
// ============================================================
static void test_direction_parameter() {
    auto db = make_test_db();

    int64_t fid = insert_file(db, "/test/a.cpp");
    int64_t fid2 = insert_file(db, "/test/b.cpp");
    insert_symbol(db, fid, "func1", "function", 1, 10);

    // Add both a callee and a caller reference
    {
        SQLite::Statement ins(db,
            "INSERT INTO lsp_definitions "
            "(caller_file_id, call_line, callee_name, def_file_path, def_line, source) "
            "VALUES (?, 5, 'func2', '/test/b.cpp', 1, 'lsp')");
        ins.bind(1, fid);
        ins.exec();
    }
    {
        SQLite::Statement ins(db,
            "INSERT INTO lsp_references "
            "(callee_file_id, callee_name, def_line, caller_file_path, caller_line, source) "
            "VALUES (?, 'func1', 1, '/test/b.cpp', 7, 'lsp')");
        ins.bind(1, fid);
        ins.exec();
    }
    (void)fid2;

    // direction=callees: should only have callees
    auto r_callees = query_call_graph_direct(db, "func1", "callees");
    check(r_callees["callees"].size() == 1, "Test5: callees direction returns 1 callee");
    check(r_callees["callers"].size() == 0, "Test5: callees direction returns 0 callers");

    // direction=callers: should only have callers
    auto r_callers = query_call_graph_direct(db, "func1", "callers");
    check(r_callers["callers"].size() == 1, "Test5: callers direction returns 1 caller");
    check(r_callers["callees"].size() == 0, "Test5: callers direction returns 0 callees");

    // direction=both: should have both
    auto r_both = query_call_graph_direct(db, "func1", "both");
    check(r_both["callees"].size() == 1, "Test5: both direction returns 1 callee");
    check(r_both["callers"].size() == 1, "Test5: both direction returns 1 caller");
}

// ============================================================
// Test 6: uri_to_path strips "file://" prefix correctly
// ============================================================
static void test_uri_to_path() {
    using codetldr::LspCallGraphResolver;

    std::string r1 = LspCallGraphResolver::uri_to_path("file:///abs/path/to/file.cpp");
    check(r1 == "/abs/path/to/file.cpp", "Test6: file:///abs/path/to/file.cpp -> /abs/path/to/file.cpp");

    std::string r2 = LspCallGraphResolver::uri_to_path("file:///home/user/project/src/main.cpp");
    check(r2 == "/home/user/project/src/main.cpp",
          "Test6: strips file:// leaving absolute path");
    check(!r2.empty() && r2[0] == '/', "Test6: result starts with /");

    // Non-file:// URI passes through unchanged
    std::string r3 = LspCallGraphResolver::uri_to_path("/already/absolute/path.cpp");
    check(r3 == "/already/absolute/path.cpp", "Test6: non-URI passes through unchanged");
}

// ============================================================
// Test 7: path_to_uri prepends "file://"
// ============================================================
static void test_path_to_uri() {
    using codetldr::LspCallGraphResolver;

    std::string uri = LspCallGraphResolver::path_to_uri("/abs/path/to/file.cpp");
    check(uri == "file:///abs/path/to/file.cpp", "Test7: path_to_uri prepends file://");
    check(uri.substr(0, 7) == "file://", "Test7: result starts with file://");
}

// ============================================================
// Test 8: LSP data takes priority over tree-sitter when both exist
// ============================================================
static void test_lsp_priority_over_tree_sitter() {
    auto db = make_test_db();

    int64_t fid_a = insert_file(db, "/test/a.cpp");
    int64_t fid_b = insert_file(db, "/test/b.cpp");
    int64_t sym_a = insert_symbol(db, fid_a, "process", "function", 1, 20);

    // Add tree-sitter call (approximate)
    insert_call(db, fid_a, sym_a, "helper", 5);

    // Add LSP definition (precise)
    {
        SQLite::Statement ins(db,
            "INSERT INTO lsp_definitions "
            "(caller_file_id, call_line, callee_name, def_file_id, def_file_path, def_line, source) "
            "VALUES (?, 5, 'helper', ?, '/test/b.cpp', 42, 'lsp')");
        ins.bind(1, fid_a);
        ins.bind(2, fid_b);
        ins.exec();
    }

    auto result = query_call_graph_direct(db, "process", "callees");
    check(result["found"].get<bool>(), "Test8: symbol 'process' found");
    // LSP data should win — should show source="lsp" not "tree-sitter-approximate"
    bool found_lsp = false;
    bool found_ts = false;
    for (const auto& edge : result["callees"]) {
        if (edge["source"].get<std::string>() == "lsp") found_lsp = true;
        if (edge["source"].get<std::string>() == "tree-sitter-approximate") found_ts = true;
    }
    check(found_lsp, "Test8: LSP source found in callees");
    check(!found_ts, "Test8: tree-sitter-approximate NOT present when LSP data exists");
}

// ============================================================
// main
// ============================================================
int main() {
    std::cout << "=== test_lsp_call_graph ===\n\n";

    test_lsp_callees();
    test_lsp_callers();
    test_fallback_tree_sitter();
    test_empty_tables();
    test_direction_parameter();
    test_uri_to_path();
    test_path_to_uri();
    test_lsp_priority_over_tree_sitter();

    std::cout << "\n=== Results: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail > 0 ? 1 : 0;
}
