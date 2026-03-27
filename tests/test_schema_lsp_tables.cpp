#include "storage/database.h"
#include "storage/schema.h"
#include "analysis/pipeline.h"
#include "analysis/tree_sitter/language_registry.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <filesystem>
#include <fstream>
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

// Check if an index exists in sqlite_master
static bool has_index(SQLite::Database& db, const std::string& idx_name) {
    SQLite::Statement q(db,
        "SELECT 1 FROM sqlite_master WHERE type='index' AND name=?");
    q.bind(1, idx_name);
    return q.executeStep();
}

int main() {
    // Create temp database path
    fs::path tmp_dir = fs::temp_directory_path() / "codetldr_lsp_tables_test";
    fs::create_directories(tmp_dir);
    fs::path db_path = tmp_dir / "test_lsp_tables.sqlite";
    // Remove if exists from previous run
    fs::remove(db_path);

    // === Apply all migrations via Database::open() ===
    auto db = codetldr::Database::open(db_path);

    // Verify we're at version 10
    assert_true(db.schema_version() == 10,
        "schema_version should be 10 after all migrations, got " + std::to_string(db.schema_version()));
    std::cout << "PASS: schema_version() == 10 after all migrations\n";

    // === Test 1: lsp_definitions table exists with correct columns ===
    {
        auto cols = table_columns(db.raw(), "lsp_definitions");
        assert_true(!cols.empty(), "lsp_definitions table should exist after migration 8");
        assert_true(has_column(cols, "id"),             "lsp_definitions.id missing");
        assert_true(has_column(cols, "caller_file_id"), "lsp_definitions.caller_file_id missing");
        assert_true(has_column(cols, "call_line"),      "lsp_definitions.call_line missing");
        assert_true(has_column(cols, "call_col"),       "lsp_definitions.call_col missing");
        assert_true(has_column(cols, "callee_name"),    "lsp_definitions.callee_name missing");
        assert_true(has_column(cols, "def_file_id"),    "lsp_definitions.def_file_id missing");
        assert_true(has_column(cols, "def_file_path"),  "lsp_definitions.def_file_path missing");
        assert_true(has_column(cols, "def_line"),       "lsp_definitions.def_line missing");
        assert_true(has_column(cols, "def_col"),        "lsp_definitions.def_col missing");
        assert_true(has_column(cols, "source"),         "lsp_definitions.source missing");
        assert_true(cols.size() == 10,
            "lsp_definitions should have 10 columns, got " + std::to_string(cols.size()));
        std::cout << "PASS: lsp_definitions table has all 10 correct columns\n";
    }

    // === Test 2: lsp_references table exists with correct columns ===
    {
        auto cols = table_columns(db.raw(), "lsp_references");
        assert_true(!cols.empty(), "lsp_references table should exist after migration 9");
        assert_true(has_column(cols, "id"),               "lsp_references.id missing");
        assert_true(has_column(cols, "callee_file_id"),   "lsp_references.callee_file_id missing");
        assert_true(has_column(cols, "callee_name"),      "lsp_references.callee_name missing");
        assert_true(has_column(cols, "def_line"),         "lsp_references.def_line missing");
        assert_true(has_column(cols, "caller_file_id"),   "lsp_references.caller_file_id missing");
        assert_true(has_column(cols, "caller_file_path"), "lsp_references.caller_file_path missing");
        assert_true(has_column(cols, "caller_line"),      "lsp_references.caller_line missing");
        assert_true(has_column(cols, "caller_col"),       "lsp_references.caller_col missing");
        assert_true(has_column(cols, "source"),           "lsp_references.source missing");
        assert_true(cols.size() == 9,
            "lsp_references should have 9 columns, got " + std::to_string(cols.size()));
        std::cout << "PASS: lsp_references table has all 9 correct columns\n";
    }

    // === Test 3: Required indexes exist ===
    {
        assert_true(has_index(db.raw(), "idx_lsp_def_caller"),
            "idx_lsp_def_caller index missing");
        assert_true(has_index(db.raw(), "idx_lsp_def_callee"),
            "idx_lsp_def_callee index missing");
        assert_true(has_index(db.raw(), "idx_lsp_ref_callee"),
            "idx_lsp_ref_callee index missing");
        assert_true(has_index(db.raw(), "idx_lsp_ref_caller"),
            "idx_lsp_ref_caller index missing");
        std::cout << "PASS: all 4 LSP indexes exist\n";
    }

    // === Test 4: INSERT into lsp_definitions with source='lsp' succeeds; source defaults to 'lsp' ===
    {
        // Insert a file first (required for FK)
        db.raw().exec(
            "INSERT INTO files (path, language, mtime_ns) VALUES ('/test/caller.cpp', 'c++', 0)");
        int64_t caller_id = db.raw().getLastInsertRowid();

        db.raw().exec(
            "INSERT INTO files (path, language, mtime_ns) VALUES ('/test/callee.cpp', 'c++', 0)");
        int64_t def_id = db.raw().getLastInsertRowid();

        // Insert with explicit source='lsp'
        SQLite::Statement ins(db.raw(),
            "INSERT INTO lsp_definitions (caller_file_id, call_line, call_col, callee_name, "
            "def_file_id, def_file_path, def_line, def_col, source) "
            "VALUES (?, 10, 5, 'myFunc', ?, '/test/callee.cpp', 20, 3, 'lsp')");
        ins.bind(1, caller_id);
        ins.bind(2, def_id);
        ins.exec();

        // Insert without source (should default to 'lsp')
        SQLite::Statement ins2(db.raw(),
            "INSERT INTO lsp_definitions (caller_file_id, call_line, callee_name) "
            "VALUES (?, 15, 'otherFunc')");
        ins2.bind(1, caller_id);
        ins2.exec();

        // Verify source default
        SQLite::Statement chk(db.raw(),
            "SELECT source FROM lsp_definitions WHERE callee_name='otherFunc'");
        assert_true(chk.executeStep(), "should find inserted otherFunc row");
        std::string src = chk.getColumn(0).getString();
        assert_true(src == "lsp",
            "source default should be 'lsp', got '" + src + "'");
        std::cout << "PASS: lsp_definitions source defaults to 'lsp'\n";
    }

    // === Test 5: DELETE FROM files CASCADE to lsp_definitions ===
    {
        // Add another caller file and lsp_definition entry
        db.raw().exec(
            "INSERT INTO files (path, language, mtime_ns) VALUES ('/test/cascade_test.cpp', 'c++', 0)");
        int64_t cascade_id = db.raw().getLastInsertRowid();

        SQLite::Statement ins(db.raw(),
            "INSERT INTO lsp_definitions (caller_file_id, call_line, callee_name) "
            "VALUES (?, 1, 'willCascade')");
        ins.bind(1, cascade_id);
        ins.exec();

        // Verify it was inserted
        {
            SQLite::Statement chk(db.raw(),
                "SELECT COUNT(*) FROM lsp_definitions WHERE callee_name='willCascade'");
            assert_true(chk.executeStep(), "count query should work");
            assert_true(chk.getColumn(0).getInt() == 1, "lsp_definitions entry should exist before cascade");
        }

        // Enable foreign keys and delete the file
        db.raw().exec("PRAGMA foreign_keys = ON");
        SQLite::Statement del(db.raw(), "DELETE FROM files WHERE id=?");
        del.bind(1, cascade_id);
        del.exec();

        // Verify cascade deleted the lsp_definitions rows
        {
            SQLite::Statement chk(db.raw(),
                "SELECT COUNT(*) FROM lsp_definitions WHERE callee_name='willCascade'");
            assert_true(chk.executeStep(), "count query should work after cascade");
            assert_true(chk.getColumn(0).getInt() == 0,
                "lsp_definitions should be cascade-deleted when caller_file is deleted");
        }
        std::cout << "PASS: lsp_definitions CASCADE deletes when caller_file deleted\n";
    }

    // === Test 6: analyze_file() populates content_hash in files table ===
    {
        // Write a temp C++ source file
        fs::path src_file = tmp_dir / "hello.cpp";
        {
            std::ofstream f(src_file);
            f << "int main() { return 0; }\n";
        }

        // Open a fresh in-memory-style DB in the tmp_dir for pipeline test
        fs::path pipeline_db_path = tmp_dir / "pipeline_test.sqlite";
        auto pipeline_db = codetldr::Database::open(pipeline_db_path);

        codetldr::LanguageRegistry registry;
        registry.initialize();
        auto result = codetldr::analyze_file(pipeline_db.raw(), registry, src_file);
        assert_true(result.success, "analyze_file should succeed on valid C++ file: " + result.error);

        // Query content_hash from files table
        SQLite::Statement q(pipeline_db.raw(),
            "SELECT content_hash FROM files WHERE path = ?");
        q.bind(1, src_file.string());
        assert_true(q.executeStep(), "files table should contain analyzed file");
        std::string stored_hash = q.getColumn(0).getString();

        assert_true(!stored_hash.empty(), "content_hash should not be empty after analyze_file");
        assert_true(stored_hash.size() == 64,
            "content_hash should be 64 hex chars, got " + std::to_string(stored_hash.size()));

        // Verify result.content_hash matches stored hash
        assert_true(result.content_hash == stored_hash,
            "AnalysisResult.content_hash should match stored hash");

        std::cout << "PASS: content_hash is a 64-char hex string in files table after analyze_file\n";
    }

    // Cleanup
    fs::remove_all(tmp_dir);

    std::cout << "All schema_lsp_tables tests PASSED\n";
    return 0;
}
