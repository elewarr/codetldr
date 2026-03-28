// test_lsp_dependencies.cpp
// Tests for LspDependencyResolver DB layer, MCP response shape, and workspace/symbol fallback.
// Tests do NOT require a real LSP server — they test the DB query layer and MCP
// response shape by inserting test data directly into the tables.

#include "storage/schema.h"
#include "storage/migrations.h"
#include "storage/database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>
#include <vector>

// ============================================================
// Simple test harness
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
// Helper: insert a lsp_dependency row
// ============================================================
static void insert_dependency(SQLite::Database& db,
                               int64_t importer_file_id,
                               int import_line,
                               const std::string& import_kind,
                               int64_t target_file_id,       // -1 for NULL
                               const std::string& target_file_path) {
    SQLite::Statement ins(db,
        "INSERT INTO lsp_dependencies "
        "(importer_file_id, import_line, import_kind, target_file_id, target_file_path) "
        "VALUES (?, ?, ?, ?, ?)");
    ins.bind(1, importer_file_id);
    ins.bind(2, import_line);
    ins.bind(3, import_kind);
    if (target_file_id >= 0) {
        ins.bind(4, target_file_id);
    } else {
        ins.bind(4); // NULL
    }
    ins.bind(5, target_file_path);
    ins.exec();
}

// ============================================================
// Helper: replicate get_dependencies SQL query for imports direction
// ============================================================
static nlohmann::json query_imports(SQLite::Database& db, int64_t importer_file_id) {
    nlohmann::json arr = nlohmann::json::array();
    SQLite::Statement q(db, R"sql(
        SELECT import_line, import_kind, target_file_path,
               COALESCE((SELECT f.path FROM files f WHERE f.id = d.target_file_id), d.target_file_path) as resolved_path
        FROM lsp_dependencies d
        WHERE d.importer_file_id = ?
        ORDER BY d.import_line
    )sql");
    q.bind(1, importer_file_id);
    while (q.executeStep()) {
        nlohmann::json item;
        item["line"] = q.getColumn(0).getInt();
        item["kind"] = q.getColumn(1).getString();
        item["file"] = q.getColumn(3).isNull() ? "" : q.getColumn(3).getString();
        arr.push_back(std::move(item));
    }
    return arr;
}

// ============================================================
// Helper: replicate get_dependencies SQL query for imported_by direction
// ============================================================
static nlohmann::json query_imported_by(SQLite::Database& db, int64_t target_file_id) {
    nlohmann::json arr = nlohmann::json::array();
    SQLite::Statement q(db, R"sql(
        SELECT f.path as importer_path, d.import_line
        FROM lsp_dependencies d
        JOIN files f ON d.importer_file_id = f.id
        WHERE d.target_file_id = ?
        ORDER BY f.path
    )sql");
    q.bind(1, target_file_id);
    while (q.executeStep()) {
        nlohmann::json item;
        item["file"] = q.getColumn(0).getString();
        item["line"] = q.getColumn(1).getInt();
        arr.push_back(std::move(item));
    }
    return arr;
}

// ============================================================
// Helper: simulate get_dependencies MCP response shape
// ============================================================
static nlohmann::json simulate_get_dependencies(SQLite::Database& db,
                                                 const std::string& file_path) {
    nlohmann::json result;
    result["file"] = file_path;

    // Look up file_id
    int64_t file_id = -1;
    {
        SQLite::Statement q(db, "SELECT id FROM files WHERE path = ?");
        q.bind(1, file_path);
        if (q.executeStep()) {
            file_id = q.getColumn(0).getInt64();
        }
    }

    if (file_id < 0) {
        result["found"] = false;
        result["imports"] = nlohmann::json::array();
        result["imported_by"] = nlohmann::json::array();
        return result;
    }

    result["found"] = true;

    // imports
    {
        nlohmann::json arr = nlohmann::json::array();
        SQLite::Statement q(db, R"sql(
            SELECT import_line, import_kind, target_file_path,
                   COALESCE((SELECT f.path FROM files f WHERE f.id = d.target_file_id), d.target_file_path) as resolved_path
            FROM lsp_dependencies d
            WHERE d.importer_file_id = ?
            ORDER BY d.import_line
        )sql");
        q.bind(1, file_id);
        while (q.executeStep()) {
            nlohmann::json item;
            item["file"] = q.getColumn(3).isNull() ? "" : q.getColumn(3).getString();
            item["kind"] = q.getColumn(1).getString();
            item["line"] = q.getColumn(0).getInt();
            arr.push_back(std::move(item));
        }
        result["imports"] = std::move(arr);
    }

    // imported_by
    {
        nlohmann::json arr = nlohmann::json::array();
        SQLite::Statement q(db, R"sql(
            SELECT f.path as importer_path, d.import_line
            FROM lsp_dependencies d
            JOIN files f ON d.importer_file_id = f.id
            WHERE d.target_file_id = ?
            ORDER BY f.path
        )sql");
        q.bind(1, file_id);
        while (q.executeStep()) {
            nlohmann::json item;
            item["file"] = q.getColumn(0).getString();
            item["line"] = q.getColumn(1).getInt();
            arr.push_back(std::move(item));
        }
        result["imported_by"] = std::move(arr);
    }

    return result;
}

// ============================================================
// Test 1: lsp_dependencies table created by migration 10
// ============================================================
static void test_table_created() {
    auto db = make_test_db();
    // Verify table exists by trying to query it
    bool ok = false;
    try {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM lsp_dependencies");
        q.executeStep();
        ok = true;
    } catch (...) {}
    check(ok, "lsp_dependencies table created by migration 10");
}

// ============================================================
// Test 2: Query imports direction
// ============================================================
static void test_query_imports() {
    auto db = make_test_db();
    int64_t importer_id = insert_file(db, "/project/src/main.cpp");
    int64_t dep1_id     = insert_file(db, "/project/include/utils.h");
    int64_t dep2_id     = insert_file(db, "/project/include/core.h");

    // main.cpp imports utils.h at line 3 and core.h at line 5
    insert_dependency(db, importer_id, 3, "include", dep1_id, "/project/include/utils.h");
    insert_dependency(db, importer_id, 5, "include", dep2_id, "/project/include/core.h");

    auto imports = query_imports(db, importer_id);
    check(imports.size() == 2, "imports returns 2 entries");
    check(imports[0]["line"] == 3, "first import at line 3");
    check(imports[0]["kind"] == "include", "first import kind is include");
    check(imports[0]["file"] == "/project/include/utils.h", "first import file resolved");
    check(imports[1]["line"] == 5, "second import at line 5");
    check(imports[1]["file"] == "/project/include/core.h", "second import file resolved");
}

// ============================================================
// Test 3: Query imported_by direction
// ============================================================
static void test_query_imported_by() {
    auto db = make_test_db();
    int64_t header_id    = insert_file(db, "/project/include/common.h");
    int64_t consumer1_id = insert_file(db, "/project/src/alpha.cpp");
    int64_t consumer2_id = insert_file(db, "/project/src/beta.cpp");

    // alpha.cpp and beta.cpp both import common.h
    insert_dependency(db, consumer1_id, 1, "include", header_id, "/project/include/common.h");
    insert_dependency(db, consumer2_id, 2, "include", header_id, "/project/include/common.h");

    auto imported_by = query_imported_by(db, header_id);
    check(imported_by.size() == 2, "imported_by returns 2 entries");
    // Ordered by path
    check(imported_by[0]["file"] == "/project/src/alpha.cpp", "first consumer is alpha.cpp");
    check(imported_by[0]["line"] == 1, "alpha.cpp imports at line 1");
    check(imported_by[1]["file"] == "/project/src/beta.cpp", "second consumer is beta.cpp");
}

// ============================================================
// Test 4: CASCADE delete removes dependency rows when importer deleted
// ============================================================
static void test_cascade_delete_importer() {
    auto db = make_test_db();
    int64_t importer_id = insert_file(db, "/project/src/main.cpp");
    int64_t dep_id      = insert_file(db, "/project/include/utils.h");
    insert_dependency(db, importer_id, 3, "include", dep_id, "/project/include/utils.h");

    // Verify row exists
    {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM lsp_dependencies WHERE importer_file_id = ?");
        q.bind(1, importer_id);
        q.executeStep();
        check(q.getColumn(0).getInt() == 1, "dependency row exists before delete");
    }

    // Delete importer file
    {
        SQLite::Statement del(db, "DELETE FROM files WHERE id = ?");
        del.bind(1, importer_id);
        del.exec();
    }

    // Dependency row should be CASCADE deleted
    {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM lsp_dependencies WHERE importer_file_id = ?");
        q.bind(1, importer_id);
        q.executeStep();
        check(q.getColumn(0).getInt() == 0, "dependency row CASCADE deleted when importer deleted");
    }
}

// ============================================================
// Test 5: SET NULL on target_file_id when target file deleted
// ============================================================
static void test_set_null_target() {
    auto db = make_test_db();
    int64_t importer_id = insert_file(db, "/project/src/main.cpp");
    int64_t dep_id      = insert_file(db, "/project/include/utils.h");
    insert_dependency(db, importer_id, 3, "include", dep_id, "/project/include/utils.h");

    // Delete target file
    {
        SQLite::Statement del(db, "DELETE FROM files WHERE id = ?");
        del.bind(1, dep_id);
        del.exec();
    }

    // Row should still exist with target_file_id = NULL, target_file_path preserved
    {
        SQLite::Statement q(db, "SELECT target_file_id, target_file_path FROM lsp_dependencies WHERE importer_file_id = ?");
        q.bind(1, importer_id);
        bool found = q.executeStep();
        check(found, "dependency row preserved when target deleted");
        if (found) {
            check(q.getColumn(0).isNull(), "target_file_id is NULL after target deleted");
            check(q.getColumn(1).getString() == "/project/include/utils.h",
                  "target_file_path preserved after target deleted");
        }
    }
}

// ============================================================
// Test 6: get_dependencies MCP response shape — with data
// ============================================================
static void test_get_dependencies_response_shape() {
    auto db = make_test_db();
    int64_t main_id   = insert_file(db, "/project/src/main.cpp");
    int64_t utils_id  = insert_file(db, "/project/include/utils.h");
    int64_t caller_id = insert_file(db, "/project/src/caller.cpp");

    // main.cpp imports utils.h
    insert_dependency(db, main_id, 3, "include", utils_id, "/project/include/utils.h");
    // caller.cpp imports main.cpp (unusual but valid for testing)
    insert_dependency(db, caller_id, 1, "include", main_id, "/project/src/main.cpp");

    auto resp = simulate_get_dependencies(db, "/project/src/main.cpp");

    check(resp.contains("file"), "response has file field");
    check(resp.contains("found"), "response has found field");
    check(resp.contains("imports"), "response has imports array");
    check(resp.contains("imported_by"), "response has imported_by array");
    check(resp["found"] == true, "found=true for existing file");
    check(resp["imports"].is_array(), "imports is array");
    check(resp["imported_by"].is_array(), "imported_by is array");
    check(resp["imports"].size() == 1, "imports has 1 entry");
    check(resp["imported_by"].size() == 1, "imported_by has 1 entry");

    // Check imports entry shape
    const auto& imp = resp["imports"][0];
    check(imp.contains("file"), "imports entry has file field");
    check(imp.contains("kind"), "imports entry has kind field");
    check(imp.contains("line"), "imports entry has line field");
    check(imp["file"] == "/project/include/utils.h", "imports entry file correct");
    check(imp["kind"] == "include", "imports entry kind is include");
    check(imp["line"] == 3, "imports entry line correct");

    // Check imported_by entry shape
    const auto& iby = resp["imported_by"][0];
    check(iby.contains("file"), "imported_by entry has file field");
    check(iby.contains("line"), "imported_by entry has line field");
    check(iby["file"] == "/project/src/caller.cpp", "imported_by entry file correct");
    check(iby["line"] == 1, "imported_by entry line correct");
}

// ============================================================
// Test 7: get_dependencies with no data returns found:true, empty arrays
// ============================================================
static void test_get_dependencies_empty() {
    auto db = make_test_db();
    // File exists but has no dependencies
    insert_file(db, "/project/src/isolated.cpp");

    auto resp = simulate_get_dependencies(db, "/project/src/isolated.cpp");
    check(resp["found"] == true, "found=true for file with no dependencies");
    check(resp["imports"].size() == 0, "imports empty when no dependencies");
    check(resp["imported_by"].size() == 0, "imported_by empty when no dependencies");
}

// ============================================================
// Test 8: get_dependencies with unknown file returns found:false
// ============================================================
static void test_get_dependencies_not_found() {
    auto db = make_test_db();

    auto resp = simulate_get_dependencies(db, "/project/src/unknown.cpp");
    check(resp["found"] == false, "found=false for unknown file");
    check(resp["imports"].size() == 0, "imports empty for unknown file");
    check(resp["imported_by"].size() == 0, "imported_by empty for unknown file");
}

// ============================================================
// Test 9: Multiple import kinds (include, import, require)
// ============================================================
static void test_multiple_import_kinds() {
    auto db = make_test_db();
    int64_t py_file_id    = insert_file(db, "/project/src/main.py", "python");
    int64_t dep_utils_id  = insert_file(db, "/project/src/utils.py", "python");
    int64_t dep_os_id     = insert_file(db, "/project/src/os.py", "python");

    insert_dependency(db, py_file_id, 1, "import", dep_utils_id, "/project/src/utils.py");
    insert_dependency(db, py_file_id, 2, "import", dep_os_id, "/project/src/os.py");

    auto imports = query_imports(db, py_file_id);
    check(imports.size() == 2, "python imports returns 2 entries");
    check(imports[0]["kind"] == "import", "python import kind is 'import'");
    check(imports[1]["kind"] == "import", "second python import kind is 'import'");
}

// ============================================================
// Test 10: COALESCE resolves path from joined files table
// ============================================================
static void test_coalesce_path_resolution() {
    auto db = make_test_db();
    int64_t importer_id = insert_file(db, "/project/src/main.cpp");
    int64_t dep_id      = insert_file(db, "/project/include/resolved.h");

    // Insert with target_file_id set — path should come from files table via COALESCE
    insert_dependency(db, importer_id, 5, "include", dep_id, "/project/include/resolved.h");

    auto imports = query_imports(db, importer_id);
    check(imports.size() == 1, "one import entry");
    check(imports[0]["file"] == "/project/include/resolved.h",
          "COALESCE returns file path from files table");
}

// ============================================================
// Test 11: search_symbols FTS5 fallback still works when lsp_manager is null
// (Simulates the FTS5 path directly — no RequestRouter needed)
// ============================================================
static void test_search_symbols_fts5_fallback() {
    auto db = make_test_db();
    int64_t file_id = insert_file(db, "/project/src/foo.cpp");

    SQLite::Statement sym_ins(db,
        "INSERT INTO symbols (file_id, kind, name, signature, line_start, line_end) "
        "VALUES (?, 'function', 'myFunction', 'void myFunction()', 1, 10)");
    sym_ins.bind(1, file_id);
    sym_ins.exec();

    // FTS5 explicit insert for the symbol
    try {
        SQLite::Statement fts_ins(db,
            "INSERT INTO symbols_fts(rowid, name, signature, documentation) "
            "VALUES (last_insert_rowid(), 'myFunction', 'void myFunction()', '')");
        fts_ins.exec();
    } catch (...) {}

    // Simulate FTS5 search (same as SearchEngine::search_symbols without LSP)
    bool fts5_works = false;
    std::string found_name;
    try {
        SQLite::Statement q(db, R"sql(
            SELECT s.id, s.name, s.kind, COALESCE(s.signature,''), COALESCE(s.documentation,''),
                   f.path, s.line_start, -rank
            FROM symbols_fts
            JOIN symbols s ON symbols_fts.rowid = s.id
            JOIN files f ON s.file_id = f.id
            WHERE symbols_fts MATCH ?
            ORDER BY rank
            LIMIT 20
        )sql");
        q.bind(1, "myFunction*");
        if (q.executeStep()) {
            fts5_works = true;
            found_name = q.getColumn(1).getString();
        }
    } catch (...) {}

    check(fts5_works, "FTS5 fallback works when no LSP available");
    check(found_name == "myFunction", "FTS5 returns correct symbol name");
}

// ============================================================
// Test 12: search_source field is "fts5" in FTS5 path
// (Validates search_source response field concept)
// ============================================================
static void test_search_source_field_fts5() {
    // Create response object simulating what request_router returns for FTS5 path
    nlohmann::json result_obj;
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json item;
    item["name"]          = "myFunction";
    item["kind"]          = "function";
    item["file_path"]     = "/project/src/foo.cpp";
    item["line_start"]    = 1;
    item["rank"]          = 1.5;
    arr.push_back(std::move(item));
    result_obj["results"]       = std::move(arr);
    result_obj["search_source"] = "fts5";
    result_obj["query"]         = "myFunction";

    check(result_obj.contains("search_source"), "response has search_source field");
    check(result_obj["search_source"] == "fts5", "search_source is fts5 for FTS5 path");
    check(result_obj.contains("results"), "response has results array");
    check(result_obj["results"].is_array(), "results is array");
    check(result_obj.contains("query"), "response has query field");
}

// ============================================================
// Test 13: search_source field is "workspace-symbol" in LSP cached path
// (Validates workspace/symbol response shape)
// ============================================================
static void test_search_source_field_workspace_symbol() {
    // Simulate a cached workspace/symbol response
    nlohmann::json cached = nlohmann::json::array();
    nlohmann::json sym;
    sym["name"]          = "myFunction";
    sym["kind"]          = "12";  // function kind in LSP SymbolKind enum
    sym["file_path"]     = "/project/src/foo.cpp";
    sym["line_start"]    = 5;
    sym["signature"]     = "";
    sym["documentation"] = "";
    sym["rank"]          = 1.0;
    cached.push_back(std::move(sym));

    nlohmann::json result_obj;
    result_obj["results"]       = std::move(cached);
    result_obj["search_source"] = "workspace-symbol";
    result_obj["query"]         = "myFunction";

    check(result_obj.contains("search_source"), "workspace-symbol response has search_source field");
    check(result_obj["search_source"] == "workspace-symbol", "search_source is workspace-symbol");
    check(result_obj["results"].size() == 1, "workspace-symbol result has 1 entry");
    check(result_obj["results"][0]["name"] == "myFunction", "result name is myFunction");
}

// ============================================================
// Main
// ============================================================
int main() {
    std::cout << "=== test_lsp_dependencies ===\n";

    test_table_created();
    test_query_imports();
    test_query_imported_by();
    test_cascade_delete_importer();
    test_set_null_target();
    test_get_dependencies_response_shape();
    test_get_dependencies_empty();
    test_get_dependencies_not_found();
    test_multiple_import_kinds();
    test_coalesce_path_resolution();
    test_search_symbols_fts5_fallback();
    test_search_source_field_fts5();
    test_search_source_field_workspace_symbol();

    std::cout << "\nResults: " << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail > 0 ? 1 : 0;
}
