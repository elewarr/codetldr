#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace codetldr {

struct InitResult {
    bool codetldr_created = false;
    bool git_exclude_updated = false;
    std::string note;  // Informational message (e.g., "not a git repo")
};

// Create .codetldr/ in project_root and add entry to .git/info/exclude.
// Never modifies .gitignore. If not in a git repo, creates .codetldr/ but
// skips git exclude (sets note explaining why).
InitResult init_project_dir(const std::filesystem::path& project_root);

// Walk up from start to find directory containing .git/
std::optional<std::filesystem::path> find_git_root(const std::filesystem::path& start);

} // namespace codetldr
