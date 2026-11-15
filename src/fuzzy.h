#ifndef XTED_FUZZY_H
#define XTED_FUZZY_H

/* Case-insensitive subsequence match of query in text. Returns -1 if the
 * query does not match, else a score (higher is better): consecutive
 * characters, matches at word starts and matches inside the file name
 * (after the last '/') weigh more. An empty query matches everything with 0. */
int fuzzy_score(const char *query, const char *text);

#endif
