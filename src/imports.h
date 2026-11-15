#ifndef XTED_IMPORTS_H
#define XTED_IMPORTS_H

#include "editor.h"

/* The include, import, use or require a symbol needs, added to the document of the editor where
 * the language wants it. Each function leaves the file alone when it already has what is asked,
 * moves the cursor down when lines go above it, and opens no undo group (the caller does). They
 * return 1 if the text changed. */

/* Adds "#include <hdr>" (local: "#include \"hdr\"") to a C file unless the file already has it:
 * among the includes of its kind (in order if they are in order); a system one before the first
 * local one, a local one after the last system one; else at the top after the comments and the
 * include guard. Returns 1 if a line was added. It does not open an undo group. */
int imp_include(Editor *e, const char *hdr, int local);
/* Adds "import mod" (name NULL) or "from mod import name" (local: a module of the
 * project, which goes into the last group of imports) to a Python file unless it has it: an
 * existing single-line "from mod import ..." is extended, else a new line goes into the first block
 * of imports in order, or after the docstring if there are none. Returns 1 if the text changed. It
 * does not open an undo group. */
int imp_python(Editor *e, const char *mod, const char *name, int local);
/* Adds the import of a Node module to a JavaScript or TypeScript file unless the file has the name
 * already: "import fs from 'fs'" (def 1) or "import { EventEmitter } from 'events'" (def 0), or the
 * require() form in a file that uses it; quotes, semicolons and "import * as" follow what the file does.
 * An existing "import { a } from 'mod'" on one line is extended. stmt gets the text added. Returns 1
 * if the text changed. It does not open an undo group. */
int imp_js(Editor *e, const char *mod, const char *name, int def, char *stmt, size_t n);
/* PHP: adds require_once __DIR__ . '/spec'; (spec relative to the file's folder) after the requires
 * of the file, else after <?php, unless the file already requires or includes it. Returns 1 if a
 * line was added. No undo group. */
int imp_php_require(Editor *e, const char *spec);
/* PHP: adds "use function Fq;" (what "function") or "use const Fq;" (what "const") after the uses
 * of classes, unless it is there or Fq is in the file's own namespace. Returns 1 if added. */
int imp_php_use_fn(Editor *e, const char *what, const char *fq);
/* Adds "use Fq;" to a PHP file unless the class is imported, defined in the file or in its
 * namespace; the block of uses stays sorted. Returns 1 if a line was added. No undo group. */
int imp_php_use(Editor *e, const char *fq);

/* Go: "fmt" into the block import ( ... ), in order in its first group; an import on its own line
 * becomes a block; with none, import "fmt" after the package line. */
int imp_go(Editor *e, const char *path);
/* Rust: use std::collections::HashMap; among the uses, in order; with none, past the //! comments
 * and the #![...] attributes at the top. A use std::collections::{HashMap, ...} counts. */
int imp_rust(Editor *e, const char *path);

#endif
