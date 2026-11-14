#include "rules.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hyprconf.h"
#include "util.h"

/* --- single rule lifecycle (public) --- */

void rule_free(struct rule *r) {
    if (!r) {
        return;
    }
    free(r->name);
    free(r->display_name);
    free(r->match.class_re);
    free(r->match.title_re);
    free(r->match.initial_class_re);
    free(r->match.initial_title_re);
    free(r->match.tag_re);

    free(r->actions.tag);
    free(r->actions.workspace);
    free(r->actions.opacity);
    free(r->actions.size);
    free(r->actions.move);

    for (size_t i = 0; i < r->extras_count; i++) {
        free(r->extras[i].key);
        free(r->extras[i].value);
    }
    free(r->extras);
}

struct rule rule_copy(const struct rule *src) {
    struct rule dst = {0};
    if (!src) return dst;

    dst.name = src->name ? strdup(src->name) : NULL;
    dst.display_name = src->display_name ? strdup(src->display_name) : NULL;

    dst.match.class_re = src->match.class_re ? strdup(src->match.class_re) : NULL;
    dst.match.title_re = src->match.title_re ? strdup(src->match.title_re) : NULL;
    dst.match.initial_class_re = src->match.initial_class_re ? strdup(src->match.initial_class_re) : NULL;
    dst.match.initial_title_re = src->match.initial_title_re ? strdup(src->match.initial_title_re) : NULL;
    dst.match.tag_re = src->match.tag_re ? strdup(src->match.tag_re) : NULL;

    dst.actions.tag = src->actions.tag ? strdup(src->actions.tag) : NULL;
    dst.actions.workspace = src->actions.workspace ? strdup(src->actions.workspace) : NULL;
    dst.actions.float_set = src->actions.float_set;
    dst.actions.float_val = src->actions.float_val;
    dst.actions.center_set = src->actions.center_set;
    dst.actions.center_val = src->actions.center_val;
    dst.actions.fullscreen_set = src->actions.fullscreen_set;
    dst.actions.fullscreen_val = src->actions.fullscreen_val;
    dst.actions.size = src->actions.size ? strdup(src->actions.size) : NULL;
    dst.actions.move = src->actions.move ? strdup(src->actions.move) : NULL;
    dst.actions.opacity = src->actions.opacity ? strdup(src->actions.opacity) : NULL;

    if (src->extras_count > 0 && src->extras) {
        dst.extras = calloc(src->extras_count, sizeof(struct rule_extra));
        if (dst.extras) {
            size_t n = 0;
            for (size_t i = 0; i < src->extras_count; i++) {
                if (!src->extras[i].key) continue;
                char *k = strdup(src->extras[i].key);
                char *v = src->extras[i].value ? strdup(src->extras[i].value) : strdup("");
                if (!k || !v) {
                    free(k);
                    free(v);
                    continue;
                }
                dst.extras[n].key = k;
                dst.extras[n].value = v;
                n++;
            }
            dst.extras_count = n;
        }
    }

    return dst;
}

static int field_eq(const char *a, const char *b) {
    if (!a || !a[0]) return !b || !b[0];
    if (!b || !b[0]) return 0;
    return strcmp(a, b) == 0;
}

int rule_match_equal(const struct rule *a, const struct rule *b) {
    if (!a || !b) return 0;
    return field_eq(a->match.class_re, b->match.class_re)
        && field_eq(a->match.title_re, b->match.title_re)
        && field_eq(a->match.initial_class_re, b->match.initial_class_re)
        && field_eq(a->match.initial_title_re, b->match.initial_title_re)
        && field_eq(a->match.tag_re, b->match.tag_re);
}

static void fputs_lua_string(FILE *f, const char *s) {
    fputc('"', f);
    if (!s) {
        fputc('"', f);
        return;
    }
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"') {
            fputc('\\', f);
            fputc(*p, f);
        } else if (*p == '\n') {
            fputs("\\n", f);
        } else if (*p == '\t') {
            fputs("\\t", f);
        } else {
            fputc(*p, f);
        }
    }
    fputc('"', f);
}

static int is_match_extra(const char *key) {
    return key && strncmp(key, "match.", 6) == 0 && key[6];
}

void rule_write(FILE *f, const struct rule *r) {
    if (!f || !r) return;

    int has_match = (r->match.class_re && r->match.class_re[0]) ||
                    (r->match.title_re && r->match.title_re[0]) ||
                    (r->match.initial_class_re && r->match.initial_class_re[0]) ||
                    (r->match.initial_title_re && r->match.initial_title_re[0]) ||
                    (r->match.tag_re && r->match.tag_re[0]);
    if (!has_match) {
        for (size_t j = 0; j < r->extras_count; j++) {
            if (is_match_extra(r->extras[j].key)) has_match = 1;
        }
    }

    fprintf(f, "hl.window_rule({\n");
    if (r->name) {
        fprintf(f, "    name = ");
        fputs_lua_string(f, r->name);
        fprintf(f, ",\n");
    }
    if (has_match) {
        fprintf(f, "    match = {\n");
        if (r->match.class_re) {
            fprintf(f, "        class = ");
            fputs_lua_string(f, r->match.class_re);
            fprintf(f, ",\n");
        }
        if (r->match.title_re) {
            fprintf(f, "        title = ");
            fputs_lua_string(f, r->match.title_re);
            fprintf(f, ",\n");
        }
        if (r->match.initial_class_re) {
            fprintf(f, "        initial_class = ");
            fputs_lua_string(f, r->match.initial_class_re);
            fprintf(f, ",\n");
        }
        if (r->match.initial_title_re) {
            fprintf(f, "        initial_title = ");
            fputs_lua_string(f, r->match.initial_title_re);
            fprintf(f, ",\n");
        }
        if (r->match.tag_re) {
            fprintf(f, "        tag = ");
            fputs_lua_string(f, r->match.tag_re);
            fprintf(f, ",\n");
        }
        for (size_t j = 0; j < r->extras_count; j++) {
            if (!is_match_extra(r->extras[j].key)) continue;
            fprintf(f, "        %s = %s,\n", r->extras[j].key + 6, r->extras[j].value);
        }
        fprintf(f, "    },\n");
    }
    if (r->actions.tag) {
        fprintf(f, "    tag = ");
        fputs_lua_string(f, r->actions.tag);
        fprintf(f, ",\n");
    }
    if (r->actions.workspace) {
        fprintf(f, "    workspace = ");
        fputs_lua_string(f, r->actions.workspace);
        fprintf(f, ",\n");
    }
    if (r->actions.float_set) fprintf(f, "    float = %s,\n", r->actions.float_val ? "true" : "false");
    if (r->actions.center_set) fprintf(f, "    center = %s,\n", r->actions.center_val ? "true" : "false");
    if (r->actions.fullscreen_set) {
        if (r->actions.fullscreen_val == 0) fprintf(f, "    fullscreen = false,\n");
        else if (r->actions.fullscreen_val == 1) fprintf(f, "    fullscreen = true,\n");
        else fprintf(f, "    fullscreen = %d,\n", r->actions.fullscreen_val);
    }
    if (r->actions.size) {
        fprintf(f, "    size = ");
        fputs_lua_string(f, r->actions.size);
        fprintf(f, ",\n");
    }
    if (r->actions.move) {
        fprintf(f, "    move = ");
        fputs_lua_string(f, r->actions.move);
        fprintf(f, ",\n");
    }
    if (r->actions.opacity) {
        fprintf(f, "    opacity = ");
        fputs_lua_string(f, r->actions.opacity);
        fprintf(f, ",\n");
    }
    for (size_t j = 0; j < r->extras_count; j++) {
        if (is_match_extra(r->extras[j].key)) continue;
        fprintf(f, "    %s = %s,\n", r->extras[j].key, r->extras[j].value);
    }
    fprintf(f, "})\n\n");
}

/* --- ruleset --- */

void ruleset_free(struct ruleset *set) {
    if (!set || !set->rules) {
        return;
    }
    for (size_t i = 0; i < set->count; i++) {
        rule_free(&set->rules[i]);
    }
    free(set->rules);
    set->rules = NULL;
    set->count = 0;
}

int ruleset_load(const char *path, struct ruleset *out) {
    return hyprconf_parse_file(path, out);
}

/* --- auto-detect config --- */

static int ident_boundary(char c) {
    return !isalnum((unsigned char)c) && c != '_';
}

static int file_has_window_rule(const char *path) {
    char *expanded = expand_home(path);
    if (!expanded) return 0;
    size_t len = 0;
    char *buf = read_file(expanded, &len);
    free(expanded);
    if (!buf) return 0;
    int found = strstr(buf, "window_rule") != NULL;
    free(buf);
    return found;
}

static char *read_call_string(const char **pp) {
    const char *p = *pp;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '(') {
        p++;
        while (*p && isspace((unsigned char)*p)) p++;
    }
    if (*p != '"' && *p != '\'') return NULL;
    char q = *p++;
    const char *start = p;
    while (*p && *p != q) {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    if (*p != q) return NULL;
    size_t n = (size_t)(p - start);
    char *s = malloc(n + 1);
    if (!s) return NULL;
    memcpy(s, start, n);
    s[n] = '\0';
    *pp = p + 1;
    return s;
}

/* "config.rules" -> base/config/rules.lua; "rules.lua" or "~/x.lua" used as a path */
static char *resolve_module(const char *base, const char *spec) {
    char *exp = expand_home(spec);
    if (!exp || !exp[0]) {
        free(exp);
        return NULL;
    }

    if (strchr(exp, '/')) {
        if (access(exp, R_OK) == 0) return exp;
        size_t n = strlen(exp);
        char *with = malloc(n + 5);
        if (with) {
            memcpy(with, exp, n);
            memcpy(with + n, ".lua", 5);
            if (access(with, R_OK) == 0) {
                free(exp);
                return with;
            }
            free(with);
        }
        free(exp);
        return NULL;
    }

    char rel[256];
    size_t n = 0;
    for (const char *p = exp; *p && n + 1 < sizeof(rel); p++) {
        rel[n++] = (*p == '.') ? '/' : *p;
    }
    rel[n] = '\0';
    free(exp);

    size_t blen = strlen(base);
    char *path = malloc(blen + 1 + n + 10);
    if (!path) return NULL;
    snprintf(path, blen + 1 + n + 10, "%s/%s.lua", base, rel);
    if (access(path, R_OK) == 0) return path;
    snprintf(path, blen + 1 + n + 10, "%s/%s/init.lua", base, rel);
    if (access(path, R_OK) == 0) return path;
    free(path);
    return NULL;
}

static char *first_required_rules(const char *buf, const char *base) {
    const char *p = buf;
    while (p && *p) {
        if (p[0] == '-' && p[1] == '-') {
            if (p[2] == '[' && p[3] == '[') {
                p = strstr(p + 4, "]]");
                if (!p) return NULL;
                p += 2;
                continue;
            }
            while (*p && *p != '\n') p++;
            continue;
        }
        int at_require = strncmp(p, "require", 7) == 0 && (p == buf || ident_boundary(p[-1])) && ident_boundary(p[7]);
        int at_dofile = strncmp(p, "dofile", 6) == 0 && (p == buf || ident_boundary(p[-1])) && ident_boundary(p[6]);
        if (at_require || at_dofile) {
            p += at_require ? 7 : 6;
            char *spec = read_call_string(&p);
            if (spec) {
                char *path = resolve_module(base, spec);
                free(spec);
                if (path && file_has_window_rule(path)) return path;
                free(path);
            }
            continue;
        }
        p++;
    }
    return NULL;
}

static int path_join(char *out, size_t out_sz, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    if (dlen + 1 + nlen + 1 > out_sz) return -1;
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen + 1);
    return 0;
}

char *hypr_find_rules_config(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;

    char base[512];
    if (path_join(base, sizeof(base), home, ".config/hypr") != 0) return NULL;

    char main_path[512];
    if (path_join(main_path, sizeof(main_path), base, "hyprland.lua") != 0) return NULL;
    if (access(main_path, R_OK) == 0) {
        size_t len = 0;
        char *buf = read_file(main_path, &len);
        if (buf) {
            char *required = first_required_rules(buf, base);
            int main_has = strstr(buf, "window_rule") != NULL;
            free(buf);
            if (required) return required;
            if (main_has) return strdup(main_path);
        }
    }

    const char *names[] = { "rules.lua", "windowrules.lua", "config/rules.lua" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[512];
        if (path_join(path, sizeof(path), base, names[i]) != 0) continue;
        if (file_has_window_rule(path)) return strdup(path);
    }
    return NULL;
}
