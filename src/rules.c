#include "rules.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hyprconf.h"

static void free_rule(struct rule *r) {
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

int ruleset_load(const char *path, struct ruleset *out) {
    return hyprconf_parse_file(path, out);
}

static char *expand_home(const char *path) {
    if (!path || path[0] != '~') {
        return strdup(path);
    }
    const char *home = getenv("HOME");
    if (!home) {
        home = ".";
    }
    size_t hlen = strlen(home);
    size_t plen = strlen(path);
    char *out = (char *)malloc(hlen + plen);
    if (!out) {
        return NULL;
    }
    strcpy(out, home);
    strcat(out, path + 1);
    return out;
}

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
        /* skip whitespace */
        while (*line && isspace((unsigned char)*line)) {
            line++;
        }
        /* skip comments */
        if (*line == '#') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }
        /* look for source = */
        if (strncmp(line, "source", 6) == 0) {
            char *p = line + 6;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '=') {
                p++;
                while (*p && isspace((unsigned char)*p)) p++;
                /* extract path */
                char *start = p;
                while (*p && *p != '\n' && *p != '#') p++;
                size_t plen = (size_t)(p - start);
                /* trim trailing whitespace */
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
        /* next line */
        while (*line && *line != '\n') line++;
        if (*line == '\n') line++;
    }

    /* check if hyprland.conf itself has windowrules */
    if (strstr(buf, "windowrule") != NULL) {
        free(buf);
        return strdup(hyprconf);
    }

    free(buf);
    return NULL;
}
