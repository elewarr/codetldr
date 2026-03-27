#pragma once
#include "query/hybrid_search_engine.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <string>

// Forward declarations for SQLiteCpp
namespace SQLite { class Database; }

namespace codetldr {

// Forward declaration — Coordinator is the owner; RequestRouter holds a reference.
class Coordinator;
class ContextBuilder;
class ModelManager;
class VectorStore;

// Routes incoming JSON-RPC 2.0 requests to handler methods on Coordinator,
// HybridSearchEngine, and ContextBuilder.
// Supported methods:
//   health_check    -> {status: "ok", pid: N}
//   get_status      -> coordinator.get_status_json()
//   stop            -> coordinator.request_stop() then {ok: true}
//   search_text     -> HybridSearchEngine::search_text(query, language, limit)
//   search_symbols  -> HybridSearchEngine::search_symbols(query, kind, language, limit)
//   get_context     -> ContextBuilder::build(req)
//   <unknown>       -> JSON-RPC error {code: -32601, message: "Method not found"}
class RequestRouter {
public:
    // Legacy constructor: FTS5-only mode (no hybrid search). Used by existing tests.
    RequestRouter(Coordinator& coordinator, SQLite::Database& db);

    // Full constructor: hybrid search mode with model and vector store.
    // db_path is used for HybridSearchEngine's own read-only SQLite connection.
    // model and store may be null — hybrid engine degrades to FTS5-only when null.
    RequestRouter(Coordinator& coordinator,
                  SQLite::Database& db,
                  const std::filesystem::path& db_path,
                  ModelManager* model = nullptr,
                  VectorStore* store  = nullptr,
                  HybridSearchConfig hybrid_config = {});

    // Destructor defined in .cpp where complete types are available.
    ~RequestRouter();

    // Dispatch a JSON-RPC 2.0 request object and return a complete response object.
    nlohmann::json dispatch(const nlohmann::json& req);

private:
    Coordinator& coordinator_;
    SQLite::Database& db_;
    std::unique_ptr<HybridSearchEngine> hybrid_engine_;
    std::unique_ptr<ContextBuilder> context_builder_;
};

} // namespace codetldr
