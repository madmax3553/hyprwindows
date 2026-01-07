#include "actions.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "appmap.h"
#include "hyprctl.h"
#include "rules.h"
#include "util.h"

struct str_list {
    char **items;
    size_t count;
    size_t cap;
};

static void list_add(struct str_list *list, const char *s) {
    if (!s) {
        return;
    }
    if (list->count + 1 > list->cap) {
        size_t next = list->cap == 0 ? 8 : list->cap * 2;
        char **items = (char **)realloc(list->items, next * sizeof(char *));
        if (!items) {
            return;
        }
        list->items = items;
        list->cap = next;
    }
    list->items[list->count++] = strdup(s);
}

static int list_contains(const struct str_list *list, const char *s) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], s) == 0) {
            return 1;
        }
    }
    return 0;
}

static void list_free(struct str_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

struct group_entry {
    char *group;
    struct str_list apps;
};

struct group_list {
    struct group_entry *items;
    size_t count;
    size_t cap;
};

static struct group_entry *group_get(struct group_list *groups, const char *group) {
    for (size_t i = 0; i < groups->count; i++) {
        if (strcmp(groups->items[i].group, group) == 0) {
            return &groups->items[i];
        }
    }
    if (groups->count + 1 > groups->cap) {
        size_t next = groups->cap == 0 ? 8 : groups->cap * 2;
        struct group_entry *items = (struct group_entry *)realloc(groups->items, next * sizeof(*items));
        if (!items) {
            return NULL;
        }
        groups->items = items;
        groups->cap = next;
    }
    struct group_entry *entry = &groups->items[groups->count++];
    entry->group = strdup(group);
    entry->apps.items = NULL;
    entry->apps.count = 0;
    entry->apps.cap = 0;
    return entry;
}

static void group_list_free(struct group_list *groups) {
    for (size_t i = 0; i < groups->count; i++) {
        free(groups->items[i].group);
        list_free(&groups->items[i].apps);
    }
    free(groups->items);
    groups->items = NULL;
    groups->count = 0;
    groups->cap = 0;
}

static const char *group_from_tag(const char *tag) {
    if (!tag || tag[0] == '\0') {
        return "ungrouped";
    }
    if (tag[0] == '+') {
        return tag + 1;
    }
    return tag;
}

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

int summarize_rules_text(const char *path, struct outbuf *out) {
    struct ruleset rules;
    if (ruleset_load_json(path, &rules) != 0) {
        outbuf_printf(out, "Failed to load rules from %s\n", path);
        return -1;
    }

    struct group_list groups = {0};
    for (size_t i = 0; i < rules.count; i++) {
        struct rule *r = &rules.rules[i];
        const char *group = group_from_tag(r->actions.tag);
        struct group_entry *entry = group_get(&groups, group);
        if (!entry) {
            continue;
        }
        if (r->match.class_re) {
            if (!list_contains(&entry->apps, r->match.class_re)) {
                list_add(&entry->apps, r->match.class_re);
            }
        } else {
            list_add(&entry->apps, "<no class match>");
        }
    }

    for (size_t i = 0; i < groups.count; i++) {
        struct group_entry *g = &groups.items[i];
        outbuf_printf(out, "Group: %s\n", g->group);
        for (size_t j = 0; j < g->apps.count; j++) {
            outbuf_printf(out, "  App: %s\n", g->apps.items[j]);
        }
    }

    group_list_free(&groups);
    ruleset_free(&rules);
    return 0;
}

static int dotfiles_has_entry(const char *dotfiles, const char *entry) {
    DIR *dir = opendir(dotfiles);
    if (!dir) {
        return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, entry) == 0) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

int scan_dotfiles_text(const char *dotfiles, const char *rules_path, const char *appmap_path,
                       const struct action_opts *opts, struct outbuf *out) {
    struct ruleset rules;
    if (ruleset_load_json(rules_path, &rules) != 0) {
        outbuf_printf(out, "Failed to load rules from %s\n", rules_path);
        return -1;
    }

    struct appmap map;
    if (appmap_load(appmap_path, &map) != 0) {
        outbuf_printf(out, "Failed to load appmap from %s\n", appmap_path);
        ruleset_free(&rules);
        return -1;
    }

    struct str_list inferred = {0};
    struct str_list missing = {0};
    struct str_list overlaps = {0};

    for (size_t i = 0; i < map.count; i++) {
        struct appmap_entry *e = &map.entries[i];
        if (!e->dotfile || !dotfiles_has_entry(dotfiles, e->dotfile)) {
            continue;
        }
        list_add(&inferred, e->dotfile);

        for (size_t c = 0; c < e->class_count; c++) {
            const char *cls = e->classes[c];
            int match_count = 0;
            for (size_t r = 0; r < rules.count; r++) {
                const char *pattern = rules.rules[r].match.class_re;
                if (!pattern) {
                    continue;
                }
                if (regex_match(pattern, cls)) {
                    match_count++;
                }
            }
            if (match_count == 0) {
                if (!list_contains(&missing, cls)) {
                    list_add(&missing, cls);
                }
            } else if (match_count > 1) {
                if (!list_contains(&overlaps, cls)) {
                    list_add(&overlaps, cls);
                }
            }
        }
    }

    outbuf_printf(out, "Inferred apps (from dotfiles):\n");
    for (size_t i = 0; i < inferred.count; i++) {
        outbuf_printf(out, "  %s\n", inferred.items[i]);
    }

    outbuf_printf(out, "\nMissing rules for classes:\n");
    if (missing.count == 0) {
        outbuf_printf(out, "  (none)\n");
    }
    for (size_t i = 0; i < missing.count; i++) {
        outbuf_printf(out, "  %s\n", missing.items[i]);
    }

    if (!opts || opts->show_overlaps) {
        outbuf_printf(out, "\nOverlapping rules for classes:\n");
        if (overlaps.count == 0) {
            outbuf_printf(out, "  (none)\n");
        }
        for (size_t i = 0; i < overlaps.count; i++) {
            outbuf_printf(out, "  %s\n", overlaps.items[i]);
        }
    }

    list_free(&inferred);
    list_free(&missing);
    list_free(&overlaps);
    appmap_free(&map);
    ruleset_free(&rules);
    return 0;
}

static char *escape_regex(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    size_t cap = len * 2 + 3;
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    out[o++] = '^';
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (strchr(".[](){}*+?|^$\\", c)) {
            out[o++] = '\\';
        }
        out[o++] = c;
    }
    out[o++] = '$';
    out[o] = '\0';
    return out;
}

static char *slugify(const char *s) {
    if (!s) {
        return strdup("window");
    }
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[i] = (char)tolower((unsigned char)c);
        } else {
            out[i] = '-';
        }
    }
    out[len] = '\0';
    return out;
}

static int rule_matches_client(const struct rule *r, const struct client *c) {
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
        return 0;
    }
    return 1;
}

int active_windows_text(const char *rules_path, const struct action_opts *opts, struct outbuf *out) {
    struct ruleset rules;
    if (ruleset_load_json(rules_path, &rules) != 0) {
        outbuf_printf(out, "Failed to load rules from %s\n", rules_path);
        return -1;
    }
    struct clients clients;
    if (hyprctl_clients(&clients) != 0) {
        outbuf_printf(out, "Failed to read hyprctl clients\n");
        ruleset_free(&rules);
        return -1;
    }

    int suggest = opts ? opts->suggest_rules : 1;
    for (size_t i = 0; i < clients.count; i++) {
        struct client *c = &clients.items[i];
        outbuf_printf(out, "Window: %s\n", c->class_name ? c->class_name : "<unknown>");
        if (c->title) {
            outbuf_printf(out, "  Title: %s\n", c->title);
        }
        if (c->workspace_id >= 0) {
            outbuf_printf(out, "  Workspace: %d\n", c->workspace_id);
        } else if (c->workspace_name) {
            outbuf_printf(out, "  Workspace: %s\n", c->workspace_name);
        }

        struct str_list matched = {0};
        for (size_t r = 0; r < rules.count; r++) {
            if (rule_matches_client(&rules.rules[r], c)) {
                if (rules.rules[r].name) {
                    list_add(&matched, rules.rules[r].name);
                } else {
                    list_add(&matched, "<unnamed>");
                }
            }
        }

        if (matched.count == 0) {
            outbuf_printf(out, "  Matches: (none)\n");
            if (suggest && c->class_name) {
                char *regex = escape_regex(c->class_name);
                char *slug = slugify(c->class_name);
                outbuf_printf(out, "  Suggestion:\n");
                outbuf_printf(out, "    {\n");
                outbuf_printf(out, "      \"name\": \"rule-auto-%s\",\n", slug ? slug : "window");
                outbuf_printf(out, "      \"match:class\": \"%s\"", regex ? regex : "^<class>$");
                if (c->workspace_id >= 0) {
                    outbuf_printf(out, ",\n      \"workspace\": \"%d\"\n", c->workspace_id);
                } else {
                    outbuf_printf(out, "\n");
                }
                outbuf_printf(out, "    }\n");
                free(regex);
                free(slug);
            }
        } else {
            outbuf_printf(out, "  Matches:");
            for (size_t m = 0; m < matched.count; m++) {
                outbuf_printf(out, " %s", matched.items[m]);
                if (m + 1 < matched.count) {
                    outbuf_printf(out, ",");
                }
            }
            outbuf_printf(out, "\n");
        }

        list_free(&matched);
        outbuf_printf(out, "\n");
    }

    clients_free(&clients);
    ruleset_free(&rules);
    return 0;
}
