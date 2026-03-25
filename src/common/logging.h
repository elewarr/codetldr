#pragma once
#include <spdlog/spdlog.h>
#include <memory>

namespace codetldr {
void init_logging(bool verbose = false);
std::shared_ptr<spdlog::logger> logger();
} // namespace codetldr
