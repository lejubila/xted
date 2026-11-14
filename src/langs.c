/* Built-in language definitions, in the same text format that will be read
 * from ~/.config/xted/syntax/ (Phase 7). One definition per language; adding
 * a language means adding data here, not code.
 *
 * Format: one directive per line, blank-separated; lines starting with '#'
 * are comments.
 *   name TEXT                 shown in the status bar
 *   ext / file / shebang / alias WORD...   how the language is detected
 *   words CLASS WORD...       keywords, types, constants (any class)
 *   comment TOKEN [END]       markers used by the comment toggle (END for block-only languages)
 *   number                    highlight numeric literals
 *   nocase                    word lists ignore case (SQL)
 *   calls CLASS               identifier followed by '(' gets CLASS
 *   sigil CLASS CHAR          CHAR followed by a name (shell $var, Python @decorator)
 *   operators CHARS           characters of class operator
 *   idchars CHARS             extra identifier characters
 *   embed LANG CLASS START END [open=TEXT] [peek]
 *                             the text between START and END is another language (name or
 *                             alias), one level deep; START and END take CLASS. open= also
 *                             colors the rest of the opening tag up to TEXT; peek leaves END
 *                             to this language. Write embeds before the regions.
 *   region CLASS START END [ml] [bol] [ns] [esc=C]
 *                             END "$" = end of line, "-" = the START token alone; ml = continues
 *                             on the next lines; bol = only at line start; ns = START not
 *                             followed by a blank. The first region that matches wins.
 *   decl KIND PATTERN         a definition for the autosuggest index. KIND: class func method
 *                             prop const var ns (ns: a block that is only a scope). A func inside a
 *                             class is a method. First matching decl at a statement start wins.
 *   include PATTERN           names a file (~ a quoted string, & a dotted module path, % a class)
 *   bind PATTERN              ties a variable (@) to a type (%), searched upwards from the cursor
 *   synonym PATTERN           a class known by another name (@ the alias, % the class)
 *   docmember PATTERN         a member declared in the doc comment above a class (@method, @property):
 *                             a pattern with a "(" is a method, otherwise a property
 *   attr PATTERN              a member created by assignment inside a method (this.x = ...)
 *   member OPS                operators that open the member list: member -> ::
 *   scope indent              blocks are marked by indentation (default: braces)
 *   Patterns are items separated by blanks, matched after skipping blanks: a|b alternatives,
 *   NAME* repeated, NAME? optional (a modifier word counts only when a name follows it),
 *   @ identifier, % type (the last segment of a qualified one; generics and * & are skipped),
 *   ^ parameters up to the matching ")" (the "(" is written before it), ~ quoted string,
 *   & dotted path, <> optional <...>, $$ end of line, =X lookahead (not consumed),
 *   anything else literal. A pattern may stop early: the rest of the line is not checked.
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
    "member . -> ::",
    "decl ns extern ~ ={",
    "decl ns namespace @ ={",
    "decl ns namespace ={",
    "decl ns namespace @ $$",
    "decl class template? <> class|struct @ final? : public|protected|private|virtual* % ={",
    "decl class template? <> class|struct @ final? ={|:",
    "decl class template? <> class|struct @ final? $$",
    "decl class typedef? struct|union|enum @ ={",
    "decl class typedef? struct|union|enum @ $$",
    "decl class typedef struct|union|enum ={",
    "decl class typedef struct|union|enum $$",
    "decl class typedef unsigned|signed|const|struct|enum|union* % @ =;",
    "decl func # define @ ( ^",
    "decl const # define @",
    "decl func static|inline|extern|const|unsigned|signed|struct|union|enum|volatile|virtual|explicit|constexpr|typename|friend* % @ ( ^",
    "decl method @ ( ^",
    "decl prop static|const|unsigned|signed|struct|union|enum|volatile|mutable* % @ =;|,|[|:|=",
    "decl var static|extern|const|unsigned|signed|struct|union|enum|volatile* % @ =;|=|[",
    "include # include ~",
    "attr this -> @ =",
    "bind auto @ = new %",
    "bind auto @ = % (",
    "bind @ = new %",
    "synonym using @ = %",
    "bind const|struct|enum|union|volatile|static|unsigned|constexpr* % @ =;|=|(|[|,|)|{|:",
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
    "scope indent",
    "member .",
    "decl class class @ ( %",
    "decl class class @",
    "decl func async? def @ ( ^ -> %",
    "decl func async? def @ ( ^",
    "decl prop @ = % (",
    "decl prop @ : %",
    "decl prop @ =",
    "decl var @ = % (",
    "decl var @ : %",
    "decl var @ =",
    "include from & import",
    "synonym from & import % as @",
    "include import &",
    "attr self . @ = % (",
    "attr self . @ : %",
    "attr self . @ =",
    "bind @ = % (",
    "bind @ : %",
    "bind @ = ' !str",
    "bind @ = \" !str",
    "bind @ = [ !list",
    "bind @ = { !dict",
    NULL
};

static const char *const lang_js[] = {
    "name JavaScript",
    "comment //",
    "ext js mjs cjs jsx",
    "shebang node deno bun",
    "alias javascript js jsx node",
    "words keyword async await break case catch class const continue debugger default delete "
        "do else export extends finally for from function get if import in instanceof let new "
        "of return set static super switch this throw try typeof var void while with yield as",
    "words type Array Object Promise Map Set WeakMap WeakSet Date RegExp Error TypeError "
        "RangeError Symbol Number String Boolean BigInt JSON Math Intl Proxy Reflect Function",
    "words constant true false null undefined NaN Infinity",
    "words variable console document window globalThis process require module exports "
        "arguments",
    "idchars $",
    "region comment /* */ ml",
    "region comment // $",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "region string ` ` ml esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~?:",
    "member .",
    "decl class export? default? abstract? class @ extends %",
    "decl class export? default? abstract? class @",
    "decl func export? default? async? function @ ( ^ : %",
    "decl func export? default? async? function @ ( ^",
    "decl func export? const|let|var @ = ( ^",
    "decl func export? const|let|var @ = async (",
    "decl func export? const|let|var @ = async function",
    "decl func export? const|let|var @ = function",
    "decl method static|async|get|set|public|private|protected|readonly|override|abstract* @ ( ^ : %",
    "decl method static|async|get|set|public|private|protected|readonly|override|abstract* @ ( ^",
    "decl prop static|public|private|protected|readonly|declare|override* @ : %",
    "decl prop static|public|private|protected|readonly|declare|override* @ =;|=",
    "decl var export? const|let|var @ = new %",
    "decl var export? const|let|var @ =",
    "include from ~",
    "include import ~",
    "include require ~",
    "attr this . @ = new %",
    "attr this . @ =",
    "bind @ = new %",
    "bind @ : %",
    "bind @ = % (",
    "bind @ = ' !String",
    "bind @ = \" !String",
    "bind @ = ` !String",
    "bind @ = [ !Array",
    NULL
};

static const char *const lang_ts[] = {
    "name TypeScript",
    "comment //",
    "ext ts tsx mts cts",
    "alias typescript ts tsx",
    "words keyword abstract as asserts async await break case catch class const continue "
        "debugger declare default delete do else enum export extends finally for from function "
        "get if implements import in infer instanceof interface is keyof let namespace new of "
        "override private protected public readonly return satisfies set static super switch "
        "this throw try type typeof var void while with yield module",
    "words type string number boolean any unknown never object symbol bigint void Array "
        "Object Promise Map Set WeakMap WeakSet Date RegExp Error Record Partial Required "
        "Readonly Pick Omit Function",
    "words constant true false null undefined NaN Infinity",
    "words variable console document window globalThis process require module exports "
        "arguments",
    "idchars $",
    "sigil preproc @",
    "region comment /* */ ml",
    "region comment // $",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "region string ` ` ml esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~?:",
    "member .",
    "decl class export? default? declare? interface @ extends %",
    "decl class export? default? declare? interface @",
    "decl class export? default? declare? enum @",
    "decl class export? default? abstract? class @ extends %",
    "decl class export? default? abstract? class @",
    "decl func export? default? async? function @ ( ^ : %",
    "decl func export? default? async? function @ ( ^",
    "decl func export? const|let|var @ = ( ^",
    "decl func export? const|let|var @ = async (",
    "decl func export? const|let|var @ = async function",
    "decl func export? const|let|var @ = function",
    "decl method static|async|get|set|public|private|protected|readonly|override|abstract* @ ( ^ : %",
    "decl method static|async|get|set|public|private|protected|readonly|override|abstract* @ ( ^",
    "decl prop static|public|private|protected|readonly|declare|override* @ : %",
    "decl prop static|public|private|protected|readonly|declare|override* @ =;|=",
    "decl var export? const|let|var @ = new %",
    "decl var export? const|let|var @ =",
    "include from ~",
    "include import ~",
    "include require ~",
    "attr this . @ = new %",
    "attr this . @ =",
    "bind @ = new %",
    "bind @ : %",
    "bind @ = % (",
    "bind @ = ' !String",
    "bind @ = \" !String",
    "bind @ = ` !String",
    "bind @ = [ !Array",
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

#define PHP_SYM_RULES \
    "member -> ::", \
    "decl class abstract|final|readonly* class|interface|trait|enum @ extends %", \
    "decl class abstract|final|readonly* class|interface|trait|enum @", \
    "decl func public|protected|private|static|abstract|final* function @ ( ^ : %", \
    "decl func public|protected|private|static|abstract|final* function @ ( ^", \
    "decl prop public|protected|private|static|readonly|var public|protected|private|static|readonly* % $ @", \
    "decl prop public|protected|private|static|readonly|var public|protected|private|static|readonly* $ @", \
    "decl const public|protected|private|final* const @ =", \
    "include require|require_once|include|include_once ~", \
    "include use % =;|as", \
    "include new %", \
    "attr $ this -> @ = new %", \
    "attr $ this -> @ =", \
    "bind $ @ = new %", \
    "bind $ @ = % (", \
    "synonym use % as @", \
    "docmember @method static? % @ ( ^", \
    "docmember @method static? @ ( ^", \
    "docmember @property|@property-read|@property-write % $ @", \
    "docmember @see %", \
    "bind var % $ @", \
    "bind % $ @"

static const char *const lang_php[] = {
    "name PHP",
    "comment //",
    "ext php php3 php4 php5 php7 php8 phps",
    "shebang php",
    "alias php",
    "words keyword abstract and as break callable case catch class clone const continue "
        "declare default do echo else elseif empty enddeclare endfor endforeach endif "
        "endswitch endwhile enum exit die extends final finally fn for foreach function global "
        "goto if implements include include_once instanceof insteadof interface isset list "
        "match namespace new or print private protected public readonly require require_once "
        "return static switch throw trait try unset use var while xor yield parent self",
    "words type int float bool string array object iterable mixed void never callable "
        "Closure Exception Throwable stdClass",
    "words constant true false null TRUE FALSE NULL PHP_EOL PHP_INT_MAX PHP_INT_MIN "
        "PHP_VERSION __FILE__ __DIR__ __LINE__ __CLASS__ __FUNCTION__ __METHOD__ "
        "__NAMESPACE__ E_ALL E_ERROR E_WARNING E_NOTICE",
    "embed HTML preproc ?> <? peek",
    "region preproc <?php -",
    "region preproc <?= -",
    "region preproc #[ ] ml",
    "region comment /* */ ml",
    "region comment // $",
    "region comment # $",
    "region string \" \" ml esc=\\",
    "region string ' ' ml esc=\\",
    "sigil variable $",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~?:.@",
    PHP_SYM_RULES,
    NULL
};

/* Script and style bodies are colored as their own language, PHP tags switch to PHP. */
#define HTML_RULES \
    "embed JavaScript keyword <script </script> open=>", \
    "embed CSS keyword <style </style> open=>", \
    "embed PHP preproc <?php ?>", \
    "embed PHP preproc <?= ?>", \
    "region comment <!-- --> ml", \
    "region preproc <! > ml", \
    "region keyword < > ml ns"

static const char *const lang_html[] = {
    "name HTML",
    "comment <!-- -->",
    "ext html htm xhtml vue svelte",
    "alias html htm xhtml vue svelte",
    HTML_RULES,
    NULL
};

/* A .php file that starts with HTML (see syn_detect), and .phtml. */
static const char *const lang_php_html[] = {
    "name PHP/HTML",
    "comment <!-- -->",
    "ext phtml",
    "alias php-html phtml",
    HTML_RULES,
    PHP_SYM_RULES,
    NULL
};

static const char *const lang_xml[] = {
    "name XML",
    "comment <!-- -->",
    "ext xml xsd xsl xslt svg plist rss atom xaml csproj",
    "alias xml svg",
    "region comment <!-- --> ml",
    "region string <![CDATA[ ]]> ml",
    "region preproc <? ?> ml",
    "region preproc <! > ml",
    "region keyword < > ml ns",
    NULL
};

static const char *const lang_css[] = {
    "name CSS",
    "comment /* */",
    "ext css scss less",
    "alias css scss less",
    "words keyword important color background background-color background-image border "
        "border-radius margin margin-top margin-right margin-bottom margin-left padding "
        "padding-top padding-right padding-bottom padding-left font font-family font-size "
        "font-weight line-height text-align text-decoration display position top right bottom "
        "left width height min-width max-width min-height max-height overflow opacity z-index "
        "flex flex-direction justify-content align-items gap grid transition transform "
        "box-shadow cursor content float clear visibility white-space",
    "words constant none auto inherit initial unset block inline inline-block flex grid "
        "absolute relative fixed sticky solid dashed dotted transparent bold normal center "
        "hidden visible pointer",
    "words type html body div span a p ul ol li h1 h2 h3 h4 h5 h6 table tr td th form input "
        "button img nav header footer section article main label select option textarea "
        "pre code",
    "idchars -",
    "sigil preproc @",
    "region comment /* */ ml",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "number",
    "calls function",
    "operators +*/=<>!:",
    NULL
};

static const char *const lang_json[] = {
    "name JSON",
    "comment //",
    "ext json jsonc json5 geojson webmanifest",
    "file .eslintrc .prettierrc .babelrc composer.lock",
    "alias json jsonc",
    "words constant true false null",
    "region comment /* */ ml",
    "region comment // $",
    "region string \" \" esc=\\",
    "number",
    "operators :,",
    NULL
};

static const char *const lang_yaml[] = {
    "name YAML",
    "comment #",
    "ext yml yaml",
    "file .clang-format",
    "alias yaml yml",
    "words constant true false yes no null True False Yes No Null",
    "region preproc --- - bol",
    "region preproc ... - bol",
    "region comment # $",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "number",
    "operators :-",
    NULL
};

static const char *const lang_toml[] = {
    "name TOML",
    "comment #",
    "ext toml",
    "file Cargo.lock Pipfile",
    "alias toml",
    "words constant true false",
    "region heading [[ ]] bol",
    "region heading [ ] bol",
    "region comment # $",
    "region string \"\"\" \"\"\" ml esc=\\",
    "region string ''' ''' ml",
    "region string \" \" esc=\\",
    "region string ' '",
    "number",
    "operators =",
    NULL
};

static const char *const lang_sql[] = {
    "name SQL",
    "comment --",
    "ext sql ddl dml",
    "alias sql mysql pgsql plsql",
    "nocase",
    "words keyword select from where and or not insert into values update set delete create "
        "alter drop table index view database schema join inner left right outer full cross on "
        "as group by order having limit offset union all distinct case when then else end is "
        "in like ilike between exists primary key foreign references default unique check "
        "constraint begin commit rollback transaction with returning asc desc add column "
        "truncate grant revoke trigger function procedure if",
    "words type int integer bigint smallint tinyint serial varchar char text date datetime "
        "timestamp time boolean bool float double decimal numeric real blob json jsonb uuid",
    "words constant null true false",
    "region comment /* */ ml",
    "region comment -- $",
    "region string ' ' ml",
    "region string \" \"",
    "region string ` `",
    "number",
    "calls function",
    "operators +-*/%=<>!|:",
    NULL
};

static const char *const lang_go[] = {
    "name Go",
    "comment //",
    "ext go",
    "file go.mod go.sum",
    "alias go golang",
    "words keyword break case chan const continue default defer else fallthrough for func go "
        "goto if import interface map package range return select struct switch type var",
    "words type bool byte complex64 complex128 error float32 float64 int int8 int16 int32 "
        "int64 rune string uint uint8 uint16 uint32 uint64 uintptr any comparable",
    "words constant nil true false iota",
    "region comment /* */ ml",
    "region comment // $",
    "region string \" \" esc=\\",
    "region string ` ` ml",
    "region string ' ' esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^:",
    NULL
};

static const char *const lang_rust[] = {
    "name Rust",
    "comment //",
    "ext rs",
    "alias rust rs",
    "words keyword as async await break const continue crate dyn else enum extern fn for if "
        "impl in let loop match mod move mut pub ref return self static struct super trait "
        "type unsafe use where while",
    "words type i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str "
        "String Vec Option Result Box Rc Arc Self HashMap HashSet",
    "words constant true false None Some Ok Err",
    "region comment /* */ ml",
    "region comment // $",
    "region preproc #![ ] ml",
    "region preproc #[ ] ml",
    "region string \" \" ml esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^?:",
    NULL
};

static const char *const lang_java[] = {
    "name Java",
    "comment //",
    "ext java",
    "alias java",
    "words keyword abstract assert break case catch class const continue default do else "
        "enum extends final finally for goto if implements import instanceof interface native "
        "new package permits private protected public record return sealed static strictfp "
        "super switch synchronized this throw throws transient try var volatile while yield",
    "words type boolean byte char short int long float double void String Object Integer "
        "Long Double Boolean List Map Set ArrayList HashMap Optional",
    "words constant true false null",
    "sigil preproc @",
    "region comment /* */ ml",
    "region comment // $",
    "region string \"\"\" \"\"\" ml esc=\\",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~?:",
    NULL
};

static const char *const lang_csharp[] = {
    "name C#",
    "comment //",
    "ext cs csx",
    "alias csharp cs c#",
    "words keyword abstract as async await base break case catch checked class const "
        "continue default delegate do else enum event explicit extern finally fixed for "
        "foreach goto if implicit in init interface internal is lock namespace new operator "
        "out override params private protected public readonly record ref return sealed "
        "sizeof stackalloc static struct switch this throw try typeof unchecked unsafe using "
        "var virtual volatile while yield get set",
    "words type bool byte char decimal double float int long object sbyte short string uint "
        "ulong ushort void dynamic String List Dictionary Task",
    "words constant true false null",
    "region comment /* */ ml",
    "region comment // $",
    "region preproc # $ bol",
    "region string @\" \" ml",
    "region string \" \" esc=\\",
    "region string ' ' esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~?:",
    NULL
};

static const char *const lang_ruby[] = {
    "name Ruby",
    "comment #",
    "ext rb rake gemspec ru",
    "file Rakefile Gemfile Vagrantfile Podfile",
    "shebang ruby",
    "alias ruby rb",
    "words keyword alias and begin break case class def defined? do else elsif end ensure "
        "for if in module next not or redo rescue retry return self super then undef unless "
        "until when while yield require require_relative include extend attr_accessor "
        "attr_reader attr_writer raise lambda proc private protected public",
    "words constant true false nil",
    "idchars ?",
    "sigil variable @",
    "region comment =begin =end ml bol",
    "region comment # $",
    "region string \" \" ml esc=\\",
    "region string ' ' ml esc=\\",
    "number",
    "calls function",
    "operators +-*/%=<>!&|^~:",
    NULL
};

static const char *const lang_make[] = {
    "name Makefile",
    "comment #",
    "ext mk mak",
    "file Makefile makefile GNUmakefile",
    "alias make makefile",
    "words keyword ifeq ifneq ifdef ifndef else endif include sinclude define endef export "
        "unexport override vpath",
    "region variable $( ) ",
    "region variable ${ }",
    "region comment # $",
    "region string \" \" esc=\\",
    "region string ' '",
    "sigil variable $",
    "operators :=+?!|",
    NULL
};

static const char *const lang_docker[] = {
    "name Dockerfile",
    "comment #",
    "ext dockerfile",
    "file Dockerfile Containerfile",
    "alias dockerfile docker",
    "words keyword FROM RUN CMD LABEL EXPOSE ENV ADD COPY ENTRYPOINT VOLUME USER WORKDIR ARG "
        "ONBUILD STOPSIGNAL HEALTHCHECK SHELL MAINTAINER AS",
    "region variable ${ }",
    "region comment # $ bol",
    "region string \" \" esc=\\",
    "region string ' '",
    "sigil variable $",
    "number",
    "operators =|&;",
    NULL
};

static const char *const lang_diff[] = {
    "name Diff",
    "ext diff patch",
    "alias diff patch",
    "region heading +++ $ bol",
    "region heading --- $ bol",
    "region heading diff $ bol",
    "region heading index $ bol",
    "region preproc @@ $ bol",
    "region comment + $ bol",
    "region string - $ bol",
    NULL
};

const LangDef lang_defs[] = {
    { "C", lang_c },
    { "Python", lang_python },
    { "JavaScript", lang_js },
    { "Shell", lang_shell },
    { "Markdown", lang_markdown },
    { "TypeScript", lang_ts },
    { "PHP", lang_php },
    { "HTML", lang_html },
    { "PHP/HTML", lang_php_html },
    { "XML", lang_xml },
    { "CSS", lang_css },
    { "JSON", lang_json },
    { "YAML", lang_yaml },
    { "TOML", lang_toml },
    { "SQL", lang_sql },
    { "Go", lang_go },
    { "Rust", lang_rust },
    { "Java", lang_java },
    { "C#", lang_csharp },
    { "Ruby", lang_ruby },
    { "Makefile", lang_make },
    { "Dockerfile", lang_docker },
    { "Diff", lang_diff },
};

const size_t lang_ndefs = sizeof lang_defs / sizeof lang_defs[0];
