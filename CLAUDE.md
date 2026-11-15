# xted

Editor di testo da terminale per Linux, scritto in C99 con sola libc POSIX.
Binario unico, avvio sotto 50 ms. Il nome è `xted` (scelto al posto del provvisorio `ked`, già usato da altri editor); `xt` è il collegamento breve che crea `make install`.
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
| `src/session.[ch]` | File di sessione per cartella (`$XDG_STATE_HOME/xted/sessions/`) |
| `src/search.[ch]` | Nucleo della ricerca: `Matcher` (testo o regex POSIX ERE, per riga), ricerca nel buffer, sostituzione, ricerca nei file; senza terminale |
| `src/find.[ch]` | Comandi di ricerca/sostituzione/vai alla riga sull'`Editor`: barra di ricerca come macchina a stati |
| `src/commands.[ch]` | Tabella dei comandi (nome, scorciatoia come testo, funzione): alimenta la command palette (Alt+X) |
| `src/complete.[ch]` | Candidati dell'autosuggest (membri dopo `->` `::` `.`, simboli del file, dei file inclusi e della libreria standard, parole dei buffer aperti, parole chiave, snippet) con firma (mostrata su ogni riga del popup; alla conferma `complete_call_body` inserisce la chiamata con gli argomenti come segnaposto di snippet), e stato del popup; in PHP dopo `use` propone i nomi qualificati delle classi del progetto (`sym_classes`: classmap e PSR-4 di Composer, alias di `config/app.php`) e, scegliendo una classe, `edit_replace_class` aggiunge l'istruzione `use` in ordine (`imp_php_use`); senza terminale |
| `src/symbols.[ch]` | Indice dei simboli di un file (classi, funzioni, metodi, attributi, inclusioni) costruito con i modelli `decl/include/bind/attr` delle definizioni dei linguaggi (`pat_match`), cache per file (`SymCache`, nel `Complete`), tipo di un oggetto e catene `a->b()->c` (`sym_members`, `sym_globals`); senza terminale |
| `src/stdsym.[ch]` | Libreria standard dei linguaggi (PHP, C, C++, Python, JS/TS) come simboli con firme, compressa nel binario e decompressa al primo uso; i dati sono `tools/lib/*.txt` compressi da `tools/packlib.py` (in `c.txt` le righe `@header` aprono il gruppo di simboli di quell'header, e una sezione di tipo ha la riga `@header` all'inizio: `std_header` li legge, `imp_include` aggiunge l'`#include` quando `complete_accept` accetta un simbolo della libreria C o C++; in `py.txt` `@modulo` precede le voci dei moduli (`os:`) e segue l'inizio di una classe di un modulo (`#Path`), e `tools/py_modules.py` li scrive; in `js.txt` `@=modulo` (import predefinito: `import fs from 'fs'`) e `@modulo` (import con nome: `import { EventEmitter } from 'events'`) seguono l'inizio di una sezione, scritti a mano; `sym_project_symbols` (symbols.c, `SymCache.hdr`: nomi di primo livello di ogni header C e degli export di ogni modulo JS/TS del progetto, letti entro il budget di tempo e riletti quando cambiano; `js_exports` legge gli export, `py_exports` i nomi pubblici di un modulo Python e `py_module_name` il suo nome con i pacchetti, `php_exports` le funzioni, classi e costanti di primo livello di un file PHP col suo namespace; `php_project` sceglie i file `autoload.files` di Composer o, senza Composer, i file del progetto per `require_once`; `sym_class_fq` dà il nome completo della classe davanti a `::`, e la direttiva `include @ ::` fa caricare la classe citata così) dà i simboli dei file non raggiunti con il loro include o import (`Cand.inc`, `hdr_spec`, `js_spec`; per JS un `=` iniziale indica l'export predefinito); `needs_for` (editor.c) decide cosa aggiungere e `imp_python` / `imp_js` lo fanno; per un file C++ (`sym_is_cxx`) si prova prima `cpp.txt`, dove la sezione `#std` ha i gruppi `@header` come quella globale di `c.txt`) in `src/stdsym_data.inc` (generati da `tools/gen_py.py` e `tools/gen_c.py`, gli altri scritti a mano) |
| `src/snippet.[ch]` | Snippet incorporati per linguaggio, analisi del corpo (`$1`, `${2:x}`, `$0`) e sessione dei segnaposti |
| `src/cursors.[ch]` | Multi-cursore: cursori extra dell'`Editor`, `cur_each` esegue un comando a cursore singolo su ciascuno (posizioni tenute come distanza dalla fine del testo), Ctrl+D / Alt+D |
| `src/fold.[ch]` | Folding: `Line.fold` righe nascoste dopo la riga piegata, intervalli per parentesi o indentazione, passaggi sulle righe visibili |
| `src/shell.[ch]` | Processi figli: `shell_start` (in background, stdout+stderr su un pipe), `shell_capture` (stdin da buffer, tempo massimo); niente `Editor` |
| `src/run.[ch]` | Comandi di shell sull'`Editor`: pannello «[Output]» (documento `scratch`), Alt+R/F5, errori F8; il pipe è osservato da `term_watch` |
| `src/errors.[ch]` | Riconoscimento di `file:riga:colonna` nell'output (gcc, tsc, Python); senza terminale |
| `src/keymap.[ch]` | Nomi dei tasti (`ctrl+s`, `alt+shift+up`) e mappa tasto → comando, con i predefiniti presi da `commands[]` |
| `src/config.[ch]` | File `~/.config/xted/config` (chiave = valore, `[linguaggio]`, `[keys]`); non conosce l'`Editor` |
| `src/git.[ch]` | Indicatori Git: `git diff -U0` analizzato in `Line.git`; senza librerie |
| `src/tags.[ch]` | File `tags` di ctags: ricerca (binaria se ordinato) e risoluzione dell'indirizzo; senza terminale |
| `src/mouse.[ch]` | Mouse: `mouse_locate` (cella → tab, browser, picker, riga/byte di un pannello) e `mouse_event` (clic, doppio/triplo clic, trascinamento, rotella, Alt+clic); gli eventi SGR vengono decodificati da `term.c` (`KEY_MOUSE`) |
| `src/indent.[ch]` | `indent_detect`: stile di indentazione (tabulatori o N spazi) dalle prime righe di un testo; senza terminale |
| `src/editorconfig.[ch]` | Lettura di `.editorconfig` (`editorconfig_lookup`: dalla cartella del file verso l'alto, `root = true`; `editorconfig_match`: glob con `*`, `**`, `?`, `[]`, `{a,b}`, `{1..3}`); senza terminale |
| `src/cfgsave.[ch]` | Scrittura delle impostazioni: `cfgsave_write` (file di `xted` a righe `chiave = valore` e `[sezione]`: sostituisce la riga, aggiunge in fondo alla sezione, conserva commenti e ordine, scrive su file temporaneo e rinomina), `cfgsave_editorconfig` (nella sezione più specifica che corrisponde al file del `.editorconfig` più vicino, o ne crea uno con `root = true`), `cfgsave_find_project`/`cfgsave_project_root` (`.xted` di progetto); senza terminale |
| `src/i18n.[ch]` | Traduzioni: `tr("English text")` dà la traduzione del testo dal catalogo della lingua in uso (file `lang/<codice>`, una riga `testo inglese<TAB>traduzione`, cartelle: `<config>/lang`, `lang/` e `../share/xted/lang` accanto all'eseguibile, `XTED_DATADIR/lang`), o il testo stesso; i testi nel codice sono in inglese, il catalogo non è nel binario; una traduzione con conversioni `printf` diverse viene scartata; `i18n_set` (`auto` legge `LC_ALL`, `LC_MESSAGES`, `LANG`), chiave `language`, comando `language` |
| `src/imports.[ch]` | Inserimento di include e import quando si accetta un simbolo: `imp_include` (C/C++, `<…>` e `"…"`), `imp_python`, `imp_js` (stile ESM o `require` del file), `imp_php_use`, `imp_php_use_fn`, `imp_php_require`; parti comuni `insert_lines` (righe vuote e cursore), `insert_block`, `head_end` (dopo commenti, include guard, `'use strict'`); non aprono gruppi di undo (li apre `complete_accept`) |
| `src/util.[ch]` | `die`, `xmalloc`, stringa dinamica |
| `man/` | Pagine del manuale: `xted.1` (inglese) e `it/xted.1` (italiano), installate da `make install`; vanno aggiornate quando cambiano opzioni, scorciatoie o chiavi di configurazione |
| `tests/` | Test automatici: `test_core.c` (moduli e comandi senza terminale) e `smoke/` (`vt.py` emula un terminale, `run.py` gli scenari: palette, lingua, Ctrl+T/E/P, ricerca, tema chiaro, salvataggio impostazioni, import, guida F1; cartelle temporanee per configurazione e stato) |

- Le cartelle di configurazione e di stato si chiamavano `ked`: `app_dir` (util.c) usa ancora quella vecchia se è l'unica che esiste, e le sessioni con intestazione `ked-session` si leggono ancora.
- Testi per l'utente: in inglese nel codice, sempre dentro `tr("...")` (per un testo senza argomenti passato come formato usare `"%s", tr(...)`); la traduzione italiana sta in `lang/it`, da tenere allineata (`make test` ne controlla i formati). I nomi dei comandi (`commands[].name`), i titoli dei gruppi e i valori delle impostazioni si traducono al momento di mostrarli; id dei comandi, chiavi di configurazione e nomi dei tasti restano in inglese.
- Ogni fase aggiunge i moduli previsti da `SPEC.md` (sintassi, comandi, configurazione, …).
- Un modulo non deve dipendere da quelli di livello più alto: `buffer` e `utf8` non conoscono terminale né UI.
- Ogni modifica al testo passa dalle primitive di `buffer.c` (`buf_insert/delete/split/join`) o dagli helper costruiti su di esse, così undo e rappresentazione interna restano isolati. Un comando che modifica il testo apre un gruppo undo (`buf_undo_begin`) e lo chiude a fine comando.
- `Editor` espone i campi `cy, cx, rowoff, …` del **pannello attivo**; gli altri pannelli hanno la loro `View` in `Win`. Cambiare pannello o documento passa da `editor_store_view`/`load_win` (editor.c); `e->buf` e `e->doc` puntano sempre al documento attivo.
- Un comando nuovo si aggiunge a `commands[]` (id inglese stabile, scorciatoia predefinita come testo di configurazione, `group` `G_*` e, se è un'impostazione, `state` `ST_*` letto da `command_state`): palette, tasti e `[keys]` lo vedono da lì. La palette elenca i comandi nell'ordine della tabella, quindi i comandi dello stesso gruppo vanno vicini e le azioni prima di `G_SETTINGS`; le intestazioni sono voci del picker che iniziano con `PICKER_HEAD` (`picker_is_head`, saltate dalle frecce) e `Editor.pal` traduce la voce scelta nell'indice del comando; le colonne si disegnano in `palette_row` (render.c; `symbol_row` e `savecfg_row` fanno lo stesso per Ctrl+T e per «Salva dove»: voci con campi separati da `\x1f` e larghezze in `Editor.pal_w1..3`). Barre, tab, elenchi e riga di stato usano i colori del tema (`bar`, `row_on`, `fg_back` in render.c), non il video inverso del terminale, quando il tema ha uno sfondo (`themed`); il cursore prende il colore del testo con OSC 12 (reimpostato con OSC 112 all'uscita). Invio su un comando con `state` lascia aperta la palette (`palette_open` con la stessa ricerca) e `Editor.pal_last` ricorda dove riaprirla. In `editor.c` restano cablati solo digitazione, movimenti e selezione; non aggiungere `case` per tasti di comando.
- L'indentazione di un documento (`Doc.tabw` larghezza di un tab disegnato, `expand_tab`, `indent_w` spazi di un livello) si risolve in `apply_cfg` con questa precedenza: comando dell'utente (`Doc.man`, `Doc.man_sug/man_pair`, palette) > `.xted` del progetto (`project_lang`, stesso formato del file utente, vale per le chiavi di linguaggio) > `~/.config/xted/config` (solo i valori scritti: `LangCfg.set`) > `.editorconfig` > stile rilevato nel testo (`Doc.det`, calcolato in `install_doc`) > predefiniti. «Salva le impostazioni…» (`editor_cmd_save_settings`, `PK_SAVECFG`) scrive su file utente, `.xted` o `.editorconfig` ciò che è stato cambiato con comandi (`Pending`: chiavi di linguaggio da `Doc.man*` del documento attivo e, in `more`, degli altri documenti aperti, una per linguaggio; globali da `Editor.pend`), avvisa se la destinazione verrebbe ignorata e poi riapplica la configurazione (`apply_all`). Le chiavi globali del `.xted` (`project_globals`, `editor_load_project`, `Editor.proj`) si applicano una volta all'avvio, dal `.xted` più vicino al primo file aperto. `edit.c` usa sempre `indent_unit` (un livello) e `indent_w`, mai `tabw`, per inserire o togliere indentazione. - `Doc.tabw`, `expand_tab`, … vengono da `config` per linguaggio (`apply_cfg`); `e->tabw` è lo specchio del documento attivo, per gli altri pannelli il renderer usa `d->tabw`. Con il margine Git (`Doc.gutter`) la larghezza del testo è `win_text_w(w)`, non `w->w`.
- I campi extra del multi-cursore (`Editor.xc`) vengono svuotati cambiando pannello/documento e con undo/redo; i comandi di `edit.c` restano a cursore singolo e `cur_each` li ripete per ogni cursore. `Line.fold`/`Buffer.folded` sono lo stato del folding: dopo un'azione che sposta il cursore, `fold_reveal` riapre ciò che lo nasconde; le funzioni di movimento e rendering contano le righe *visibili* (`fold_next/prev/advance/back/nth/count`), mai `cy + 1`.
- Il mouse passa da `KEY_MOUSE` (`term_mouse_event`) a `mouse_event`; la geometria dello schermo si ricava con `mouse_locate`/`render_tab_at`/`render_picker_height`, non si ricalcola altrove. Per far agire il mouse come un tasto in una lista si usa `editor_key`.
- Il ridimensionamento degli split passa solo da `layout_set_size/layout_resize/layout_equalize` seguiti da `layout_refresh` (mouse: trascinamento di bordi e browser; tastiera: modalità Alt+E, `Editor.resize_mode`). Lo stato di sintassi di una riga può indicare un linguaggio incorporato (bit 15 di `Line.hl`): per i marcatori di commento di una riga usare `syn_comment_at`, non `syn_comment`.
- L'autosuggest dei simboli passa da `complete_open` → `sym_members`/`sym_globals`: per aggiungere un linguaggio bastano le direttive `decl/include/bind/attr/synonym/docmember/member` in `langs.c` e, se serve, una lista in `tools/lib` (poi `tools/packlib.py`). `Buffer.gen` cresce a ogni modifica: le cache di dati derivati lo confrontano. Le classi nominate (`use`, `new`, alias globali di `config/app.php`) si cercano con la mappa PSR-4 di Composer (`vendor/composer/autoload_psr4.php`) e, se manca, nei file del progetto senza `vendor`; i metodi dichiarati in `@method` sopra una classe (facade di Laravel) sono indicizzati dalla direttiva `docmember`, e una facade che ha solo `@see` inoltra ai metodi pubblici delle classi indicate. Le scansioni del progetto (file per cercare le classi, elenco delle classi per `use`) hanno un limite di tempo: `complete_open` chiama `sym_budget` (`COMPLETE_SCAN_MS`), le scansioni (`files_scan_begin/step`) si fermano e riprendono alla chiamata successiva, `sym_busy` lo segnala e `complete_resume`, chiamata da `next_key` quando l'utente non scrive, le prosegue mostrando «Scansione» nella barra di stato; senza `sym_budget` (test) non c'è limite. Il contesto di una parentesi aperta (`sym_call_at`: nome, indice dell'argomento, definizione o chiamata, posizione del tipo) alimenta tre cose: le variabili locali di `sym_locals` (parametri e nomi dichiarati nella funzione che contiene il cursore, euristiche per famiglia di linguaggio: PHP `$x`, Python assegnazioni/`for`/`as`, JS `let/const/var`, C `tipo nome`; `CAND_LOCAL` con bonus di punteggio), i soli tipi dove va il tipo di un parametro (`Gather.types_only`) e il suggerimento della firma (`sym_call_hint` → `Editor.hint`, calcolato in `prepare_frame` solo quando cambiano posizione o testo, mostrato da `status_row`). `sym_outline` elenca le definizioni del documento per «Vai al simbolo» (Ctrl+T, `PK_SYMBOLS`; le posizioni stanno in `Editor.outline`). Ctrl+P legge il progetto con `files_scan_begin/step` (`Editor.fscan`): `files_scan_pump` aggiunge i file al picker a passi di `FILES_STEP_US` quando `next_key` non ha tasti in attesa, `picker_refilter` conserva la voce scelta e `Picker.loading` fa mostrare l'indicatore dal renderer. Il limite di dimensione del binario statico è 520 KB (SPEC.md).
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
make                 # build ottimizzata (./xted)
make STATIC=1        # link statico
make SAN=1           # build con AddressSanitizer e UBSan
make test            # test automatici
make smoke           # l'editor su un vero pseudo-terminale (tests/smoke, serve python3)
make clean
./xted [file]        # esecuzione
make install         # in $(PREFIX)/bin (predefinito /usr/local): xted e il collegamento xt
```

## Metodo di lavoro

1. Una sola fase della roadmap alla volta, solo quella richiesta.
2. Prima di scrivere codice, proporre un piano (file coinvolti, strutture dati) e aspettare l'ok.
3. A fine fase: compilare senza warning, eseguire i test (anche con `make SAN=1`) e `make smoke`, provare l'editor in un terminale. Una funzione nuova visibile a schermo ha il suo scenario in `tests/smoke/run.py`.
4. Poi commit Git, solo su richiesta esplicita dell'utente.
5. Le decisioni ancora aperte in `SPEC.md` (nome, scorciatoie predefinite) non si decidono da soli: chiedere.
