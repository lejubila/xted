#include "commands.h"
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
    { "save", "Salva", "ctrl+s", editor_cmd_save, 0, 0 },
    { "save_as", "Salva con nome", "alt+s", editor_cmd_save_as, 0, 0 },
    { "save_all", "Salva tutto", "alt+a", editor_cmd_save_all, 0, 0 },
    { "open", "Apri file", "ctrl+o", editor_cmd_open, 0, 0 },
    { "quick_open", "Apri rapido", "ctrl+p", editor_cmd_quick_open, 0, 0 },
    { "buffers", "Elenco buffer", "ctrl+e", editor_cmd_buffers, 0, 0 },
    { "close_buffer", "Chiudi buffer", "ctrl+w", editor_cmd_close_buffer, 0, 0 },
    { "next_buffer", "Buffer successivo", "alt+.", editor_cmd_next_buffer, 0, 0 },
    { "prev_buffer", "Buffer precedente", "alt+,", editor_cmd_prev_buffer, 0, 0 },
    { "quit", "Esci", "ctrl+q", editor_cmd_quit, 0, 0 },
    { "split_v", "Dividi pannello in verticale", "alt+\\", editor_cmd_split_v, 0, 0 },
    { "split_h", "Dividi pannello in orizzontale", "alt+-", editor_cmd_split_h, 0, 0 },
    { "close_win", "Chiudi pannello", "alt+q", editor_cmd_close_win, 0, 0 },
    { "next_win", "Pannello successivo", "alt+o", editor_cmd_next_win, 0, 0 },
    { "prev_win", "Pannello precedente", "alt+left", editor_cmd_prev_win, 0, 0 },
    { "toggle_tree", "Mostra/nascondi browser dei file", "ctrl+b", editor_cmd_toggle_tree, 0, 0 },
    { "find", "Cerca", "ctrl+f", c_find, 0, 0 },
    { "replace", "Sostituisci", "ctrl+r", c_replace, 1, 0 },
    { "find_files", "Cerca nei file", "alt+f", find_files_open, 0, 0 },
    { "find_next", "Occorrenza successiva", "f3", c_next, 0, 0 },
    { "find_prev", "Occorrenza precedente", "shift+f3", c_prev, 0, 0 },
    { "goto_line", "Vai alla riga", "ctrl+g", find_goto, 0, 0 },
    { "undo", "Annulla", "ctrl+z", edit_undo, 1, 0 },
    { "redo", "Ripeti", "ctrl+y", edit_redo, 1, 0 },
    { "select_all", "Seleziona tutto", "ctrl+a", edit_select_all, 0, 0 },
    { "select_line", "Seleziona riga", "alt+l", edit_select_line, 0, 0 },
    { "select_word", "Seleziona parola", "alt+w", edit_select_word, 0, 0 },
    { "copy", "Copia", "ctrl+c", edit_copy, 0, 0 },
    { "cut", "Taglia", "ctrl+x", edit_cut, 1, 0 },
    { "paste", "Incolla", "ctrl+v", edit_paste, 1, 0 },
    { "paste_older", "Incolla la voce precedente", "alt+v", edit_paste_older, 1, 0 },
    { "delete_lines", "Elimina righe", "ctrl+k", edit_delete_lines, 1, 0 },
    { "join_lines", "Unisci righe", "ctrl+j", edit_join_lines, 1, 0 },
    { "move_up", "Sposta riga in su", "alt+up", c_move_up, 1, 1 },
    { "move_down", "Sposta riga in giu", "alt+down", c_move_down, 1, 1 },
    { "duplicate_up", "Duplica riga sopra", "", c_dup_up, 1, 1 },
    { "duplicate_down", "Duplica riga sotto", "alt+y", c_dup_down, 1, 1 },
    { "comment", "Commenta/decommenta", "alt+/", edit_toggle_comment, 1, 0 },
    { "filter", "Filtra con un comando", "alt+k", edit_filter, 1, 0 },
    { "run", "Esegui comando", "alt+r", run_prompt, 0, 0 },
    { "run_again", "Ripeti ultimo comando", "f5", run_again, 0, 0 },
    { "run_stop", "Interrompi comando", "shift+f5", run_stop, 0, 0 },
    { "next_error", "Errore successivo", "f8", c_next_err, 0, 0 },
    { "prev_error", "Errore precedente", "shift+f8", c_prev_err, 0, 0 },
    { "cursor_next", "Aggiungi cursore sulla prossima occorrenza", "ctrl+d", cur_add_next, 0, 0 },
    { "cursor_all", "Aggiungi cursori su tutte le occorrenze", "alt+d", cur_add_all, 0, 0 },
    { "cursor_above", "Aggiungi cursore sopra", "alt+shift+up", c_cur_up, 0, 1 },
    { "cursor_below", "Aggiungi cursore sotto", "alt+shift+down", c_cur_down, 0, 1 },
    { "fold", "Piega/espandi blocco", "alt+z", fold_toggle, 0, 0 },
    { "fold_all", "Piega tutto", "", fold_all, 0, 0 },
    { "unfold_all", "Espandi tutto", "", unfold_all, 0, 0 },
    { "git_refresh", "Aggiorna gli indicatori Git", "", editor_cmd_git, 0, 0 },
    { "goto_tag", "Vai alla definizione", "f12", c_tag, 0, 0 },
    { "match", "Parentesi corrispondente", "alt+m", editor_cmd_match, 0, 0 },
    { "theme", "Cambia tema", "alt+t", editor_cmd_theme, 0, 0 },
    { "palette", "Command palette", "alt+x", editor_cmd_palette, 0, 0 },
    { "redraw", "Ridisegna lo schermo", "ctrl+l", editor_cmd_redraw, 0, 0 },
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
