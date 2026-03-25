#pragma once
#include <SQLiteCpp/SQLiteCpp.h>

namespace codetldr {
// Run all pending migrations. Creates schema_version table if not exists.
// Throws std::runtime_error on migration failure.
void run_migrations(SQLite::Database& db);
} // namespace codetldr
