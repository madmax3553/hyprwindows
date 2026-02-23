#include "history.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void history_init(struct history_stack *h) {
    if (!h) return;
    memset(h, 0, sizeof(struct history_stack));
    h->capacity = 50;
    h->records = calloc(h->capacity, sizeof(struct change_record));
}

void history_record(struct history_stack *h, enum change_type type, int rule_index,
                    const struct rule *old_state, const struct rule *new_state,
                    const char *description) {
    if (!h || !h->records) return;

    /* clear redo stack */
    for (size_t i = h->current; i < h->count; i++) {
        rule_free(&h->records[i].old_state);
        rule_free(&h->records[i].new_state);
    }
    h->count = h->current;

    /* shift if at capacity */
    if (h->count >= h->capacity) {
        rule_free(&h->records[0].old_state);
        rule_free(&h->records[0].new_state);
        memmove(&h->records[0], &h->records[1],
                (h->capacity - 1) * sizeof(struct change_record));
        h->count = h->capacity - 1;
        h->current = h->capacity - 1;
    }

    struct change_record *rec = &h->records[h->count];
    rec->type = type;
    rec->rule_index = rule_index;
    rec->old_state = rule_copy(old_state);
    rec->new_state = rule_copy(new_state);
    rec->timestamp = time(NULL);
    snprintf(rec->description, sizeof(rec->description), "%s", description);

    h->count++;
    h->current = h->count;
}

struct rule *history_undo(struct history_stack *h, int *out_index, enum change_type *out_type) {
    if (!h || h->current == 0) return NULL;

    h->current--;
    struct change_record *rec = &h->records[h->current];

    if (out_index) *out_index = rec->rule_index;
    if (out_type) *out_type = rec->type;

    struct rule *restored = malloc(sizeof(struct rule));
    if (restored) {
        *restored = rule_copy(&rec->old_state);
    }
    return restored;
}

struct rule *history_redo(struct history_stack *h, int *out_index, enum change_type *out_type) {
    if (!h || h->current >= h->count) return NULL;

    struct change_record *rec = &h->records[h->current];
    h->current++;

    if (out_index) *out_index = rec->rule_index;
    if (out_type) *out_type = rec->type;

    /* for delete records, return old_state (the deleted rule) so caller knows what was deleted;
       for edit records, return new_state as before */
    struct rule *restored = malloc(sizeof(struct rule));
    if (restored) {
        *restored = (rec->type == CHANGE_DELETE)
                    ? rule_copy(&rec->old_state)
                    : rule_copy(&rec->new_state);
    }
    return restored;
}

int history_can_undo(const struct history_stack *h) {
    return h && h->current > 0;
}

int history_can_redo(const struct history_stack *h) {
    return h && h->current < h->count;
}

void history_free(struct history_stack *h) {
    if (!h) return;
    for (size_t i = 0; i < h->count; i++) {
        rule_free(&h->records[i].old_state);
        rule_free(&h->records[i].new_state);
    }
    free(h->records);
    memset(h, 0, sizeof(struct history_stack));
}
