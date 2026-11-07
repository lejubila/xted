#ifndef KED_UTF8_H
#define KED_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Decodes one code point from s (len > 0). Always consumes >= 1 byte; an
 * invalid sequence yields U+FFFD and consumes a single byte. */
int utf8_decode(const char *s, size_t len, uint32_t *cp);
/* Encodes cp into out (>= 4 bytes); returns the byte count. */
int utf8_encode(uint32_t cp, char *out);

int cp_is_combining(uint32_t cp);
/* Terminal cells of a printable code point: 0, 1 or 2. */
int cp_width(uint32_t cp);
/* Cells a code point occupies when drawn at display column col
 * (tabs expand to the next stop, control characters show as ^X). */
int char_cols(uint32_t cp, size_t col, int tabw);

/* Display column reached after the first `upto` bytes of s. */
size_t utf8_cols(const char *s, size_t len, size_t upto, int tabw);
/* Byte offset of the character covering display column col (len if past end). */
size_t utf8_byte_at_col(const char *s, size_t len, size_t col, int tabw);

/* Offset after the code point at i / start of the code point before i. */
size_t utf8_next(const char *s, size_t len, size_t i);
size_t utf8_prev(const char *s, size_t i);
/* Same, but a base character and its combining marks move together. */
size_t grapheme_next(const char *s, size_t len, size_t i);
size_t grapheme_prev(const char *s, size_t len, size_t i);

#endif
