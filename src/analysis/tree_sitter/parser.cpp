#include "analysis/tree_sitter/parser.h"
#include <spdlog/spdlog.h>
#include <cassert>

namespace codetldr {

TsTreePtr parse_source(const TSLanguage* lang, std::string_view source) {
    TsParserPtr parser(ts_parser_new());
    if (!parser) {
        SPDLOG_ERROR("ts_parser_new() returned null");
        return nullptr;
    }

    if (!ts_parser_set_language(parser.get(), lang)) {
        SPDLOG_ERROR("ts_parser_set_language failed -- ABI version mismatch");
        return nullptr;
    }

    TSTree* tree = ts_parser_parse_string(
        parser.get(),
        nullptr,
        source.data(),
        static_cast<uint32_t>(source.size())
    );

    return TsTreePtr(tree);
}

} // namespace codetldr
