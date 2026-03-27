#include "storage/database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void assert_true(bool cond, const std::string& msg) {
    if (!cond) {
        std::cerr << "ASSERTION FAILED: " << msg << "\n";
        std::exit(1);
    }
}

// Collect column names from PRAGMA table_info
static std::vector<std::string> table_columns(SQLite::Database& db, const std::string& table) {
    std::vector<std::string> cols;
    SQLite::Statement q(db, "PRAGMA table_info(" + table + ")");
    while (q.executeStep()) {
        cols.push_back(q.getColumn(1).getString()); // column 1 = name
    }
    return cols;
}

static bool has_column(const std::vector<std::string>& cols, const std::string& name) {
    for (const auto& c : cols) {
        if (c == name) return true;
    }
    return false;
}

int main() {
    // Create temp database path
    fs::path tmp_dir = fs::temp_directory_path() / "codetldr_schema_test";
    fs::create_directories(tmp_dir);
    fs::path db_path = tmp_dir / "test_v2v3.sqlite";
    // Remove if exists from previous run
    fs::remove(db_path);

    // Open database - runs migrations
    auto db = codetldr::Database::open(db_path);

    // Test: schema version == 6 (migration v6 adds dfg_edges table)
    assert_true(db.schema_version() == 6,
        "schema_version should be 6 after migrations, got " + std::to_string(db.schema_version()));
    std::cout << "PASS: schema_version == 6\n";

    // Test: symbols table columns
    {
        auto cols = table_columns(db.raw(), "symbols");
        assert_true(!cols.empty(), "symbols table should exist");
        assert_true(has_column(cols, "id"), "symbols.id missing");
        assert_true(has_column(cols, "file_id"), "symbols.file_id missing");
        assert_true(has_column(cols, "kind"), "symbols.kind missing");
        assert_true(has_column(cols, "name"), "symbols.name missing");
        assert_true(has_column(cols, "signature"), "symbols.signature missing");
        assert_true(has_column(cols, "line_start"), "symbols.line_start missing");
        assert_true(has_column(cols, "line_end"), "symbols.line_end missing");
        assert_true(has_column(cols, "documentation"), "symbols.documentation missing");
        assert_true(has_column(cols, "indexed_at"), "symbols.indexed_at missing");
        std::cout << "PASS: symbols table has correct columns\n";
    }

    // Test: calls table columns
    {
        auto cols = table_columns(db.raw(), "calls");
        assert_true(!cols.empty(), "calls table should exist");
        assert_true(has_column(cols, "id"), "calls.id missing");
        assert_true(has_column(cols, "caller_id"), "calls.caller_id missing");
        assert_true(has_column(cols, "callee_id"), "calls.callee_id missing");
        assert_true(has_column(cols, "callee_name"), "calls.callee_name missing");
        assert_true(has_column(cols, "file_id"), "calls.file_id missing");
        assert_true(has_column(cols, "line"), "calls.line missing");
        std::cout << "PASS: calls table has correct columns\n";
    }

    // Test: cfg_nodes table columns
    {
        auto cols = table_columns(db.raw(), "cfg_nodes");
        assert_true(!cols.empty(), "cfg_nodes table should exist");
        assert_true(has_column(cols, "id"), "cfg_nodes.id missing");
        assert_true(has_column(cols, "file_id"), "cfg_nodes.file_id missing");
        assert_true(has_column(cols, "symbol_id"), "cfg_nodes.symbol_id missing");
        assert_true(has_column(cols, "node_type"), "cfg_nodes.node_type missing");
        assert_true(has_column(cols, "condition"), "cfg_nodes.condition missing");
        assert_true(has_column(cols, "line"), "cfg_nodes.line missing");
        assert_true(has_column(cols, "depth"), "cfg_nodes.depth missing");
        std::cout << "PASS: cfg_nodes table has correct columns\n";
    }

    // Test: dfg_edges table columns (migration v6)
    {
        auto cols = table_columns(db.raw(), "dfg_edges");
        assert_true(!cols.empty(), "dfg_edges table should exist");
        assert_true(has_column(cols, "id"), "dfg_edges.id missing");
        assert_true(has_column(cols, "file_id"), "dfg_edges.file_id missing");
        assert_true(has_column(cols, "symbol_id"), "dfg_edges.symbol_id missing");
        assert_true(has_column(cols, "edge_type"), "dfg_edges.edge_type missing");
        assert_true(has_column(cols, "lhs"), "dfg_edges.lhs missing");
        assert_true(has_column(cols, "rhs_snippet"), "dfg_edges.rhs_snippet missing");
        assert_true(has_column(cols, "line"), "dfg_edges.line missing");
        assert_true(cols.size() == 7, "dfg_edges should have 7 columns");
        std::cout << "PASS: dfg_edges table has correct columns\n";
    }

    // Test: INSERT into files, then symbols, then calls
    {
        auto& raw = db.raw();
        // Insert a file
        raw.exec("INSERT INTO files (path, language, mtime_ns) VALUES ('/test/file.py', 'python', 0)");
        int64_t file_id = raw.getLastInsertRowid();

        // Insert a symbol
        SQLite::Statement ins_sym(raw,
            "INSERT INTO symbols (file_id, kind, name, signature, line_start, line_end) "
            "VALUES (?, 'function', 'test_func', 'test_func()', 1, 5)");
        ins_sym.bind(1, static_cast<int64_t>(file_id));
        ins_sym.exec();
        int64_t sym_id = raw.getLastInsertRowid();

        // Insert a call
        SQLite::Statement ins_call(raw,
            "INSERT INTO calls (caller_id, callee_name, file_id, line) VALUES (?, 'other_func', ?, 3)");
        ins_call.bind(1, static_cast<int64_t>(sym_id));
        ins_call.bind(2, static_cast<int64_t>(file_id));
        ins_call.exec();

        std::cout << "PASS: symbols and calls tables accept valid INSERTs\n";
    }

    // Cleanup
    fs::remove_all(tmp_dir);

    std::cout << "All schema_v2v3 tests PASSED\n";
    return 0;
}
