#include "appmap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/*
 * Simple parser for appmap.json.
 * Format is a flat array of objects with known keys:
 *   { "dotfile": "...", "package": "...", "classes": ["...", ...], "group": "..." }
 *
 * We don't need a full JSON parser for this predictable structure.
 */

/* extract a quoted string value after current position, return strdup'd */
static char *extract_string(const char *p, const char *end, const char **out_pos) {
    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '"') return NULL;
    p++; /* skip opening quote */

    const char *start = p;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) p++;
        p++;
    }
    if (p >= end) return NULL;

    size_t len = (size_t)(p - start);
    char *s = malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, start, len);
    s[len] = '\0';

    if (out_pos) *out_pos = p + 1; /* past closing quote */
    return s;
}

/* find "key": in buf, return pointer after the colon, or NULL */
static const char *find_key(const char *buf, const char *end, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t patlen = strlen(pat);

    const char *p = buf;
    while (p + patlen < end) {
        p = strstr(p, pat);
        if (!p || p >= end) return NULL;
        p += patlen;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p < end && *p == ':') return p + 1;
    }
    return NULL;
}

/* parse "classes": ["a", "b", ...] */
static int parse_classes(const char *buf, const char *end, struct appmap_entry *e) {
    const char *p = find_key(buf, end, "classes");
    if (!p) return 0;

    while (p < end && isspace((unsigned char)*p)) p++;
    if (p >= end || *p != '[') return 0;
    p++; /* skip '[' */

    size_t cap = 4;
    e->classes = calloc(cap, sizeof(char *));
    if (!e->classes) return -1;
    e->class_count = 0;

    while (p < end && *p != ']') {
        while (p < end && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= end || *p == ']') break;

        const char *next = NULL;
        char *cls = extract_string(p, end, &next);
        if (!cls) break;

        if (e->class_count >= cap) {
            cap *= 2;
            char **tmp = realloc(e->classes, cap * sizeof(char *));
            if (!tmp) { free(cls); break; }
            e->classes = tmp;
        }
        e->classes[e->class_count++] = cls;
        p = next;
    }
    return 0;
}

/* find the end of a JSON object starting at '{' */
static const char *skip_object(const char *p, const char *end) {
    if (p >= end || *p != '{') return p;
    int depth = 1;
    p++;
    int in_str = 0;
    while (p < end && depth > 0) {
        if (in_str) {
            if (*p == '\\') p++;
            else if (*p == '"') in_str = 0;
        } else {
            if (*p == '"') in_str = 1;
            else if (*p == '{') depth++;
            else if (*p == '}') depth--;
        }
        p++;
    }
    return p;
}

static void free_entry(struct appmap_entry *e) {
    if (!e) return;
    free(e->dotfile);
    free(e->package);
    free(e->group);
    for (size_t i = 0; i < e->class_count; i++) {
        free(e->classes[i]);
    }
    free(e->classes);
}

int appmap_load(const char *path, struct appmap *out) {
    memset(out, 0, sizeof(*out));

    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) return -1;

    const char *end = buf + len;

    /* count top-level objects */
    size_t count = 0;
    int depth = 0;
    for (const char *p = buf; p < end; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        else if (*p == '{' && depth == 1) count++;
    }

    if (count == 0) { free(buf); return 0; }

    struct appmap_entry *entries = calloc(count, sizeof(struct appmap_entry));
    if (!entries) { free(buf); return -1; }

    const char *p = buf;
    size_t idx = 0;
    while (p < end && idx < count) {
        while (p < end && *p != '{') p++;
        if (p >= end) break;

        const char *obj_start = p;
        const char *obj_end = skip_object(p, end);

        struct appmap_entry *e = &entries[idx];

        const char *val;
        val = find_key(obj_start, obj_end, "dotfile");
        if (val) e->dotfile = extract_string(val, obj_end, NULL);

        val = find_key(obj_start, obj_end, "package");
        if (val) e->package = extract_string(val, obj_end, NULL);

        val = find_key(obj_start, obj_end, "group");
        if (val) e->group = extract_string(val, obj_end, NULL);

        parse_classes(obj_start, obj_end, e);

        idx++;
        p = obj_end;
    }

    free(buf);
    out->entries = entries;
    out->count = count;
    return 0;
}

void appmap_free(struct appmap *map) {
    if (!map || !map->entries) return;
    for (size_t i = 0; i < map->count; i++) {
        free_entry(&map->entries[i]);
    }
    free(map->entries);
    map->entries = NULL;
    map->count = 0;
}
