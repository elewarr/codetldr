#include "storage/database.h"
#include "storage/schema.h"
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
    fs::path tmp_dir = fs::temp_directory_path() / "codetldr_cfg_upgrade_test";
    fs::create_directories(tmp_dir);
    fs::path db_path = tmp_dir / "test_v4_to_v6.sqlite";
    // Remove if exists from previous run
    fs::remove(db_path);

    // === Seed a v1.1 database (schema_version = 4) ===
    // Apply only kMigrations[0..3] (versions 1-4): files, symbols, calls, symbols_fts
    {
        SQLite::Database raw_db(db_path.string(),
            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);

        // Create schema_version table manually (same DDL as run_migrations)
        raw_db.exec(
            "CREATE TABLE IF NOT EXISTS schema_version ("
            "  version INTEGER PRIMARY KEY,"
            "  applied TEXT NOT NULL DEFAULT (datetime('now'))"
            ")"
        );

        // Apply only versions 1-4 (simulate v1.1 state — no CFG or DFG tables)
        for (const auto& [ver, sql] : codetldr::kMigrations) {
            if (ver > 4) break;

            SQLite::Transaction txn(raw_db);
            raw_db.exec(std::string(sql));
            SQLite::Statement ins(raw_db, "INSERT INTO schema_version (version) VALUES (?)");
            ins.bind(1, ver);
            ins.exec();
            txn.commit();
        }

        // Verify seed state: version should be 4
        SQLite::Statement q(raw_db, "SELECT MAX(version) FROM schema_version");
        assert_true(q.executeStep(), "schema_version query should return a row");
        int seed_ver = q.getColumn(0).getInt();
        assert_true(seed_ver == 4,
            "Seed database should be at version 4, got " + std::to_string(seed_ver));
        std::cout << "PASS: seeded database at schema_version == 4\n";
    }
    // raw_db goes out of scope here — file closed before Database::open()

    // === Test 1: Database::open() upgrades v4 -> v9 transparently ===
    auto db = codetldr::Database::open(db_path);

    assert_true(db.schema_version() == 9,
        "schema_version should be 9 after upgrade, got " + std::to_string(db.schema_version()));
    std::cout << "PASS: schema_version() == 9 after v4 -> v9 upgrade\n";

    // === Test 2: cfg_nodes table exists with all 7 columns ===
    {
        auto cols = table_columns(db.raw(), "cfg_nodes");
        assert_true(!cols.empty(), "cfg_nodes table should exist after upgrade");
        assert_true(has_column(cols, "id"),        "cfg_nodes.id missing");
        assert_true(has_column(cols, "file_id"),   "cfg_nodes.file_id missing");
        assert_true(has_column(cols, "symbol_id"), "cfg_nodes.symbol_id missing");
        assert_true(has_column(cols, "node_type"), "cfg_nodes.node_type missing");
        assert_true(has_column(cols, "condition"), "cfg_nodes.condition missing");
        assert_true(has_column(cols, "line"),      "cfg_nodes.line missing");
        assert_true(has_column(cols, "depth"),     "cfg_nodes.depth missing");
        assert_true(cols.size() == 7,
            "cfg_nodes should have 7 columns, got " + std::to_string(cols.size()));
        std::cout << "PASS: cfg_nodes table has all 7 correct columns\n";
    }

    // === Test 3: dfg_edges table also exists (full v4->v7 migration chain) ===
    {
        auto cols = table_columns(db.raw(), "dfg_edges");
        assert_true(!cols.empty(), "dfg_edges table should exist after full upgrade");
        assert_true(has_column(cols, "id"),          "dfg_edges.id missing");
        assert_true(has_column(cols, "file_id"),     "dfg_edges.file_id missing");
        assert_true(has_column(cols, "symbol_id"),   "dfg_edges.symbol_id missing");
        assert_true(has_column(cols, "edge_type"),   "dfg_edges.edge_type missing");
        assert_true(has_column(cols, "lhs"),         "dfg_edges.lhs missing");
        assert_true(has_column(cols, "rhs_snippet"), "dfg_edges.rhs_snippet missing");
        assert_true(has_column(cols, "line"),        "dfg_edges.line missing");
        std::cout << "PASS: dfg_edges table exists after full v4->v7 migration chain\n";
    }

    // === Test 4: embedded_files table also exists (migration v7) ===
    {
        auto cols = table_columns(db.raw(), "embedded_files");
        assert_true(!cols.empty(), "embedded_files table should exist after v7 upgrade");
        assert_true(has_column(cols, "id"),          "embedded_files.id missing");
        assert_true(has_column(cols, "symbol_id"),   "embedded_files.symbol_id missing");
        assert_true(has_column(cols, "file_id"),     "embedded_files.file_id missing");
        assert_true(has_column(cols, "chunk_index"), "embedded_files.chunk_index missing");
        assert_true(has_column(cols, "embedded_at"), "embedded_files.embedded_at missing");
        std::cout << "PASS: embedded_files table exists after v7 upgrade\n";
    }

    // Cleanup
    fs::remove_all(tmp_dir);

    std::cout << "All cfg_schema_upgrade tests PASSED\n";
    return 0;
}
