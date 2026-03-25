#pragma once
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

// Forward declarations for SQLiteCpp
namespace SQLite { class Database; }

namespace codetldr {

// Forward declaration — Coordinator is the owner; RequestRouter holds a reference.
class Coordinator;
class SearchEngine;
class ContextBuilder;

// Routes incoming JSON-RPC 2.0 requests to handler methods on Coordinator,
// SearchEngine, and ContextBuilder.
// Supported methods:
//   health_check    -> {status: "ok", pid: N}
//   get_status      -> coordinator.get_status_json()
//   stop            -> coordinator.request_stop() then {ok: true}
//   search_text     -> SearchEngine::search_text(query, limit)
//   search_symbols  -> SearchEngine::search_symbols(query, kind, limit)
//   get_context     -> ContextBuilder::build(req)
//   <unknown>       -> JSON-RPC error {code: -32601, message: "Method not found"}
class RequestRouter {
public:
    // Primary constructor: takes Coordinator and Database references.
    // Creates SearchEngine and ContextBuilder from the database.
    RequestRouter(Coordinator& coordinator, SQLite::Database& db);

    // Destructor defined in .cpp where SearchEngine and ContextBuilder are complete types.
    ~RequestRouter();

    // Dispatch a JSON-RPC 2.0 request object and return a complete response object.
    nlohmann::json dispatch(const nlohmann::json& req);

private:
    Coordinator& coordinator_;
    std::unique_ptr<SearchEngine> search_engine_;
    std::unique_ptr<ContextBuilder> context_builder_;
};

} // namespace codetldr
