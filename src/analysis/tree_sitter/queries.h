#pragma once

namespace codetldr::queries {

struct LanguageQueries {
    const char* symbols;  // Query string for symbol definitions
    const char* calls;    // Query string for call sites
    const char* cfg;      // CFG query string (nullptr for languages without CFG support)
};

inline LanguageQueries python() {
    return {
        // symbols
        "(function_definition name: (identifier) @name) @definition.function\n"
        "(class_definition name: (identifier) @name) @definition.class\n",
        // calls
        "(call function: (identifier) @name) @reference.call\n"
        "(call function: (attribute attribute: (identifier) @name)) @reference.call\n",
        // cfg
        "(if_statement) @cfg.branch\n"
        "(elif_clause) @cfg.branch\n"
        "(else_clause) @cfg.branch\n"
        "(while_statement) @cfg.loop\n"
        "(for_statement) @cfg.loop\n"
        "(return_statement) @cfg.return\n"
        "(try_statement) @cfg.branch\n"
        "(except_clause) @cfg.branch\n"
    };
}

inline LanguageQueries javascript() {
    return {
        // symbols
        "(function_declaration name: (identifier) @name) @definition.function\n"
        "(method_definition name: (property_identifier) @name) @definition.method\n"
        "(class_declaration name: (identifier) @name) @definition.class\n",
        // calls
        "(call_expression function: (identifier) @name) @reference.call\n"
        "(call_expression function: (member_expression property: (property_identifier) @name)) @reference.call\n",
        // cfg
        "(if_statement) @cfg.branch\n"
        "(else_clause) @cfg.branch\n"
        "(while_statement) @cfg.loop\n"
        "(for_statement) @cfg.loop\n"
        "(for_in_statement) @cfg.loop\n"
        "(do_statement) @cfg.loop\n"
        "(switch_case) @cfg.branch\n"
        "(return_statement) @cfg.return\n"
        "(try_statement) @cfg.branch\n"
        "(catch_clause) @cfg.branch\n"
    };
}

inline LanguageQueries typescript() {
    return {
        // symbols -- class_declaration uses type_identifier (not identifier) for name
        "(function_declaration name: (identifier) @name) @definition.function\n"
        "(method_definition name: (property_identifier) @name) @definition.method\n"
        "(class_declaration name: (type_identifier) @name) @definition.class\n"
        "(interface_declaration name: (type_identifier) @name) @definition.interface\n",
        // calls
        "(call_expression function: (identifier) @name) @reference.call\n"
        "(call_expression function: (member_expression property: (property_identifier) @name)) @reference.call\n",
        // cfg -- no CFG support for TypeScript in this plan
        nullptr
    };
}

inline LanguageQueries tsx() {
    // TSX is TypeScript with JSX -- same queries as TypeScript (including no CFG)
    return typescript();
}

inline LanguageQueries rust() {
    return {
        // symbols
        "(function_item name: (identifier) @name) @definition.function\n"
        "(impl_item type: (type_identifier) @name) @definition.class\n"
        "(struct_item name: (type_identifier) @name) @definition.class\n"
        "(enum_item name: (type_identifier) @name) @definition.class\n"
        "(trait_item name: (type_identifier) @name) @definition.interface\n",
        // calls
        "(call_expression function: (identifier) @name) @reference.call\n"
        "(call_expression function: (scoped_identifier name: (identifier) @name)) @reference.call\n",
        // cfg -- no CFG support for Rust in this plan
        nullptr
    };
}

inline LanguageQueries c() {
    return {
        // symbols
        "(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function\n"
        "(struct_specifier name: (type_identifier) @name) @definition.class\n"
        "(enum_specifier name: (type_identifier) @name) @definition.class\n",
        // calls
        "(call_expression function: (identifier) @name) @reference.call\n",
        // cfg
        "(if_statement) @cfg.branch\n"
        "(else_clause) @cfg.branch\n"
        "(while_statement) @cfg.loop\n"
        "(for_statement) @cfg.loop\n"
        "(do_statement) @cfg.loop\n"
        "(case_statement) @cfg.branch\n"
        "(return_statement) @cfg.return\n"
    };
}

inline LanguageQueries cpp() {
    return {
        // symbols
        "(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function\n"
        "(function_definition declarator: (function_declarator declarator: (qualified_identifier name: (identifier) @name))) @definition.function\n"
        "(class_specifier name: (type_identifier) @name) @definition.class\n"
        "(struct_specifier name: (type_identifier) @name) @definition.class\n",
        // calls
        "(call_expression function: (identifier) @name) @reference.call\n"
        "(call_expression function: (qualified_identifier name: (identifier) @name)) @reference.call\n",
        // cfg
        "(if_statement) @cfg.branch\n"
        "(else_clause) @cfg.branch\n"
        "(while_statement) @cfg.loop\n"
        "(for_statement) @cfg.loop\n"
        "(for_range_loop) @cfg.loop\n"
        "(do_statement) @cfg.loop\n"
        "(case_statement) @cfg.branch\n"
        "(return_statement) @cfg.return\n"
        "(try_statement) @cfg.branch\n"
        "(catch_clause) @cfg.branch\n"
    };
}

inline LanguageQueries java() {
    return {
        // symbols
        "(method_declaration name: (identifier) @name) @definition.method\n"
        "(class_declaration name: (identifier) @name) @definition.class\n"
        "(interface_declaration name: (identifier) @name) @definition.interface\n",
        // calls
        "(method_invocation name: (identifier) @name) @reference.call\n",
        // cfg -- no CFG support for Java in this plan
        nullptr
    };
}

inline LanguageQueries kotlin() {
    return {
        // symbols
        "(function_declaration (simple_identifier) @name) @definition.function\n"
        "(class_declaration (type_identifier) @name) @definition.class\n",
        // calls
        "(call_expression (simple_identifier) @name) @reference.call\n",
        // cfg -- no CFG support for Kotlin in this plan
        nullptr
    };
}

inline LanguageQueries swift() {
    return {
        // symbols -- tree-sitter-swift has no struct_declaration node;
        // structs appear inside class_declaration or as keyword only
        "(function_declaration name: (simple_identifier) @name) @definition.function\n"
        "(class_declaration name: (type_identifier) @name) @definition.class\n"
        "(protocol_declaration name: (type_identifier) @name) @definition.interface\n",
        // calls
        "(call_expression (simple_identifier) @name) @reference.call\n",
        // cfg -- no CFG support for Swift in this plan
        nullptr
    };
}

inline LanguageQueries objc() {
    return {
        // symbols -- ObjC grammar uses class_interface (not class_declaration)
        "(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function\n"
        "(class_interface (identifier) @name) @definition.class\n"
        "(protocol_declaration (identifier) @name) @definition.interface\n"
        "(method_definition (identifier) @name) @definition.method\n",
        // calls -- message_expression has method: (identifier) children; also C-style calls
        "(message_expression method: (identifier) @name) @reference.call\n"
        "(call_expression function: (identifier) @name) @reference.call\n",
        // cfg -- no CFG support for ObjC in this plan
        nullptr
    };
}

} // namespace codetldr::queries
