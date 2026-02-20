#ifndef HYPRWINDOWS_APPMAP_H
#define HYPRWINDOWS_APPMAP_H

#include <stddef.h>

struct appmap_entry {
    char *dotfile;
    char *package;
    char **classes;
    size_t class_count;
    char *group;
};

struct appmap {
    struct appmap_entry *entries;
    size_t count;
};

int appmap_load(const char *path, struct appmap *out);
void appmap_free(struct appmap *map);

#endif
