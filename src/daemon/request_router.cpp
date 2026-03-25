#include "daemon/request_router.h"
#include "daemon/coordinator.h"
#include <unistd.h>

namespace codetldr {

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
    } else {
        nlohmann::json error;
        error["code"]    = -32601;
        error["message"] = "Method not found";
        response["error"] = error;
    }

    return response;
}

} // namespace codetldr
