; Outline panel and symbol search (cmd-shift-o) for Iron buffers.

(object_declaration
  (visibility_modifier)? @context
  "object" @context
  name: (identifier) @name) @item

(patch_declaration
  "patch" @context
  "object" @context
  target: (identifier) @name) @item

(interface_declaration
  (visibility_modifier)? @context
  "interface" @context
  name: (identifier) @name) @item

(enum_declaration
  (visibility_modifier)? @context
  "enum" @context
  name: (identifier) @name) @item

(enum_variant
  name: (identifier) @name) @item

(field_declaration
  qualifier: _ @context
  name: (identifier) @name) @item

(func_declaration
  (visibility_modifier)? @context
  "func" @context
  name: (identifier) @name
  parameters: (parameter_list) @context) @item

(method_declaration
  "func" @context
  type_name: (identifier)? @context
  name: (identifier) @name
  parameters: (parameter_list) @context) @item

(block_method_declaration
  (visibility_modifier)? @context
  "func" @context
  name: (identifier) @name
  parameters: (parameter_list) @context) @item

(method_signature
  "func" @context
  name: (identifier) @name
  parameters: (parameter_list) @context) @item

(extern_func_declaration
  "extern" @context
  "func" @context
  name: (identifier) @name) @item

(init_declaration
  "init" @name
  name: (identifier)? @name
  parameters: (parameter_list) @context) @item

(test_declaration
  "test" @context
  name: (string_literal) @name) @item

(source_file
  (val_declaration
    "val" @context
    binding: (identifier) @name) @item)

(source_file
  (var_declaration
    "var" @context
    binding: (identifier) @name) @item)
