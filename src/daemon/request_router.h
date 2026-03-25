#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace codetldr {

// Forward declaration — Coordinator is the owner; RequestRouter holds a reference.
class Coordinator;

// Routes incoming JSON-RPC 2.0 requests to handler methods on Coordinator.
// Supported methods:
//   health_check  -> {status: "ok", pid: N}
//   get_status    -> coordinator.get_status_json()
//   stop          -> coordinator.request_stop() then {ok: true}
//   <unknown>     -> JSON-RPC error {code: -32601, message: "Method not found"}
class RequestRouter {
public:
    explicit RequestRouter(Coordinator& coordinator) : coordinator_(coordinator) {}

    // Dispatch a JSON-RPC 2.0 request object and return a complete response object.
    nlohmann::json dispatch(const nlohmann::json& req);

private:
    Coordinator& coordinator_;
};

} // namespace codetldr
