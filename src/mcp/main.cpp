// codetldr-mcp: MCP stdio server for CodeTLDR
//
// Reads NDJSON from stdin, dispatches MCP protocol messages, proxies
// tools/call requests to the daemon via DaemonClient, writes NDJSON to stdout.
//
// CRITICAL: stdout is the protocol wire. All logging goes to stderr via spdlog.
// Protocol: MCP 2025-11-25, stdio transport (newline-delimited JSON)

#include "daemon/daemon_client.h"
#include "config/project_dir.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <CLI/CLI.hpp>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Helper: make_tools_list_response
// Build all 8 MCP tool definitions with inputSchema and wrap in JSON-RPC envelope.
// ---------------------------------------------------------------------------
static nlohmann::json make_tools_list_response(const nlohmann::json& id) {
    nlohmann::json tools = nlohmann::json::array();

    tools.push_back({
        {"name", "search_symbols"},
        {"description", "Search for symbols (functions, classes, methods) by name using FTS5 full-text search with BM25 ranking. Returns ranked results with file path, line number, signature, and documentation."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"query", {{"type","string"},{"description","Symbol name or partial name to search for"}}},
                {"kind",  {{"type","string"},{"description","Filter by symbol kind: function, class, method, struct, enum (optional)"}}},
                {"limit", {{"type","integer"},{"description","Maximum results (default 20)"}}}
            }},
            {"required", nlohmann::json::array({"query"})}
        }}
    });

    tools.push_back({
        {"name", "search_text"},
        {"description", "Full-text search over symbol names, documentation, and source content. Use for finding code by keyword, not by exact symbol name."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"query", {{"type","string"},{"description","Search terms"}}},
                {"limit", {{"type","integer"},{"description","Maximum results (default 20)"}}}
            }},
            {"required", nlohmann::json::array({"query"})}
        }}
    });

    tools.push_back({
        {"name", "get_file_summary"},
        {"description", "Get a token-efficient structured summary of a source file: all symbols with signatures, line ranges, and documentation. Condensed format uses <10% of raw source tokens."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file_path", {{"type","string"},{"description","Absolute or project-relative path to the source file"}}},
                {"format",    {{"type","string"},{"description","Output format: condensed (default), detailed, diff_aware"}}}
            }},
            {"required", nlohmann::json::array({"file_path"})}
        }}
    });

    tools.push_back({
        {"name", "get_function_detail"},
        {"description", "Get detailed information about a specific function or method: signature, documentation, callers, callees, and line range."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name",      {{"type","string"},{"description","Exact function or method name"}}},
                {"file_path", {{"type","string"},{"description","Scope search to this file (optional)"}}}
            }},
            {"required", nlohmann::json::array({"name"})}
        }}
    });

    tools.push_back({
        {"name", "get_call_graph"},
        {"description", "Get forward (callees) and backward (callers) call relationships for a function. Returns approximate call graph from AST analysis."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name",      {{"type","string"},{"description","Function name"}}},
                {"direction", {{"type","string"},{"description","callers, callees, or both (default: both)"}}},
                {"depth",     {{"type","integer"},{"description","Traversal depth (default 1)"}}}
            }},
            {"required", nlohmann::json::array({"name"})}
        }}
    });

    tools.push_back({
        {"name", "get_project_overview"},
        {"description", "Get a high-level overview of the project: language breakdown, file count, top-level symbols by language, indexing status. Use as first call to orient in an unfamiliar codebase."},
        {"inputSchema", {
            {"type", "object"},
            {"additionalProperties", false}
        }}
    });

    tools.push_back({
        {"name", "get_control_flow"},
        {"description", "Get control flow graph (CFG) for a function: branches, loops, returns, and switch cases. Returns nodes with type, condition, line, and nesting depth. Empty for unsupported languages."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name",      {{"type","string"},{"description","Exact function or method name"}}},
                {"file_path", {{"type","string"},{"description","Scope search to this file (optional)"}}}
            }},
            {"required", nlohmann::json::array({"name"})}
        }}
    });

    tools.push_back({
        {"name", "get_data_flow"},
        {"description", "Get data flow graph (DFG) for a function: assignments, parameter bindings, and return values. Returns edges with type, lhs variable, rhs snippet, and line. Empty for unsupported languages."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"name",      {{"type","string"},{"description","Exact function or method name"}}},
                {"file_path", {{"type","string"},{"description","Scope search to this file (optional)"}}}
            }},
            {"required", nlohmann::json::array({"name"})}
        }}
    });

    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"result",  {{"tools", tools}}}
    };
}

// ---------------------------------------------------------------------------
// Helper: dispatch_tool_call
// Map MCP tool name to daemon RPC method, call via DaemonClient, format result.
// Creates a FRESH DaemonClient for each call (one-request-per-connection design).
// ---------------------------------------------------------------------------
static nlohmann::json dispatch_tool_call(const std::string& tool_name,
                                          const nlohmann::json& arguments,
                                          const fs::path& sock_path) {
    // Create fresh connection for this call
    codetldr::DaemonClient client;
    if (!client.connect(sock_path)) {
        return {
            {"content", {{{"type","text"},{"text","Daemon not running. Start with: codetldr start"}}}},
            {"isError", true}
        };
    }

    try {
        nlohmann::json daemon_resp;

        if (tool_name == "search_symbols") {
            daemon_resp = client.call("search_symbols", arguments);
        } else if (tool_name == "search_text") {
            daemon_resp = client.call("search_text", arguments);
        } else if (tool_name == "get_file_summary") {
            daemon_resp = client.call("get_file_summary", arguments);
        } else if (tool_name == "get_function_detail") {
            daemon_resp = client.call("get_function_detail", arguments);
        } else if (tool_name == "get_call_graph") {
            daemon_resp = client.call("get_call_graph", arguments);
        } else if (tool_name == "get_project_overview") {
            daemon_resp = client.call("get_project_overview", nlohmann::json::object());
        } else if (tool_name == "get_control_flow") {
            daemon_resp = client.call("get_control_flow", arguments);
        } else if (tool_name == "get_data_flow") {
            daemon_resp = client.call("get_data_flow", arguments);
        } else {
            return {
                {"content", {{{"type","text"},{"text","Unknown tool: " + tool_name}}}},
                {"isError", true}
            };
        }

        // Extract result from JSON-RPC envelope
        if (daemon_resp.contains("result")) {
            std::string text = daemon_resp["result"].dump(2);
            return {
                {"content", {{{"type","text"},{"text", text}}}}
            };
        } else if (daemon_resp.contains("error")) {
            std::string msg = daemon_resp["error"].value("message", "daemon error");
            return {
                {"content", {{{"type","text"},{"text", msg}}}},
                {"isError", true}
            };
        } else {
            return {
                {"content", {{{"type","text"},{"text", daemon_resp.dump(2)}}}},
                {"isError", true}
            };
        }

    } catch (const std::exception& ex) {
        return {
            {"content", {{{"type","text"},{"text", std::string("Daemon error: ") + ex.what()}}}},
            {"isError", true}
        };
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    CLI::App app{"CodeTLDR MCP stdio server"};

    std::string project_root_str;
    app.add_option("--project-root", project_root_str,
                   "Project root directory (default: git root or cwd)");

    CLI11_PARSE(app, argc, argv);

    // Resolve project root: --project-root > find_git_root(cwd) > cwd
    fs::path project_root;
    if (!project_root_str.empty()) {
        project_root = fs::path(project_root_str);
    } else {
        auto git_root = codetldr::find_git_root(fs::current_path());
        project_root = git_root.value_or(fs::current_path());
    }

    fs::path sock_path = project_root / ".codetldr" / "daemon.sock";

    // Configure spdlog to log to stderr ONLY — stdout is the protocol wire
    auto logger = spdlog::stderr_color_mt("mcp");
    spdlog::set_default_logger(logger);
    spdlog::set_level(spdlog::level::info);

    spdlog::info("codetldr-mcp starting, project_root={}", project_root.string());
    spdlog::info("daemon socket: {}", sock_path.string());

    // Main NDJSON protocol loop
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        nlohmann::json req;
        try {
            req = nlohmann::json::parse(line);
        } catch (const std::exception& ex) {
            spdlog::warn("Failed to parse JSON line: {}", ex.what());
            continue;
        }

        std::string method = req.value("method", "");
        // id is optional — notifications have no id
        nlohmann::json id = req.contains("id") ? req["id"] : nlohmann::json(nullptr);
        bool is_notification = !req.contains("id");

        spdlog::debug("Received method={} is_notification={}", method, is_notification);

        if (method == "initialize") {
            nlohmann::json result = {
                {"protocolVersion", "2025-11-25"},
                {"capabilities",   {{"tools", nlohmann::json::object()}}},
                {"serverInfo",     {{"name", "codetldr"}, {"version", "0.1.0"}}}
            };
            nlohmann::json response = {
                {"jsonrpc", "2.0"},
                {"id",      id},
                {"result",  result}
            };
            std::cout << response.dump() << "\n" << std::flush;

        } else if (method == "notifications/initialized") {
            // Notification — no response per MCP spec
            spdlog::debug("Received notifications/initialized");

        } else if (method == "ping") {
            nlohmann::json response = {
                {"jsonrpc", "2.0"},
                {"id",      id},
                {"result",  nlohmann::json::object()}
            };
            std::cout << response.dump() << "\n" << std::flush;

        } else if (method == "tools/list") {
            std::cout << make_tools_list_response(id).dump() << "\n" << std::flush;

        } else if (method == "tools/call") {
            std::string tool_name;
            nlohmann::json arguments = nlohmann::json::object();

            if (req.contains("params")) {
                tool_name = req["params"].value("name", "");
                if (req["params"].contains("arguments")) {
                    arguments = req["params"]["arguments"];
                }
            }

            nlohmann::json tool_result = dispatch_tool_call(tool_name, arguments, sock_path);
            nlohmann::json response = {
                {"jsonrpc", "2.0"},
                {"id",      id},
                {"result",  tool_result}
            };
            std::cout << response.dump() << "\n" << std::flush;

        } else if (is_notification) {
            // Unknown notification — ignore silently per MCP spec
            spdlog::debug("Ignoring unknown notification: {}", method);

        } else {
            // Unknown method with id — return JSON-RPC -32601 error
            nlohmann::json response = {
                {"jsonrpc", "2.0"},
                {"id",      id},
                {"error",   {{"code", -32601}, {"message", "Method not found"}}}
            };
            std::cout << response.dump() << "\n" << std::flush;
        }
    }

    spdlog::info("stdin closed, exiting");
    return 0;
}
