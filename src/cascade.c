#include "cascade.h"
#include "actions.h"
#include "util.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* helper: safe append to a bounded buffer, tracking position */
static int buf_append(char *buf, size_t sz, int pos, const char *fmt, ...) {
    if (pos < 0 || (size_t)pos >= sz) return pos;
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + pos, sz - pos, fmt, args);
    va_end(args);
    return (n > 0) ? pos + n : pos;
}

static void cascade_make_explanation(
    const struct rule_actions *prev,
    struct rule_actions *current,
    char *out,
    size_t out_sz
) {
    if (!out || out_sz == 0) return;

    out[0] = '\0';
    char buf[240];
    int pos = 0;
    int any = 0;

    if (current->workspace && (!prev->workspace || strcmp(current->workspace, prev->workspace) != 0)) {
        pos = buf_append(buf, sizeof(buf), pos, "workspace=%s", current->workspace);
        any = 1;
    }
    if (current->tag && (!prev->tag || strcmp(current->tag, prev->tag) != 0)) {
        if (any) pos = buf_append(buf, sizeof(buf), pos, ", ");
        pos = buf_append(buf, sizeof(buf), pos, "tag=%s", current->tag);
        any = 1;
    }
    if (current->float_set && current->float_val && (!prev->float_set || !prev->float_val)) {
        if (any) pos = buf_append(buf, sizeof(buf), pos, ", ");
        pos = buf_append(buf, sizeof(buf), pos, "float=true");
        any = 1;
    }
    if (current->center_set && current->center_val && (!prev->center_set || !prev->center_val)) {
        if (any) pos = buf_append(buf, sizeof(buf), pos, ", ");
        pos = buf_append(buf, sizeof(buf), pos, "center=true");
        any = 1;
    }
    if (current->size && (!prev->size || strcmp(current->size, prev->size) != 0)) {
        if (any) pos = buf_append(buf, sizeof(buf), pos, ", ");
        pos = buf_append(buf, sizeof(buf), pos, "size=%s", current->size);
        any = 1;
    }
    if (current->opacity && (!prev->opacity || strcmp(current->opacity, prev->opacity) != 0)) {
        if (any) pos = buf_append(buf, sizeof(buf), pos, ", ");
        pos = buf_append(buf, sizeof(buf), pos, "opacity=%s", current->opacity);
        any = 1;
    }

    if (any) {
        snprintf(out, out_sz, "Adds: %s", buf);
    } else {
        snprintf(out, out_sz, "No visible changes");
    }
}

static void actions_merge(struct rule_actions *current, const struct rule_actions *new_rule) {
    if (!current || !new_rule) return;

    if (new_rule->workspace) {
        free(current->workspace);
        current->workspace = strdup(new_rule->workspace);
    }
    if (new_rule->tag) {
        free(current->tag);
        current->tag = strdup(new_rule->tag);
    }
    if (new_rule->float_set) {
        current->float_set = 1;
        current->float_val = new_rule->float_val;
    }
    if (new_rule->center_set) {
        current->center_set = 1;
        current->center_val = new_rule->center_val;
    }
    if (new_rule->size) {
        free(current->size);
        current->size = strdup(new_rule->size);
    }
    if (new_rule->move) {
        free(current->move);
        current->move = strdup(new_rule->move);
    }
    if (new_rule->opacity) {
        free(current->opacity);
        current->opacity = strdup(new_rule->opacity);
    }
}

struct cascade_analysis *cascade_analyze(
    const struct ruleset *ruleset,
    const struct client *client
) {
    if (!ruleset || !client) return NULL;

    struct cascade_analysis *analysis = calloc(1, sizeof(struct cascade_analysis));
    if (!analysis) return NULL;

    struct rule_actions current = {0};

    analysis->steps = calloc(ruleset->count, sizeof(struct cascade_step));
    if (!analysis->steps) {
        free(analysis);
        return NULL;
    }

    for (size_t i = 0; i < ruleset->count; i++) {
        struct rule *r = &ruleset->rules[i];

        if (!rule_matches_client(r, client)) {
            continue;
        }

        struct rule_actions prev = current;

        struct cascade_step *step = &analysis->steps[analysis->step_count];
        step->rule_index = i;

        step->delta.workspace = r->actions.workspace ? strdup(r->actions.workspace) : NULL;
        step->delta.tag = r->actions.tag ? strdup(r->actions.tag) : NULL;
        step->delta.float_set = r->actions.float_set;
        step->delta.float_val = r->actions.float_val;
        step->delta.center_set = r->actions.center_set;
        step->delta.center_val = r->actions.center_val;
        step->delta.size = r->actions.size ? strdup(r->actions.size) : NULL;
        step->delta.move = r->actions.move ? strdup(r->actions.move) : NULL;
        step->delta.opacity = r->actions.opacity ? strdup(r->actions.opacity) : NULL;

        actions_merge(&current, &r->actions);

        cascade_make_explanation(&prev, &current, step->explanation, sizeof(step->explanation));

        analysis->step_count++;
    }

    analysis->final = current;

    /* Generate summary with bounds-safe snprintf */
    if (analysis->step_count == 0) {
        snprintf(analysis->summary, sizeof(analysis->summary),
                 "No rules match this window");
    } else {
        int pos = snprintf(analysis->summary, sizeof(analysis->summary),
                           "%zu rule(s) match: ", analysis->step_count);
        for (size_t i = 0; i < analysis->step_count && i < 3; i++) {
            const char *name = ruleset->rules[analysis->steps[i].rule_index].display_name;
            if (!name) name = "(unnamed)";
            if (i > 0 && pos < (int)sizeof(analysis->summary)) {
                pos += snprintf(analysis->summary + pos,
                                sizeof(analysis->summary) - pos, " > ");
            }
            if (pos < (int)sizeof(analysis->summary)) {
                pos += snprintf(analysis->summary + pos,
                                sizeof(analysis->summary) - pos, "%s", name);
            }
        }
        if (analysis->step_count > 3 && pos < (int)sizeof(analysis->summary)) {
            snprintf(analysis->summary + pos,
                     sizeof(analysis->summary) - pos, " + more");
        }
    }

    return analysis;
}

void cascade_free(struct cascade_analysis *analysis) {
    if (!analysis) return;

    for (size_t i = 0; i < analysis->step_count; i++) {
        free(analysis->steps[i].delta.workspace);
        free(analysis->steps[i].delta.tag);
        free(analysis->steps[i].delta.size);
        free(analysis->steps[i].delta.move);
        free(analysis->steps[i].delta.opacity);
    }
    free(analysis->steps);

    free(analysis->final.workspace);
    free(analysis->final.tag);
    free(analysis->final.size);
    free(analysis->final.move);
    free(analysis->final.opacity);

    free(analysis);
}

char *cascade_explain_rule(
    const struct rule *rule,
    const struct rule_actions *prev_state __attribute__((unused))
) {
    if (!rule) return strdup("(unknown rule)");

    char *buf = malloc(512);
    if (!buf) return strdup("(memory error)");

    char actions[256];
    int pos = 0;
    int any = 0;

    if (rule->actions.workspace) {
        pos += snprintf(actions + pos, sizeof(actions) - pos, "workspace=%s", rule->actions.workspace);
        any = 1;
    }
    if (rule->actions.tag) {
        if (any) pos += snprintf(actions + pos, sizeof(actions) - pos, ", ");
        pos += snprintf(actions + pos, sizeof(actions) - pos, "tag=%s", rule->actions.tag);
        any = 1;
    }
    if (rule->actions.float_set) {
        if (any) pos += snprintf(actions + pos, sizeof(actions) - pos, ", ");
        pos += snprintf(actions + pos, sizeof(actions) - pos, "%s",
                        rule->actions.float_val ? "float" : "nofloat");
        any = 1;
    }
    if (rule->actions.center_set) {
        if (any) pos += snprintf(actions + pos, sizeof(actions) - pos, ", ");
        pos += snprintf(actions + pos, sizeof(actions) - pos, "%s",
                        rule->actions.center_val ? "center" : "nocenter");
        any = 1;
    }
    if (rule->actions.opacity) {
        if (any) pos += snprintf(actions + pos, sizeof(actions) - pos, ", ");
        snprintf(actions + pos, sizeof(actions) - pos, "opacity=%s", rule->actions.opacity);
        any = 1;
    }

    if (any) {
        snprintf(buf, 512, "Applies: %s", actions);
    } else {
        snprintf(buf, 512, "No explicit actions");
    }

    return buf;
}
