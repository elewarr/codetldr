#pragma once
#include <tree_sitter/api.h>
#include <memory>
#include <string>
#include <string_view>

namespace codetldr {

struct TsTreeDeleter   { void operator()(TSTree* t)         { if (t) ts_tree_delete(t); } };
struct TsParserDeleter { void operator()(TSParser* p)       { if (p) ts_parser_delete(p); } };
struct TsQueryDeleter  { void operator()(TSQuery* q)        { if (q) ts_query_delete(q); } };
struct TsCursorDeleter { void operator()(TSQueryCursor* c)  { if (c) ts_query_cursor_delete(c); } };

using TsTreePtr   = std::unique_ptr<TSTree,         TsTreeDeleter>;
using TsParserPtr = std::unique_ptr<TSParser,       TsParserDeleter>;
using TsQueryPtr  = std::unique_ptr<TSQuery,        TsQueryDeleter>;
using TsCursorPtr = std::unique_ptr<TSQueryCursor,  TsCursorDeleter>;

// Parse source code with a given TSLanguage. Returns non-null tree on success,
// or nullptr if set_language fails (ABI mismatch).
TsTreePtr parse_source(const TSLanguage* lang, std::string_view source);

} // namespace codetldr
