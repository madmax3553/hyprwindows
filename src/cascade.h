#ifndef HYPRWINDOWS_CASCADE_H
#define HYPRWINDOWS_CASCADE_H

#include "rules.h"
#include "hyprctl.h"

/**
 * Cascade analysis - explains how rules stack top-to-bottom
 * Shows which rules match a window and what they collectively do
 */

/**
 * Represents one step in a rule cascade
 */
struct cascade_step {
    int rule_index;              /* Index in ruleset */
    struct rule_actions delta;   /* What this rule changes/adds */
    char explanation[256];       /* Human-readable explanation */
};

/**
 * Result of analyzing how rules cascade for a window
 */
struct cascade_analysis {
    struct cascade_step *steps;   /* Array of matching rules */
    size_t step_count;
    struct rule_actions final;    /* Final resolved state after all rules */
    char summary[512];            /* Summary of what happens */
};

/**
 * Analyze how rules cascade for a given window
 * 
 * ruleset: loaded rules
 * client: active window to analyze
 * 
 * Returns: allocated analysis (must be freed by caller)
 */
struct cascade_analysis *cascade_analyze(
    const struct ruleset *ruleset,
    const struct client *client
);

/**
 * Free cascade analysis result
 */
void cascade_free(struct cascade_analysis *analysis);

/**
 * Get human-readable explanation of what a rule contributes
 */
char *cascade_explain_rule(
    const struct rule *rule,
    const struct rule_actions *prev_state
);

#endif
