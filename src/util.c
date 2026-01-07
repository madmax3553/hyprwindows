#include "util.h"

#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

int regex_match(const char *pattern, const char *text) {
    if (!pattern || !text) {
        return 0;
    }
    regex_t re;
    int rc = regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB);
    if (rc != 0) {
        return 0;
    }
    rc = regexec(&re, text, 0, NULL, 0);
    regfree(&re);
    return rc == 0;
}

char *str_to_lower(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        out[i] = (char)tolower((unsigned char)s[i]);
    }
    out[len] = '\0';
    return out;
}
