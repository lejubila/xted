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
    { "save", N_("Save"), "ctrl+s", editor_cmd_save, 0, 0, G_FILE, ST_NONE },
    { "save_as", N_("Save as"), "alt+s", editor_cmd_save_as, 0, 0, G_FILE, ST_NONE },
    { "save_all", N_("Save all"), "alt+a", editor_cmd_save_all, 0, 0, G_FILE, ST_NONE },
    { "open", N_("Open file"), "ctrl+o", editor_cmd_open, 0, 0, G_FILE, ST_NONE },
    { "quick_open", N_("Quick open"), "ctrl+p", editor_cmd_quick_open, 0, 0, G_FILE, ST_NONE },
    { "quit", N_("Quit"), "ctrl+q", editor_cmd_quit, 0, 0, G_FILE, ST_NONE },
    { "buffers", N_("List buffers"), "ctrl+e", editor_cmd_buffers, 0, 0, G_BUFFERS, ST_NONE },
    { "close_buffer", N_("Close buffer"), "ctrl+w", editor_cmd_close_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "next_buffer", N_("Next buffer"), "alt+.", editor_cmd_next_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "prev_buffer", N_("Previous buffer"), "alt+,", editor_cmd_prev_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "split_v", N_("Split pane vertically"), "alt+\\", editor_cmd_split_v, 0, 0, G_BUFFERS, ST_NONE },
    { "split_h", N_("Split pane horizontally"), "alt+-", editor_cmd_split_h, 0, 0, G_BUFFERS, ST_NONE },
    { "resize_mode", N_("Resize panes"), "alt+e", editor_cmd_resize_mode, 0, 0, G_BUFFERS, ST_NONE },
    { "equalize_wins", N_("Equalize panes"), "", editor_cmd_equalize, 0, 0, G_BUFFERS, ST_NONE },
    { "close_win", N_("Close pane"), "alt+q", editor_cmd_close_win, 0, 0, G_BUFFERS, ST_NONE },
    { "next_win", N_("Next pane"), "alt+o", editor_cmd_next_win, 0, 0, G_BUFFERS, ST_NONE },
    { "prev_win", N_("Previous pane"), "alt+left", editor_cmd_prev_win, 0, 0, G_BUFFERS, ST_NONE },
    { "toggle_tree", N_("Show/hide file browser"), "ctrl+b", editor_cmd_toggle_tree, 0, 0, G_BUFFERS, ST_NONE },
    { "find", N_("Find"), "ctrl+f", c_find, 0, 0, G_SEARCH, ST_NONE },
    { "replace", N_("Replace"), "ctrl+r", c_replace, 1, 0, G_SEARCH, ST_NONE },
    { "find_files", N_("Find in files"), "alt+f", find_files_open, 0, 0, G_SEARCH, ST_NONE },
    { "find_next", N_("Next match"), "f3", c_next, 0, 0, G_SEARCH, ST_NONE },
    { "find_prev", N_("Previous match"), "shift+f3", c_prev, 0, 0, G_SEARCH, ST_NONE },
    { "goto_line", N_("Go to line"), "ctrl+g", find_goto, 0, 0, G_SEARCH, ST_NONE },
    { "symbols", N_("Go to symbol"), "ctrl+t", editor_cmd_symbols, 0, 0, G_SEARCH, ST_NONE },
    { "goto_tag", N_("Go to definition"), "f12", c_tag, 0, 0, G_SEARCH, ST_NONE },
    { "match", N_("Matching bracket"), "alt+m", editor_cmd_match, 0, 0, G_SEARCH, ST_NONE },
    { "undo", N_("Undo"), "ctrl+z", edit_undo, 1, 0, G_EDIT, ST_NONE },
    { "redo", N_("Redo"), "ctrl+y", edit_redo, 1, 0, G_EDIT, ST_NONE },
    { "select_all", N_("Select all"), "ctrl+a", edit_select_all, 0, 0, G_EDIT, ST_NONE },
    { "select_line", N_("Select line"), "alt+l", edit_select_line, 0, 0, G_EDIT, ST_NONE },
    { "select_word", N_("Select word"), "alt+w", edit_select_word, 0, 0, G_EDIT, ST_NONE },
    { "copy", N_("Copy"), "ctrl+c", edit_copy, 0, 0, G_EDIT, ST_NONE },
    { "cut", N_("Cut"), "ctrl+x", edit_cut, 1, 0, G_EDIT, ST_NONE },
    { "paste", N_("Paste"), "ctrl+v", edit_paste, 1, 0, G_EDIT, ST_NONE },
    { "paste_older", N_("Paste previous entry"), "alt+v", edit_paste_older, 1, 0, G_EDIT, ST_NONE },
    { "delete_lines", N_("Delete lines"), "ctrl+k", edit_delete_lines, 1, 0, G_EDIT, ST_NONE },
    { "join_lines", N_("Join lines"), "ctrl+j", edit_join_lines, 1, 0, G_EDIT, ST_NONE },
    { "move_up", N_("Move line up"), "alt+up", c_move_up, 1, 1, G_EDIT, ST_NONE },
    { "move_down", N_("Move line down"), "alt+down", c_move_down, 1, 1, G_EDIT, ST_NONE },
    { "duplicate_up", N_("Duplicate line above"), "", c_dup_up, 1, 1, G_EDIT, ST_NONE },
    { "duplicate_down", N_("Duplicate line below"), "alt+y", c_dup_down, 1, 1, G_EDIT, ST_NONE },
    { "comment", N_("Toggle comment"), "alt+/", edit_toggle_comment, 1, 0, G_EDIT, ST_NONE },
    { "filter", N_("Filter through a command"), "alt+k", edit_filter, 1, 0, G_EDIT, ST_NONE },
    { "indent_to_spaces", N_("Convert indentation to spaces"), "", editor_cmd_indent_to_spaces, 1, 0, G_EDIT, ST_NONE },
    { "indent_to_tabs", N_("Convert indentation to tabs"), "", editor_cmd_indent_to_tabs, 1, 0, G_EDIT, ST_NONE },
    { "cursor_next", N_("Add cursor at next occurrence"), "ctrl+d", cur_add_next, 0, 0, G_BLOCKS, ST_NONE },
    { "cursor_all", N_("Add cursors at all occurrences"), "alt+d", cur_add_all, 0, 0, G_BLOCKS, ST_NONE },
    { "cursor_above", N_("Add cursor above"), "alt+shift+up", c_cur_up, 0, 1, G_BLOCKS, ST_NONE },
    { "cursor_below", N_("Add cursor below"), "alt+shift+down", c_cur_down, 0, 1, G_BLOCKS, ST_NONE },
    { "fold", N_("Fold/unfold block"), "alt+z", fold_toggle, 0, 0, G_BLOCKS, ST_NONE },
    { "fold_all", N_("Fold all"), "", fold_all, 0, 0, G_BLOCKS, ST_NONE },
    { "unfold_all", N_("Unfold all"), "", unfold_all, 0, 0, G_BLOCKS, ST_NONE },
    { "run", N_("Run command"), "alt+r", run_prompt, 0, 0, G_RUN, ST_NONE },
    { "run_again", N_("Repeat last command"), "f5", run_again, 0, 0, G_RUN, ST_NONE },
    { "run_stop", N_("Stop command"), "shift+f5", run_stop, 0, 0, G_RUN, ST_NONE },
    { "next_error", N_("Next error"), "f8", c_next_err, 0, 0, G_RUN, ST_NONE },
    { "prev_error", N_("Previous error"), "shift+f8", c_prev_err, 0, 0, G_RUN, ST_NONE },
    { "git_refresh", N_("Refresh Git markers"), "", editor_cmd_git, 0, 0, G_RUN, ST_NONE },
    { "palette", N_("Command palette"), "alt+x", editor_cmd_palette, 0, 0, G_OTHER, ST_NONE },
    { "redraw", N_("Redraw screen"), "ctrl+l", editor_cmd_redraw, 0, 0, G_OTHER, ST_NONE },
    { "toggle_mouse", N_("Toggle mouse"), "", editor_cmd_toggle_mouse, 0, 0, G_SETTINGS, ST_MOUSE },
    { "toggle_autosuggest", N_("Toggle autosuggest"), "", editor_cmd_toggle_autosuggest, 0, 0, G_SETTINGS, ST_SUGGEST },
    { "toggle_autopair", N_("Toggle auto-pairs"), "", editor_cmd_toggle_autopair, 0, 0, G_SETTINGS, ST_PAIR },
    { "toggle_git", N_("Toggle Git markers"), "", editor_cmd_toggle_git, 0, 0, G_SETTINGS, ST_GIT },
    { "toggle_clipboard", N_("Toggle system clipboard"), "", editor_cmd_toggle_clipboard, 0, 0, G_SETTINGS, ST_CLIP },
    { "indent_tabs", N_("Indentation: tab"), "", editor_cmd_indent_tabs, 0, 0, G_SETTINGS, ST_TABS },
    { "indent_spaces_2", N_("Indentation: 2 spaces"), "", editor_cmd_indent_spaces2, 0, 0, G_SETTINGS, ST_SP2 },
    { "indent_spaces_4", N_("Indentation: 4 spaces"), "", editor_cmd_indent_spaces4, 0, 0, G_SETTINGS, ST_SP4 },
    { "indent_spaces_8", N_("Indentation: 8 spaces"), "", editor_cmd_indent_spaces8, 0, 0, G_SETTINGS, ST_SP8 },
    { "indent_detect", N_("Detect indentation style"), "", editor_cmd_indent_detect, 0, 0, G_SETTINGS, ST_NONE },
    { "theme", N_("Change theme"), "alt+t", editor_cmd_theme, 0, 0, G_SETTINGS, ST_THEME },
    { "language", N_("Language"), "", editor_cmd_language, 0, 0, G_SETTINGS, ST_LANG },
    { "save_settings", N_("Save settings…"), "", editor_cmd_save_settings, 0, 0, G_SETTINGS, ST_NONE },
};

const char *const command_groups[G_COUNT] = {
    N_("File"), N_("Buffers and panes"), N_("Search and go to"), N_("Edit"), N_("Cursors and blocks"), N_("Run"), N_("Other"),
    N_("Settings"),
};

const size_t ncommands = sizeof commands / sizeof commands[0];

void command_run(Editor *e, int idx)
{
    const Command *c;

    if (idx < 0 || (size_t)idx >= ncommands)
        return;
    c = &commands[idx];
    if (c->modifies && e->doc->readonly) {
        editor_set_msg(e, MSG_TTL_MS, "%s", tr("File is read-only (Alt+S to save under another name)"));
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
