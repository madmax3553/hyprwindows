#ifndef HYPRWINDOWS_SIMPLEJSON_H
#define HYPRWINDOWS_SIMPLEJSON_H

#include <stddef.h>

/*
 * Minimal JSON parser for hyprctl output and appmap.json
 * Uses a simple recursive descent approach
 */

struct sjson_value;

struct sjson_object {
    char **keys;
    struct sjson_value *values;
    size_t count;
};

struct sjson_array {
    struct sjson_value *items;
    size_t count;
};

enum sjson_type {
    SJSON_NULL,
    SJSON_BOOL,
    SJSON_NUMBER,
    SJSON_STRING,
    SJSON_ARRAY,
    SJSON_OBJECT
};

struct sjson_value {
    enum sjson_type type;
    union {
        int bool_val;
        double num_val;
        char *str_val;
        struct sjson_array arr_val;
        struct sjson_object obj_val;
    } u;
};

int sjson_parse(const char *buf, size_t len, struct sjson_value *out);
void sjson_free(struct sjson_value *val);

/* accessors */
const char *sjson_get_string(const struct sjson_value *obj, const char *key);
int sjson_get_int(const struct sjson_value *obj, const char *key, int def);
const struct sjson_value *sjson_get(const struct sjson_value *obj, const char *key);
size_t sjson_array_len(const struct sjson_value *arr);
const struct sjson_value *sjson_array_at(const struct sjson_value *arr, size_t idx);

#endif
