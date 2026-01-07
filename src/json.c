#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static char *strip_jsonc(const char *src, size_t len, size_t *out_len) {
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }

    size_t i = 0;
    size_t o = 0;
    int in_string = 0;
    int in_line_comment = 0;
    int in_block_comment = 0;
    int escaped = 0;

    while (i < len) {
        char c = src[i];
        char next = (i + 1 < len) ? src[i + 1] : '\0';

        if (in_line_comment) {
            if (c == '\n') {
                in_line_comment = 0;
                out[o++] = c;
            }
            i++;
            continue;
        }
        if (in_block_comment) {
            if (c == '*' && next == '/') {
                in_block_comment = 0;
                i += 2;
                continue;
            }
            i++;
            continue;
        }

        if (in_string) {
            out[o++] = c;
            if (escaped) {
                escaped = 0;
            } else if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                in_string = 0;
            }
            i++;
            continue;
        }

        if (c == '"') {
            in_string = 1;
            out[o++] = c;
            i++;
            continue;
        }

        if (c == '/' && next == '/') {
            in_line_comment = 1;
            i += 2;
            continue;
        }
        if (c == '/' && next == '*') {
            in_block_comment = 1;
            i += 2;
            continue;
        }

        out[o++] = c;
        i++;
    }

    out[o] = '\0';
    if (out_len) {
        *out_len = o;
    }
    return out;
}

static int json_parse_inner(char *clean, size_t clean_len, struct json_doc *out_doc) {
    jsmn_parser parser;
    jsmn_init(&parser);
    int tokcount = jsmn_parse(&parser, clean, clean_len, NULL, 0);
    if (tokcount < 0) {
        free(clean);
        return -1;
    }

    jsmntok_t *toks = (jsmntok_t *)calloc((size_t)tokcount, sizeof(jsmntok_t));
    if (!toks) {
        free(clean);
        return -1;
    }
    jsmn_init(&parser);
    int ret = jsmn_parse(&parser, clean, clean_len, toks, (unsigned int)tokcount);
    if (ret < 0) {
        free(clean);
        free(toks);
        return -1;
    }

    out_doc->buf = clean;
    out_doc->toks = toks;
    out_doc->tokcount = tokcount;
    return 0;
}

int json_parse_file(const char *path, struct json_doc *out_doc) {
    memset(out_doc, 0, sizeof(*out_doc));

    size_t raw_len = 0;
    char *raw = read_file(path, &raw_len);
    if (!raw) {
        return -1;
    }

    size_t clean_len = 0;
    char *clean = strip_jsonc(raw, raw_len, &clean_len);
    free(raw);
    if (!clean) {
        return -1;
    }

    return json_parse_inner(clean, clean_len, out_doc);
}

int json_parse_buf(const char *buf, size_t len, struct json_doc *out_doc) {
    memset(out_doc, 0, sizeof(*out_doc));
    if (!buf) {
        return -1;
    }
    size_t clean_len = 0;
    char *clean = strip_jsonc(buf, len, &clean_len);
    if (!clean) {
        return -1;
    }
    return json_parse_inner(clean, clean_len, out_doc);
}

void json_free(struct json_doc *doc) {
    if (!doc) {
        return;
    }
    free(doc->buf);
    free(doc->toks);
    doc->buf = NULL;
    doc->toks = NULL;
    doc->tokcount = 0;
}

int json_is_object(const struct json_doc *doc, int tok) {
    return doc && tok >= 0 && tok < doc->tokcount && doc->toks[tok].type == JSMN_OBJECT;
}

int json_is_array(const struct json_doc *doc, int tok) {
    return doc && tok >= 0 && tok < doc->tokcount && doc->toks[tok].type == JSMN_ARRAY;
}

int json_is_string(const struct json_doc *doc, int tok) {
    return doc && tok >= 0 && tok < doc->tokcount && doc->toks[tok].type == JSMN_STRING;
}

int json_is_primitive(const struct json_doc *doc, int tok) {
    return doc && tok >= 0 && tok < doc->tokcount && doc->toks[tok].type == JSMN_PRIMITIVE;
}

int json_skip(const struct json_doc *doc, int tok) {
    if (!doc || tok < 0 || tok >= doc->tokcount) {
        return tok + 1;
    }
    int end = tok + 1;
    if (doc->toks[tok].type == JSMN_OBJECT) {
        int pairs = doc->toks[tok].size;
        for (int i = 0; i < pairs; i++) {
            end = json_skip(doc, end); /* key */
            end = json_skip(doc, end); /* value */
        }
    } else if (doc->toks[tok].type == JSMN_ARRAY) {
        int count = doc->toks[tok].size;
        for (int i = 0; i < count; i++) {
            end = json_skip(doc, end);
        }
    }
    return end;
}

int json_obj_get(const struct json_doc *doc, int obj_tok, const char *key) {
    if (!json_is_object(doc, obj_tok)) {
        return -1;
    }
    int cur = obj_tok + 1;
    int pairs = doc->toks[obj_tok].size;
    size_t key_len = strlen(key);
    for (int i = 0; i < pairs; i++) {
        int key_tok = cur;
        int val_tok = cur + 1;
        if (json_is_string(doc, key_tok)) {
            const char *k = NULL;
            size_t klen = 0;
            json_tok_str(doc, key_tok, &k, &klen);
            if (klen == key_len && strncmp(k, key, klen) == 0) {
                return val_tok;
            }
        }
        cur = json_skip(doc, val_tok);
    }
    return -1;
}

int json_arr_len(const struct json_doc *doc, int arr_tok) {
    if (!json_is_array(doc, arr_tok)) {
        return 0;
    }
    return doc->toks[arr_tok].size;
}

int json_arr_at(const struct json_doc *doc, int arr_tok, int index) {
    if (!json_is_array(doc, arr_tok)) {
        return -1;
    }
    int count = doc->toks[arr_tok].size;
    if (index < 0 || index >= count) {
        return -1;
    }
    int cur = arr_tok + 1;
    for (int i = 0; i < index; i++) {
        cur = json_skip(doc, cur);
    }
    return cur;
}

int json_tok_str(const struct json_doc *doc, int tok, const char **out, size_t *out_len) {
    if (!doc || tok < 0 || tok >= doc->tokcount) {
        return -1;
    }
    if (doc->toks[tok].type != JSMN_STRING && doc->toks[tok].type != JSMN_PRIMITIVE) {
        return -1;
    }
    if (out) {
        *out = doc->buf + doc->toks[tok].start;
    }
    if (out_len) {
        *out_len = (size_t)(doc->toks[tok].end - doc->toks[tok].start);
    }
    return 0;
}

char *json_tok_strdup(const struct json_doc *doc, int tok) {
    const char *s = NULL;
    size_t len = 0;
    if (json_tok_str(doc, tok, &s, &len) != 0) {
        return NULL;
    }
    char *dup = (char *)malloc(len + 1);
    if (!dup) {
        return NULL;
    }
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}
