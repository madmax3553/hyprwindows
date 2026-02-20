#include "hyprconf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static char *strip_comments(const char *src, size_t len, size_t *out_len) {
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }

    size_t i = 0;
    size_t o = 0;
    int in_comment = 0;

    while (i < len) {
        char c = src[i];

        if (in_comment) {
            if (c == '\n') {
                in_comment = 0;
                out[o++] = c;
            }
            i++;
            continue;
        }

        if (c == '#') {
            in_comment = 1;
            i++;
            continue;
        }

        out[o++] = c;
        i++;
    }

    out[o] = '\0';
    if (out_len) {
        *out_len = o;
    }
    return out;
}

static void skip_ws(const char *buf, size_t len, size_t *pos) {
    while (*pos < len && isspace((unsigned char)buf[*pos])) {
        (*pos)++;
    }
}

static char *read_word(const char *buf, size_t len, size_t *pos) {
    skip_ws(buf, len, pos);
    size_t start = *pos;
    while (*pos < len && !isspace((unsigned char)buf[*pos]) && buf[*pos] != '{' && buf[*pos] != '}' && buf[*pos] != '=') {
        (*pos)++;
    }
    if (*pos == start) {
        return NULL;
    }
    size_t wlen = *pos - start;
    char *word = (char *)malloc(wlen + 1);
    if (!word) {
        return NULL;
    }
    memcpy(word, buf + start, wlen);
    word[wlen] = '\0';
    return word;
}

static char *read_value(const char *buf, size_t len, size_t *pos) {
    skip_ws(buf, len, pos);
    size_t start = *pos;
    while (*pos < len && buf[*pos] != '\n' && buf[*pos] != '}') {
        (*pos)++;
    }
    /* trim trailing whitespace */
    size_t end = *pos;
    while (end > start && isspace((unsigned char)buf[end - 1])) {
        end--;
    }
    if (end == start) {
        return NULL;
    }
    size_t vlen = end - start;
    char *val = (char *)malloc(vlen + 1);
    if (!val) {
        return NULL;
    }
    memcpy(val, buf + start, vlen);
    val[vlen] = '\0';
    return val;
}

static int str_eq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static void assign_str(char **dst, char *src) {
    if (*dst) {
        free(src);
        return;
    }
    *dst = src;
}

static int parse_bool_str(const char *s, int *out_set, int *out_val) {
    if (!s) {
        return -1;
    }
    if (str_eq(s, "true") || str_eq(s, "yes") || str_eq(s, "1")) {
        *out_set = 1;
        *out_val = 1;
        return 0;
    }
    if (str_eq(s, "false") || str_eq(s, "no") || str_eq(s, "0")) {
        *out_set = 1;
        *out_val = 0;
        return 0;
    }
    return -1;
}

static void parse_rule_kv(struct rule *r, const char *key, char *val) {
    if (str_eq(key, "name")) {
        assign_str(&r->name, val);
        return;
    }
    if (str_eq(key, "match:class")) {
        assign_str(&r->match.class_re, val);
        return;
    }
    if (str_eq(key, "match:title")) {
        assign_str(&r->match.title_re, val);
        return;
    }
    if (str_eq(key, "match:initialClass") || str_eq(key, "match:initial_class")) {
        assign_str(&r->match.initial_class_re, val);
        return;
    }
    if (str_eq(key, "match:initialTitle") || str_eq(key, "match:initial_title")) {
        assign_str(&r->match.initial_title_re, val);
        return;
    }
    if (str_eq(key, "match:tag")) {
        assign_str(&r->match.tag_re, val);
        return;
    }
    /* skip other match: fields we don't use yet */
    if (strncmp(key, "match:", 6) == 0) {
        free(val);
        return;
    }
    if (str_eq(key, "tag")) {
        assign_str(&r->actions.tag, val);
        return;
    }
    if (str_eq(key, "workspace")) {
        assign_str(&r->actions.workspace, val);
        return;
    }
    if (str_eq(key, "opacity")) {
        assign_str(&r->actions.opacity, val);
        return;
    }
    if (str_eq(key, "size")) {
        assign_str(&r->actions.size, val);
        return;
    }
    if (str_eq(key, "move")) {
        assign_str(&r->actions.move, val);
        return;
    }
    if (str_eq(key, "float")) {
        parse_bool_str(val, &r->actions.float_set, &r->actions.float_val);
        free(val);
        return;
    }
    if (str_eq(key, "center")) {
        parse_bool_str(val, &r->actions.center_set, &r->actions.center_val);
        free(val);
        return;
    }
    /* unknown key - store in extras (grow with doubling) */
    size_t n = r->extras_count;
    /* Check if we need to grow - use doubling strategy */
    size_t cap = n;  /* current allocation = count (tight) */
    if (n == 0 || (n & (n - 1)) == 0) {
        /* need to grow: double or start at 4 */
        size_t new_cap = n == 0 ? 4 : n * 2;
        struct rule_extra *new_extras = realloc(r->extras, new_cap * sizeof(struct rule_extra));
        if (!new_extras) {
            free(val);
            return;
        }
        r->extras = new_extras;
        (void)cap;
    }
    r->extras[n].key = strdup(key);
    r->extras[n].value = val;
    r->extras_count = n + 1;
}

static int parse_windowrule_block(const char *buf, size_t len, size_t *pos, struct rule *r) {
    skip_ws(buf, len, pos);
    if (*pos >= len || buf[*pos] != '{') {
        return -1;
    }
    (*pos)++; /* skip '{' */

    while (*pos < len) {
        skip_ws(buf, len, pos);
        if (*pos >= len) {
            break;
        }
        if (buf[*pos] == '}') {
            (*pos)++;
            return 0;
        }

        char *key = read_word(buf, len, pos);
        if (!key) {
            continue;
        }

        skip_ws(buf, len, pos);
        if (*pos >= len || buf[*pos] != '=') {
            free(key);
            continue;
        }
        (*pos)++; /* skip '=' */

        char *val = read_value(buf, len, pos);
        if (!val) {
            free(key);
            continue;
        }

        parse_rule_kv(r, key, val);
        free(key);
    }

    return -1;
}

int hyprconf_parse_file(const char *path, struct ruleset *out) {
    memset(out, 0, sizeof(*out));

    size_t raw_len = 0;
    char *raw = read_file(path, &raw_len);
    if (!raw) {
        return -1;
    }

    size_t clean_len = 0;
    char *clean = strip_comments(raw, raw_len, &clean_len);
    free(raw);
    if (!clean) {
        return -1;
    }

    /* first pass: count windowrule blocks */
    size_t count = 0;
    size_t pos = 0;
    while (pos < clean_len) {
        char *word = read_word(clean, clean_len, &pos);
        if (!word) {
            break;
        }
        if (str_eq(word, "windowrule")) {
            count++;
            /* skip to end of block */
            skip_ws(clean, clean_len, &pos);
            if (pos < clean_len && clean[pos] == '{') {
                int depth = 1;
                pos++;
                while (pos < clean_len && depth > 0) {
                    if (clean[pos] == '{') depth++;
                    else if (clean[pos] == '}') depth--;
                    pos++;
                }
            }
        }
        free(word);
    }

    if (count == 0) {
        free(clean);
        return 0;
    }

    struct rule *rules = (struct rule *)calloc(count, sizeof(struct rule));
    if (!rules) {
        free(clean);
        return -1;
    }

    /* second pass: parse windowrule blocks */
    pos = 0;
    size_t idx = 0;
    while (pos < clean_len && idx < count) {
        char *word = read_word(clean, clean_len, &pos);
        if (!word) {
            break;
        }
        if (str_eq(word, "windowrule")) {
            if (parse_windowrule_block(clean, clean_len, &pos, &rules[idx]) == 0) {
                idx++;
            }
        }
        free(word);
    }

    free(clean);
    out->rules = rules;
    out->count = idx;
    return 0;
}
