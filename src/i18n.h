#ifndef XTED_I18N_H
#define XTED_I18N_H

#include <stddef.h>

/* Translations. The texts in the code are English; tr() gives back the translation of one from
 * the catalog of the language in use, or the text itself (English, or a text the catalog does not
 * have). A catalog is a text file named after the language ("it") in a lang/ folder:
 *     # comment
 *     @name<TAB>Italiano            the name of the language, in that language
 *     English text<TAB>Testo tradotto
 * with \n, \t and \\ as escapes. A translation whose printf conversions are not those of the
 * English text is dropped when the file is read, so that it cannot break a format.
 * The folders searched, in this order: <config dir>/lang, lang/ and ../share/xted/lang next to
 * the executable, then XTED_DATADIR/lang. */

#ifdef __GNUC__
#define XTED_FORMAT_ARG(n) __attribute__((format_arg(n)))
#else
#define XTED_FORMAT_ARG(n)
#endif

const char *tr(const char *msg) XTED_FORMAT_ARG(1);

/* Sets the folder of the user's configuration (NULL: none); call before i18n_set. */
void i18n_init(const char *cfgdir);
/* Uses a language: "en", a code with a catalog, or "auto" (from LC_ALL, LC_MESSAGES, LANG).
 * Returns 0 on success; -1 if there is no catalog for the language, then English is used. */
int i18n_set(const char *lang);
const char *i18n_code(void);    /* "en", "it", ... */
const char *i18n_name(void);    /* "English", "Italiano", ... */
/* The language codes that can be used, "en" first, then the catalogs found; returns how many. */
size_t i18n_available(char (*codes)[16], size_t max);
/* The language wanted by the environment ("en" if none or not known). */
void i18n_detect(char *out, size_t n);
/* 1 if a and b have the same printf conversions in the same order. */
int i18n_same_formats(const char *a, const char *b);

#endif
