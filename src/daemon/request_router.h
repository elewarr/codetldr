#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

// Forward declarations for SQLiteCpp
namespace SQLite { class Database; }

namespace codetldr {

// Forward declaration — Coordinator is the owner; RequestRouter holds a reference.
class Coordinator;
class SearchEngine;
class ContextBuilder;
class LspManager;

// Routes incoming JSON-RPC 2.0 requests to handler methods on Coordinator,
// SearchEngine, and ContextBuilder.
// Supported methods:
//   health_check       -> {status: "ok", pid: N}
//   get_status         -> coordinator.get_status_json()
//   stop               -> coordinator.request_stop() then {ok: true}
//   search_text        -> SearchEngine::search_text(query, limit)
//   search_symbols     -> SearchEngine::search_symbols(query, kind, limit) with workspace/symbol primary
//   get_context        -> ContextBuilder::build(req)
//   get_dependencies   -> lsp_dependencies table query (imports + imported_by)
//   <unknown>          -> JSON-RPC error {code: -32601, message: "Method not found"}
class RequestRouter {
public:
    // Primary constructor: takes Coordinator and Database references.
    // Creates SearchEngine and ContextBuilder from the database.
    RequestRouter(Coordinator& coordinator, SQLite::Database& db);

    // Destructor defined in .cpp where SearchEngine and ContextBuilder are complete types.
    ~RequestRouter();

    // Dispatch a JSON-RPC 2.0 request object and return a complete response object.
    nlohmann::json dispatch(const nlohmann::json& req);

    // Inject LspManager for workspace/symbol search. Non-owning. (Phase 27)
    void set_lsp_manager(LspManager* mgr) { lsp_manager_ = mgr; }

private:
    Coordinator& coordinator_;
    SQLite::Database& db_;
    std::unique_ptr<SearchEngine> search_engine_;
    std::unique_ptr<ContextBuilder> context_builder_;

    // LspManager pointer for workspace/symbol dispatch — non-owning, may be null
    LspManager* lsp_manager_ = nullptr;

    // Cache for workspace/symbol results: query -> (timestamp, results_array)
    // No mutex needed: dispatch() and LSP callbacks both run on the same poll() thread.
    using CacheEntry = std::pair<std::chrono::steady_clock::time_point, nlohmann::json>;
    std::unordered_map<std::string, CacheEntry> lsp_symbol_cache_;

    static constexpr int kLspCacheTtlSeconds = 30;
};

} // namespace codetldr
