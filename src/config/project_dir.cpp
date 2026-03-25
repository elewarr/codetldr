#include "config/project_dir.h"
#include <fstream>
#include <string>

namespace codetldr {

std::optional<std::filesystem::path> find_git_root(const std::filesystem::path& start) {
    namespace fs = std::filesystem;
    fs::path current = fs::absolute(start);

    while (true) {
        if (fs::exists(current / ".git") && fs::is_directory(current / ".git")) {
            return current;
        }
        fs::path parent = current.parent_path();
        if (parent == current) {
            // Reached filesystem root
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

InitResult init_project_dir(const std::filesystem::path& project_root) {
    namespace fs = std::filesystem;

    InitResult result;

    // 1. Create .codetldr/ directory
    fs::create_directories(project_root / ".codetldr");
    result.codetldr_created = true;

    // 2. Find git root
    auto git_root = find_git_root(project_root);
    if (!git_root) {
        result.git_exclude_updated = false;
        result.note = "Not inside a git repository";
        return result;
    }

    // 3. Build exclude file path
    fs::path exclude_file = *git_root / ".git" / "info" / "exclude";

    // 4. Ensure .git/info/ directory exists
    fs::create_directories(exclude_file.parent_path());

    // 5. Check if .codetldr/ already present in exclude file
    if (fs::exists(exclude_file)) {
        std::ifstream in(exclude_file);
        std::string line;
        while (std::getline(in, line)) {
            if (line == ".codetldr/") {
                // Entry already present -- no update needed
                result.git_exclude_updated = false;
                return result;
            }
        }
    }

    // 6. Append .codetldr/ entry to exclude file
    {
        std::ofstream out(exclude_file, std::ios::app);
        out << "\n# codetldr per-project index\n.codetldr/\n";
    }
    result.git_exclude_updated = true;

    return result;
}

} // namespace codetldr
