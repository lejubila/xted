# xted

Editor di testo da terminale per Linux, scritto in **C99** con la sola libc POSIX.
Un unico binario, senza ncurses né altre librerie: il terminale è controllato a mano con
termios e sequenze ANSI. Parte in meno di 50 ms e il binario statico resta sotto 520 KB.

Il nome è `xted` (X = Unix/Linux, T = text/terminal, ED = editor); `make install` crea anche
il collegamento breve `xt`.

Comandi non modali e scorciatoie familiari (Ctrl+S, Ctrl+C, Ctrl+V, Ctrl+Z), come negli
editor grafici. Per scrivere codice ha evidenziazione della sintassi, indentazione
automatica, suggerimenti dei simboli, snippet, multi-cursore, folding e salto alle
definizioni, tutto interno: non serve alcun language server.

## Indice

- [Installazione](#installazione)
- [Uso](#uso)
- [Funzioni](#funzioni)
- [Scorciatoie](#scorciatoie)
- [Command palette](#command-palette-altx)
- [Configurazione](#configurazione)
- [Temi e linguaggi dell'utente](#temi-e-linguaggi-dellutente)
- [Sviluppo](#sviluppo)
- [Stato del progetto](#stato-del-progetto)
- [Licenza](#licenza)

## Installazione

Servono un compilatore C (`gcc` o `clang`) e `make`. Nient'altro.

```
make                  # build ottimizzata: ./xted
make install          # in /usr/local/bin: xted e il collegamento xt
make install PREFIX=$HOME/.local        # senza permessi di amministratore
man xted                                 # la pagina del manuale (inglese e italiano)
make uninstall
```

`make install` installa anche i cataloghi di lingua (`$PREFIX/share/xted/lang/`) e le pagine
del manuale (`man/xted.1` in inglese, `man/it/xted.1` in italiano, in `$MANDIR`, di norma
`$PREFIX/share/man`). Non sovrascrive un comando `xt` che non sia il suo collegamento a `xted`.

Altre modalità di compilazione:

```
make STATIC=1 CC=musl-gcc    # binario statico e piccolo (-Os)
make SAN=1                   # AddressSanitizer e UBSan, per lo sviluppo
make test                    # test automatici
make clean
```

Il codice compila senza warning con `-Wall -Wextra -pedantic`.

## Uso

```
xted [file[:riga[:colonna]] ...]
xted -                    legge il testo da stdin
xted cartella             apre il browser dei file su quella cartella
```

- `xted app.c:120` apre il file e porta il cursore alla riga 120 (`:120:5` anche la colonna).
- Senza argomenti riapre i file e la posizione del cursore della sessione precedente di
  quella cartella.
- Più file diventano più buffer, mostrati in tab.
- Per uscire: `Ctrl+Q` (chiede conferma se ci sono file non salvati).

Il terminale viene sempre ripristinato all'uscita, anche su SIGTERM, SIGHUP o un arresto
anomalo; l'editor usa lo schermo alternativo, quindi la schermata di prima torna com'era.
Ctrl+C è la scorciatoia per copiare, non interrompe il programma.

## Funzioni

### File e buffer

- **Buffer multipli** con barra dei tab (il simbolo `*` indica le modifiche non salvate).
  `Ctrl+E` apre l'elenco dei buffer con nome e percorso in colonne, filtrabile.
- **Apertura rapida** (`Ctrl+P`): ricerca fuzzy dei file del progetto. L'elenco si apre subito
  e si riempie mentre la cartella viene letta (anche su dischi di rete lenti): si può già
  filtrare e scegliere, e sulla riga del titolo c'è «Caricamento» con un contatore.
- **Browser dei file** laterale (`Ctrl+B`), con cartelle apribili e chiudibili.
- **Split** orizzontali e verticali, anche dello stesso file; i bordi si ridimensionano con
  il mouse o con la modalità ridimensionamento (`Alt+E`).
- **Salvataggio** atomico (file temporaneo e `rename`) che conserva i permessi; salva, salva
  con nome, salva tutto.
- **Protezioni**: conferma prima di chiudere file non salvati, avviso se il file è cambiato
  su disco, file in sola lettura gestiti, avviso sui file binari.
- **Fine riga**: rileva e conserva LF o CRLF, e l'assenza del newline finale.
- **UTF-8 completo**, con larghezza corretta dei caratteri larghi (CJK, emoji).
- **Sessione** per cartella (file aperti e cursore), in `$XDG_STATE_HOME/xted/sessions/`.

### Editing

- **Movimento**: frecce, Home/End (con "smart home"), PgUp/PgDn, parola per parola con
  Ctrl+frecce, inizio e fine del file.
- **Selezione**: Shift+movimento, seleziona tutto, seleziona riga o parola.
- **Copia, taglia, incolla** con una cronologia (kill ring): `Alt+V` incolla la voce
  precedente. Copia e taglia lavorano sulla riga intera se non c'è selezione. Gli appunti
  di sistema si raggiungono con OSC 52 (si attiva con `clipboard = on`).
- **Undo/redo** illimitato, con raggruppamento delle modifiche consecutive.
- **Righe**: duplica, elimina, unisci, sposta su e giù (`Alt+frecce`).
- **Indentazione**: Tab e Shift+Tab su una selezione indentano o tolgono un livello;
  autoindent e smart indent (più indentazione dopo `{`, `:` in Python, `(`; meno alla
  chiusura).
- **Coppie automatiche**: scrivendo `(`, `[`, `{`, `"`, `'` si inserisce anche la chiusura;
  la chiusura già presente si scavalca, Backspace in una coppia vuota le toglie entrambe,
  una selezione viene racchiusa, Invio tra `{}` mette la graffa su una riga propria.
  Non agisce in stringhe e commenti.
- **Commento** su riga o selezione, in base al linguaggio (`Alt+/`).
- **Multi-cursore**: `Ctrl+D` aggiunge la prossima occorrenza della parola, `Alt+D` tutte,
  `Alt+Shift+Su/Giù` un cursore sopra o sotto, `Alt+clic` un cursore dove si clicca.
- **Filtro**: `Alt+K` passa la selezione a un comando esterno e la sostituisce con l'output
  (`sort`, un formattatore, …).
- **Folding** di blocchi (`Alt+Z`), per parentesi o per indentazione.
- **Bracketed paste**: l'incollato dal terminale non viene interpretato come tasti.
- **Indentazione per file**: rileva se il file usa tabulatori o N spazi; si può cambiare con
  la palette («Indentazione: tabulatore / 2 / 4 / 8 spazi») e convertire.
- **EditorConfig**: legge `.editorconfig` (`indent_style`, `indent_size`, `tab_width`,
  `trim_trailing_whitespace`, `insert_final_newline`).

### Ricerca

- **Cerca** (`Ctrl+F`): incrementale, evidenzia tutte le occorrenze mentre si digita.
  Sulla riga della barra compaiono i tasti utili (le opzioni attive in grassetto, poi `Invio` ed `Esc`, in forma più corta se lo schermo è stretto). Opzioni con `Alt+C` (maiuscole/minuscole), `Alt+W` (parola intera) e `Alt+R` (espressione
  regolare POSIX ERE, con gruppi di cattura nella sostituzione). `F3` e `Shift+F3` (o
  `Alt+N` e `Alt+P`) passano all'occorrenza successiva e precedente.
- **Sostituisci** (`Ctrl+R`): con conferma per ogni occorrenza («s = sì, n = no, t = tutte,
  Esc = fine»), anche limitata alla selezione.
- **Cerca nei file** (`Alt+F`): elenco di risultati navigabile.
- **Vai alla riga** (`Ctrl+G`) e alla colonna.
- **Vai al simbolo** (`Ctrl+T`): elenco filtrabile di classi, funzioni, metodi, proprietà e
  costanti del file, con tipo, nome, parametri e riga in colonne; Invio porta il cursore sul
  nome. Parte dalla definizione sopra il cursore.
- **Vai alla definizione** (`F12`) con un file `tags` di ctags, se presente.
- **Parentesi corrispondente**: evidenziata, e `Alt+M` salta all'altra.

### Programmazione

- **Evidenziazione della sintassi** con un motore a regole: C/C++, Python, JavaScript,
  TypeScript, Shell, Markdown, PHP, HTML, PHP/HTML, XML, CSS, JSON, YAML, TOML, SQL, Go,
  Rust, Java, C#, Ruby, Makefile, Dockerfile, Diff. Il linguaggio si riconosce da
  estensione, nome del file e shebang. Costrutti multiriga (commenti, stringhe, heredoc) e
  linguaggi incorporati (JS e CSS in HTML, HTML in PHP) sono gestiti.
- **Autosuggest**: popup con membri dopo `->`, `::` e `.`, simboli del file, dei file inclusi
  e della libreria standard (PHP, C, C++, Python, JavaScript/TypeScript), parole degli altri
  buffer, parole chiave e snippet. Ogni riga mostra la firma completa con i parametri e una
  lettera per il tipo (`f` funzione, `c` classe, `m` metodo, `p` attributo, `k` costante,
  `v` variabile locale). Dentro una chiamata hanno la precedenza le variabili locali e i
  parametri; nella dichiarazione di un parametro si propongono i tipi; la firma della
  funzione e il parametro corrente compaiono nella barra di stato.
  Confermando una funzione si inserisce la chiamata con gli argomenti come segnaposti.
  - `Tab` accetta (dentro uno snippet solo dopo aver scelto con le frecce), `Invio` sceglie
    la voce evidenziata (la prima se non ci si è mossi) e non va a capo, `Esc` chiude.
  - In PHP, dopo `use`, propone le classi del progetto (classmap e PSR-4 di Composer, alias
    di `config/app.php`) e aggiunge l'istruzione `use` in ordine; i metodi dichiarati in
    `@method` (facade di Laravel) sono riconosciuti.
  - In C e C++, scegliendo un simbolo della libreria standard (`strlen`, `malloc`, `FILE`, `INT_MAX`, `uint32_t`,
    `O_RDONLY`, …, e in C++ `vector`, `sort`, `make_unique`, …) l'editor aggiunge anche `#include <string.h>`
    (o `<vector>`, `<algorithm>`, `<memory>`, `<cstdio>`, l'header giusto) se manca, e se non c'è già
    l'header equivalente (`<cstdio>` o `<stdio.h>`) né `<bits/stdc++.h>`: tra gli
    `#include <…>` esistenti in ordine, prima di quelli locali `"…"`, oppure in cima al file dopo i
    commenti e l'include guard. Un solo Annulla toglie sia l'include sia il testo inserito.
  - In C e C++ si propongono anche le funzioni, i tipi, le macro e le variabili degli header del
    progetto che il file non include ancora (la cartella del progetto è quella con `.git`); scegliendone
    uno si aggiunge `#include "buffer.h"`, scritto come gli include che il file ha già o relativo alla sua
    cartella (`"../src/buffer.h"`), tra gli include locali in ordine o dopo quelli di sistema. Gli header
    si leggono a poco a poco (al massimo 400) e si rileggono quando cambiano.
  - In Python, scegliendo un modulo della libreria standard (`os`, `json`, `pathlib`, …), una sua
    classe (`Path`, `OrderedDict`, `deque`, …) o un membro dopo il nome del modulo (`os.getcwd`)
    l'editor aggiunge `import os` o `from pathlib import Path` se manca. L'import va nel primo blocco di
    import in ordine alfabetico (gli `import x` prima dei `from x import`); un `from modulo import a, b`
    già presente diventa `from modulo import a, b, Path`; senza import si inserisce dopo i commenti
    iniziali e il docstring. Non fa nulla se il nome è già importato, se il file lo assegna o lo
    definisce (`os = …`), e per i nomi predefiniti (`str`, `ValueError`, …).
  - Sempre in Python si propone anche ciò che definiscono gli altri moduli del progetto (`def`,
    `class` e nomi assegnati al primo livello, esclusi quelli che iniziano con `_` e i file di `venv/`,
    `site-packages/`): scegliendolo si aggiunge `from myapp.utils import slugify` nell'ultimo gruppo di
    import. Il nome del modulo segue i pacchetti (cartelle con `__init__.py`); per un file accanto
    senza pacchetto è il solo nome del file (`from helpers import x`).
  - In JavaScript e TypeScript, scegliendo un modulo di Node (`fs`, `path`, `os`, `http`, `util`,
    `child_process`), una sua classe (`EventEmitter`, `Readable`, …) o un membro dopo il nome del modulo
    (`fs.readFile`) l'editor aggiunge `import fs from 'fs'`, `import { EventEmitter } from 'events'` oppure,
    in un file che usa `require`, `const fs = require('fs')`. Lo stile segue il file: ESM se ha già `import`
    (o per `.ts`, `.mjs`, `.jsx`, o se il `package.json` più vicino ha `"type": "module"`), CJS se ha già
    `require`; apici, punto e virgola e `import * as` come nelle righe esistenti. Un `import { a } from 'mod'`
    su una riga viene esteso (`{ a, b }`). La riga va tra gli import in ordine di modulo, oppure dopo i
    commenti e `'use strict'` se non ce ne sono. Niente se il nome è già importato o dichiarato nel file.
  - Sempre in JavaScript e TypeScript si propone anche ciò che gli altri file del progetto esportano
    (`export function`, `export class`, `export const`, `interface`, `type`, `enum`, `export { … }`,
    `export default`, e in CommonJS `module.exports = { … }` ed `exports.x =`): scegliendolo si aggiunge
    `import { formatDate } from './util'` (o `import Store from './util'` per l'export predefinito, o
    `const { slugify } = require('./helpers')`), con il percorso relativo senza estensione né `/index`
    (l'estensione resta se il file la usa già negli import relativi). Se il modulo è già importato per
    un altro nome, l'import esistente viene esteso. Gli import dei pacchetti restano prima di quelli
    dei file del progetto.
- **Snippet** con segnaposti navigabili con Tab (`for` → ciclo completo, e così via).
- **Esecuzione di comandi** (`Alt+R`, poi `F5` per ripeterlo, `Shift+F5` per interromperlo):
  l'output compare in un pannello `[Output]`. `F8` e `Shift+F8` saltano all'errore
  successivo e precedente (formato `file:riga:colonna` di gcc, tsc, Python).
- **Indicatori Git** nel margine (riga aggiunta, modificata, cancellata), usando il comando
  `git` se presente.
- **Barra di stato** con nome del file, linguaggio, fine riga, codifica, indentazione
  (`Spazi N` o `Tab N`), riga e colonna; con una chiamata in corso mostra la firma.

### Mouse

Clic per posizionare il cursore, doppio clic per la parola, triplo clic per la riga,
trascinamento per selezionare, rotella per scorrere, `Alt+clic` per un altro cursore, clic
sui tab e sulle voci dei pannelli, trascinamento dei bordi degli split e del browser per
ridimensionarli. Si disattiva con `mouse = off` o dalla palette.

## Scorciatoie

Tutte le scorciatoie sono rimappabili (sezione `[keys]` della configurazione) e compaiono
accanto ai comandi nella palette. Quelle predefinite:

### File e buffer

| Tasto | Comando |
|---|---|
| `Ctrl+S` | Salva |
| `Alt+S` | Salva con nome |
| `Alt+A` | Salva tutto |
| `Ctrl+O` | Apri file |
| `Ctrl+P` | Apri rapido (ricerca fuzzy nel progetto) |
| `Ctrl+E` | Elenco dei buffer |
| `Ctrl+W` | Chiudi buffer |
| `Alt+.` · `Ctrl+PgDn` | Buffer successivo |
| `Alt+,` · `Ctrl+PgUp` | Buffer precedente |
| `Ctrl+B` | Mostra/nascondi il browser dei file |
| `Ctrl+Q` | Esci |

### Pannelli

| Tasto | Comando |
|---|---|
| `Alt+\` | Dividi in verticale |
| `Alt+-` | Dividi in orizzontale |
| `Alt+E` | Ridimensiona i pannelli (con le frecce) |
| `Alt+Q` | Chiudi pannello |
| `Alt+O` · `Alt+Destra` · `Ctrl+Tab` | Pannello successivo |
| `Alt+Sinistra` · `Ctrl+Shift+Tab` | Pannello precedente |

### Cerca e vai

| Tasto | Comando |
|---|---|
| `Ctrl+F` | Cerca |
| `Ctrl+R` | Sostituisci |
| `Alt+F` | Cerca nei file |
| `F3` · `Alt+N` | Occorrenza successiva |
| `Shift+F3` · `Alt+P` | Occorrenza precedente |
| `Ctrl+G` | Vai alla riga |
| `Ctrl+T` | Vai al simbolo |
| `F12` | Vai alla definizione (ctags) |
| `Alt+M` | Parentesi corrispondente |

Dentro la barra di ricerca: `Alt+C` maiuscole, `Alt+W` parola intera, `Alt+R` regex, `Esc`
annulla, `Invio` conferma.

### Modifica

| Tasto | Comando |
|---|---|
| `Ctrl+Z` · `Ctrl+Y` | Annulla · Ripeti |
| `Ctrl+A` | Seleziona tutto |
| `Alt+L` · `Alt+W` | Seleziona riga · parola |
| `Ctrl+C` · `Ctrl+X` · `Ctrl+V` | Copia · Taglia · Incolla |
| `Alt+V` | Incolla la voce precedente della cronologia |
| `Ctrl+K` | Elimina righe |
| `Ctrl+J` | Unisci righe |
| `Alt+Su` · `Alt+Giù` | Sposta la riga in su · in giù |
| `Alt+Y` | Duplica la riga sotto |
| `Alt+/` | Commenta/decommenta |
| `Alt+K` | Filtra la selezione con un comando |
| `Tab` · `Shift+Tab` | Indenta · deindenta |

### Cursori e blocchi

| Tasto | Comando |
|---|---|
| `Ctrl+D` | Aggiungi un cursore sulla prossima occorrenza |
| `Alt+D` | Aggiungi cursori su tutte le occorrenze |
| `Alt+Shift+Su` · `Alt+Shift+Giù` | Aggiungi un cursore sopra · sotto |
| `Alt+Z` | Piega/espandi il blocco |

### Esecuzione

| Tasto | Comando |
|---|---|
| `Alt+R` | Esegui un comando di shell |
| `F5` · `Shift+F5` | Ripeti l'ultimo comando · interrompilo |
| `F8` · `Shift+F8` | Errore successivo · precedente |

### Altro

| Tasto | Comando |
|---|---|
| `Alt+X` | Command palette |
| `Alt+T` | Cambia tema |
| `Ctrl+L` | Ridisegna lo schermo |

Nelle liste (palette, buffer, simboli, apertura rapida): `Su`/`Giù`/`PgSu`/`PgGiù` per
muoversi, `Invio` per scegliere, `Esc` per chiudere, e digitando si filtra.

## Command palette (Alt+X)

Elenca **tutti** i comandi e si filtra digitando una parte del nome. Con la ricerca vuota i
comandi sono divisi in sezioni: prima le **azioni** (File, Buffer e pannelli, Cerca e vai,
Modifica, Cursori e blocchi, Esegui, Altro), poi le **impostazioni** che cambiano il
comportamento dell'editor. Ogni riga ha il nome in grassetto, il valore corrente di
un'impostazione (`●` attivo, `○` spento, in evidenza se attivo) e tutte le scorciatoie del
comando, in colonne allineate.

- `Invio` su un'**azione** la esegue e chiude la palette.
- `Invio` su un'**impostazione** la cambia e lascia la palette aperta, con il nuovo valore.
- Riaprendo la palette la selezione torna sull'ultimo comando usato.

Impostazioni disponibili dalla palette: mouse, autosuggest, coppie automatiche, indicatori
Git, appunti di sistema, indentazione (tabulatore, 2, 4, 8 spazi), tema, «Rileva lo stile di
indentazione», e **«Salva le impostazioni…»**, che scrive le scelte fatte nei file di
configurazione (vedi sotto). Altri comandi senza scorciatoia: «Pareggia i pannelli»,
«Duplica riga sopra», «Piega tutto», «Espandi tutto», «Converti l'indentazione in spazi /
tabulatori», «Aggiorna gli indicatori Git».

## Configurazione

L'editor funziona senza alcun file di configurazione. Se vuoi cambiare qualcosa, la cartella
è `$XDG_CONFIG_HOME/xted` oppure `~/.config/xted`:

```
~/.config/xted/
├── config          impostazioni e scorciatoie
├── syntax/         linguaggi aggiuntivi (un file per linguaggio)
├── themes/         temi aggiuntivi (un file per tema)
└── lang/           cataloghi di traduzione (un file per lingua)
```

### Il file `config`

Righe `chiave = valore`, commenti con `#`. Le chiavi prima della prima sezione valgono per
tutti i linguaggi; una sezione `[Linguaggio]` (nome del linguaggio, senza distinguere
maiuscole) le cambia per quel linguaggio; `[keys]` contiene le scorciatoie.

```
# globali
theme = light             # dark (predefinito) o light, o un tema tuo (anche scuro/chiaro)
language = auto           # auto (da LC_ALL, LC_MESSAGES, LANG), en, it, o un codice con catalogo
mouse = on
git = on
clipboard = on            # copie anche negli appunti di sistema (OSC 52)

# valori per tutti i linguaggi
tab_width = 8
expand_tab = off          # Tab inserisce spazi
indent_width = 4          # spazi di un livello (se manca vale tab_width)
trim_trailing_space = off # al salvataggio
final_newline = keep      # keep | always
autosuggest = on
autopair = on
suggest_min = 2           # lettere scritte prima che compaia il popup (1..10)

[Python]
expand_tab = on
indent_width = 4

[Makefile]
expand_tab = off

[keys]
ctrl+shift+d = duplicate_down
alt+j = duplicate_up
alt+y = none              # toglie una scorciatoia
```

I valori sì/no accettano `on`/`off`, `yes`/`no`, `true`/`false`, `1`/`0`. Le chiavi
sconosciute o i valori non validi vengono segnalati all'avvio nella barra dei messaggi.

**Valori predefiniti**: `tab_width = 8`, `expand_tab = off`, `indent_width` come
`tab_width`, `trim_trailing_space = off`, `final_newline = keep`, `autosuggest = on`,
`autopair = on`, `suggest_min = 2`.

Nomi dei tasti: `ctrl+s`, `alt+x`, `shift+f3`, `alt+shift+up`, `alt+left`, `pgdn`, `tab`,
`enter`, …; i nomi dei comandi (`save`, `quick_open`, `symbols`, `toggle_tree`, …) sono gli
id stabili in inglese elencati in `src/commands.c`.

### Il file `.xted` di progetto

Stesso formato del file utente, nella cartella del progetto (o sopra i file). Si usa il più
vicino al file. Le chiavi per linguaggio valgono per i documenti del progetto e battono il
file utente. Le chiavi globali (`theme`, `mouse`, `git`, `clipboard`) valgono per tutta la
sessione e si leggono all'avvio dal `.xted` più vicino al primo file aperto (o alla cartella
corrente).

### `.editorconfig`

Letto dalla cartella del file verso l'alto, fino a un `root = true`. Per i glob supporta `*`,
`**`, `?`, `[]`, `{a,b}` e `{1..3}`. Chiavi usate: `indent_style`, `indent_size`,
`tab_width`, `trim_trailing_whitespace`, `insert_final_newline`.

### Precedenza

Per ogni impostazione di un documento, dal più forte al più debole:

1. un comando dato durante la sessione (dalla palette),
2. il `.xted` del progetto,
3. i valori scritti nel file utente,
4. `.editorconfig`,
5. lo stile rilevato nel testo (tabulatori o N spazi),
6. i valori predefiniti.

### Salvare le impostazioni dalla palette

«Salva le impostazioni…» (`Alt+X`) scrive nei file di configurazione ciò che hai cambiato
con i comandi, senza doverli modificare a mano. Ti chiede **dove** salvare:

- **Utente**: `~/.config/xted/config`, nella sezione globale («tutti i linguaggi») o in
  quella del linguaggio del documento («solo Python»).
- **Progetto**: il `.xted` del progetto (creato nella radice del progetto, riconosciuta da
  `.xted`, `.git`, `composer.json` o `package.json`). Riceve anche tema, mouse, Git e
  appunti.
- **`.editorconfig`**: solo le chiavi di indentazione, nella sezione che corrisponde al file.

Le modifiche degli altri documenti aperti si salvano insieme, in una sezione per linguaggio.
Accanto a ogni destinazione una nota avvisa se verrebbe ignorata da un file più forte («nel
progetto vince il suo .xted», «ignorato: vince il file utente»). I file si modificano riga
per riga, senza perdere commenti e ordine, e si scrivono in modo atomico.

## Lingua dell'interfaccia

I testi dell'editor (messaggi, domande, nomi dei comandi nella palette, errori) sono in
inglese nel codice. La traduzione sta in file esterni, non nel binario: oggi c'è l'italiano,
`lang/it`, nel repository.

- La lingua si sceglie con `language = auto | en | it` nel `config` (o nel `.xted`). Con `auto`
  si legge `LC_ALL`, `LC_MESSAGES` e `LANG` (`it_IT.UTF-8` → italiano); se la lingua non è
  nota o il suo file manca si usa l'inglese, senza errori.
- Dalla palette (`Alt+X`) il comando **Language / Lingua**, tra le impostazioni, passa alla
  lingua successiva. Nella stessa riga compare il file da cui è stata caricata («built in» per
  l'inglese). «Salva le impostazioni…» la salva come chiave `language`.
- Un catalogo è un file di testo chiamato come la lingua (`it`): una riga per testo,
  `testo inglese<TAB>traduzione`, `#` per i commenti, `@name<TAB>Nome della lingua`, e `\n`,
  `\t`, `\\` come escape. Una traduzione con conversioni `printf` (`%s`, `%zu`, …) diverse
  dall'originale viene ignorata.
- I cataloghi si cercano, nell'ordine, in `~/.config/xted/lang/`, in `lang/` e
  `../share/xted/lang/` accanto all'eseguibile, e in `$PREFIX/share/xted/lang/`. `make install`
  li copia lì. Per aggiungere una lingua basta un nuovo file, anche solo in `~/.config/xted/lang/`,
  senza ricompilare.
- `tools/i18n_check.py` confronta i testi del codice con il catalogo e segnala quelli senza
  traduzione o non più usati.

## Temi e linguaggi dell'utente

**Temi** (`themes/NOME`): righe `chiave valore` con i colori `#rrggbb`:
`name`, `foreground`, `background` (anche `none` per usare lo sfondo del terminale), una riga
per classe di sintassi, `match`, `popup`, `popup_sel`, `git_add`, `git_mod`, `git_del`. Ciò
che manca viene dal tema `dark`. I colori si adattano al terminale: truecolor, 256 o 16
colori, rilevati da `$COLORTERM` e `$TERM`; con `NO_COLOR` si disattivano.

I temi con uno sfondo (come `light`) colorano anche barre, tab, riga di stato, elenchi e
cursore con i propri colori; senza sfondo (come `dark`) si usano i colori del terminale.

**Linguaggi** (`syntax/NOME`): file di testo con le stesse direttive dei linguaggi
incorporati (`name`, `ext`, `words`, `comment`, `region`, `decl`, `member`, …); il formato è
documentato all'inizio di `src/langs.c`, che contiene anche tutte le definizioni
incorporate come esempi. Aggiungere un linguaggio non richiede ricompilare.

## Sviluppo

Struttura dei sorgenti (`src/`):

| Modulo | Cosa fa |
|---|---|
| `term` | raw mode, schermo alternativo, segnali, `poll()`, tasti e mouse |
| `buffer`, `undo`, `utf8` | testo, modifiche, undo/redo, UTF-8; indipendenti dall'interfaccia |
| `render` | disegno differenziale, un solo `write()` per frame |
| `editor`, `edit`, `cursors`, `fold` | stato, ciclo degli eventi, comandi di modifica, multi-cursore |
| `syntax`, `langs`, `theme` | evidenziazione, definizioni dei linguaggi, temi |
| `complete`, `symbols`, `stdsym`, `snippet` | autosuggest, indice dei simboli, libreria standard compressa, snippet |
| `search`, `find` | ricerca e sostituzione (regex POSIX) |
| `picker`, `fuzzy`, `files`, `commands`, `keymap` | liste filtrabili, ricerca fuzzy, comandi e scorciatoie |
| `config`, `cfgsave`, `editorconfig`, `indent` | configurazione, scrittura delle impostazioni, rilevamento dell'indentazione |
| `run`, `shell`, `errors`, `git`, `tags` | comandi esterni, errori del compilatore, Git, ctags |
| `session`, `mouse`, `util` | sessioni, mouse, utilità |

Le regole per chi modifica il codice (vincoli, stile, metodo di lavoro) sono in
[`CLAUDE.md`](CLAUDE.md) e le specifiche complete e la roadmap in [`SPEC.md`](SPEC.md).

La libreria standard usata dall'autosuggest sta in `tools/lib/*.txt` (alcuni file sono
generati da `tools/gen_py.py` e `tools/gen_c.py`) e viene compressa nel binario da
`tools/packlib.py`.

I test (`make test`) provano buffer, undo/redo, ricerca, sintassi, configurazione,
salvataggio delle impostazioni, comandi e comportamento dell'editor senza terminale.

## Stato del progetto

Le fasi 1–9 della roadmap di `SPEC.md` sono completate: nucleo, editing, sintassi,
multifile, ricerca, produttività, strumenti, configurazione e rifiniture. Resta fuori il
client LSP (opzionale). Non c'è ancora la guida integrata con `F1`.

## Licenza

xted è distribuito con licenza **MIT**: puoi usarlo, modificarlo e ridistribuirlo liberamente,
anche in programmi chiusi, mantenendo l'avviso di copyright. Il testo completo è nel file
[`LICENSE`](LICENSE).

Copyright (c) 2026 David Bigagli.
