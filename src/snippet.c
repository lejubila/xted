#include "snippet.h"

#include <ctype.h>
#include <string.h>

static const SnipDef snips_c[] = {
    { "for", "for (${1:int i = 0}; ${2:i < n}; ${3:i++}) {\n\t$0\n}" },
    { "while", "while (${1:cond}) {\n\t$0\n}" },
    { "do", "do {\n\t$0\n} while (${1:cond});" },
    { "if", "if (${1:cond}) {\n\t$0\n}" },
    { "ife", "if (${1:cond}) {\n\t$2\n} else {\n\t$0\n}" },
    { "switch", "switch (${1:expr}) {\ncase ${2:x}:\n\t$0\n\tbreak;\ndefault:\n\tbreak;\n}" },
    { "func", "${1:void} ${2:name}(${3:void})\n{\n\t$0\n}" },
    { "main", "int main(int argc, char **argv)\n{\n\t$0\n\treturn 0;\n}" },
    { "struct", "typedef struct {\n\t$0\n} ${1:Name};" },
    { "inc", "#include <${1:stdio.h}>" },
    { "guard", "#ifndef ${1:NAME_H}\n#define $1\n\n$0\n\n#endif" },
};

static const SnipDef snips_py[] = {
    { "for", "for ${1:item} in ${2:items}:\n\t${0:pass}" },
    { "while", "while ${1:cond}:\n\t${0:pass}" },
    { "if", "if ${1:cond}:\n\t${0:pass}" },
    { "ife", "if ${1:cond}:\n\t${2:pass}\nelse:\n\t${0:pass}" },
    { "def", "def ${1:name}(${2:args}):\n\t${0:pass}" },
    { "class", "class ${1:Name}:\n\tdef __init__(self${2:}):\n\t\t${0:pass}" },
    { "main", "if __name__ == \"__main__\":\n\t${0:main()}" },
    { "try", "try:\n\t${1:pass}\nexcept ${2:Exception} as ${3:e}:\n\t${0:raise}" },
    { "with", "with ${1:open(path)} as ${2:f}:\n\t${0:pass}" },
};

static const SnipDef snips_js[] = {
    { "for", "for (let ${1:i} = 0; $1 < ${2:n}; $1++) {\n\t$0\n}" },
    { "forof", "for (const ${1:item} of ${2:items}) {\n\t$0\n}" },
    { "while", "while (${1:cond}) {\n\t$0\n}" },
    { "if", "if (${1:cond}) {\n\t$0\n}" },
    { "ife", "if (${1:cond}) {\n\t$2\n} else {\n\t$0\n}" },
    { "function", "function ${1:name}(${2:args}) {\n\t$0\n}" },
    { "arrow", "const ${1:name} = (${2:args}) => {\n\t$0\n};" },
    { "class", "class ${1:Name} {\n\tconstructor(${2:args}) {\n\t\t$0\n\t}\n}" },
    { "try", "try {\n\t$1\n} catch (${2:e}) {\n\t$0\n}" },
    { "log", "console.log(${1:value});$0" },
};

static const SnipDef snips_php[] = {
    { "for", "for ($i = 0; $i < ${1:$n}; $i++) {\n\t$0\n}" },
    { "foreach", "foreach ($$${1:items} as $$${2:item}) {\n\t$0\n}" },
    { "if", "if (${1:cond}) {\n\t$0\n}" },
    { "ife", "if (${1:cond}) {\n\t$2\n} else {\n\t$0\n}" },
    { "function", "function ${1:name}(${2:$args}) {\n\t$0\n}" },
    { "class", "class ${1:Name}\n{\n\tpublic function __construct(${2:})\n\t{\n\t\t$0\n\t}\n}" },
    { "try", "try {\n\t$1\n} catch (${2:Exception} $$${3:e}) {\n\t$0\n}" },
    { "echo", "echo ${1:$value};$0" },
    { "php", "<?php\n\n$0" },
};

static const SnipDef snips_sh[] = {
    { "for", "for ${1:i} in ${2:list}; do\n\t$0\ndone" },
    { "while", "while ${1:cond}; do\n\t$0\ndone" },
    { "if", "if ${1:cond}; then\n\t$0\nfi" },
    { "ife", "if ${1:cond}; then\n\t$2\nelse\n\t$0\nfi" },
    { "case", "case ${1:\"$var\"} in\n\t${2:pattern})\n\t\t$0\n\t\t;;\nesac" },
    { "func", "${1:name}() {\n\t$0\n}" },
};

#define N(a) (sizeof a / sizeof a[0])

/* Markdown: what a line starts with (the popup offers them from the first character typed). */
static const SnipDef snips_md[] = {
    { "#", "# $0" },
    { "##", "## $0" },
    { "###", "### $0" },
    { "####", "#### $0" },
    { "#####", "##### $0" },
    { "######", "###### $0" },
    { "-", "- $0" },
    { "- [ ]", "- [ ] $0" },
    { "*", "* $0" },
    { "1.", "1. $0" },
    { ">", "> $0" },
    { "```", "```$1\n$0\n```" },
    { "|", "| ${1:A} | ${2:B} |\n| --- | --- |\n| $3 | $4 |\n$0" },
    { "---", "---\n$0" },
    { "[", "[${1:text}](${2:url})$0" },
    { "![", "![${1:text}](${2:image})$0" },
};

const SnipDef *snip_lang(const char *lang, size_t *n)
{
    *n = 0;
    if (!lang)
        return NULL;
    if (strcmp(lang, "C") == 0) {
        *n = N(snips_c);
        return snips_c;
    }
    if (strcmp(lang, "Python") == 0) {
        *n = N(snips_py);
        return snips_py;
    }
    if (strcmp(lang, "PHP") == 0 || strcmp(lang, "PHP/HTML") == 0) {
        *n = N(snips_php);
        return snips_php;
    }
    if (strcmp(lang, "JavaScript") == 0 || strcmp(lang, "TypeScript") == 0) {
        *n = N(snips_js);
        return snips_js;
    }
    if (strcmp(lang, "Shell") == 0) {
        *n = N(snips_sh);
        return snips_sh;
    }
    if (strcmp(lang, "Markdown") == 0) {
        *n = N(snips_md);
        return snips_md;
    }
    return NULL;
}

typedef struct {
    int num;
    size_t off, len;
} RawStop;

void snip_parse(const char *body, SBuf *text, SnipStops *out)
{
    RawStop raw[SNIP_MAX + 6];
    size_t nraw = 0, base = text->len, i = 0, k;
    int seen[10] = { 0 }, have0 = 0;

    out->n = 0;
    while (body[i]) {
        if (body[i] == '$' && body[i + 1] == '$') {
            sbuf_add(text, "$", 1);
            i += 2;
        } else if (body[i] == '$' && isdigit((unsigned char)body[i + 1])) {
            int num = body[i + 1] - '0';

            if (nraw < sizeof raw / sizeof raw[0] && !seen[num]) {
                raw[nraw].num = num;
                raw[nraw].off = text->len - base;
                raw[nraw].len = 0;
                nraw++;
                seen[num] = 1;
            }
            i += 2;
        } else if (body[i] == '$' && body[i + 1] == '{' && isdigit((unsigned char)body[i + 2]) &&
                   body[i + 3] == ':') {
            int num = body[i + 2] - '0';
            size_t j = i + 4, start = text->len;

            while (body[j] && body[j] != '}') {
                if (body[j] == '$' && body[j + 1] == '$')
                    j++;                /* "$$" is a literal dollar here too */
                sbuf_add(text, body + j, 1);
                j++;
            }
            if (nraw < sizeof raw / sizeof raw[0] && !seen[num]) {
                raw[nraw].num = num;
                raw[nraw].off = start - base;
                raw[nraw].len = text->len - start;
                nraw++;
                seen[num] = 1;
            }
            i = body[j] ? j + 1 : j;
        } else {
            sbuf_add(text, body + i, 1);
            i++;
        }
    }
    /* visiting order: 1..9, then 0 (explicit, or implicitly at the end) */
    for (k = 1; k <= 9; k++) {
        size_t r;

        for (r = 0; r < nraw; r++)
            if (raw[r].num == (int)k && out->n < SNIP_MAX) {
                out->stop[out->n].off = raw[r].off;
                out->stop[out->n].len = raw[r].len;
                out->n++;
            }
    }
    for (k = 0; k < nraw; k++)
        if (raw[k].num == 0) {
            out->stop[out->n].off = raw[k].off;
            out->stop[out->n].len = raw[k].len;
            out->n++;
            have0 = 1;
        }
    if (!have0 && out->n < SNIP_MAX) {
        out->stop[out->n].off = text->len - base;
        out->stop[out->n].len = 0;
        out->n++;
    }
}

int snip_adjust(SnipSession *s, size_t nlines_before, size_t nlines_now, size_t len_before,
                size_t len_now, size_t cy, size_t cx)
{
    size_t i, y, x, len;
    long delta = (long)len_now - (long)len_before;

    if (!s->on)
        return 0;
    y = s->stop[s->cur].y;
    x = s->stop[s->cur].x;
    len = s->stop[s->cur].len;
    if (nlines_now != nlines_before || cy != y)
        goto end;
    if (delta < 0 && len < (size_t)-delta)
        goto end;
    len = (size_t)((long)len + delta);
    if (cx < x || cx > x + len)
        goto end;
    s->stop[s->cur].len = len;
    if (delta != 0) {
        size_t old_end = x + (size_t)((long)len - delta);

        for (i = 0; i < s->n; i++)
            if (i != s->cur && s->stop[i].y == y && s->stop[i].x >= old_end)
                s->stop[i].x = (size_t)((long)s->stop[i].x + delta);
    }
    return 1;
end:
    s->on = 0;
    return 0;
}
