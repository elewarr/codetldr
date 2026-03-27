#include "embedding/chunk_extractor.h"
#include "storage/database.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    namespace fs = std::filesystem;

    // Setup: temp directory with a source file and SQLite DB
    const fs::path test_dir = fs::temp_directory_path() / "codetldr_test_chunks";
    fs::remove_all(test_dir);
    fs::create_directories(test_dir);

    // Create a fake source file
    const fs::path src_file = test_dir / "example.cpp";
    {
        std::ofstream f(src_file);
        f << "// line 1\n"        // line 1
          << "void foo() {\n"     // line 2
          << "  int x = 1;\n"     // line 3
          << "  return;\n"        // line 4
          << "}\n"                // line 5
          << "\n"                 // line 6
          << "int bar(int a) {\n" // line 7
          << "  return a + 1;\n"  // line 8
          << "}\n";               // line 9
    }

    // Open a real SQLite database with migrations
    auto db = codetldr::Database::open(test_dir / "index.sqlite");

    // Seed files and symbols tables
    db.raw().exec("INSERT INTO files(id, path, mtime_ns) VALUES(1, 'example.cpp', 0)");
    db.raw().exec(
        "INSERT INTO symbols(id, file_id, kind, name, signature, line_start, line_end) "
        "VALUES(10, 1, 'function', 'foo', '()', 2, 5)");
    db.raw().exec(
        "INSERT INTO symbols(id, file_id, kind, name, signature, line_start, line_end) "
        "VALUES(11, 1, 'function', 'bar', '(int a)', 7, 9)");
    // Add a class symbol (should be excluded -- only function/method)
    db.raw().exec(
        "INSERT INTO symbols(id, file_id, kind, name, signature, line_start, line_end) "
        "VALUES(12, 1, 'class', 'MyClass', '', 1, 9)");

    // Test 1: make_context_header format (CHK-02)
    {
        auto hdr = codetldr::make_context_header("example.cpp", "function", "foo", "()");
        assert(hdr == "// example.cpp\n// function: foo()\n\n");
        std::cout << "PASS: make_context_header format correct\n";
    }

    // Test 2: make_context_header with empty signature
    {
        auto hdr = codetldr::make_context_header("a.cpp", "function", "main", "");
        assert(hdr == "// a.cpp\n// function: main\n\n");
        std::cout << "PASS: make_context_header empty signature\n";
    }

    // Test 3: extract_chunks returns only function/method symbols (CHK-01)
    {
        auto chunks = codetldr::extract_chunks(db.raw(), test_dir);
        assert(chunks.size() == 2);  // foo and bar, NOT MyClass
        std::cout << "PASS: extract_chunks returns 2 function chunks (excludes class)\n";
    }

    // Test 4: chunk text starts with context header (CHK-02)
    {
        auto chunks = codetldr::extract_chunks(db.raw(), test_dir);
        // Find foo chunk
        const codetldr::Chunk* foo = nullptr;
        for (const auto& c : chunks) {
            if (c.name == "foo") { foo = &c; break; }
        }
        assert(foo != nullptr);
        assert(foo->symbol_id == 10);
        assert(foo->file_path == "example.cpp");
        assert(foo->line_start == 2);
        assert(foo->line_end == 5);
        assert(foo->text.find("// example.cpp\n// function: foo()\n\n") == 0);
        // Body should contain the function source lines 2-5
        assert(foo->text.find("void foo()") != std::string::npos);
        assert(foo->text.find("return;") != std::string::npos);
        std::cout << "PASS: foo chunk has correct header and body\n";
    }

    // Test 5: extract_chunks with file_id filter (CHK-01)
    {
        auto chunks = codetldr::extract_chunks(db.raw(), test_dir, 1);
        assert(chunks.size() == 2);  // same result -- only file_id=1 exists
        std::cout << "PASS: extract_chunks with file_id filter works\n";
    }

    // Test 6: extract_chunks on non-existent file_id returns empty
    {
        auto chunks = codetldr::extract_chunks(db.raw(), test_dir, 999);
        assert(chunks.empty());
        std::cout << "PASS: extract_chunks with invalid file_id returns empty\n";
    }

    // Cleanup
    fs::remove_all(test_dir);

    std::cout << "\nAll chunk extractor tests passed.\n";
    return 0;
}
