#pragma once
#include <CLI/CLI.hpp>
#include <string>

// Register the model subcommand (codetldr model download) on the CLI11 app.
// project_root_str is the shared --project-root option string (unused for model, but follows convention).
void register_model_cmd(CLI::App& app, std::string& project_root_str);
