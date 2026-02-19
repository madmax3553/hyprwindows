#include "export_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/*
 * Write rules to file, preserving original format where possible.
 *
 * Strategy (current: simple rewrite):
 * 1. Write header comment
 * 2. Write all rules using rule_write()
 *
 * TODO: format-preserving save that reads the original file and
 * patches only changed rules while keeping comments/whitespace.
 */
int export_save_rules(
    const char *original_file,
    const char *output_file,
    const struct ruleset *modified_rs
) {
    if (!original_file || !output_file || !modified_rs) {
        return -1;
    }

    (void)original_file;  /* TODO: use for format-preserving save */

    FILE *f = fopen(output_file, "w");
    if (!f) return -1;

    fprintf(f, "# Window Rules - managed by hyprwindows\n");
    fprintf(f, "# See https://wiki.hyprland.org/Configuring/Window-Rules/\n\n");

    for (size_t i = 0; i < modified_rs->count; i++) {
        rule_write(f, &modified_rs->rules[i]);
    }

    fclose(f);
    return 0;
}

int export_rule_to_file(const char *path, const struct rule *r, const char *mode) {
    if (!path || !r || !mode) return -1;

    FILE *f = fopen(path, mode);
    if (!f) return -1;

    rule_write(f, r);
    fclose(f);
    return 0;
}
