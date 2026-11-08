#include "fuzzy.h"

#include <ctype.h>
#include <string.h>

static int is_sep(char c)
{
    return c == '/' || c == '_' || c == '-' || c == '.' || c == ' ';
}

/* Leftmost match of q in t; -1 if there is none. */
static int score_from(const char *q, const char *t)
{
    size_t qi = 0, i, first = 0, last = 0, qn = strlen(q);
    int score = 0;
    long prev = -2;

    for (i = 0; t[i] && q[qi]; i++) {
        if (tolower((unsigned char)t[i]) != tolower((unsigned char)q[qi]))
            continue;
        score += 1;
        if (i == 0 || is_sep(t[i - 1]))
            score += 10;
        if (prev == (long)i - 1)
            score += 6;
        if (q[qi] == t[i])
            score += 1;     /* exact case */
        if (qi == 0)
            first = i;
        last = i;
        prev = (long)i;
        qi++;
    }
    if (q[qi])
        return -1;
    return score - (int)((last - first + 1 - qn) / 4);
}

int fuzzy_score(const char *query, const char *text)
{
    const char *slash = strrchr(text, '/'), *base = slash ? slash + 1 : text;
    int whole, inbase = -1, best;

    if (!query[0])
        return 0;
    whole = score_from(query, text);
    if (base[0])
        inbase = score_from(query, base);
    if (whole < 0 && inbase < 0)
        return -1;
    best = whole;
    if (inbase >= 0 && inbase + 20 > best)
        best = inbase + 20;
    return best - (int)(strlen(text) / 8);
}
