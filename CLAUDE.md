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
| `src/edit.[ch]` | Comandi di modifica: selezione, appunti, operazioni sulle righe, indentazione, autoindent |
| `src/util.[ch]` | `die`, `xmalloc`, stringa dinamica |
| `tests/` | Test automatici |

- Ogni fase aggiunge i moduli previsti da `SPEC.md` (sintassi, comandi, configurazione, …).
- Un modulo non deve dipendere da quelli di livello più alto: `buffer` e `utf8` non conoscono terminale né UI.
- Ogni modifica al testo passa dalle primitive di `buffer.c` (`buf_insert/delete/split/join`) o dagli helper costruiti su di esse, così undo e rappresentazione interna restano isolati. Un comando che modifica il testo apre un gruppo undo (`buf_undo_begin`) e lo chiude a fine comando.

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
