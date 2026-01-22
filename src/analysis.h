#ifndef HYPRWINDOWS_ANALYSIS_H
#define HYPRWINDOWS_ANALYSIS_H

#include "rules.h"
#include "hyprctl.h"

/**
 * Comprehensive rule analysis - detects 5 types of issues
 */

/**
 * Types of rule issues (flag everything)
 */
enum issue_type {
    ISSUE_EXACT_DUPLICATE,     /* Same patterns exactly */
    ISSUE_SUBSUMED,            /* Rule A overshadowed by Rule B */
    ISSUE_CONFLICTING,         /* Same match, different actions */
    ISSUE_REDUNDANT,           /* Rule B is less specific than A */
    ISSUE_ORPHANED,            /* No windows match this rule */
};

/**
 * Severity levels
 */
enum issue_severity {
    SEVERITY_ERROR,            /* Must fix */
    SEVERITY_WARNING,          /* Should fix */
    SEVERITY_INFO,             /* FYI */
};

/**
 * A single detected issue
 */
struct rule_issue {
    enum issue_type type;
    enum issue_severity severity;
    char description[256];     /* User-friendly description */
    int *affected_rules;       /* Array of rule indices involved */
    size_t affected_count;
    char suggestion[256];      /* Suggested action */
};

/**
 * Complete analysis report
 */
struct analysis_report {
    struct rule_issue *issues;
    size_t count;
    size_t errors;             /* Count by severity */
    size_t warnings;
    size_t infos;
};

/**
 * Run comprehensive analysis on ruleset
 * 
 * ruleset: loaded rules
 * clients: active windows (can be NULL for simpler analysis)
 * 
 * Returns: allocated report (must be freed by caller)
 */
struct analysis_report *analysis_run(
    const struct ruleset *ruleset,
    const struct clients *clients
);

/**
 * Free analysis report
 */
void analysis_free(struct analysis_report *report);

/**
 * Get string name for issue type
 */
const char *analysis_issue_type_string(enum issue_type type);

/**
 * Get string name for severity
 */
const char *analysis_severity_string(enum issue_severity severity);

#endif
