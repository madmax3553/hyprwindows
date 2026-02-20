#include "hyprctl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Simple JSON string extraction for hyprctl -j clients output.
 * We know the exact structure: an array of objects with known keys.
 * No need for a full parser.
 */

static char *read_pipe(const char *cmd, size_t *out_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    size_t nread;
    while ((nread = fread(buf + len, 1, 1024, fp)) > 0) {
        len += nread;
        if (len + 1024 > cap) {
            cap *= 2;
            char *next = realloc(buf, cap);
            if (!next) { free(buf); pclose(fp); return NULL; }
            buf = next;
        }
    }

    if (pclose(fp) != 0) { free(buf); return NULL; }
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* find "key": "value" in buf starting at pos, return strdup'd value */
static char *json_get_str(const char *buf, size_t len, size_t start, const char *key) {
    /* build search pattern: "key": " */
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    size_t patlen = strlen(pat);

    const char *p = buf + start;
    const char *end = buf + len;

    while (p < end) {
        p = strstr(p, pat);
        if (!p || p >= end) return NULL;
        p += patlen;

        /* skip whitespace */
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p != '"') return NULL;
        p++; /* skip opening quote */

        /* find closing quote, handling escapes */
        const char *val_start = p;
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) p++;
            p++;
        }
        if (p >= end) return NULL;

        size_t vlen = (size_t)(p - val_start);
        char *result = malloc(vlen + 1);
        if (!result) return NULL;
        memcpy(result, val_start, vlen);
        result[vlen] = '\0';
        return result;
    }
    return NULL;
}

/* find "key": <number> in buf starting at pos */
static int json_get_int(const char *buf, size_t len, size_t start, const char *key, int def) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);

    const char *p = strstr(buf + start, pat);
    if (!p || p >= buf + len) return def;
    p += strlen(pat);

    while (p < buf + len && isspace((unsigned char)*p)) p++;
    if (p >= buf + len) return def;

    char *end_ptr = NULL;
    long val = strtol(p, &end_ptr, 10);
    if (end_ptr == p) return def;
    return (int)val;
}

/* find the end of a JSON object starting at '{' */
static size_t json_skip_object(const char *buf, size_t len, size_t pos) {
    if (pos >= len || buf[pos] != '{') return pos;
    int depth = 1;
    pos++;
    int in_string = 0;
    while (pos < len && depth > 0) {
        char c = buf[pos];
        if (in_string) {
            if (c == '\\') { pos++; }
            else if (c == '"') { in_string = 0; }
        } else {
            if (c == '"') in_string = 1;
            else if (c == '{') depth++;
            else if (c == '}') depth--;
        }
        pos++;
    }
    return pos;
}

static void free_client(struct client *c) {
    if (!c) return;
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
    if (!buf) return -1;

    /* count objects by counting top-level '{' */
    size_t count = 0;
    int depth = 0;
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '[') depth++;
        else if (buf[i] == ']') depth--;
        else if (buf[i] == '{' && depth == 1) count++;
    }

    if (count == 0) { free(buf); return 0; }

    struct client *items = calloc(count, sizeof(struct client));
    if (!items) { free(buf); return -1; }

    /* parse each top-level object */
    size_t idx = 0;
    size_t pos = 0;
    while (pos < len && idx < count) {
        /* find next '{' at depth 1 */
        while (pos < len && buf[pos] != '{') pos++;
        if (pos >= len) break;

        size_t obj_start = pos;
        size_t obj_end = json_skip_object(buf, len, pos);

        struct client *c = &items[idx];
        c->class_name = json_get_str(buf, obj_end, obj_start, "class");
        c->title = json_get_str(buf, obj_end, obj_start, "title");
        c->initial_class = json_get_str(buf, obj_end, obj_start, "initialClass");
        c->initial_title = json_get_str(buf, obj_end, obj_start, "initialTitle");

        /* workspace is a nested object: "workspace": { "id": N, "name": "..." } */
        c->workspace_id = -1;
        char pat[] = "\"workspace\":";
        const char *ws = strstr(buf + obj_start, pat);
        if (ws && ws < buf + obj_end) {
            const char *brace = strchr(ws + sizeof(pat) - 1, '{');
            if (brace && brace < buf + obj_end) {
                size_t ws_start = (size_t)(brace - buf);
                size_t ws_end = json_skip_object(buf, obj_end, ws_start);
                c->workspace_id = json_get_int(buf, ws_end, ws_start, "id", -1);
                c->workspace_name = json_get_str(buf, ws_end, ws_start, "name");
            }
        }

        idx++;
        pos = obj_end;
    }

    free(buf);
    out->items = items;
    out->count = count;
    return 0;
}

void clients_free(struct clients *list) {
    if (!list || !list->items) return;
    for (size_t i = 0; i < list->count; i++) {
        free_client(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
}
