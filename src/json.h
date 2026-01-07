#ifndef HYPRWINDOWS_JSON_H
#define HYPRWINDOWS_JSON_H

#include <stddef.h>
#include "../third_party/jsmn.h"

struct json_doc {
    char *buf;
    jsmntok_t *toks;
    int tokcount;
};

int json_parse_file(const char *path, struct json_doc *out_doc);
int json_parse_buf(const char *buf, size_t len, struct json_doc *out_doc);
void json_free(struct json_doc *doc);

int json_is_object(const struct json_doc *doc, int tok);
int json_is_array(const struct json_doc *doc, int tok);
int json_is_string(const struct json_doc *doc, int tok);
int json_is_primitive(const struct json_doc *doc, int tok);

int json_obj_get(const struct json_doc *doc, int obj_tok, const char *key);
int json_arr_len(const struct json_doc *doc, int arr_tok);
int json_arr_at(const struct json_doc *doc, int arr_tok, int index);
int json_skip(const struct json_doc *doc, int tok);

int json_tok_str(const struct json_doc *doc, int tok, const char **out, size_t *out_len);
char *json_tok_strdup(const struct json_doc *doc, int tok);

#endif
