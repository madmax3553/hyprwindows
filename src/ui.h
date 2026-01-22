#ifndef HYPRWINDOWS_UI_H
#define HYPRWINDOWS_UI_H

#include "history.h"
#include "cascade.h"
#include "analysis.h"

/**
 * Search state for /filter functionality
 */
struct search_state {
    char query[256];           /* Current search text */
    int *matches;              /* Array of matching rule indices */
    size_t match_count;
    int current_match;         /* Which match is highlighted */
    int active;                /* Search is active */
};

/**
 * Grouping mode for rule display
 */
enum grouping_mode {
    GROUP_BY_WORKSPACE,        /* Default */
    GROUP_BY_TAG,
    GROUP_BY_FLOAT,
    GROUP_UNGROUPED,
};

int run_tui(void);

#endif
