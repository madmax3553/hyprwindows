#include "rules.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hyprconf.h"
#include "util.h"

/* --- single rule lifecycle (public) --- */

void rule_free(struct rule *r) {
    if (!r) {
        return;
    }
    free(r->name);
    free(r->display_name);
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

    for (size_t i = 0; i < r->extras_count; i++) {
        free(r->extras[i].key);
        free(r->extras[i].value);
    }
    free(r->extras);
}

struct rule rule_copy(const struct rule *src) {
    struct rule dst = {0};
    if (!src) return dst;

    dst.name = src->name ? strdup(src->name) : NULL;
    dst.display_name = src->display_name ? strdup(src->display_name) : NULL;

    dst.match.class_re = src->match.class_re ? strdup(src->match.class_re) : NULL;
    dst.match.title_re = src->match.title_re ? strdup(src->match.title_re) : NULL;
    dst.match.initial_class_re = src->match.initial_class_re ? strdup(src->match.initial_class_re) : NULL;
    dst.match.initial_title_re = src->match.initial_title_re ? strdup(src->match.initial_title_re) : NULL;
    dst.match.tag_re = src->match.tag_re ? strdup(src->match.tag_re) : NULL;

    dst.actions.tag = src->actions.tag ? strdup(src->actions.tag) : NULL;
    dst.actions.workspace = src->actions.workspace ? strdup(src->actions.workspace) : NULL;
    dst.actions.float_set = src->actions.float_set;
    dst.actions.float_val = src->actions.float_val;
    dst.actions.center_set = src->actions.center_set;
    dst.actions.center_val = src->actions.center_val;
    dst.actions.size = src->actions.size ? strdup(src->actions.size) : NULL;
    dst.actions.move = src->actions.move ? strdup(src->actions.move) : NULL;
    dst.actions.opacity = src->actions.opacity ? strdup(src->actions.opacity) : NULL;

    if (src->extras_count > 0) {
        dst.extras = malloc(src->extras_count * sizeof(struct rule_extra));
        if (dst.extras) {
            for (size_t i = 0; i < src->extras_count; i++) {
                dst.extras[i].key = strdup(src->extras[i].key);
                dst.extras[i].value = strdup(src->extras[i].value);
            }
            dst.extras_count = src->extras_count;
        }
    }

    return dst;
}

void rule_write(FILE *f, const struct rule *r) {
    if (!f || !r) return;

    fprintf(f, "windowrule {\n");
    if (r->name) fprintf(f, "    name = %s\n", r->name);
    if (r->match.class_re) fprintf(f, "    match:class = %s\n", r->match.class_re);
    if (r->match.title_re) fprintf(f, "    match:title = %s\n", r->match.title_re);
    if (r->match.initial_class_re) fprintf(f, "    match:initial_class = %s\n", r->match.initial_class_re);
    if (r->match.initial_title_re) fprintf(f, "    match:initial_title = %s\n", r->match.initial_title_re);
    if (r->match.tag_re) fprintf(f, "    match:tag = %s\n", r->match.tag_re);
    if (r->actions.tag) fprintf(f, "    tag = %s\n", r->actions.tag);
    if (r->actions.workspace) fprintf(f, "    workspace = %s\n", r->actions.workspace);
    if (r->actions.float_set) fprintf(f, "    float = %s\n", r->actions.float_val ? "true" : "false");
    if (r->actions.center_set) fprintf(f, "    center = %s\n", r->actions.center_val ? "true" : "false");
    if (r->actions.size) fprintf(f, "    size = %s\n", r->actions.size);
    if (r->actions.move) fprintf(f, "    move = %s\n", r->actions.move);
    if (r->actions.opacity) fprintf(f, "    opacity = %s\n", r->actions.opacity);
    for (size_t j = 0; j < r->extras_count; j++) {
        fprintf(f, "    %s = %s\n", r->extras[j].key, r->extras[j].value);
    }
    fprintf(f, "}\n\n");
}

/* --- ruleset --- */

void ruleset_free(struct ruleset *set) {
    if (!set || !set->rules) {
        return;
    }
    for (size_t i = 0; i < set->count; i++) {
        rule_free(&set->rules[i]);
    }
    free(set->rules);
    set->rules = NULL;
    set->count = 0;
}

int ruleset_load(const char *path, struct ruleset *out) {
    return hyprconf_parse_file(path, out);
}

/* --- auto-detect config --- */

static int file_has_windowrules(const char *path) {
    char *expanded = expand_home(path);
    if (!expanded) {
        return 0;
    }
    size_t len = 0;
    char *buf = read_file(expanded, &len);
    free(expanded);
    if (!buf) {
        return 0;
    }
    int found = strstr(buf, "windowrule") != NULL;
    free(buf);
    return found;
}

char *hypr_find_rules_config(void) {
    const char *home = getenv("HOME");
    if (!home) {
        return NULL;
    }

    char hyprconf[512];
    snprintf(hyprconf, sizeof(hyprconf), "%s/.config/hypr/hyprland.conf", home);

    size_t len = 0;
    char *buf = read_file(hyprconf, &len);
    if (!buf) {
        return NULL;
    }

    /* scan for source = lines and check if they contain windowrules */
    char *line = buf;
    while (line && *line) {
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }
        if (*line == '#') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }
        if (strncmp(line, "source", 6) == 0) {
            char *p = line + 6;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '=') {
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
                char *start = p;
                while (*p && *p != '\n' && *p != '#') p++;
                size_t plen = (size_t)(p - start);
                while (plen > 0 && isspace((unsigned char)start[plen - 1])) plen--;
                if (plen > 0) {
                    char *spath = (char *)malloc(plen + 1);
                    if (spath) {
                        memcpy(spath, start, plen);
                        spath[plen] = '\0';
                        if (file_has_windowrules(spath)) {
                            free(buf);
                            char *expanded = expand_home(spath);
                            free(spath);
                            return expanded;
                        }
                        free(spath);
                    }
                }
            }
        }
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    if (strstr(buf, "windowrule") != NULL) {
        free(buf);
        return strdup(hyprconf);
    }

    free(buf);
    return NULL;
}
