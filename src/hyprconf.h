#ifndef HYPRWINDOWS_HYPRCONF_H
#define HYPRWINDOWS_HYPRCONF_H

#include "rules.h"

int hyprconf_parse_file(const char *path, struct ruleset *out);

/* Replace a `rules = { ... }` table in place, leaving the rest of the file alone.
   order[i] is the ruleset index to write at position i, or NULL for natural order.
   Returns 0 if a table was replaced, -1 if this file has no such table. */
int hyprconf_replace_rules_table(const char *path, const struct ruleset *set, const int *order);

/* 1 if the file is missing, or contains only blanks, comments, and hl.window_rule() calls.
   0 if it has any other Lua statement — that file must not be rewritten. */
int hyprconf_file_is_rules_only(const char *path);

#endif
