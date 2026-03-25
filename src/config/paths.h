#pragma once
#include <filesystem>

namespace codetldr {

struct XdgPaths {
    std::filesystem::path config_home;  // ~/.config/codetldr or $XDG_CONFIG_HOME/codetldr
    std::filesystem::path cache_home;   // ~/.cache/codetldr  or $XDG_CACHE_HOME/codetldr
    std::filesystem::path data_home;    // ~/.local/share/codetldr or $XDG_DATA_HOME/codetldr
};

// Resolve XDG directories. Reads XDG_CONFIG_HOME, XDG_CACHE_HOME, XDG_DATA_HOME
// env vars. Falls back to $HOME-based defaults. Falls back to getpwuid if HOME unset.
XdgPaths resolve_xdg_paths();

} // namespace codetldr
