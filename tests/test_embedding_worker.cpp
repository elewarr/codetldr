// test_embedding_worker.cpp -- Unit tests for EmbeddingWorker class (Phase 17 Plan 01)
//
// Tests cover:
//   1. nullptr model: no crash, thread not started, enqueue is no-op
//   2. stop on empty queue: clean exit, no deadlock
//   3. Schema migration v7: metadata table exists with key TEXT PK
//   4. Schema migration v8: embedded_files table exists with (file_id, symbol_id) PK
//   5. AnalysisResult has int64_t file_id field (compile test)
//   6. compute_model_fingerprint: returns size:mtime for real file, "" for missing

#include "embedding/embedding_worker.h"
#include "analysis/pipeline.h"    // AnalysisResult -- compile test
#include "storage/schema.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

void run_migrations(SQLite::Database& db) {
    for (const auto& [version, sql] : codetldr::kMigrations) {
        db.exec(std::string(sql));
    }
    db.exec("PRAGMA user_version = 8");
}

// Test 1: nullptr model — no crash, no thread, enqueue is no-op
void test_nullptr_model_no_crash() {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    run_migrations(db);
    std::filesystem::path root = "/tmp/test_project";

    codetldr::EmbeddingWorker worker(db, root, nullptr, nullptr, "");
    worker.enqueue(1);
    worker.enqueue(2);
    worker.enqueue_full_rebuild();
    worker.stop();
    std::cout << "PASS: nullptr model no crash\n";
}

// Test 2: stop on empty queue — clean exit, no deadlock
void test_stop_empty_queue() {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    run_migrations(db);

    codetldr::EmbeddingWorker worker(db, "/tmp", nullptr, nullptr, "");
    worker.stop();
    std::cout << "PASS: stop empty queue\n";
}

// Test 3: double stop is safe
void test_double_stop() {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    run_migrations(db);

    codetldr::EmbeddingWorker worker(db, "/tmp", nullptr, nullptr, "");
    worker.stop();
    worker.stop();  // safe to call multiple times
    std::cout << "PASS: double stop safe\n";
}

// Test 4: schema migration v7 — metadata table exists
void test_schema_migration_v7_metadata() {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    run_migrations(db);

    // Verify table exists and is queryable
    SQLite::Statement q(db, "SELECT COUNT(*) FROM metadata");
    assert(q.executeStep());
    assert(q.getColumn(0).getInt() == 0);

    // Insert and read back
    db.exec("INSERT INTO metadata(key, value) VALUES('test_key', 'test_val')");
    SQLite::Statement r(db, "SELECT value FROM metadata WHERE key = 'test_key'");
    assert(r.executeStep());
    assert(r.getColumn(0).getString() == "test_val");

    // Verify PRIMARY KEY uniqueness
    bool threw = false;
    try {
        db.exec("INSERT INTO metadata(key, value) VALUES('test_key', 'duplicate')");
    } catch (const SQLite::Exception&) {
        threw = true;
    }
    assert(threw && "metadata key must be PRIMARY KEY (unique)");

    std::cout << "PASS: schema migration v7 metadata\n";
}

// Test 5: schema migration v8 — embedded_files table exists
void test_schema_migration_v8_embedded_files() {
    SQLite::Database db(":memory:", SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
    run_migrations(db);

    // Insert a file first (for FK constraint)
    db.exec("INSERT INTO files(path, mtime_ns) VALUES('test.cpp', 0)");
    int64_t file_id = db.getLastInsertRowid();

    // Insert embedded_files row
    {
        SQLite::Statement ins(db, "INSERT INTO embedded_files(file_id, symbol_id) VALUES(?, ?)");
        ins.bind(1, file_id);
        ins.bind(2, static_cast<long long>(42));
        ins.exec();
    }

    // Verify row exists
    {
        SQLite::Statement q(db, "SELECT symbol_id FROM embedded_files WHERE file_id = ?");
        q.bind(1, file_id);
        assert(q.executeStep());
        assert(q.getColumn(0).getInt64() == 42);
    }

    // Verify PK uniqueness: duplicate (file_id, symbol_id) should throw
    bool threw = false;
    try {
        SQLite::Statement ins(db, "INSERT INTO embedded_files(file_id, symbol_id) VALUES(?, ?)");
        ins.bind(1, file_id);
        ins.bind(2, static_cast<long long>(42));
        ins.exec();
    } catch (const SQLite::Exception&) {
        threw = true;
    }
    assert(threw && "embedded_files (file_id, symbol_id) must be PRIMARY KEY (unique)");

    std::cout << "PASS: schema migration v8 embedded_files\n";
}

// Test 6: AnalysisResult has int64_t file_id (compile test)
void test_analysis_result_file_id() {
    codetldr::AnalysisResult r{};
    r.file_id = 42;
    assert(r.file_id == 42);
    r.file_id = 0;
    assert(r.file_id == 0);
    // Verify the field is int64_t (can hold large values)
    r.file_id = INT64_MAX;
    assert(r.file_id == INT64_MAX);
    std::cout << "PASS: AnalysisResult has int64_t file_id\n";
}

// Test 7: compute_model_fingerprint returns "size:mtime" for real file, "" for missing
void test_model_fingerprint() {
    // Create a temp file
    auto tmp = std::filesystem::temp_directory_path() / "test_model_17_01.onnx";
    {
        std::ofstream f(tmp);
        f << "fake model content for fingerprint test";
    }

    auto fp = codetldr::EmbeddingWorker::compute_model_fingerprint(tmp);
    assert(!fp.empty() && "fingerprint must be non-empty for existing file");
    assert(fp.find(':') != std::string::npos && "fingerprint format must be 'size:mtime'");

    // Verify stable: calling again returns same value
    auto fp2 = codetldr::EmbeddingWorker::compute_model_fingerprint(tmp);
    assert(fp == fp2 && "fingerprint must be deterministic");

    std::filesystem::remove(tmp);

    // Non-existent file returns empty string
    auto fp3 = codetldr::EmbeddingWorker::compute_model_fingerprint("/nonexistent/path_no_exist.onnx");
    assert(fp3.empty() && "fingerprint for missing file must be empty");

    std::cout << "PASS: model fingerprint\n";
}

// Test 8: kFullRebuildSentinel is -1
void test_full_rebuild_sentinel_value() {
    assert(codetldr::EmbeddingWorker::kFullRebuildSentinel == -1);
    std::cout << "PASS: kFullRebuildSentinel == -1\n";
}

} // namespace

int main() {
    std::cout << "=== test_embedding_worker ===\n";

    test_nullptr_model_no_crash();
    test_stop_empty_queue();
    test_double_stop();
    test_schema_migration_v7_metadata();
    test_schema_migration_v8_embedded_files();
    test_analysis_result_file_id();
    test_model_fingerprint();
    test_full_rebuild_sentinel_value();

    std::cout << "All embedding_worker tests passed\n";
    return 0;
}
