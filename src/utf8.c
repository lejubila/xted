#include "utf8.h"

typedef struct {
    uint32_t lo, hi;
} Range;

/* Zero-width: combining marks, joiners, variation selectors. */
static const Range combining[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F},
    {0x202A, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};

/* Wide (East Asian W/F and emoji presentation); an approximation of
 * Unicode's EastAsianWidth.txt, sorted for binary search. */
static const Range wide[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
    {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE}, {0x2614, 0x2615},
    {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE},
    {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5},
    {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
    {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0xA4CF}, {0xA960, 0xA97F},
    {0xAC00, 0xD7A3}, {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F},
    {0xFF01, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0},
    {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440},
    {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7},
    {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF},
    {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

static int in_table(const Range *t, size_t n, uint32_t cp)
{
    size_t lo = 0, hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (cp < t[mid].lo)
            hi = mid;
        else if (cp > t[mid].hi)
            lo = mid + 1;
        else
            return 1;
    }
    return 0;
}

int utf8_decode(const char *s, size_t len, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned c = u[0];
    size_t n, i;
    uint32_t v, min;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2; v = c & 0x1F; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3; v = c & 0x0F; min = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4; v = c & 0x07; min = 0x10000;
    } else {
        *cp = 0xFFFD;
        return 1;
    }
    if (n > len) {
        *cp = 0xFFFD;
        return 1;
    }
    for (i = 1; i < n; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = (v << 6) | (u[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        *cp = 0xFFFD;
        return 1;
    }
    *cp = v;
    return (int)n;
}

int utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

int cp_is_combining(uint32_t cp)
{
    return cp >= 0x300 && in_table(combining, NELEM(combining), cp);
}

int cp_is_word(uint32_t cp)
{
    return cp >= 0x80 || cp == '_' || (cp >= '0' && cp <= '9') ||
           (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

int cp_width(uint32_t cp)
{
    if (cp < 0x300)
        return 1;
    if (cp_is_combining(cp))
        return 0;
    return in_table(wide, NELEM(wide), cp) ? 2 : 1;
}

int char_cols(uint32_t cp, size_t col, int tabw)
{
    if (cp == '\t')
        return tabw - (int)(col % (size_t)tabw);
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        return 2;
    return cp_width(cp);
}

size_t utf8_cols(const char *s, size_t len, size_t upto, int tabw)
{
    size_t i = 0, col = 0;
    uint32_t cp;

    while (i < upto && i < len) {
        i += (size_t)utf8_decode(s + i, len - i, &cp);
        col += (size_t)char_cols(cp, col, tabw);
    }
    return col;
}

size_t utf8_byte_at_col(const char *s, size_t len, size_t col, int tabw)
{
    size_t i = 0, c = 0;
    uint32_t cp;

    while (i < len) {
        int n = utf8_decode(s + i, len - i, &cp);
        int w = char_cols(cp, c, tabw);

        if (w > 0 && c + (size_t)w > col)
            return i;
        c += (size_t)w;
        i += (size_t)n;
    }
    return len;
}

size_t utf8_next(const char *s, size_t len, size_t i)
{
    uint32_t cp;

    if (i >= len)
        return len;
    return i + (size_t)utf8_decode(s + i, len - i, &cp);
}

size_t utf8_prev(const char *s, size_t i)
{
    size_t j;
    int steps = 0;
    uint32_t cp;

    if (i == 0)
        return 0;
    j = i - 1;
    while (j > 0 && steps < 3 && ((unsigned char)s[j] & 0xC0) == 0x80) {
        j--;
        steps++;
    }
    if (j + (size_t)utf8_decode(s + j, i - j, &cp) != i)
        return i - 1;
    return j;
}

size_t grapheme_next(const char *s, size_t len, size_t i)
{
    uint32_t cp;

    i = utf8_next(s, len, i);
    while (i < len) {
        int n = utf8_decode(s + i, len - i, &cp);

        if (!cp_is_combining(cp))
            break;
        i += (size_t)n;
    }
    return i;
}

size_t grapheme_prev(const char *s, size_t len, size_t i)
{
    uint32_t cp;

    i = utf8_prev(s, i);
    while (i > 0) {
        utf8_decode(s + i, len - i, &cp);
        if (!cp_is_combining(cp))
            break;
        i = utf8_prev(s, i);
    }
    return i;
}
