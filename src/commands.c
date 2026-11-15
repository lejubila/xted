#include "commands.h"

#include <stdio.h>
#include "cursors.h"
#include "edit.h"
#include "find.h"
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
    { "save", "Salva", "ctrl+s", editor_cmd_save, 0, 0, G_FILE, ST_NONE },
    { "save_as", "Salva con nome", "alt+s", editor_cmd_save_as, 0, 0, G_FILE, ST_NONE },
    { "save_all", "Salva tutto", "alt+a", editor_cmd_save_all, 0, 0, G_FILE, ST_NONE },
    { "open", "Apri file", "ctrl+o", editor_cmd_open, 0, 0, G_FILE, ST_NONE },
    { "quick_open", "Apri rapido", "ctrl+p", editor_cmd_quick_open, 0, 0, G_FILE, ST_NONE },
    { "quit", "Esci", "ctrl+q", editor_cmd_quit, 0, 0, G_FILE, ST_NONE },
    { "buffers", "Elenco buffer", "ctrl+e", editor_cmd_buffers, 0, 0, G_BUFFERS, ST_NONE },
    { "close_buffer", "Chiudi buffer", "ctrl+w", editor_cmd_close_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "next_buffer", "Buffer successivo", "alt+.", editor_cmd_next_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "prev_buffer", "Buffer precedente", "alt+,", editor_cmd_prev_buffer, 0, 0, G_BUFFERS, ST_NONE },
    { "split_v", "Dividi pannello in verticale", "alt+\\", editor_cmd_split_v, 0, 0, G_BUFFERS, ST_NONE },
    { "split_h", "Dividi pannello in orizzontale", "alt+-", editor_cmd_split_h, 0, 0, G_BUFFERS, ST_NONE },
    { "resize_mode", "Ridimensiona i pannelli", "alt+e", editor_cmd_resize_mode, 0, 0, G_BUFFERS, ST_NONE },
    { "equalize_wins", "Pareggia i pannelli", "", editor_cmd_equalize, 0, 0, G_BUFFERS, ST_NONE },
    { "close_win", "Chiudi pannello", "alt+q", editor_cmd_close_win, 0, 0, G_BUFFERS, ST_NONE },
    { "next_win", "Pannello successivo", "alt+o", editor_cmd_next_win, 0, 0, G_BUFFERS, ST_NONE },
    { "prev_win", "Pannello precedente", "alt+left", editor_cmd_prev_win, 0, 0, G_BUFFERS, ST_NONE },
    { "toggle_tree", "Mostra/nascondi browser dei file", "ctrl+b", editor_cmd_toggle_tree, 0, 0, G_BUFFERS, ST_NONE },
    { "find", "Cerca", "ctrl+f", c_find, 0, 0, G_SEARCH, ST_NONE },
    { "replace", "Sostituisci", "ctrl+r", c_replace, 1, 0, G_SEARCH, ST_NONE },
    { "find_files", "Cerca nei file", "alt+f", find_files_open, 0, 0, G_SEARCH, ST_NONE },
    { "find_next", "Occorrenza successiva", "f3", c_next, 0, 0, G_SEARCH, ST_NONE },
    { "find_prev", "Occorrenza precedente", "shift+f3", c_prev, 0, 0, G_SEARCH, ST_NONE },
    { "goto_line", "Vai alla riga", "ctrl+g", find_goto, 0, 0, G_SEARCH, ST_NONE },
    { "symbols", "Vai al simbolo", "ctrl+t", editor_cmd_symbols, 0, 0, G_SEARCH, ST_NONE },
    { "goto_tag", "Vai alla definizione", "f12", c_tag, 0, 0, G_SEARCH, ST_NONE },
    { "match", "Parentesi corrispondente", "alt+m", editor_cmd_match, 0, 0, G_SEARCH, ST_NONE },
    { "undo", "Annulla", "ctrl+z", edit_undo, 1, 0, G_EDIT, ST_NONE },
    { "redo", "Ripeti", "ctrl+y", edit_redo, 1, 0, G_EDIT, ST_NONE },
    { "select_all", "Seleziona tutto", "ctrl+a", edit_select_all, 0, 0, G_EDIT, ST_NONE },
    { "select_line", "Seleziona riga", "alt+l", edit_select_line, 0, 0, G_EDIT, ST_NONE },
    { "select_word", "Seleziona parola", "alt+w", edit_select_word, 0, 0, G_EDIT, ST_NONE },
    { "copy", "Copia", "ctrl+c", edit_copy, 0, 0, G_EDIT, ST_NONE },
    { "cut", "Taglia", "ctrl+x", edit_cut, 1, 0, G_EDIT, ST_NONE },
    { "paste", "Incolla", "ctrl+v", edit_paste, 1, 0, G_EDIT, ST_NONE },
    { "paste_older", "Incolla la voce precedente", "alt+v", edit_paste_older, 1, 0, G_EDIT, ST_NONE },
    { "delete_lines", "Elimina righe", "ctrl+k", edit_delete_lines, 1, 0, G_EDIT, ST_NONE },
    { "join_lines", "Unisci righe", "ctrl+j", edit_join_lines, 1, 0, G_EDIT, ST_NONE },
    { "move_up", "Sposta riga in su", "alt+up", c_move_up, 1, 1, G_EDIT, ST_NONE },
    { "move_down", "Sposta riga in giu", "alt+down", c_move_down, 1, 1, G_EDIT, ST_NONE },
    { "duplicate_up", "Duplica riga sopra", "", c_dup_up, 1, 1, G_EDIT, ST_NONE },
    { "duplicate_down", "Duplica riga sotto", "alt+y", c_dup_down, 1, 1, G_EDIT, ST_NONE },
    { "comment", "Commenta/decommenta", "alt+/", edit_toggle_comment, 1, 0, G_EDIT, ST_NONE },
    { "filter", "Filtra con un comando", "alt+k", edit_filter, 1, 0, G_EDIT, ST_NONE },
    { "indent_to_spaces", "Converti l'indentazione in spazi", "", editor_cmd_indent_to_spaces, 1, 0, G_EDIT, ST_NONE },
    { "indent_to_tabs", "Converti l'indentazione in tabulatori", "", editor_cmd_indent_to_tabs, 1, 0, G_EDIT, ST_NONE },
    { "cursor_next", "Aggiungi cursore sulla prossima occorrenza", "ctrl+d", cur_add_next, 0, 0, G_BLOCKS, ST_NONE },
    { "cursor_all", "Aggiungi cursori su tutte le occorrenze", "alt+d", cur_add_all, 0, 0, G_BLOCKS, ST_NONE },
    { "cursor_above", "Aggiungi cursore sopra", "alt+shift+up", c_cur_up, 0, 1, G_BLOCKS, ST_NONE },
    { "cursor_below", "Aggiungi cursore sotto", "alt+shift+down", c_cur_down, 0, 1, G_BLOCKS, ST_NONE },
    { "fold", "Piega/espandi blocco", "alt+z", fold_toggle, 0, 0, G_BLOCKS, ST_NONE },
    { "fold_all", "Piega tutto", "", fold_all, 0, 0, G_BLOCKS, ST_NONE },
    { "unfold_all", "Espandi tutto", "", unfold_all, 0, 0, G_BLOCKS, ST_NONE },
    { "run", "Esegui comando", "alt+r", run_prompt, 0, 0, G_RUN, ST_NONE },
    { "run_again", "Ripeti ultimo comando", "f5", run_again, 0, 0, G_RUN, ST_NONE },
    { "run_stop", "Interrompi comando", "shift+f5", run_stop, 0, 0, G_RUN, ST_NONE },
    { "next_error", "Errore successivo", "f8", c_next_err, 0, 0, G_RUN, ST_NONE },
    { "prev_error", "Errore precedente", "shift+f8", c_prev_err, 0, 0, G_RUN, ST_NONE },
    { "git_refresh", "Aggiorna gli indicatori Git", "", editor_cmd_git, 0, 0, G_RUN, ST_NONE },
    { "palette", "Command palette", "alt+x", editor_cmd_palette, 0, 0, G_OTHER, ST_NONE },
    { "redraw", "Ridisegna lo schermo", "ctrl+l", editor_cmd_redraw, 0, 0, G_OTHER, ST_NONE },
    { "toggle_mouse", "Attiva/disattiva il mouse", "", editor_cmd_toggle_mouse, 0, 0, G_SETTINGS, ST_MOUSE },
    { "toggle_autosuggest", "Attiva/disattiva l'autosuggest", "", editor_cmd_toggle_autosuggest, 0, 0, G_SETTINGS, ST_SUGGEST },
    { "toggle_autopair", "Attiva/disattiva le coppie automatiche", "", editor_cmd_toggle_autopair, 0, 0, G_SETTINGS, ST_PAIR },
    { "toggle_git", "Attiva/disattiva gli indicatori Git", "", editor_cmd_toggle_git, 0, 0, G_SETTINGS, ST_GIT },
    { "toggle_clipboard", "Attiva/disattiva gli appunti di sistema", "", editor_cmd_toggle_clipboard, 0, 0, G_SETTINGS, ST_CLIP },
    { "indent_tabs", "Indentazione: tabulatore", "", editor_cmd_indent_tabs, 0, 0, G_SETTINGS, ST_TABS },
    { "indent_spaces_2", "Indentazione: 2 spazi", "", editor_cmd_indent_spaces2, 0, 0, G_SETTINGS, ST_SP2 },
    { "indent_spaces_4", "Indentazione: 4 spazi", "", editor_cmd_indent_spaces4, 0, 0, G_SETTINGS, ST_SP4 },
    { "indent_spaces_8", "Indentazione: 8 spazi", "", editor_cmd_indent_spaces8, 0, 0, G_SETTINGS, ST_SP8 },
    { "indent_detect", "Rileva lo stile di indentazione", "", editor_cmd_indent_detect, 0, 0, G_SETTINGS, ST_NONE },
    { "theme", "Cambia tema", "alt+t", editor_cmd_theme, 0, 0, G_SETTINGS, ST_THEME },
    { "save_settings", "Salva le impostazioni…", "", editor_cmd_save_settings, 0, 0, G_SETTINGS, ST_NONE },
};

const char *const command_groups[G_COUNT] = {
    "File", "Buffer e pannelli", "Cerca e vai", "Modifica", "Cursori e blocchi", "Esegui", "Altro",
    "Impostazioni",
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
    default:
        val[0] = '\0';
        return -1;
    }
    if (commands[idx].state >= ST_TABS)
        snprintf(val, n, "%s", on ? "attiva" : "");
    else
        snprintf(val, n, "%s", on ? "attivo" : "spento");
    return on;
}
