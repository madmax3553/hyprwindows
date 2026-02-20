#include "actions.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "appmap.h"
#include "hyprctl.h"
#include "rules.h"
#include "util.h"

void outbuf_init(struct outbuf *out) {
    out->data = NULL;
    out->len = 0;
    out->cap = 0;
}

void outbuf_free(struct outbuf *out) {
    free(out->data);
    out->data = NULL;
    out->len = 0;
    out->cap = 0;
}

int outbuf_printf(struct outbuf *out, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list args2;
    va_copy(args2, args);
    int needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        va_end(args2);
        return -1;
    }

    size_t add = (size_t)needed;
    if (out->len + add + 1 > out->cap) {
        size_t next = out->cap == 0 ? 256 : out->cap;
        while (out->len + add + 1 > next) {
            next *= 2;
        }
        char *buf = (char *)realloc(out->data, next);
        if (!buf) {
            va_end(args2);
            return -1;
        }
        out->data = buf;
        out->cap = next;
    }

    vsnprintf(out->data + out->len, out->cap - out->len, fmt, args2);
    va_end(args2);
    out->len += add;
    return 0;
}

int rule_matches_client(const struct rule *r, const struct client *c) {
    if (r->match.class_re) {
        if (!c->class_name || !regex_match(r->match.class_re, c->class_name)) {
            return 0;
        }
    }
    if (r->match.title_re) {
        if (!c->title || !regex_match(r->match.title_re, c->title)) {
            return 0;
        }
    }
    if (r->match.initial_class_re) {
        if (!c->initial_class || !regex_match(r->match.initial_class_re, c->initial_class)) {
            return 0;
        }
    }
    if (r->match.initial_title_re) {
        if (!c->initial_title || !regex_match(r->match.initial_title_re, c->initial_title)) {
            return 0;
        }
    }
    if (r->match.tag_re) {
        if (!c->workspace_name || !regex_match(r->match.tag_re, c->workspace_name)) {
            return 0;
        }
    }
    return 1;
}

int review_rules_text(const char *rules_path, struct outbuf *out) {
    struct ruleset rules;
    if (ruleset_load(rules_path, &rules) != 0) {
        outbuf_printf(out, "Failed to load rules from %s\n", rules_path);
        return -1;
    }
    struct clients clients;
    if (hyprctl_clients(&clients) != 0) {
        outbuf_printf(out, "Failed to read hyprctl clients\n");
        ruleset_free(&rules);
        return -1;
    }

    int *matched = calloc(rules.count, sizeof(int));
    if (!matched) {
        clients_free(&clients);
        ruleset_free(&rules);
        return -1;
    }

    int windows_without_rules = 0;
    for (size_t i = 0; i < clients.count; i++) {
        struct client *c = &clients.items[i];
        int has_match = 0;
        for (size_t r = 0; r < rules.count; r++) {
            if (rule_matches_client(&rules.rules[r], c)) {
                matched[r] = 1;
                has_match = 1;
            }
        }
        if (!has_match) windows_without_rules++;
    }

    int unused_count = 0;
    outbuf_printf(out, "=== Rules Review ===\n\n");
    outbuf_printf(out, "Potentially unused rules (no matching windows):\n");
    for (size_t r = 0; r < rules.count; r++) {
        if (!matched[r]) {
            struct rule *rule = &rules.rules[r];
            const char *name = rule->name ? rule->name : "(unnamed)";
            const char *class_re = rule->match.class_re ? rule->match.class_re : "-";
            outbuf_printf(out, "  %s: %s\n", name, class_re);
            unused_count++;
        }
    }
    if (unused_count == 0) {
        outbuf_printf(out, "  (none - all rules match at least one window)\n");
    }

    outbuf_printf(out, "\nSummary:\n");
    outbuf_printf(out, "  Total rules: %zu\n", rules.count);
    outbuf_printf(out, "  Active rules: %zu\n", rules.count - unused_count);
    outbuf_printf(out, "  Unused rules: %d\n", unused_count);
    outbuf_printf(out, "  Windows without rules: %d\n", windows_without_rules);

    free(matched);
    clients_free(&clients);
    ruleset_free(&rules);
    return 0;
}

/* check if any rule matches a class pattern (simple substring check) */
static int rules_cover_class(const struct ruleset *rules, const char *class_name) {
    for (size_t i = 0; i < rules->count; i++) {
        const char *re = rules->rules[i].match.class_re;
        if (!re) continue;
        if (strcasestr(re, class_name)) return 1;
    }
    return 0;
}

/* check if package is installed via pacman */
static int package_installed(const char *pkg) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pacman -Qi %s >/dev/null 2>&1", pkg);
    return system(cmd) == 0;
}

/* check if dotfile config exists */
static int dotfile_exists(const char *dotfiles_path, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dotfiles_path, name);
    return access(path, F_OK) == 0;
}

/* build class regex from appmap entry */
static char *build_class_regex(const struct appmap_entry *e) {
    if (!e || e->class_count == 0) return NULL;

    size_t len = 4; /* ^()$ */
    for (size_t i = 0; i < e->class_count; i++) {
        len += strlen(e->classes[i]) + 1;
    }

    char *re = malloc(len + 1);
    if (!re) return NULL;

    strcpy(re, "^(");
    for (size_t i = 0; i < e->class_count; i++) {
        if (i > 0) strcat(re, "|");
        strcat(re, e->classes[i]);
    }
    strcat(re, ")$");
    return re;
}

void missing_rules_free(struct missing_rules *mr) {
    if (!mr) return;
    for (size_t i = 0; i < mr->count; i++) {
        free(mr->items[i].app_name);
        free(mr->items[i].class_pattern);
        free(mr->items[i].group);
        free(mr->items[i].source);
    }
    free(mr->items);
    mr->items = NULL;
    mr->count = 0;
}

int find_missing_rules(const char *rules_path, const char *appmap_path,
                       const char *dotfiles_path, struct missing_rules *out) {
    memset(out, 0, sizeof(*out));

    struct ruleset rules;
    if (ruleset_load(rules_path, &rules) != 0) {
        return -1;
    }

    struct appmap appmap;
    if (appmap_load(appmap_path, &appmap) != 0) {
        ruleset_free(&rules);
        return -1;
    }

    char *dotfiles_exp = dotfiles_path ? expand_home(dotfiles_path) : NULL;
    const char *dotfiles = dotfiles_exp ? dotfiles_exp : dotfiles_path;

    size_t cap = 0;
    for (size_t i = 0; i < appmap.count; i++) {
        struct appmap_entry *e = &appmap.entries[i];
        if (e->class_count == 0) continue;

        int covered = 0;
        for (size_t j = 0; j < e->class_count && !covered; j++) {
            if (rules_cover_class(&rules, e->classes[j])) {
                covered = 1;
            }
        }
        if (covered) continue;

        const char *source = NULL;
        const char *pkg = e->package ? e->package : e->dotfile;

        if (pkg && package_installed(pkg)) {
            source = "package";
        } else if (e->dotfile && dotfiles && dotfile_exists(dotfiles, e->dotfile)) {
            source = "dotfile";
        }

        if (!source) continue;

        if (out->count >= cap) {
            cap = cap ? cap * 2 : 8;
            struct missing_rule *new_items = realloc(out->items, cap * sizeof(struct missing_rule));
            if (!new_items) continue;
            out->items = new_items;
        }

        struct missing_rule *mr = &out->items[out->count++];
        mr->app_name = strdup(e->dotfile ? e->dotfile : pkg);
        mr->class_pattern = build_class_regex(e);
        mr->group = e->group ? strdup(e->group) : NULL;
        mr->source = strdup(source);
    }

    free(dotfiles_exp);
    appmap_free(&appmap);
    ruleset_free(&rules);
    return 0;
}
