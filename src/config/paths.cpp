#include "config/paths.h"
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>

namespace codetldr {

namespace {

std::filesystem::path get_home() {
    if (const char* h = std::getenv("HOME"); h && h[0] != '\0')
        return h;
    struct passwd* pw = getpwuid(getuid());
    return pw ? pw->pw_dir : "/tmp";
}

std::filesystem::path xdg_dir(const char* env_var, const char* fallback_rel) {
    if (const char* val = std::getenv(env_var); val && val[0] != '\0') {
        return std::filesystem::path(val) / "codetldr";
    }
    return get_home() / fallback_rel / "codetldr";
}

} // namespace

XdgPaths resolve_xdg_paths() {
    return XdgPaths{
        .config_home = xdg_dir("XDG_CONFIG_HOME", ".config"),
        .cache_home  = xdg_dir("XDG_CACHE_HOME",  ".cache"),
        .data_home   = xdg_dir("XDG_DATA_HOME",   ".local/share"),
    };
}

} // namespace codetldr
