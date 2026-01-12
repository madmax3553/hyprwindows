#ifndef HYPRWINDOWS_HYPRCONF_H
#define HYPRWINDOWS_HYPRCONF_H

#include "rules.h"

int hyprconf_parse_file(const char *path, struct ruleset *out);

#endif
