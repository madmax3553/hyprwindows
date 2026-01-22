#include "export_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Read a file into memory
 */
static char *read_file(const char *path, size_t *out_size) __attribute__((unused));
static char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    size_t read = fread(buf, 1, size, f);
    fclose(f);
    
    if (read != (size_t)size) {
        free(buf);
        return NULL;
    }
    
    buf[size] = '\0';
    if (out_size) *out_size = size;
    
    return buf;
}

/**
 * Find the start of a windowrule block in file content
 */
static char *find_windowrule_block(char *content, const char *pattern) __attribute__((unused));
static char *find_windowrule_block(char *content, const char *pattern) {
    if (!content || !pattern) return NULL;
    
    char *pos = content;
    while ((pos = strstr(pos, "windowrule")) != NULL) {
        /* Found a windowrule, now check if it contains our pattern */
        char *block_end = strstr(pos, "}");
        if (!block_end) {
            pos++;
            continue;
        }
        
        char *pattern_in_block = strstr(pos, pattern);
        if (pattern_in_block && pattern_in_block < block_end) {
            return pos;  /* Found the right block */
        }
        
        pos = block_end + 1;
    }
    
    return NULL;
}

/**
 * Write rules to file, preserving original format where possible
 * 
 * Strategy:
 * 1. If original file exists, try to preserve its structure
 * 2. For each modified rule, find and update its block in the original
 * 3. Append new rules at the end
 * 4. Delete rules that were removed (comment them out or delete)
 */
int export_save_rules(
    const char *original_file,
    const char *output_file,
    const struct ruleset *modified_rs
) {
    if (!original_file || !output_file || !modified_rs) {
        return -1;
    }
    
    /* For now, implement simple approach: write all rules from scratch */
    /* TODO: Implement format-preserving logic */
    
    FILE *f = fopen(output_file, "w");
    if (!f) return -1;
    
    /* Write header */
    fprintf(f, "# Window Rules - managed by hyprwindows\n");
    fprintf(f, "# See https://wiki.hyprland.org/Configuring/Window-Rules/\n\n");
    
    /* Write each rule */
    for (size_t i = 0; i < modified_rs->count; i++) {
        struct rule *r = &modified_rs->rules[i];
        
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
        
        /* Write extras */
        for (size_t j = 0; j < r->extras_count; j++) {
            fprintf(f, "    %s = %s\n", r->extras[j].key, r->extras[j].value);
        }
        
        fprintf(f, "}\n\n");
    }
    
    fclose(f);
    return 0;
}

/**
 * Write a single rule to file
 */
int export_rule_to_file(const char *path, const struct rule *r, const char *mode) {
    if (!path || !r || !mode) return -1;
    
    FILE *f = fopen(path, mode);
    if (!f) return -1;
    
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
    
    for (size_t i = 0; i < r->extras_count; i++) {
        fprintf(f, "    %s = %s\n", r->extras[i].key, r->extras[i].value);
    }
    
    fprintf(f, "}\n\n");
    fclose(f);
    return 0;
}
