#include "daemon/request_router.h"
#include "daemon/coordinator.h"
#include "query/search_engine.h"
#include "query/context_builder.h"
#include <unistd.h>

namespace codetldr {

RequestRouter::RequestRouter(Coordinator& coordinator, SQLite::Database& db)
    : coordinator_(coordinator)
    , search_engine_(std::make_unique<SearchEngine>(db))
    , context_builder_(std::make_unique<ContextBuilder>(db))
{}

// Destructor defined here where SearchEngine and ContextBuilder are complete types.
RequestRouter::~RequestRouter() = default;

nlohmann::json RequestRouter::dispatch(const nlohmann::json& req) {
    nlohmann::json response;
    response["jsonrpc"] = "2.0";

    // Extract id (may be null or absent)
    if (req.contains("id")) {
        response["id"] = req["id"];
    } else {
        response["id"] = nullptr;
    }

    std::string method;
    if (req.contains("method") && req["method"].is_string()) {
        method = req["method"].get<std::string>();
    }

    if (method == "health_check") {
        nlohmann::json result;
        result["status"] = "ok";
        result["pid"]    = static_cast<int>(::getpid());
        response["result"] = result;

    } else if (method == "get_status") {
        response["result"] = coordinator_.get_status_json();

    } else if (method == "stop") {
        coordinator_.request_stop();
        nlohmann::json result;
        result["ok"] = true;
        response["result"] = result;

    } else if (method == "search_text") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();
            std::string query = params.value("query", "");
            int limit = params.value("limit", 20);

            auto results = search_engine_->search_text(query, limit);

            nlohmann::json arr = nlohmann::json::array();
            for (const auto& r : results) {
                nlohmann::json item;
                item["symbol_id"]     = r.symbol_id;
                item["name"]          = r.name;
                item["kind"]          = r.kind;
                item["signature"]     = r.signature;
                item["documentation"] = r.documentation;
                item["file_path"]     = r.file_path;
                item["line_start"]    = r.line_start;
                item["rank"]          = r.rank;
                arr.push_back(std::move(item));
            }
            response["result"] = std::move(arr);
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else if (method == "search_symbols") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();
            std::string query = params.value("query", "");
            std::string kind  = params.value("kind", "");
            int limit = params.value("limit", 20);

            auto results = search_engine_->search_symbols(query, kind, limit);

            nlohmann::json arr = nlohmann::json::array();
            for (const auto& r : results) {
                nlohmann::json item;
                item["symbol_id"]     = r.symbol_id;
                item["name"]          = r.name;
                item["kind"]          = r.kind;
                item["signature"]     = r.signature;
                item["documentation"] = r.documentation;
                item["file_path"]     = r.file_path;
                item["line_start"]    = r.line_start;
                item["rank"]          = r.rank;
                arr.push_back(std::move(item));
            }
            response["result"] = std::move(arr);
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else if (method == "get_context") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();

            ContextRequest ctx_req;

            // Map format string to enum
            std::string fmt_str = params.value("format", "condensed");
            if (fmt_str == "detailed") {
                ctx_req.format = ContextFormat::kDetailed;
            } else if (fmt_str == "diff_aware") {
                ctx_req.format = ContextFormat::kDiffAware;
            } else {
                ctx_req.format = ContextFormat::kCondensed;
            }

            // Extract file_paths array
            if (params.contains("files") && params["files"].is_array()) {
                for (const auto& f : params["files"]) {
                    if (f.is_string()) {
                        ctx_req.file_paths.push_back(f.get<std::string>());
                    }
                }
            }

            // Extract changed_paths array
            if (params.contains("changed") && params["changed"].is_array()) {
                for (const auto& c : params["changed"]) {
                    if (c.is_string()) {
                        ctx_req.changed_paths.push_back(c.get<std::string>());
                    }
                }
            }

            ctx_req.max_symbols = params.value("max_symbols", 200);

            ContextResponse ctx_resp = context_builder_->build(ctx_req);

            nlohmann::json result;
            result["text"]             = ctx_resp.text;
            result["symbol_count"]     = ctx_resp.symbol_count;
            result["file_count"]       = ctx_resp.file_count;
            result["estimated_tokens"] = ctx_resp.estimated_tokens;
            response["result"] = std::move(result);
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else {
        nlohmann::json error;
        error["code"]    = -32601;
        error["message"] = "Method not found";
        response["error"] = error;
    }

    return response;
}

} // namespace codetldr
