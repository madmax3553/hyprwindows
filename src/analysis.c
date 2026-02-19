#include "analysis.h"
#include "actions.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int rules_exact_duplicate(const struct rule *a, const struct rule *b) {
    if (!a || !b || a == b) return 0;

    if (a->match.class_re && b->match.class_re) {
        if (strcmp(a->match.class_re, b->match.class_re) != 0) return 0;
    } else if (a->match.class_re != b->match.class_re) {
        return 0;
    }

    if (a->match.title_re && b->match.title_re) {
        if (strcmp(a->match.title_re, b->match.title_re) != 0) return 0;
    } else if (a->match.title_re != b->match.title_re) {
        return 0;
    }

    return 1;
}

static int pattern_subsumed_by(const struct rule_match *a, const struct rule_match *b) {
    if (!a || !b) return 0;

    if (a->class_re && b->class_re) {
        const char *a_pattern = a->class_re;
        const char *b_pattern = b->class_re;

        if (a_pattern[0] == '^') a_pattern++;
        if (b_pattern[0] == '^') b_pattern++;

        size_t a_len = strlen(a_pattern);
        size_t b_len = strlen(b_pattern);

        if (a_len > 0 && b_len > 0) {
            if (strstr(b_pattern, a_pattern) != NULL) {
                return 1;
            }
        }
    }

    return 0;
}

static int rules_conflicting_actions(const struct rule *a, const struct rule *b) {
    if (!a || !b) return 0;

    if (a->match.class_re && b->match.class_re) {
        if (strcmp(a->match.class_re, b->match.class_re) != 0) return 0;
    } else {
        return 0;
    }

    if (a->actions.workspace && b->actions.workspace) {
        if (strcmp(a->actions.workspace, b->actions.workspace) != 0) return 1;
    }
    if (a->actions.tag && b->actions.tag) {
        if (strcmp(a->actions.tag, b->actions.tag) != 0) return 1;
    }
    if (a->actions.float_set && b->actions.float_set) {
        if (a->actions.float_val != b->actions.float_val) return 1;
    }

    return 0;
}

/* dynamically growing issue list to prevent heap overflow */
static int analysis_add_issue(
    struct analysis_report *report,
    size_t *capacity,
    enum issue_type type,
    enum issue_severity severity,
    const char *description,
    const char *suggestion,
    int *affected_rules,
    size_t affected_count
) {
    if (!report) return -1;

    /* grow if needed */
    if (report->count >= *capacity) {
        size_t new_cap = *capacity == 0 ? 16 : *capacity * 2;
        struct rule_issue *new_issues = realloc(report->issues, new_cap * sizeof(struct rule_issue));
        if (!new_issues) return -1;
        report->issues = new_issues;
        *capacity = new_cap;
    }

    struct rule_issue *issue = &report->issues[report->count];
    memset(issue, 0, sizeof(*issue));
    issue->type = type;
    issue->severity = severity;

    snprintf(issue->description, sizeof(issue->description), "%s", description);
    snprintf(issue->suggestion, sizeof(issue->suggestion), "%s", suggestion);

    issue->affected_rules = malloc(affected_count * sizeof(int));
    if (issue->affected_rules) {
        memcpy(issue->affected_rules, affected_rules, affected_count * sizeof(int));
        issue->affected_count = affected_count;
    }

    report->count++;

    switch (severity) {
        case SEVERITY_ERROR: report->errors++; break;
        case SEVERITY_WARNING: report->warnings++; break;
        case SEVERITY_INFO: report->infos++; break;
    }
    return 0;
}

struct analysis_report *analysis_run(
    const struct ruleset *ruleset,
    const struct clients *clients
) {
    if (!ruleset) return NULL;

    struct analysis_report *report = calloc(1, sizeof(struct analysis_report));
    if (!report) return NULL;

    size_t capacity = 0;  /* managed by analysis_add_issue */

    for (size_t i = 0; i < ruleset->count; i++) {
        struct rule *ri = &ruleset->rules[i];

        for (size_t j = i + 1; j < ruleset->count; j++) {
            struct rule *rj = &ruleset->rules[j];

            int affected[2] = {(int)i, (int)j};

            if (rules_exact_duplicate(ri, rj)) {
                char desc[256];
                snprintf(desc, sizeof(desc),
                        "Exact duplicate: Rule %zu and Rule %zu match identically",
                        i, j);
                analysis_add_issue(report, &capacity, ISSUE_EXACT_DUPLICATE, SEVERITY_ERROR,
                                 desc, "Delete one of the duplicate rules",
                                 affected, 2);
            }

            if (pattern_subsumed_by(&ri->match, &rj->match)) {
                char desc[256];
                snprintf(desc, sizeof(desc),
                        "Subsumed rule: Rule %zu's pattern is covered by Rule %zu",
                        i, j);
                analysis_add_issue(report, &capacity, ISSUE_SUBSUMED, SEVERITY_WARNING,
                                 desc, "Rule will never match if broader rule comes first",
                                 affected, 2);
            }

            if (rules_conflicting_actions(ri, rj)) {
                char desc[256];
                snprintf(desc, sizeof(desc),
                        "Conflicting actions: Rule %zu and Rule %zu match same pattern with different actions",
                        i, j);
                analysis_add_issue(report, &capacity, ISSUE_CONFLICTING, SEVERITY_WARNING,
                                 desc, "Clarify which rule should take precedence",
                                 affected, 2);
            }
        }

        /* Check if orphaned */
        if (clients) {
            int matched = 0;
            for (size_t c = 0; c < clients->count; c++) {
                if (rule_matches_client(ri, &clients->items[c])) {
                    matched = 1;
                    break;
                }
            }

            if (!matched) {
                char desc[256];
                snprintf(desc, sizeof(desc),
                        "Orphaned rule: Rule %zu doesn't match any currently open windows",
                        i);
                int idx = (int)i;
                analysis_add_issue(report, &capacity, ISSUE_ORPHANED, SEVERITY_INFO,
                                 desc, "Rule may be unused, or for apps not currently running",
                                 &idx, 1);
            }
        }
    }

    return report;
}

void analysis_free(struct analysis_report *report) {
    if (!report) return;

    for (size_t i = 0; i < report->count; i++) {
        free(report->issues[i].affected_rules);
    }
    free(report->issues);
    free(report);
}

const char *analysis_issue_type_string(enum issue_type type) {
    switch (type) {
        case ISSUE_EXACT_DUPLICATE: return "Exact Duplicate";
        case ISSUE_SUBSUMED: return "Subsumed Rule";
        case ISSUE_CONFLICTING: return "Conflicting Actions";
        case ISSUE_REDUNDANT: return "Redundant Rule";
        case ISSUE_ORPHANED: return "Orphaned Rule";
        default: return "(unknown)";
    }
}

const char *analysis_severity_string(enum issue_severity severity) {
    switch (severity) {
        case SEVERITY_ERROR: return "ERROR";
        case SEVERITY_WARNING: return "WARNING";
        case SEVERITY_INFO: return "INFO";
        default: return "(unknown)";
    }
}
