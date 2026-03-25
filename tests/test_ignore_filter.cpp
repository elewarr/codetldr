#include "watcher/ignore_filter.h"
#include <filesystem>
#include <fstream>
#include <cassert>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;

// Minimal test harness
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                         \
    do {                                                          \
        if (cond) {                                               \
            ++g_pass;                                             \
        } else {                                                  \
            ++g_fail;                                             \
            fprintf(stderr, "FAIL: %s  (line %d)\n", msg, __LINE__); \
        }                                                         \
    } while (0)

using codetldr::IgnoreFilter;

// Helper: create a temp directory (RAII)
struct TempDir {
    fs::path path;
    TempDir() {
        char tmpl[] = "/tmp/codetldr_test_XXXXXX";
        char* p = ::mkdtemp(tmpl);
        if (!p) throw std::runtime_error("mkdtemp failed");
        path = p;
    }
    ~TempDir() {
        fs::remove_all(path);
    }
};

int main() {
    // --- Default pattern tests ---
    {
        // Default IgnoreFilter (no file — all defaults apply)
        IgnoreFilter f = IgnoreFilter::from_project_root("/nonexistent_dir_for_test");

        // build/ directory pattern
        CHECK(f.should_ignore("build/main.o"), "build/main.o should be ignored");

        // node_modules
        CHECK(f.should_ignore("node_modules/express/index.js"), "node_modules/express/index.js should be ignored");

        // .git
        CHECK(f.should_ignore(".git/HEAD"), ".git/HEAD should be ignored");

        // source file — should NOT be ignored
        CHECK(!f.should_ignore("src/main.cpp"), "src/main.cpp should NOT be ignored");

        // vendor directory
        CHECK(f.should_ignore("vendor/lib.py"), "vendor/lib.py should be ignored");
    }

    // --- Glob pattern: *.o ---
    {
        IgnoreFilter f;
        f.add_pattern("*.o");
        CHECK(f.should_ignore("build/foo.o"), "*.o should match build/foo.o");
        CHECK(f.should_ignore("foo.o"), "*.o should match foo.o");
        CHECK(!f.should_ignore("foo.cpp"), "*.o should not match foo.cpp");
    }

    // --- Negation pattern ---
    {
        IgnoreFilter f;
        f.add_pattern("*.o");
        f.add_pattern("!important.o");
        CHECK(!f.should_ignore("important.o"), "!important.o negation should un-ignore important.o");
        CHECK(f.should_ignore("other.o"), "*.o still matches other.o after negation");
    }

    // --- Directory pattern: dist/ ---
    {
        IgnoreFilter f;
        f.add_pattern("dist/");
        CHECK(f.should_ignore("dist/bundle.js"), "dist/ should ignore dist/bundle.js");
        CHECK(f.should_ignore("dist/sub/file.js"), "dist/ should ignore dist/sub/file.js");
        // A file named "distfile.js" should NOT be ignored
        CHECK(!f.should_ignore("distfile.js"), "dist/ should NOT match distfile.js");
    }

    // --- from_project_root loads .codetldrignore ---
    {
        TempDir tmp;
        // Write a custom .codetldrignore
        std::ofstream ofs(tmp.path / ".codetldrignore");
        ofs << "# comment line\n";
        ofs << "\n";  // blank line
        ofs << "*.log\n";
        ofs << "custom_dir/\n";
        ofs.close();

        IgnoreFilter f = IgnoreFilter::from_project_root(tmp.path);
        CHECK(f.should_ignore("app.log"), "custom *.log pattern should work");
        CHECK(f.should_ignore("custom_dir/file.txt"), "custom_dir/ pattern should work");
        // default build/ should NOT apply since we have a custom file
        // (The .codetldrignore replaces defaults)
        CHECK(!f.should_ignore("build/main.o"), "build/ not in custom .codetldrignore should not be ignored");
    }

    // --- from_project_root uses defaults when no .codetldrignore ---
    {
        TempDir tmp;
        // No .codetldrignore file
        IgnoreFilter f = IgnoreFilter::from_project_root(tmp.path);
        CHECK(f.should_ignore("build/main.o"), "default build/ should apply when no .codetldrignore");
        CHECK(f.should_ignore("node_modules/pkg/file.js"), "default node_modules/ should apply");
    }

    // --- .codetldr directory itself should be ignored ---
    {
        IgnoreFilter f = IgnoreFilter::from_project_root("/nonexistent_dir_for_test");
        CHECK(f.should_ignore(".codetldr/index.sqlite"), ".codetldr/ should be ignored by default");
    }

    // --- target/ (Rust/Maven) ---
    {
        IgnoreFilter f = IgnoreFilter::from_project_root("/nonexistent_dir_for_test");
        CHECK(f.should_ignore("target/debug/binary"), "target/ should be ignored");
    }

    // --- Compiled artifacts: .so, .dylib, .a ---
    {
        IgnoreFilter f = IgnoreFilter::from_project_root("/nonexistent_dir_for_test");
        CHECK(f.should_ignore("lib/libfoo.so"), "*.so should be ignored");
        CHECK(f.should_ignore("lib/libfoo.dylib"), "*.dylib should be ignored");
        CHECK(f.should_ignore("lib/libfoo.a"), "*.a should be ignored");
    }

    // Print summary
    int total = g_pass + g_fail;
    printf("test_ignore_filter: %d/%d passed\n", g_pass, total);
    return g_fail == 0 ? 0 : 1;
}
