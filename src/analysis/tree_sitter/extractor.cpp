// extractor.cpp -- Symbol and call extraction from Tree-sitter ASTs.

#include "analysis/tree_sitter/extractor.h"
#include <tree_sitter/api.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace codetldr {

// ---------------------------------------------------------------------------
// Helper: extract text from source bytes using node byte offsets
// ---------------------------------------------------------------------------
static std::string node_text(const TSNode& node, const std::string& source) {
    uint32_t start = ts_node_start_byte(node);
    uint32_t end   = ts_node_end_byte(node);
    if (start >= end || end > source.size()) return {};
    return source.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// Helper: extract documentation preceding a definition node.
//
// Strategy:
// 1. Walk backwards from the node's start_byte, skipping blank lines.
// 2. Check for line comment chains (///, //, #) or block comment closing (*/
// 3. For Python: also check if the FIRST child of the function/class body is a
//    string literal (docstring).
// ---------------------------------------------------------------------------
static std::string extract_preceding_doc(const TSNode& def_node,
                                          const std::string& source) {
    uint32_t start = ts_node_start_byte(def_node);
    if (start == 0) return {};

    // Scan backwards from byte before start_byte
    int pos = static_cast<int>(start) - 1;

    // Skip trailing whitespace / newlines at the end of preceding lines
    while (pos >= 0 && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\n' || source[pos] == '\r')) {
        --pos;
    }
    if (pos < 0) return {};

    std::string doc;

    // Check for C-style block comment ending: */
    if (pos >= 1 && source[pos] == '/' && source[pos-1] == '*') {
        int end_pos = pos - 1;  // points at the *
        // Find the matching /*
        int search = end_pos - 1;
        while (search >= 1) {
            if (source[search] == '*' && source[search-1] == '/') {
                // Found /*
                doc = source.substr(search - 1, end_pos - (search - 1) + 2);
                break;
            }
            --search;
        }
    } else {
        // Check for line comment chains (///, //, #)
        // Walk backwards line by line
        std::vector<std::string> lines;

        while (pos >= 0) {
            // Find start of this line
            int line_end = pos;
            // Skip backwards to newline or start
            while (pos >= 0 && source[pos] != '\n') --pos;
            int line_start = pos + 1;

            // Extract the line content
            std::string line = source.substr(line_start, line_end - line_start + 1);

            // Trim leading whitespace
            size_t trim = 0;
            while (trim < line.size() && (line[trim] == ' ' || line[trim] == '\t')) ++trim;
            std::string trimmed = line.substr(trim);

            bool is_comment = false;
            if (trimmed.size() >= 3 && trimmed.substr(0, 3) == "///") {
                is_comment = true;
            } else if (trimmed.size() >= 2 && trimmed.substr(0, 2) == "//") {
                is_comment = true;
            } else if (!trimmed.empty() && trimmed[0] == '#') {
                // Python comment -- but we prefer docstrings; still capture
                is_comment = true;
            }

            if (!is_comment) break;
            lines.push_back(trimmed);
            --pos;  // move past the newline
        }

        // lines are in reverse order; reverse them
        std::reverse(lines.begin(), lines.end());
        for (const auto& l : lines) {
            if (!doc.empty()) doc += '\n';
            doc += l;
        }
    }

    if (doc.size() > 1024) doc.resize(1024);
    return doc;
}

// ---------------------------------------------------------------------------
// Helper: find Python docstring as first statement of function/class body.
// The docstring is the first expression_statement whose child is a string node.
// ---------------------------------------------------------------------------
// Extract string content from a Tree-sitter string node (strips surrounding quotes).
static std::string extract_string_content(const TSNode& str_node, const std::string& source) {
    // First try to get the string_content child node (tree-sitter 0.20+)
    uint32_t count = ts_node_child_count(str_node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(str_node, i);
        const char* t = ts_node_type(child);
        if (std::strcmp(t, "string_content") == 0) {
            return node_text(child, source);
        }
    }
    // Fallback: strip quotes from full text
    std::string text = node_text(str_node, source);
    if (text.size() >= 6 && (text.substr(0, 3) == "\"\"\"" || text.substr(0, 3) == "'''")) {
        text = text.substr(3, text.size() - 6);
    } else if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'')) {
        text = text.substr(1, text.size() - 2);
    }
    // Trim surrounding whitespace/newlines
    size_t s = 0, e = text.size();
    while (s < e && (text[s] == ' ' || text[s] == '\n' || text[s] == '\r' || text[s] == '\t')) ++s;
    while (e > s && (text[e-1] == ' ' || text[e-1] == '\n' || text[e-1] == '\r' || text[e-1] == '\t')) --e;
    return text.substr(s, e - s);
}

static std::string extract_python_docstring(const TSNode& def_node,
                                             const std::string& source) {
    // Navigate: function_definition/class_definition -> block -> first child
    // In newer tree-sitter-python (0.20+), the docstring may be:
    //   1. A `string` node directly in `block` (first child)
    //   2. An `expression_statement` whose first child is `string`
    uint32_t child_count = ts_node_child_count(def_node);
    for (uint32_t i = 0; i < child_count; ++i) {
        TSNode child = ts_node_child(def_node, i);
        const char* child_type = ts_node_type(child);
        if (std::strcmp(child_type, "block") == 0) {
            // Found the body block
            uint32_t block_count = ts_node_child_count(child);
            for (uint32_t j = 0; j < block_count; ++j) {
                TSNode stmt = ts_node_child(child, j);
                const char* stmt_type = ts_node_type(stmt);

                // Case 1: direct string node in block (tree-sitter-python 0.23+)
                if (std::strcmp(stmt_type, "string") == 0) {
                    std::string content = extract_string_content(stmt, source);
                    if (content.size() > 1024) content.resize(1024);
                    return content;
                }

                // Case 2: expression_statement wrapping a string
                if (std::strcmp(stmt_type, "expression_statement") == 0) {
                    uint32_t expr_count = ts_node_child_count(stmt);
                    for (uint32_t k = 0; k < expr_count; ++k) {
                        TSNode expr = ts_node_child(stmt, k);
                        if (std::strcmp(ts_node_type(expr), "string") == 0) {
                            std::string content = extract_string_content(expr, source);
                            if (content.size() > 1024) content.resize(1024);
                            return content;
                        }
                    }
                    break;  // Only check first statement
                }

                // Skip comment/newline tokens to find the first real statement
                if (std::strcmp(stmt_type, "comment") != 0) {
                    break;  // First non-comment statement is not a docstring
                }
            }
            break;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Helper: extract parameter signature from function definition node.
// Finds the first "parameters" or "parameter_list" child and returns its text.
// ---------------------------------------------------------------------------
static std::string extract_signature(const TSNode& def_node, const std::string& source) {
    // Search for a child named "parameters", "parameter_list", "formal_parameters", etc.
    uint32_t count = ts_node_child_count(def_node);
    for (uint32_t i = 0; i < count; ++i) {
        TSNode child = ts_node_child(def_node, i);
        const char* t = ts_node_type(child);
        if (std::strcmp(t, "parameters") == 0 ||
            std::strcmp(t, "parameter_list") == 0 ||
            std::strcmp(t, "formal_parameters") == 0 ||
            std::strcmp(t, "function_value_parameters") == 0 ||
            std::strcmp(t, "params") == 0) {
            return node_text(child, source);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// extract_symbols
// ---------------------------------------------------------------------------
std::vector<Symbol> extract_symbols(const TSTree* tree,
                                     const TSQuery* symbol_query,
                                     const std::string& source) {
    std::vector<Symbol> results;
    if (!tree || !symbol_query) return results;

    TSNode root = ts_tree_root_node(const_cast<TSTree*>(tree));

    // Use a non-const TSQuery pointer for cursor exec (API requires it)
    TSQuery* q = const_cast<TSQuery*>(symbol_query);
    uint32_t cap_count = ts_query_capture_count(q);

    // Build capture name -> index mapping
    // We use ts_query_capture_name_for_id to get each capture name
    std::unordered_map<uint32_t, std::string> cap_names;
    for (uint32_t i = 0; i < cap_count; ++i) {
        uint32_t len = 0;
        const char* name = ts_query_capture_name_for_id(q, i, &len);
        if (name) cap_names[i] = std::string(name, len);
    }

    // Execute query
    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, q, root);

    // Track which definition nodes we've already processed (to avoid duplicates)
    // when the same node can be matched by multiple patterns
    struct PendingSymbol {
        TSNode def_node;
        std::string kind;
        std::string name;
        bool is_python;
    };
    std::vector<PendingSymbol> pending;

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        std::string def_kind;
        TSNode def_node{};
        bool has_def = false;
        std::string sym_name;
        bool has_name = false;

        for (uint32_t i = 0; i < match.capture_count; ++i) {
            uint32_t cap_idx = match.captures[i].index;
            TSNode cap_node = match.captures[i].node;

            auto it = cap_names.find(cap_idx);
            if (it == cap_names.end()) continue;

            const std::string& cap_name = it->second;

            if (cap_name.substr(0, 11) == "definition.") {
                def_kind = cap_name.substr(11);  // e.g., "function", "class", etc.
                def_node = cap_node;
                has_def = true;
            } else if (cap_name == "name") {
                sym_name = node_text(cap_node, source);
                has_name = true;
            }
        }

        if (has_def && has_name && !sym_name.empty() && !def_kind.empty()) {
            // Check if it's a Python-style function/class (uses docstring extraction)
            const char* node_type = ts_node_type(def_node);
            bool is_python_func_or_class =
                std::strcmp(node_type, "function_definition") == 0 ||
                std::strcmp(node_type, "class_definition") == 0;

            // Promote "function" -> "method" when the node is inside a class body.
            // Check grandparent: function_definition -> block/class_body -> class_definition/class_declaration/class_specifier
            if (def_kind == "function") {
                TSNode parent = ts_node_parent(def_node);
                if (!ts_node_is_null(parent)) {
                    const char* parent_type = ts_node_type(parent);
                    // Python: function inside "block" inside "class_definition"
                    // Java/JS/TS: function inside "class_body"
                    // Kotlin: function inside "class_body"
                    // Swift: function inside "class_body"
                    bool in_body = std::strcmp(parent_type, "block") == 0 ||
                                   std::strcmp(parent_type, "class_body") == 0;
                    if (in_body) {
                        TSNode grandparent = ts_node_parent(parent);
                        if (!ts_node_is_null(grandparent)) {
                            const char* gp_type = ts_node_type(grandparent);
                            if (std::strcmp(gp_type, "class_definition") == 0 ||
                                std::strcmp(gp_type, "class_declaration") == 0 ||
                                std::strcmp(gp_type, "class_specifier") == 0 ||
                                std::strcmp(gp_type, "impl_item") == 0) {
                                def_kind = "method";
                            }
                        }
                    }
                }
            }

            // Check for duplicate (same start_byte)
            uint32_t start_b = ts_node_start_byte(def_node);
            bool dup = false;
            for (const auto& p : pending) {
                if (ts_node_start_byte(p.def_node) == start_b && p.name == sym_name) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                pending.push_back({def_node, def_kind, sym_name, is_python_func_or_class});
            }
        }
    }

    ts_query_cursor_delete(cursor);

    // Convert pending to Symbol structs
    for (const auto& p : pending) {
        Symbol sym;
        sym.name       = p.name;
        sym.kind       = p.kind;
        sym.line_start = static_cast<int>(ts_node_start_point(p.def_node).row) + 1;
        sym.line_end   = static_cast<int>(ts_node_end_point(p.def_node).row) + 1;
        sym.start_byte = ts_node_start_byte(p.def_node);
        sym.end_byte   = ts_node_end_byte(p.def_node);

        // Extract signature
        sym.signature = extract_signature(p.def_node, source);

        // Extract documentation
        // For Python: prefer docstring inside the body
        if (p.is_python) {
            sym.documentation = extract_python_docstring(p.def_node, source);
        }
        // If no docstring found, try preceding comments
        if (sym.documentation.empty()) {
            sym.documentation = extract_preceding_doc(p.def_node, source);
        }

        results.push_back(std::move(sym));
    }

    // Sort by line_start ascending
    std::sort(results.begin(), results.end(), [](const Symbol& a, const Symbol& b) {
        return a.line_start < b.line_start;
    });

    return results;
}

// ---------------------------------------------------------------------------
// extract_calls
// ---------------------------------------------------------------------------
std::vector<CallEdge> extract_calls(const TSTree* tree,
                                     const TSQuery* call_query,
                                     const std::string& source,
                                     const std::vector<Symbol>& symbols) {
    std::vector<CallEdge> results;
    if (!tree || !call_query) return results;

    TSNode root = ts_tree_root_node(const_cast<TSTree*>(tree));
    TSQuery* q  = const_cast<TSQuery*>(call_query);

    uint32_t cap_count = ts_query_capture_count(q);
    std::unordered_map<uint32_t, std::string> cap_names;
    for (uint32_t i = 0; i < cap_count; ++i) {
        uint32_t len = 0;
        const char* name = ts_query_capture_name_for_id(q, i, &len);
        if (name) cap_names[i] = std::string(name, len);
    }

    // Build sorted index of symbols by start_byte for binary search
    // (symbols is already sorted by line_start, which correlates with start_byte)
    // We'll do a linear scan since fixture files are small
    auto find_caller = [&](uint32_t byte_offset) -> std::string {
        for (const auto& sym : symbols) {
            if (sym.start_byte <= byte_offset && byte_offset < sym.end_byte) {
                return sym.name;
            }
        }
        return {};
    };

    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_exec(cursor, q, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
        for (uint32_t i = 0; i < match.capture_count; ++i) {
            uint32_t cap_idx  = match.captures[i].index;
            TSNode   cap_node = match.captures[i].node;

            auto it = cap_names.find(cap_idx);
            if (it == cap_names.end()) continue;

            // We want the "name" capture (the identifier of the called function)
            if (it->second != "name") continue;

            std::string callee_name = node_text(cap_node, source);
            if (callee_name.empty()) continue;

            int line = static_cast<int>(ts_node_start_point(cap_node).row) + 1;
            uint32_t call_byte = ts_node_start_byte(cap_node);
            std::string caller_name = find_caller(call_byte);

            // Skip if callee_name equals the caller_name (recursive self-reference is fine, but
            // skip empty callee names)
            CallEdge edge;
            edge.caller_name = caller_name;
            edge.callee_name = callee_name;
            edge.line        = line;
            results.push_back(std::move(edge));
        }
    }

    ts_query_cursor_delete(cursor);
    return results;
}

} // namespace codetldr
