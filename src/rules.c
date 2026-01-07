#include "rules.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int str_eq(const char *a, size_t alen, const char *b) {
    size_t blen = strlen(b);
    return alen == blen && strncmp(a, b, alen) == 0;
}

static int starts_with(const char *s, size_t len, const char *prefix) {
    size_t plen = strlen(prefix);
    if (len < plen) {
        return 0;
    }
    return strncmp(s, prefix, plen) == 0;
}

static int parse_bool_tok(const struct json_doc *doc, int tok, int *out_set, int *out_val) {
    const char *s = NULL;
    size_t len = 0;
    if (json_tok_str(doc, tok, &s, &len) != 0) {
        return -1;
    }
    if (len == 0) {
        return -1;
    }

    if (str_eq(s, len, "true") || str_eq(s, len, "yes") || str_eq(s, len, "1")) {
        *out_set = 1;
        *out_val = 1;
        return 0;
    }
    if (str_eq(s, len, "false") || str_eq(s, len, "no") || str_eq(s, len, "0")) {
        *out_set = 1;
        *out_val = 0;
        return 0;
    }
    return -1;
}

static void assign_str(char **dst, const struct json_doc *doc, int tok) {
    if (*dst) {
        return;
    }
    *dst = json_tok_strdup(doc, tok);
}

static void parse_match_kv(struct rule_match *m, const char *key, size_t key_len,
                           const struct json_doc *doc, int val_tok) {
    if (str_eq(key, key_len, "class") || str_eq(key, key_len, "initialClass")) {
        if (str_eq(key, key_len, "class")) {
            assign_str(&m->class_re, doc, val_tok);
        } else {
            assign_str(&m->initial_class_re, doc, val_tok);
        }
        return;
    }
    if (str_eq(key, key_len, "title") || str_eq(key, key_len, "initialTitle")) {
        if (str_eq(key, key_len, "title")) {
            assign_str(&m->title_re, doc, val_tok);
        } else {
            assign_str(&m->initial_title_re, doc, val_tok);
        }
        return;
    }
    if (str_eq(key, key_len, "initial_class")) {
        assign_str(&m->initial_class_re, doc, val_tok);
        return;
    }
    if (str_eq(key, key_len, "initial_title")) {
        assign_str(&m->initial_title_re, doc, val_tok);
        return;
    }
    if (str_eq(key, key_len, "tag")) {
        assign_str(&m->tag_re, doc, val_tok);
        return;
    }
}

static void parse_match_object(struct rule_match *m, const struct json_doc *doc, int obj_tok) {
    if (!json_is_object(doc, obj_tok)) {
        return;
    }
    int cur = obj_tok + 1;
    int pairs = doc->toks[obj_tok].size;
    for (int i = 0; i < pairs; i++) {
        int key_tok = cur;
        int val_tok = cur + 1;
        const char *k = NULL;
        size_t klen = 0;
        if (json_tok_str(doc, key_tok, &k, &klen) == 0) {
            parse_match_kv(m, k, klen, doc, val_tok);
        }
        cur = json_skip(doc, val_tok);
    }
}

static void parse_action_kv(struct rule_actions *a, const char *key, size_t key_len,
                            const struct json_doc *doc, int val_tok) {
    if (str_eq(key, key_len, "tag")) {
        assign_str(&a->tag, doc, val_tok);
        return;
    }
    if (str_eq(key, key_len, "workspace")) {
        assign_str(&a->workspace, doc, val_tok);
        return;
    }
    if (str_eq(key, key_len, "opacity")) {
        assign_str(&a->opacity, doc, val_tok);
        return;
    }
    if (str_eq(key, key_len, "size")) {
        assign_str(&a->size, doc, val_tok);
        return;
    }
    if (str_eq(key, key_len, "move")) {
        assign_str(&a->move, doc, val_tok);
        return;
    }
    if (str_eq(key, key_len, "float")) {
        parse_bool_tok(doc, val_tok, &a->float_set, &a->float_val);
        return;
    }
    if (str_eq(key, key_len, "center")) {
        parse_bool_tok(doc, val_tok, &a->center_set, &a->center_val);
        return;
    }
}

static void parse_rule_object(struct rule *rule, const struct json_doc *doc, int obj_tok) {
    if (!json_is_object(doc, obj_tok)) {
        return;
    }

    int cur = obj_tok + 1;
    int pairs = doc->toks[obj_tok].size;
    for (int i = 0; i < pairs; i++) {
        int key_tok = cur;
        int val_tok = cur + 1;
        const char *k = NULL;
        size_t klen = 0;
        if (json_tok_str(doc, key_tok, &k, &klen) == 0) {
            if (str_eq(k, klen, "name")) {
                rule->name = json_tok_strdup(doc, val_tok);
            } else if (str_eq(k, klen, "match")) {
                parse_match_object(&rule->match, doc, val_tok);
            } else if (starts_with(k, klen, "match:")) {
                const char *suffix = k + strlen("match:");
                size_t slen = klen - strlen("match:");
                parse_match_kv(&rule->match, suffix, slen, doc, val_tok);
            } else if (str_eq(k, klen, "class") || str_eq(k, klen, "title") ||
                       str_eq(k, klen, "initialClass") || str_eq(k, klen, "initialTitle") ||
                       str_eq(k, klen, "initial_class") || str_eq(k, klen, "initial_title") ||
                       str_eq(k, klen, "tag")) {
                parse_match_kv(&rule->match, k, klen, doc, val_tok);
            } else {
                parse_action_kv(&rule->actions, k, klen, doc, val_tok);
            }
        }
        cur = json_skip(doc, val_tok);
    }
}

static int find_rules_array(const struct json_doc *doc) {
    if (doc->tokcount <= 0) {
        return -1;
    }
    if (json_is_array(doc, 0)) {
        return 0;
    }
    if (!json_is_object(doc, 0)) {
        return -1;
    }

    const char *keys[] = {"windowrule", "windowrules", "rules", "window_rules"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        int tok = json_obj_get(doc, 0, keys[i]);
        if (tok >= 0 && json_is_array(doc, tok)) {
            return tok;
        }
    }
    return -1;
}

int ruleset_load_json(const char *path, struct ruleset *out) {
    memset(out, 0, sizeof(*out));

    struct json_doc doc;
    if (json_parse_file(path, &doc) != 0) {
        return -1;
    }

    int arr_tok = find_rules_array(&doc);
    if (arr_tok < 0) {
        json_free(&doc);
        return -1;
    }

    int count = json_arr_len(&doc, arr_tok);
    if (count <= 0) {
        json_free(&doc);
        return 0;
    }

    struct rule *rules = (struct rule *)calloc((size_t)count, sizeof(struct rule));
    if (!rules) {
        json_free(&doc);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        int tok = json_arr_at(&doc, arr_tok, i);
        if (tok < 0 || !json_is_object(&doc, tok)) {
            continue;
        }
        parse_rule_object(&rules[i], &doc, tok);
    }

    json_free(&doc);
    out->rules = rules;
    out->count = (size_t)count;
    return 0;
}

static void free_rule(struct rule *r) {
    if (!r) {
        return;
    }
    free(r->name);
    free(r->match.class_re);
    free(r->match.title_re);
    free(r->match.initial_class_re);
    free(r->match.initial_title_re);
    free(r->match.tag_re);

    free(r->actions.tag);
    free(r->actions.workspace);
    free(r->actions.opacity);
    free(r->actions.size);
    free(r->actions.move);
}

void ruleset_free(struct ruleset *set) {
    if (!set || !set->rules) {
        return;
    }
    for (size_t i = 0; i < set->count; i++) {
        free_rule(&set->rules[i]);
    }
    free(set->rules);
    set->rules = NULL;
    set->count = 0;
}
