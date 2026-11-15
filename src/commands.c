#include "commands.h"

#include <stdio.h>
#include "cursors.h"
#include "edit.h"
#include "find.h"
#include "i18n.h"
#include "fold.h"
#include "run.h"

static void c_find(Editor *e) { find_open(e, 0); }
static void c_replace(Editor *e) { find_open(e, 1); }
static void c_next(Editor *e) { find_next(e, 1); }
static void c_prev(Editor *e) { find_next(e, 0); }
static void c_move_up(Editor *e) { edit_move_lines(e, 0); }
static void c_move_down(Editor *e) { edit_move_lines(e, 1); }
static void c_dup_up(Editor *e) { edit_duplicate_lines(e, 0); }
static void c_dup_down(Editor *e) { edit_duplicate_lines(e, 1); }
static void c_tag(Editor *e) { find_tag(e); }
static void c_cur_up(Editor *e) { cur_add_vertical(e, 0); }
static void c_cur_down(Editor *e) { cur_add_vertical(e, 1); }
static void c_next_err(Editor *e) { run_next_error(e); }
static void c_prev_err(Editor *e) { run_prev_error(e); }

const Command commands[] = {
    { "save", "Save", "ctrl+s", editor_cmd_save, 0, 0, G_FILE, ST_NONE },
    { "save_as", "Save as", "alt+s", editor_cmd_save_as, 0, 0, G_FILE, ST_NONE },
    { "save_all", "Save all", "alt+a", editor_cmd_save_all, 0, 0, G_FILE, ST_NONE },
    { "open", "Open file", "ctrl+o", editor_cmd_open, 0, 0, G_FILE, ST_NONE },
    { "quick_open", "Quick open", "ctrl+p", editor_cmd_quick_open, 0, 0, G_FILE, ST_NONE },
    { "quit", "Quit", "ctrl+q", editor_cmd_quit, 0, 0, G_FILE, ST_NONE },
    { "buffers", "List buffers", "ctrl+e", editor_cmd_buffers, 0, 0, G_BUFFERS, ST_NONE },
    { "close_buffer", "Close buffer", "ctrl+w", editor_cmd_close_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "next_buffer", "Next buffer", "alt+.", editor_cmd_next_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "prev_buffer", "Previous buffer", "alt+,", editor_cmd_prev_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "split_v", "Split pane vertically", "alt+\\", editor_cmd_split_v, 0, 0, G_BUFFERS, ST_NONE },
    { "split_h", "Split pane horizontally", "alt+-", editor_cmd_split_h, 0, 0, G_BUFFERS, ST_NONE },
    { "resize_mode", "Resize panes", "alt+e", editor_cmd_resize_mode, 0, 0, G_BUFFERS, ST_NONE },
    { "equalize_wins", "Equalize panes", "", editor_cmd_equalize, 0, 0, G_BUFFERS, ST_NONE },
    { "close_win", "Close pane", "alt+q", editor_cmd_close_win, 0, 0, G_BUFFERS, ST_NONE },
    { "next_win", "Next pane", "alt+o", editor_cmd_next_win, 0, 0, G_BUFFERS, ST_NONE },
    { "prev_win", "Previous pane", "alt+left", editor_cmd_prev_win, 0, 0, G_BUFFERS, ST_NONE },
    { "toggle_tree", "Show/hide file browser", "ctrl+b", editor_cmd_toggle_tree, 0, 0, G_BUFFERS, ST_NONE },
    { "find", "Find", "ctrl+f", c_find, 0, 0, G_SEARCH, ST_NONE },
    { "replace", "Replace", "ctrl+r", c_replace, 1, 0, G_SEARCH, ST_NONE },
    { "find_files", "Find in files", "alt+f", find_files_open, 0, 0, G_SEARCH, ST_NONE },
    { "find_next", "Next match", "f3", c_next, 0, 0, G_SEARCH, ST_NONE },
    { "find_prev", "Previous match", "shift+f3", c_prev, 0, 0, G_SEARCH, ST_NONE },
    { "goto_line", "Go to line", "ctrl+g", find_goto, 0, 0, G_SEARCH, ST_NONE },
    { "symbols", "Go to symbol", "ctrl+t", editor_cmd_symbols, 0, 0, G_SEARCH, ST_NONE },
    { "goto_tag", "Go to definition", "f12", c_tag, 0, 0, G_SEARCH, ST_NONE },
    { "match", "Matching bracket", "alt+m", editor_cmd_match, 0, 0, G_SEARCH, ST_NONE },
    { "undo", "Undo", "ctrl+z", edit_undo, 1, 0, G_EDIT, ST_NONE },
    { "redo", "Redo", "ctrl+y", edit_redo, 1, 0, G_EDIT, ST_NONE },
    { "select_all", "Select all", "ctrl+a", edit_select_all, 0, 0, G_EDIT, ST_NONE },
    { "select_line", "Select line", "alt+l", edit_select_line, 0, 0, G_EDIT, ST_NONE },
    { "select_word", "Select word", "alt+w", edit_select_word, 0, 0, G_EDIT, ST_NONE },
    { "copy", "Copy", "ctrl+c", edit_copy, 0, 0, G_EDIT, ST_NONE },
    { "cut", "Cut", "ctrl+x", edit_cut, 1, 0, G_EDIT, ST_NONE },
    { "paste", "Paste", "ctrl+v", edit_paste, 1, 0, G_EDIT, ST_NONE },
    { "paste_older", "Paste previous entry", "alt+v", edit_paste_older, 1, 0, G_EDIT, ST_NONE },
    { "delete_lines", "Delete lines", "ctrl+k", edit_delete_lines, 1, 0, G_EDIT, ST_NONE },
    { "join_lines", "Join lines", "ctrl+j", edit_join_lines, 1, 0, G_EDIT, ST_NONE },
    { "move_up", "Move line up", "alt+up", c_move_up, 1, 1, G_EDIT, ST_NONE },
    { "move_down", "Move line down", "alt+down", c_move_down, 1, 1, G_EDIT, ST_NONE },
    { "duplicate_up", "Duplicate line above", "", c_dup_up, 1, 1, G_EDIT, ST_NONE },
    { "duplicate_down", "Duplicate line below", "alt+y", c_dup_down, 1, 1, G_EDIT, ST_NONE },
    { "comment", "Toggle comment", "alt+/", edit_toggle_comment, 1, 0, G_EDIT, ST_NONE },
    { "filter", "Filter through a command", "alt+k", edit_filter, 1, 0, G_EDIT, ST_NONE },
    { "indent_to_spaces", "Convert indentation to spaces", "", editor_cmd_indent_to_spaces, 1, 0, G_EDIT, ST_NONE },
    { "indent_to_tabs", "Convert indentation to tabs", "", editor_cmd_indent_to_tabs, 1, 0, G_EDIT, ST_NONE },
    { "cursor_next", "Add cursor at next occurrence", "ctrl+d", cur_add_next, 0, 0, G_BLOCKS, ST_NONE },
    { "cursor_all", "Add cursors at all occurrences", "alt+d", cur_add_all, 0, 0, G_BLOCKS, ST_NONE },
    { "cursor_above", "Add cursor above", "alt+shift+up", c_cur_up, 0, 1, G_BLOCKS, ST_NONE },
    { "cursor_below", "Add cursor below", "alt+shift+down", c_cur_down, 0, 1, G_BLOCKS, ST_NONE },
    { "fold", "Fold/unfold block", "alt+z", fold_toggle, 0, 0, G_BLOCKS, ST_NONE },
    { "fold_all", "Fold all", "", fold_all, 0, 0, G_BLOCKS, ST_NONE },
    { "unfold_all", "Unfold all", "", unfold_all, 0, 0, G_BLOCKS, ST_NONE },
    { "run", "Run command", "alt+r", run_prompt, 0, 0, G_RUN, ST_NONE },
    { "run_again", "Repeat last command", "f5", run_again, 0, 0, G_RUN, ST_NONE },
    { "run_stop", "Stop command", "shift+f5", run_stop, 0, 0, G_RUN, ST_NONE },
    { "next_error", "Next error", "f8", c_next_err, 0, 0, G_RUN, ST_NONE },
    { "prev_error", "Previous error", "shift+f8", c_prev_err, 0, 0, G_RUN, ST_NONE },
    { "git_refresh", "Refresh Git markers", "", editor_cmd_git, 0, 0, G_RUN, ST_NONE },
    { "palette", "Command palette", "alt+x", editor_cmd_palette, 0, 0, G_OTHER, ST_NONE },
    { "redraw", "Redraw screen", "ctrl+l", editor_cmd_redraw, 0, 0, G_OTHER, ST_NONE },
    { "toggle_mouse", "Toggle mouse", "", editor_cmd_toggle_mouse, 0, 0, G_SETTINGS, ST_MOUSE },
    { "toggle_autosuggest", "Toggle autosuggest", "", editor_cmd_toggle_autosuggest, 0, 0, G_SETTINGS, ST_SUGGEST },
    { "toggle_autopair", "Toggle auto-pairs", "", editor_cmd_toggle_autopair, 0, 0, G_SETTINGS, ST_PAIR },
    { "toggle_git", "Toggle Git markers", "", editor_cmd_toggle_git, 0, 0, G_SETTINGS, ST_GIT },
    { "toggle_clipboard", "Toggle system clipboard", "", editor_cmd_toggle_clipboard, 0, 0, G_SETTINGS, ST_CLIP },
    { "indent_tabs", "Indentation: tab", "", editor_cmd_indent_tabs, 0, 0, G_SETTINGS, ST_TABS },
    { "indent_spaces_2", "Indentation: 2 spaces", "", editor_cmd_indent_spaces2, 0, 0, G_SETTINGS, ST_SP2 },
    { "indent_spaces_4", "Indentation: 4 spaces", "", editor_cmd_indent_spaces4, 0, 0, G_SETTINGS, ST_SP4 },
    { "indent_spaces_8", "Indentation: 8 spaces", "", editor_cmd_indent_spaces8, 0, 0, G_SETTINGS, ST_SP8 },
    { "indent_detect", "Detect indentation style", "", editor_cmd_indent_detect, 0, 0, G_SETTINGS, ST_NONE },
    { "theme", "Change theme", "alt+t", editor_cmd_theme, 0, 0, G_SETTINGS, ST_THEME },
    { "language", "Language", "", editor_cmd_language, 0, 0, G_SETTINGS, ST_LANG },
    { "save_settings", "Save settings…", "", editor_cmd_save_settings, 0, 0, G_SETTINGS, ST_NONE },
};

const char *const command_groups[G_COUNT] = {
    "File", "Buffers and panes", "Search and go to", "Edit", "Cursors and blocks", "Run", "Other",
    "Settings",
};

const size_t ncommands = sizeof commands / sizeof commands[0];

void command_run(Editor *e, int idx)
{
    const Command *c;

    if (idx < 0 || (size_t)idx >= ncommands)
        return;
    c = &commands[idx];
    if (c->modifies && e->doc->readonly) {
        editor_set_msg(e, MSG_TTL_MS, "File in sola lettura (Alt+S per salvare con un altro nome)");
        return;
    }
    buf_undo_close(e->buf);
    if (c->fn != edit_paste_older)
        e->paste_valid = 0;
    c->fn(e);
    if (!c->vertical)
        e->want_col = editor_cursor_col(e);
}

int command_state(const Editor *e, int idx, char *val, size_t n)
{
    int on;

    switch (commands[idx].state) {
    case ST_MOUSE: on = e->mouse_on; break;
    case ST_SUGGEST: on = e->doc->autosuggest; break;
    case ST_PAIR: on = e->doc->autopair; break;
    case ST_GIT: on = e->cfg.git != 0; break;
    case ST_CLIP: on = e->clipboard_osc52; break;
    case ST_TABS: on = !e->doc->expand_tab; break;
    case ST_SP2: on = e->doc->expand_tab && e->doc->indent_w == 2; break;
    case ST_SP4: on = e->doc->expand_tab && e->doc->indent_w == 4; break;
    case ST_SP8: on = e->doc->expand_tab && e->doc->indent_w == 8; break;
    case ST_THEME:
        snprintf(val, n, "%s", e->theme->name);
        return 2;
    case ST_LANG:
        snprintf(val, n, "%s", i18n_name());
        return 2;
    default:
        val[0] = '\0';
        return -1;
    }
    if (commands[idx].state >= ST_TABS)
        snprintf(val, n, "%s", on ? tr("active") : "");
    else
        snprintf(val, n, "%s", on ? tr("on") : tr("off"));
    return on;
}
