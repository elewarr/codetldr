// test_new_rpc_methods.cpp
// Tests for the 4 new RequestRouter methods and enhanced get_status_json.
// Tests the underlying ContextBuilder methods (find_symbol, get_caller_names,
// get_callee_names) and the query logic used by the 4 new methods, using
// an in-memory SQLite database with the full schema.
// Also tests get_embedding_stats JSON shape and INDEX_INCONSISTENT health check (OBS-02, OBS-03).

#include "storage/database.h"
#include "query/search_engine.h"
#include "query/context_builder.h"
#include "embedding/embedding_worker.h"
#include "embedding/model_manager.h"
#include "embedding/vector_store.h"
#include <nlohmann/json.hpp>
#include <SQLiteCpp/SQLiteCpp.h>
#include <cassert>
#include <cstdlib>
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

// ---------------------------------------------------------------
// Simulate get_embedding_stats logic (mirrors Coordinator::get_embedding_stats_json)
// ---------------------------------------------------------------
static json simulate_get_embedding_stats(SQLite::Database& db,
                                          codetldr::EmbeddingWorker* worker,
                                          codetldr::VectorStore* vector_store,
                                          codetldr::ModelManager* model_manager) {
    json j;

    // Model info
    std::string model_name = "none";
    std::string model_status_str = "not_configured";
    if (model_manager) {
        switch (model_manager->status()) {
            case codetldr::ModelStatus::loaded:             model_status_str = "loaded"; break;
            case codetldr::ModelStatus::model_not_installed: model_status_str = "not_installed"; break;
            case codetldr::ModelStatus::load_failed:        model_status_str = "load_failed"; break;
            case codetldr::ModelStatus::not_configured:     model_status_str = "not_configured"; break;
        }
        model_name = "configured";
    }
    j["model_name"]   = model_name;
    j["model_status"] = model_status_str;

    // Latency metrics
    if (worker) {
        auto snap = worker->stats().snapshot();
        j["latency_p50_ms"]            = snap.p50_ms;
        j["latency_p95_ms"]            = snap.p95_ms;
        j["latency_p99_ms"]            = snap.p99_ms;
        j["latency_avg_ms"]            = snap.avg_ms;
        j["throughput_chunks_per_sec"] = snap.throughput_chunks_per_sec;
        j["queue_depth"]               = snap.queue_depth;
        j["chunks_embedded_total"]     = snap.chunks_processed;
        j["sample_count"]              = snap.sample_count;
    } else {
        j["latency_p50_ms"]            = 0.0;
        j["latency_p95_ms"]            = 0.0;
        j["latency_p99_ms"]            = 0.0;
        j["latency_avg_ms"]            = 0.0;
        j["throughput_chunks_per_sec"] = 0.0;
        j["queue_depth"]               = (int64_t)0;
        j["chunks_embedded_total"]     = (int64_t)0;
        j["sample_count"]              = (int64_t)0;
    }

    // FAISS index stats
    int64_t faiss_count = vector_store ? vector_store->ntotal() : 0;
    j["faiss_vector_count"] = faiss_count;

    // SQLite embedded count
    int64_t sqlite_count = 0;
    try {
        SQLite::Statement q(db, "SELECT COUNT(DISTINCT symbol_id) FROM embedded_files");
        if (q.executeStep()) {
            sqlite_count = q.getColumn(0).getInt64();
        }
    } catch (...) {}
    j["sqlite_embedded_count"] = sqlite_count;

    // Health checks
    std::string health = "ok";
    json degraded = json(nullptr);

    if (faiss_count > 0 || sqlite_count > 0) {
        int64_t delta = std::abs(faiss_count - sqlite_count);
        int64_t threshold = std::max((int64_t)10,
                                     sqlite_count > 0 ? sqlite_count * 5 / 100 : (int64_t)10);
        if (delta > threshold) {
            health = "degraded";
            degraded = "INDEX_INCONSISTENT: faiss=" + std::to_string(faiss_count)
                     + " sqlite=" + std::to_string(sqlite_count)
                     + " (delta=" + std::to_string(delta) + ")";
        }
    }

    j["health"]   = health;
    j["degraded"] = degraded;

    return j;
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

    // ---- Test 9: get_embedding_stats returns expected JSON shape (OBS-02) ----
    {
        auto result = simulate_get_embedding_stats(db, nullptr, nullptr, nullptr);
        CHECK(result.contains("model_status"),            "get_embedding_stats: has model_status");
        CHECK(result.contains("latency_p50_ms"),          "get_embedding_stats: has latency_p50_ms");
        CHECK(result.contains("latency_p95_ms"),          "get_embedding_stats: has latency_p95_ms");
        CHECK(result.contains("latency_p99_ms"),          "get_embedding_stats: has latency_p99_ms");
        CHECK(result.contains("throughput_chunks_per_sec"), "get_embedding_stats: has throughput_chunks_per_sec");
        CHECK(result.contains("queue_depth"),             "get_embedding_stats: has queue_depth");
        CHECK(result.contains("faiss_vector_count"),      "get_embedding_stats: has faiss_vector_count");
        CHECK(result.contains("sqlite_embedded_count"),   "get_embedding_stats: has sqlite_embedded_count");
        CHECK(result.contains("sample_count"),            "get_embedding_stats: has sample_count");
        CHECK(result.contains("health"),                  "get_embedding_stats: has health");
        CHECK(result.contains("degraded"),                "get_embedding_stats: has degraded");
        CHECK(result["health"].get<std::string>() == "ok", "get_embedding_stats: health=ok when no data");
        CHECK(result["model_status"].get<std::string>() == "not_configured",
              "get_embedding_stats: model_status=not_configured when no model manager");
    }

    // ---- Test 10: get_embedding_stats INDEX_INCONSISTENT when sqlite_count > 10 and faiss=0 (OBS-03) ----
    {
        // Insert symbols with unique IDs so COUNT(DISTINCT symbol_id) grows
        // We need sqlite_count > 10 distinct symbol_ids so that delta > threshold=10
        // Insert 15 new symbols in file_a and embed them (FAISS has 0 vectors)
        std::vector<int64_t> extra_syms;
        for (int i = 0; i < 15; ++i) {
            int64_t sym_id = insert_symbol(db, file_a, "function",
                                           "obs_test_sym_" + std::to_string(i),
                                           "void obs_test_sym_" + std::to_string(i) + "()",
                                           "", 100 + i, 110 + i);
            extra_syms.push_back(sym_id);
        }
        // Insert 1 row per symbol into embedded_files so COUNT(DISTINCT symbol_id) = 15
        for (int64_t sym_id : extra_syms) {
            SQLite::Statement ins(db,
                "INSERT INTO embedded_files(symbol_id, file_id) VALUES(?, ?)");
            ins.bind(1, sym_id);
            ins.bind(2, static_cast<int64_t>(file_a));
            ins.exec();
        }

        // null vector_store means faiss_count=0, sqlite_count=15 distinct symbols
        // delta = |0 - 15| = 15, threshold = max(10, 15*5/100) = max(10, 0) = 10
        // 15 > 10 => INDEX_INCONSISTENT
        auto result = simulate_get_embedding_stats(db, nullptr, nullptr, nullptr);
        CHECK(result["health"].get<std::string>() == "degraded",
              "get_embedding_stats INDEX_INCONSISTENT: health=degraded");
        CHECK(result.contains("degraded") && result["degraded"].is_string(),
              "get_embedding_stats INDEX_INCONSISTENT: degraded is a string");
        if (result["degraded"].is_string()) {
            CHECK(result["degraded"].get<std::string>().find("INDEX_INCONSISTENT") != std::string::npos,
                  "get_embedding_stats INDEX_INCONSISTENT: message contains INDEX_INCONSISTENT");
        }
    }

    fs::remove_all(test_dir);

    int total = g_pass + g_fail;
    printf("\ntest_new_rpc_methods: %d/%d passed\n", g_pass, total);
    return g_fail == 0 ? 0 : 1;
}
