#include "simplejson.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void skip_ws(const char *buf, size_t len, size_t *pos) {
    while (*pos < len && isspace((unsigned char)buf[*pos])) {
        (*pos)++;
    }
}

static int parse_value(const char *buf, size_t len, size_t *pos, struct sjson_value *out);

static char *parse_string(const char *buf, size_t len, size_t *pos) {
    if (*pos >= len || buf[*pos] != '"') {
        return NULL;
    }
    (*pos)++;
    size_t cap = 64;
    size_t slen = 0;
    char *str = (char *)malloc(cap);
    if (!str) {
        return NULL;
    }

    while (*pos < len && buf[*pos] != '"') {
        char c = buf[*pos];
        if (c == '\\' && *pos + 1 < len) {
            (*pos)++;
            c = buf[*pos];
            switch (c) {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case '\\': c = '\\'; break;
                case '"': c = '"'; break;
                case '/': c = '/'; break;
                default: break;
            }
        }
        if (slen + 1 >= cap) {
            cap *= 2;
            char *tmp = (char *)realloc(str, cap);
            if (!tmp) {
                free(str);
                return NULL;
            }
            str = tmp;
        }
        str[slen++] = c;
        (*pos)++;
    }
    if (*pos < len && buf[*pos] == '"') {
        (*pos)++;
    }
    str[slen] = '\0';
    return str;
}

static int parse_array(const char *buf, size_t len, size_t *pos, struct sjson_array *out) {
    out->items = NULL;
    out->count = 0;

    if (*pos >= len || buf[*pos] != '[') {
        return -1;
    }
    (*pos)++;
    skip_ws(buf, len, pos);

    size_t cap = 8;
    struct sjson_value *items = (struct sjson_value *)calloc(cap, sizeof(struct sjson_value));
    if (!items) {
        return -1;
    }

    while (*pos < len && buf[*pos] != ']') {
        if (out->count >= cap) {
            cap *= 2;
            struct sjson_value *tmp = (struct sjson_value *)realloc(items, cap * sizeof(struct sjson_value));
            if (!tmp) {
                free(items);
                return -1;
            }
            items = tmp;
        }
        if (parse_value(buf, len, pos, &items[out->count]) != 0) {
            for (size_t i = 0; i < out->count; i++) {
                sjson_free(&items[i]);
            }
            free(items);
            return -1;
        }
        out->count++;
        skip_ws(buf, len, pos);
        if (*pos < len && buf[*pos] == ',') {
            (*pos)++;
            skip_ws(buf, len, pos);
        }
    }
    if (*pos < len && buf[*pos] == ']') {
        (*pos)++;
    }
    out->items = items;
    return 0;
}

static int parse_object(const char *buf, size_t len, size_t *pos, struct sjson_object *out) {
    out->keys = NULL;
    out->values = NULL;
    out->count = 0;

    if (*pos >= len || buf[*pos] != '{') {
        return -1;
    }
    (*pos)++;
    skip_ws(buf, len, pos);

    size_t cap = 8;
    char **keys = (char **)calloc(cap, sizeof(char *));
    struct sjson_value *values = (struct sjson_value *)calloc(cap, sizeof(struct sjson_value));
    if (!keys || !values) {
        free(keys);
        free(values);
        return -1;
    }

    while (*pos < len && buf[*pos] != '}') {
        skip_ws(buf, len, pos);
        char *key = parse_string(buf, len, pos);
        if (!key) {
            break;
        }
        skip_ws(buf, len, pos);
        if (*pos < len && buf[*pos] == ':') {
            (*pos)++;
        }
        skip_ws(buf, len, pos);

        if (out->count >= cap) {
            cap *= 2;
            char **ktmp = (char **)realloc(keys, cap * sizeof(char *));
            struct sjson_value *vtmp = (struct sjson_value *)realloc(values, cap * sizeof(struct sjson_value));
            if (!ktmp || !vtmp) {
                free(key);
                break;
            }
            keys = ktmp;
            values = vtmp;
        }

        keys[out->count] = key;
        memset(&values[out->count], 0, sizeof(struct sjson_value));
        if (parse_value(buf, len, pos, &values[out->count]) != 0) {
            free(key);
            break;
        }
        out->count++;

        skip_ws(buf, len, pos);
        if (*pos < len && buf[*pos] == ',') {
            (*pos)++;
            skip_ws(buf, len, pos);
        }
    }
    if (*pos < len && buf[*pos] == '}') {
        (*pos)++;
    }
    out->keys = keys;
    out->values = values;
    return 0;
}

static int parse_value(const char *buf, size_t len, size_t *pos, struct sjson_value *out) {
    memset(out, 0, sizeof(*out));
    skip_ws(buf, len, pos);

    if (*pos >= len) {
        out->type = SJSON_NULL;
        return 0;
    }

    char c = buf[*pos];

    if (c == '"') {
        out->type = SJSON_STRING;
        out->u.str_val = parse_string(buf, len, pos);
        return out->u.str_val ? 0 : -1;
    }

    if (c == '[') {
        out->type = SJSON_ARRAY;
        return parse_array(buf, len, pos, &out->u.arr_val);
    }

    if (c == '{') {
        out->type = SJSON_OBJECT;
        return parse_object(buf, len, pos, &out->u.obj_val);
    }

    if (c == 't' && *pos + 4 <= len && strncmp(buf + *pos, "true", 4) == 0) {
        out->type = SJSON_BOOL;
        out->u.bool_val = 1;
        *pos += 4;
        return 0;
    }

    if (c == 'f' && *pos + 5 <= len && strncmp(buf + *pos, "false", 5) == 0) {
        out->type = SJSON_BOOL;
        out->u.bool_val = 0;
        *pos += 5;
        return 0;
    }

    if (c == 'n' && *pos + 4 <= len && strncmp(buf + *pos, "null", 4) == 0) {
        out->type = SJSON_NULL;
        *pos += 4;
        return 0;
    }

    if (c == '-' || isdigit((unsigned char)c)) {
        out->type = SJSON_NUMBER;
        char *end = NULL;
        out->u.num_val = strtod(buf + *pos, &end);
        if (end) {
            *pos = (size_t)(end - buf);
        }
        return 0;
    }

    return -1;
}

int sjson_parse(const char *buf, size_t len, struct sjson_value *out) {
    size_t pos = 0;
    return parse_value(buf, len, &pos, out);
}

void sjson_free(struct sjson_value *val) {
    if (!val) {
        return;
    }
    switch (val->type) {
        case SJSON_STRING:
            free(val->u.str_val);
            break;
        case SJSON_ARRAY:
            for (size_t i = 0; i < val->u.arr_val.count; i++) {
                sjson_free(&val->u.arr_val.items[i]);
            }
            free(val->u.arr_val.items);
            break;
        case SJSON_OBJECT:
            for (size_t i = 0; i < val->u.obj_val.count; i++) {
                free(val->u.obj_val.keys[i]);
                sjson_free(&val->u.obj_val.values[i]);
            }
            free(val->u.obj_val.keys);
            free(val->u.obj_val.values);
            break;
        default:
            break;
    }
    memset(val, 0, sizeof(*val));
}

const char *sjson_get_string(const struct sjson_value *obj, const char *key) {
    if (!obj || obj->type != SJSON_OBJECT) {
        return NULL;
    }
    for (size_t i = 0; i < obj->u.obj_val.count; i++) {
        if (strcmp(obj->u.obj_val.keys[i], key) == 0) {
            struct sjson_value *v = &obj->u.obj_val.values[i];
            if (v->type == SJSON_STRING) {
                return v->u.str_val;
            }
            return NULL;
        }
    }
    return NULL;
}

int sjson_get_int(const struct sjson_value *obj, const char *key, int def) {
    if (!obj || obj->type != SJSON_OBJECT) {
        return def;
    }
    for (size_t i = 0; i < obj->u.obj_val.count; i++) {
        if (strcmp(obj->u.obj_val.keys[i], key) == 0) {
            struct sjson_value *v = &obj->u.obj_val.values[i];
            if (v->type == SJSON_NUMBER) {
                return (int)v->u.num_val;
            }
            return def;
        }
    }
    return def;
}

const struct sjson_value *sjson_get(const struct sjson_value *obj, const char *key) {
    if (!obj || obj->type != SJSON_OBJECT) {
        return NULL;
    }
    for (size_t i = 0; i < obj->u.obj_val.count; i++) {
        if (strcmp(obj->u.obj_val.keys[i], key) == 0) {
            return &obj->u.obj_val.values[i];
        }
    }
    return NULL;
}

size_t sjson_array_len(const struct sjson_value *arr) {
    if (!arr || arr->type != SJSON_ARRAY) {
        return 0;
    }
    return arr->u.arr_val.count;
}

const struct sjson_value *sjson_array_at(const struct sjson_value *arr, size_t idx) {
    if (!arr || arr->type != SJSON_ARRAY || idx >= arr->u.arr_val.count) {
        return NULL;
    }
    return &arr->u.arr_val.items[idx];
}
