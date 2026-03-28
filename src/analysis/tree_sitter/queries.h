#pragma once

namespace codetldr::queries {

struct LanguageQueries {
    const char* symbols;  // Query string for symbol definitions
    const char* calls;    // Query string for call sites
    const char* cfg;      // CFG query string (nullptr for languages without CFG support)
    const char* dfg;      // DFG query string (nullptr for languages without DFG support)
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
        "(except_clause) @cfg.branch\n",
        // dfg
        "(assignment left: (_) @dfg.lhs right: (_) @dfg.rhs) @dfg.assignment\n"
        "(augmented_assignment left: (_) @dfg.lhs right: (_) @dfg.rhs) @dfg.assignment\n"
        "(parameters (identifier) @dfg.param) @dfg.parameter\n"
        "(parameters (default_parameter name: (identifier) @dfg.param)) @dfg.parameter\n"
        "(return_statement (_) @dfg.rhs) @dfg.return\n"
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
        "(catch_clause) @cfg.branch\n",
        // dfg
        "(lexical_declaration (variable_declarator name: (identifier) @dfg.lhs value: (_) @dfg.rhs)) @dfg.assignment\n"
        "(variable_declaration (variable_declarator name: (identifier) @dfg.lhs value: (_) @dfg.rhs)) @dfg.assignment\n"
        "(assignment_expression left: (identifier) @dfg.lhs right: (_) @dfg.rhs) @dfg.assignment\n"
        "(formal_parameters (identifier) @dfg.param) @dfg.parameter\n"
        "(return_statement (_) @dfg.rhs) @dfg.return\n"
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
        nullptr,
        // dfg -- no DFG support for TypeScript in this plan
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
        nullptr,
        // dfg -- no DFG support for Rust in this plan
        nullptr
    };
}

inline LanguageQueries go() {
    return {
        // symbols
        "(function_declaration name: (identifier) @name) @definition.function\n"
        "(method_declaration name: (field_identifier) @name) @definition.method\n"
        "(type_declaration (type_spec name: (type_identifier) @name)) @definition.class\n",
        // calls
        "(call_expression function: (identifier) @name) @reference.call\n"
        "(call_expression function: (selector_expression field: (field_identifier) @name)) @reference.call\n",
        // cfg -- no CFG support for Go yet
        nullptr,
        // dfg -- no DFG support for Go yet
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
        "(return_statement) @cfg.return\n",
        // dfg
        "(declaration declarator: (init_declarator declarator: (identifier) @dfg.lhs value: (_) @dfg.rhs)) @dfg.assignment\n"
        "(assignment_expression left: (identifier) @dfg.lhs right: (_) @dfg.rhs) @dfg.assignment\n"
        "(parameter_declaration declarator: (identifier) @dfg.param) @dfg.parameter\n"
        "(parameter_declaration declarator: (pointer_declarator declarator: (identifier) @dfg.param)) @dfg.parameter\n"
        "(return_statement (_) @dfg.rhs) @dfg.return\n"
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
        "(catch_clause) @cfg.branch\n",
        // dfg
        "(declaration declarator: (init_declarator declarator: (identifier) @dfg.lhs value: (_) @dfg.rhs)) @dfg.assignment\n"
        "(assignment_expression left: (identifier) @dfg.lhs right: (_) @dfg.rhs) @dfg.assignment\n"
        "(parameter_declaration declarator: (identifier) @dfg.param) @dfg.parameter\n"
        "(parameter_declaration declarator: (pointer_declarator declarator: (identifier) @dfg.param)) @dfg.parameter\n"
        "(parameter_declaration declarator: (reference_declarator (identifier) @dfg.param)) @dfg.parameter\n"
        "(return_statement (_) @dfg.rhs) @dfg.return\n"
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
        nullptr,
        // dfg -- no DFG support for Java in this plan
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
        nullptr,
        // dfg -- no DFG support for Kotlin in this plan
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
        nullptr,
        // dfg -- no DFG support for Swift in this plan
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
        nullptr,
        // dfg -- no DFG support for ObjC in this plan
        nullptr
    };
}

inline LanguageQueries ruby() {
    return {
        // symbols — use (_) wildcard for method name: Ruby's method name is polymorphic
        // (identifier, setter, operator, constant — must use (_) wildcard)
        "(method name: (_) @name) @definition.method\n"
        "(singleton_method name: (_) @name) @definition.method\n"
        "(class name: (constant) @name) @definition.class\n"
        "(class name: (scope_resolution name: (constant) @name)) @definition.class\n"
        "(module name: (constant) @name) @definition.class\n"
        "(module name: (scope_resolution name: (constant) @name)) @definition.class\n",
        // calls — Ruby method calls: obj.method(args) uses (call method: ...)
        "(call method: (_) @name) @reference.call\n",
        // cfg — no CFG support for Ruby
        nullptr,
        // dfg — no DFG support for Ruby
        nullptr
    };
}

inline LanguageQueries lua() {
    return {
        // symbols -- 3 forms of function_declaration name:
        // Form 1: Plain identifier: function foo() end
        "(function_declaration name: (identifier) @name) @definition.function\n"
        // Form 2: Dot index (table method): function M.foo() end
        "(function_declaration name: (dot_index_expression field: (identifier) @name)) @definition.method\n"
        // Form 3: Colon method: function M:foo() end
        "(function_declaration name: (method_index_expression method: (identifier) @name)) @definition.method\n",
        // calls -- Lua uses function_call node (NOT call_expression)
        "(function_call name: (identifier) @name) @reference.call\n"
        "(function_call name: (dot_index_expression field: (identifier) @name)) @reference.call\n"
        "(function_call name: (method_index_expression method: (identifier) @name)) @reference.call\n",
        // cfg -- no CFG support for Lua
        nullptr,
        // dfg -- no DFG support for Lua
        nullptr
    };
}

} // namespace codetldr::queries
