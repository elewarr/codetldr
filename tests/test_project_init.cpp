#include "config/project_dir.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

int main() {
    namespace fs = std::filesystem;

    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_init";
    fs::remove_all(test_dir);

    // Test 1: Init creates .codetldr/ directory
    {
        fs::create_directories(test_dir / ".git" / "info");
        auto result = codetldr::init_project_dir(test_dir);
        assert(result.codetldr_created);
        assert(fs::is_directory(test_dir / ".codetldr"));
        std::cout << "PASS: .codetldr/ created\n";
    }

    // Test 2: .git/info/exclude contains .codetldr/
    {
        const auto exclude = test_dir / ".git" / "info" / "exclude";
        std::ifstream in(exclude);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        assert(content.find(".codetldr/") != std::string::npos);
        std::cout << "PASS: .git/info/exclude updated\n";
    }

    // Test 3: .gitignore does NOT exist (never created)
    {
        assert(!fs::exists(test_dir / ".gitignore"));
        std::cout << "PASS: .gitignore not created\n";
    }

    // Test 4: Running init twice does not duplicate entry
    {
        auto result2 = codetldr::init_project_dir(test_dir);
        assert(result2.codetldr_created);
        assert(!result2.git_exclude_updated);  // already present, no update

        const auto exclude = test_dir / ".git" / "info" / "exclude";
        std::ifstream in(exclude);
        std::string content((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
        // Count occurrences of ".codetldr/"
        size_t count = 0;
        size_t pos = 0;
        while ((pos = content.find(".codetldr/", pos)) != std::string::npos) {
            count++;
            pos += 10;
        }
        assert(count == 1);
        std::cout << "PASS: no duplicate entry on re-init\n";
    }

    // Test 5: Init outside git repo creates dir but skips exclude
    {
        const fs::path no_git = fs::temp_directory_path() / "codetldr_test_nogit";
        fs::remove_all(no_git);
        fs::create_directories(no_git);

        auto result = codetldr::init_project_dir(no_git);
        assert(result.codetldr_created);
        assert(!result.git_exclude_updated);
        assert(!result.note.empty());  // should have a note explaining why
        assert(fs::is_directory(no_git / ".codetldr"));
        std::cout << "PASS: works outside git repo\n";

        fs::remove_all(no_git);
    }

    // Cleanup
    fs::remove_all(test_dir);

    std::cout << "All project init tests passed.\n";
    return 0;
}
