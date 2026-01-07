#include "hyprctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

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

    struct json_doc doc;
    if (json_parse_buf(buf, len, &doc) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    if (!json_is_array(&doc, 0)) {
        json_free(&doc);
        return -1;
    }

    int count = json_arr_len(&doc, 0);
    struct client *items = (struct client *)calloc((size_t)count, sizeof(struct client));
    if (!items) {
        json_free(&doc);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        int tok = json_arr_at(&doc, 0, i);
        if (!json_is_object(&doc, tok)) {
            continue;
        }
        struct client *c = &items[i];
        int class_tok = json_obj_get(&doc, tok, "class");
        int title_tok = json_obj_get(&doc, tok, "title");
        int ic_tok = json_obj_get(&doc, tok, "initialClass");
        int it_tok = json_obj_get(&doc, tok, "initialTitle");
        int ws_tok = json_obj_get(&doc, tok, "workspace");

        if (class_tok >= 0) {
            c->class_name = json_tok_strdup(&doc, class_tok);
        }
        if (title_tok >= 0) {
            c->title = json_tok_strdup(&doc, title_tok);
        }
        if (ic_tok >= 0) {
            c->initial_class = json_tok_strdup(&doc, ic_tok);
        }
        if (it_tok >= 0) {
            c->initial_title = json_tok_strdup(&doc, it_tok);
        }
        c->workspace_id = -1;
        if (ws_tok >= 0) {
            if (json_is_object(&doc, ws_tok)) {
                int id_tok = json_obj_get(&doc, ws_tok, "id");
                int name_tok = json_obj_get(&doc, ws_tok, "name");
                if (id_tok >= 0) {
                    c->workspace_id = atoi(doc.buf + doc.toks[id_tok].start);
                }
                if (name_tok >= 0) {
                    c->workspace_name = json_tok_strdup(&doc, name_tok);
                }
            }
        }
    }

    json_free(&doc);
    out->items = items;
    out->count = (size_t)count;
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
