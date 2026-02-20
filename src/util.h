#ifndef HYPRWINDOWS_UTIL_H
#define HYPRWINDOWS_UTIL_H

#include <stddef.h>

/* regex matching (with optional caching) */
int regex_match(const char *pattern, const char *text);

/* string utilities */
void str_to_lower_inplace(char *s);

/* file I/O - shared across modules */
char *read_file(const char *path, size_t *out_len);
char *expand_home(const char *path);

#endif
