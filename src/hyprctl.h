#ifndef HYPRWINDOWS_HYPRCTL_H
#define HYPRWINDOWS_HYPRCTL_H

#include <stddef.h>

struct client {
    char *class_name;
    char *title;
    char *initial_class;
    char *initial_title;
    char *workspace_name;
    int workspace_id;
};

struct clients {
    struct client *items;
    size_t count;
};

int hyprctl_clients(struct clients *out);
void clients_free(struct clients *list);

#endif
