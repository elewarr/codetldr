#pragma once

namespace codetldr::queries {

struct LanguageQueries {
    const char* symbols;  // Query string for symbol definitions
    const char* calls;    // Query string for call sites
};

inline LanguageQueries python() {
    return {
        // symbols
        "(function_definition name: (identifier) @name) @definition.function\n"
        "(class_definition name: (identifier) @name) @definition.class\n",
        // calls
        "(call function: (identifier) @name) @reference.call\n"
        "(call function: (attribute attribute: (identifier) @name)) @reference.call\n"
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
        "(call_expression function: (member_expression property: (property_identifier) @name)) @reference.call\n"
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
        "(call_expression function: (member_expression property: (property_identifier) @name)) @reference.call\n"
    };
}

inline LanguageQueries tsx() {
    // TSX is TypeScript with JSX -- same queries as TypeScript
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
        "(call_expression function: (scoped_identifier name: (identifier) @name)) @reference.call\n"
    };
}

inline LanguageQueries c() {
    return {
        // symbols
        "(function_definition declarator: (function_declarator declarator: (identifier) @name)) @definition.function\n"
        "(struct_specifier name: (type_identifier) @name) @definition.class\n"
        "(enum_specifier name: (type_identifier) @name) @definition.class\n",
        // calls
        "(call_expression function: (identifier) @name) @reference.call\n"
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
        "(call_expression function: (qualified_identifier name: (identifier) @name)) @reference.call\n"
    };
}

inline LanguageQueries java() {
    return {
        // symbols
        "(method_declaration name: (identifier) @name) @definition.method\n"
        "(class_declaration name: (identifier) @name) @definition.class\n"
        "(interface_declaration name: (identifier) @name) @definition.interface\n",
        // calls
        "(method_invocation name: (identifier) @name) @reference.call\n"
    };
}

inline LanguageQueries kotlin() {
    return {
        // symbols
        "(function_declaration (simple_identifier) @name) @definition.function\n"
        "(class_declaration (type_identifier) @name) @definition.class\n",
        // calls
        "(call_expression (simple_identifier) @name) @reference.call\n"
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
        "(call_expression (simple_identifier) @name) @reference.call\n"
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
        "(call_expression function: (identifier) @name) @reference.call\n"
    };
}

} // namespace codetldr::queries
