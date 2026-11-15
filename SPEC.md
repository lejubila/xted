# Specifiche editor di testo da terminale

Nov 7, 2026 · @David

## Obiettivi e vincoli

Un editor da terminale per Linux, scritto in C99 con sola libc POSIX: un unico binario statico sotto 480 KB, avvio sotto 50 ms.

- **Zero dipendenze**: niente ncurses, niente librerie esterne; controllo del terminale via termios e sequenze ANSI/VT100 scritte a mano.
- **Linguaggio**: C99 (alternativa: Rust senza crate esterni, ma binario più grande).
- **Build**: un solo `Makefile`, compilabile con `gcc` o `clang`, opzione di link statico (`-static`, es. con musl).
- **Nome**: `xted` (X = Unix/Linux, T = text/terminal, ED = editor), con `xt` come comando breve (collegamento creato da `make install`). **Licenza**: MIT (file `LICENSE`).
- **Ispirazione**: kilo/antirez per la base, nano per la semplicità, micro per i comandi familiari (Ctrl+C/Ctrl+V).

## Architettura e terminale

Il nucleo è un buffer di testo indipendente dalla UI, con rendering differenziale sul terminale in raw mode.

- **Raw mode** con termios; ripristino garantito del terminale all'uscita, su crash (SIGSEGV, SIGABRT) e su SIGTERM.
- **Alternate screen buffer** (`ESC[?1049h`): all'uscita il terminale torna com'era.
- **Ridimensionamento** della finestra gestito con SIGWINCH e `ioctl(TIOCGWINSZ)`.
- **Struttura dati del buffer**: array di righe con gap buffer per riga, o piece table (consigliata per undo e file grandi).
- **Rendering differenziale**: ridisegna solo le righe cambiate; output in un unico `write()` per frame per evitare sfarfallio.
- **UTF-8 completo**: cursore per carattere, larghezza dei caratteri larghi (CJK, emoji) tramite tabella `wcwidth` interna.
- **Colori**: 16, 256 e truecolor, rilevati da `$COLORTERM` e `$TERM`, con degradazione automatica.
- **Input**: decodifica di tasti speciali e combinazioni (Ctrl, Alt, Shift+frecce), bracketed paste, mouse SGR opzionale (click, selezione, rotella).
- **Fine riga**: rileva e preserva LF o CRLF; preserva l'assenza di newline finale.
- **Modularità**: file separati per terminale, buffer, rendering, sintassi, comandi, configurazione.

## Gestione file e multifile

Più file aperti contemporaneamente come buffer, mostrati in tab e affiancabili in split.

- **Apertura da riga di comando**: `xted file1 file2 ...`, anche `xted file.c:120` per saltare alla riga 120; `xted -` legge da stdin.
- **Buffer multipli**: apri, chiudi, passa al successivo/precedente, lista dei buffer aperti con selezione rapida.
- **Tab bar** in alto con nome file e indicatore di modifica (`*`).
- **Split** orizzontale e verticale, anche dello stesso file; spostamento del focus tra i pannelli.
- **File browser** integrato (albero della directory corrente) in un pannello laterale attivabile.
- **Apertura rapida** con ricerca fuzzy dei file nel progetto (stile Ctrl+P). L'elenco si apre subito e si riempie a passi mentre la cartella viene letta (anche su dischi di rete lenti): si può già filtrare e scegliere, e sulla riga del titolo compare «Caricamento» con un contatore finché la lettura non finisce.
- **Salvataggio**: salva, salva con nome, salva tutti; scrittura atomica (file temporaneo + `rename`) preservando i permessi.
- **Protezione**: conferma prima di chiudere file non salvati; avviso se il file è cambiato su disco (ricarica o ignora).
- **Backup e recupero**: file di swap/autosalvataggio periodico e recupero dopo crash.
- **Sessione**: memorizza file aperti e posizione del cursore per ogni file, ripristinabili alla riapertura.
- **File speciali**: gestione dei file in sola lettura, avviso sui file binari, supporto a file grandi (100 MB+) senza bloccare l'editor.

## Editing di base

Comandi non modali e scorciatoie familiari (Ctrl+S, Ctrl+C, Ctrl+V, Ctrl+Z), come negli editor grafici.

- **Movimento**: frecce, Home/End, PgUp/PgDn, inizio/fine file, parola per parola (Ctrl+frecce), "smart home" (primo carattere non spazio).
- **Selezione**: Shift+movimento, seleziona tutto, seleziona parola/riga, selezione a blocco (colonna).
- **Copia, taglia, incolla**: clipboard interna con storico (kill ring); integrazione con la clipboard di sistema via OSC 52, senza dipendenze; copia/taglia riga intera se non c'è selezione.
- **Undo/redo** illimitato, con raggruppamento delle modifiche consecutive.
- **Operazioni sulle righe**: duplica, cancella, sposta su/giù (Alt+frecce), unisci righe.
- **Indentazione**: Tab/Shift+Tab su selezione per indentare/deindentare blocchi.
- **Commento**: attiva/disattiva commento su riga o selezione in base al linguaggio.
- **Multi-cursore**: aggiungi cursore alla prossima occorrenza della parola (Ctrl+D) e su righe adiacenti.
- **Varie**: overwrite mode (Ins), maiuscole/minuscole della selezione, cancellazione parola precedente/successiva, inserimento di caratteri per codice Unicode.

## Funzioni per la programmazione

Evidenziazione, indentazione e suggerimenti sono tutti interni: nessun language server obbligatorio.

### Evidenziazione della sintassi

- Motore a regole definite in file di sintassi testuali (parole chiave, tipi, stringhe, numeri, commenti, preprocessore, operatori).
- Stato per riga per gestire costrutti multiriga (commenti `/* */`, stringhe `"""`, heredoc).
- Rilevamento del linguaggio da estensione, shebang (`#!/usr/bin/env python`) e modeline.
- Linguaggi iniziali: C/C++, Python, JavaScript/TypeScript, Go, Rust, Java, Shell, HTML, CSS, JSON, YAML, Markdown, Makefile, SQL, Lua, PHP.
- Temi colore intercambiabili (almeno uno scuro e uno chiaro).

### Indentazione

- **Autoindent**: la nuova riga eredita l'indentazione della precedente.
- **Coppie automatiche** (`autopair`, attivo di predefinito, per linguaggio): scrivendo `(` `[` `{` `"` `'` (e il backtick in JS/TS/Markdown/Shell) si inserisce anche il carattere di chiusura; la chiusura già presente si scavalca, Backspace in una coppia vuota le toglie entrambe, una selezione viene racchiusa, Invio tra `{}` mette la graffa su una riga propria. Non agisce in stringhe e commenti, davanti a una parola, con più cursori; le virgolette sono escluse in testo semplice, Markdown e Diff, l'apostrofo anche in Rust.
- **Smart indent**: indentazione in più dopo `{`, `:` (Python), `(`, e deindentazione automatica alla chiusura `}`.
- Tab o spazi configurabili per linguaggio (`expand_tab`, `tab_width` = larghezza con cui si disegna un tabulatore, `indent_width` = spazi di un livello, se manca vale `tab_width`); rilevamento automatico dello stile del file aperto; comandi della palette «Indentazione: tabulatore / 2 / 4 / 8 spazi», «Converti l'indentazione in spazi / tabulatori», «Rileva lo stile di indentazione»; la barra di stato mostra `Spazi N` o `Tab N`. Precedenza: comando dell'utente, configurazione, `.editorconfig`, stile del file, predefiniti.
- Supporto a `.editorconfig` (`indent_style`, `indent_size`, `tab_width`, `trim_trailing_whitespace`, `insert_final_newline`; non ancora `end_of_line` e `charset`).

### Aiuti alla scrittura

- **Parentesi**: evidenziazione della coppia corrispondente, salto alla parentesi corrispondente, chiusura automatica di parentesi e virgolette.
- **Vai al simbolo** (Ctrl+T, comando `symbols`): elenco filtrabile (fuzzy) di classi, funzioni, metodi, proprietà e costanti del file, nell'ordine in cui compaiono, con i membri qualificati dalla classe e i parametri; Invio porta il cursore sul nome. Non elenca i membri dichiarati solo in un commento (`@method`) né gli attributi creati da assegnazioni (`$this->x = …`). Parte dalla definizione sopra il cursore.
- **Autosuggest/autocompletamento**: popup con parole presenti in tutti i buffer aperti, parole chiave del linguaggio e snippet; filtro fuzzy, selezione con Tab/Invio (Invio sceglie la voce evidenziata, la prima se non ci si è mossi; dentro uno snippet Tab passa al segnaposto successivo e accetta solo dopo una scelta con le frecce). Dentro le parentesi di una chiamata e nel corpo di una funzione le variabili locali e i parametri della funzione (PHP, Python, JS/TS, C) hanno la precedenza (lettera `v`); nella dichiarazione dei parametri, dove va il tipo (`function f(Us|`, `int f(Poi|`, `def f(x: in|`), si propongono solo tipi e classi. Per una chiamata a una funzione o a un metodo noti la barra di stato mostra la firma con il parametro corrente tra parentesi quadre. In C, accettando un simbolo della libreria standard si aggiunge l'`#include` del suo header se manca (`edit_add_include`: dati `@header` in `tools/lib/c.txt`, nello stesso passo di annullamento).
- **Snippet** con segnaposti navigabili (es. `for` → ciclo completo).
- **Code folding** basato su indentazione o parentesi.
- **Rimozione spazi finali** e newline finale al salvataggio (opzionale).
- **Guide di indentazione** e visualizzazione opzionale di tab e spazi.

### Integrazione con gli strumenti

- **Esecuzione comandi**: lancia un comando di shell (es. `make`) e mostra l'output in un pannello.
- **Errori del compilatore**: analizza l'output `file:riga:colonna` e permette di saltare all'errore.
- **Filtro**: passa la selezione a un comando esterno e sostituiscila con l'output (es. `sort`, formattatori).
- **Git**: indicatori nel margine delle righe aggiunte/modificate/cancellate (usando il comando `git` se presente, senza linkare librerie).
- **Tag**: salto alla definizione tramite file `ctags`, se presente.
- **LSP opzionale** (fase avanzata): client Language Server su stdin/stdout per completamento e diagnostica, solo se un server è installato.

## Ricerca e navigazione

Ricerca incrementale nel file e ricerca globale nel progetto, con un motore regex interno.

- **Ricerca incrementale** (Ctrl+F): evidenzia tutte le occorrenze mentre si digita; avanti/indietro tra i risultati.
- **Opzioni**: maiuscole/minuscole, parola intera, espressioni regolari (motore regex scritto internamente o `regex.h` di POSIX, che fa parte della libc).
- **Sostituisci**: singola, tutte, con conferma; gruppi di cattura nelle regex; limitata alla selezione.
- **Cerca nei file** del progetto con lista dei risultati navigabile.
- **Vai alla riga** (Ctrl+G) e alla colonna.
- **Lista dei simboli** del file (funzioni, classi) ricavata dalle regole di sintassi, con salto rapido.
- **Segnalibri** su righe e cronologia delle posizioni (indietro/avanti tra i salti).
- **Command palette**: tutti i comandi cercabili per nome. Con la ricerca vuota sono divisi in sezioni (intestazioni non selezionabili): prima le **azioni**, per tema (File, Buffer e pannelli, Cerca e vai, Modifica, Cursori e blocchi, Esegui, Altro), poi le **impostazioni** che cambiano il comportamento dell'editor. Il nome è in grassetto, poi il valore corrente di un'impostazione (●/○ per attivo/spento, in evidenza se attivo; l'indentazione e il tema in uso sono segnati) e tutte le scorciatoie del comando (separate da virgole) nel colore dei commenti, in colonne uguali per tutte le righe. Invio su un'impostazione la cambia e lascia aperta la palette (la riga mostra il nuovo valore); Invio su un'azione la esegue e chiude. Riaprendo la palette la selezione torna sull'ultimo comando scelto o su cui si è usciti. Anche l'elenco di Ctrl+T (tipo, nome, parametri, riga) e quello di «Salva le impostazioni…» (tipo di file, ambito, nota, percorso) usano colonne e colori distinti. Digitando, le intestazioni spariscono e resta l'ordine per punteggio.

## Interfaccia, configurazione, prestazioni

L'interfaccia è minimale ma informativa; tutto è configurabile da un file di testo semplice.

### Interfaccia

- **Numeri di riga** (assoluti o relativi) ed evidenziazione della riga corrente.
- **Barra di stato**: nome file, stato modificato, linguaggio, riga:colonna, encoding, fine riga, tab/spazi.
- **Riga messaggi/prompt** per input dei comandi, conferme ed errori.
- **Righello** a colonna configurabile (es. 80) e a capo visivo (soft wrap) attivabile.
- **Scorrimento** con margine (il cursore non tocca il bordo) e scorrimento orizzontale per righe lunghe.
- **Help integrato** (F1) con l'elenco delle scorciatoie.

### Configurazione

- File `~/.config/xted/config` in formato chiave = valore, con override per linguaggio.
- File `.xted` di progetto (nella cartella del progetto o sopra il file, il più vicino), stesso formato: le chiavi per linguaggio hanno la precedenza sul file utente, per il documento che le riguarda. Le chiavi globali (`theme`, `mouse`, `git`, `clipboard`) valgono per tutta la sessione e si leggono dal `.xted` più vicino al primo file aperto (o alla cartella corrente) all'avvio. Ordine: comando scelto nella palette > `.xted` > file utente > `.editorconfig` > stile rilevato nel file > predefiniti.
- **Salva le impostazioni…** (palette): scrive ciò che è stato cambiato con comandi (indentazione, tema, interruttori di autosuggest, coppie automatiche, mouse, Git, appunti di sistema) nel file scelto dall'utente: utente (sezione globale o del linguaggio), `.xted` di progetto (creato nella radice del progetto se manca; riceve anche tema, mouse, Git e appunti) o `.editorconfig` (solo indentazione). Le modifiche degli altri documenti aperti si salvano insieme, una sezione `[Linguaggio]` per linguaggio (nel `.editorconfig`, nella sezione del file di ciascun documento); «tutti i linguaggi» riguarda il solo documento attivo. Ogni destinazione indica se verrebbe ignorata da un file più forte. I file si modificano riga per riga, senza perdere commenti e ordine.
- Scorciatoie da tastiera rimappabili.
- Cartella `~/.config/xted/syntax/` per aggiungere linguaggi senza ricompilare.
- Temi in `~/.config/xted/themes/` (i predefiniti si chiamano `dark` e `light`; `scuro` e `chiaro` si accettano ancora).
- **Lingua dell'interfaccia**: i testi nel codice sono in inglese e si traducono da cataloghi esterni (`lang/<codice>`, cercati in `~/.config/xted/lang/`, accanto all'eseguibile e in `$PREFIX/share/xted/lang/`), non incorporati nel binario. Chiave `language = auto|en|it` (`auto` da `LC_ALL`, `LC_MESSAGES`, `LANG`) e comando «Language» nella palette, che mostra il file del catalogo in uso. Una traduzione con conversioni `printf` diverse viene scartata.
- Valori predefiniti sensati: l'editor funziona senza alcun file di configurazione.

### Prestazioni e qualità

- Memoria a riposo sotto 5 MB con un file piccolo aperto.
- Apertura di un file da 10 MB in meno di 1 secondo; evidenziazione calcolata solo sulle righe visibili più lo stato necessario.
- Nessun lavoro in background inutile; il ciclo principale attende input con `poll()`.
- Compilazione senza warning con `-Wall -Wextra -pedantic`; verifica con AddressSanitizer e Valgrind.
- Test automatici sul buffer, undo/redo, ricerca e parser di sintassi.
- Pagina `man` (`man/xted.1` in inglese, `man/it/xted.1` in italiano) e `README` con l'elenco dei comandi.

## Roadmap a fasi e istruzioni per Claude Code

Conviene chiedere a Claude Code una fase alla volta, verificando che compili e funzioni prima di passare alla successiva.

1. **Nucleo**: raw mode, rendering, apertura/salvataggio di un file, movimento, inserimento/cancellazione, UTF-8, barra di stato.
2. **Editing**: selezione, copia/taglia/incolla, undo/redo, operazioni sulle righe, autoindent.
3. **Sintassi**: motore di evidenziazione, primi 5 linguaggi (C, Python, JS, Shell, Markdown), temi, parentesi corrispondenti.
4. **Multifile**: buffer multipli, tab, split, apertura fuzzy, file browser, sessioni.
5. **Ricerca**: ricerca incrementale, regex, sostituzione, cerca nei file, vai alla riga.
6. **Produttività**: autosuggest, snippet, multi-cursore, commenti, folding, command palette.
7. **Strumenti**: esecuzione comandi, salto agli errori, indicatori Git, configurazione e keybinding personalizzabili.
8. **Avanzato (opzionale)**: client LSP, mouse, linguaggi aggiuntivi.

### Come passare il documento a Claude Code

- Salva questo documento come `SPEC.md` nella cartella del progetto e chiedi a Claude Code di leggerlo prima di iniziare.
- Primo prompt suggerito: *"Leggi SPEC.md. Proponi la struttura dei file e le strutture dati principali, poi implementa solo la Fase 1. Nessuna dipendenza oltre la libc."*
- Chiedi di creare un `CLAUDE.md` con i vincoli fissi (C99, zero dipendenze, stile del codice) così restano validi in tutte le sessioni.
- A fine di ogni fase chiedi test, verifica con `-Wall -Wextra` e un commit Git.

### Decisioni ancora aperte

- Nome definitivo dell'editor.
- C oppure Rust.
- Scorciatoie stile moderno (Ctrl+C/V) oppure stile nano/emacs come predefinite.
