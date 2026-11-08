#include "commands.h"
#include "cursors.h"
#include "edit.h"
#include "find.h"
#include "fold.h"

static void c_find(Editor *e) { find_open(e, 0); }
static void c_replace(Editor *e) { find_open(e, 1); }
static void c_next(Editor *e) { find_next(e, 1); }
static void c_prev(Editor *e) { find_next(e, 0); }
static void c_move_up(Editor *e) { edit_move_lines(e, 0); }
static void c_move_down(Editor *e) { edit_move_lines(e, 1); }
static void c_dup_up(Editor *e) { edit_duplicate_lines(e, 0); }
static void c_dup_down(Editor *e) { edit_duplicate_lines(e, 1); }

static void c_cur_up(Editor *e) { cur_add_vertical(e, 0); }
static void c_cur_down(Editor *e) { cur_add_vertical(e, 1); }

const Command commands[] = {
    { "Salva", "Ctrl+S", editor_cmd_save, 0 },
    { "Salva con nome", "Alt+S", editor_cmd_save_as, 0 },
    { "Salva tutto", "Alt+A", editor_cmd_save_all, 0 },
    { "Apri file", "Ctrl+O", editor_cmd_open, 0 },
    { "Apri rapido", "Ctrl+P", editor_cmd_quick_open, 0 },
    { "Elenco buffer", "Ctrl+E", editor_cmd_buffers, 0 },
    { "Chiudi buffer", "Ctrl+W", editor_cmd_close_buffer, 0 },
    { "Buffer successivo", "Alt+.", editor_cmd_next_buffer, 0 },
    { "Buffer precedente", "Alt+,", editor_cmd_prev_buffer, 0 },
    { "Esci", "Ctrl+Q", editor_cmd_quit, 0 },
    { "Dividi pannello in verticale", "Alt+\\", editor_cmd_split_v, 0 },
    { "Dividi pannello in orizzontale", "Alt+-", editor_cmd_split_h, 0 },
    { "Chiudi pannello", "Alt+Q", editor_cmd_close_win, 0 },
    { "Pannello successivo", "Alt+O", editor_cmd_next_win, 0 },
    { "Pannello precedente", "Alt+Sinistra", editor_cmd_prev_win, 0 },
    { "Mostra/nascondi browser dei file", "Ctrl+B", editor_cmd_toggle_tree, 0 },
    { "Cerca", "Ctrl+F", c_find, 0 },
    { "Sostituisci", "Ctrl+R", c_replace, 1 },
    { "Cerca nei file", "Alt+F", find_files_open, 0 },
    { "Occorrenza successiva", "F3", c_next, 0 },
    { "Occorrenza precedente", "Maiusc+F3", c_prev, 0 },
    { "Vai alla riga", "Ctrl+G", find_goto, 0 },
    { "Annulla", "Ctrl+Z", edit_undo, 1 },
    { "Ripeti", "Ctrl+Y", edit_redo, 1 },
    { "Seleziona tutto", "Ctrl+A", edit_select_all, 0 },
    { "Seleziona riga", "Alt+L", edit_select_line, 0 },
    { "Seleziona parola", "Alt+W", edit_select_word, 0 },
    { "Elimina righe", "Ctrl+K", edit_delete_lines, 1 },
    { "Unisci righe", "Ctrl+J", edit_join_lines, 1 },
    { "Sposta riga in su", "Alt+Su", c_move_up, 1 },
    { "Sposta riga in giu", "Alt+Giu", c_move_down, 1 },
    { "Duplica riga sopra", "Alt+Maiusc+Su", c_dup_up, 1 },
    { "Duplica riga sotto", "Alt+Maiusc+Giu", c_dup_down, 1 },
    { "Commenta/decommenta", "Alt+/", edit_toggle_comment, 1 },
    { "Aggiungi cursore sulla prossima occorrenza", "Ctrl+D", cur_add_next, 0 },
    { "Aggiungi cursori su tutte le occorrenze", "Alt+D", cur_add_all, 0 },
    { "Aggiungi cursore sopra", "Ctrl+Alt+Su", c_cur_up, 0 },
    { "Aggiungi cursore sotto", "Ctrl+Alt+Giu", c_cur_down, 0 },
    { "Piega/espandi blocco", "Alt+Z", fold_toggle, 0 },
    { "Piega tutto", "", fold_all, 0 },
    { "Espandi tutto", "", unfold_all, 0 },
    { "Parentesi corrispondente", "Alt+M", editor_cmd_match, 0 },
    { "Cambia tema", "Alt+T", editor_cmd_theme, 0 },
};

const size_t ncommands = sizeof commands / sizeof commands[0];
