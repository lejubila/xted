#include "keymap.h"
#include "commands.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    const char *name;
    uint32_t code;
} named[] = {
    { "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
    { "home", KEY_HOME }, { "end", KEY_END }, { "pgup", KEY_PGUP }, { "pgdn", KEY_PGDN },
    { "del", KEY_DEL }, { "ins", KEY_INS }, { "tab", KEY_TAB }, { "enter", KEY_ENTER },
    { "esc", KEY_ESC }, { "space", ' ' }, { "backspace", KEY_BACKSPACE },
};

#define NNAMED (sizeof named / sizeof named[0])

Key key_norm(Key k)
{
    /* Ctrl+letter arrives as the control code, with or without the flag */
    if (k.code >= 1 && k.code <= 26 && k.code != KEY_TAB && k.code != KEY_ENTER)
        k.mods &= ~MOD_CTRL;
    if ((k.mods & MOD_ALT) && k.code >= 'A' && k.code <= 'Z') {
        k.code += 32;
        k.mods |= MOD_SHIFT;
    }
    return k;
}

int key_parse(const char *spec, Key *out, char *err, size_t errlen)
{
    Key k;
    const char *p = spec, *plus;
    size_t i;
    char tok[32];

    memset(&k, 0, sizeof k);
    for (;;) {
        size_t n;

        plus = strchr(p, '+');
        /* "alt++" is Alt and the '+' key */
        if (!plus || plus[1] == '\0' || (plus == p && plus[1] == '+'))
            break;
        n = (size_t)(plus - p);
        if (n == 0 || n >= sizeof tok) {
            snprintf(err, errlen, "tasto «%s» non valido", spec);
            return -1;
        }
        memcpy(tok, p, n);
        tok[n] = '\0';
        for (i = 0; tok[i]; i++)
            tok[i] = (char)tolower((unsigned char)tok[i]);
        if (strcmp(tok, "ctrl") == 0)
            k.mods |= MOD_CTRL;
        else if (strcmp(tok, "alt") == 0)
            k.mods |= MOD_ALT;
        else if (strcmp(tok, "shift") == 0)
            k.mods |= MOD_SHIFT;
        else {
            snprintf(err, errlen, "modificatore «%s» sconosciuto (ctrl, alt, shift)", tok);
            return -1;
        }
        p = plus + 1;
    }
    /* what is left is the key */
    if (*p == '\0') {
        snprintf(err, errlen, "manca il tasto in «%s»", spec);
        return -1;
    }
    if (strlen(p) >= sizeof tok) {
        snprintf(err, errlen, "tasto «%s» non valido", spec);
        return -1;
    }
    for (i = 0; p[i]; i++)
        tok[i] = (char)tolower((unsigned char)p[i]);
    tok[i] = '\0';
    k.code = 0;
    for (i = 0; i < NNAMED; i++)
        if (strcmp(tok, named[i].name) == 0)
            k.code = named[i].code;
    if (!k.code && tok[0] == 'f' && isdigit((unsigned char)tok[1])) {
        int n = atoi(tok + 1);

        if (n >= 1 && n <= 12 && (n < 10 ? tok[2] == '\0' : tok[3] == '\0'))
            k.code = KEY_F1 + (uint32_t)(n - 1);
    }
    /* letters are read without regard to case: Shift has to be written */
    if (!k.code && strlen(p) == 1 && (unsigned char)p[0] >= 0x20 && (unsigned char)p[0] < 0x7F)
        k.code = (uint32_t)(unsigned char)tok[0];
    if (!k.code && (unsigned char)p[0] >= 0x80) {
        /* one non-ASCII character */
        const unsigned char *u = (const unsigned char *)p;
        size_t len = strlen(p);

        if ((u[0] & 0xE0) == 0xC0 && len == 2)
            k.code = (uint32_t)(u[0] & 0x1F) << 6 | (u[1] & 0x3F);
        else if ((u[0] & 0xF0) == 0xE0 && len == 3)
            k.code = (uint32_t)(u[0] & 0x0F) << 12 | (uint32_t)(u[1] & 0x3F) << 6 | (u[2] & 0x3F);
    }
    if (!k.code) {
        snprintf(err, errlen, "tasto «%s» sconosciuto", p);
        return -1;
    }
    if (k.mods & MOD_CTRL) {
        if (k.code >= 'a' && k.code <= 'z') {
            uint32_t c = k.code & 0x1f;

            if (c == KEY_TAB || c == KEY_ENTER || c == 8) {
                snprintf(err, errlen,
                         "ctrl+%c coincide con Tab, Invio o Backspace: il terminale non li distingue",
                         (int)k.code);
                return -1;
            }
            k.code = c;
            k.mods &= ~MOD_CTRL;
        }
    }
    *out = key_norm(k);
    return 0;
}

void key_format(Key k, int pretty, char *out, size_t n)
{
    char buf[48] = "";
    char key[16];
    size_t i;
    int ctrl = (k.mods & MOD_CTRL) != 0;
    uint32_t code = k.code;

    if (code >= 1 && code <= 26 && code != KEY_TAB && code != KEY_ENTER) {
        ctrl = 1;
        code += 'a' - 1;
    }
    key[0] = '\0';
    for (i = 0; i < NNAMED; i++)
        if (named[i].code == code)
            snprintf(key, sizeof key, "%s", named[i].name);
    if (!key[0] && code >= KEY_F1 && code <= KEY_F12)
        snprintf(key, sizeof key, "f%u", (unsigned)(code - KEY_F1 + 1));
    if (!key[0] && code >= 0x20 && code < 0x7F)
        snprintf(key, sizeof key, "%c", (int)code);
    if (!key[0])
        snprintf(key, sizeof key, "?");
    if (ctrl)
        strcat(buf, pretty ? "Ctrl+" : "ctrl+");
    if (k.mods & MOD_ALT)
        strcat(buf, pretty ? "Alt+" : "alt+");
    if (k.mods & MOD_SHIFT)
        strcat(buf, pretty ? "Shift+" : "shift+");
    if (pretty && key[0] >= 'a' && key[0] <= 'z')
        key[0] = (char)(key[0] - 32);
    strcat(buf, key);
    snprintf(out, n, "%s", buf);
}

/* ---- the map ------------------------------------------------------------- */

static const struct {
    const char *key, *id;
} extras[] = {
    { "alt+right", "next_win" }, { "ctrl+tab", "next_win" }, { "ctrl+shift+tab", "prev_win" },
    { "ctrl+pgdn", "next_buffer" }, { "ctrl+pgup", "prev_buffer" },
    { "alt+n", "find_next" }, { "alt+p", "find_prev" },
};

int command_by_id(const char *id)
{
    size_t i;

    for (i = 0; i < ncommands; i++)
        if (strcmp(commands[i].id, id) == 0)
            return (int)i;
    return -1;
}

void keymap_bind(Keymap *m, Key k, int cmd)
{
    size_t i;

    k = key_norm(k);
    for (i = 0; i < m->n; i++)
        if (m->b[i].key.code == k.code && m->b[i].key.mods == k.mods) {
            m->b[i].cmd = cmd;
            return;
        }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 64;
        m->b = realloc(m->b, m->cap * sizeof *m->b);
        if (!m->b)
            abort();
    }
    m->b[m->n].key = k;
    m->b[m->n].cmd = cmd;
    m->n++;
}

void keymap_init(Keymap *m)
{
    size_t i;
    char err[64];
    Key k;

    memset(m, 0, sizeof *m);
    for (i = 0; i < ncommands; i++)
        if (commands[i].key[0] && key_parse(commands[i].key, &k, err, sizeof err) == 0)
            keymap_bind(m, k, (int)i);
    for (i = 0; i < sizeof extras / sizeof extras[0]; i++) {
        int c = command_by_id(extras[i].id);

        if (c >= 0 && key_parse(extras[i].key, &k, err, sizeof err) == 0)
            keymap_bind(m, k, c);
    }
}

void keymap_free(Keymap *m)
{
    free(m->b);
    memset(m, 0, sizeof *m);
}

int keymap_find(const Keymap *m, Key k)
{
    size_t i;

    k = key_norm(k);
    for (i = 0; i < m->n; i++)
        if (m->b[i].key.code == k.code && m->b[i].key.mods == k.mods)
            return m->b[i].cmd;
    return -1;
}

void keymap_label(const Keymap *m, int cmd, char *out, size_t n)
{
    size_t i;

    out[0] = '\0';
    for (i = 0; i < m->n; i++)
        if (m->b[i].cmd == cmd) {
            key_format(m->b[i].key, 1, out, n);
            return;
        }
}
