#ifndef HYPRWINDOWS_HISTORY_H
#define HYPRWINDOWS_HISTORY_H

#include "rules.h"
#include <time.h>

/**
 * Undo/redo history system for tracking rule changes
 */

/**
 * Types of changes that can be tracked
 */
enum change_type {
    CHANGE_EDIT,          /* Rule was edited */
    CHANGE_DELETE,        /* Rule was deleted */
    CHANGE_DISABLE,       /* Rule was moved to disabled */
    CHANGE_RENAME,        /* Rule name was changed */
};

/**
 * A single change record
 */
struct change_record {
    enum change_type type;
    int rule_index;              /* Index in ruleset */
    struct rule old_state;       /* Copy of rule before change */
    struct rule new_state;       /* Copy of rule after change */
    char description[128];       /* Human-readable description */
    time_t timestamp;
};

/**
 * Undo/redo history stack
 */
struct history_stack {
    struct change_record *records;
    size_t capacity;             /* Max 50 changes */
    size_t current;              /* Current position (for redo) */
    size_t count;                /* Total records stored */
};

/**
 * Initialize history stack
 */
void history_init(struct history_stack *h);

/**
 * Record a change (push to undo stack)
 */
void history_record(
    struct history_stack *h,
    enum change_type type,
    int rule_index,
    const struct rule *old_state,
    const struct rule *new_state,
    const char *description
);

/**
 * Undo the last change
 * Returns the old rule to restore, or NULL if nothing to undo
 */
struct rule *history_undo(struct history_stack *h);

/**
 * Redo the last undone change
 * Returns the new rule to restore, or NULL if nothing to redo
 */
struct rule *history_redo(struct history_stack *h);

/**
 * Check if undo is available
 */
int history_can_undo(const struct history_stack *h);

/**
 * Check if redo is available
 */
int history_can_redo(const struct history_stack *h);

/**
 * Get status string (e.g., "3 changes, undo available")
 */
void history_get_status(const struct history_stack *h, char *out, size_t out_sz);

/**
 * Clear redo stack (when making new change after undo)
 */
void history_clear_redo(struct history_stack *h);

/**
 * Free history stack
 */
void history_free(struct history_stack *h);

#endif
