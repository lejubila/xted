/* Built-in language definitions, in the same text format that will be read
 * from ~/.config/ked/syntax/ (Phase 7). One definition per language; adding
 * a language means adding data here, not code.
 *
 * Format: one directive per line, blank-separated; lines starting with '#'
 * are comments.
 *   name TEXT                 shown in the status bar
 *   ext / file / shebang / alias WORD...   how the language is detected
 *   words CLASS WORD...       keywords, types, constants (any class)
 *   comment TOKEN [END]       markers used by the comment toggle (END for block-only languages)
 *   number                    highlight numeric literals
 *   calls CLASS               identifier followed by '(' gets CLASS
 *   sigil CLASS CHAR          CHAR followed by a name (shell $var, Python @decorator)
 *   operators CHARS           characters of class operator
 *   idchars CHARS             extra identifier characters
 *   region CLASS START END [ml] [bol] [ns] [esc=C]
 *                             END "$" = end of line; ml = continues on the next
 *                             lines; bol = only at line start; ns = START not
 *                             followed by a blank. The first region that matches wins.
 * Classes: normal comment string keyword type constant number preproc function
 *          operator variable heading emphasis code link
 */
#include "langs.h"

static const char *const lang_c[] = {
    "name C",
    "comment //",
    "ext c h cc cpp cxx hpp hh hxx",
    "alias c++ cpp",
    "words keyword if else for while do switch case default break continue return goto "
        "sizeof typedef struct union enum static extern const volatile inline register "
        "class public private protected namespace using template typename new delete "
        "this try catch throw operator virtual override final auto constexpr",
    "words type void char short int long float double signed unsigned size_t ssize_t "
        "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t uintptr_t "
        "intptr_t ptrdiff_t bool FILE wchar_t",
    "words constant NULL true false nullptr EOF",
    "region comment /* */ ml",
    "region comment // $",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "region preproc # $ bol",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~?:",
    NULL
};

static const char *const lang_python[] = {
    "name Python",
    "comment #",
    "ext py pyw pyi",
    "file SConstruct SConscript",
    "shebang python pypy",
    "words keyword and as assert async await break class continue def del elif else "
        "except finally for from global if import in is lambda nonlocal not or pass "
        "raise return try while with yield match case",
    "words type int str float list dict set frozenset tuple bool bytes bytearray object "
        "type complex range",
    "words constant True False None Ellipsis NotImplemented __name__ __file__",
    "words variable self cls",
    "region comment # $",
    "region string \"\"\" \"\"\" ml esc=\\",
    "region string ''' ''' ml esc=\\",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "sigil preproc @",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~:",
    NULL
};

static const char *const lang_js[] = {
    "name JavaScript",
    "comment //",
    "ext js mjs cjs jsx ts tsx",
    "shebang node deno bun",
    "alias javascript typescript js ts",
    "words keyword break case catch class const continue debugger default delete do else "
        "export extends finally for function if import in instanceof let new of return "
        "super switch this throw try typeof var void while with yield async await static "
        "get set interface implements enum namespace declare abstract readonly private "
        "public protected as from type",
    "words type string number boolean any unknown never object symbol bigint Array "
        "Object Promise Map Set Date RegExp Error",
    "words constant true false null undefined NaN Infinity",
    "idchars $",
    "region comment /* */ ml",
    "region comment // $",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "region string ` ` ml esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~?:",
    NULL
};

static const char *const lang_shell[] = {
    "name Shell",
    "comment #",
    "ext sh bash zsh ksh",
    "file .bashrc .bash_profile .bash_aliases .profile .zshrc .zprofile",
    "shebang sh bash zsh dash ash ksh",
    "alias sh bash zsh shell",
    "words keyword if then else elif fi for while until do done case esac in function "
        "select time coproc",
    "words type echo cd export local read return exit set unset shift test source alias "
        "eval exec printf trap wait kill declare readonly typeset let getopts break "
        "continue pwd true false",
    "region variable ${ }",
    "region comment # $",
    "region string \" \" ml esc=\\",
    "region string ' ' ml",
    "sigil variable $",
    "number",
    "operators |&;<>=!",
    NULL
};

static const char *const lang_markdown[] = {
    "name Markdown",
    "comment <!-- -->",
    "ext md markdown mdown",
    "alias md markdown",
    "region code ``` ``` ml bol",
    "region code ~~~ ~~~ ml bol",
    "region heading # $ bol",
    "region comment > $ bol",
    "region code ` `",
    "region emphasis ** **",
    "region emphasis __ __",
    "region emphasis * * ns",
    "region link [ ]",
    NULL
};

const LangDef lang_defs[] = {
    { "C", lang_c },
    { "Python", lang_python },
    { "JavaScript", lang_js },
    { "Shell", lang_shell },
    { "Markdown", lang_markdown },
};

const size_t lang_ndefs = sizeof lang_defs / sizeof lang_defs[0];
