#include "analysis/tree_sitter/language_registry.h"
#include "analysis/tree_sitter/queries.h"
#include <spdlog/spdlog.h>
#include <tree_sitter/api.h>

// Grammar entry points
extern "C" {
const TSLanguage* tree_sitter_python(void);
const TSLanguage* tree_sitter_javascript(void);
const TSLanguage* tree_sitter_typescript(void);
const TSLanguage* tree_sitter_tsx(void);
const TSLanguage* tree_sitter_rust(void);
const TSLanguage* tree_sitter_c(void);
const TSLanguage* tree_sitter_cpp(void);
const TSLanguage* tree_sitter_java(void);
const TSLanguage* tree_sitter_kotlin(void);
const TSLanguage* tree_sitter_swift(void);
const TSLanguage* tree_sitter_objc(void);
}

namespace codetldr {

bool LanguageRegistry::initialize() {
    bool ok = true;

    auto q = queries::python();
    ok &= register_language("python", tree_sitter_python(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::javascript();
    ok &= register_language("javascript", tree_sitter_javascript(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::typescript();
    ok &= register_language("typescript", tree_sitter_typescript(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::tsx();
    ok &= register_language("tsx", tree_sitter_tsx(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::rust();
    ok &= register_language("rust", tree_sitter_rust(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::c();
    ok &= register_language("c", tree_sitter_c(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::cpp();
    ok &= register_language("cpp", tree_sitter_cpp(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::java();
    ok &= register_language("java", tree_sitter_java(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::kotlin();
    ok &= register_language("kotlin", tree_sitter_kotlin(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::swift();
    ok &= register_language("swift", tree_sitter_swift(), q.symbols, q.calls, q.cfg, q.dfg);

    q = queries::objc();
    ok &= register_language("objc", tree_sitter_objc(), q.symbols, q.calls, q.cfg, q.dfg);

    // Extension -> language mappings
    ext_to_lang_[".py"]  = "python";
    ext_to_lang_[".pyi"] = "python";

    ext_to_lang_[".js"]  = "javascript";
    ext_to_lang_[".mjs"] = "javascript";
    ext_to_lang_[".cjs"] = "javascript";

    ext_to_lang_[".ts"]  = "typescript";
    ext_to_lang_[".tsx"] = "tsx";

    ext_to_lang_[".rs"]  = "rust";

    ext_to_lang_[".c"]   = "c";
    ext_to_lang_[".h"]   = "c";  // Default .h to C; heuristic for C++ in future

    ext_to_lang_[".cpp"] = "cpp";
    ext_to_lang_[".cc"]  = "cpp";
    ext_to_lang_[".cxx"] = "cpp";
    ext_to_lang_[".hpp"] = "cpp";
    ext_to_lang_[".hxx"] = "cpp";

    ext_to_lang_[".java"] = "java";

    ext_to_lang_[".kt"]  = "kotlin";
    ext_to_lang_[".kts"] = "kotlin";

    ext_to_lang_[".swift"] = "swift";

    ext_to_lang_[".m"]  = "objc";
    ext_to_lang_[".mm"] = "objc";

    return ok;
}

bool LanguageRegistry::register_language(const std::string& name,
                                          const TSLanguage* lang,
                                          const char* symbol_query_str,
                                          const char* call_query_str,
                                          const char* cfg_query_str,
                                          const char* dfg_query_str) {
    if (!lang) {
        SPDLOG_ERROR("Language '{}': null TSLanguage pointer", name);
        return false;
    }

    // Compile symbol query
    uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;

    TSQuery* sym_q = ts_query_new(lang, symbol_query_str,
                                  static_cast<uint32_t>(strlen(symbol_query_str)),
                                  &error_offset, &error_type);
    if (!sym_q || error_type != TSQueryErrorNone) {
        SPDLOG_ERROR("Language '{}': symbol query failed at offset {}, error type {}",
                     name, error_offset, static_cast<int>(error_type));
        if (sym_q) ts_query_delete(sym_q);
        return false;
    }

    // Compile call query
    TSQuery* call_q = ts_query_new(lang, call_query_str,
                                   static_cast<uint32_t>(strlen(call_query_str)),
                                   &error_offset, &error_type);
    if (!call_q || error_type != TSQueryErrorNone) {
        SPDLOG_ERROR("Language '{}': call query failed at offset {}, error type {}",
                     name, error_offset, static_cast<int>(error_type));
        ts_query_delete(sym_q);
        if (call_q) ts_query_delete(call_q);
        return false;
    }

    // Compile cfg query (optional -- nullptr means no CFG for this language)
    TSQuery* cfg_q = nullptr;
    if (cfg_query_str && cfg_query_str[0] != '\0') {
        cfg_q = ts_query_new(lang, cfg_query_str,
                             static_cast<uint32_t>(strlen(cfg_query_str)),
                             &error_offset, &error_type);
        if (!cfg_q || error_type != TSQueryErrorNone) {
            SPDLOG_ERROR("Language '{}': cfg query failed at offset {}, error type {}",
                         name, error_offset, static_cast<int>(error_type));
            ts_query_delete(sym_q);
            ts_query_delete(call_q);
            if (cfg_q) ts_query_delete(cfg_q);
            return false;
        }
    }

    // Compile dfg query (optional -- nullptr means no DFG for this language)
    TSQuery* dfg_q = nullptr;
    if (dfg_query_str && dfg_query_str[0] != '\0') {
        dfg_q = ts_query_new(lang, dfg_query_str,
                             static_cast<uint32_t>(strlen(dfg_query_str)),
                             &error_offset, &error_type);
        if (!dfg_q || error_type != TSQueryErrorNone) {
            SPDLOG_ERROR("Language '{}': dfg query failed at offset {}, error type {}",
                         name, error_offset, static_cast<int>(error_type));
            ts_query_delete(sym_q);
            ts_query_delete(call_q);
            if (cfg_q) ts_query_delete(cfg_q);
            if (dfg_q) ts_query_delete(dfg_q);
            return false;
        }
    }

    LanguageEntry entry;
    entry.name = name;
    entry.language = lang;
    entry.symbol_query = TsQueryPtr(sym_q);
    entry.call_query   = TsQueryPtr(call_q);
    entry.cfg_query    = TsQueryPtr(cfg_q);
    entry.dfg_query    = TsQueryPtr(dfg_q);

    entries_[name] = std::move(entry);
    return true;
}

const LanguageEntry* LanguageRegistry::for_language(const std::string& name) const {
    auto it = entries_.find(name);
    return it != entries_.end() ? &it->second : nullptr;
}

const LanguageEntry* LanguageRegistry::for_extension(const std::string& ext) const {
    auto it = ext_to_lang_.find(ext);
    if (it == ext_to_lang_.end()) return nullptr;

    auto eit = entries_.find(it->second);
    if (eit == entries_.end()) return nullptr;

    return &eit->second;
}

std::vector<std::string> LanguageRegistry::language_names() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const auto& [name, _] : entries_) {
        names.push_back(name);
    }
    return names;
}

} // namespace codetldr
