#include "actions.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#include "appmap.h"
#include "hyprctl.h"
#include "rules.h"
#include "util.h"

static int match_field_set(const char *re) {
    return re && re[0];
}

int rule_matches_client(const struct rule *r, const struct client *c) {
    if (!r || !c) return 0;

    int verifiable = 0;

    if (match_field_set(r->match.class_re)) {
        verifiable = 1;
        if (!c->class_name || !regex_match(r->match.class_re, c->class_name)) {
            return 0;
        }
    }
    if (match_field_set(r->match.title_re)) {
        verifiable = 1;
        if (!c->title || !regex_match(r->match.title_re, c->title)) {
            return 0;
        }
    }
    if (match_field_set(r->match.initial_class_re)) {
        verifiable = 1;
        if (!c->initial_class || !regex_match(r->match.initial_class_re, c->initial_class)) {
            return 0;
        }
    }
    if (match_field_set(r->match.initial_title_re)) {
        verifiable = 1;
        if (!c->initial_title || !regex_match(r->match.initial_title_re, c->initial_title)) {
            return 0;
        }
    }

    /* hyprctl clients do not expose tags. A tag-only rule must not match
       every window — that made pipeline tag rules look like overlaps. */
    if (!verifiable && match_field_set(r->match.tag_re)) {
        return 0;
    }
    return 1;
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

/* check if package is installed via pacman, without a shell */
#define PKG_CACHE_MAX 256

static struct {
    char *name;
    int installed;
} pkg_cache[PKG_CACHE_MAX];
static size_t pkg_cache_len;

static int pkg_name_ok(const char *pkg) {
    if (!pkg || !pkg[0] || strlen(pkg) > 128) return 0;
    for (const char *p = pkg; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '+' || c == '.' || c == '@') {
            continue;
        }
        return 0;
    }
    return 1;
}

static int package_installed_uncached(const char *pkg) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execlp("pacman", "pacman", "-Qi", pkg, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int package_installed(const char *pkg) {
    if (!pkg_name_ok(pkg)) return 0;
    for (size_t i = 0; i < pkg_cache_len; i++) {
        if (strcmp(pkg_cache[i].name, pkg) == 0) return pkg_cache[i].installed;
    }
    int installed = package_installed_uncached(pkg);
    if (pkg_cache_len < PKG_CACHE_MAX) {
        char *name = strdup(pkg);
        if (name) {
            pkg_cache[pkg_cache_len].name = name;
            pkg_cache[pkg_cache_len].installed = installed;
            pkg_cache_len++;
        }
    }
    return installed;
}

void package_cache_clear(void) {
    for (size_t i = 0; i < pkg_cache_len; i++) {
        free(pkg_cache[i].name);
        pkg_cache[i].name = NULL;
    }
    pkg_cache_len = 0;
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

int find_missing_rules(const struct ruleset *rules, const char *appmap_path,
                       const char *dotfiles_path, struct missing_rules *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!rules) return -1;

    struct appmap appmap;
    if (appmap_load(appmap_path, &appmap) != 0) {
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
            if (rules_cover_class(rules, e->classes[j])) {
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
    return 0;
}
