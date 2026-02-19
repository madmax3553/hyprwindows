#include "history.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void history_init(struct history_stack *h) {
    if (!h) return;

    memset(h, 0, sizeof(struct history_stack));
    h->capacity = 50;
    h->records = calloc(h->capacity, sizeof(struct change_record));
    h->current = 0;
    h->count = 0;
}

void history_record(
    struct history_stack *h,
    enum change_type type,
    int rule_index,
    const struct rule *old_state,
    const struct rule *new_state,
    const char *description
) {
    if (!h || !h->records) return;

    /* Clear redo stack when making new change */
    for (size_t i = h->current; i < h->count; i++) {
        rule_free(&h->records[i].old_state);
        rule_free(&h->records[i].new_state);
    }
    h->count = h->current;

    /* If at capacity, shift older records out */
    if (h->count >= h->capacity) {
        rule_free(&h->records[0].old_state);
        rule_free(&h->records[0].new_state);
        memmove(&h->records[0], &h->records[1],
                (h->capacity - 1) * sizeof(struct change_record));
        h->count = h->capacity - 1;
        h->current = h->capacity - 1;
    }

    /* Add new record */
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

struct rule *history_undo(struct history_stack *h) {
    if (!h || h->current == 0) return NULL;

    h->current--;
    struct change_record *rec = &h->records[h->current];

    struct rule *restored = malloc(sizeof(struct rule));
    if (restored) {
        *restored = rule_copy(&rec->old_state);
    }

    return restored;
}

struct rule *history_redo(struct history_stack *h) {
    if (!h || h->current >= h->count) return NULL;

    struct change_record *rec = &h->records[h->current];
    h->current++;

    struct rule *restored = malloc(sizeof(struct rule));
    if (restored) {
        *restored = rule_copy(&rec->new_state);
    }

    return restored;
}

int history_can_undo(const struct history_stack *h) {
    return h && h->current > 0;
}

int history_can_redo(const struct history_stack *h) {
    return h && h->current < h->count;
}

void history_get_status(const struct history_stack *h, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;

    if (!h) {
        snprintf(out, out_sz, "No history");
        return;
    }

    int pos = 0;
    pos += snprintf(out + pos, out_sz - pos, "Changes: %zu", h->count);
    if (history_can_undo(h) && pos < (int)out_sz) {
        pos += snprintf(out + pos, out_sz - pos, " | Undo available");
    }
    if (history_can_redo(h) && pos < (int)out_sz) {
        snprintf(out + pos, out_sz - pos, " | Redo available");
    }
}

void history_clear_redo(struct history_stack *h) {
    if (!h) return;

    /* Free records after current position */
    for (size_t i = h->current; i < h->count; i++) {
        rule_free(&h->records[i].old_state);
        rule_free(&h->records[i].new_state);
    }

    h->count = h->current;
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
