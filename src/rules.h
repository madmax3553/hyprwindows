#ifndef HYPRWINDOWS_RULES_H
#define HYPRWINDOWS_RULES_H

#include <stddef.h>

struct rule_match {
    char *class_re;
    char *title_re;
    char *initial_class_re;
    char *initial_title_re;
    char *tag_re;
};

struct rule_actions {
    char *tag;
    char *workspace;
    char *opacity;
    char *size;
    char *move;
    int float_set;
    int float_val;
    int center_set;
    int center_val;
};

/* key-value pair for unknown/extra fields */
struct rule_extra {
    char *key;
    char *value;
};

struct rule {
    char *name;
    char *display_name;  /* derived human-readable name */
    struct rule_match match;
    struct rule_actions actions;
    struct rule_extra *extras;
    size_t extras_count;
};

struct ruleset {
    struct rule *rules;
    size_t count;
};

int ruleset_load(const char *path, struct ruleset *out);
void ruleset_free(struct ruleset *set);

char *hypr_find_rules_config(void);

#endif
