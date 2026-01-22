#ifndef HYPRWINDOWS_EXPORT_RULES_H
#define HYPRWINDOWS_EXPORT_RULES_H

#include "rules.h"

/**
 * Format-preserving save of rules to config file
 * Maintains original formatting, comments, and structure while updating rule changes
 */

/**
 * Save modified ruleset back to original config file
 * Attempts to preserve formatting and comments while updating rules
 * 
 * original_file: path to original hyprland config file
 * output_file: path to write updated config (can be same as original)
 * modified_rs: the modified ruleset to save
 * 
 * Returns: 0 on success, -1 on failure
 */
int export_save_rules(
    const char *original_file,
    const char *output_file,
    const struct ruleset *modified_rs
);

/**
 * Write a single rule to file in hyprland format
 * (Already exists in ui.c, but exposed here for consistency)
 */
int export_rule_to_file(const char *path, const struct rule *r, const char *mode);

#endif
