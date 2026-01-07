#ifndef HYPRWINDOWS_ACTIONS_H
#define HYPRWINDOWS_ACTIONS_H

#include <stddef.h>

struct outbuf {
    char *data;
    size_t len;
    size_t cap;
};

struct action_opts {
    int suggest_rules;
    int show_overlaps;
};

void outbuf_init(struct outbuf *out);
void outbuf_free(struct outbuf *out);
int outbuf_printf(struct outbuf *out, const char *fmt, ...);

int summarize_rules_text(const char *path, struct outbuf *out);
int scan_dotfiles_text(const char *dotfiles, const char *rules_path, const char *appmap_path,
                       const struct action_opts *opts, struct outbuf *out);
int active_windows_text(const char *rules_path, const struct action_opts *opts, struct outbuf *out);

#endif
