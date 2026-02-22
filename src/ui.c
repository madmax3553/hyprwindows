#include "ui.h"

#include <ctype.h>
#include <locale.h>
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
};

/* rule status flags */
enum rule_status {
    RULE_OK = 0,
    RULE_UNUSED = 1,
    RULE_DUPLICATE = 2,
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

    /* cached review data */
    struct missing_rules missing;
    char *review_text;
    int review_loaded;

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
static void draw_ui(ui_state_machine_t *sm);
static void handle_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);
static void handle_rules_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);
static void handle_windows_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);
static void handle_review_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni);

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

static int compare_rules_by_tag(const void *a, const void *b) {
    const struct rule *ra = (const struct rule *)a;
    const struct rule *rb = (const struct rule *)b;
    const char *ta = ra->actions.tag ? ra->actions.tag : "";
    const char *tb = rb->actions.tag ? rb->actions.tag : "";
    return strcmp(ta, tb);
}

static int rules_duplicate(const struct rule *a, const struct rule *b) {
    if (a->match.class_re && b->match.class_re) {
        if (strcmp(a->match.class_re, b->match.class_re) == 0) return 1;
    }
    if (a->match.title_re && b->match.title_re) {
        if (strcmp(a->match.title_re, b->match.title_re) == 0) return 1;
    }
    return 0;
}

static void compute_rule_status(struct ui_state *st) {
    free(st->rule_status);
    st->rule_status = calloc(st->rules.count, sizeof(enum rule_status));
    if (!st->rule_status) return;

    struct clients clients = {0};
    int have_clients = (hyprctl_clients(&clients) == 0);

    for (size_t i = 0; i < st->rules.count; i++) {
        struct rule *r = &st->rules.rules[i];

        for (size_t j = 0; j < st->rules.count; j++) {
            if (i != j && rules_duplicate(r, &st->rules.rules[j])) {
                st->rule_status[i] = RULE_DUPLICATE;
                break;
            }
        }

        if (st->rule_status[i] == RULE_OK && have_clients) {
            int matched = 0;
            for (size_t c = 0; c < clients.count; c++) {
                if (rule_matches_client(r, &clients.items[c])) {
                    matched = 1;
                    break;
                }
            }
            if (!matched) {
                st->rule_status[i] = RULE_UNUSED;
            }
        }
    }

    if (have_clients) clients_free(&clients);
}

static void load_review_data(struct ui_state *st) {
    missing_rules_free(&st->missing);
    free(st->review_text);
    st->review_text = NULL;
    st->review_loaded = 0;

    struct outbuf out;
    outbuf_init(&out);
    char *path = expand_home(st->rules_path);
    review_rules_text(path ? path : st->rules_path, &out);

    char *appmap_path = expand_home(st->appmap_path);
    find_missing_rules(path ? path : st->rules_path,
                       appmap_path ? appmap_path : st->appmap_path,
                       st->dotfiles_path, &st->missing);
    free(appmap_path);
    free(path);

    st->review_text = out.data;
    out.data = NULL;
    st->review_loaded = 1;
}

static void load_rules(struct ui_state *st) {
    ruleset_free(&st->rules);
    free(st->rule_status);
    st->rule_status = NULL;
    free(st->rule_modified);
    st->rule_modified = NULL;
    st->review_loaded = 0;

    char *path = expand_home(st->rules_path);
    if (ruleset_load(path ? path : st->rules_path, &st->rules) == 0) {
        for (size_t i = 0; i < st->rules.count; i++) {
            update_display_name(&st->rules.rules[i]);
        }
        if (st->rules.count > 1) {
            qsort(st->rules.rules, st->rules.count, sizeof(struct rule), compare_rules_by_tag);
        }
        compute_rule_status(st);
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

static void draw_tabs(struct ncplane *n, int y, unsigned width, enum view_mode mode) {
    (void)width;
    const char *tabs[] = {"[1] Rules", "[2] Windows", "[3] Review"};
    int x = 2;
    for (int i = 0; i < 3; i++) {
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
        if (*p == '.' && p[1] == '+') { p += 2; continue; }
        if (*p == '\\' && p[1] == 'd') { p += 2; continue; }
        if (*p == '\\' && p[1]) {
            p++;
            out[o++] = *p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';

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
    draw_box(n, y, 0, h, w, "Window Rules");

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
    int col_ws_w  = 6;
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
    if (max_scroll > 0) {
        int bar_h = visible;
        int thumb = (bar_h * visible) / (int)st->rules.count;
        if (thumb < 1) thumb = 1;
        int thumb_pos = (bar_h * st->scroll) / (int)st->rules.count;

        for (int i = 0; i < bar_h; i++) {
            if (i >= thumb_pos && i < thumb_pos + thumb) {
                ui_set_color(n, COL_SELECT);
                ncplane_putstr_yx(n, y + 2 + i, w - 1, "\u2588");
            } else {
                ui_set_color(n, COL_DIM);
                ncplane_putstr_yx(n, y + 2 + i, w - 1, "\u2502");
            }
            ui_reset_color(n);
        }
    }
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

    struct clients clients;
    memset(&clients, 0, sizeof(clients));
    if (hyprctl_clients(&clients) != 0) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, (w - 24) / 2, "Failed to read windows");
        ui_reset_color(n);
        return;
    }

    if (clients.count == 0) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, (w - 24) / 2, "No windows found");
        ui_reset_color(n);
        clients_free(&clients);
        return;
    }

    /* clamp selected */
    if (st->selected >= (int)clients.count) st->selected = (int)clients.count - 1;
    if (st->selected < 0) st->selected = 0;

    int visible = h - 3; /* box top + header + box bottom */
    int max_scroll = (int)clients.count > visible ? (int)clients.count - visible : 0;
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

    for (int i = 0; i < visible && (st->scroll + i) < (int)clients.count; i++) {
        int idx = st->scroll + i;
        struct client *c = &clients.items[idx];
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
    if (max_scroll > 0) {
        int bar_h = visible;
        int thumb = (bar_h * visible) / (int)clients.count;
        if (thumb < 1) thumb = 1;
        int thumb_pos = (bar_h * st->scroll) / (int)clients.count;
        for (int i = 0; i < bar_h; i++) {
            if (i >= thumb_pos && i < thumb_pos + thumb) {
                ui_set_color(n, COL_SELECT);
                ncplane_putstr_yx(n, y + 2 + i, w - 1, "\u2588");
            } else {
                ui_set_color(n, COL_DIM);
                ncplane_putstr_yx(n, y + 2 + i, w - 1, "\u2502");
            }
            ui_reset_color(n);
        }
    }

    clients_free(&clients);
}

static void window_detail_popup(ui_state_machine_t *sm, struct client *c, struct ruleset *rs) {
    struct ncplane *n = sm->std;
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);

    /* collect matching rules */
    int matches[256];
    int match_count = 0;
    for (size_t j = 0; j < rs->count && match_count < 256; j++) {
        if (rule_matches_client(&rs->rules[j], c))
            matches[match_count++] = (int)j;
    }

    /* compute content lines */
    int content_lines = 5 + match_count; /* class, title, ws, initial_class, blank, matches header, match lines */
    if (match_count == 0) content_lines = 6; /* ...+ "(none)" */
    else content_lines = 6 + match_count;

    int h = content_lines + 3; /* box top/bottom + close hint */
    int w_popup = 60;
    if (w_popup > (int)scr_w - 4) w_popup = (int)scr_w - 4;
    if (h > (int)scr_h - 2) h = (int)scr_h - 2;
    int popup_y = ((int)scr_h - h) / 2;
    int popup_x = ((int)scr_w - w_popup) / 2;
    int content_w = w_popup - 4;

    while (1) {
        for (int i = 0; i < h; i++)
            ui_fill_row(n, popup_y + i, popup_x, w_popup, ' ');
        draw_box(n, popup_y, popup_x, h, w_popup,
                 c->class_name ? c->class_name : "Window Details");

        int r = popup_y + 2;
        int lx = popup_x + 2;
        int vx = popup_x + 18;

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

        if (match_count == 0 && r < popup_y + h - 1) {
            ui_set_color(n, COL_DIM);
            ncplane_printf_yx(n, r, lx + 2, "(none)");
            ui_reset_color(n);
        } else {
            for (int m = 0; m < match_count && r < popup_y + h - 1; m++) {
                int ri = matches[m];
                const char *rname = rs->rules[ri].display_name
                    ? rs->rules[ri].display_name
                    : rs->rules[ri].name;
                ui_set_color(n, COL_NORMAL);
                ncplane_printf_yx(n, r, lx + 2, "[%d] %.*s", ri, content_w - 8, rname ? rname : "<unnamed>");
                ui_reset_color(n);
                r++;
            }
        }

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, popup_y + h - 1, popup_x + 3, " Press any key to close ");
        ui_reset_color(n);

        notcurses_render(sm->nc);

        ncinput ni;
        uint32_t id = notcurses_get(sm->nc, NULL, &ni);
        if (id == (uint32_t)-1) continue;
        if (ni.evtype == NCTYPE_RELEASE) continue;
        break;
    }
}

static void draw_review_view(ui_state_machine_t *sm, struct ui_state *st, int y, int h, int w) {
    struct ncplane *n = sm->std;
    draw_box(n, y, 0, h, w, "Rules Review");

    if (!st->review_loaded) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, (w - 15) / 2, "Loading...");
        ui_reset_color(n);
        notcurses_render(sm->nc);
        load_review_data(st);
    }

    if (!st->review_text && st->missing.count == 0) {
        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + h / 2, (w - 20) / 2, "Review unavailable");
        ui_reset_color(n);
        return;
    }

    /* clamp scroll */
    if (st->scroll < 0) st->scroll = 0;

    int visible = h - 2;
    int row = y + 1;
    int max_row = y + h - 1;
    int line = 0; /* logical line counter for scroll */

#define REV_LINE(code) do { \
    if (line >= st->scroll && row < max_row) { code; row++; } \
    line++; \
} while (0)

    if (st->review_text) {
        const char *p = st->review_text;
        while (*p) {
            const char *end = strchr(p, '\n');
            int len = end ? (int)(end - p) : (int)strlen(p);

            int is_header = (strncmp(p, "===", 3) == 0 || strncmp(p, "Summary", 7) == 0 ||
                             strncmp(p, "Potentially", 11) == 0);

            REV_LINE({
                if (is_header) {
                    ncplane_on_styles(n, NCSTYLE_BOLD);
                    ui_set_color(n, COL_ACCENT);
                }
                ncplane_printf_yx(n, row, 2, "%.*s", w - 4 < len ? w - 4 : len, p);
                if (is_header) {
                    ncplane_off_styles(n, NCSTYLE_BOLD);
                    ui_reset_color(n);
                }
            });

            p += len;
            if (*p == '\n') p++;
        }
    }

    /* missing rules */
    if (st->missing.count > 0) {
        REV_LINE((void)0); /* blank line */

        REV_LINE({
            ncplane_on_styles(n, NCSTYLE_BOLD);
            ui_set_color(n, COL_ERROR);
            ncplane_printf_yx(n, row, 2, "Missing rules (installed apps without rules):");
            ncplane_off_styles(n, NCSTYLE_BOLD);
            ui_reset_color(n);
        });

        for (size_t i = 0; i < st->missing.count; i++) {
            struct missing_rule *mr = &st->missing.items[i];
            REV_LINE({
                ui_set_color(n, COL_WARN);
                ncplane_printf_yx(n, row, 4, "%-16s [%s] %s",
                         mr->app_name ? mr->app_name : "?",
                         mr->source ? mr->source : "?",
                         mr->group ? mr->group : "");
                ui_reset_color(n);
            });
        }
    }

#undef REV_LINE


    (void)visible;
}

/* --- dialogs --- */

static int confirm_dialog(ui_state_machine_t *sm, const char *title, const char *msg) {
    struct ncplane *n = sm->std;
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);
    int h = 7, w = 50;
    int y = ((int)scr_h - h) / 2;
    int x = ((int)scr_w - w) / 2;
    int choice = 0; /* 0 = Yes, 1 = No */

    while (1) {
        ui_set_color(n, COL_BORDER);
        for (int i = 0; i < h; i++) {
            ui_fill_row(n, y + i, x, w, ' ');
        }
        draw_box(n, y, x, h, w, title);
        ui_reset_color(n);

        ncplane_printf_yx(n, y + 2, x + 3, "%.44s", msg);

        if (choice == 0) ui_set_color(n, COL_SELECT);
        else ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + 4, x + 10, " Yes ");
        ui_reset_color(n);

        if (choice == 1) ui_set_color(n, COL_SELECT);
        else ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + 4, x + 20, " No ");
        ui_reset_color(n);

        ui_set_color(n, COL_DIM);
        ncplane_printf_yx(n, y + 5, x + 3, "y/n  Left/Right  Enter");
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
        if (id == NCKEY_BUTTON1 && ni.y == y + 4) {
            if (ni.x >= x + 10 && ni.x < x + 16) return 1;
            if (ni.x >= x + 20 && ni.x < x + 25) return 0;
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

/* --- rule edit modal --- */

static int edit_rule_modal(ui_state_machine_t *sm, struct rule *r, int rule_index, struct history_stack *history) {
    struct ncplane *n = sm->std;
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);

    int base_h = 20;
    int extras_h = r->extras_count > 0 ? (int)r->extras_count + 2 : 0;
    int h = base_h + extras_h;
    if (h > (int)scr_h - 4) h = (int)scr_h - 4;
    int w = 60;
    int y = ((int)scr_h - h) / 2;
    int x = ((int)scr_w - w) / 2;

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

    while (1) {
        ui_set_color(n, COL_BORDER);
        for (int i = 0; i < h; i++) {
            ui_fill_row(n, y + i, x, w, ' ');
        }
        draw_box(n, y, x, h, w, "Edit Rule");
        ui_reset_color(n);

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
                if (editing && i == field) {
                    ncplane_printf_yx(n, row, x + 14, "%s_", bufs[i]);
                } else {
                    ncplane_printf_yx(n, row, x + 14, "%-40.40s", bufs[i]);
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
            ncplane_printf_yx(n, y + h - 3, x + 2, "Type to edit, Backspace to delete");
            ncplane_printf_yx(n, y + h - 2, x + 2, "Enter:Done  Esc:Cancel edit");
        } else {
            ncplane_printf_yx(n, y + h - 3, x + 2, "Up/Down:Select  Enter:Edit  Space:Toggle");
            ncplane_printf_yx(n, y + h - 2, x + 2, "s:Save     q:Cancel");
        }
        ui_reset_color(n);

        if (editing) {
            notcurses_cursor_enable(sm->nc, y + 2 + field, x + 14 + (int)strlen(bufs[field]));
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
            size_t len = strlen(buf);
            if (id == NCKEY_ENTER || id == '\n') {
                editing = 0;
                notcurses_cursor_disable(sm->nc);
            } else if (id == NCKEY_ESC) {
                editing = 0;
                notcurses_cursor_disable(sm->nc);
            } else if (id == NCKEY_BACKSPACE || id == 127 || id == 8) {
                if (len > 0) buf[len - 1] = '\0';
            } else if (id >= 32 && id < 127 && len < 126) {
                buf[len] = (char)id;
                buf[len + 1] = '\0';
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
                        editing = 1;
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
            else if (bufs[field]) { editing = 1; }
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
            history_record(history, rule_index, &old_state, r, desc);
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
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);
    int h = 7, w = 60;
    int y = ((int)scr_h - h) / 2;
    int x = ((int)scr_w - w) / 2;

    while (1) {
        ui_set_color(n, COL_BORDER);
        for (int i = 0; i < h; i++) ui_fill_row(n, y + i, x, w, ' ');
        draw_box(n, y, x, h, w, "Search Rules");
        ui_reset_color(n);

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

/* --- splash/loading --- */

static void draw_loading(ui_state_machine_t *sm, const char *msg) {
    struct ncplane *n = sm->std;
    unsigned height, width;
    ncplane_dim_yx(n, &height, &width);
    ncplane_erase(n);
    ui_set_color(n, COL_TITLE);
    ncplane_printf_yx(n, (int)height / 2, ((int)width - (int)strlen(msg)) / 2, "%s", msg);
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
    }

    const char *help;
    switch (sm->current_state) {
    case VIEW_RULES:
        help = "Enter:Edit  /:Find  ^S:Save  F1:Help";
        break;
    case VIEW_WINDOWS:
        help = "Enter:Details  r:Reload  F1:Help";
        break;
    case VIEW_REVIEW:
        help = "Up/Down:Scroll  r:Reload  F1:Help";
        break;
    default:
        help = "F1:Help";
        break;
    }
    draw_statusbar(n, (int)height - 1, width, st->status, help);

    notcurses_render(sm->nc);
}

static void help_popup(ui_state_machine_t *sm) {
    struct ncplane *n = sm->std;
    unsigned scr_h, scr_w;
    ncplane_dim_yx(n, &scr_h, &scr_w);

    static const char *lines[] = {
        "Navigation",
        "  1 / 2 / 3     Switch views",
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
        "",
        "Windows View",
        "  Enter          Show window details",
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

    int h = nlines + 4;
    int w = 42;
    if (h > (int)scr_h - 2) h = (int)scr_h - 2;
    if (w > (int)scr_w - 4) w = (int)scr_w - 4;
    int y = ((int)scr_h - h) / 2;
    int x = ((int)scr_w - w) / 2;

    while (1) {
        for (int i = 0; i < h; i++)
            ui_fill_row(n, y + i, x, w, ' ');
        draw_box(n, y, x, h, w, "Keybindings");

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
            unsigned scr_h, scr_w;
            ncplane_dim_yx(n, &scr_h, &scr_w);

            while (1) {
                int dh = 9, dw = 50;
                int dy = ((int)scr_h - dh) / 2;
                int dx = ((int)scr_w - dw) / 2;

                ui_set_color(n, COL_BORDER);
                for (int i = 0; i < dh; i++) ui_fill_row(n, dy + i, dx, dw, ' ');
                draw_box(n, dy, dx, dh, dw, "Unsaved Changes");
                ui_reset_color(n);

                ncplane_printf_yx(n, dy + 2, dx + 3, "You have unsaved changes.");
                ncplane_printf_yx(n, dy + 3, dx + 3, "What would you like to do?");

                const char *opts[] = {"Save and quit", "Quit without saving", "Cancel"};
                for (int i = 0; i < 3; i++) {
                    if (i == choice) ui_set_color(n, COL_SELECT);
                    else ui_set_color(n, COL_DIM);
                    ncplane_printf_yx(n, dy + 5 + i, dx + 5, " %s ", opts[i]);
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
    if (id == 's' && ncinput_ctrl_p(ni)) {
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
    if (id == 'b' && ncinput_ctrl_p(ni)) {
        if (create_backup(st) == 0) {
            set_status(st, "Backup created: %s", st->backup_path);
        } else {
            set_status(st, "Failed to create backup");
        }
        return;
    }

    if (id == '1') { sm->current_state = VIEW_RULES; st->selected = 0; st->scroll = 0; return; }
    if (id == '2') { sm->current_state = VIEW_WINDOWS; st->selected = 0; st->scroll = 0; return; }
    if (id == '3') { sm->current_state = VIEW_REVIEW; return; }

    if (id == 'r' || id == 'R') {
        if (st->modified) {
            if (!confirm_dialog(sm, "Reload", "Discard unsaved changes?")) {
                return;
            }
        }
        draw_loading(sm, "Reloading...");
        load_rules(st);
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
        /* grow the rules array by one */
        struct rule *new_rules = realloc(st->rules.rules, (st->rules.count + 1) * sizeof(struct rule));
        if (new_rules) {
            st->rules.rules = new_rules;
            memset(&st->rules.rules[st->rules.count], 0, sizeof(struct rule));
            int new_idx = (int)st->rules.count;
            st->rules.count++;

            /* also grow rule_status and rule_modified */
            enum rule_status *new_status = realloc(st->rule_status, st->rules.count * sizeof(enum rule_status));
            if (new_status) {
                st->rule_status = new_status;
                st->rule_status[new_idx] = RULE_OK;
            }
            int *new_mod = realloc(st->rule_modified, st->rules.count * sizeof(int));
            if (new_mod) {
                st->rule_modified = new_mod;
                st->rule_modified[new_idx] = 0;
            }

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
            struct rule deleted_copy = rule_copy(r);
            char desc[128];
            snprintf(desc, sizeof(desc), "Delete rule %d", st->selected);
            history_record(&st->history, st->selected, &deleted_copy, NULL, desc);
            rule_free(&deleted_copy);

            rule_free(r);
            for (int i = st->selected; i < (int)st->rules.count - 1; i++) {
                st->rules.rules[i] = st->rules.rules[i + 1];
                if (st->rule_status) st->rule_status[i] = st->rule_status[i + 1];
                if (st->rule_modified) st->rule_modified[i] = st->rule_modified[i + 1];
            }
            st->rules.count--;
            if (st->selected >= (int)st->rules.count && st->selected > 0) st->selected--;
            st->modified = 1;
            compute_rule_status(st);
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
                struct rule disabled_copy = rule_copy(r);
                char desc[128];
                snprintf(desc, sizeof(desc), "Disable rule %d", st->selected);
                history_record(&st->history, st->selected, &disabled_copy, NULL, desc);
                rule_free(&disabled_copy);

                rule_free(r);
                for (int i = st->selected; i < (int)st->rules.count - 1; i++) {
                    st->rules.rules[i] = st->rules.rules[i + 1];
                    if (st->rule_status) st->rule_status[i] = st->rule_status[i + 1];
                    if (st->rule_modified) st->rule_modified[i] = st->rule_modified[i + 1];
                }
                st->rules.count--;
                if (st->selected >= (int)st->rules.count && st->selected > 0) st->selected--;
                st->modified = 1;
                compute_rule_status(st);
                set_status(st, "Rule disabled -> %s", disabled_path);
            } else {
                set_status(st, "Failed to write to %s", disabled_path);
            }
        }
    }
    /* Ctrl+Z undo */
    else if (id == 'z' && ncinput_ctrl_p(ni)) {
        if (history_can_undo(&st->history)) {
            int rule_index = -1;
            struct rule *old_rule = history_undo(&st->history, &rule_index);
            if (old_rule && rule_index >= 0 && rule_index < (int)st->rules.count) {
                rule_free(&st->rules.rules[rule_index]);
                st->rules.rules[rule_index] = *old_rule;
                st->selected = rule_index;
                st->modified = 1;
                if (st->rule_modified) st->rule_modified[rule_index] = 1;
                compute_rule_status(st);
                set_status(st, "Undo complete");
                free(old_rule);
            }
        } else {
            set_status(st, "Nothing to undo");
        }
    }
    /* Ctrl+Y redo */
    else if (id == 'y' && ncinput_ctrl_p(ni)) {
        if (history_can_redo(&st->history)) {
            int rule_index = -1;
            struct rule *new_rule = history_redo(&st->history, &rule_index);
            if (new_rule && rule_index >= 0 && rule_index < (int)st->rules.count) {
                rule_free(&st->rules.rules[rule_index]);
                st->rules.rules[rule_index] = *new_rule;
                st->selected = rule_index;
                st->modified = 1;
                if (st->rule_modified) st->rule_modified[rule_index] = 1;
                compute_rule_status(st);
                set_status(st, "Redo complete");
                free(new_rule);
            }
        } else {
            set_status(st, "Nothing to redo");
        }
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
        /* fetch clients and show detail for selected */
        struct clients clients;
        memset(&clients, 0, sizeof(clients));
        if (hyprctl_clients(&clients) == 0 && st->selected < (int)clients.count) {
            window_detail_popup(sm, &clients.items[st->selected], &st->rules);
        }
        clients_free(&clients);
    }
}

static void handle_review_input(ui_state_machine_t *sm, uint32_t id, ncinput *ni) {
    struct ui_state *st = sm->st;
    (void)ni;
    if (id == NCKEY_UP) { st->scroll -= 1; if (st->scroll < 0) st->scroll = 0; }
    if (id == NCKEY_PGUP) { st->scroll -= 10; if (st->scroll < 0) st->scroll = 0; }
    if (id == NCKEY_DOWN) st->scroll += 1;
    if (id == NCKEY_PGDOWN) st->scroll += 10;
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
    }
}

/* --- main entry --- */

int run_tui(void) {
    struct ui_state st;
    memset(&st, 0, sizeof(st));
    history_init(&st.history);
    init_paths(&st);

    setlocale(LC_ALL, "");

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

    ui_state_machine_t sm;
    sm.current_state = VIEW_RULES;
    sm.running = 1;
    sm.st = &st;
    sm.nc = nc;
    sm.std = std;

    draw_splash(&sm);
    draw_loading(&sm, "Loading rules...");
    load_rules(&st);

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
                if (ni.x >= 2 && ni.x < 14) {
                    sm.current_state = VIEW_RULES; st.selected = 0; st.scroll = 0;
                } else if (ni.x >= 15 && ni.x < 29) {
                    sm.current_state = VIEW_WINDOWS; st.scroll = 0;
                } else if (ni.x >= 30 && ni.x < 43) {
                    sm.current_state = VIEW_REVIEW;
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
    ruleset_free(&st.rules);
    free(st.rule_status);
    free(st.rule_modified);
    missing_rules_free(&st.missing);
    free(st.review_text);
    history_free(&st.history);

    return 0;
}
