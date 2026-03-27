#include "daemon/request_router.h"
#include "daemon/coordinator.h"
#include "query/search_engine.h"
#include "query/context_builder.h"
#include <unistd.h>

namespace codetldr {

RequestRouter::RequestRouter(Coordinator& coordinator, SQLite::Database& db)
    : coordinator_(coordinator)
    , db_(db)
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

            auto results = search_engine_->search_text(query, "", limit);

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

            auto results = search_engine_->search_symbols(query, kind, "", limit);

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

    } else if (method == "get_file_summary") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();

            if (!params.contains("file_path") || !params["file_path"].is_string()) {
                nlohmann::json error;
                error["code"]    = -32602;
                error["message"] = "file_path required";
                response["error"] = error;
            } else {
                std::string file_path = params["file_path"].get<std::string>();
                std::string fmt_str = params.value("format", "condensed");

                ContextRequest ctx_req;
                if (fmt_str == "detailed") {
                    ctx_req.format = ContextFormat::kDetailed;
                } else if (fmt_str == "diff_aware") {
                    ctx_req.format = ContextFormat::kDiffAware;
                } else {
                    ctx_req.format = ContextFormat::kCondensed;
                }
                ctx_req.file_paths = {file_path};
                ctx_req.max_symbols = 200;

                ContextResponse ctx_resp = context_builder_->build(ctx_req);

                nlohmann::json result;
                result["text"]             = ctx_resp.text;
                result["symbol_count"]     = ctx_resp.symbol_count;
                result["file_count"]       = ctx_resp.file_count;
                result["estimated_tokens"] = ctx_resp.estimated_tokens;
                response["result"] = std::move(result);
            }
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else if (method == "get_function_detail") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();

            if (!params.contains("name") || !params["name"].is_string()) {
                nlohmann::json error;
                error["code"]    = -32602;
                error["message"] = "name required";
                response["error"] = error;
            } else {
                std::string name = params["name"].get<std::string>();
                std::string file_path = params.value("file_path", "");

                SymbolInfo sym = context_builder_->find_symbol(name, file_path);

                nlohmann::json result;
                result["found"] = sym.found;
                result["name"]  = sym.found ? sym.name : name;

                if (sym.found) {
                    result["kind"]          = sym.kind;
                    result["signature"]     = sym.signature;
                    result["documentation"] = sym.documentation;
                    result["file_path"]     = sym.file_path;
                    result["line_start"]    = sym.line_start;
                    result["line_end"]      = sym.line_end;

                    auto callers = context_builder_->get_caller_names(sym.id);
                    auto callees = context_builder_->get_callee_names(sym.id);

                    result["callers"] = nlohmann::json::array();
                    for (const auto& c : callers) result["callers"].push_back(c);

                    result["callees"] = nlohmann::json::array();
                    for (const auto& c : callees) result["callees"].push_back(c);
                }

                response["result"] = std::move(result);
            }
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else if (method == "get_call_graph") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();

            if (!params.contains("name") || !params["name"].is_string()) {
                nlohmann::json error;
                error["code"]    = -32602;
                error["message"] = "name required";
                response["error"] = error;
            } else {
                std::string name      = params["name"].get<std::string>();
                std::string direction = params.value("direction", "both");
                int depth             = params.value("depth", 1);
                // Cap depth at 3
                if (depth > 3) depth = 3;

                SymbolInfo sym = context_builder_->find_symbol(name);

                nlohmann::json result;
                result["name"]    = name;
                result["found"]   = sym.found;
                result["callers"] = nlohmann::json::array();
                result["callees"] = nlohmann::json::array();

                if (sym.found) {
                    if (direction == "callers" || direction == "both") {
                        auto callers = context_builder_->get_caller_names(sym.id);
                        for (const auto& c : callers) result["callers"].push_back(c);
                    }
                    if (direction == "callees" || direction == "both") {
                        auto callees = context_builder_->get_callee_names(sym.id);
                        for (const auto& c : callees) result["callees"].push_back(c);

                        // Recurse for depth > 1
                        if (depth > 1) {
                            nlohmann::json nested = nlohmann::json::object();
                            for (const auto& callee_name : callees) {
                                SymbolInfo callee_sym = context_builder_->find_symbol(callee_name);
                                if (callee_sym.found) {
                                    auto sub_callees = context_builder_->get_callee_names(callee_sym.id);
                                    nlohmann::json sub_arr = nlohmann::json::array();
                                    for (const auto& sc : sub_callees) sub_arr.push_back(sc);
                                    nested[callee_name] = std::move(sub_arr);
                                }
                            }
                            if (!nested.empty()) {
                                result["callees_depth2"] = std::move(nested);
                            }
                        }
                    }
                }

                response["result"] = std::move(result);
            }
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else if (method == "get_control_flow") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();

            if (!params.contains("name") || !params["name"].is_string()) {
                nlohmann::json error;
                error["code"]    = -32602;
                error["message"] = "name required";
                response["error"] = error;
            } else {
                std::string name      = params["name"].get<std::string>();
                std::string file_path = params.value("file_path", "");

                SymbolInfo sym = context_builder_->find_symbol(name, file_path);

                nlohmann::json result;
                result["found"] = sym.found;
                result["name"]  = sym.found ? sym.name : name;
                result["nodes"] = nlohmann::json::array();

                if (sym.found) {
                    result["nodes"] = context_builder_->get_control_flow(sym.id);
                }

                response["result"] = std::move(result);
            }
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else if (method == "get_data_flow") {
        try {
            const auto& params = req.contains("params") ? req["params"] : nlohmann::json::object();

            if (!params.contains("name") || !params["name"].is_string()) {
                nlohmann::json error;
                error["code"]    = -32602;
                error["message"] = "name required";
                response["error"] = error;
            } else {
                std::string name      = params["name"].get<std::string>();
                std::string file_path = params.value("file_path", "");

                SymbolInfo sym = context_builder_->find_symbol(name, file_path);

                nlohmann::json result;
                result["found"] = sym.found;
                result["name"]  = sym.found ? sym.name : name;
                result["edges"] = nlohmann::json::array();

                if (sym.found) {
                    result["edges"] = context_builder_->get_data_flow(sym.id);
                }

                response["result"] = std::move(result);
            }
        } catch (const std::exception& e) {
            nlohmann::json error;
            error["code"]    = -32000;
            error["message"] = e.what();
            response["error"] = error;
        }

    } else if (method == "get_project_overview") {
        try {
            nlohmann::json result;

            // File count
            {
                SQLite::Statement q(db_, "SELECT COUNT(*) FROM files");
                q.executeStep();
                result["file_count"] = q.getColumn(0).getInt();
            }

            // Symbol count
            {
                SQLite::Statement q(db_, "SELECT COUNT(*) FROM symbols");
                q.executeStep();
                result["symbol_count"] = q.getColumn(0).getInt();
            }

            // Language breakdown
            {
                nlohmann::json breakdown = nlohmann::json::object();
                SQLite::Statement q(db_,
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

            // Language support matrix
            result["language_support"] = coordinator_.get_language_support();

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
