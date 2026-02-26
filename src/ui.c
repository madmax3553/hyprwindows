#include "ui.h"

#include <ctype.h>
#include <locale.h>
#include <notcurses/notcurses.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "actions.h"
#include "hyprctl.h"
#include "rules.h"
#include "util.h"
#include "history.h"

#define UI_MIN_WIDTH 80
#define UI_MIN_HEIGHT 24

/* views */
enum view_mode {
    VIEW_RULES,
    VIEW_WINDOWS,
    VIEW_REVIEW,
    VIEW_ACTIONS,
};

/* rule status flags */
enum rule_status {
    RULE_OK = 0,
    RULE_UNUSED = 1,
    RULE_DUPLICATE = 2,
};

/* sort modes for rules view */
enum sort_mode {
    SORT_TAG,
    SORT_NAME,
    SORT_STATUS,
    SORT_FILE_ORDER,
    SORT_MODE_COUNT,
};

struct ui_state_machine;
typedef struct ui_state_machine ui_state_machine_t;

struct ui_state {
    int selected;
    int scroll;

    /* loaded data */
    struct ruleset rules;
    enum rule_status *rule_status;
    char rules_path[512];
    char dotfiles_path[512];
    char appmap_path[512];

    /* sorting */
    enum sort_mode sort_mode;
    int *file_order; /* original index of each rule (for SORT_FILE_ORDER) */

    /* cached review data */
    struct missing_rules missing;
    int review_loaded;

    /* cached window data */
    struct clients clients;
    int clients_loaded;

    /* dirty tracking */
    int modified;
    int backup_created;
    char backup_path[512];

    /* undo/redo history */
    struct history_stack history;

    /* per-rule modified tracking */
    int *rule_modified;

    /* status message */
    char status[256];
};

struct ui_state_machine {
    enum view_mode current_state;
    int running;
    struct ui_state *st;
    struct notcurses *nc;
    struct ncplane *std;
};

/* true-color palette */
static const struct {
    unsigned r, g, b;
} ui_fg[] = {
    /* COL_TITLE   0 */ {100, 200, 255},
    /* COL_BORDER  1 */ { 80, 160, 210},
    /* COL_STATUS  2 */ {200, 200, 200},
    /* COL_SELECT  3 */ {  0,   0,   0},
    /* COL_NORMAL  4 */ {200, 200, 200},
    /* COL_DIM     5 */ {100, 110, 130},
    /* COL_ACCENT  6 */ {255, 200,  60},
    /* COL_WARN    7 */ {255, 180,  40},
    /* COL_ERROR   8 */ {255,  80,  80},
};

static const struct {
    unsigned r, g, b;
    int use; /* 0 = default bg, 1 = use rgb */
} ui_bg[] = {
    /* COL_TITLE   0 */ {0, 0, 0, 0},
    /* COL_BORDER  1 */ {0, 0, 0, 0},
    /* COL_STATUS  2 */ {30, 50, 90, 1},
    /* COL_SELECT  3 */ {80, 180, 230, 1},
    /* COL_NORMAL  4 */ {0, 0, 0, 0},
    /* COL_DIM     5 */ {0, 0, 0, 0},
    /* COL_ACCENT  6 */ {0, 0, 0, 0},
    /* COL_WARN    7 */ {0, 0, 0, 0},
    /* COL_ERROR   8 */ {0, 0, 0, 0},
};

enum {
    COL_TITLE = 0,
    COL_BORDER,
    COL_STATUS,
    COL_SELECT,
    COL_NORMAL,
    COL_DIM,
    COL_ACCENT,
    COL_WARN,
    COL_ERROR,
    COL_COUNT,
};

static void ui_set_color(struct ncplane *n, int col) {
    ncplane_set_fg_rgb8(n, ui_fg[col].r, ui_fg[col].g, ui_fg[col].b);
    if (ui_bg[col].use) {
        ncplane_set_bg_rgb8(n, ui_bg[col].r, ui_bg[col].g, ui_bg[col].b);
    } else {
        ncplane_set_bg_default(n);
    }
}

static void ui_reset_color(struct ncplane *n) {
    ncplane_set_fg_default(n);
    ncplane_set_bg_default(n);
    ncplane_set_styles(n, NCSTYLE_NONE);
}

/* forward declarations */
static void clean_class_name(const char *regex, char *out, size_t out_sz);
static void update_display_name(struct rule *r);
static void compute_rule_status(struct ui_state *st);
static int edit_rule_modal(ui_state_machine_t *sm, struct rule *r, int rule_index, struct history_stack *history);
static int confirm_dialog(ui_state_machine_t *sm, const char *title, const char *msg);
static void run_with_spinner(ui_state_machine_t *sm, const char *msg,
                             int cy, int cx,
                             void (*func)(void *), void *arg);
static void draw_ui(ui_state_machine_t *sm);
static void handle_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);
static void handle_rules_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);
static void handle_windows_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);
static void handle_review_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);
static void handle_actions_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);

/* --- parallel array helpers --- */

/* remove rule at index, shifting all parallel arrays down */
static void remove_rule_at(struct ui_state *st, int idx) {
    rule_free(&st->rules.rules[idx]);
    for (int i = idx; i < (int)st->rules.count - 1; i++) {
        st->rules.rules[i] = st->rules.rules[i + 1];
        if (st->rule_status) st->rule_status[i] = st->rule_status[i + 1];
        if (st->rule_modified) st->rule_modified[i] = st->rule_modified[i + 1];
        if (st->file_order) st->file_order[i] = st->file_order[i + 1];
    }
    st->rules.count--;
}

/* insert rule at index, shifting all parallel arrays up; returns 0 on success, -1 on failure */
static int insert_rule_at(struct ui_state *st, int idx, const struct rule *r) {
    struct rule *nr = realloc(st->rules.rules, (st->rules.count + 1) * sizeof(struct rule));
    if (!nr) return -1;
    st->rules.rules = nr;
    st->rules.count++;

    /* grow parallel arrays */
    enum rule_status *ns = realloc(st->rule_status, st->rules.count * sizeof(enum rule_status));
    if (ns) st->rule_status = ns;
    int *nm = realloc(st->rule_modified, st->rules.count * sizeof(int));
    if (nm) st->rule_modified = nm;
    int *nf = realloc(st->file_order, st->rules.count * sizeof(int));
    if (nf) st->file_order = nf;

    /* shift elements up from the end */
    for (int i = (int)st->rules.count - 1; i > idx; i--) {
        st->rules.rules[i] = st->rules.rules[i - 1];
        if (st->rule_status) st->rule_status[i] = st->rule_status[i - 1];
        if (st->rule_modified) st->rule_modified[i] = st->rule_modified[i - 1];
        if (st->file_order) st->file_order[i] = st->file_order[i - 1];
    }

    /* place the rule */
    st->rules.rules[idx] = rule_copy(r);
    if (st->rule_status) st->rule_status[idx] = RULE_OK;
    if (st->rule_modified) st->rule_modified[idx] = 1;
    if (st->file_order) st->file_order[idx] = idx;
    return 0;
}

/* grow all parallel arrays by one, returns new index or -1 on failure */
static int append_rule(struct ui_state *st) {
    struct rule *nr = realloc(st->rules.rules, (st->rules.count + 1) * sizeof(struct rule));
    if (!nr) return -1;
    st->rules.rules = nr;
    int idx = (int)st->rules.count;
    memset(&st->rules.rules[idx], 0, sizeof(struct rule));
    st->rules.count++;

    enum rule_status *ns = realloc(st->rule_status, st->rules.count * sizeof(enum rule_status));
    if (ns) { st->rule_status = ns; st->rule_status[idx] = RULE_OK; }
    int *nm = realloc(st->rule_modified, st->rules.count * sizeof(int));
    if (nm) { st->rule_modified = nm; st->rule_modified[idx] = 0; }
    int *nf = realloc(st->file_order, st->rules.count * sizeof(int));
    if (nf) { st->file_order = nf; st->file_order[idx] = idx; }
    return idx;
}

/* record history, remove rule, recompute status */
static void delete_rule_with_history(struct ui_state *st, int idx, const char *desc_prefix) {
    struct rule copy = rule_copy(&st->rules.rules[idx]);
    char desc[128];
    snprintf(desc, sizeof(desc), "%s rule %d", desc_prefix, idx);
    history_record(&st->history, CHANGE_DELETE, idx, &copy, NULL, desc);
    rule_free(&copy);
    remove_rule_at(st, idx);
    st->modified = 1;
    compute_rule_status(st);
}

static void set_status(struct ui_state *st, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(st->status, sizeof(st->status), fmt, args);
    va_end(args);
}

static void init_paths(struct ui_state *st) {
    const char *home = getenv("HOME");
    if (!home) home = ".";

    char *detected = hypr_find_rules_config();
    if (detected) {
        snprintf(st->rules_path, sizeof(st->rules_path), "%s", detected);
        free(detected);
    } else {
        snprintf(st->rules_path, sizeof(st->rules_path), "%s/.config/hypr/hyprland.conf", home);
    }

    snprintf(st->dotfiles_path, sizeof(st->dotfiles_path), "%s/dotfiles", home);
    snprintf(st->appmap_path, sizeof(st->appmap_path), "data/appmap.json");
}


static const char *sort_mode_label(enum sort_mode m) {
    switch (m) {
    case SORT_TAG:        return "Tag";
    case SORT_NAME:       return "Name";
    case SORT_STATUS:     return "Status";
    case SORT_FILE_ORDER: return "File";
    default:              return "?";
    }
}

/* permute rules (and parallel arrays) according to an index array.
 * idx[i] = which old position should go to new position i. */
static void permute_rules(struct ui_state *st, int *idx) {
    size_t n = st->rules.count;
    struct rule *tmp_rules = malloc(n * sizeof(struct rule));
    if (!tmp_rules) return;

    enum rule_status *tmp_status = st->rule_status ? malloc(n * sizeof(enum rule_status)) : NULL;
    int *tmp_modified = st->rule_modified ? malloc(n * sizeof(int)) : NULL;
    int *tmp_fo = st->file_order ? malloc(n * sizeof(int)) : NULL;

    for (size_t i = 0; i < n; i++) {
        tmp_rules[i] = st->rules.rules[idx[i]];
        if (tmp_status) tmp_status[i] = st->rule_status[idx[i]];
        if (tmp_modified) tmp_modified[i] = st->rule_modified[idx[i]];
        if (tmp_fo) tmp_fo[i] = st->file_order[idx[i]];
    }

    memcpy(st->rules.rules, tmp_rules, n * sizeof(struct rule));
    if (tmp_status) memcpy(st->rule_status, tmp_status, n * sizeof(enum rule_status));
    if (tmp_modified) memcpy(st->rule_modified, tmp_modified, n * sizeof(int));
    if (tmp_fo) memcpy(st->file_order, tmp_fo, n * sizeof(int));

    free(tmp_rules);
    free(tmp_status);
    free(tmp_modified);
    free(tmp_fo);
}

/* sort context for index-based comparators */
static struct ui_state *sort_ctx;

static int compare_idx_by_tag(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    const char *ta = sort_ctx->rules.rules[ia].actions.tag ? sort_ctx->rules.rules[ia].actions.tag : "";
    const char *tb = sort_ctx->rules.rules[ib].actions.tag ? sort_ctx->rules.rules[ib].actions.tag : "";
    return strcmp(ta, tb);
}

static int compare_idx_by_name(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    const struct rule *ra = &sort_ctx->rules.rules[ia];
    const struct rule *rb = &sort_ctx->rules.rules[ib];
    const char *na = ra->display_name ? ra->display_name : ra->name ? ra->name : "";
    const char *nb = rb->display_name ? rb->display_name : rb->name ? rb->name : "";
    return strcasecmp(na, nb);
}

static int compare_idx_by_status(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int sa = sort_ctx->rule_status ? sort_ctx->rule_status[ia] : 0;
    int sb = sort_ctx->rule_status ? sort_ctx->rule_status[ib] : 0;
    /* sort errors/warnings first: DUPLICATE(2) > UNUSED(1) > OK(0) */
    if (sa != sb) return sb - sa;
    /* tie-break by name */
    const struct rule *ra = &sort_ctx->rules.rules[ia];
    const struct rule *rb = &sort_ctx->rules.rules[ib];
    const char *na = ra->display_name ? ra->display_name : ra->name ? ra->name : "";
    const char *nb = rb->display_name ? rb->display_name : rb->name ? rb->name : "";
    return strcasecmp(na, nb);
}

static int compare_idx_by_file_order(const void *a, const void *b) {
    int ia = *(const int *)a, ib = *(const int *)b;
    int fa = sort_ctx->file_order ? sort_ctx->file_order[ia] : ia;
    int fb = sort_ctx->file_order ? sort_ctx->file_order[ib] : ib;
    return fa - fb;
}

static void apply_sort(struct ui_state *st) {
    size_t n = st->rules.count;
    if (n < 2) return;

    int *idx = malloc(n * sizeof(int));
    if (!idx) return;
    for (size_t i = 0; i < n; i++) idx[i] = (int)i;

    sort_ctx = st;
    switch (st->sort_mode) {
    case SORT_TAG:        qsort(idx, n, sizeof(int), compare_idx_by_tag); break;
    case SORT_NAME:       qsort(idx, n, sizeof(int), compare_idx_by_name); break;
    case SORT_STATUS:     qsort(idx, n, sizeof(int), compare_idx_by_status); break;
    case SORT_FILE_ORDER: qsort(idx, n, sizeof(int), compare_idx_by_file_order); break;
    default: break;
    }
    sort_ctx = NULL;

    permute_rules(st, idx);
    free(idx);
}

static int rules_duplicate(const struct rule *a, const struct rule *b) {
    /* duplicate if they share the same effective name */
    const char *na = a->display_name ? a->display_name : a->name;
    const char *nb = b->display_name ? b->display_name : b->name;
    if (!na || !nb) return 0;
    return strcasecmp(na, nb) == 0;
}

static void load_clients(struct ui_state *st) {
    if (st->clients_loaded) return;
    clients_free(&st->clients);
    memset(&st->clients, 0, sizeof(st->clients));
    hyprctl_clients(&st->clients);
    st->clients_loaded = 1;
}

static void compute_rule_status(struct ui_state *st) {
    free(st->rule_status);
    st->rule_status = calloc(st->rules.count, sizeof(enum rule_status));
    if (!st->rule_status) return;

    load_clients(st);

    /* ensure display_name is populated before duplicate check */
    for (size_t i = 0; i < st->rules.count; i++) {
        update_display_name(&st->rules.rules[i]);
    }

    for (size_t i = 0; i < st->rules.count; i++) {
        struct rule *r = &st->rules.rules[i];

        for (size_t j = 0; j < st->rules.count; j++) {
            if (i != j && rules_duplicate(r, &st->rules.rules[j])) {
                st->rule_status[i] = RULE_DUPLICATE;
                break;
            }
        }

        if (st->rule_status[i] == RULE_OK && st->clients.count > 0) {
            int matched = 0;
            for (size_t c = 0; c < st->clients.count; c++) {
                if (rule_matches_client(r, &st->clients.items[c])) {
                    matched = 1;
                    break;
                }
            }
            if (!matched) {
                st->rule_status[i] = RULE_UNUSED;
            }
        }
    }
}

static void load_review_data(struct ui_state *st) {
    missing_rules_free(&st->missing);
    st->review_loaded = 0;

    char *path = expand_home(st->rules_path);
    char *appmap_path = expand_home(st->appmap_path);
    find_missing_rules(path ? path : st->rules_path,
                       appmap_path ? appmap_path : st->appmap_path,
                       st->dotfiles_path, &st->missing);
    free(appmap_path);
    free(path);

    /* ensure rule_status is computed */
    if (!st->rule_status && st->rules.count > 0) {
        compute_rule_status(st);
    }

    st->review_loaded = 1;
}

static void load_rules(struct ui_state *st) {
    ruleset_free(&st->rules);
    free(st->rule_status);
    st->rule_status = NULL;
    free(st->rule_modified);
    st->rule_modified = NULL;
    free(st->file_order);
    st->file_order = NULL;
    st->review_loaded = 0;
    clients_free(&st->clients);
    st->clients_loaded = 0;

    char *path = expand_home(st->rules_path);
    if (ruleset_load(path ? path : st->rules_path, &st->rules) == 0) {
        /* record original file order before sorting */
        st->file_order = malloc(st->rules.count * sizeof(int));
        if (st->file_order) {
            for (size_t i = 0; i < st->rules.count; i++)
                st->file_order[i] = (int)i;
        }
        compute_rule_status(st);
        apply_sort(st);
        st->rule_modified = calloc(st->rules.count, sizeof(int));
        set_status(st, "Loaded %zu rules from %s", st->rules.count, st->rules_path);
    } else {
        set_status(st, "Failed to load rules from %s", st->rules_path);
    }
    free(path);
}

/* --- drawing helpers --- */

static void ui_fill_row(struct ncplane *n, int y, int x, int w, char ch) {
    for (int i = 0; i < w; i++) {
        ncplane_putchar_yx(n, y, x + i, ch);
    }
}

static void draw_box(struct ncplane *n, int y, int x, int h, int w, const char *title) {
    ui_set_color(n, COL_BORDER);
    ncplane_cursor_move_yx(n, y, x);
    uint64_t channels = NCCHANNELS_INITIALIZER(
        ui_fg[COL_BORDER].r, ui_fg[COL_BORDER].g, ui_fg[COL_BORDER].b,
        0, 0, 0);
    ncplane_rounded_box_sized(n, 0, channels, (unsigned)h, (unsigned)w, 0);
    if (title) {
        ncplane_printf_yx(n, y, x + 2, " %s ", title);
    }
    ui_reset_color(n);
}

struct popup_rect { int y, x, h, w; };

/* Compute centered popup dimensions, clamped to screen with margins */
static struct popup_rect popup_center(struct ncplane *n, int want_h, int want_w,
                                       int margin_h, int margin_w) {
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);
    struct popup_rect r;
    r.h = want_h;
    r.w = want_w;
    if (margin_h > 0 && r.h > (int)scr_h - margin_h) r.h = (int)scr_h - margin_h;
    if (margin_w > 0 && r.w > (int)scr_w - margin_w) r.w = (int)scr_w - margin_w;
    r.y = ((int)scr_h - r.h) / 2;
    r.x = ((int)scr_w - r.w) / 2;
    return r;
}

/* Fill popup area and draw box border with title */
static void popup_draw(struct ncplane *n, struct popup_rect r, const char *title) {
    for (int i = 0; i < r.h; i++)
        ui_fill_row(n, r.y + i, r.x, r.w, ' ');
    draw_box(n, r.y, r.x, r.h, r.w, title);
}

static void draw_scrollbar(struct ncplane *n, int top_y, int x,
                            int visible, int total, int scroll) {
    int thumb = (visible * visible) / total;
    if (thumb < 1) thumb = 1;
    int thumb_pos = (visible * scroll) / total;
    for (int i = 0; i < visible; i++) {
        if (i >= thumb_pos && i < thumb_pos + thumb) {
            ui_set_color(n, COL_SELECT);
            ncplane_putstr_yx(n, top_y + i, x, "\u2588");
        } else {
            ui_set_color(n, COL_DIM);
            ncplane_putstr_yx(n, top_y + i, x, "\u2502");
        }
        ui_reset_color(n);
    }
}

static void draw_header(struct ncplane *n, unsigned width, const char *title) {
    ui_set_color(n, COL_TITLE);
    ncplane_on_styles(n, NCSTYLE_BOLD);
    ui_fill_row(n, 0, 0, (int)width, ' ');
    int title_len = (int)strlen(title);
    ncplane_printf_yx(n, 0, ((int)width - title_len) / 2, "%s", title);
    ncplane_off_styles(n, NCSTYLE_BOLD);
    ui_reset_color(n);
}

static void draw_statusbar(struct ncplane *n, int y, unsigned width, const char *left, const char *right) {
    ui_set_color(n, COL_STATUS);
    ui_fill_row(n, y, 0, (int)width, ' ');
    if (left) ncplane_printf_yx(n, y, 1, "%s", left);
    if (right) ncplane_printf_yx(n, y, (int)width - (int)strlen(right) - 1, "%s", right);
    ui_reset_color(n);
}

static int tab_x_start[4], tab_x_end[4]; /* populated by draw_tabs */

static void draw_tabs(struct ncplane *n, int y, unsigned width, enum view_mode mode) {
    (void)width;
    const char *tabs[] = {"[1] Rules", "[2] Windows", "[3] Review", "[4] Actions"};
    int x = 2;
    for (int i = 0; i < 4; i++) {
        tab_x_start[i] = x;
        tab_x_end[i] = x + (int)strlen(tabs[i]) + 2; /* " label " = len+2 */
        if (i == (int)mode) {
            ncplane_on_styles(n, NCSTYLE_BOLD);
            ui_set_color(n, COL_SELECT);
        } else {
            ui_set_color(n, COL_DIM);
        }
        ncplane_printf_yx(n, y, x, " %s ", tabs[i]);
        if (i == (int)mode) {
            ncplane_off_styles(n, NCSTYLE_BOLD);
        }
        ui_reset_color(n);
        x += (int)strlen(tabs[i]) + 3;
    }
}

/* extract readable class name from regex */
static void clean_class_name(const char *regex, char *out, size_t out_sz) {
    if (!regex || !out || out_sz == 0) {
        if (out && out_sz > 0) out[0] = '\0';
        return;
    }

    const char *p = regex;
    size_t o = 0;

    if (*p == '^') p++;
    if (*p == '(') p++;
    if (*p == '[' && p[1] && p[2] == ']') {
        out[o++] = (p[1] >= 'a' && p[1] <= 'z') ? p[1] - 32 : p[1];
        p += 3;
    }

    while (*p && o < out_sz - 1) {
        if (*p == '$' || *p == ')' || *p == '|') break;
        if (*p == '[') {
            if (p[1] && p[1] != ']') out[o++] = p[1];
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
            continue;
        }
        if (*p == '+' || *p == '*' || *p == '?') { p++; continue; }
        if (*p == '.' && (p[1] == '+' || p[1] == '*' || p[1] == '?')) { p += 2; continue; }
        if (*p == '.' && (p[1] == '$' || p[1] == ')' || p[1] == '\0')) { p++; continue; }
        if (*p == '\\' && p[1] == 'd') { p += 2; continue; }
        if (*p == '\\' && p[1]) {
            p++;
            out[o++] = *p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';

    /* strip leading dots */
    {
        size_t start = 0;
        while (out[start] == '.') start++;
        if (start > 0 && out[start]) {
            memmove(out, out + start, o - start + 1);
            o -= start;
        } else if (start > 0) {
            out[0] = '\0';
            o = 0;
        }
    }
    /* strip trailing dots */
    while (o > 0 && out[o - 1] == '.') {
        out[--o] = '\0';
    }

    if (out[0] == '\0' && regex[0]) {
        p = regex;
        o = 0;
        while (*p && o < out_sz - 1) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') {
                out[o++] = *p;
            } else if (o > 0) {
                break;
            }
            p++;
        }
        out[o] = '\0';
    }

    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] -= 32;
    }
}

static void update_display_name(struct rule *r) {
    char buf[64] = "";

    clean_class_name(r->match.class_re, buf, sizeof(buf));

    if (!buf[0]) {
        clean_class_name(r->match.title_re, buf, sizeof(buf));
    }

    if (!buf[0] && r->name) {
        snprintf(buf, sizeof(buf), "%s", r->name);
    }

    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "(unnamed)");
    }

    free(r->display_name);
    r->display_name = strdup(buf);
}

static const char *clean_tag(const char *tag) {
    if (!tag) return "-";
    if (tag[0] == '+') return tag + 1;
    return tag;
}

/* --- view drawing --- */

static void draw_rules_view(struct ncplane *n, struct ui_state *st, int y, int h, int w) {
    char title[64];
    snprintf(title, sizeof(title), "Window Rules [%s]", sort_mode_label(st->sort_mode));
    draw_box(n, y, 0, h, w, title);

    if (st->rules.count == 0) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, (w - 20) / 2, "No rules loaded");
        ui_reset_color(n);
        return;
    }

    int visible = h - 3;
    int max_scroll = (int)st->rules.count > visible ? (int)st->rules.count - visible : 0;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0) st->scroll = 0;

    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + visible) st->scroll = st->selected - visible + 1;

    /* proportional column layout: name(30%) tag(20%) ws(10%) status(12%) opts(rest) */
    int usable = w - 4; /* 2 padding each side */
    int col_name  = 2;
    int col_name_w = usable * 30 / 100;
    if (col_name_w < 10) col_name_w = 10;
    if (col_name_w > 24) col_name_w = 24;
    int col_tag   = col_name + col_name_w + 1;
    int col_tag_w = usable * 20 / 100;
    if (col_tag_w < 6) col_tag_w = 6;
    if (col_tag_w > 16) col_tag_w = 16;
    int col_ws    = col_tag + col_tag_w + 1;
    int col_ws_w  = usable * 15 / 100;
    if (col_ws_w < 6) col_ws_w = 6;
    if (col_ws_w > 16) col_ws_w = 16;
    int col_stat  = col_ws + col_ws_w + 1;
    int col_stat_w = 8;
    int col_opts  = col_stat + col_stat_w + 1;
    int col_opts_w = w - col_opts - 2;
    if (col_opts_w < 4) col_opts_w = 4;

    ui_set_color(n, COL_DIM);
    ncplane_printf_yx(n, y + 1, col_name, "%-*s", col_name_w, "Application");
    ncplane_printf_yx(n, y + 1, col_tag, "%-*s", col_tag_w, "Tag");
    ncplane_printf_yx(n, y + 1, col_ws, "%-*s", col_ws_w, "WS");
    ncplane_printf_yx(n, y + 1, col_stat, "%-*s", col_stat_w, "Status");
    ncplane_printf_yx(n, y + 1, col_opts, "%s", "Options");
    ui_reset_color(n);

    const char *last_tag = NULL;

    for (int i = 0; i < visible && (st->scroll + i) < (int)st->rules.count; i++) {
        int idx = st->scroll + i;
        struct rule *r = &st->rules.rules[idx];
        int row = y + 2 + i;

        enum rule_status status = st->rule_status ? st->rule_status[idx] : RULE_OK;

        const char *display = r->display_name ? r->display_name : "(unnamed)";
        const char *tag = clean_tag(r->actions.tag);
        const char *ws = r->actions.workspace ? r->actions.workspace : "-";

        /* build options string */
        char opts[32] = "";
        int opos = 0;
        if (r->actions.float_set && r->actions.float_val)
            opos += snprintf(opts + opos, sizeof(opts) - (size_t)opos, "F ");
        if (r->actions.center_set && r->actions.center_val)
            opos += snprintf(opts + opos, sizeof(opts) - (size_t)opos, "C ");
        if (r->actions.size)
            opos += snprintf(opts + opos, sizeof(opts) - (size_t)opos, "S ");
        if (r->actions.opacity)
            opos += snprintf(opts + opos, sizeof(opts) - (size_t)opos, "O ");
        if (r->extras_count > 0) {
            snprintf(opts + opos, sizeof(opts) - (size_t)opos, "+%zu", r->extras_count);
        }
        if (opts[0] == '\0') strcpy(opts, "-");

        int show_tag = 1;
        if (last_tag && r->actions.tag && strcmp(last_tag, r->actions.tag) == 0) {
            show_tag = 0;
        }
        last_tag = r->actions.tag;

        if (idx == st->selected) {
            ui_set_color(n, COL_SELECT);
            ui_fill_row(n, row, 1, w - 2, ' ');
        }

        /* modified indicator */
        int is_mod = (st->rule_modified && st->rule_modified[idx]);
        if (is_mod) {
            if (idx != st->selected) ui_set_color(n, COL_WARN);
            ncplane_putchar_yx(n, row, 1, '*');
            if (idx == st->selected) ui_set_color(n, COL_SELECT);
            else ui_reset_color(n);
        }

        ncplane_printf_yx(n, row, col_name, "%-*.*s", col_name_w, col_name_w, display);

        if (show_tag && tag[0] != '-') {
            ncplane_on_styles(n, NCSTYLE_BOLD);
            if (idx != st->selected) ui_set_color(n, COL_ACCENT);
            ncplane_printf_yx(n, row, col_tag, "%-*.*s", col_tag_w, col_tag_w, tag);
            ncplane_off_styles(n, NCSTYLE_BOLD);
            if (idx == st->selected) ui_set_color(n, COL_SELECT);
            else ui_reset_color(n);
        } else {
            ncplane_printf_yx(n, row, col_tag, "%-*.*s", col_tag_w, col_tag_w, show_tag ? tag : "");
        }

        if (idx == st->selected) ui_set_color(n, COL_SELECT);
        ncplane_printf_yx(n, row, col_ws, "%-*.*s", col_ws_w, col_ws_w, ws);

        const char *status_str;
        int status_color;
        switch (status) {
        case RULE_UNUSED:
            status_str = "unused";
            status_color = COL_WARN;
            break;
        case RULE_DUPLICATE:
            status_str = "dup";
            status_color = COL_ERROR;
            break;
        default:
            status_str = "ok";
            status_color = COL_DIM;
            break;
        }
        if (idx != st->selected) ui_set_color(n, status_color);
        ncplane_printf_yx(n, row, col_stat, "%-*s", col_stat_w, status_str);

        if (idx != st->selected) ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, col_opts, "%.*s", col_opts_w, opts);

        ui_reset_color(n);
    }

    /* scrollbar */
    if (max_scroll > 0)
        draw_scrollbar(n, y + 2, w - 1, visible, (int)st->rules.count, st->scroll);
}

static void draw_rule_detail(struct ncplane *n, struct ui_state *st, int y, int x, int h, int w) {
    draw_box(n, y, x, h, w, "Rule Details");

    if (st->selected < 0 || st->selected >= (int)st->rules.count) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, x + (w - 18) / 2, "No rule selected");
        ui_reset_color(n);
        return;
    }

    struct rule *r = &st->rules.rules[st->selected];
    int row = y + 2;
    int col = x + 3;

    const char *display = r->display_name ? r->display_name : "(unnamed)";

    ncplane_on_styles(n, NCSTYLE_BOLD);
    ui_set_color(n, COL_ACCENT);
    ncplane_printf_yx(n, row++, col, "%s", display);
    ncplane_off_styles(n, NCSTYLE_BOLD);
    ui_reset_color(n);

    row++;

    ui_set_color(n, COL_DIM);
    ncplane_printf_yx(n, row++, col, "Matching");
    ui_reset_color(n);

    if (r->match.class_re)
        ncplane_printf_yx(n, row++, col + 2, "Class:  %.*s", w - 12, r->match.class_re);
    if (r->match.title_re)
        ncplane_printf_yx(n, row++, col + 2, "Title:  %.*s", w - 12, r->match.title_re);

    row++;

    ui_set_color(n, COL_DIM);
    ncplane_printf_yx(n, row++, col, "Actions");
    ui_reset_color(n);

    if (r->actions.tag)
        ncplane_printf_yx(n, row++, col + 2, "Tag:       %s", clean_tag(r->actions.tag));
    if (r->actions.workspace)
        ncplane_printf_yx(n, row++, col + 2, "Workspace: %s", r->actions.workspace);
    if (r->actions.float_set)
        ncplane_printf_yx(n, row++, col + 2, "Float:     %s", r->actions.float_val ? "Yes" : "No");
    if (r->actions.center_set)
        ncplane_printf_yx(n, row++, col + 2, "Center:    %s", r->actions.center_val ? "Yes" : "No");
    if (r->actions.size)
        ncplane_printf_yx(n, row++, col + 2, "Size:      %s", r->actions.size);
    if (r->actions.move)
        ncplane_printf_yx(n, row++, col + 2, "Position:  %s", r->actions.move);
    if (r->actions.opacity)
        ncplane_printf_yx(n, row++, col + 2, "Opacity:   %s", r->actions.opacity);

    if (r->extras_count > 0) {
        row++;
        ui_set_color(n, COL_ACCENT);
        ncplane_printf_yx(n, row++, col, "Other (%zu)", r->extras_count);
        ui_reset_color(n);

        for (size_t i = 0; i < r->extras_count && row < y + h - 3; i++) {
            ncplane_printf_yx(n, row++, col + 2, "%-10.10s %.*s",
                     r->extras[i].key, w - 16, r->extras[i].value);
        }
    }

    ui_set_color(n, COL_DIM);
    ncplane_printf_yx(n, y + h - 2, col, "Press Enter to edit");
    ui_reset_color(n);
}

static void draw_windows_view(struct ncplane *n, struct ui_state *st, int y, int h, int w) {
    draw_box(n, y, 0, h, w, "Active Windows");

    load_clients(st);

    if (st->clients.count == 0) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, (w - 24) / 2, "No windows found");
        ui_reset_color(n);
        return;
    }

    /* clamp selected */
    if (st->selected >= (int)st->clients.count) st->selected = (int)st->clients.count - 1;
    if (st->selected < 0) st->selected = 0;

    int visible = h - 3; /* box top + header + box bottom */
    int max_scroll = (int)st->clients.count > visible ? (int)st->clients.count - visible : 0;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0) st->scroll = 0;
    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + visible) st->scroll = st->selected - visible + 1;

    /* column layout */
    int usable = w - 4;
    int col_class = 2;
    int col_class_w = usable * 30 / 100;
    if (col_class_w < 12) col_class_w = 12;
    if (col_class_w > 28) col_class_w = 28;
    int col_title = col_class + col_class_w + 1;
    int col_title_w = usable * 40 / 100;
    if (col_title_w < 10) col_title_w = 10;
    int col_ws = col_title + col_title_w + 1;
    int col_ws_w = 6;
    int col_match = col_ws + col_ws_w + 1;
    int col_match_w = w - col_match - 2;
    if (col_match_w < 4) col_match_w = 4;

    /* header */
    ui_set_color(n, COL_DIM);
    ncplane_printf_yx(n, y + 1, col_class, "%-*s", col_class_w, "Class");
    ncplane_printf_yx(n, y + 1, col_title, "%-*s", col_title_w, "Title");
    ncplane_printf_yx(n, y + 1, col_ws, "%-*s", col_ws_w, "WS");
    ncplane_printf_yx(n, y + 1, col_match, "%s", "Rules");
    ui_reset_color(n);

    for (int i = 0; i < visible && (st->scroll + i) < (int)st->clients.count; i++) {
        int idx = st->scroll + i;
        struct client *c = &st->clients.items[idx];
        int row = y + 2 + i;

        /* count matching rules */
        int match_count = 0;
        for (size_t j = 0; j < st->rules.count; j++) {
            if (rule_matches_client(&st->rules.rules[j], c))
                match_count++;
        }

        if (idx == st->selected) {
            ui_set_color(n, COL_SELECT);
            ui_fill_row(n, row, 1, w - 2, ' ');
        }

        const char *cls = c->class_name ? c->class_name : "<unknown>";
        ncplane_printf_yx(n, row, col_class, "%-*.*s", col_class_w, col_class_w, cls);

        const char *title = c->title ? c->title : "";
        ncplane_printf_yx(n, row, col_title, "%-*.*s", col_title_w, col_title_w, title);

        if (c->workspace_id >= 0)
            ncplane_printf_yx(n, row, col_ws, "%-*d", col_ws_w, c->workspace_id);
        else if (c->workspace_name)
            ncplane_printf_yx(n, row, col_ws, "%-*.*s", col_ws_w, col_ws_w, c->workspace_name);
        else
            ncplane_printf_yx(n, row, col_ws, "%-*s", col_ws_w, "-");

        if (match_count > 0) {
            if (idx != st->selected) ui_set_color(n, COL_ACCENT);
            ncplane_printf_yx(n, row, col_match, "%d match%s", match_count, match_count == 1 ? "" : "es");
        } else {
            if (idx != st->selected) ui_set_color(n, COL_DIM);
            ncplane_printf_yx(n, row, col_match, "none");
        }

        ui_reset_color(n);
    }

    /* scrollbar */
    if (max_scroll > 0)
        draw_scrollbar(n, y + 2, w - 1, visible, (int)st->clients.count, st->scroll);
}

/* returns rule index to jump to, or -1 if closed without selection */
static int window_detail_popup(ui_state_machine_t *sm, struct client *c, struct ruleset *rs) {
    struct ncplane *n = sm->std;

    /* collect matching rules */
    int matches[256];
    int match_count = 0;
    for (size_t j = 0; j < rs->count && match_count < 256; j++) {
        if (rule_matches_client(&rs->rules[j], c))
            matches[match_count++] = (int)j;
    }

    int sel = 0; /* selected match index */

    /* compute content lines */
    int content_lines = 5 + match_count; /* class, title, ws, initial_class, blank, matches header, match lines */
    if (match_count == 0) content_lines = 6; /* ...+ "(none)" */
    else content_lines = 6 + match_count;

    struct popup_rect p = popup_center(n, content_lines + 3, 60, 2, 4);
    int content_w = p.w - 4;

    while (1) {
        popup_draw(n, p, c->class_name ? c->class_name : "Window Details");

        int r = p.y + 2;
        int lx = p.x + 2;
        int vx = p.x + 18;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, r, lx, "Class:");
        ui_set_color(n, COL_NORMAL);
        ncplane_printf_yx(n, r, vx, "%.*s", content_w - 16, c->class_name ? c->class_name : "-");
        r++;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, r, lx, "Title:");
        ui_set_color(n, COL_NORMAL);
        ncplane_printf_yx(n, r, vx, "%.*s", content_w - 16, c->title ? c->title : "-");
        r++;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, r, lx, "Init class:");
        ui_set_color(n, COL_NORMAL);
        ncplane_printf_yx(n, r, vx, "%.*s", content_w - 16, c->initial_class ? c->initial_class : "-");
        r++;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, r, lx, "Workspace:");
        ui_set_color(n, COL_NORMAL);
        if (c->workspace_id >= 0)
            ncplane_printf_yx(n, r, vx, "%d%s%s%s", c->workspace_id,
                c->workspace_name ? " (" : "", c->workspace_name ? c->workspace_name : "", c->workspace_name ? ")" : "");
        else if (c->workspace_name)
            ncplane_printf_yx(n, r, vx, "%s", c->workspace_name);
        else
            ncplane_printf_yx(n, r, vx, "-");
        r++;

        r++; /* blank line */

        ncplane_on_styles(n, NCSTYLE_BOLD);
        ui_set_color(n, COL_ACCENT);
        ncplane_printf_yx(n, r, lx, "Matching Rules (%d):", match_count);
        ncplane_off_styles(n, NCSTYLE_BOLD);
        ui_reset_color(n);
        r++;

        if (match_count == 0 && r < p.y + p.h - 1) {
            ui_set_color(n, COL_DIM);
            ncplane_printf_yx(n, r, lx + 2, "(none)");
            ui_reset_color(n);
        } else {
            for (int m = 0; m < match_count && r < p.y + p.h - 1; m++) {
                int ri = matches[m];
                const char *rname = rs->rules[ri].display_name
                    ? rs->rules[ri].display_name
                    : rs->rules[ri].name;
                if (m == sel) {
                    ui_set_color(n, COL_SELECT);
                    ui_fill_row(n, r, lx + 1, content_w - 1, ' ');
                } else {
                    ui_set_color(n, COL_NORMAL);
                }
                ncplane_printf_yx(n, r, lx + 2, "[%d] %.*s", ri, content_w - 8, rname ? rname : "<unnamed>");
                ui_reset_color(n);
                r++;
            }
        }

        ui_set_color(n, COL_DIM);
        if (match_count > 0)
            ncplane_printf_yx(n, p.y + p.h - 1, p.x + 3, " Enter:Go to rule  Esc:Close ");
        else
            ncplane_printf_yx(n, p.y + p.h - 1, p.x + 3, " Esc:Close ");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (id == 27 || id == 'q') /* Esc or q to close */
            return -1;

        if (match_count > 0) {
            if (id == NCKEY_UP && sel > 0) sel--;
            else if (id == NCKEY_DOWN && sel < match_count - 1) sel++;
            else if (id == NCKEY_HOME) sel = 0;
            else if (id == NCKEY_END) sel = match_count - 1;
            else if (id == NCKEY_ENTER || id == '\n')
                return matches[sel];
        } else {
            /* no matches: any key closes */
            return -1;
        }
    }
}

/* count unused rules (RULE_UNUSED status) */
static int review_count_unused(struct ui_state *st) {
    int count = 0;
    if (!st->rule_status) return 0;
    for (size_t i = 0; i < st->rules.count; i++) {
        if (st->rule_status[i] == RULE_UNUSED) count++;
    }
    return count;
}

/* get the ruleset index of the nth unused rule */
static int review_unused_index(struct ui_state *st, int nth) {
    int count = 0;
    for (size_t i = 0; i < st->rules.count; i++) {
        if (st->rule_status[i] == RULE_UNUSED) {
            if (count == nth) return (int)i;
            count++;
        }
    }
    return -1;
}

/* total selectable items in review view */
static int review_total_items(struct ui_state *st) {
    return review_count_unused(st) + (int)st->missing.count;
}

static void draw_review_view(ui_state_machine_t *sm, struct ui_state *st, int y, int h, int w) {
    struct ncplane *n = sm->std;
    draw_box(n, y, 0, h, w, "Rules Review");

    if (!st->review_loaded) {
        run_with_spinner(sm, "Loading...", y + h / 2, (w - 14) / 2,
                         (void (*)(void *))load_review_data, st);
    }

    int unused_count = review_count_unused(st);
    int missing_count = (int)st->missing.count;
    int total = unused_count + missing_count;

    if (total == 0) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, (w - 30) / 2, "All rules active, none missing");
        ui_reset_color(n);
        return;
    }

    /* clamp selected */
    if (st->selected >= total) st->selected = total - 1;
    if (st->selected < 0) st->selected = 0;

    /* summary line at top */
    int summary_y = y + 1;
    {
        int active_count = (int)st->rules.count - unused_count;
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, summary_y, 2,
            "Total: %zu  Active: %d  ", st->rules.count, active_count);
        if (unused_count > 0) ui_set_color(n, COL_WARN);
        ncplane_printf(n, "Unused: %d  ", unused_count);
        if (missing_count > 0) ui_set_color(n, COL_ERROR);
        else ui_set_color(n, COL_DIM);
        ncplane_printf(n, "Missing: %d", missing_count);
    }
    ui_reset_color(n);

    /* table area starts after summary + blank line */
    int table_y = summary_y + 2;
    int visible = y + h - 1 - table_y; /* rows available for table content */
    if (visible < 1) return;

    /* column layout */
    int usable = w - 4;
    int col_name = 2;
    int col_name_w = usable * 35 / 100;
    if (col_name_w < 12) col_name_w = 12;
    if (col_name_w > 30) col_name_w = 30;
    int col_class = col_name + col_name_w + 1;
    int col_class_w = usable * 35 / 100;
    if (col_class_w < 10) col_class_w = 10;
    int col_info = col_class + col_class_w + 1;
    int col_info_w = w - col_info - 2;
    if (col_info_w < 6) col_info_w = 6;

    /* header */
    ui_set_color(n, COL_DIM);
    ncplane_printf_yx(n, table_y - 1, col_name, "%-*s", col_name_w, "Name");
    ncplane_printf_yx(n, table_y - 1, col_class, "%-*s", col_class_w, "Class");
    ncplane_printf_yx(n, table_y - 1, col_info, "%s", "Info");
    ui_reset_color(n);

    /* draw items */
    int header_rows = (unused_count > 0 ? 1 : 0) + (missing_count > 0 ? 1 : 0);
    int display_total = total + header_rows;
    int max_scroll_d = display_total > visible ? display_total - visible : 0;
    if (st->scroll > max_scroll_d) st->scroll = max_scroll_d;

    /* map selected item index to display row (accounting for section headers) */
    int sel_display = st->selected;
    if (unused_count > 0) sel_display++; /* unused header before item 0 */
    if (st->selected >= unused_count && missing_count > 0) sel_display++; /* missing header */

    if (sel_display < st->scroll) st->scroll = sel_display;
    if (sel_display >= st->scroll + visible) st->scroll = sel_display - visible + 1;

    for (int vi = 0; vi < visible; vi++) {
        int di = st->scroll + vi; /* display index (includes headers) */
        int row = table_y + vi;

        /* determine what this display row shows */
        int unused_hdr = (unused_count > 0) ? 0 : -1; /* display index of unused header */
        int missing_hdr = -1;
        int item_offset = 0; /* running offset to subtract from di to get item index */

        /* unused section header is at display index 0 if unused_count > 0 */
        if (unused_hdr >= 0 && di == unused_hdr) {
            ui_set_color(n, COL_WARN);
            int cx = 2;
            ncplane_putstr_yx(n, row, cx, "\u2500\u2500 "); cx += 3;
            char label[64];
            snprintf(label, sizeof(label), "Unused Rules (%d) ", unused_count);
            ncplane_putstr_yx(n, row, cx, label); cx += (int)strlen(label);
            for (; cx < w - 2; cx++)
                ncplane_putstr_yx(n, row, cx, "\u2500");
            ui_reset_color(n);
            continue;
        }
        if (unused_count > 0) item_offset = 1;

        /* missing section header position */
        missing_hdr = unused_count + item_offset;
        if (missing_count > 0 && di == missing_hdr) {
            ui_set_color(n, COL_ERROR);
            int cx = 2;
            ncplane_putstr_yx(n, row, cx, "\u2500\u2500 "); cx += 3;
            char label[64];
            snprintf(label, sizeof(label), "Missing Rules (%d) ", missing_count);
            ncplane_putstr_yx(n, row, cx, label); cx += (int)strlen(label);
            for (; cx < w - 2; cx++)
                ncplane_putstr_yx(n, row, cx, "\u2500");
            ui_reset_color(n);
            continue;
        }
        if (missing_count > 0 && di > missing_hdr) item_offset++;

        int idx = di - item_offset;
        if (idx < 0 || idx >= total) continue;

        if (idx < unused_count) {
            /* unused rule */
            int ri = review_unused_index(st, idx);
            if (ri < 0) continue;
            struct rule *r = &st->rules.rules[ri];

            if (idx == st->selected) {
                ui_set_color(n, COL_SELECT);
                ui_fill_row(n, row, 1, w - 2, ' ');
            }

            const char *name = r->display_name ? r->display_name : r->name;
            ncplane_printf_yx(n, row, col_name, "%-*.*s", col_name_w, col_name_w,
                              name ? name : "<unnamed>");

            const char *cls = r->match.class_re ? r->match.class_re : "-";
            ncplane_printf_yx(n, row, col_class, "%-*.*s", col_class_w, col_class_w, cls);

            if (idx == st->selected) {
                /* keep select color */
            } else {
                ui_set_color(n, COL_WARN);
            }
            ncplane_printf_yx(n, row, col_info, "unused");
            ui_reset_color(n);
        } else {
            /* missing rule */
            int mi = idx - unused_count;
            struct missing_rule *mr = &st->missing.items[mi];

            if (idx == st->selected) {
                ui_set_color(n, COL_SELECT);
                ui_fill_row(n, row, 1, w - 2, ' ');
            }

            ncplane_printf_yx(n, row, col_name, "%-*.*s", col_name_w, col_name_w,
                              mr->app_name ? mr->app_name : "?");

            ncplane_printf_yx(n, row, col_class, "%-*.*s", col_class_w, col_class_w,
                              mr->class_pattern ? mr->class_pattern : "?");

            if (idx == st->selected) {
                /* keep select color */
            } else {
                ui_set_color(n, COL_ERROR);
            }
            ncplane_printf_yx(n, row, col_info, "missing [%s]",
                              mr->source ? mr->source : "?");
            ui_reset_color(n);
        }
    }

    /* scrollbar */
    if (max_scroll_d > 0)
        draw_scrollbar(n, table_y, w - 1, visible, display_total, st->scroll);
}

/* returns rule index to jump to, -1 if closed, -2 if rule was deleted */
static int review_unused_popup(ui_state_machine_t *sm, int rule_idx) {
    struct ui_state *st = sm->st;
    struct ncplane *n = sm->std;

    if (rule_idx < 0 || rule_idx >= (int)st->rules.count)
        return -1;

    struct rule *r = &st->rules.rules[rule_idx];

    struct popup_rect p = popup_center(n, 16, 60, 2, 4);
    int content_w = p.w - 4;

    while (1) {
        const char *display = r->display_name ? r->display_name : r->name;
        popup_draw(n, p, display ? display : "Unused Rule");

        int row = p.y + 2;
        int lx = p.x + 2;

        ncplane_on_styles(n, NCSTYLE_BOLD);
        ui_set_color(n, COL_WARN);
        ncplane_printf_yx(n, row, lx, "UNUSED");
        ncplane_off_styles(n, NCSTYLE_BOLD);
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx + 9, "(no matching windows)");
        ui_reset_color(n);
        row += 2;

        /* matching */
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx, "Matching");
        ui_reset_color(n);
        row++;
        if (r->match.class_re)
            ncplane_printf_yx(n, row++, lx + 2, "Class:  %.*s", content_w - 12, r->match.class_re);
        if (r->match.title_re)
            ncplane_printf_yx(n, row++, lx + 2, "Title:  %.*s", content_w - 12, r->match.title_re);
        row++;

        /* actions */
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx, "Actions");
        ui_reset_color(n);
        row++;
        if (r->actions.tag)
            ncplane_printf_yx(n, row++, lx + 2, "Tag:       %.*s", content_w - 16, clean_tag(r->actions.tag));
        if (r->actions.workspace)
            ncplane_printf_yx(n, row++, lx + 2, "Workspace: %s", r->actions.workspace);
        if (r->actions.float_set)
            ncplane_printf_yx(n, row++, lx + 2, "Float:     %s", r->actions.float_val ? "Yes" : "No");

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + p.h - 1, p.x + 3,
                          " Enter:Go to rule  d:Delete  Esc:Close ");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (id == 27 || id == 'q')
            return -1;
        if (id == NCKEY_ENTER || id == '\n')
            return rule_idx;
        if (id == 'd' || id == NCKEY_DEL) {
            const char *rname = r->name ? r->name : "(unnamed)";
            char msg[128];
            snprintf(msg, sizeof(msg), "Delete rule '%s'?", rname);
            if (confirm_dialog(sm, "Delete Rule", msg)) {
                delete_rule_with_history(st, rule_idx, "Delete");
                return -2;
            }
            /* confirm_dialog returned false — redraw popup */
        }
    }
}

static int review_missing_popup(ui_state_machine_t *sm, struct missing_rule *mr) {
    struct ncplane *n = sm->std;

    struct popup_rect p = popup_center(n, 12, 56, 2, 4);
    int content_w = p.w - 4;

    while (1) {
        popup_draw(n, p, mr->app_name ? mr->app_name : "Missing Rule");

        int row = p.y + 2;
        int lx = p.x + 2;
        int vx = p.x + 18;

        ncplane_on_styles(n, NCSTYLE_BOLD);
        ui_set_color(n, COL_ERROR);
        ncplane_printf_yx(n, row, lx, "MISSING");
        ncplane_off_styles(n, NCSTYLE_BOLD);
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx + 10, "(no rule for this app)");
        ui_reset_color(n);
        row += 2;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx, "App name:");
        ui_set_color(n, COL_NORMAL);
        ncplane_printf_yx(n, row, vx, "%.*s", content_w - 16,
                          mr->app_name ? mr->app_name : "?");
        row++;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx, "Class pattern:");
        ui_set_color(n, COL_NORMAL);
        ncplane_printf_yx(n, row, vx, "%.*s", content_w - 16,
                          mr->class_pattern ? mr->class_pattern : "?");
        row++;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx, "Source:");
        ui_set_color(n, COL_NORMAL);
        ncplane_printf_yx(n, row, vx, "%s", mr->source ? mr->source : "?");
        row++;

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, row, lx, "Group:");
        ui_set_color(n, COL_NORMAL);
        ncplane_printf_yx(n, row, vx, "%s", mr->group ? mr->group : "-");
        ui_reset_color(n);

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + p.h - 1, p.x + 3,
                          " Enter:Create rule  Esc:Close ");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (id == 27 || id == 'q')
            return -1;

        if (id == NCKEY_ENTER || id == '\n') {
            struct ui_state *st = sm->st;

            int new_idx = append_rule(st);
            if (new_idx < 0) return -1;

            /* pre-fill from missing rule data */
            struct rule *r = &st->rules.rules[new_idx];
            if (mr->class_pattern)
                r->match.class_re = strdup(mr->class_pattern);
            if (mr->app_name)
                r->name = strdup(mr->app_name);

            /* open editor; if saved, return index; if cancelled, remove */
            if (edit_rule_modal(sm, r, new_idx, &st->history)) {
                update_display_name(r);
                st->modified = 1;
                if (st->rule_modified) st->rule_modified[new_idx] = 1;
                return new_idx;
            } else {
                /* cancelled — remove the empty rule */
                rule_free(r);
                st->rules.count--;
                return -1;
            }
        }
    }
}

/* --- dialogs --- */

static int confirm_dialog(ui_state_machine_t *sm, const char *title, const char *msg) {
    struct ncplane *n = sm->std;
    struct popup_rect p = popup_center(n, 7, 50, 0, 0);
    int choice = 0; /* 0 = Yes, 1 = No */

    while (1) {
        ui_set_color(n, COL_BORDER);
        popup_draw(n, p, title);

        ncplane_printf_yx(n, p.y + 2, p.x + 3, "%.44s", msg);

        if (choice == 0) ui_set_color(n, COL_SELECT);
        else ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + 4, p.x + 10, " Yes ");
        ui_reset_color(n);

        if (choice == 1) ui_set_color(n, COL_SELECT);
        else ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + 4, p.x + 20, " No ");
        ui_reset_color(n);

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + 5, p.x + 3, "y/n  Left/Right  Enter");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (id == 'y' || id == 'Y') return 1;
        if (id == 'n' || id == 'N' || id == NCKEY_ESC || id == 'q') return 0;
        if (id == NCKEY_LEFT || id == NCKEY_RIGHT || id == '\t') choice = !choice;
        if (id == NCKEY_ENTER || id == '\n') return choice == 0 ? 1 : 0;
        if (id == NCKEY_BUTTON1 && ni.y == p.y + 4) {
            if (ni.x >= p.x + 10 && ni.x < p.x + 16) return 1;
            if (ni.x >= p.x + 20 && ni.x < p.x + 25) return 0;
        }
    }
}

/* --- file operations --- */

static int create_backup(struct ui_state *st) {
    char *expanded = expand_home(st->rules_path);
    const char *src = expanded ? expanded : st->rules_path;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm);

    const char *dot = strrchr(src, '.');
    const char *slash = strrchr(src, '/');
    char tmp[1024];
    if (dot && (!slash || dot > slash)) {
        snprintf(tmp, sizeof(tmp), "%.*s.backup_%s%s",
                 (int)(dot - src), src, timestamp, dot);
    } else {
        snprintf(tmp, sizeof(tmp), "%s.backup_%s", src, timestamp);
    }
    strncpy(st->backup_path, tmp, sizeof(st->backup_path) - 1);
    st->backup_path[sizeof(st->backup_path) - 1] = '\0';

    FILE *in = fopen(src, "rb");
    if (!in) {
        free(expanded);
        return -1;
    }
    FILE *out = fopen(st->backup_path, "wb");
    if (!out) {
        fclose(in);
        free(expanded);
        return -1;
    }

    char buf[4096];
    size_t nb;
    while ((nb = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, nb, out) != nb) {
            fclose(in);
            fclose(out);
            free(expanded);
            return -1;
        }
    }

    fclose(in);
    fclose(out);
    free(expanded);

    st->backup_created = 1;
    return 0;
}

static int save_rules(struct ui_state *st) {
    char *expanded = expand_home(st->rules_path);
    const char *path = expanded ? expanded : st->rules_path;

    FILE *f = fopen(path, "w");
    if (!f) {
        free(expanded);
        return -1;
    }

    fprintf(f, "# Window Rules - managed by hyprwindows\n");
    fprintf(f, "# See https://wiki.hyprland.org/Configuring/Window-Rules/\n\n");

    for (size_t i = 0; i < st->rules.count; i++) {
        rule_write(f, &st->rules.rules[i]);
    }

    fclose(f);
    free(expanded);

    st->modified = 0;
    if (st->rule_modified)
        memset(st->rule_modified, 0, st->rules.count * sizeof(int));
    return 0;
}

static void get_disabled_path(const char *rules_path, char *out, size_t out_sz) {
    if (out_sz < 20) return;
    const char *dot = strrchr(rules_path, '.');
    const char *slash = strrchr(rules_path, '/');
    size_t path_len = strlen(rules_path);
    if (path_len > out_sz - 15) path_len = out_sz - 15;

    if (dot && (!slash || dot > slash)) {
        size_t base_len = (size_t)(dot - rules_path);
        if (base_len > path_len) base_len = path_len;
        snprintf(out, out_sz, "%.*s.disabled%s", (int)base_len, rules_path, dot);
    } else {
        snprintf(out, out_sz, "%.*s.disabled", (int)path_len, rules_path);
    }
}

/* --- class alternatives popup --- */

enum match_mode { MATCH_EXACT, MATCH_PREFIX, MATCH_CONTAINS, MATCH_MODE_COUNT };
static const char *match_mode_labels[] = {"Exact", "Prefix", "Contains"};
static const char *match_mode_short[]  = {"=", "^", "*"};

/*
 * parse "^(A|B.*|.*C.*)$" or "(?i)^(A|B.*)$" into alternatives with modes.
 * returns count; fills alts[] (raw class name), modes[]; sets *case_insensitive.
 */
static int parse_class_alternatives(const char *regex, char alts[][128],
                                    int modes[], int max_alts, int *case_insensitive) {
    if (!regex || !regex[0]) return 0;

    const char *p = regex;
    *case_insensitive = 0;

    /* check for (?i) prefix */
    if (p[0] == '(' && p[1] == '?' && p[2] == 'i' && p[3] == ')') {
        *case_insensitive = 1;
        p += 4;
    }

    /* must start with ^( */
    if (*p != '^') return 0;
    p++;
    if (*p != '(') return 0;
    p++;

    int count = 0;
    while (*p && count < max_alts) {
        char raw[128];
        int ri = 0;
        int depth = 0;
        while (*p && ri < 126) {
            if (*p == '(') depth++;
            else if (*p == ')') {
                if (depth > 0) depth--;
                else break;
            }
            if (*p == '|' && depth == 0) break;
            raw[ri++] = *p++;
        }
        raw[ri] = '\0';

        /* detect match mode from the raw alternative */
        int mode = MATCH_EXACT;
        char *r = raw;
        int rlen = ri;

        int has_leading = (rlen >= 2 && r[0] == '.' && r[1] == '*');
        int has_trailing = (rlen >= 2 && r[rlen - 1] == '*' && r[rlen - 2] == '.');

        if (has_leading && has_trailing) {
            mode = MATCH_CONTAINS;
            r += 2; rlen -= 4;
            if (rlen < 0) rlen = 0;
        } else if (has_trailing && !has_leading) {
            mode = MATCH_PREFIX;
            rlen -= 2;
            if (rlen < 0) rlen = 0;
        }

        if (rlen > 0 && rlen < 128) {
            memcpy(alts[count], r, rlen);
            alts[count][rlen] = '\0';
            modes[count] = mode;
            count++;
        }

        if (*p == '|') p++;
        else break;
    }

    /* verify ends with )$ */
    if (*p == ')') {
        p++;
        if (*p == '$' && p[1] == '\0')
            return count;
    }
    return 0; /* not a valid compound pattern */
}

/* rebuild regex from alternatives with modes and case flag */
static void build_class_from_alts(char *buf, size_t buf_sz, char alts[][128],
                                  int *checked, int modes[], int count,
                                  int case_insensitive) {
    size_t o = 0;

    if (case_insensitive) {
        if (o + 4 < buf_sz) { buf[o++] = '('; buf[o++] = '?'; buf[o++] = 'i'; buf[o++] = ')'; }
    }
    if (o < buf_sz) buf[o++] = '^';
    if (o < buf_sz) buf[o++] = '(';

    int first = 1;
    for (int i = 0; i < count; i++) {
        if (!checked[i]) continue;
        if (!first && o < buf_sz - 1) buf[o++] = '|';
        first = 0;

        size_t alen = strlen(alts[i]);

        if (modes[i] == MATCH_CONTAINS && o + 2 < buf_sz) {
            buf[o++] = '.'; buf[o++] = '*';
        }

        for (size_t j = 0; j < alen && o < buf_sz - 4; j++)
            buf[o++] = alts[i][j];

        if ((modes[i] == MATCH_PREFIX || modes[i] == MATCH_CONTAINS) && o + 2 < buf_sz) {
            buf[o++] = '.'; buf[o++] = '*';
        }
    }

    if (first) {
        buf[0] = '\0';
        return;
    }
    if (o + 2 < buf_sz) { buf[o++] = ')'; buf[o++] = '$'; }
    buf[o] = '\0';
}

/* popup for editing class alternatives with checkboxes, match modes, and case toggle */
static int class_alternatives_popup(ui_state_machine_t *sm, char *class_buf, size_t class_buf_sz) {
    struct ncplane *n = sm->std;

    char alts[32][128];
    int checked[32];
    int modes[32];
    int case_insensitive = 0;
    int count = parse_class_alternatives(class_buf, alts, modes, 32, &case_insensitive);
    if (count == 0) return 0; /* not a compound pattern, caller should use text edit */

    for (int i = 0; i < count; i++) checked[i] = 1;

    int sel = 0; /* selected row: 0..count-1 = alternatives, count = add row */
    int adding = 0; /* 1 = typing in the add row */
    char add_buf[128] = "";
    int add_cursor = 0;
    int scroll = 0;

    while (1) {
        int total_rows = count + 1; /* alternatives + add row */
        /* +7: border*2 + case toggle + header + items + hint*2 */
        int popup_h = total_rows + 8;
        if (popup_h > 24) popup_h = 24;
        int popup_w = 60;
        struct popup_rect p = popup_center(n, popup_h, popup_w, 2, 4);
        int content_h = p.h - 8;
        if (content_h < 1) content_h = 1;
        int avail_w = p.w - 14; /* space for " [x] [=] text" */

        if (scroll > sel) scroll = sel;
        if (sel >= scroll + content_h) scroll = sel - content_h + 1;
        if (scroll < 0) scroll = 0;

        popup_draw(n, p, "Class Patterns");

        /* case toggle */
        ui_set_color(n, COL_ACCENT);
        ncplane_printf_yx(n, p.y + 2, p.x + 2,
            "Case: [%c] insensitive      (i:toggle)",
            case_insensitive ? 'x' : ' ');
        ui_reset_color(n);

        /* header */
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + 3, p.x + 2, " Chk Mode  Class name");
        ncplane_printf_yx(n, p.y + 3, p.x + 2 + avail_w + 10, "m:cycle");
        ui_reset_color(n);

        int row = p.y + 4;
        for (int vi = 0; vi < content_h && scroll + vi < total_rows; vi++) {
            int idx = scroll + vi;
            if (idx < count) {
                /* alternative row */
                if (idx == sel) ui_set_color(n, COL_SELECT);
                else ui_set_color(n, COL_NORMAL);

                ncplane_printf_yx(n, row, p.x + 2, " [%c] [%s] %-*.*s",
                    checked[idx] ? 'x' : ' ',
                    match_mode_short[modes[idx]],
                    avail_w, avail_w, alts[idx]);

                /* show match mode label dimmed at right edge */
                if (idx == sel) {
                    int label_len = (int)strlen(match_mode_labels[modes[idx]]);
                    int label_x = p.x + p.w - label_len - 3;
                    ui_set_color(n, COL_DIM);
                    ncplane_printf_yx(n, row, label_x, "%s", match_mode_labels[modes[idx]]);
                }
                ui_reset_color(n);
            } else {
                /* add row */
                if (idx == sel) ui_set_color(n, COL_ACCENT);
                else ui_set_color(n, COL_DIM);

                if (adding) {
                    int text_avail = avail_w;
                    if (text_avail < 4) text_avail = 4;
                    int text_scroll = 0;
                    if (add_cursor > text_avail - 1)
                        text_scroll = add_cursor - (text_avail - 1);
                    int vis_len = (int)strlen(add_buf) - text_scroll;
                    if (vis_len > text_avail) vis_len = text_avail;
                    if (vis_len < 0) vis_len = 0;
                    ncplane_printf_yx(n, row, p.x + 2, "  +        %-*.*s", text_avail, text_avail, "");
                    ncplane_printf_yx(n, row, p.x + 13, "%.*s", vis_len, add_buf + text_scroll);
                } else {
                    ncplane_printf_yx(n, row, p.x + 2, "  +        %-*.*s", avail_w, avail_w, "Add class...");
                }
                ui_reset_color(n);
            }
            row++;
        }

        if (total_rows > content_h) {
            draw_scrollbar(n, p.y + 4, p.x + p.w - 2, content_h, total_rows, scroll);
        }

        /* hints */
        ui_set_color(n, COL_DIM);
        if (adding) {
            ncplane_printf_yx(n, p.y + p.h - 3, p.x + 2,
                "Type class name");
            ncplane_printf_yx(n, p.y + p.h - 2, p.x + 2,
                "Enter:Confirm  Esc:Cancel");
        } else {
            ncplane_printf_yx(n, p.y + p.h - 3, p.x + 2,
                "Space:Toggle  m:Match mode  i:Case  d:Delete");
            ncplane_printf_yx(n, p.y + p.h - 2, p.x + 2,
                "Enter:Add  s:Save  q:Cancel");
        }
        ui_reset_color(n);

        if (adding) {
            int text_avail = avail_w;
            if (text_avail < 4) text_avail = 4;
            int text_scroll = 0;
            if (add_cursor > text_avail - 1)
                text_scroll = add_cursor - (text_avail - 1);
            int add_vis_row = p.y + 4 + (count - scroll);
            if (add_vis_row >= p.y + 4 && add_vis_row < p.y + 4 + content_h)
                notcurses_cursor_enable(sm->nc, add_vis_row, p.x + 13 + (add_cursor - text_scroll));
        } else {
            notcurses_cursor_disable(sm->nc);
        }

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (adding) {
            int len = (int)strlen(add_buf);
            if (id == NCKEY_ENTER || id == '\n') {
                if (add_buf[0] && count < 32) {
                    snprintf(alts[count], 128, "%s", add_buf);
                    checked[count] = 1;
                    modes[count] = MATCH_EXACT;
                    count++;
                    add_buf[0] = '\0';
                    add_cursor = 0;
                    sel = count; /* select the add row again */
                }
                adding = 0;
                notcurses_cursor_disable(sm->nc);
            } else if (id == NCKEY_ESC) {
                add_buf[0] = '\0';
                add_cursor = 0;
                adding = 0;
                notcurses_cursor_disable(sm->nc);
            } else if (id == NCKEY_LEFT) {
                if (add_cursor > 0) add_cursor--;
            } else if (id == NCKEY_RIGHT) {
                if (add_cursor < len) add_cursor++;
            } else if (id == NCKEY_HOME) {
                add_cursor = 0;
            } else if (id == NCKEY_END) {
                add_cursor = len;
            } else if (id == NCKEY_BACKSPACE || id == 127 || id == 8) {
                if (add_cursor > 0) {
                    memmove(add_buf + add_cursor - 1, add_buf + add_cursor, len - add_cursor + 1);
                    add_cursor--;
                }
            } else if (id == NCKEY_DEL) {
                if (add_cursor < len) {
                    memmove(add_buf + add_cursor, add_buf + add_cursor + 1, len - add_cursor);
                }
            } else if (id >= 32 && id < 127 && len < 126) {
                memmove(add_buf + add_cursor + 1, add_buf + add_cursor, len - add_cursor + 1);
                add_buf[add_cursor] = (char)id;
                add_cursor++;
            }
            continue;
        }

        /* navigation mode */
        if (id == NCKEY_UP || id == NCKEY_SCROLL_UP) {
            if (sel > 0) sel--;
        } else if (id == NCKEY_DOWN || id == NCKEY_SCROLL_DOWN) {
            if (sel < total_rows - 1) sel++;
        } else if (id == ' ') {
            if (sel < count) checked[sel] = !checked[sel];
        } else if (id == 'm' || id == 'M') {
            /* cycle match mode for selected alternative */
            if (sel < count) {
                modes[sel] = (modes[sel] + 1) % MATCH_MODE_COUNT;
            }
        } else if (id == 'i' || id == 'I') {
            /* toggle case sensitivity */
            case_insensitive = !case_insensitive;
        } else if (id == NCKEY_ENTER || id == '\n') {
            if (sel == count) {
                /* add row */
                adding = 1;
                add_buf[0] = '\0';
                add_cursor = 0;
            } else if (sel < count) {
                checked[sel] = !checked[sel];
            }
        } else if (id == NCKEY_DEL || id == 'd' || id == 'D') {
            if (sel < count && count > 1) {
                /* remove this alternative */
                for (int i = sel; i < count - 1; i++) {
                    memcpy(alts[i], alts[i + 1], 128);
                    checked[i] = checked[i + 1];
                    modes[i] = modes[i + 1];
                }
                count--;
                if (sel >= count) sel = count - 1;
            }
        } else if (id == 's' || id == 'S') {
            /* save - rebuild regex */
            build_class_from_alts(class_buf, class_buf_sz, alts, checked, modes,
                                  count, case_insensitive);
            notcurses_cursor_disable(sm->nc);
            return 1;
        } else if (id == 'q' || id == 'Q' || id == NCKEY_ESC) {
            notcurses_cursor_disable(sm->nc);
            return -1; /* cancelled */
        } else if (id == NCKEY_BUTTON1) {
            int clicked_vi = ni.y - (p.y + 4);
            if (clicked_vi >= 0 && clicked_vi < content_h) {
                int clicked_idx = scroll + clicked_vi;
                if (clicked_idx < total_rows) {
                    sel = clicked_idx;
                    if (sel < count) checked[sel] = !checked[sel];
                    else if (sel == count) {
                        adding = 1;
                        add_buf[0] = '\0';
                        add_cursor = 0;
                    }
                }
            }
            /* check if clicked case toggle row */
            if (ni.y == p.y + 2 && ni.x >= p.x + 2 && ni.x < p.x + 40) {
                case_insensitive = !case_insensitive;
            }
        }
    }
}

/* --- rule edit modal --- */

static int edit_rule_modal(ui_state_machine_t *sm, struct rule *r, int rule_index, struct history_stack *history) {
    struct ncplane *n = sm->std;

    int base_h = 20;
    int extras_h = r->extras_count > 0 ? (int)r->extras_count + 2 : 0;
    struct popup_rect p = popup_center(n, base_h + extras_h, 60, 4, 0);
    int h = p.h, w = p.w, y = p.y, x = p.x;

    enum { F_NAME, F_DERIVED, F_CLASS, F_TITLE, F_TAG, F_WORKSPACE, F_FLOAT, F_CENTER, F_SIZE, F_OPACITY, F_COUNT };
    int field = 0;

    char name_buf[128], class_buf[128], title_buf[128], tag_buf[64], ws_buf[32], size_buf[32], opacity_buf[32];
    char orig_name[128], orig_class[128], orig_title[128], orig_tag[64], orig_ws[32], orig_size[32], orig_opacity[32];
    int float_val = r->actions.float_set ? r->actions.float_val : 0;
    int center_val = r->actions.center_set ? r->actions.center_val : 0;
    int orig_float = float_val, orig_center = center_val;

    snprintf(name_buf, sizeof(name_buf), "%s", r->name ? r->name : "");
    snprintf(class_buf, sizeof(class_buf), "%s", r->match.class_re ? r->match.class_re : "");
    snprintf(title_buf, sizeof(title_buf), "%s", r->match.title_re ? r->match.title_re : "");
    snprintf(tag_buf, sizeof(tag_buf), "%s", r->actions.tag ? r->actions.tag : "");
    snprintf(ws_buf, sizeof(ws_buf), "%s", r->actions.workspace ? r->actions.workspace : "");
    snprintf(size_buf, sizeof(size_buf), "%s", r->actions.size ? r->actions.size : "");
    snprintf(opacity_buf, sizeof(opacity_buf), "%s", r->actions.opacity ? r->actions.opacity : "");

    snprintf(orig_name, sizeof(orig_name), "%s", name_buf);
    snprintf(orig_class, sizeof(orig_class), "%s", class_buf);
    snprintf(orig_title, sizeof(orig_title), "%s", title_buf);
    snprintf(orig_tag, sizeof(orig_tag), "%s", tag_buf);
    snprintf(orig_ws, sizeof(orig_ws), "%s", ws_buf);
    snprintf(orig_size, sizeof(orig_size), "%s", size_buf);
    snprintf(orig_opacity, sizeof(orig_opacity), "%s", opacity_buf);

    int editing = 0;
    int cursor_pos = 0;

    while (1) {
        ui_set_color(n, COL_BORDER);
        popup_draw(n, p, "Edit Rule");

        char derived[64] = "";
        clean_class_name(class_buf, derived, sizeof(derived));
        if (!derived[0]) {
            clean_class_name(title_buf, derived, sizeof(derived));
        }
        if (!derived[0]) snprintf(derived, sizeof(derived), "(unnamed)");

        int changed[F_COUNT] = {0};
        changed[F_NAME] = strcmp(name_buf, orig_name) != 0;
        changed[F_CLASS] = strcmp(class_buf, orig_class) != 0;
        changed[F_TITLE] = strcmp(title_buf, orig_title) != 0;
        changed[F_TAG] = strcmp(tag_buf, orig_tag) != 0;
        changed[F_WORKSPACE] = strcmp(ws_buf, orig_ws) != 0;
        changed[F_FLOAT] = (float_val != orig_float);
        changed[F_CENTER] = (center_val != orig_center);
        changed[F_SIZE] = strcmp(size_buf, orig_size) != 0;
        changed[F_OPACITY] = strcmp(opacity_buf, orig_opacity) != 0;

        int row = y + 2;
        const char *labels[] = {"Name:", "", "Class:", "Title:", "Tag:", "Workspace:", "Float:", "Center:", "Size:", "Opacity:"};
        char *bufs[] = {name_buf, NULL, class_buf, title_buf, tag_buf, ws_buf, NULL, NULL, size_buf, opacity_buf};

        for (int i = 0; i < F_COUNT; i++) {
            if (i == F_DERIVED) {
                if (field == F_DERIVED) ui_set_color(n, COL_ACCENT);
                else ui_set_color(n, COL_DIM);
                ncplane_printf_yx(n, row, x + 2, "  -> %-38.38s", derived);
                if (field == F_DERIVED) {
                    ncplane_printf_yx(n, row, x + 44, "[Enter to use]");
                }
                ui_reset_color(n);
                row++;
                continue;
            }

            if (i == field) ui_set_color(n, COL_SELECT);
            else ui_set_color(n, COL_NORMAL);

            char label[16];
            snprintf(label, sizeof(label), "%s%s", changed[i] ? "*" : " ", labels[i]);
            ncplane_printf_yx(n, row, x + 1, "%-12s", label);

            if (i == F_FLOAT) {
                ncplane_printf_yx(n, row, x + 14, "[%c] %s", float_val ? 'x' : ' ', float_val ? "Yes" : "No");
            } else if (i == F_CENTER) {
                ncplane_printf_yx(n, row, x + 14, "[%c] %s", center_val ? 'x' : ' ', center_val ? "Yes" : "No");
            } else {
                int avail = w - 14 - 2; /* usable text width inside popup */
                if (avail < 4) avail = 4;
                if (editing && i == field) {
                    int len = (int)strlen(bufs[i]);
                    /* compute scroll offset so cursor is always visible */
                    int scroll = 0;
                    if (cursor_pos > avail - 1)
                        scroll = cursor_pos - (avail - 1);
                    /* print visible portion */
                    int vis_len = len - scroll;
                    if (vis_len > avail) vis_len = avail;
                    if (vis_len < 0) vis_len = 0;
                    /* blank the field area first */
                    ncplane_printf_yx(n, row, x + 14, "%-*.*s", avail, avail, "");
                    ncplane_printf_yx(n, row, x + 14, "%.*s", vis_len, bufs[i] + scroll);
                    /* draw cursor character */
                    int cursor_x = x + 14 + (cursor_pos - scroll);
                    if (cursor_pos < len)
                        ncplane_putchar_yx(n, row, cursor_x, bufs[i][cursor_pos]);
                } else {
                    ncplane_printf_yx(n, row, x + 14, "%-*.*s", avail, avail, bufs[i]);
                }
            }

            ui_reset_color(n);
            row++;
        }

        if (r->extras_count > 0) {
            row++;
            ui_set_color(n, COL_ACCENT);
            ncplane_printf_yx(n, row++, x + 2, "Other properties:");
            ui_reset_color(n);

            ui_set_color(n, COL_DIM);
            for (size_t i = 0; i < r->extras_count && row < y + h - 4; i++) {
                ncplane_printf_yx(n, row++, x + 4, "%-12.12s = %.30s", r->extras[i].key, r->extras[i].value);
            }
            ui_reset_color(n);
        }

        ui_set_color(n, COL_DIM);
        if (editing) {
            ncplane_printf_yx(n, y + h - 3, x + 2, "Type to edit, Backspace/Del to delete");
            ncplane_printf_yx(n, y + h - 2, x + 2, "Left/Right:Move  Enter:Done  Esc:Cancel");
        } else {
            ncplane_printf_yx(n, y + h - 3, x + 2, "Up/Down:Select  Enter:Edit  Space:Toggle");
            ncplane_printf_yx(n, y + h - 2, x + 2, "s:Save     q:Cancel");
        }
        ui_reset_color(n);

        if (editing) {
            int avail = w - 14 - 2;
            if (avail < 4) avail = 4;
            int scroll = 0;
            if (cursor_pos > avail - 1) scroll = cursor_pos - (avail - 1);
            notcurses_cursor_enable(sm->nc, y + 2 + field, x + 14 + (cursor_pos - scroll));
        } else {
            notcurses_cursor_disable(sm->nc);
        }

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (editing && bufs[field]) {
            char *buf = bufs[field];
            int len = (int)strlen(buf);
            if (id == NCKEY_ENTER || id == '\n') {
                editing = 0;
                notcurses_cursor_disable(sm->nc);
            } else if (id == NCKEY_ESC) {
                editing = 0;
                notcurses_cursor_disable(sm->nc);
            } else if (id == NCKEY_LEFT) {
                if (cursor_pos > 0) cursor_pos--;
            } else if (id == NCKEY_RIGHT) {
                if (cursor_pos < len) cursor_pos++;
            } else if (id == NCKEY_HOME) {
                cursor_pos = 0;
            } else if (id == NCKEY_END) {
                cursor_pos = len;
            } else if (id == NCKEY_BACKSPACE || id == 127 || id == 8) {
                if (cursor_pos > 0) {
                    memmove(buf + cursor_pos - 1, buf + cursor_pos, len - cursor_pos + 1);
                    cursor_pos--;
                }
            } else if (id == NCKEY_DEL) {
                if (cursor_pos < len) {
                    memmove(buf + cursor_pos, buf + cursor_pos + 1, len - cursor_pos);
                }
            } else if (id >= 32 && id < 127 && len < 126) {
                memmove(buf + cursor_pos + 1, buf + cursor_pos, len - cursor_pos + 1);
                buf[cursor_pos] = (char)id;
                cursor_pos++;
            }
            continue;
        }

        if (id == NCKEY_BUTTON1) {
            if (ni.x >= x && ni.x < x + w) {
                int clicked_row = ni.y - (y + 2);
                if (clicked_row >= 0 && clicked_row < F_COUNT) {
                    field = clicked_row;
                    if (field == F_DERIVED) {
                        snprintf(name_buf, sizeof(name_buf), "%s", derived);
                        field = F_NAME;
                    }
                    else if (field == F_FLOAT) float_val = !float_val;
                    else if (field == F_CENTER) center_val = !center_val;
                    else if (bufs[field]) {
                        if (field == F_CLASS) {
                            int ret = class_alternatives_popup(sm, class_buf, sizeof(class_buf));
                            if (ret == 0) { editing = 1; cursor_pos = (int)strlen(bufs[field]); }
                        } else {
                            editing = 1;
                            cursor_pos = (int)strlen(bufs[field]);
                        }
                    }
                }
            }
            continue;
        }
        if (id == NCKEY_SCROLL_UP && field > 0) { field--; continue; }
        if (id == NCKEY_SCROLL_DOWN && field < F_COUNT - 1) { field++; continue; }

        if (id == NCKEY_UP && field > 0) field--;
        else if (id == NCKEY_DOWN && field < F_COUNT - 1) field++;
        else if (id == NCKEY_ENTER || id == '\n') {
            if (field == F_DERIVED) {
                snprintf(name_buf, sizeof(name_buf), "%s", derived);
                field = F_NAME;
            }
            else if (field == F_FLOAT) { float_val = !float_val; }
            else if (field == F_CENTER) { center_val = !center_val; }
            else if (bufs[field]) {
                if (field == F_CLASS) {
                    int ret = class_alternatives_popup(sm, class_buf, sizeof(class_buf));
                    if (ret == 0) { editing = 1; cursor_pos = (int)strlen(bufs[field]); }
                    /* ret == 1 (saved) or -1 (cancelled): stay in edit_rule_modal */
                } else {
                    editing = 1; cursor_pos = (int)strlen(bufs[field]);
                }
            }
        }
        else if (id == ' ') {
            if (field == F_FLOAT) { float_val = !float_val; }
            else if (field == F_CENTER) { center_val = !center_val; }
        }
        else if (id == 's' || id == 'S') {
            /* deep copy BEFORE modifying rule fields */
            struct rule old_state = rule_copy(r);

            /* apply changes to rule */
            free(r->name); r->name = name_buf[0] ? strdup(name_buf) : NULL;
            free(r->match.class_re); r->match.class_re = class_buf[0] ? strdup(class_buf) : NULL;
            free(r->match.title_re); r->match.title_re = title_buf[0] ? strdup(title_buf) : NULL;
            free(r->actions.tag); r->actions.tag = tag_buf[0] ? strdup(tag_buf) : NULL;
            free(r->actions.workspace); r->actions.workspace = ws_buf[0] ? strdup(ws_buf) : NULL;
            free(r->actions.size); r->actions.size = size_buf[0] ? strdup(size_buf) : NULL;
            free(r->actions.opacity); r->actions.opacity = opacity_buf[0] ? strdup(opacity_buf) : NULL;
            r->actions.float_set = 1; r->actions.float_val = float_val;
            r->actions.center_set = 1; r->actions.center_val = center_val;
            update_display_name(r);

            char desc[128];
            snprintf(desc, sizeof(desc), "Edit rule %d", rule_index);
            history_record(history, CHANGE_EDIT, rule_index, &old_state, r, desc);
            rule_free(&old_state);

            notcurses_cursor_disable(sm->nc);
            return 1;
        }
        else if (id == 'q' || id == 'Q' || id == NCKEY_ESC) {
            notcurses_cursor_disable(sm->nc);
            return 0;
        }
    }
}

/* --- search --- */

struct search_state {
    char query[256];
    int *matches;
    size_t match_count;
    int current_match;
    int active;
};

static void search_init(struct search_state *s, size_t max_matches) {
    if (!s) return;
    memset(s, 0, sizeof(struct search_state));
    if (max_matches < 16) max_matches = 16;
    s->matches = malloc(max_matches * sizeof(int));
}

static void search_free(struct search_state *s) {
    if (!s) return;
    free(s->matches);
    memset(s, 0, sizeof(struct search_state));
}

static void search_update(struct search_state *s, struct ruleset *rs) {
    if (!s || !rs) return;

    s->match_count = 0;
    if (!s->query[0]) {
        s->active = 0;
        return;
    }

    s->active = 1;

    char lower_query[256];
    snprintf(lower_query, sizeof(lower_query), "%s", s->query);
    str_to_lower_inplace(lower_query);

    for (size_t i = 0; i < rs->count; i++) {
        struct rule *r = &rs->rules[i];

        const char *name = r->display_name ? r->display_name : "";
        const char *class_re = r->match.class_re ? r->match.class_re : "";
        const char *title_re = r->match.title_re ? r->match.title_re : "";
        const char *tag = r->actions.tag ? r->actions.tag : "";
        const char *workspace = r->actions.workspace ? r->actions.workspace : "";

        char name_lower[128];
        snprintf(name_lower, sizeof(name_lower), "%s", name);
        str_to_lower_inplace(name_lower);

        char class_lower[128];
        snprintf(class_lower, sizeof(class_lower), "%s", class_re);
        str_to_lower_inplace(class_lower);

        if (strstr(name_lower, lower_query) != NULL ||
            strstr(class_lower, lower_query) != NULL ||
            strstr(title_re, s->query) != NULL ||
            strstr(tag, s->query) != NULL ||
            strstr(workspace, s->query) != NULL) {
            s->matches[s->match_count++] = i;
        }
    }

    s->current_match = 0;
}

static void search_next(struct search_state *s) {
    if (!s || s->match_count == 0) return;
    s->current_match = (s->current_match + 1) % s->match_count;
}

static void search_prev(struct search_state *s) {
    if (!s || s->match_count == 0) return;
    if (s->current_match == 0) {
        s->current_match = (int)s->match_count - 1;
    } else {
        s->current_match--;
    }
}

static int search_modal(ui_state_machine_t *sm, struct search_state *s, struct ruleset *rs) {
    struct ncplane *n = sm->std;
    struct popup_rect p = popup_center(n, 7, 60, 0, 0);
    int y = p.y, x = p.x;

    while (1) {
        ui_set_color(n, COL_BORDER);
        popup_draw(n, p, "Search Rules");

        ncplane_printf_yx(n, y + 2, x + 2, "Query: ");
        ui_set_color(n, COL_SELECT);
        ncplane_printf_yx(n, y + 2, x + 10, "%-46s_", s->query);
        ui_reset_color(n);

        if (s->match_count > 0) {
            ncplane_printf_yx(n, y + 4, x + 2, "Found %zu matches (n/N to navigate)", s->match_count);
        } else {
            ncplane_printf_yx(n, y + 4, x + 2, "No matches");
        }

        ncplane_printf_yx(n, y + 5, x + 2, "Enter to jump, Esc to close");

        notcurses_cursor_enable(sm->nc, y + 2, x + 10 + (int)strlen(s->query));
        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        size_t len = strlen(s->query);

        if (id == NCKEY_ENTER || id == '\n') {
            notcurses_cursor_disable(sm->nc);
            if (s->match_count > 0) {
                return s->matches[s->current_match];
            }
            return -1;
        } else if (id == NCKEY_ESC) {
            notcurses_cursor_disable(sm->nc);
            return -1;
        } else if (id == NCKEY_BACKSPACE || id == 127 || id == 8) {
            if (len > 0) {
                s->query[len - 1] = '\0';
                search_update(s, rs);
            }
        } else if (id == 'n' || id == 'N') {
            if (id == 'n') search_next(s);
            else search_prev(s);
        } else if (id >= 32 && id < 127 && len < sizeof(s->query) - 1) {
            s->query[len] = (char)id;
            s->query[len + 1] = '\0';
            search_update(s, rs);
        }
    }
}

/* --- threaded spinner --- */

struct spinner_work {
    void (*func)(void *arg);
    void *arg;
    volatile int done;
};

static void *spinner_thread_fn(void *arg) {
    struct spinner_work *w = arg;
    w->func(w->arg);
    w->done = 1;
    return NULL;
}

/* retro braille spinner frames */
static const char *spinner_frames[] = {
    "\u2839", "\u2838", "\u2834", "\u2826",
    "\u2807", "\u280f", "\u2819", "\u2839",
};
#define SPINNER_NFRAMES 8
#define SPINNER_INTERVAL_MS 80

/*
 * Run work->func(work->arg) in a background thread while animating
 * a retro spinner on the notcurses plane.
 *
 * If sm is NULL (no TUI yet), runs synchronously without animation.
 * cx/cy are the center coords for the spinner + message.
 * If cy < 0, centers vertically. If cx < 0, centers horizontally.
 */
static void run_with_spinner(ui_state_machine_t *sm, const char *msg,
                             int cy, int cx,
                             void (*func)(void *), void *arg) {
    if (!sm || !sm->nc) {
        func(arg);
        return;
    }

    struct spinner_work work = { .func = func, .arg = arg, .done = 0 };

    pthread_t tid;
    if (pthread_create(&tid, NULL, spinner_thread_fn, &work) != 0) {
        /* fallback: run synchronously */
        func(arg);
        return;
    }

    struct ncplane *n = sm->std;
    unsigned height, width;
    ncplane_dim_yx(n, &height, &width);

    if (cy < 0) cy = (int)height / 2;
    int msg_len = (int)strlen(msg);
    if (cx < 0) cx = ((int)width - msg_len - 2) / 2;

    int frame = 0;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = SPINNER_INTERVAL_MS * 1000000L };

    while (!work.done) {
        const char *sp = spinner_frames[frame % SPINNER_NFRAMES];

        /* draw spinner + message */
        ui_set_color(n, COL_ACCENT);
        ncplane_printf_yx(n, cy, cx, "%s", sp);
        ui_set_color(n, COL_TITLE);
        ncplane_printf_yx(n, cy, cx + 2, "%s", msg);
        ui_reset_color(n);
        notcurses_render(sm->nc);

        nanosleep(&ts, NULL);
        frame++;
    }

    pthread_join(tid, NULL);

    /* final frame */
    ui_set_color(n, COL_ACCENT);
    ncplane_printf_yx(n, cy, cx, "\u2800");
    ui_set_color(n, COL_TITLE);
    ncplane_printf_yx(n, cy, cx + 2, "%s", msg);
    ui_reset_color(n);
    notcurses_render(sm->nc);
}

/* --- splash screen --- */

static const char *splash_logo[] = {
    "  \u2584\u2588\u2588\u2588\u2588  \u2588\u2588\u2580\u2588\u2588\u2588   \u2592\u2588\u2588\u2588\u2588\u2588   \u2592\u2588\u2588\u2588\u2588\u2588  \u2584\u2584\u2584\u2588\u2588\u2588\u2588\u2588\u2593",
    " \u2588\u2588\u2592 \u2580\u2588\u2592\u2593\u2588\u2588 \u2592 \u2588\u2588\u2592\u2592\u2588\u2588\u2592  \u2588\u2588\u2592\u2592\u2588\u2588\u2592  \u2588\u2588\u2592\u2593  \u2588\u2588\u2592 \u2593\u2592",
    "\u2592\u2588\u2588\u2591\u2584\u2584\u2584\u2591\u2593\u2588\u2588 \u2591\u2584\u2588 \u2592\u2592\u2588\u2588\u2591  \u2588\u2588\u2592\u2592\u2588\u2588\u2591  \u2588\u2588\u2592\u2592 \u2593\u2588\u2588\u2591 \u2592\u2591",
    "\u2591\u2593\u2588  \u2588\u2588\u2593\u2592\u2588\u2588\u2580\u2580\u2588\u2584  \u2592\u2588\u2588   \u2588\u2588\u2591\u2592\u2588\u2588   \u2588\u2588\u2591\u2591 \u2593\u2588\u2588\u2593 \u2591 ",
    "\u2591\u2592\u2593\u2588\u2588\u2588\u2580\u2592\u2591\u2588\u2588\u2593 \u2592\u2588\u2588\u2592\u2591 \u2588\u2588\u2588\u2588\u2593\u2592\u2591\u2591 \u2588\u2588\u2588\u2588\u2593\u2592\u2591  \u2592\u2588\u2588\u2592 \u2591 ",
    " \u2591\u2592   \u2592 \u2591 \u2592\u2593 \u2591\u2592\u2593\u2591\u2591 \u2592\u2591\u2592\u2591\u2592\u2591 \u2591 \u2592\u2591\u2592\u2591\u2592\u2591   \u2592 \u2591\u2591   ",
    "  \u2591   \u2591   \u2591\u2592 \u2591 \u2592\u2591  \u2591 \u2592 \u2592\u2591   \u2591 \u2592 \u2592\u2591     \u2591    ",
    "\u2591 \u2591   \u2591   \u2591\u2591   \u2591 \u2591 \u2591 \u2592  \u2591 \u2591 \u2591 \u2592    \u2591      ",
    "      \u2591    \u2591         \u2591 \u2591      \u2591 \u2591            ",
};
#define SPLASH_LOGO_LINES 9
#define SPLASH_LOGO_WIDTH 45

/* rainbow gradient: true RGB per line, matching the bash logo-colors.sh */
static const struct { unsigned r, g, b; } splash_colors[SPLASH_LOGO_LINES] = {
    {204,  50,  50},  /* red */
    {255,  85,  85},  /* light red */
    {255, 200,  60},  /* yellow */
    {100, 255, 100},  /* light green */
    { 50, 180,  50},  /* green */
    {100, 255, 255},  /* light cyan */
    { 50, 200, 200},  /* cyan */
    {100, 160, 255},  /* light blue */
    {200, 120, 255},  /* light magenta */
};

/* --- actions view --- */

struct action_item {
    const char *name;
    const char *description;
};

static const struct action_item actions_list[] = {
    {"Rename all rules to display names",
     "Sets each rule's name to its derived display name (from class/title pattern)."},
    {"Merge duplicate rules",
     "Combines rules with identical match fields into one, merging their actions."},
    {"Reload Hyprland config",
     "Runs 'hyprctl reload' to apply saved window rules."},
};
#define ACTIONS_COUNT ((int)(sizeof(actions_list) / sizeof(actions_list[0])))

static void action_bulk_rename(ui_state_machine_t *sm) {
    struct ui_state *st = sm->st;
    struct ncplane *n = sm->std;
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);

    /* build list of rules that would change */
    int *change_idx = NULL;
    int would_change = 0;
    for (size_t i = 0; i < st->rules.count; i++) {
        struct rule *r = &st->rules.rules[i];
        update_display_name(r);
        if (!r->display_name) continue;
        if (!r->name || strcmp(r->name, r->display_name) != 0)
            would_change++;
    }

    if (would_change == 0) {
        set_status(st, "All rules already have display names");
        return;
    }

    change_idx = malloc((size_t)would_change * sizeof(int));
    if (!change_idx) return;
    int ci = 0;
    for (size_t i = 0; i < st->rules.count; i++) {
        struct rule *r = &st->rules.rules[i];
        if (!r->display_name) continue;
        if (!r->name || strcmp(r->name, r->display_name) != 0)
            change_idx[ci++] = (int)i;
    }

    /* scrollable preview popup */
    int scroll = 0;
    int want_h = (int)scr_h - 4;
    int want_w = (int)scr_w - 8;
    if (want_h < 10) want_h = 10;
    if (want_w < 50) want_w = 50;
    struct popup_rect p = popup_center(n, want_h, want_w, 2, 2);
    int content_w = p.w - 4;
    int visible = p.h - 5; /* header + footer rows */

    while (1) {
        char title[64];
        snprintf(title, sizeof(title), "Bulk Rename (%d change%s)",
                 would_change, would_change == 1 ? "" : "s");
        popup_draw(n, p, title);

        int lx = p.x + 2;
        int row = p.y + 2;

        /* column headers */
        ncplane_on_styles(n, NCSTYLE_BOLD);
        ui_set_color(n, COL_DIM);
        int half = (content_w - 4) / 2;
        ncplane_printf_yx(n, row, lx, "%-*.*s", half, half, "Current Name");
        ncplane_printf_yx(n, row, lx + half + 1, "  ");
        ncplane_printf_yx(n, row, lx + half + 4, "%-*.*s", half, half, "New Name");
        ncplane_off_styles(n, NCSTYLE_BOLD);
        ui_reset_color(n);
        row++;

        /* list entries */
        for (int i = 0; i < visible && scroll + i < would_change; i++) {
            int ri = change_idx[scroll + i];
            struct rule *r = &st->rules.rules[ri];
            const char *old_name = r->name ? r->name : "(none)";
            const char *new_name = r->display_name;

            ui_set_color(n, COL_WARN);
            ncplane_printf_yx(n, row + i, lx, "%-*.*s", half, half, old_name);
            ui_reset_color(n);
            ui_set_color(n, COL_DIM);
            ncplane_printf_yx(n, row + i, lx + half + 1, "->");
            ui_reset_color(n);
            ui_set_color(n, COL_ACCENT);
            ncplane_printf_yx(n, row + i, lx + half + 4, "%-*.*s", half, half, new_name);
            ui_reset_color(n);
        }

        /* scroll indicator */
        if (would_change > visible) {
            ui_set_color(n, COL_DIM);
            ncplane_printf_yx(n, p.y + p.h - 2, lx,
                              "(%d-%d of %d)",
                              scroll + 1,
                              scroll + visible < would_change ? scroll + visible : would_change,
                              would_change);
            ui_reset_color(n);
        }

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + p.h - 1, p.x + 3,
                          " Enter:Apply  Esc:Cancel ");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (id == NCKEY_ESC || id == 'q') {
            free(change_idx);
            set_status(st, "Bulk rename cancelled");
            return;
        }
        if (id == NCKEY_UP || id == NCKEY_SCROLL_UP) {
            if (scroll > 0) scroll--;
        }
        else if (id == NCKEY_DOWN || id == NCKEY_SCROLL_DOWN) {
            if (scroll < would_change - visible) scroll++;
        }
        else if (id == NCKEY_PGUP) {
            scroll -= visible;
            if (scroll < 0) scroll = 0;
        }
        else if (id == NCKEY_PGDOWN) {
            scroll += visible;
            if (scroll > would_change - visible) scroll = would_change - visible;
            if (scroll < 0) scroll = 0;
        }
        else if (id == NCKEY_ENTER || id == '\n') {
            /* apply all renames */
            int renamed = 0;
            for (int i = 0; i < would_change; i++) {
                int ri = change_idx[i];
                struct rule *r = &st->rules.rules[ri];
                free(r->name);
                r->name = strdup(r->display_name);
                st->rule_modified[ri] = 1;
                renamed++;
            }
            st->modified = 1;
            free(change_idx);
            set_status(st, "Renamed %d rule%s (not saved to file)",
                       renamed, renamed == 1 ? "" : "s");
            return;
        }
    }
}

/* merge actions from src into dst, keeping dst's values where both are set */
static void merge_rule_actions(struct rule *dst, const struct rule *src) {
    if (!dst->actions.tag && src->actions.tag)
        dst->actions.tag = strdup(src->actions.tag);
    if (!dst->actions.workspace && src->actions.workspace)
        dst->actions.workspace = strdup(src->actions.workspace);
    if (!dst->actions.opacity && src->actions.opacity)
        dst->actions.opacity = strdup(src->actions.opacity);
    if (!dst->actions.size && src->actions.size)
        dst->actions.size = strdup(src->actions.size);
    if (!dst->actions.move && src->actions.move)
        dst->actions.move = strdup(src->actions.move);
    if (!dst->actions.float_set && src->actions.float_set) {
        dst->actions.float_set = 1;
        dst->actions.float_val = src->actions.float_val;
    }
    if (!dst->actions.center_set && src->actions.center_set) {
        dst->actions.center_set = 1;
        dst->actions.center_val = src->actions.center_val;
    }
    /* merge extras that dst doesn't already have */
    for (size_t i = 0; i < src->extras_count; i++) {
        int found = 0;
        for (size_t j = 0; j < dst->extras_count; j++) {
            if (strcmp(dst->extras[j].key, src->extras[i].key) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            struct rule_extra *tmp = realloc(dst->extras,
                (dst->extras_count + 1) * sizeof(struct rule_extra));
            if (tmp) {
                dst->extras = tmp;
                dst->extras[dst->extras_count].key = strdup(src->extras[i].key);
                dst->extras[dst->extras_count].value = strdup(src->extras[i].value);
                dst->extras_count++;
            }
        }
    }
}

struct dup_group {
    int *indices;
    int count;
};

static void action_merge_duplicates(ui_state_machine_t *sm) {
    struct ui_state *st = sm->st;
    struct ncplane *n = sm->std;
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);

    if (!st->rule_status) {
        set_status(st, "No review data — switch to Review tab first");
        return;
    }

    /* find duplicate groups */
    int *visited = calloc(st->rules.count, sizeof(int));
    if (!visited) return;

    struct dup_group *groups = NULL;
    int ngroups = 0;

    for (size_t i = 0; i < st->rules.count; i++) {
        if (visited[i] || st->rule_status[i] != RULE_DUPLICATE) continue;
        visited[i] = 1;

        /* start a new group */
        struct dup_group g = {0};
        g.indices = malloc(st->rules.count * sizeof(int));
        if (!g.indices) { free(visited); return; }
        g.indices[0] = (int)i;
        g.count = 1;

        for (size_t j = i + 1; j < st->rules.count; j++) {
            if (visited[j]) continue;
            if (rules_duplicate(&st->rules.rules[i], &st->rules.rules[j])) {
                visited[j] = 1;
                g.indices[g.count++] = (int)j;
            }
        }

        struct dup_group *tmp = realloc(groups, (size_t)(ngroups + 1) * sizeof(struct dup_group));
        if (!tmp) { free(g.indices); break; }
        groups = tmp;
        groups[ngroups++] = g;
    }
    free(visited);

    if (ngroups == 0) {
        set_status(st, "No duplicate rules found");
        free(groups);
        return;
    }

    /* count total rules that will be removed */
    int total_removed = 0;
    for (int g = 0; g < ngroups; g++)
        total_removed += groups[g].count - 1;

    /* scrollable preview popup */
    int scroll = 0;
    int want_h = (int)scr_h - 4;
    int want_w = (int)scr_w - 8;
    if (want_h < 10) want_h = 10;
    if (want_w < 50) want_w = 50;
    struct popup_rect p = popup_center(n, want_h, want_w, 2, 2);
    int content_w = p.w - 4;
    int visible = p.h - 5;

    /* build preview lines */
    /* each group: header + one line per rule showing its unique actions */
    int nlines = 0;
    for (int g = 0; g < ngroups; g++)
        nlines += 1 + groups[g].count + 1; /* header + rules + blank */

    char **lines = calloc((size_t)nlines, sizeof(char *));
    int *line_colors = calloc((size_t)nlines, sizeof(int)); /* 0=normal, 1=header, 2=keep, 3=remove */
    if (!lines || !line_colors) {
        free(lines); free(line_colors);
        for (int g = 0; g < ngroups; g++) free(groups[g].indices);
        free(groups);
        return;
    }

    int li = 0;
    for (int g = 0; g < ngroups; g++) {
        struct rule *first = &st->rules.rules[groups[g].indices[0]];
        const char *match_str = first->match.class_re ? first->match.class_re :
                                first->match.title_re ? first->match.title_re : "?";
        lines[li] = malloc(256);
        if (lines[li])
            snprintf(lines[li], 256, "Group %d: %.*s (%d rules)",
                     g + 1, content_w - 20, match_str, groups[g].count);
        line_colors[li] = 1;
        li++;

        for (int r = 0; r < groups[g].count; r++) {
            int ri = groups[g].indices[r];
            struct rule *rule = &st->rules.rules[ri];
            /* summarize actions */
            char acts[200] = "";
            int alen = 0;
            if (rule->actions.tag)
                alen += snprintf(acts + alen, sizeof(acts) - (size_t)alen, "tag:%s ", rule->actions.tag);
            if (rule->actions.workspace)
                alen += snprintf(acts + alen, sizeof(acts) - (size_t)alen, "ws:%s ", rule->actions.workspace);
            if (rule->actions.float_set)
                alen += snprintf(acts + alen, sizeof(acts) - (size_t)alen, "float:%s ", rule->actions.float_val ? "on" : "off");
            if (rule->actions.opacity)
                alen += snprintf(acts + alen, sizeof(acts) - (size_t)alen, "opacity:%s ", rule->actions.opacity);
            if (rule->actions.size)
                alen += snprintf(acts + alen, sizeof(acts) - (size_t)alen, "size:%s ", rule->actions.size);
            if (rule->actions.center_set)
                alen += snprintf(acts + alen, sizeof(acts) - (size_t)alen, "center:%s ", rule->actions.center_val ? "on" : "off");
            for (size_t e = 0; e < rule->extras_count && alen < (int)sizeof(acts) - 20; e++)
                alen += snprintf(acts + alen, sizeof(acts) - (size_t)alen, "%s:%s ", rule->extras[e].key, rule->extras[e].value);
            if (alen == 0) snprintf(acts, sizeof(acts), "(no actions)");

            const char *rname = rule->display_name ? rule->display_name :
                                rule->name ? rule->name : "";
            lines[li] = malloc(512);
            if (lines[li]) {
                if (r == 0)
                    snprintf(lines[li], 512, "  KEEP  #%d %-12.*s  %.*s",
                             ri + 1, 12, rname, content_w - 30, acts);
                else
                    snprintf(lines[li], 512, "  MERGE #%d %-12.*s  %.*s",
                             ri + 1, 12, rname, content_w - 30, acts);
            }
            line_colors[li] = (r == 0) ? 2 : 3;
            li++;
        }

        lines[li] = strdup("");
        line_colors[li] = 0;
        li++;
    }
    nlines = li;

    while (1) {
        char title[64];
        snprintf(title, sizeof(title), "Merge Duplicates (%d group%s, %d removed)",
                 ngroups, ngroups == 1 ? "" : "s", total_removed);
        popup_draw(n, p, title);

        int lx = p.x + 2;
        int row = p.y + 2;

        for (int i = 0; i < visible && scroll + i < nlines; i++) {
            const char *line = lines[scroll + i];
            if (!line) continue;
            int color = line_colors[scroll + i];
            if (color == 1) {
                ncplane_on_styles(n, NCSTYLE_BOLD);
                ui_set_color(n, COL_ACCENT);
            } else if (color == 2) {
                ui_set_color(n, COL_ACCENT);
            } else if (color == 3) {
                ui_set_color(n, COL_WARN);
            } else {
                ui_set_color(n, COL_NORMAL);
            }
            ncplane_printf_yx(n, row + i, lx, "%.*s", content_w, line);
            if (color == 1) ncplane_off_styles(n, NCSTYLE_BOLD);
            ui_reset_color(n);
        }

        if (nlines > visible) {
            ui_set_color(n, COL_DIM);
            ncplane_printf_yx(n, p.y + p.h - 2, lx,
                              "(%d-%d of %d lines)",
                              scroll + 1,
                              scroll + visible < nlines ? scroll + visible : nlines,
                              nlines);
            ui_reset_color(n);
        }

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, p.y + p.h - 1, p.x + 3,
                          " Enter:Merge  Esc:Cancel ");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        if (id == NCKEY_ESC || id == 'q') {
            set_status(st, "Merge cancelled");
            goto cleanup;
        }
        if (id == NCKEY_UP || id == NCKEY_SCROLL_UP) {
            if (scroll > 0) scroll--;
        }
        else if (id == NCKEY_DOWN || id == NCKEY_SCROLL_DOWN) {
            if (scroll < nlines - visible) scroll++;
        }
        else if (id == NCKEY_PGUP) {
            scroll -= visible;
            if (scroll < 0) scroll = 0;
        }
        else if (id == NCKEY_PGDOWN) {
            scroll += visible;
            if (scroll > nlines - visible) scroll = nlines - visible;
            if (scroll < 0) scroll = 0;
        }
        else if (id == NCKEY_ENTER || id == '\n') {
            /* perform merges — process groups in reverse to keep indices valid */
            int merged = 0;
            for (int g = ngroups - 1; g >= 0; g--) {
                int keep = groups[g].indices[0];
                /* merge actions from all others into the kept rule */
                for (int r = 1; r < groups[g].count; r++) {
                    int ri = groups[g].indices[r];
                    merge_rule_actions(&st->rules.rules[keep], &st->rules.rules[ri]);
                }
                /* delete merged-away rules in reverse order */
                for (int r = groups[g].count - 1; r >= 1; r--) {
                    int ri = groups[g].indices[r];
                    remove_rule_at(st, ri);
                    merged++;
                }
                if (st->rule_modified) st->rule_modified[keep] = 1;
            }
            st->modified = 1;
            compute_rule_status(st);
            set_status(st, "Merged %d duplicate%s (%d rule%s removed)",
                       ngroups, ngroups == 1 ? "" : "s",
                       merged, merged == 1 ? "" : "s");
            goto cleanup;
        }
    }

cleanup:
    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    free(line_colors);
    for (int g = 0; g < ngroups; g++) free(groups[g].indices);
    free(groups);
}

static void action_hyprctl_reload(ui_state_machine_t *sm) {
    struct ui_state *st = sm->st;

    if (st->modified) {
        if (!confirm_dialog(sm, "Unsaved Changes",
                            "Save before reloading Hyprland?")) {
            return;
        }
        if (!st->backup_created) create_backup(st);
        if (save_rules(st) == 0) {
            set_status(st, "Saved %zu rules", st->rules.count);
        } else {
            set_status(st, "Failed to save rules");
            return;
        }
    }

    int rc = system("hyprctl reload > /dev/null 2>&1");
    if (rc == 0) {
        set_status(st, "Hyprland config reloaded");
    } else {
        set_status(st, "hyprctl reload failed (exit %d)", rc);
    }
}

static void draw_actions_view(struct ncplane *n, struct ui_state *st,
                               int y, int h, int w) {
    /* title */
    ui_set_color(n, COL_DIM);
    ncplane_printf_yx(n, y, 2, "Bulk Actions");
    ui_reset_color(n);

    int list_y = y + 2;
    int visible_rows = h - 3;
    if (visible_rows < 1) return;

    int row = list_y;
    for (int i = 0; i < ACTIONS_COUNT && row - list_y < visible_rows; i++) {
        if (i == st->selected) {
            ui_set_color(n, COL_SELECT);
            ui_fill_row(n, row, 1, w - 2, ' ');
            ncplane_printf_yx(n, row, 3, "> %s", actions_list[i].name);
            ui_reset_color(n);
            row++;
            /* description below */
            if (row - list_y < visible_rows) {
                ui_set_color(n, COL_DIM);
                ncplane_printf_yx(n, row, 5, "%.*s", w - 8, actions_list[i].description);
                ui_reset_color(n);
                row++;
            }
        } else {
            ui_set_color(n, COL_NORMAL);
            ncplane_printf_yx(n, row, 3, "  %s", actions_list[i].name);
            ui_reset_color(n);
            row++;
        }
    }
}

static void handle_actions_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni) {
    struct ui_state *st = sm->st;
    (void)ni;

    if (id == NCKEY_UP && st->selected > 0) st->selected--;
    else if (id == NCKEY_DOWN && st->selected < ACTIONS_COUNT - 1) st->selected++;
    else if (id == NCKEY_HOME) st->selected = 0;
    else if (id == NCKEY_END) st->selected = ACTIONS_COUNT - 1;
    else if (id == NCKEY_ENTER || id == '\n') {
        switch (st->selected) {
        case 0: action_bulk_rename(sm); break;
        case 1: action_merge_duplicates(sm); break;
        case 2: action_hyprctl_reload(sm); break;
        default: break;
        }
    }
}

static void draw_splash(ui_state_machine_t *sm) {
    struct ncplane *n = sm->std;
    unsigned height, width;
    ncplane_dim_yx(n, &height, &width);
    ncplane_erase(n);

    /* center the logo block: 9 lines logo + 2 blank + subtitle + 1 blank + "hyprwindows" + 1 blank + prompt = ~16 */
    int total_h = SPLASH_LOGO_LINES + 6;
    int start_y = ((int)height - total_h) / 2;
    if (start_y < 1) start_y = 1;

    /* draw logo lines with rainbow gradient */
    for (int i = 0; i < SPLASH_LOGO_LINES; i++) {
        ncplane_set_fg_rgb8(n, splash_colors[i].r, splash_colors[i].g, splash_colors[i].b);
        ncplane_set_bg_default(n);
        int lx = ((int)width - SPLASH_LOGO_WIDTH) / 2;
        if (lx < 0) lx = 0;
        ncplane_putstr_yx(n, start_y + i, lx, splash_logo[i]);
    }

    ui_reset_color(n);

    /* "hyprwindows" title below logo */
    int title_y = start_y + SPLASH_LOGO_LINES + 1;
    const char *title = "h y p r w i n d o w s";
    int title_len = (int)strlen(title);
    ncplane_set_fg_rgb8(n, 100, 200, 255);
    ncplane_on_styles(n, NCSTYLE_BOLD);
    ncplane_putstr_yx(n, title_y, ((int)width - title_len) / 2, title);
    ncplane_off_styles(n, NCSTYLE_BOLD);

    /* subtitle */
    const char *subtitle = "hyprland window rules manager";
    int sub_len = (int)strlen(subtitle);
    ncplane_set_fg_rgb8(n, 100, 110, 130);
    ncplane_putstr_yx(n, title_y + 1, ((int)width - sub_len) / 2, subtitle);

    /* prompt */
    const char *prompt = "press any key to continue";
    int prompt_len = (int)strlen(prompt);
    ncplane_set_fg_rgb8(n, 80, 90, 110);
    ncplane_putstr_yx(n, title_y + 3, ((int)width - prompt_len) / 2, prompt);

    ui_reset_color(n);
    notcurses_render(sm->nc);

    /* wait for any keypress (ignore mouse and releases) */
    while (1) {
        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;
        if (nckey_mouse_p(id)) continue;
        break;
    }
}

/* --- main draw/input dispatch --- */

static void draw_ui(ui_state_machine_t *sm) {
    struct ui_state *st = sm->st;
    struct ncplane *n = sm->std;
    unsigned height, width;
    ncplane_dim_yx(n, &height, &width);

    if (height < UI_MIN_HEIGHT || width < UI_MIN_WIDTH) {
        ncplane_erase(n);
        ncplane_printf_yx(n, (int)height / 2, 0, "Resize to %dx%d", UI_MIN_WIDTH, UI_MIN_HEIGHT);
        notcurses_render(sm->nc);
        return;
    }

    ncplane_erase(n);

    if (st->modified) {
        draw_header(n, width, "hyprwindows [*]");
    } else {
        draw_header(n, width, "hyprwindows");
    }

    draw_tabs(n, 1, width, sm->current_state);

    int content_y = 2;
    int content_h = (int)height - 4;

    switch (sm->current_state) {
    case VIEW_RULES:
        if ((int)width > 100) {
            int list_w = (int)width * 2 / 3;
            draw_rules_view(n, st, content_y, content_h, list_w);
            draw_rule_detail(n, st, content_y, list_w, content_h, (int)width - list_w);
        } else {
            draw_rules_view(n, st, content_y, content_h, (int)width);
        }
        break;
    case VIEW_WINDOWS:
        draw_windows_view(n, st, content_y, content_h, (int)width);
        break;
    case VIEW_REVIEW:
        draw_review_view(sm, st, content_y, content_h, (int)width);
        break;
    case VIEW_ACTIONS:
        draw_actions_view(n, st, content_y, content_h, (int)width);
        break;
    }

    const char *help = "F1:Help";
    switch (sm->current_state) {
    case VIEW_RULES:
        help = "Enter:Edit  /:Find  s:Sort  ^S:Save  F1:Help";
        break;
    case VIEW_WINDOWS:
        help = "Enter:Details  r:Reload  F1:Help";
        break;
    case VIEW_REVIEW:
        help = "Enter:Details/Create  r:Reload  F1:Help";
        break;
    case VIEW_ACTIONS:
        help = "Enter:Run action  F1:Help";
        break;
    }
    draw_statusbar(n, (int)height - 1, width, st->status, help);

    notcurses_render(sm->nc);
}

static void help_popup(ui_state_machine_t *sm) {
    struct ncplane *n = sm->std;

    static const char *lines[] = {
        "Navigation",
        "  1 / 2 / 3 / 4 Switch views",
        "  Up / Down      Move cursor / scroll",
        "  PgUp / PgDn    Scroll by page",
        "  Home / End     Jump to first / last",
        "",
        "Rules View",
        "  Enter          Edit selected rule",
        "  n              New rule",
        "  d / Del        Delete rule",
        "  x              Disable rule",
        "  /              Search rules",
        "  s              Cycle sort mode",
        "",
        "Windows View",
        "  Enter          Show window details",
        "",
        "Review View",
        "  Enter          Details / create rule",
        "  d              Delete unused rule",
        "",
        "Actions View",
        "  Enter          Run selected action",
        "",
        "Editing",
        "  Ctrl+S         Save to file",
        "  Ctrl+B         Create backup",
        "  Ctrl+Z         Undo",
        "  Ctrl+Y         Redo",
        "  r              Reload from file",
        "",
        "General",
        "  q              Quit",
        "  F1             This help",
    };
    int nlines = (int)(sizeof(lines) / sizeof(lines[0]));

    struct popup_rect p = popup_center(n, nlines + 4, 42, 2, 4);
    int h = p.h, w = p.w, y = p.y, x = p.x;

    while (1) {
        popup_draw(n, p, "Keybindings");

        int visible = h - 3;
        for (int i = 0; i < visible && i < nlines; i++) {
            const char *line = lines[i];
            if (line[0] && line[0] != ' ') {
                /* section header */
                ncplane_on_styles(n, NCSTYLE_BOLD);
                ui_set_color(n, COL_ACCENT);
                ncplane_printf_yx(n, y + 2 + i, x + 2, "%.*s", w - 4, line);
                ncplane_off_styles(n, NCSTYLE_BOLD);
                ui_reset_color(n);
            } else {
                ui_set_color(n, COL_NORMAL);
                ncplane_printf_yx(n, y + 2 + i, x + 2, "%.*s", w - 4, line);
                ui_reset_color(n);
            }
        }

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h - 1, x + 3, " Press any key to close ");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;
        break;
    }
}

static void handle_global_keys(ui_state_machine_t *sm, uint32_t id, ncinput *ni) {
    struct ui_state *st = sm->st;

    if (id == 'q' || id == 'Q') {
        if (ncinput_ctrl_p(ni)) return; /* Ctrl+Q: ignore */
        if (st->modified) {
            int choice = 0;
            struct ncplane *n = sm->std;
            struct popup_rect dp = popup_center(n, 9, 50, 0, 0);

            while (1) {
                ui_set_color(n, COL_BORDER);
                popup_draw(n, dp, "Unsaved Changes");

                ncplane_printf_yx(n, dp.y + 2, dp.x + 3, "You have unsaved changes.");
                ncplane_printf_yx(n, dp.y + 3, dp.x + 3, "What would you like to do?");

                const char *opts[] = {"Save and quit", "Quit without saving", "Cancel"};
                for (int i = 0; i < 3; i++) {
                    if (i == choice) ui_set_color(n, COL_SELECT);
                    else ui_set_color(n, COL_DIM);
                    ncplane_printf_yx(n, dp.y + 5 + i, dp.x + 5, " %s ", opts[i]);
                    ui_reset_color(n);
                }
                notcurses_render(sm->nc);

                ncinput dni;
                uint32_t did = notcurses_get(sm->nc, NULL, &dni);
                if (did == (uint32_t)-1) continue;
                if (dni.evtype == NCTYPE_RELEASE) continue;

                if (did == NCKEY_UP && choice > 0) choice--;
                else if (did == NCKEY_DOWN && choice < 2) choice++;
                else if (did == NCKEY_ENTER || did == '\n') break;
                else if (did == NCKEY_ESC) { choice = 2; break; }
                else if (did == 's' || did == 'S') { choice = 0; break; }
                else if (did == 'q') { choice = 1; break; }
            }

            if (choice == 0) {
                if (!st->backup_created) create_backup(st);
                if (save_rules(st) == 0) {
                    set_status(st, "Saved to %s", st->rules_path);
                }
                sm->running = 0;
            } else if (choice == 1) {
                sm->running = 0;
            }
        } else {
            sm->running = 0;
        }
        return;
    }

    /* Ctrl+S: save (global, works from any view) */
    if (((id == 's' || id == 'S') && ncinput_ctrl_p(ni)) || id == 0x13) {
        if (st->modified) {
            if (!st->backup_created) {
                if (create_backup(st) == 0) {
                    set_status(st, "Backup created: %s", st->backup_path);
                }
            }
            if (save_rules(st) == 0) {
                set_status(st, "Saved %zu rules to %s", st->rules.count, st->rules_path);
            } else {
                set_status(st, "Failed to save rules");
            }
        } else {
            set_status(st, "No changes to save");
        }
        return;
    }

    /* Ctrl+B: backup (global) */
    if (((id == 'b' || id == 'B') && ncinput_ctrl_p(ni)) || id == 0x02) {
        if (create_backup(st) == 0) {
            set_status(st, "Backup created: %s", st->backup_path);
        } else {
            set_status(st, "Failed to create backup");
        }
        return;
    }

    if (id == '1') { sm->current_state = VIEW_RULES; st->selected = 0; st->scroll = 0; return; }
    if (id == '2') { sm->current_state = VIEW_WINDOWS; st->selected = 0; st->scroll = 0; st->clients_loaded = 0; return; }
    if (id == '3') { sm->current_state = VIEW_REVIEW; st->selected = 0; st->scroll = 0; return; }
    if (id == '4') { sm->current_state = VIEW_ACTIONS; st->selected = 0; st->scroll = 0; return; }

    if (id == 'r' || id == 'R') {
        if (st->modified) {
            if (!confirm_dialog(sm, "Reload", "Discard unsaved changes?")) {
                return;
            }
        }
        run_with_spinner(sm, "Reloading...", -1, -1,
                         (void (*)(void *))load_rules, st);
        ncplane_erase(sm->std);
        run_with_spinner(sm, "Scanning apps...", -1, -1,
                         (void (*)(void *))load_review_data, st);
        return;
    }

    if (id == NCKEY_F01) {
        help_popup(sm);
        return;
    }
}

static void handle_rules_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni) {
    struct ui_state *st = sm->st;

    if (id == NCKEY_UP && st->selected > 0) st->selected--;
    else if (id == NCKEY_DOWN && st->selected < (int)st->rules.count - 1) st->selected++;
    else if (id == NCKEY_PGUP) { st->selected -= 10; if (st->selected < 0) st->selected = 0; }
    else if (id == NCKEY_PGDOWN) { st->selected += 10; if (st->selected >= (int)st->rules.count) st->selected = (int)st->rules.count - 1; }
    else if (id == NCKEY_HOME) st->selected = 0;
    else if (id == NCKEY_END) st->selected = (int)st->rules.count - 1;
    else if (id == '/') {
        struct search_state search;
        search_init(&search, st->rules.count);
        int result = search_modal(sm, &search, &st->rules);
        if (result >= 0) {
            st->selected = result;
        }
        search_free(&search);
    }
    else if ((id == NCKEY_ENTER || id == '\n') && st->selected >= 0 && st->selected < (int)st->rules.count) {
        if (edit_rule_modal(sm, &st->rules.rules[st->selected], st->selected, &st->history)) {
            st->modified = 1;
            if (st->rule_modified) st->rule_modified[st->selected] = 1;
            set_status(st, "Rule modified (not saved to file)");
        }
    }
    /* new rule */
    else if (id == 'n' || id == 'N') {
        int new_idx = append_rule(st);
        if (new_idx >= 0) {
            st->selected = new_idx;

            if (edit_rule_modal(sm, &st->rules.rules[new_idx], new_idx, &st->history)) {
                update_display_name(&st->rules.rules[new_idx]);
                st->modified = 1;
                if (st->rule_modified) st->rule_modified[new_idx] = 1;
                set_status(st, "New rule added (not saved to file)");
            } else {
                /* user cancelled -- remove the empty rule */
                rule_free(&st->rules.rules[new_idx]);
                st->rules.count--;
                if (st->selected >= (int)st->rules.count && st->selected > 0)
                    st->selected--;
            }
        } else {
            set_status(st, "Failed to allocate new rule");
        }
    }
    /* delete rule */
    else if ((id == 'd' || id == NCKEY_DEL) && st->selected >= 0 && st->selected < (int)st->rules.count) {
        struct rule *r = &st->rules.rules[st->selected];
        char msg[64];
        snprintf(msg, sizeof(msg), "Delete rule '%s'?", r->name ? r->name : "(unnamed)");
        if (confirm_dialog(sm, "Delete Rule", msg)) {
            delete_rule_with_history(st, st->selected, "Delete");
            if (st->selected >= (int)st->rules.count && st->selected > 0) st->selected--;
            set_status(st, "Rule deleted (not saved to file)");
        }
    }
    /* disable rule */
    else if (id == 'x' && st->selected >= 0 && st->selected < (int)st->rules.count) {
        struct rule *r = &st->rules.rules[st->selected];
        char msg[64];
        snprintf(msg, sizeof(msg), "Disable rule '%s'?", r->name ? r->name : "(unnamed)");
        if (confirm_dialog(sm, "Disable Rule", msg)) {
            char disabled_path[512];
            char *expanded = expand_home(st->rules_path);
            get_disabled_path(expanded ? expanded : st->rules_path, disabled_path, sizeof(disabled_path));
            free(expanded);

            FILE *df = fopen(disabled_path, "a");
            int wrote_ok = 0;
            if (df) {
                rule_write(df, r);
                fclose(df);
                wrote_ok = 1;
            }

            if (wrote_ok) {
                delete_rule_with_history(st, st->selected, "Disable");
                if (st->selected >= (int)st->rules.count && st->selected > 0) st->selected--;
                set_status(st, "Rule disabled -> %s", disabled_path);
            } else {
                set_status(st, "Failed to write to %s", disabled_path);
            }
        }
    }
    /* Ctrl+Z undo */
    else if (((id == 'z' || id == 'Z') && ncinput_ctrl_p(ni)) || id == 0x1a) {
        if (history_can_undo(&st->history)) {
            int rule_index = -1;
            enum change_type ctype = CHANGE_EDIT;
            struct rule *old_rule = history_undo(&st->history, &rule_index, &ctype);
            if (old_rule && rule_index >= 0) {
                if (ctype == CHANGE_DELETE) {
                    /* undo delete = re-insert the deleted rule */
                    int clamp = rule_index > (int)st->rules.count ? (int)st->rules.count : rule_index;
                    if (insert_rule_at(st, clamp, old_rule) == 0) {
                        st->selected = clamp;
                        st->modified = 1;
                        compute_rule_status(st);
                        set_status(st, "Undo delete complete");
                    }
                } else if (rule_index < (int)st->rules.count) {
                    /* undo edit = replace with old state */
                    rule_free(&st->rules.rules[rule_index]);
                    st->rules.rules[rule_index] = *old_rule;
                    old_rule = NULL; /* ownership transferred */
                    st->selected = rule_index;
                    st->modified = 1;
                    if (st->rule_modified) st->rule_modified[rule_index] = 1;
                    compute_rule_status(st);
                    set_status(st, "Undo complete");
                }
                if (old_rule) { rule_free(old_rule); }
                free(old_rule);
            }
        } else {
            set_status(st, "Nothing to undo");
        }
    }
    /* Ctrl+Y redo */
    else if (((id == 'y' || id == 'Y') && ncinput_ctrl_p(ni)) || id == 0x19) {
        if (history_can_redo(&st->history)) {
            int rule_index = -1;
            enum change_type ctype = CHANGE_EDIT;
            struct rule *redo_rule = history_redo(&st->history, &rule_index, &ctype);
            if (redo_rule && rule_index >= 0) {
                if (ctype == CHANGE_DELETE) {
                    /* redo delete = remove the rule again */
                    if (rule_index < (int)st->rules.count) {
                        remove_rule_at(st, rule_index);
                        if (st->selected >= (int)st->rules.count && st->selected > 0)
                            st->selected--;
                        st->modified = 1;
                        compute_rule_status(st);
                        set_status(st, "Redo delete complete");
                    }
                    rule_free(redo_rule);
                    free(redo_rule);
                } else if (rule_index < (int)st->rules.count) {
                    /* redo edit = replace with new state */
                    rule_free(&st->rules.rules[rule_index]);
                    st->rules.rules[rule_index] = *redo_rule;
                    st->selected = rule_index;
                    st->modified = 1;
                    if (st->rule_modified) st->rule_modified[rule_index] = 1;
                    compute_rule_status(st);
                    set_status(st, "Redo complete");
                    free(redo_rule);
                }
            }
        } else {
            set_status(st, "Nothing to redo");
        }
    }
    /* cycle sort mode */
    else if (id == 's') {
        st->sort_mode = (st->sort_mode + 1) % SORT_MODE_COUNT;
        apply_sort(st);
        st->selected = 0;
        st->scroll = 0;
        set_status(st, "Sort: %s", sort_mode_label(st->sort_mode));
    }
}

static void handle_windows_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni) {
    struct ui_state *st = sm->st;
    (void)ni;
    if (id == NCKEY_UP && st->selected > 0) st->selected--;
    else if (id == NCKEY_DOWN) st->selected++;
    else if (id == NCKEY_PGUP) { st->selected -= 10; if (st->selected < 0) st->selected = 0; }
    else if (id == NCKEY_PGDOWN) st->selected += 10;
    else if (id == NCKEY_HOME) st->selected = 0;
    else if (id == NCKEY_END) st->selected = 9999; /* clamped in draw */
    else if (id == NCKEY_ENTER || id == '\n') {
        if (st->clients_loaded && st->selected < (int)st->clients.count) {
            int jump = window_detail_popup(sm, &st->clients.items[st->selected], &st->rules);
            if (jump >= 0 && jump < (int)st->rules.count) {
                /* jump to the selected rule in the rules view */
                sm->current_state = VIEW_RULES;
                st->selected = jump;
                st->scroll = 0;
            }
        }
    }
    else if (id == 'r' || id == 'R') {
        st->clients_loaded = 0;
        set_status(st, "Refreshed windows");
    }
}

static void handle_review_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni) {
    struct ui_state *st = sm->st;
    (void)ni;
    int total = review_total_items(st);
    if (total == 0) return;

    if (id == NCKEY_UP && st->selected > 0) st->selected--;
    else if (id == NCKEY_DOWN && st->selected < total - 1) st->selected++;
    else if (id == NCKEY_PGUP) { st->selected -= 10; if (st->selected < 0) st->selected = 0; }
    else if (id == NCKEY_PGDOWN) { st->selected += 10; if (st->selected >= total) st->selected = total - 1; }
    else if (id == NCKEY_HOME) st->selected = 0;
    else if (id == NCKEY_END) st->selected = total - 1;
    else if (id == NCKEY_ENTER || id == '\n') {
        int unused_count = review_count_unused(st);
        if (st->selected < unused_count) {
            /* unused rule: show detail popup, optionally jump to rules view */
            int ri = review_unused_index(st, st->selected);
            if (ri >= 0) {
                int jump = review_unused_popup(sm, ri);
                if (jump == -2) {
                    /* rule was deleted — stay in review, status already recomputed */
                    int new_total = review_total_items(st);
                    if (st->selected >= new_total && new_total > 0)
                        st->selected = new_total - 1;
                    set_status(st, "Rule deleted (not saved to file)");
                } else if (jump >= 0 && jump < (int)st->rules.count) {
                    sm->current_state = VIEW_RULES;
                    st->selected = jump;
                    st->scroll = 0;
                }
            }
        } else {
            /* missing rule: show detail popup, optionally create rule */
            int mi = st->selected - unused_count;
            if (mi >= 0 && mi < (int)st->missing.count) {
                int new_idx = review_missing_popup(sm, &st->missing.items[mi]);
                if (new_idx >= 0) {
                    /* rule created — switch to rules view */
                    sm->current_state = VIEW_RULES;
                    st->selected = new_idx;
                    st->scroll = 0;
                    st->review_loaded = 0; /* invalidate review data */
                    set_status(st, "Rule created from missing entry (not saved)");
                }
            }
        }
    }
}

static void handle_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni) {
    /* global keys first (handles q, Ctrl+S, Ctrl+B, 1/2/3, r) */
    handle_global_keys(sm, id, ni);

    if (!sm->running) return;

    /* then view-specific keys */
    switch (sm->current_state) {
    case VIEW_RULES:
        handle_rules_input(sm, id, ni);
        break;
    case VIEW_WINDOWS:
        handle_windows_input(sm, id, ni);
        break;
    case VIEW_REVIEW:
        handle_review_input(sm, id, ni);
        break;
    case VIEW_ACTIONS:
        handle_actions_input(sm, id, ni);
        break;
    }
}

/* --- main entry --- */

int run_tui(void) {
    struct ui_state st;
    memset(&st, 0, sizeof(st));
    history_init(&st.history);
    init_paths(&st);

    setlocale(LC_ALL, "");

    /* save original terminal settings before notcurses changes them */
    struct termios tios_orig;
    int tios_saved = 0;
    if (tcgetattr(STDIN_FILENO, &tios_orig) == 0) {
        tios_saved = 1;
    }

    struct notcurses_options opts = {
        .flags = NCOPTION_SUPPRESS_BANNERS,
    };
    struct notcurses *nc = notcurses_core_init(&opts, NULL);
    if (!nc) {
        fprintf(stderr, "Failed to initialize notcurses\n");
        return -1;
    }

    struct ncplane *std = notcurses_stdplane(nc);
    notcurses_mice_enable(nc, NCMICE_ALL_EVENTS);

    /* disable XON/XOFF flow control so Ctrl+S reaches the app */
    {
        struct termios tios;
        if (tcgetattr(STDIN_FILENO, &tios) == 0) {
            tios.c_iflag &= ~IXON;
            tcsetattr(STDIN_FILENO, TCSANOW, &tios);
        }
    }

    ui_state_machine_t sm;
    sm.current_state = VIEW_RULES;
    sm.running = 1;
    sm.st = &st;
    sm.nc = nc;
    sm.std = std;

    draw_splash(&sm);
    ncplane_erase(std);
    run_with_spinner(&sm, "Loading rules...", -1, -1,
                     (void (*)(void *))load_rules, &st);
    ncplane_erase(std);
    run_with_spinner(&sm, "Scanning apps...", -1, -1,
                     (void (*)(void *))load_review_data, &st);

    while (sm.running) {
        draw_ui(&sm);

        ncinput ni;
        uint32_t id = notcurses_get(nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;

        /* mouse events */
        if (nckey_mouse_p(id)) {
            unsigned height, width;
            ncplane_dim_yx(std, &height, &width);

            if (sm.current_state == VIEW_RULES && id == NCKEY_BUTTON1 && ni.y > 3) {
                /* check for double-click via rapid re-click -- notcurses doesn't
                   have a built-in double-click concept like ncurses, so treat
                   single click as select */
                int content_y = 2;
                int list_row = ni.y - content_y - 2;
                if (list_row >= 0 && ni.x > 0 && ni.x < (int)width * 2 / 3) {
                    int clicked_idx = st.scroll + list_row;
                    if (clicked_idx >= 0 && clicked_idx < (int)st.rules.count) {
                        if (st.selected == clicked_idx) {
                            /* second click on same row = edit */
                            if (edit_rule_modal(&sm, &st.rules.rules[st.selected], st.selected, &st.history)) {
                                st.modified = 1;
                                set_status(&st, "Rule modified (not saved to file)");
                            }
                        } else {
                            st.selected = clicked_idx;
                        }
                    }
                }
            }
            else if (ni.y == 1 && id == NCKEY_BUTTON1) {
                if (ni.x >= tab_x_start[0] && ni.x < tab_x_end[0]) {
                    sm.current_state = VIEW_RULES; st.selected = 0; st.scroll = 0;
                } else if (ni.x >= tab_x_start[1] && ni.x < tab_x_end[1]) {
                    sm.current_state = VIEW_WINDOWS; st.scroll = 0; st.clients_loaded = 0;
                } else if (ni.x >= tab_x_start[2] && ni.x < tab_x_end[2]) {
                    sm.current_state = VIEW_REVIEW; st.selected = 0; st.scroll = 0;
                } else if (ni.x >= tab_x_start[3] && ni.x < tab_x_end[3]) {
                    sm.current_state = VIEW_ACTIONS; st.selected = 0; st.scroll = 0;
                }
            }
            else if (id == NCKEY_SCROLL_UP) {
                if (sm.current_state == VIEW_RULES && st.selected > 0) st.selected--;
                else if (sm.current_state == VIEW_WINDOWS && st.scroll > 0) st.scroll--;
            }
            else if (id == NCKEY_SCROLL_DOWN) {
                if (sm.current_state == VIEW_RULES && st.selected < (int)st.rules.count - 1) st.selected++;
                else if (sm.current_state == VIEW_WINDOWS) st.scroll++;
            }
            continue;
        }

        handle_input(&sm, id, &ni);
    }

    notcurses_stop(nc);
    if (tios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &tios_orig);
    ruleset_free(&st.rules);
    free(st.rule_status);
    free(st.rule_modified);
    free(st.file_order);
    clients_free(&st.clients);
    missing_rules_free(&st.missing);
    history_free(&st.history);

    return 0;
}
