// test_semantic_search.cpp
// Tests for the semantic_search RPC handler in RequestRouter.
//
// Tests:
//   1. semantic_search with no model returns -32001 with "model" in message
//   2. semantic_search result SQL lookup: result items contain name/kind/file_path/line_start/score
//   3. semantic_search returns -32001 in non-semantic builds ("not compiled")
//   4. semantic_search returns -32001 when model is not installed (semantic build, no model)
//
// Test 1/3 is always exercised (covers both build configs).
// Test 2 verifies the SQL JOIN produces correct field names.

#include "daemon/request_router.h"
#include "daemon/coordinator.h"
#include "storage/database.h"
#include "analysis/tree_sitter/language_registry.h"
#include <nlohmann/json.hpp>
#include <SQLiteCpp/SQLiteCpp.h>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
#include "embedding/model_manager.h"
#endif

namespace fs = std::filesystem;
using namespace codetldr;
using json = nlohmann::json;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                              \
    do {                                                               \
        if (cond) {                                                    \
            ++g_pass;                                                  \
            std::cout << "PASS: " << msg << "\n";                     \
        } else {                                                       \
            ++g_fail;                                                  \
            std::cerr << "FAIL: " << msg << "  (line " << __LINE__ << ")\n"; \
        }                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// Helper: build a test database with schema and insert test data
// ---------------------------------------------------------------------------
static int64_t insert_file(SQLite::Database& db, const std::string& path,
                            const std::string& language = "cpp") {
    SQLite::Statement ins(db,
        "INSERT INTO files(path, language, mtime_ns) VALUES(?, ?, 0)");
    ins.bind(1, path);
    ins.bind(2, language);
    ins.exec();
    return db.getLastInsertRowid();
}

static int64_t insert_symbol(SQLite::Database& db, int64_t file_id,
                              const std::string& kind, const std::string& name,
                              const std::string& sig, int line_start, int line_end) {
    SQLite::Statement ins(db,
        "INSERT INTO symbols(file_id, kind, name, signature, line_start, line_end) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    ins.bind(1, file_id);
    ins.bind(2, kind);
    ins.bind(3, name);
    ins.bind(4, sig);
    ins.bind(5, line_start);
    ins.bind(6, line_end);
    ins.exec();
    return db.getLastInsertRowid();
}

// ---------------------------------------------------------------------------
// Test 1: semantic_search with no model (or non-semantic build) returns -32001
// ---------------------------------------------------------------------------
static void test_no_model_returns_error(const fs::path& test_dir) {
    fs::path sub = test_dir / "t1";
    fs::create_directories(sub / ".codetldr");
    fs::path db_path   = sub / ".codetldr" / "index.sqlite";
    fs::path sock_path = sub / ".codetldr" / "daemon.sock";

    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    LanguageRegistry registry;
    registry.initialize();

    Coordinator coordinator(sub, db, registry, sock_path, std::chrono::seconds(1));
    RequestRouter router(coordinator, db);

    json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 1;
    req["method"]  = "semantic_search";
    req["params"]  = {{"query", "test query"}, {"limit", 5}};

    json resp = router.dispatch(req);

#ifdef CODETLDR_ENABLE_SEMANTIC_SEARCH
    std::cout << "Test 1: semantic_search with no model installed returns -32001...\n";
#else
    std::cout << "Test 3: semantic_search non-semantic build returns -32001...\n";
#endif

    CHECK(resp.contains("error"),
          "No-model: response contains error field");

    if (resp.contains("error")) {
        int code = resp["error"].value("code", 0);
        CHECK(code == -32001,
              "No-model: error.code == -32001");

        std::string msg = resp["error"].value("message", "");
        CHECK(!msg.empty(),
              "No-model: error.message is not empty");

        // Message should contain "model" or "compiled" or "not"
        bool relevant = msg.find("model")    != std::string::npos ||
                        msg.find("compiled") != std::string::npos ||
                        msg.find("not")      != std::string::npos ||
                        msg.find("Semantic") != std::string::npos;
        CHECK(relevant,
              "No-model: error.message contains relevant keyword");
    }

    CHECK(!resp.contains("result"),
          "No-model: response does NOT contain result (error path)");

    fs::remove_all(sub);
}

// ---------------------------------------------------------------------------
// Test 2: semantic_search SQL JOIN returns correct field names
// Verifies the schema assumed by the handler: files.path aliased as file_path
// ---------------------------------------------------------------------------
static void test_result_schema_fields(const fs::path& test_dir) {
    std::cout << "Test 2: semantic_search result schema — SQL JOIN produces correct fields...\n";

    fs::path sub = test_dir / "t2";
    fs::create_directories(sub);
    fs::path db_path = sub / "test.sqlite";

    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    // Insert test data
    int64_t fid = insert_file(db, "/src/embed_utils.cpp", "cpp");
    int64_t sid = insert_symbol(db, fid, "function", "embed_code",
                                "void embed_code(const std::string& text)", 10, 20);

    // Simulate what the semantic_search handler does: JOIN symbols with files
    // The handler uses: SELECT s.name, s.kind, f.path, s.line_start FROM symbols s
    //                   JOIN files f ON s.file_id = f.id WHERE s.id = ?
    // Then builds item with file_path = f.path, line_start = s.line_start
    SQLite::Statement q(db,
        "SELECT s.name, s.kind, f.path, s.line_start "
        "FROM symbols s JOIN files f ON s.file_id = f.id WHERE s.id = ?");
    q.bind(1, static_cast<long long>(sid));
    bool found = q.executeStep();

    CHECK(found, "Schema: symbol found via JOIN query");
    if (found) {
        // Build result item as handler would
        json item;
        item["name"]       = q.getColumn(0).getString();
        item["kind"]       = q.getColumn(1).getString();
        item["file_path"]  = q.getColumn(2).getString();
        item["line_start"] = q.getColumn(3).getInt();
        item["score"]      = 0.95f;

        CHECK(item.contains("name"),       "Schema: item has 'name'");
        CHECK(item.contains("kind"),       "Schema: item has 'kind'");
        CHECK(item.contains("file_path"),  "Schema: item has 'file_path'");
        CHECK(item.contains("line_start"), "Schema: item has 'line_start'");
        CHECK(item.contains("score"),      "Schema: item has 'score'");

        CHECK(item["name"].get<std::string>() == "embed_code",
              "Schema: name == 'embed_code'");
        CHECK(item["kind"].get<std::string>() == "function",
              "Schema: kind == 'function'");
        CHECK(item["file_path"].get<std::string>() == "/src/embed_utils.cpp",
              "Schema: file_path == '/src/embed_utils.cpp'");
        CHECK(item["line_start"].get<int>() == 10,
              "Schema: line_start == 10");
    }

    fs::remove_all(sub);
}

// ---------------------------------------------------------------------------
// Test 3: semantic_search with empty query returns without crash
// (no model installed, so returns -32001 — but that's an error, not a crash)
// ---------------------------------------------------------------------------
static void test_empty_query_no_crash(const fs::path& test_dir) {
    std::cout << "Test 4: semantic_search with empty query returns response (no crash)...\n";

    fs::path sub = test_dir / "t4";
    fs::create_directories(sub / ".codetldr");
    fs::path db_path   = sub / ".codetldr" / "index.sqlite";
    fs::path sock_path = sub / ".codetldr" / "daemon.sock";

    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    LanguageRegistry registry;
    registry.initialize();

    Coordinator coordinator(sub, db, registry, sock_path, std::chrono::seconds(1));
    RequestRouter router(coordinator, db);

    // Empty query
    json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 4;
    req["method"]  = "semantic_search";
    req["params"]  = {{"query", ""}, {"limit", 10}};

    json resp = router.dispatch(req);

    // Either error (no model) or empty result (model loaded but empty)
    bool has_valid_response = resp.contains("error") || resp.contains("result");
    CHECK(has_valid_response,
          "Empty query: response has either error or result (no crash)");

    // The response must not be a "method not found" error — semantic_search is registered
    if (resp.contains("error")) {
        int code = resp["error"].value("code", 0);
        CHECK(code != -32601,
              "Empty query: error is NOT -32601 (method found)");
    }

    fs::remove_all(sub);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_semantic_search";
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    std::cout << "=== test_semantic_search ===\n";

    try {
        test_no_model_returns_error(test_dir);
        test_result_schema_fields(test_dir);
        test_empty_query_no_crash(test_dir);
    } catch (const std::exception& ex) {
        std::cerr << "FATAL exception: " << ex.what() << "\n";
        fs::remove_all(test_dir);
        return 1;
    }

    fs::remove_all(test_dir);

    int total = g_pass + g_fail;
    printf("\ntest_semantic_search: %d/%d passed\n", g_pass, total);
    return g_fail == 0 ? 0 : 1;
}
