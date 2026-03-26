#pragma once
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <cstdint>

namespace codetldr {

struct Symbol {
    std::string name;
    std::string kind;       // "function", "method", "class", "struct", "enum", "interface", "import", "export", "constant"
    std::string signature;  // parameter list if extractable, empty otherwise
    int line_start;         // 1-indexed
    int line_end;           // 1-indexed
    std::string documentation;  // doc comment content, max 1024 chars
    uint32_t start_byte;   // for caller identification
    uint32_t end_byte;     // for caller identification
};

struct CallEdge {
    std::string caller_name;   // enclosing function/method name, empty if top-level
    std::string callee_name;   // called function/method name
    int line;                  // 1-indexed line of call site
};

struct CfgNode {
    std::string node_type;   // "if_branch", "else_branch", "loop",
                             // "early_return", "switch_case", "try_catch"
    std::string condition;   // condition text, max 256 chars, empty if n/a
    int line;                // 1-indexed
    int depth;               // 0 = function body, 1 = first nesting
    std::string symbol_name; // enclosing function name (for persistence lookup)
};

struct DfgEdge {
    std::string edge_type;   // "assignment", "parameter", "return_value"
    std::string lhs;         // defined variable name
    std::string rhs_snippet; // truncated RHS text (128 chars); empty for "parameter" edges
    int line;                // 1-indexed
    std::string symbol_name; // enclosing function name (for persistence lookup)
};

struct ExtractionResult {
    std::vector<Symbol> symbols;
    std::vector<CallEdge> calls;
};

// Extract symbols from a parsed tree using the language's symbol query.
// source is the original source code (needed for text extraction from byte offsets).
std::vector<Symbol> extract_symbols(
    const TSTree* tree,
    const TSQuery* symbol_query,
    const std::string& source);

// Extract call edges from a parsed tree using the language's call query.
// symbols is the previously extracted symbol list (used to determine caller context).
std::vector<CallEdge> extract_calls(
    const TSTree* tree,
    const TSQuery* call_query,
    const std::string& source,
    const std::vector<Symbol>& symbols);

// Extract CFG nodes (branches, loops, returns) from a parsed tree.
// symbols is the previously extracted symbol list (used for enclosing function lookup).
std::vector<CfgNode> extract_cfg_nodes(
    const TSTree* tree,
    const TSQuery* cfg_query,
    const std::string& source,
    const std::vector<Symbol>& symbols);

// Extract DFG edges (assignments, parameters, return values) from a parsed tree.
// symbols is the previously extracted symbol list (used for enclosing function lookup).
std::vector<DfgEdge> extract_dfg_edges(
    const TSTree* tree,
    const TSQuery* dfg_query,
    const std::string& source,
    const std::vector<Symbol>& symbols);

} // namespace codetldr
