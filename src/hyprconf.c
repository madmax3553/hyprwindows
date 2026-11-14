#include "hyprconf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

/*
 * Parser for Hyprland's Lua window rules. Not a Lua interpreter.
 * Understands both call forms:
 *   hl.window_rule({ name = "...", match = { class = "..." }, float = true })
 *   local rules = { { name = "...", match = { class = [[...]] } } }
 *   hl.window_rule(rule)
 * and both quote styles: "..." and [[...]].
 */

static int is_ident(unsigned char c) {
    return isalnum(c) || c == '_';
}

static void lua_skip(const char *buf, size_t len, size_t *pos) {
    while (*pos < len) {
        unsigned char c = (unsigned char)buf[*pos];
        if (isspace(c)) {
            (*pos)++;
            continue;
        }
        if (c == '-' && *pos + 1 < len && buf[*pos + 1] == '-') {
            *pos += 2;
            if (*pos + 1 < len && buf[*pos] == '[' && buf[*pos + 1] == '[') {
                *pos += 2;
                while (*pos + 1 < len && !(buf[*pos] == ']' && buf[*pos + 1] == ']')) {
                    (*pos)++;
                }
                if (*pos + 1 < len) *pos += 2;
            } else {
                while (*pos < len && buf[*pos] != '\n') (*pos)++;
            }
            continue;
        }
        break;
    }
}

static int at_window_rule(const char *buf, size_t len, size_t pos) {
    const char *word = "window_rule";
    size_t wlen = 11;
    if (pos + wlen > len || memcmp(buf + pos, word, wlen) != 0) return 0;
    if (pos > 0 && is_ident((unsigned char)buf[pos - 1])) return 0;
    if (pos + wlen < len && is_ident((unsigned char)buf[pos + wlen])) return 0;
    return 1;
}

static char *read_ident(const char *buf, size_t len, size_t *pos) {
    size_t start = *pos;
    if (start >= len || !is_ident((unsigned char)buf[start]) || isdigit((unsigned char)buf[start])) {
        return NULL;
    }
    while (*pos < len && is_ident((unsigned char)buf[*pos])) (*pos)++;
    size_t n = *pos - start;
    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, buf + start, n);
    s[n] = '\0';
    return s;
}

static int long_string_level(const char *buf, size_t len, size_t pos) {
    if (pos >= len || buf[pos] != '[') return -1;
    size_t i = pos + 1;
    int eq = 0;
    while (i < len && buf[i] == '=') {
        eq++;
        i++;
    }
    if (i < len && buf[i] == '[') return eq;
    return -1;
}

static char *read_long_string(const char *buf, size_t len, size_t *pos) {
    int level = long_string_level(buf, len, *pos);
    if (level < 0) return NULL;
    *pos += (size_t)(2 + level); /* [==[ */

    size_t start = *pos;
    while (*pos < len) {
        if (buf[*pos] == ']') {
            size_t j = *pos + 1;
            int eq = 0;
            while (j < len && buf[j] == '=') {
                eq++;
                j++;
            }
            if (eq == level && j < len && buf[j] == ']') {
                size_t n = *pos - start;
                char *s = malloc(n + 1);
                if (!s) return NULL;
                memcpy(s, buf + start, n);
                s[n] = '\0';
                *pos = j + 1;
                return s;
            }
        }
        (*pos)++;
    }
    return NULL;
}

static char *read_lua_string(const char *buf, size_t len, size_t *pos) {
    if (*pos >= len || (buf[*pos] != '"' && buf[*pos] != '\'')) return NULL;
    char q = buf[(*pos)++];
    size_t cap = 32, n = 0;
    char *out = malloc(cap);
    if (!out) return NULL;

    while (*pos < len && buf[*pos] != q) {
        char c = buf[*pos];
        if (c == '\\' && *pos + 1 < len) {
            char e = buf[++(*pos)];
            if (e == 'n') c = '\n';
            else if (e == 't') c = '\t';
            else if (e == 'r') c = '\r';
            else c = e;
        }
        if (n + 1 >= cap) {
            cap *= 2;
            char *next = realloc(out, cap);
            if (!next) { free(out); return NULL; }
            out = next;
        }
        out[n++] = c;
        (*pos)++;
    }
    if (*pos >= len || buf[*pos] != q) {
        free(out);
        return NULL;
    }
    (*pos)++;
    out[n] = '\0';
    return out;
}

static char *read_any_string(const char *buf, size_t len, size_t *pos) {
    if (*pos < len && buf[*pos] == '[') return read_long_string(buf, len, pos);
    return read_lua_string(buf, len, pos);
}

static char *lua_quote_dup(const char *s) {
    if (!s) s = "";
    size_t extra = 2;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '\n' || *p == '\t') extra++;
    }
    char *out = malloc(strlen(s) + extra + 1);
    if (!out) return NULL;
    char *w = out;
    *w++ = '"';
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"') {
            *w++ = '\\';
            *w++ = *p;
        } else if (*p == '\n') {
            *w++ = '\\';
            *w++ = 'n';
        } else if (*p == '\t') {
            *w++ = '\\';
            *w++ = 't';
        } else {
            *w++ = *p;
        }
    }
    *w++ = '"';
    *w = '\0';
    return out;
}

/* copy a balanced (), {}, or [] span, including the delimiters. pos at the opener. */
static char *capture_span(const char *buf, size_t len, size_t *pos, char open, char close) {
    if (*pos >= len || buf[*pos] != open) return NULL;
    size_t start = *pos;
    int depth = 0;
    int in_str = 0;
    char q = 0;
    while (*pos < len) {
        char c = buf[*pos];
        if (in_str) {
            if (c == '\\' && *pos + 1 < len) { *pos += 2; continue; }
            if (c == q) in_str = 0;
            (*pos)++;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_str = 1;
            q = c;
            (*pos)++;
            continue;
        }
        if (long_string_level(buf, len, *pos) >= 0) {
            char *s = read_long_string(buf, len, pos);
            if (!s) return NULL;
            free(s);
            continue;
        }
        if (c == '-' && *pos + 1 < len && buf[*pos + 1] == '-') {
            size_t cpos = *pos;
            lua_skip(buf, len, &cpos);
            /* lua_skip only advances when it sees a comment or space; force past this one */
            if (cpos == *pos) cpos += 2;
            *pos = cpos;
            continue;
        }
        if (c == open) depth++;
        else if (c == close) {
            depth--;
            (*pos)++;
            if (depth == 0) {
                size_t n = *pos - start;
                char *s = malloc(n + 1);
                if (!s) return NULL;
                memcpy(s, buf + start, n);
                s[n] = '\0';
                return s;
            }
            continue;
        }
        (*pos)++;
    }
    return NULL;
}

static void add_extra(struct rule *r, const char *key, char *literal) {
    if (!r || !key || !literal) {
        free(literal);
        return;
    }
    size_t n = r->extras_count;
    struct rule_extra *next = realloc(r->extras, (n + 1) * sizeof(struct rule_extra));
    if (!next) {
        free(literal);
        return;
    }
    r->extras = next;
    r->extras[n].key = strdup(key);
    if (!r->extras[n].key) {
        free(literal);
        return;
    }
    r->extras[n].value = literal;
    r->extras_count = n + 1;
}

static void assign_str(char **dst, char *src) {
    if (*dst) {
        free(src);
        return;
    }
    *dst = src;
}

static int parse_bool_text(const char *s, int *out_set, int *out_val) {
    if (!s) return -1;
    if (strcmp(s, "true") == 0 || strcmp(s, "yes") == 0 || strcmp(s, "1") == 0) {
        *out_set = 1;
        *out_val = 1;
        return 0;
    }
    if (strcmp(s, "false") == 0 || strcmp(s, "no") == 0 || strcmp(s, "0") == 0) {
        *out_set = 1;
        *out_val = 0;
        return 0;
    }
    return -1;
}

static int parse_int_text(const char *s, int *out) {
    if (!s || !out) return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    *out = (int)v;
    return 0;
}

/* "{ 800, 600 }" or "{ \"20\", \"monitor_h-120\" }" -> "800 600". NULL if not a pair. */
static char *flatten_pair(const char *raw) {
    if (!raw || raw[0] != '{') return NULL;
    size_t len = strlen(raw);
    size_t pos = 1;
    char *parts[2] = {0};
    int n = 0;
    int ok = 1;

    while (pos < len && n < 2) {
        lua_skip(raw, len, &pos);
        if (pos >= len || raw[pos] == '}') break;
        if (raw[pos] == '"' || raw[pos] == '\'') {
            parts[n] = read_lua_string(raw, len, &pos);
            if (!parts[n]) { ok = 0; break; }
        } else if (raw[pos] == '-' || isdigit((unsigned char)raw[pos])) {
            size_t start = pos;
            if (raw[pos] == '-') pos++;
            while (pos < len && (isdigit((unsigned char)raw[pos]) || raw[pos] == '.')) pos++;
            size_t nlen = pos - start;
            parts[n] = malloc(nlen + 1);
            if (!parts[n]) { ok = 0; break; }
            memcpy(parts[n], raw + start, nlen);
            parts[n][nlen] = '\0';
        } else {
            ok = 0;
            break;
        }
        n++;
        lua_skip(raw, len, &pos);
        if (pos < len && raw[pos] == ',') pos++;
    }
    lua_skip(raw, len, &pos);
    if (!ok || n != 2 || pos >= len || raw[pos] != '}') {
        free(parts[0]);
        free(parts[1]);
        return NULL;
    }
    size_t out_len = strlen(parts[0]) + 1 + strlen(parts[1]);
    char *out = malloc(out_len + 1);
    if (out) snprintf(out, out_len + 1, "%s %s", parts[0], parts[1]);
    free(parts[0]);
    free(parts[1]);
    return out;
}

static void apply_field(struct rule *r, const char *key, const char *text, char *literal, int in_match) {
    if (!key) {
        free(literal);
        return;
    }

    if (in_match) {
        if (strcmp(key, "class") == 0) { assign_str(&r->match.class_re, strdup(text ? text : "")); free(literal); return; }
        if (strcmp(key, "title") == 0) { assign_str(&r->match.title_re, strdup(text ? text : "")); free(literal); return; }
        if (strcmp(key, "initial_class") == 0 || strcmp(key, "initialClass") == 0) {
            assign_str(&r->match.initial_class_re, strdup(text ? text : ""));
            free(literal);
            return;
        }
        if (strcmp(key, "initial_title") == 0 || strcmp(key, "initialTitle") == 0) {
            assign_str(&r->match.initial_title_re, strdup(text ? text : ""));
            free(literal);
            return;
        }
        if (strcmp(key, "tag") == 0) { assign_str(&r->match.tag_re, strdup(text ? text : "")); free(literal); return; }

        char prefixed[128];
        snprintf(prefixed, sizeof(prefixed), "match.%s", key);
        add_extra(r, prefixed, literal);
        return;
    }

    if (strcmp(key, "name") == 0) { assign_str(&r->name, strdup(text ? text : "")); free(literal); return; }
    if (strcmp(key, "tag") == 0) { assign_str(&r->actions.tag, strdup(text ? text : "")); free(literal); return; }
    if (strcmp(key, "workspace") == 0) { assign_str(&r->actions.workspace, strdup(text ? text : "")); free(literal); return; }
    if (strcmp(key, "opacity") == 0) { assign_str(&r->actions.opacity, strdup(text ? text : "")); free(literal); return; }
    if (strcmp(key, "size") == 0) { assign_str(&r->actions.size, strdup(text ? text : "")); free(literal); return; }
    if (strcmp(key, "move") == 0) { assign_str(&r->actions.move, strdup(text ? text : "")); free(literal); return; }
    if (strcmp(key, "float") == 0) {
        parse_bool_text(text, &r->actions.float_set, &r->actions.float_val);
        free(literal);
        return;
    }
    if (strcmp(key, "center") == 0) {
        parse_bool_text(text, &r->actions.center_set, &r->actions.center_val);
        free(literal);
        return;
    }
    if (strcmp(key, "fullscreen") == 0) {
        int num = 0;
        if (parse_bool_text(text, &r->actions.fullscreen_set, &r->actions.fullscreen_val) == 0) {
            free(literal);
            return;
        }
        if (parse_int_text(text, &num) == 0) {
            r->actions.fullscreen_set = 1;
            r->actions.fullscreen_val = num;
            free(literal);
            return;
        }
    }
    add_extra(r, key, literal);
}

static int rule_has_match(const struct rule *r) {
    if (!r) return 0;
    if ((r->match.class_re && r->match.class_re[0]) ||
        (r->match.title_re && r->match.title_re[0]) ||
        (r->match.initial_class_re && r->match.initial_class_re[0]) ||
        (r->match.initial_title_re && r->match.initial_title_re[0]) ||
        (r->match.tag_re && r->match.tag_re[0])) {
        return 1;
    }
    for (size_t i = 0; i < r->extras_count; i++) {
        if (r->extras[i].key && strncmp(r->extras[i].key, "match.", 6) == 0) return 1;
    }
    return 0;
}

static int next_is_keyed(const char *buf, size_t len, size_t pos) {
    lua_skip(buf, len, &pos);
    if (pos >= len) return 0;
    if (buf[pos] == '[') {
        if (pos + 1 < len && (buf[pos + 1] == '[' || buf[pos + 1] == '=')) return 0;
        return 1;
    }
    if (!is_ident((unsigned char)buf[pos]) || isdigit((unsigned char)buf[pos])) return 0;
    while (pos < len && is_ident((unsigned char)buf[pos])) pos++;
    lua_skip(buf, len, &pos);
    return pos < len && buf[pos] == '=';
}

static int skip_scalar(const char *buf, size_t len, size_t *pos) {
    lua_skip(buf, len, pos);
    if (*pos >= len) return -1;
    if (buf[*pos] == '"' || buf[*pos] == '\'' || buf[*pos] == '[') {
        char *s = read_any_string(buf, len, pos);
        if (!s) return -1;
        free(s);
        return 0;
    }
    if (buf[*pos] == '-' || isdigit((unsigned char)buf[*pos])) {
        if (buf[*pos] == '-') (*pos)++;
        while (*pos < len && (isdigit((unsigned char)buf[*pos]) || buf[*pos] == '.')) (*pos)++;
        return 0;
    }
    char *id = read_ident(buf, len, pos);
    if (!id) return -1;
    free(id);
    return 0;
}

static int append_parsed_rule(struct ruleset *out, struct rule *r);
static int parse_table(const char *buf, size_t len, size_t *pos, struct rule *r, int in_match,
                       struct ruleset *harvest);

static int parse_field_value(const char *buf, size_t len, size_t *pos, struct rule *r,
                             const char *key, int in_match) {
    lua_skip(buf, len, pos);
    if (*pos >= len) return -1;

    if (!in_match && (strcmp(key, "match") == 0) && buf[*pos] == '{') {
        return parse_table(buf, len, pos, r, 1, NULL);
    }

    if (buf[*pos] == '{') {
        char *raw = capture_span(buf, len, pos, '{', '}');
        if (!raw) return -1;
        if (!in_match && (strcmp(key, "size") == 0 || strcmp(key, "move") == 0)) {
            char *flat = flatten_pair(raw);
            if (flat) {
                apply_field(r, key, flat, NULL, 0);
                free(flat);
                free(raw);
                return 0;
            }
        }
        apply_field(r, key, raw, raw, in_match);
        return 0;
    }

    if (buf[*pos] == '"' || buf[*pos] == '\'' || buf[*pos] == '[') {
        char *text = read_any_string(buf, len, pos);
        if (!text) return -1;
        char *literal = lua_quote_dup(text);
        apply_field(r, key, text, literal, in_match);
        free(text);
        return 0;
    }

    if (buf[*pos] == '-' || isdigit((unsigned char)buf[*pos])) {
        size_t start = *pos;
        if (buf[*pos] == '-') (*pos)++;
        while (*pos < len && (isdigit((unsigned char)buf[*pos]) || buf[*pos] == '.')) (*pos)++;
        size_t n = *pos - start;
        char *text = malloc(n + 1);
        if (!text) return -1;
        memcpy(text, buf + start, n);
        text[n] = '\0';
        char *literal = strdup(text);
        apply_field(r, key, text, literal, in_match);
        free(text);
        return 0;
    }

    char *ident = read_ident(buf, len, pos);
    if (!ident) return -1;
    char *literal = strdup(ident);
    apply_field(r, key, ident, literal, in_match);
    free(ident);
    return 0;
}

static int parse_table(const char *buf, size_t len, size_t *pos, struct rule *r, int in_match,
                       struct ruleset *harvest) {
    lua_skip(buf, len, pos);
    if (*pos >= len || buf[*pos] != '{') return -1;
    (*pos)++;

    while (*pos < len) {
        lua_skip(buf, len, pos);
        if (*pos >= len) return -1;
        if (buf[*pos] == '}') {
            (*pos)++;
            return 0;
        }

        /* array element: { { name = ..., match = ... }, ... } */
        if (!next_is_keyed(buf, len, *pos)) {
            if (buf[*pos] == '{') {
                struct rule child = {0};
                if (parse_table(buf, len, pos, &child, 0, harvest) != 0) {
                    rule_free(&child);
                    return -1;
                }
                if (harvest && rule_has_match(&child)) {
                    if (append_parsed_rule(harvest, &child) != 0) rule_free(&child);
                } else {
                    rule_free(&child);
                }
            } else if (skip_scalar(buf, len, pos) != 0) {
                return -1;
            }
            lua_skip(buf, len, pos);
            if (*pos < len && buf[*pos] == ',') (*pos)++;
            continue;
        }

        char *key = NULL;
        if (buf[*pos] == '[') {
            (*pos)++;
            lua_skip(buf, len, pos);
            key = read_lua_string(buf, len, pos);
            lua_skip(buf, len, pos);
            if (*pos < len && buf[*pos] == ']') (*pos)++;
        } else {
            key = read_ident(buf, len, pos);
        }
        if (!key) return -1;

        lua_skip(buf, len, pos);
        if (*pos >= len || buf[*pos] != '=') {
            free(key);
            return -1;
        }
        (*pos)++;
        if (parse_field_value(buf, len, pos, r, key, in_match) != 0) {
            free(key);
            return -1;
        }
        free(key);

        lua_skip(buf, len, pos);
        if (*pos < len && buf[*pos] == ',') (*pos)++;
    }
    return -1;
}

static int append_parsed_rule(struct ruleset *out, struct rule *r) {
    size_t n = out->count + 1;
    struct rule *next = realloc(out->rules, n * sizeof(struct rule));
    if (!next) return -1;
    out->rules = next;
    out->rules[out->count++] = *r;
    memset(r, 0, sizeof(*r));
    return 0;
}

int hyprconf_parse_file(const char *path, struct ruleset *out) {
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) return -1;

    size_t pos = 0;
    int in_str = 0;
    char q = 0;
    int in_line = 0;
    int in_block = 0;

    while (pos < len) {
        char c = buf[pos];
        if (in_block) {
            if (c == ']' && pos + 1 < len && buf[pos + 1] == ']') {
                in_block = 0;
                pos += 2;
            } else {
                pos++;
            }
            continue;
        }
        if (in_line) {
            if (c == '\n') in_line = 0;
            pos++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && pos + 1 < len) { pos += 2; continue; }
            if (c == q) in_str = 0;
            pos++;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_str = 1;
            q = c;
            pos++;
            continue;
        }
        if (long_string_level(buf, len, pos) >= 0) {
            char *s = read_long_string(buf, len, &pos);
            if (!s) break;
            free(s);
            continue;
        }
        if (c == '-' && pos + 1 < len && buf[pos + 1] == '-') {
            pos += 2;
            if (pos + 1 < len && buf[pos] == '[' && buf[pos + 1] == '[') {
                in_block = 1;
                pos += 2;
            } else {
                in_line = 1;
            }
            continue;
        }
        if (c == '{') {
            size_t start = pos;
            struct rule self = {0};
            size_t table_end = pos;
            if (parse_table(buf, len, &table_end, &self, 0, out) == 0) {
                if (rule_has_match(&self)) {
                    if (append_parsed_rule(out, &self) != 0) rule_free(&self);
                } else {
                    rule_free(&self);
                }
                pos = table_end;
                continue;
            }
            rule_free(&self);
            pos = start;
            char *span = capture_span(buf, len, &pos, '{', '}');
            if (span) {
                free(span);
                continue;
            }
        }
        if (at_window_rule(buf, len, pos)) {
            size_t call = pos + 11;
            lua_skip(buf, len, &call);
            if (call < len && buf[call] == '(') {
                call++;
                lua_skip(buf, len, &call);
                if (call < len && buf[call] == '{') {
                    struct rule rule = {0};
                    if (parse_table(buf, len, &call, &rule, 0, NULL) == 0) {
                        if (append_parsed_rule(out, &rule) != 0) rule_free(&rule);
                    } else {
                        rule_free(&rule);
                    }
                    pos = call;
                    continue;
                }
            }
        }
        pos++;
    }

    free(buf);
    return 0;
}

static int skip_balanced(const char *buf, size_t len, size_t *pos, char open, char close) {
    if (*pos >= len || buf[*pos] != open) return -1;
    char *span = capture_span(buf, len, pos, open, close);
    int ok = span != NULL;
    free(span);
    return ok ? 0 : -1;
}

static int write_rule_element(FILE *out, const struct rule *r) {
    FILE *mem = tmpfile();
    if (!mem) return -1;
    rule_write(mem, r);
    if (fflush(mem) != 0) {
        fclose(mem);
        return -1;
    }
    long sz = ftell(mem);
    if (sz < 0) {
        fclose(mem);
        return -1;
    }
    rewind(mem);
    char *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, mem) != (size_t)sz) {
        free(buf);
        fclose(mem);
        return -1;
    }
    buf[sz] = '\0';
    fclose(mem);

    char *start = strchr(buf, '{');
    char *end = strrchr(buf, '}');
    int rc = -1;
    if (start && end && end >= start) {
        if (fwrite(start, 1, (size_t)(end - start + 1), out) == (size_t)(end - start + 1) &&
            fputs(",\n", out) >= 0) {
            rc = 0;
        }
    }
    free(buf);
    return rc;
}

static int find_rules_table(const char *buf, size_t len, size_t *open, size_t *after) {
    size_t pos = 0;
    int in_str = 0;
    char q = 0;
    int in_line = 0;
    int in_block = 0;

    while (pos < len) {
        char c = buf[pos];
        if (in_block) {
            if (c == ']' && pos + 1 < len && buf[pos + 1] == ']') {
                in_block = 0;
                pos += 2;
            } else {
                pos++;
            }
            continue;
        }
        if (in_line) {
            if (c == '\n') in_line = 0;
            pos++;
            continue;
        }
        if (in_str) {
            if (c == '\\' && pos + 1 < len) { pos += 2; continue; }
            if (c == q) in_str = 0;
            pos++;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_str = 1;
            q = c;
            pos++;
            continue;
        }
        if (long_string_level(buf, len, pos) >= 0) {
            char *s = read_long_string(buf, len, &pos);
            if (!s) return 0;
            free(s);
            continue;
        }
        if (c == '-' && pos + 1 < len && buf[pos + 1] == '-') {
            pos += 2;
            if (pos + 1 < len && buf[pos] == '[' && buf[pos + 1] == '[') {
                in_block = 1;
                pos += 2;
            } else {
                in_line = 1;
            }
            continue;
        }

        if (pos + 5 <= len && memcmp(buf + pos, "rules", 5) == 0 &&
            (pos == 0 || !is_ident((unsigned char)buf[pos - 1])) &&
            (pos + 5 >= len || !is_ident((unsigned char)buf[pos + 5]))) {
            size_t p = pos + 5;
            lua_skip(buf, len, &p);
            if (p < len && buf[p] == '=') {
                p++;
                lua_skip(buf, len, &p);
                if (p < len && buf[p] == '{') {
                    size_t end = p;
                    char *span = capture_span(buf, len, &end, '{', '}');
                    if (span) {
                        free(span);
                        *open = p;
                        *after = end;
                        return 1;
                    }
                }
            }
        }
        pos++;
    }
    return 0;
}

int hyprconf_replace_rules_table(const char *path, const struct ruleset *set, const int *order) {
    if (!path || !set) return -1;
    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) return -1;

    size_t open = 0, after = 0;
    if (!find_rules_table(buf, len, &open, &after)) {
        free(buf);
        return -1;
    }

    char tmp[1024];
    size_t plen = strlen(path);
    if (plen + 5 >= sizeof(tmp)) {
        free(buf);
        return -1;
    }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        free(buf);
        return -1;
    }

    int rc = -1;
    if (fwrite(buf, 1, open, out) == open && fputs("{\n", out) >= 0) {
        rc = 0;
        for (size_t i = 0; i < set->count; i++) {
            int idx = order ? order[i] : (int)i;
            if (idx < 0 || idx >= (int)set->count) {
                rc = -1;
                break;
            }
            if (write_rule_element(out, &set->rules[idx]) != 0) {
                rc = -1;
                break;
            }
        }
    }
    if (rc == 0) {
        if (fputs("}", out) < 0 || fwrite(buf + after, 1, len - after, out) != len - after) {
            rc = -1;
        }
    }
    free(buf);
    if (rc == 0 && (fflush(out) != 0 || fsync(fileno(out)) != 0)) rc = -1;
    fclose(out);
    if (rc == 0 && rename(tmp, path) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    return rc;
}

int hyprconf_file_is_rules_only(const char *path) {
    if (!path) return 0;
    if (access(path, F_OK) != 0) return 1;

    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) return 0;

    size_t pos = 0;
    int ok = 1;
    while (pos < len) {
        lua_skip(buf, len, &pos);
        if (pos >= len) break;

        if (pos + 5 <= len && memcmp(buf + pos, "local", 5) == 0 &&
            (pos + 5 >= len || !is_ident((unsigned char)buf[pos + 5]))) {
            pos += 5;
            lua_skip(buf, len, &pos);
            char *name = read_ident(buf, len, &pos);
            free(name);
            if (!name) { ok = 0; break; }
            lua_skip(buf, len, &pos);
            if (pos >= len || buf[pos] != '=') { ok = 0; break; }
            pos++;
            lua_skip(buf, len, &pos);
        }

        if (pos + 3 <= len && memcmp(buf + pos, "hl", 2) == 0 && !is_ident((unsigned char)buf[pos + 2])) {
            pos += 2;
            lua_skip(buf, len, &pos);
            if (pos >= len || buf[pos] != '.') { ok = 0; break; }
            pos++;
            lua_skip(buf, len, &pos);
        }

        if (!at_window_rule(buf, len, pos)) { ok = 0; break; }
        pos += 11;
        lua_skip(buf, len, &pos);
        if (skip_balanced(buf, len, &pos, '(', ')') != 0) { ok = 0; break; }

        lua_skip(buf, len, &pos);
        while (pos < len && buf[pos] == ':') {
            pos++;
            lua_skip(buf, len, &pos);
            char *method = read_ident(buf, len, &pos);
            free(method);
            if (!method) { ok = 0; break; }
            lua_skip(buf, len, &pos);
            if (pos < len && buf[pos] == '(') {
                if (skip_balanced(buf, len, &pos, '(', ')') != 0) { ok = 0; break; }
            }
            lua_skip(buf, len, &pos);
        }
        if (!ok) break;
        if (pos < len && buf[pos] == ';') pos++;
    }

    free(buf);
    return ok;
}
