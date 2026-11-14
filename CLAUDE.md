# ked

Editor di testo da terminale per Linux, scritto in C99 con sola libc POSIX.
Binario unico, avvio sotto 50 ms. `ked` è il nome provvisorio.
Le funzionalità e la roadmap a fasi sono in `SPEC.md`, che è la fonte di verità: non duplicarle qui.

## Vincoli non negoziabili

- C99 (`-std=c99`), con `-D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700` (musl dichiara `realpath` solo con quest'ultimo).
- Nessuna dipendenza oltre la libc POSIX: niente ncurses, terminfo, termcap o librerie esterne.
- Terminale controllato a mano con termios e sequenze ANSI/VT100.
- Un solo binario e un solo `Makefile`, compilabile con `gcc` o `clang`, con link statico opzionale.
- Nessun lavoro in background inutile: il ciclo principale attende input con `poll()`.

## Compilazione

- Flag: `-Wall -Wextra -pedantic`. Il codice deve compilare **senza warning**.
- Un warning si corregge nel codice, non si silenzia con flag.

## Struttura dei sorgenti

| File | Responsabilità |
|---|---|
| `src/main.c` | Argomenti da riga di comando e avvio |
| `src/term.[ch]` | Raw mode, alternate screen, segnali, `poll()`, decodifica dei tasti |
| `src/buffer.[ch]` | Testo (indipendente dalla UI), caricamento e salvataggio, primitive di modifica, helper multi-riga, undo/redo |
| `src/undo.[ch]` | Log delle operazioni e gruppi di undo; non conosce `Buffer` |
| `src/clip.[ch]` | Kill ring e codifica base64/OSC 52 per gli appunti di sistema |
| `src/utf8.[ch]` | Decodifica/codifica UTF-8, `wcwidth` interna |
| `src/render.[ch]` | Rendering differenziale, un solo `write()` per frame |
| `src/editor.[ch]` | Stato dell'editor, ciclo eventi, prompt, movimento, dispatch dei tasti |
| `src/edit.[ch]` | Comandi di modifica: selezione, appunti, operazioni sulle righe, indentazione, autoindent, coppie automatiche (`edit_type`, `edit_backspace_pair`, Invio tra `{}`; chiave `autopair`) |
| `src/syntax.[ch]` | Motore di evidenziazione a regioni (parser delle definizioni, stato per riga, rilevamento del linguaggio, parentesi corrispondenti, linguaggi dell'utente da `syntax/`) |
| `src/langs.[ch]` | Definizioni dei linguaggi incorporate, come dati testuali: un nuovo linguaggio è solo una voce qui (direttive in testa al file, anche `nocase`, le regioni a soli token `END = -` e `embed` per i linguaggi annidati, un livello; `decl/include/bind/attr/synonym/docmember/member/scope` per l'indice dei simboli) |
| `src/theme.[ch]` | Temi colore e codifica SGR con ripiego truecolor → 256 → 16 colori |
| `src/doc.[ch]` | `Doc`: buffer + linguaggio + stato su disco (cambiamenti, sola lettura) di un file aperto; `View` (cursore/scorrimento/selezione) |
| `src/layout.[ch]` | Albero degli split e geometria dei pannelli (quote per mille `Node.frac`), bordi (`layout_border_at/set_size/resize/equalize`); senza terminale |
| `src/fuzzy.[ch]`, `src/picker.[ch]` | Punteggio fuzzy e lista filtrabile (elenco buffer, apertura rapida) |
| `src/files.[ch]` | Scansione dei file del progetto e modello ad albero del file browser |
| `src/session.[ch]` | File di sessione per cartella (`$XDG_STATE_HOME/ked/sessions/`) |
| `src/search.[ch]` | Nucleo della ricerca: `Matcher` (testo o regex POSIX ERE, per riga), ricerca nel buffer, sostituzione, ricerca nei file; senza terminale |
| `src/find.[ch]` | Comandi di ricerca/sostituzione/vai alla riga sull'`Editor`: barra di ricerca come macchina a stati |
| `src/commands.[ch]` | Tabella dei comandi (nome, scorciatoia come testo, funzione): alimenta la command palette (Alt+X) |
| `src/complete.[ch]` | Candidati dell'autosuggest (membri dopo `->` `::` `.`, simboli del file, dei file inclusi e della libreria standard, parole dei buffer aperti, parole chiave, snippet) con firma (mostrata su ogni riga del popup; alla conferma `complete_call_body` inserisce la chiamata con gli argomenti come segnaposto di snippet), e stato del popup; in PHP dopo `use` propone i nomi qualificati delle classi del progetto (`sym_classes`: classmap e PSR-4 di Composer, alias di `config/app.php`) e, scegliendo una classe, `edit_replace_class` aggiunge l'istruzione `use` in ordine; senza terminale |
| `src/symbols.[ch]` | Indice dei simboli di un file (classi, funzioni, metodi, attributi, inclusioni) costruito con i modelli `decl/include/bind/attr` delle definizioni dei linguaggi (`pat_match`), cache per file (`SymCache`, nel `Complete`), tipo di un oggetto e catene `a->b()->c` (`sym_members`, `sym_globals`); senza terminale |
| `src/stdsym.[ch]` | Libreria standard dei linguaggi (PHP, C, C++, Python, JS/TS) come simboli con firme, compressa nel binario e decompressa al primo uso; i dati sono `tools/lib/*.txt` compressi da `tools/packlib.py` in `src/stdsym_data.inc` (generati da `tools/gen_py.py` e `tools/gen_c.py`, gli altri scritti a mano) |
| `src/snippet.[ch]` | Snippet incorporati per linguaggio, analisi del corpo (`$1`, `${2:x}`, `$0`) e sessione dei segnaposti |
| `src/cursors.[ch]` | Multi-cursore: cursori extra dell'`Editor`, `cur_each` esegue un comando a cursore singolo su ciascuno (posizioni tenute come distanza dalla fine del testo), Ctrl+D / Alt+D |
| `src/fold.[ch]` | Folding: `Line.fold` righe nascoste dopo la riga piegata, intervalli per parentesi o indentazione, passaggi sulle righe visibili |
| `src/shell.[ch]` | Processi figli: `shell_start` (in background, stdout+stderr su un pipe), `shell_capture` (stdin da buffer, tempo massimo); niente `Editor` |
| `src/run.[ch]` | Comandi di shell sull'`Editor`: pannello «[Output]» (documento `scratch`), Alt+R/F5, errori F8; il pipe è osservato da `term_watch` |
| `src/errors.[ch]` | Riconoscimento di `file:riga:colonna` nell'output (gcc, tsc, Python); senza terminale |
| `src/keymap.[ch]` | Nomi dei tasti (`ctrl+s`, `alt+shift+up`) e mappa tasto → comando, con i predefiniti presi da `commands[]` |
| `src/config.[ch]` | File `~/.config/ked/config` (chiave = valore, `[linguaggio]`, `[keys]`); non conosce l'`Editor` |
| `src/git.[ch]` | Indicatori Git: `git diff -U0` analizzato in `Line.git`; senza librerie |
| `src/tags.[ch]` | File `tags` di ctags: ricerca (binaria se ordinato) e risoluzione dell'indirizzo; senza terminale |
| `src/mouse.[ch]` | Mouse: `mouse_locate` (cella → tab, browser, picker, riga/byte di un pannello) e `mouse_event` (clic, doppio/triplo clic, trascinamento, rotella, Alt+clic); gli eventi SGR vengono decodificati da `term.c` (`KEY_MOUSE`) |
| `src/indent.[ch]` | `indent_detect`: stile di indentazione (tabulatori o N spazi) dalle prime righe di un testo; senza terminale |
| `src/editorconfig.[ch]` | Lettura di `.editorconfig` (`editorconfig_lookup`: dalla cartella del file verso l'alto, `root = true`; `editorconfig_match`: glob con `*`, `**`, `?`, `[]`, `{a,b}`, `{1..3}`); senza terminale |
| `src/util.[ch]` | `die`, `xmalloc`, stringa dinamica |
| `tests/` | Test automatici |

- Ogni fase aggiunge i moduli previsti da `SPEC.md` (sintassi, comandi, configurazione, …).
- Un modulo non deve dipendere da quelli di livello più alto: `buffer` e `utf8` non conoscono terminale né UI.
- Ogni modifica al testo passa dalle primitive di `buffer.c` (`buf_insert/delete/split/join`) o dagli helper costruiti su di esse, così undo e rappresentazione interna restano isolati. Un comando che modifica il testo apre un gruppo undo (`buf_undo_begin`) e lo chiude a fine comando.
- `Editor` espone i campi `cy, cx, rowoff, …` del **pannello attivo**; gli altri pannelli hanno la loro `View` in `Win`. Cambiare pannello o documento passa da `editor_store_view`/`load_win` (editor.c); `e->buf` e `e->doc` puntano sempre al documento attivo.
- Un comando nuovo si aggiunge a `commands[]` (id inglese stabile, scorciatoia predefinita come testo di configurazione): palette, tasti e `[keys]` lo vedono da lì. In `editor.c` restano cablati solo digitazione, movimenti e selezione; non aggiungere `case` per tasti di comando.
- L'indentazione di un documento (`Doc.tabw` larghezza di un tab disegnato, `expand_tab`, `indent_w` spazi di un livello) si risolve in `apply_cfg` con questa precedenza: comando dell'utente (`Doc.man`, palette «Indentazione: …») > `~/.config/ked/config` (solo i valori scritti: `LangCfg.set`) > `.editorconfig` > stile rilevato nel testo (`Doc.det`, calcolato in `install_doc`) > predefiniti. `edit.c` usa sempre `indent_unit` (un livello) e `indent_w`, mai `tabw`, per inserire o togliere indentazione. - `Doc.tabw`, `expand_tab`, … vengono da `config` per linguaggio (`apply_cfg`); `e->tabw` è lo specchio del documento attivo, per gli altri pannelli il renderer usa `d->tabw`. Con il margine Git (`Doc.gutter`) la larghezza del testo è `win_text_w(w)`, non `w->w`.
- I campi extra del multi-cursore (`Editor.xc`) vengono svuotati cambiando pannello/documento e con undo/redo; i comandi di `edit.c` restano a cursore singolo e `cur_each` li ripete per ogni cursore. `Line.fold`/`Buffer.folded` sono lo stato del folding: dopo un'azione che sposta il cursore, `fold_reveal` riapre ciò che lo nasconde; le funzioni di movimento e rendering contano le righe *visibili* (`fold_next/prev/advance/back/nth/count`), mai `cy + 1`.
- Il mouse passa da `KEY_MOUSE` (`term_mouse_event`) a `mouse_event`; la geometria dello schermo si ricava con `mouse_locate`/`render_tab_at`/`render_picker_height`, non si ricalcola altrove. Per far agire il mouse come un tasto in una lista si usa `editor_key`.
- Il ridimensionamento degli split passa solo da `layout_set_size/layout_resize/layout_equalize` seguiti da `layout_refresh` (mouse: trascinamento di bordi e browser; tastiera: modalità Alt+E, `Editor.resize_mode`). Lo stato di sintassi di una riga può indicare un linguaggio incorporato (bit 15 di `Line.hl`): per i marcatori di commento di una riga usare `syn_comment_at`, non `syn_comment`.
- L'autosuggest dei simboli passa da `complete_open` → `sym_members`/`sym_globals`: per aggiungere un linguaggio bastano le direttive `decl/include/bind/attr/synonym/docmember/member` in `langs.c` e, se serve, una lista in `tools/lib` (poi `tools/packlib.py`). `Buffer.gen` cresce a ogni modifica: le cache di dati derivati lo confrontano. Le classi nominate (`use`, `new`, alias globali di `config/app.php`) si cercano con la mappa PSR-4 di Composer (`vendor/composer/autoload_psr4.php`) e, se manca, nei file del progetto senza `vendor`; i metodi dichiarati in `@method` sopra una classe (facade di Laravel) sono indicizzati dalla direttiva `docmember`, e una facade che ha solo `@see` inoltra ai metodi pubblici delle classi indicate. Le scansioni del progetto (file per cercare le classi, elenco delle classi per `use`) hanno un limite di tempo: `complete_open` chiama `sym_budget` (`COMPLETE_SCAN_MS`), le scansioni (`files_scan_begin/step`) si fermano e riprendono alla chiamata successiva, `sym_busy` lo segnala e `complete_resume`, chiamata da `next_key` quando l'utente non scrive, le prosegue mostrando «Scansione» nella barra di stato; senza `sym_budget` (test) non c'è limite. Il contesto di una parentesi aperta (`sym_call_at`: nome, indice dell'argomento, definizione o chiamata, posizione del tipo) alimenta tre cose: le variabili locali di `sym_locals` (parametri e nomi dichiarati nella funzione che contiene il cursore, euristiche per famiglia di linguaggio: PHP `$x`, Python assegnazioni/`for`/`as`, JS `let/const/var`, C `tipo nome`; `CAND_LOCAL` con bonus di punteggio), i soli tipi dove va il tipo di un parametro (`Gather.types_only`) e il suggerimento della firma (`sym_call_hint` → `Editor.hint`, calcolato in `prepare_frame` solo quando cambiano posizione o testo, mostrato da `status_row`). `sym_outline` elenca le definizioni del documento per «Vai al simbolo» (Ctrl+T, `PK_SYMBOLS`; le posizioni stanno in `Editor.outline`). Ctrl+P legge il progetto con `files_scan_begin/step` (`Editor.fscan`): `files_scan_pump` aggiunge i file al picker a passi di `FILES_STEP_US` quando `next_key` non ha tasti in attesa, `picker_refilter` conserva la voce scelta e `Picker.loading` fa mostrare l'indicatore dal renderer. Il limite di dimensione del binario statico è 440 KB (SPEC.md).
- Lo stato derivato dal testo (evidenziazione) vive in `Line.hl` e vale per le righe `< Buffer.stable`: le `raw_*` di `buffer.c` lo invalidano, nessun altro codice deve farlo.

## Stile del codice

- Indentazione a 4 spazi, niente tab. Righe entro 100 colonne.
- Parentesi graffa di funzione su riga propria; per `if/for/while` sulla stessa riga. Le graffe sono obbligatorie solo quando il corpo ha più istruzioni.
- Nomi in `snake_case`; funzioni pubbliche con prefisso del modulo (`buf_insert`, `term_init`). Tipi in `CamelCase`. Costanti ed enum in `MAIUSCOLO`.
- Funzioni interne `static`. Un header espone solo ciò che serve agli altri moduli.
- Commenti in inglese, pochi, che spiegano il *perché* e non il *cosa*. Messaggi per l'utente in italiano.
- Memoria: `xmalloc`/`xrealloc` (terminano con errore su OOM). Niente VLA. Ogni `malloc` ha un `free` chiaro.
- Il codice chiamato da signal handler deve essere async-signal-safe.

## Comandi

```
make                 # build ottimizzata (./ked)
make STATIC=1        # link statico
make SAN=1           # build con AddressSanitizer e UBSan
make test            # test automatici
make clean
./ked [file]         # esecuzione
```

## Metodo di lavoro

1. Una sola fase della roadmap alla volta, solo quella richiesta.
2. Prima di scrivere codice, proporre un piano (file coinvolti, strutture dati) e aspettare l'ok.
3. A fine fase: compilare senza warning, eseguire i test (anche con `make SAN=1`), provare l'editor in un terminale.
4. Poi commit Git, solo su richiesta esplicita dell'utente.
5. Le decisioni ancora aperte in `SPEC.md` (nome, scorciatoie predefinite) non si decidono da soli: chiedere.
