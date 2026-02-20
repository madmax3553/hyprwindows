#ifndef HYPRWINDOWS_HISTORY_H
#define HYPRWINDOWS_HISTORY_H

#include "rules.h"
#include <time.h>

/* undo/redo history for rule edits */

struct change_record {
    int rule_index;
    struct rule old_state;
    struct rule new_state;
    char description[128];
    time_t timestamp;
};

struct history_stack {
    struct change_record *records;
    size_t capacity;
    size_t current;
    size_t count;
};

void history_init(struct history_stack *h);
void history_record(struct history_stack *h, int rule_index,
                    const struct rule *old_state, const struct rule *new_state,
                    const char *description);
struct rule *history_undo(struct history_stack *h, int *out_index);
struct rule *history_redo(struct history_stack *h, int *out_index);
int history_can_undo(const struct history_stack *h);
int history_can_redo(const struct history_stack *h);
void history_free(struct history_stack *h);

#endif
