#include "appmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "simplejson.h"

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    if (out_len) {
        *out_len = (size_t)size;
    }
    return buf;
}

static void free_entry(struct appmap_entry *e) {
    if (!e) {
        return;
    }
    free(e->dotfile);
    free(e->package);
    free(e->group);
    for (size_t i = 0; i < e->class_count; i++) {
        free(e->classes[i]);
    }
    free(e->classes);
    e->classes = NULL;
    e->class_count = 0;
}

int appmap_load(const char *path, struct appmap *out) {
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    char *buf = read_file(path, &len);
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
    if (count == 0) {
        sjson_free(&root);
        return 0;
    }

    struct appmap_entry *entries = (struct appmap_entry *)calloc(count, sizeof(struct appmap_entry));
    if (!entries) {
        sjson_free(&root);
        return -1;
    }

    for (size_t i = 0; i < count; i++) {
        const struct sjson_value *item = sjson_array_at(&root, i);
        if (!item || item->type != SJSON_OBJECT) {
            continue;
        }
        struct appmap_entry *e = &entries[i];

        const char *dotfile = sjson_get_string(item, "dotfile");
        if (dotfile) {
            e->dotfile = strdup(dotfile);
        }
        const char *package = sjson_get_string(item, "package");
        if (package) {
            e->package = strdup(package);
        }
        const char *group = sjson_get_string(item, "group");
        if (group) {
            e->group = strdup(group);
        }

        const struct sjson_value *classes = sjson_get(item, "classes");
        if (classes && classes->type == SJSON_ARRAY) {
            size_t ccount = sjson_array_len(classes);
            if (ccount > 0) {
                e->classes = (char **)calloc(ccount, sizeof(char *));
                if (e->classes) {
                    e->class_count = ccount;
                    for (size_t j = 0; j < ccount; j++) {
                        const struct sjson_value *cv = sjson_array_at(classes, j);
                        if (cv && cv->type == SJSON_STRING) {
                            e->classes[j] = strdup(cv->u.str_val);
                        }
                    }
                }
            }
        }
    }

    sjson_free(&root);
    out->entries = entries;
    out->count = count;
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

const struct appmap_entry *appmap_find_by_class(const struct appmap *map, const char *class_name) {
    if (!map || !class_name) return NULL;
    for (size_t i = 0; i < map->count; i++) {
        for (size_t j = 0; j < map->entries[i].class_count; j++) {
            if (map->entries[i].classes[j] && 
                strcasecmp(map->entries[i].classes[j], class_name) == 0) {
                return &map->entries[i];
            }
        }
    }
    return NULL;
}
