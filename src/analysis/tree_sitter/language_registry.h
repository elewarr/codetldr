#pragma once
#include "analysis/tree_sitter/parser.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace codetldr {

struct LanguageEntry {
    std::string name;           // "python", "javascript", etc.
    const TSLanguage* language; // TSLanguage pointer from grammar
    TsQueryPtr symbol_query;    // Compiled symbol query
    TsQueryPtr call_query;      // Compiled call query
    TsQueryPtr cfg_query;       // Compiled CFG query (nullptr for non-CFG languages)
    TsQueryPtr dfg_query;       // Compiled DFG query (nullptr for non-DFG languages)
};

class LanguageRegistry {
public:
    // Initialize all 10 languages. Returns false if any grammar fails ABI check.
    bool initialize();

    // Lookup by file extension (including dot). Returns nullptr if unknown.
    const LanguageEntry* for_extension(const std::string& ext) const;

    // Lookup by language name. Returns nullptr if unknown.
    const LanguageEntry* for_name(const std::string& name) const;

    // Get all registered language names
    std::vector<std::string> language_names() const;

private:
    bool register_language(const std::string& name,
                           const TSLanguage* lang,
                           const char* symbol_query_str,
                           const char* call_query_str,
                           const char* cfg_query_str,
                           const char* dfg_query_str);

    std::unordered_map<std::string, LanguageEntry> entries_;    // name -> entry
    std::unordered_map<std::string, std::string> ext_to_lang_;  // ".py" -> "python"
};

} // namespace codetldr
