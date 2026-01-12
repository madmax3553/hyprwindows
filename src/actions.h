#ifndef HYPRWINDOWS_ACTIONS_H
#define HYPRWINDOWS_ACTIONS_H

#include <stddef.h>

#include "hyprctl.h"
#include "rules.h"

struct outbuf {
    char *data;
    size_t len;
    size_t cap;
};

struct action_opts {
    int suggest_rules;
    int show_overlaps;
};

/* missing rule suggestion */
struct missing_rule {
    char *app_name;      /* from appmap dotfile/package */
    char *class_pattern; /* suggested class regex */
    char *group;         /* category */
    char *source;        /* "package" or "dotfile" */
};

struct missing_rules {
    struct missing_rule *items;
    size_t count;
};

void outbuf_init(struct outbuf *out);
void outbuf_free(struct outbuf *out);
int outbuf_printf(struct outbuf *out, const char *fmt, ...);

int summarize_rules_text(const char *path, struct outbuf *out);
int scan_dotfiles_text(const char *dotfiles, const char *rules_path, const char *appmap_path,
                       const struct action_opts *opts, struct outbuf *out);
int active_windows_text(const char *rules_path, const struct action_opts *opts, struct outbuf *out);
int review_rules_text(const char *rules_path, struct outbuf *out);
int rule_matches_client(const struct rule *r, const struct client *c);

int find_missing_rules(const char *rules_path, const char *appmap_path, 
                       const char *dotfiles_path, struct missing_rules *out);
void missing_rules_free(struct missing_rules *mr);

#endif
