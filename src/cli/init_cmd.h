#pragma once
#include <CLI/CLI.hpp>
#include <string>

// Register the init subcommand on the CLI11 app.
// project_root_str is the shared --project-root option string.
void register_init_cmd(CLI::App& app, std::string& project_root_str);
