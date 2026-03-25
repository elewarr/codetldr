#include "config/paths.h"
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main() {
    namespace fs = std::filesystem;

    // Test 1: Default paths contain "codetldr"
    auto paths = codetldr::resolve_xdg_paths();
    assert(!paths.config_home.empty());
    assert(paths.config_home.string().find("codetldr") != std::string::npos);
    assert(paths.cache_home.string().find("codetldr") != std::string::npos);
    assert(paths.data_home.string().find("codetldr") != std::string::npos);
    std::cout << "PASS: default paths contain codetldr\n";

    // Test 2: Default fallback paths end with expected suffixes
    // Unset all XDG vars to test defaults
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("XDG_CACHE_HOME");
    unsetenv("XDG_DATA_HOME");
    auto defaults = codetldr::resolve_xdg_paths();
    assert(defaults.config_home.string().find(".config/codetldr") != std::string::npos);
    assert(defaults.cache_home.string().find(".cache/codetldr") != std::string::npos);
    assert(defaults.data_home.string().find(".local/share/codetldr") != std::string::npos);
    std::cout << "PASS: default fallbacks are correct\n";

    // Test 3: XDG_CONFIG_HOME override
    setenv("XDG_CONFIG_HOME", "/tmp/codetldr_test_xdg_config", 1);
    auto overridden = codetldr::resolve_xdg_paths();
    assert(overridden.config_home == fs::path("/tmp/codetldr_test_xdg_config/codetldr"));
    unsetenv("XDG_CONFIG_HOME");
    std::cout << "PASS: XDG_CONFIG_HOME override works\n";

    // Test 4: XDG_CACHE_HOME override
    setenv("XDG_CACHE_HOME", "/tmp/codetldr_test_xdg_cache", 1);
    auto cache_override = codetldr::resolve_xdg_paths();
    assert(cache_override.cache_home == fs::path("/tmp/codetldr_test_xdg_cache/codetldr"));
    unsetenv("XDG_CACHE_HOME");
    std::cout << "PASS: XDG_CACHE_HOME override works\n";

    // Test 5: XDG_DATA_HOME override
    setenv("XDG_DATA_HOME", "/tmp/codetldr_test_xdg_data", 1);
    auto data_override = codetldr::resolve_xdg_paths();
    assert(data_override.data_home == fs::path("/tmp/codetldr_test_xdg_data/codetldr"));
    unsetenv("XDG_DATA_HOME");
    std::cout << "PASS: XDG_DATA_HOME override works\n";

    std::cout << "All XDG path tests passed.\n";
    return 0;
}
