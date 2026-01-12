#ifndef HYPRWINDOWS_APPMAP_H
#define HYPRWINDOWS_APPMAP_H

#include <stddef.h>

struct appmap_entry {
    char *dotfile;      /* config dir name (e.g., "firefox") */
    char *package;      /* package name if different (e.g., "firefox-developer-edition") */
    char **classes;     /* window class names */
    size_t class_count;
    char *group;        /* category (e.g., "browser") */
};

struct appmap {
    struct appmap_entry *entries;
    size_t count;
};

int appmap_load(const char *path, struct appmap *out);
void appmap_free(struct appmap *map);
const struct appmap_entry *appmap_find_by_class(const struct appmap *map, const char *class_name);

#endif
