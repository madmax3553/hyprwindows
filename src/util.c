#include "util.h"

#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- regex cache for performance --- */

#define REGEX_CACHE_SIZE 64

struct regex_entry {
    char *pattern;
    regex_t compiled;
    int valid;
};

static struct regex_entry regex_cache[REGEX_CACHE_SIZE];
static size_t regex_cache_next;  /* circular index */

static regex_t *cache_get(const char *pattern) {
    for (size_t i = 0; i < REGEX_CACHE_SIZE; i++) {
        if (regex_cache[i].valid && regex_cache[i].pattern &&
            strcmp(regex_cache[i].pattern, pattern) == 0) {
            return &regex_cache[i].compiled;
        }
    }
    return NULL;
}

static regex_t *cache_put(const char *pattern) {
    struct regex_entry *e = &regex_cache[regex_cache_next];
    regex_cache_next = (regex_cache_next + 1) % REGEX_CACHE_SIZE;

    /* evict old entry */
    if (e->valid) {
        regfree(&e->compiled);
        free(e->pattern);
        e->valid = 0;
    }

    int rc = regcomp(&e->compiled, pattern, REG_EXTENDED | REG_NOSUB | REG_ICASE);
    if (rc != 0) {
        return NULL;
    }
    e->pattern = strdup(pattern);
    if (!e->pattern) {
        regfree(&e->compiled);
        return NULL;
    }
    e->valid = 1;
    return &e->compiled;
}

int regex_match(const char *pattern, const char *text) {
    if (!pattern || !text) {
        return 0;
    }
    regex_t *re = cache_get(pattern);
    if (!re) {
        re = cache_put(pattern);
        if (!re) return 0;
    }
    return regexec(re, text, 0, NULL, 0) == 0;
}

/* --- string utilities --- */

void str_to_lower_inplace(char *s) {
    if (!s) return;
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

/* --- file I/O --- */

char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    if (out_len) {
        *out_len = (size_t)size;
    }
    return buf;
}

char *expand_home(const char *path) {
    if (!path) return NULL;
    if (path[0] != '~') {
        return strdup(path);
    }
    const char *home = getenv("HOME");
    if (!home) {
        home = ".";
    }
    size_t hlen = strlen(home);
    size_t plen = strlen(path);
    /* allocate: hlen + (plen - 1 for skipped '~') + 1 for NUL */
    char *out = (char *)malloc(hlen + plen);
    if (!out) {
        return NULL;
    }
    memcpy(out, home, hlen);
    memcpy(out + hlen, path + 1, plen - 1);
    out[hlen + plen - 1] = '\0';
    return out;
}
