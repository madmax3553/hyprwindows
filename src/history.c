#include "history.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**
 * Deep copy a rule (including all allocated strings)
 */
static struct rule rule_copy(const struct rule *src) {
    struct rule dst = {0};
    
    if (!src) return dst;
    
    dst.name = src->name ? strdup(src->name) : NULL;
    dst.display_name = src->display_name ? strdup(src->display_name) : NULL;
    
    /* Copy match patterns */
    dst.match.class_re = src->match.class_re ? strdup(src->match.class_re) : NULL;
    dst.match.title_re = src->match.title_re ? strdup(src->match.title_re) : NULL;
    dst.match.initial_class_re = src->match.initial_class_re ? strdup(src->match.initial_class_re) : NULL;
    dst.match.initial_title_re = src->match.initial_title_re ? strdup(src->match.initial_title_re) : NULL;
    dst.match.tag_re = src->match.tag_re ? strdup(src->match.tag_re) : NULL;
    
    /* Copy actions */
    dst.actions.tag = src->actions.tag ? strdup(src->actions.tag) : NULL;
    dst.actions.workspace = src->actions.workspace ? strdup(src->actions.workspace) : NULL;
    dst.actions.float_set = src->actions.float_set;
    dst.actions.float_val = src->actions.float_val;
    dst.actions.center_set = src->actions.center_set;
    dst.actions.center_val = src->actions.center_val;
    dst.actions.size = src->actions.size ? strdup(src->actions.size) : NULL;
    dst.actions.move = src->actions.move ? strdup(src->actions.move) : NULL;
    dst.actions.opacity = src->actions.opacity ? strdup(src->actions.opacity) : NULL;
    
    /* Copy extras (simplified - just shallow copy for now) */
    if (src->extras_count > 0) {
        dst.extras = malloc(src->extras_count * sizeof(struct rule_extra));
        if (dst.extras) {
            for (size_t i = 0; i < src->extras_count; i++) {
                dst.extras[i].key = strdup(src->extras[i].key);
                dst.extras[i].value = strdup(src->extras[i].value);
            }
            dst.extras_count = src->extras_count;
        }
    }
    
    return dst;
}

/**
 * Initialize history stack
 */
void history_init(struct history_stack *h) {
    if (!h) return;
    
    memset(h, 0, sizeof(struct history_stack));
    h->capacity = 50;
    h->records = calloc(h->capacity, sizeof(struct change_record));
    h->current = 0;
    h->count = 0;
}

/**
 * Record a change
 */
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
    h->count = h->current;
    
    /* If at capacity, shift older records out */
    if (h->count >= h->capacity) {
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

/**
 * Undo - revert to old state
 */
struct rule *history_undo(struct history_stack *h) {
    if (!h || h->current == 0) return NULL;
    
    h->current--;
    struct change_record *rec = &h->records[h->current];
    
    /* Return copy of old state to restore */
    struct rule *restored = malloc(sizeof(struct rule));
    if (restored) {
        *restored = rule_copy(&rec->old_state);
    }
    
    return restored;
}

/**
 * Redo - reapply state
 */
struct rule *history_redo(struct history_stack *h) {
    if (!h || h->current >= h->count) return NULL;
    
    struct change_record *rec = &h->records[h->current];
    h->current++;
    
    /* Return copy of new state to restore */
    struct rule *restored = malloc(sizeof(struct rule));
    if (restored) {
        *restored = rule_copy(&rec->new_state);
    }
    
    return restored;
}

/**
 * Check if undo available
 */
int history_can_undo(const struct history_stack *h) {
    return h && h->current > 0;
}

/**
 * Check if redo available
 */
int history_can_redo(const struct history_stack *h) {
    return h && h->current < h->count;
}

/**
 * Get status string
 */
void history_get_status(const struct history_stack *h, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    
    if (!h) {
        snprintf(out, out_sz, "No history");
        return;
    }
    
    char buf[256] = "";
    snprintf(buf, sizeof(buf), "Changes: %zu", h->count);
    
    if (history_can_undo(h)) {
        strcat(buf, " | Undo available");
    }
    if (history_can_redo(h)) {
        strcat(buf, " | Redo available");
    }
    
    snprintf(out, out_sz, "%s", buf);
}

/**
 * Clear redo stack
 */
void history_clear_redo(struct history_stack *h) {
    if (!h) return;
    
    /* Free records after current position */
    for (size_t i = h->current; i < h->count; i++) {
        /* ruleset_free would normally do this, but we're just freeing individual rules */
        /* For now, leave them - they'll be overwritten */
    }
    
    h->count = h->current;
}

/**
 * Free a single rule's allocated fields
 */
static void rule_free_fields(struct rule *r) {
    if (!r) return;
    
    free(r->name);
    free(r->display_name);
    free(r->match.class_re);
    free(r->match.title_re);
    free(r->match.initial_class_re);
    free(r->match.initial_title_re);
    free(r->match.tag_re);
    free(r->actions.tag);
    free(r->actions.workspace);
    free(r->actions.size);
    free(r->actions.move);
    free(r->actions.opacity);
    
    for (size_t i = 0; i < r->extras_count; i++) {
        free(r->extras[i].key);
        free(r->extras[i].value);
    }
    free(r->extras);
}

/**
 * Free history stack
 */
void history_free(struct history_stack *h) {
    if (!h) return;
    
    for (size_t i = 0; i < h->count; i++) {
        rule_free_fields(&h->records[i].old_state);
        rule_free_fields(&h->records[i].new_state);
    }
    
    free(h->records);
    memset(h, 0, sizeof(struct history_stack));
}
