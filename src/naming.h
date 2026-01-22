#ifndef HYPRWINDOWS_NAMING_H
#define HYPRWINDOWS_NAMING_H

#include "rules.h"

/**
 * Rule naming system - generates human-readable names for rules
 * and manages the workflow for naming rules
 */

/**
 * Generate a suggested human-readable name for a rule
 * based on class, title, appmap lookup, or fallback
 * 
 * Returns: allocated string (must be freed by caller)
 */
char *naming_suggest_name(struct rule *r);

/**
 * Update a rule's name and mark as user-assigned
 * 
 * old_name: previous name (for undo/history)
 * new_name: new name to assign
 */
void naming_set_rule_name(struct rule *r, const char *new_name);

/**
 * Check if rule's display_name differs from actual name field
 * Useful for detecting mismatches when editing
 */
int naming_has_mismatch(struct rule *r);

/**
 * Get the best name to display (preferring user-assigned name)
 */
const char *naming_get_display_name(struct rule *r);

#endif
