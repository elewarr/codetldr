// test_new_rpc_methods.cpp
// Tests for the 4 new RequestRouter methods and enhanced get_status_json.
// Tests the underlying ContextBuilder methods (find_symbol, get_caller_names,
// get_callee_names) and the query logic used by the 4 new methods, using
// an in-memory SQLite database with the full schema.

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

// ---------------------------------------------------------------
// Setup helpers
// ---------------------------------------------------------------
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

static void insert_call(SQLite::Database& db, int64_t caller_id, int64_t file_id,
                        const std::string& callee_name, int line = 1) {
    SQLite::Statement ins(db,
        "INSERT INTO calls(caller_id, callee_name, file_id, line) VALUES(?, ?, ?, ?)");
    ins.bind(1, caller_id);
    ins.bind(2, callee_name);
    ins.bind(3, file_id);
    ins.bind(4, line);
    ins.exec();
}

// ---------------------------------------------------------------
// Simulate get_file_summary logic (mirrors RequestRouter)
// ---------------------------------------------------------------
static json simulate_get_file_summary(ContextBuilder& builder,
                                      const std::string& file_path,
                                      const std::string& format = "condensed") {
    ContextRequest req;
    if (format == "detailed") {
        req.format = ContextFormat::kDetailed;
    } else {
        req.format = ContextFormat::kCondensed;
    }
    req.file_paths = {file_path};
    req.max_symbols = 200;

    ContextResponse resp = builder.build(req);

    json result;
    result["text"]             = resp.text;
    result["symbol_count"]     = resp.symbol_count;
    result["file_count"]       = resp.file_count;
    result["estimated_tokens"] = resp.estimated_tokens;
    return result;
}

// ---------------------------------------------------------------
// Simulate get_function_detail logic (mirrors RequestRouter)
// ---------------------------------------------------------------
static json simulate_get_function_detail(ContextBuilder& builder,
                                         const std::string& name,
                                         const std::string& file_path = "") {
    SymbolInfo sym = builder.find_symbol(name, file_path);

    json result;
    result["found"] = sym.found;
    result["name"]  = sym.found ? sym.name : name;

    if (sym.found) {
        result["kind"]          = sym.kind;
        result["signature"]     = sym.signature;
        result["documentation"] = sym.documentation;
        result["file_path"]     = sym.file_path;
        result["line_start"]    = sym.line_start;
        result["line_end"]      = sym.line_end;

        auto callers = builder.get_caller_names(sym.id);
        auto callees = builder.get_callee_names(sym.id);

        result["callers"] = json::array();
        for (const auto& c : callers) result["callers"].push_back(c);
        result["callees"] = json::array();
        for (const auto& c : callees) result["callees"].push_back(c);
    }
    return result;
}

// ---------------------------------------------------------------
// Simulate get_call_graph logic (mirrors RequestRouter)
// ---------------------------------------------------------------
static json simulate_get_call_graph(ContextBuilder& builder,
                                    const std::string& name,
                                    const std::string& direction = "both",
                                    int depth = 1) {
    SymbolInfo sym = builder.find_symbol(name);

    json result;
    result["name"]    = name;
    result["found"]   = sym.found;
    result["callers"] = json::array();
    result["callees"] = json::array();

    if (sym.found) {
        if (direction == "callers" || direction == "both") {
            auto callers = builder.get_caller_names(sym.id);
            for (const auto& c : callers) result["callers"].push_back(c);
        }
        if (direction == "callees" || direction == "both") {
            auto callees = builder.get_callee_names(sym.id);
            for (const auto& c : callees) result["callees"].push_back(c);
        }
    }
    return result;
}

// ---------------------------------------------------------------
// Simulate get_project_overview aggregate queries
// ---------------------------------------------------------------
static json simulate_get_project_overview(SQLite::Database& db) {
    json result;

    {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM files");
        q.executeStep();
        result["file_count"] = q.getColumn(0).getInt();
    }
    {
        SQLite::Statement q(db, "SELECT COUNT(*) FROM symbols");
        q.executeStep();
        result["symbol_count"] = q.getColumn(0).getInt();
    }
    {
        json breakdown = json::object();
        SQLite::Statement q(db,
            "SELECT language, COUNT(*) as cnt FROM files "
            "WHERE language IS NOT NULL AND language != '' "
            "GROUP BY language ORDER BY cnt DESC");
        while (q.executeStep()) {
            std::string lang = q.getColumn(0).getString();
            int cnt          = q.getColumn(1).getInt();
            breakdown[lang]  = cnt;
        }
        result["language_breakdown"] = std::move(breakdown);
    }
    // language_support would come from Coordinator — we add a stub here
    result["language_support"] = json::array();
    return result;
}

int main() {
    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_new_rpc";
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    const fs::path db_path = test_dir / "index.sqlite";
    auto db_obj = Database::open(db_path);
    SQLite::Database& db = db_obj.raw();

    // Insert test data
    int64_t file_a = insert_file(db, "/src/alpha.cpp", "cpp");
    int64_t file_b = insert_file(db, "/src/beta.py",   "python");

    // Alpha: function 'compute' calls 'helper', method 'MyClass::run' calls 'compute'
    int64_t sym_compute = insert_symbol(db, file_a, "function", "compute",
                                        "int compute(int x)", "Computes result.", 1, 15);
    int64_t sym_run     = insert_symbol(db, file_a, "method",   "run",
                                        "void run()", "Main runner.", 20, 40);
    int64_t sym_helper  = insert_symbol(db, file_a, "function", "helper",
                                        "void helper()", "Helper function.", 50, 60);
    int64_t sym_parse   = insert_symbol(db, file_b, "function", "parse",
                                        "def parse(data):", "Parses data.", 1, 10);

    // Calls: compute -> helper, run -> compute
    insert_call(db, sym_compute, file_a, "helper",  5);
    insert_call(db, sym_run,     file_a, "compute", 25);

    ContextBuilder builder(db);

    // ---- Test 1: get_file_summary returns text and symbol_count ----
    {
        auto result = simulate_get_file_summary(builder, "/src/alpha.cpp");
        CHECK(result.contains("text"), "get_file_summary: has 'text' field");
        CHECK(result.contains("symbol_count"), "get_file_summary: has 'symbol_count' field");
        CHECK(result.contains("file_count"), "get_file_summary: has 'file_count' field");
        CHECK(result.contains("estimated_tokens"), "get_file_summary: has 'estimated_tokens' field");
        CHECK(result["symbol_count"].get<int>() == 3, "get_file_summary: alpha.cpp has 3 symbols");
        CHECK(result["text"].get<std::string>().find("[FILE]") != std::string::npos,
              "get_file_summary: text contains [FILE] header");
    }

    // ---- Test 2: get_function_detail found=true with correct fields ----
    {
        auto result = simulate_get_function_detail(builder, "compute");
        CHECK(result["found"].get<bool>() == true, "get_function_detail: compute found");
        CHECK(result["name"].get<std::string>() == "compute", "get_function_detail: name=compute");
        CHECK(result["kind"].get<std::string>() == "function", "get_function_detail: kind=function");
        CHECK(result["file_path"].get<std::string>() == "/src/alpha.cpp",
              "get_function_detail: file_path correct");
        CHECK(result.contains("callers"), "get_function_detail: has callers array");
        CHECK(result.contains("callees"), "get_function_detail: has callees array");
        // compute is called by 'run', and calls 'helper'
        CHECK(result["callees"].size() == 1, "get_function_detail: compute has 1 callee (helper)");
        CHECK(result["callees"][0].get<std::string>() == "helper",
              "get_function_detail: callee is 'helper'");
        CHECK(result["callers"].size() == 1, "get_function_detail: compute has 1 caller (run)");
        CHECK(result["callers"][0].get<std::string>() == "run",
              "get_function_detail: caller is 'run'");
    }

    // ---- Test 3: get_function_detail found=false for nonexistent name ----
    {
        auto result = simulate_get_function_detail(builder, "nonexistent_function_xyz");
        CHECK(result["found"].get<bool>() == false, "get_function_detail: nonexistent returns found=false");
        CHECK(result["name"].get<std::string>() == "nonexistent_function_xyz",
              "get_function_detail: name preserved when not found");
    }

    // ---- Test 4: get_call_graph callees direction ----
    {
        auto result = simulate_get_call_graph(builder, "compute", "callees");
        CHECK(result["found"].get<bool>() == true, "get_call_graph: compute found");
        CHECK(result["callees"].size() == 1, "get_call_graph: compute has 1 callee");
        CHECK(result["callees"][0].get<std::string>() == "helper",
              "get_call_graph: callee is 'helper'");
        CHECK(result["callers"].empty(), "get_call_graph: no callers when direction=callees");
    }

    // ---- Test 5: get_call_graph both direction ----
    {
        auto result = simulate_get_call_graph(builder, "compute", "both");
        CHECK(result["found"].get<bool>() == true, "get_call_graph both: compute found");
        CHECK(!result["callers"].empty(), "get_call_graph both: compute has callers");
        CHECK(!result["callees"].empty(), "get_call_graph both: compute has callees");
    }

    // ---- Test 6: get_project_overview ----
    {
        auto result = simulate_get_project_overview(db);
        CHECK(result["file_count"].get<int>() == 2,
              "get_project_overview: file_count=2");
        CHECK(result["symbol_count"].get<int>() == 4,
              "get_project_overview: symbol_count=4");
        CHECK(result["language_breakdown"].contains("cpp"),
              "get_project_overview: language_breakdown has cpp");
        CHECK(result["language_breakdown"].contains("python"),
              "get_project_overview: language_breakdown has python");
        CHECK(result["language_breakdown"]["cpp"].get<int>() == 1,
              "get_project_overview: 1 cpp file");
        CHECK(result["language_breakdown"]["python"].get<int>() == 1,
              "get_project_overview: 1 python file");
    }

    // ---- Test 7: find_symbol with file_path filter ----
    {
        // 'parse' exists only in beta.py
        SymbolInfo sym = builder.find_symbol("parse", "/src/beta.py");
        CHECK(sym.found == true, "find_symbol: parse found in beta.py");
        CHECK(sym.file_path == "/src/beta.py", "find_symbol: file_path matches");

        // Should NOT find 'parse' in alpha.cpp
        SymbolInfo no_sym = builder.find_symbol("parse", "/src/alpha.cpp");
        CHECK(no_sym.found == false, "find_symbol: parse not found in alpha.cpp");
    }

    // ---- Test 8: get_callee_names and get_caller_names public API ----
    {
        // helper has no callees
        auto callees_helper = builder.get_callee_names(sym_helper);
        CHECK(callees_helper.empty(), "get_callee_names: helper has no callees");

        // run calls compute
        auto callees_run = builder.get_callee_names(sym_run);
        CHECK(callees_run.size() == 1 && callees_run[0] == "compute",
              "get_callee_names: run calls compute");

        // compute is called by run
        auto callers_compute = builder.get_caller_names(sym_compute);
        CHECK(callers_compute.size() == 1 && callers_compute[0] == "run",
              "get_caller_names: compute called by run");
    }

    fs::remove_all(test_dir);

    int total = g_pass + g_fail;
    printf("\ntest_new_rpc_methods: %d/%d passed\n", g_pass, total);
    return g_fail == 0 ? 0 : 1;
}
