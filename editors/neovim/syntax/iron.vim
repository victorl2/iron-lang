" editors/neovim/syntax/iron.vim
" Regex highlighting for Iron, used when the tree-sitter parser is not
" installed (ftplugin/iron.lua starts tree-sitter when it is, which turns
" this off for the buffer). ironls semantic tokens add types, functions,
" parameters and fields on top.
"
" The keyword list mirrors src/lexer/lexer.c kw_table; the Neovim e2e suite
" checks it against the tree-sitter grammar's _keyword rule.

if exists('b:current_syntax')
  finish
endif

syn keyword ironConditional if elif else match
syn keyword ironRepeat      while for in break continue parallel
syn keyword ironStatement   return defer free leak spawn await
syn keyword ironKeyword     func val var init object interface enum patch impl extends extern import
syn keyword ironKeyword     drop copy heap rc weak pool comptime super
syn keyword ironModifier    pub private pure readonly mut nocopy unchecked
syn keyword ironOperator    and or not is
syn keyword ironBoolean     true false
syn keyword ironConstant    null
syn keyword ironSelf        self
" Contextual keywords: `test "name" { }`, `patch object T implements I`,
" `import a.b as c`.
syn match   ironKeyword     /^\s*\zstest\ze\s\+"/
syn keyword ironKeyword     implements as

syn keyword ironType        Int Int8 Int16 Int32 Int64 UInt UInt8 UInt16 UInt32 UInt64
syn keyword ironType        Float Float32 Float64 Bool String Char Byte Void

syn match   ironNumber      /\<0[xX][0-9a-fA-F_]\+\>/
syn match   ironNumber      /\<0[bB][01_]\+\>/
syn match   ironNumber      /\<\d[0-9_]*\>/
syn match   ironFloat       /\<\d[0-9_]*\.\d[0-9_]*\([eE][+-]\=\d\+\)\=\>/

syn match   ironEscape      contained /\\\(u{\x\{1,6}}\|.\)/
syn region  ironInterp      contained matchgroup=ironInterpDelim start=/{/ end=/}/ contains=TOP
syn region  ironString      start=/"/ skip=/\\\\\|\\"/ end=/"/ contains=ironEscape,ironInterp

syn match   ironFuncName    /\<func\s\+\zs\h\w*\ze\s*[\[(]/
syn match   ironTypeName    /\<\(object\|interface\|enum\|patch\s\+object\)\s\+\zs\h\w*/

syn match   ironDocComment  /\/\/\/.*$/ contains=@Spell
syn match   ironComment     /--.*$/ contains=ironTodo,@Spell
syn keyword ironTodo        contained TODO FIXME XXX NOTE

hi def link ironConditional Conditional
hi def link ironRepeat      Repeat
hi def link ironStatement   Statement
hi def link ironKeyword     Keyword
hi def link ironModifier    StorageClass
hi def link ironOperator    Operator
hi def link ironBoolean     Boolean
hi def link ironConstant    Constant
hi def link ironSelf        Special
hi def link ironType        Type
hi def link ironNumber      Number
hi def link ironFloat       Float
hi def link ironEscape      SpecialChar
hi def link ironInterpDelim Delimiter
hi def link ironString      String
hi def link ironFuncName    Function
hi def link ironTypeName    Type
hi def link ironDocComment  SpecialComment
hi def link ironComment     Comment
hi def link ironTodo        Todo

let b:current_syntax = 'iron'
