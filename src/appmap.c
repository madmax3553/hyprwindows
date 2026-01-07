#include "appmap.h"

#include <stdlib.h>
#include <string.h>

static void free_entry(struct appmap_entry *e) {
    if (!e) {
        return;
    }
    free(e->dotfile);
    free(e->group);
    for (size_t i = 0; i < e->class_count; i++) {
        free(e->classes[i]);
    }
    free(e->classes);
    e->classes = NULL;
    e->class_count = 0;
}

static int parse_classes(const struct json_doc *doc, int arr_tok, struct appmap_entry *e) {
    if (!json_is_array(doc, arr_tok)) {
        return -1;
    }
    int count = json_arr_len(doc, arr_tok);
    if (count <= 0) {
        return 0;
    }
    e->classes = (char **)calloc((size_t)count, sizeof(char *));
    if (!e->classes) {
        return -1;
    }
    e->class_count = (size_t)count;
    for (int i = 0; i < count; i++) {
        int tok = json_arr_at(doc, arr_tok, i);
        e->classes[i] = json_tok_strdup(doc, tok);
    }
    return 0;
}

int appmap_load(const char *path, struct appmap *out) {
    memset(out, 0, sizeof(*out));

    struct json_doc doc;
    if (json_parse_file(path, &doc) != 0) {
        return -1;
    }
    if (!json_is_array(&doc, 0)) {
        json_free(&doc);
        return -1;
    }

    int count = json_arr_len(&doc, 0);
    if (count <= 0) {
        json_free(&doc);
        return 0;
    }

    struct appmap_entry *entries = (struct appmap_entry *)calloc((size_t)count, sizeof(struct appmap_entry));
    if (!entries) {
        json_free(&doc);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        int tok = json_arr_at(&doc, 0, i);
        if (!json_is_object(&doc, tok)) {
            continue;
        }
        struct appmap_entry *e = &entries[i];
        int dot_tok = json_obj_get(&doc, tok, "dotfile");
        if (dot_tok >= 0) {
            e->dotfile = json_tok_strdup(&doc, dot_tok);
        }
        int group_tok = json_obj_get(&doc, tok, "group");
        if (group_tok >= 0) {
            e->group = json_tok_strdup(&doc, group_tok);
        }
        int classes_tok = json_obj_get(&doc, tok, "classes");
        if (classes_tok >= 0) {
            parse_classes(&doc, classes_tok, e);
        }
    }

    json_free(&doc);
    out->entries = entries;
    out->count = (size_t)count;
    return 0;
}

void appmap_free(struct appmap *map) {
    if (!map || !map->entries) {
        return;
    }
    for (size_t i = 0; i < map->count; i++) {
        free_entry(&map->entries[i]);
    }
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
}
