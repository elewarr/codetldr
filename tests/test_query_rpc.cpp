// test_query_rpc.cpp -- JSON-RPC query integration tests.
// Tests SearchEngine and ContextBuilder with the same parameter extraction
// logic that the JSON-RPC handlers in RequestRouter use.
// Uses assert() + return 0/1 pattern consistent with existing tests.

#include "storage/database.h"
#include "query/search_engine.h"
#include "query/context_builder.h"
#include <nlohmann/json.hpp>
#include <SQLiteCpp/SQLiteCpp.h>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace codetldr;
using json = nlohmann::json;

// ---------------------------------------------------------------
// Helper: serialize a SearchResult to JSON (mirrors RequestRouter)
// ---------------------------------------------------------------
static json serialize_search_result(const SearchResult& r) {
    json item;
    item["symbol_id"]     = r.symbol_id;
    item["name"]          = r.name;
    item["kind"]          = r.kind;
    item["signature"]     = r.signature;
    item["documentation"] = r.documentation;
    item["file_path"]     = r.file_path;
    item["line_start"]    = r.line_start;
    item["rank"]          = r.rank;
    return item;
}

static json serialize_search_results(const std::vector<SearchResult>& results) {
    json arr = json::array();
    for (const auto& r : results) {
        arr.push_back(serialize_search_result(r));
    }
    return arr;
}

// ---------------------------------------------------------------
// Helper: extract ContextRequest from JSON params (mirrors RequestRouter)
// ---------------------------------------------------------------
static ContextRequest extract_context_request(const json& params) {
    ContextRequest req;

    std::string fmt_str = params.value("format", "condensed");
    if (fmt_str == "detailed") {
        req.format = ContextFormat::kDetailed;
    } else if (fmt_str == "diff_aware") {
        req.format = ContextFormat::kDiffAware;
    } else {
        req.format = ContextFormat::kCondensed;
    }

    if (params.contains("files") && params["files"].is_array()) {
        for (const auto& f : params["files"]) {
            if (f.is_string()) {
                req.file_paths.push_back(f.get<std::string>());
            }
        }
    }

    if (params.contains("changed") && params["changed"].is_array()) {
        for (const auto& c : params["changed"]) {
            if (c.is_string()) {
                req.changed_paths.push_back(c.get<std::string>());
            }
        }
    }

    req.max_symbols = params.value("max_symbols", 200);
    return req;
}

// ---------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------
static int64_t insert_file(SQLite::Database& db, const std::string& path) {
    SQLite::Statement ins(db,
        "INSERT INTO files(path, language, mtime_ns) VALUES(?, 'cpp', 0)");
    ins.bind(1, path);
    ins.exec();
    return db.getLastInsertRowid();
}

static int64_t insert_symbol(SQLite::Database& db, int64_t file_id,
                              const std::string& kind, const std::string& name,
                              const std::string& sig, const std::string& doc,
                              int line_start, int line_end) {
    SQLite::Statement ins(db,
        "INSERT INTO symbols(file_id, kind, name, signature, documentation, line_start, line_end) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, file_id);
    ins.bind(2, kind);
    ins.bind(3, name);
    ins.bind(4, sig);
    ins.bind(5, doc);
    ins.bind(6, line_start);
    ins.bind(7, line_end);
    ins.exec();
    return db.getLastInsertRowid();
}

static void populate_fts(SQLite::Database& db) {
    db.exec(
        "INSERT INTO symbols_fts(rowid, name, signature, documentation) "
        "SELECT id, name, COALESCE(signature,''), COALESCE(documentation,'') FROM symbols"
    );
}

int main() {
    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_query_rpc";
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    const fs::path db_path = test_dir / "index.sqlite";
    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    // Insert test data
    int64_t file_a = insert_file(db, "/src/a.cpp");
    int64_t file_b = insert_file(db, "/src/b.cpp");

    // File A: function + class + method
    insert_symbol(db, file_a, "function", "test_compute",
                  "int test_compute(int x)", "Test compute function.", 1, 10);
    insert_symbol(db, file_a, "class", "TestClass",
                  "class TestClass", "A test class.", 12, 50);
    insert_symbol(db, file_a, "method", "run_test",
                  "void run_test()", "Runs the test.", 20, 30);

    // File B: function only
    insert_symbol(db, file_b, "function", "process_data",
                  "void process_data(int* buf, int len)", "Processes buffer.", 1, 20);

    // Populate FTS index
    populate_fts(db);

    SearchEngine engine(db);
    ContextBuilder builder(db);

    // Test a: search_text with "test" query returns results with expected JSON fields
    {
        // Simulate search_text RPC: params = {"query": "test", "limit": 20}
        json params = {{"query", "test"}, {"limit", 20}};
        std::string query = params.value("query", "");
        int limit = params.value("limit", 20);

        auto results = engine.search_text(query, limit);
        json arr = serialize_search_results(results);

        assert(!arr.empty());
        // Verify all expected JSON fields are present
        const json& first = arr[0];
        assert(first.contains("symbol_id"));
        assert(first.contains("name"));
        assert(first.contains("kind"));
        assert(first.contains("signature"));
        assert(first.contains("documentation"));
        assert(first.contains("file_path"));
        assert(first.contains("line_start"));
        assert(first.contains("rank"));

        // Verify at least one result mentions "test"
        bool found_test = false;
        for (const auto& item : arr) {
            std::string name = item["name"].get<std::string>();
            if (name.find("test") != std::string::npos ||
                name.find("Test") != std::string::npos) {
                found_test = true;
                break;
            }
        }
        assert(found_test);
        std::cout << "PASS: Test a - search_text returns results with all JSON fields (" << arr.size() << " results)\n";
    }

    // Test b: search_symbols with kind="function" only returns functions
    {
        // Simulate search_symbols RPC: params = {"query": "", "kind": "function"}
        json params = {{"query", ""}, {"kind", "function"}, {"limit", 20}};
        std::string query = params.value("query", "");
        std::string kind  = params.value("kind", "");
        int limit = params.value("limit", 20);

        auto results = engine.search_symbols(query, kind, /*language=*/"", limit);
        json arr = serialize_search_results(results);

        assert(!arr.empty());
        for (const auto& item : arr) {
            assert(item["kind"].get<std::string>() == "function");
        }
        std::cout << "PASS: Test b - search_symbols kind=function returns only functions (" << arr.size() << " results)\n";
    }

    // Test c: get_context with format="condensed" returns text with [FILE] header
    {
        json params = {{"format", "condensed"}, {"files", json::array({"/src/a.cpp"})}};
        ContextRequest ctx_req = extract_context_request(params);

        ContextResponse ctx_resp = builder.build(ctx_req);

        assert(!ctx_resp.text.empty());
        assert(ctx_resp.text.find("[FILE]") != std::string::npos);
        assert(ctx_resp.text.find("/src/a.cpp") != std::string::npos);

        // Verify JSON response structure (mirrors RequestRouter)
        json result;
        result["text"]             = ctx_resp.text;
        result["symbol_count"]     = ctx_resp.symbol_count;
        result["file_count"]       = ctx_resp.file_count;
        result["estimated_tokens"] = ctx_resp.estimated_tokens;

        assert(result.contains("text"));
        assert(result.contains("symbol_count"));
        assert(result.contains("file_count"));
        assert(result.contains("estimated_tokens"));
        assert(result["symbol_count"].get<int>() == 3);
        std::cout << "PASS: Test c - get_context condensed returns [FILE] header with correct response fields\n";
    }

    // Test d: get_context with format="diff_aware" and changed=["a.cpp"] only includes a.cpp symbols
    {
        json params = {
            {"format", "diff_aware"},
            {"changed", json::array({"/src/a.cpp"})}
        };
        ContextRequest ctx_req = extract_context_request(params);

        ContextResponse ctx_resp = builder.build(ctx_req);

        assert(!ctx_resp.text.empty());
        // Should include a.cpp
        assert(ctx_resp.text.find("/src/a.cpp") != std::string::npos);
        // Should NOT include b.cpp
        assert(ctx_resp.text.find("/src/b.cpp") == std::string::npos);
        assert(ctx_resp.text.find("process_data") == std::string::npos);
        std::cout << "PASS: Test d - get_context diff_aware only includes changed file symbols\n";
    }

    fs::remove_all(test_dir);
    std::cout << "All query_rpc tests passed.\n";
    return 0;
}
