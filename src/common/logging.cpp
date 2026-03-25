#include "common/logging.h"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace codetldr {

static std::shared_ptr<spdlog::logger> g_logger;

void init_logging(bool verbose) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    g_logger = std::make_shared<spdlog::logger>("codetldr", sink);
    g_logger->set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger> logger() {
    if (!g_logger) init_logging();
    return g_logger;
}

} // namespace codetldr
