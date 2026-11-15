#ifndef XTED_EDITORCONFIG_H
#define XTED_EDITORCONFIG_H

/* The .editorconfig settings that apply to a file. -1 means "not set". */
typedef struct {
    int expand;             /* indent_style: 1 space, 0 tab */
    int size;               /* indent_size: spaces of a level; 0 for "tab" */
    int tab_width;          /* tab_width */
    int trim;               /* trim_trailing_whitespace */
    int final_nl;           /* insert_final_newline */
} EcProps;

/* Reads the .editorconfig files from the directory of path upward (up to one with root = true)
 * and fills out with what they say for that file; the nearest file and the last matching
 * section win. Returns the number of files read. */
int editorconfig_lookup(const char *path, EcProps *out);

/* Does the EditorConfig glob match name (a path relative to the directory of the file that holds
 * the section)? A glob with no slash matches at any depth. Exposed for the tests. */
int editorconfig_match(const char *glob, const char *name);

#endif
