#include "hyprctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simplejson.h"

static char *read_pipe(const char *cmd, size_t *out_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return NULL;
    }
    size_t cap = 4096;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        pclose(fp);
        return NULL;
    }

    size_t nread = 0;
    while (!feof(fp)) {
        if (len + 1024 > cap) {
            cap *= 2;
            char *next = (char *)realloc(buf, cap);
            if (!next) {
                free(buf);
                pclose(fp);
                return NULL;
            }
            buf = next;
        }
        nread = fread(buf + len, 1, 1024, fp);
        len += nread;
    }

    int rc = pclose(fp);
    if (rc != 0) {
        free(buf);
        return NULL;
    }

    buf = (char *)realloc(buf, len + 1);
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

static void free_client(struct client *c) {
    if (!c) {
        return;
    }
    free(c->class_name);
    free(c->title);
    free(c->initial_class);
    free(c->initial_title);
    free(c->workspace_name);
}

int hyprctl_clients(struct clients *out) {
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    char *buf = read_pipe("hyprctl -j clients", &len);
    if (!buf) {
        return -1;
    }

    struct sjson_value root;
    if (sjson_parse(buf, len, &root) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    if (root.type != SJSON_ARRAY) {
        sjson_free(&root);
        return -1;
    }

    size_t count = sjson_array_len(&root);
    struct client *items = (struct client *)calloc(count, sizeof(struct client));
    if (!items) {
        sjson_free(&root);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        const struct sjson_value *item = sjson_array_at(&root, i);
        if (!item || item->type != SJSON_OBJECT) {
            continue;
        }
        struct client *c = &items[i];

        const char *cls = sjson_get_string(item, "class");
        if (cls) {
            c->class_name = strdup(cls);
        }
        const char *title = sjson_get_string(item, "title");
        if (title) {
            c->title = strdup(title);
        }
        const char *ic = sjson_get_string(item, "initialClass");
        if (ic) {
            c->initial_class = strdup(ic);
        }
        const char *it = sjson_get_string(item, "initialTitle");
        if (it) {
            c->initial_title = strdup(it);
        }

        c->workspace_id = -1;
        const struct sjson_value *ws = sjson_get(item, "workspace");
        if (ws && ws->type == SJSON_OBJECT) {
            c->workspace_id = sjson_get_int(ws, "id", -1);
            const char *wsname = sjson_get_string(ws, "name");
            if (wsname) {
                c->workspace_name = strdup(wsname);
            }
        }
    }

    sjson_free(&root);
    out->items = items;
    out->count = count;
    return 0;
}

void clients_free(struct clients *list) {
    if (!list || !list->items) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free_client(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}
