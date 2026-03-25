#pragma once
#include <CLI/CLI.hpp>
#include <string>

// Register the search subcommand on the CLI11 app.
// project_root_str is the shared --project-root option string.
void register_search_cmd(CLI::App& app, std::string& project_root_str);
