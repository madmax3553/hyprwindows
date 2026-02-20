#include "ui.h"

#include <ctype.h>
#include <locale.h>
#include <ncurses.h>
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

    /* status message */
    char status[256];
};

struct ui_state_machine {
    enum view_mode current_state;
    int running;
    struct ui_state *st;
};

/* colors */
enum {
    COL_TITLE = 1,
    COL_BORDER,
    COL_STATUS,
    COL_SELECT,
    COL_NORMAL,
    COL_DIM,
    COL_ACCENT,
    COL_WARN,
    COL_ERROR,
};

/* forward declarations */
static void clean_class_name(const char *regex, char *out, size_t out_sz);
static void update_display_name(struct rule *r);
static void draw_ui(ui_state_machine_t *sm);
static void handle_input(ui_state_machine_t *sm, int ch);
static void handle_rules_input(ui_state_machine_t *sm, int ch);
static void handle_windows_input(ui_state_machine_t *sm, int ch);
static void handle_review_input(ui_state_machine_t *sm, int ch);

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
        set_status(st, "Loaded %zu rules from %s", st->rules.count, st->rules_path);
    } else {
        set_status(st, "Failed to load rules from %s", st->rules_path);
    }
    free(path);
}

/* --- drawing helpers --- */

static void draw_box(int y, int x, int h, int w, const char *title) {
    attron(COLOR_PAIR(COL_BORDER));
    mvprintw(y, x, "+");
    mvprintw(y, x + w - 1, "+");
    mvprintw(y + h - 1, x, "+");
    mvprintw(y + h - 1, x + w - 1, "+");
    for (int i = 1; i < w - 1; i++) {
        mvprintw(y, x + i, "-");
        mvprintw(y + h - 1, x + i, "-");
    }
    for (int i = 1; i < h - 1; i++) {
        mvprintw(y + i, x, "|");
        mvprintw(y + i, x + w - 1, "|");
    }
    if (title) {
        mvprintw(y, x + 2, " %s ", title);
    }
    attroff(COLOR_PAIR(COL_BORDER));
}

static void draw_header(int width, const char *title) {
    attron(A_BOLD | COLOR_PAIR(COL_TITLE));
    mvhline(0, 0, ' ', width);
    mvprintw(0, (width - (int)strlen(title)) / 2, "%s", title);
    attroff(A_BOLD | COLOR_PAIR(COL_TITLE));
}

static void draw_statusbar(int y, int width, const char *left, const char *right) {
    attron(COLOR_PAIR(COL_STATUS));
    mvhline(y, 0, ' ', width);
    if (left) mvprintw(y, 1, "%s", left);
    if (right) mvprintw(y, width - (int)strlen(right) - 1, "%s", right);
    attroff(COLOR_PAIR(COL_STATUS));
}

static void draw_tabs(int y, int width, enum view_mode mode) {
    (void)width;
    const char *tabs[] = {"[1] Rules", "[2] Windows", "[3] Review"};
    int x = 2;
    for (int i = 0; i < 3; i++) {
        if (i == (int)mode) {
            attron(A_BOLD | COLOR_PAIR(COL_SELECT));
        } else {
            attron(COLOR_PAIR(COL_DIM));
        }
        mvprintw(y, x, " %s ", tabs[i]);
        if (i == (int)mode) {
            attroff(A_BOLD | COLOR_PAIR(COL_SELECT));
        } else {
            attroff(COLOR_PAIR(COL_DIM));
        }
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

static void draw_rules_view(struct ui_state *st, int y, int h, int w) {
    draw_box(y, 0, h, w, "Window Rules");

    if (st->rules.count == 0) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 20) / 2, "No rules loaded");
        attroff(COLOR_PAIR(COL_DIM));
        return;
    }

    int visible = h - 3;
    int max_scroll = (int)st->rules.count > visible ? (int)st->rules.count - visible : 0;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0) st->scroll = 0;

    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + visible) st->scroll = st->selected - visible + 1;

    attron(COLOR_PAIR(COL_DIM));
    mvprintw(y + 1, 3, "%-16s %-12s %-6s %-8s %s", "Application", "Tag", "WS", "Status", "Options");
    attroff(COLOR_PAIR(COL_DIM));

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
            opos += snprintf(opts + opos, sizeof(opts) - opos, "F ");
        if (r->actions.center_set && r->actions.center_val)
            opos += snprintf(opts + opos, sizeof(opts) - opos, "C ");
        if (r->actions.size)
            opos += snprintf(opts + opos, sizeof(opts) - opos, "S ");
        if (r->actions.opacity)
            opos += snprintf(opts + opos, sizeof(opts) - opos, "O ");
        if (r->extras_count > 0) {
            snprintf(opts + opos, sizeof(opts) - opos, "+%zu", r->extras_count);
        }
        if (opts[0] == '\0') strcpy(opts, "-");

        int show_tag = 1;
        if (last_tag && r->actions.tag && strcmp(last_tag, r->actions.tag) == 0) {
            show_tag = 0;
        }
        last_tag = r->actions.tag;

        if (idx == st->selected) {
            attron(COLOR_PAIR(COL_SELECT));
            mvhline(row, 1, ' ', w - 2);
        }

        mvprintw(row, 3, "%-16.16s", display);

        if (show_tag && tag[0] != '-') {
            attron(A_BOLD);
            mvprintw(row, 20, "%-12.12s", tag);
            attroff(A_BOLD);
            if (idx == st->selected) attron(COLOR_PAIR(COL_SELECT));
        } else {
            mvprintw(row, 20, "%-12.12s", show_tag ? tag : "");
        }

        mvprintw(row, 33, "%-6.6s", ws);

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
        if (idx != st->selected) attron(COLOR_PAIR(status_color));
        mvprintw(row, 40, "%-8s", status_str);
        if (idx != st->selected) attroff(COLOR_PAIR(status_color));

        attron(COLOR_PAIR(COL_DIM));
        if (idx != st->selected) {
            mvprintw(row, 49, "%.15s", opts);
        } else {
            attroff(COLOR_PAIR(COL_DIM));
            mvprintw(row, 49, "%.15s", opts);
        }
        attroff(COLOR_PAIR(COL_DIM));

        if (idx == st->selected) {
            attroff(COLOR_PAIR(COL_SELECT));
        }
    }

    /* scrollbar */
    if (max_scroll > 0) {
        int bar_h = visible;
        int thumb = (bar_h * visible) / (int)st->rules.count;
        if (thumb < 1) thumb = 1;
        int thumb_pos = (bar_h * st->scroll) / (int)st->rules.count;

        for (int i = 0; i < bar_h; i++) {
            if (i >= thumb_pos && i < thumb_pos + thumb) {
                attron(COLOR_PAIR(COL_SELECT));
                mvprintw(y + 2 + i, w - 1, "#");
                attroff(COLOR_PAIR(COL_SELECT));
            } else {
                attron(COLOR_PAIR(COL_DIM));
                mvprintw(y + 2 + i, w - 1, "|");
                attroff(COLOR_PAIR(COL_DIM));
            }
        }
    }

    attron(COLOR_PAIR(COL_DIM));
    mvprintw(y + h - 1, 3, " Enter: Edit  /: Search  n: New ");
    attroff(COLOR_PAIR(COL_DIM));
}

static void draw_rule_detail(struct ui_state *st, int y, int x, int h, int w) {
    draw_box(y, x, h, w, "Rule Details");

    if (st->selected < 0 || st->selected >= (int)st->rules.count) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, x + (w - 18) / 2, "No rule selected");
        attroff(COLOR_PAIR(COL_DIM));
        return;
    }

    struct rule *r = &st->rules.rules[st->selected];
    int row = y + 2;
    int col = x + 3;

    const char *display = r->display_name ? r->display_name : "(unnamed)";

    attron(A_BOLD | COLOR_PAIR(COL_ACCENT));
    mvprintw(row++, col, "%s", display);
    attroff(A_BOLD | COLOR_PAIR(COL_ACCENT));

    row++;

    attron(COLOR_PAIR(COL_DIM));
    mvprintw(row++, col, "Matching");
    attroff(COLOR_PAIR(COL_DIM));

    if (r->match.class_re)
        mvprintw(row++, col + 2, "Class:  %.*s", w - 12, r->match.class_re);
    if (r->match.title_re)
        mvprintw(row++, col + 2, "Title:  %.*s", w - 12, r->match.title_re);

    row++;

    attron(COLOR_PAIR(COL_DIM));
    mvprintw(row++, col, "Actions");
    attroff(COLOR_PAIR(COL_DIM));

    if (r->actions.tag)
        mvprintw(row++, col + 2, "Tag:       %s", clean_tag(r->actions.tag));
    if (r->actions.workspace)
        mvprintw(row++, col + 2, "Workspace: %s", r->actions.workspace);
    if (r->actions.float_set)
        mvprintw(row++, col + 2, "Float:     %s", r->actions.float_val ? "Yes" : "No");
    if (r->actions.center_set)
        mvprintw(row++, col + 2, "Center:    %s", r->actions.center_val ? "Yes" : "No");
    if (r->actions.size)
        mvprintw(row++, col + 2, "Size:      %s", r->actions.size);
    if (r->actions.move)
        mvprintw(row++, col + 2, "Position:  %s", r->actions.move);
    if (r->actions.opacity)
        mvprintw(row++, col + 2, "Opacity:   %s", r->actions.opacity);

    if (r->extras_count > 0) {
        row++;
        attron(COLOR_PAIR(COL_ACCENT));
        mvprintw(row++, col, "Other (%zu)", r->extras_count);
        attroff(COLOR_PAIR(COL_ACCENT));

        for (size_t i = 0; i < r->extras_count && row < y + h - 3; i++) {
            mvprintw(row++, col + 2, "%-10.10s %.*s",
                     r->extras[i].key, w - 16, r->extras[i].value);
        }
    }

    attron(COLOR_PAIR(COL_DIM));
    mvprintw(y + h - 2, col, "Press Enter to edit");
    attroff(COLOR_PAIR(COL_DIM));
}

static void draw_windows_view(struct ui_state *st, int y, int h, int w) {
    draw_box(y, 0, h, w, "Active Windows");

    struct clients clients;
    memset(&clients, 0, sizeof(clients));
    if (hyprctl_clients(&clients) != 0) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 24) / 2, "Failed to read windows");
        attroff(COLOR_PAIR(COL_DIM));
        return;
    }

    if (clients.count == 0) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 24) / 2, "No windows found");
        attroff(COLOR_PAIR(COL_DIM));
        clients_free(&clients);
        return;
    }

    int row = y + 1;
    int max_row = y + h - 1;

    for (size_t i = 0; i < clients.count && row < max_row - 2; i++) {
        struct client *c = &clients.items[i];

        attron(A_BOLD | COLOR_PAIR(COL_ACCENT));
        mvprintw(row++, 2, "Window: %s", c->class_name ? c->class_name : "<unknown>");
        attroff(A_BOLD | COLOR_PAIR(COL_ACCENT));

        if (row >= max_row - 2) break;

        if (c->title) {
            mvprintw(row++, 4, "Title: %.40s", c->title);
        }
        if (row >= max_row - 2) break;

        if (c->workspace_id >= 0) {
            mvprintw(row++, 4, "Workspace: %d", c->workspace_id);
        } else if (c->workspace_name) {
            mvprintw(row++, 4, "Workspace: %s", c->workspace_name);
        }
        if (row >= max_row - 2) break;

        /* show which rules match this window */
        int found_match = 0;
        for (size_t j = 0; j < st->rules.count && row < max_row - 2; j++) {
            if (rule_matches_client(&st->rules.rules[j], c)) {
                if (!found_match) {
                    mvprintw(row++, 4, "Matches:");
                    found_match = 1;
                }
                const char *rule_name = st->rules.rules[j].display_name
                    ? st->rules.rules[j].display_name
                    : st->rules.rules[j].name;
                mvprintw(row++, 6, "[%zu] %s", j,
                         rule_name ? rule_name : "<unnamed>");
            }
        }
        if (!found_match) {
            attron(COLOR_PAIR(COL_DIM));
            mvprintw(row++, 4, "Matches: (none)");
            attroff(COLOR_PAIR(COL_DIM));
        }

        if (row < max_row - 1) {
            row++;
        }
    }

    clients_free(&clients);
}


static void draw_review_view(struct ui_state *st, int y, int h, int w) {
    draw_box(y, 0, h, w, "Rules Review");

    if (!st->review_loaded) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 15) / 2, "Loading...");
        attroff(COLOR_PAIR(COL_DIM));
        refresh();
        load_review_data(st);
    }

    if (!st->review_text && st->missing.count == 0) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 20) / 2, "Review unavailable");
        attroff(COLOR_PAIR(COL_DIM));
        return;
    }

    int row = y + 1;
    int max_row = y + h - 1;

    if (st->review_text) {
        const char *p = st->review_text;
        while (*p && row < max_row - (int)st->missing.count - 3) {
            const char *end = strchr(p, '\n');
            int len = end ? (int)(end - p) : (int)strlen(p);

            if (strncmp(p, "===", 3) == 0 || strncmp(p, "Summary", 7) == 0 ||
                strncmp(p, "Potentially", 11) == 0) {
                attron(A_BOLD | COLOR_PAIR(COL_ACCENT));
            }
            mvprintw(row, 2, "%.*s", w - 4 < len ? w - 4 : len, p);
            if (strncmp(p, "===", 3) == 0 || strncmp(p, "Summary", 7) == 0 ||
                strncmp(p, "Potentially", 11) == 0) {
                attroff(A_BOLD | COLOR_PAIR(COL_ACCENT));
            }
            row++;

            p += len;
            if (*p == '\n') p++;
        }
    }

    /* missing rules */
    if (st->missing.count > 0 && row < max_row - 2) {
        row++;
        attron(A_BOLD | COLOR_PAIR(COL_ERROR));
        mvprintw(row++, 2, "Missing rules (installed apps without rules):");
        attroff(A_BOLD | COLOR_PAIR(COL_ERROR));

        for (size_t i = 0; i < st->missing.count && row < max_row; i++) {
            struct missing_rule *mr = &st->missing.items[i];
            attron(COLOR_PAIR(COL_WARN));
            mvprintw(row++, 4, "%-16s [%s] %s",
                     mr->app_name ? mr->app_name : "?",
                     mr->source ? mr->source : "?",
                     mr->group ? mr->group : "");
            attroff(COLOR_PAIR(COL_WARN));
        }
    }
}

/* --- dialogs --- */

static int confirm_dialog(int scr_h, int scr_w, const char *title, const char *msg) {
    int h = 7, w = 50;
    int y = (scr_h - h) / 2;
    int x = (scr_w - w) / 2;

    while (1) {
        attron(COLOR_PAIR(COL_BORDER));
        for (int i = 0; i < h; i++) {
            mvhline(y + i, x, ' ', w);
        }
        draw_box(y, x, h, w, title);
        attroff(COLOR_PAIR(COL_BORDER));

        mvprintw(y + 2, x + 3, "%.44s", msg);

        attron(COLOR_PAIR(COL_SELECT));
        mvprintw(y + 4, x + 10, " Yes ");
        attroff(COLOR_PAIR(COL_SELECT));
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + 4, x + 20, " No ");
        attroff(COLOR_PAIR(COL_DIM));

        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + 5, x + 3, "y/n or click");
        attroff(COLOR_PAIR(COL_DIM));

        refresh();

        int ch = getch();
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N' || ch == 27 || ch == 'q') return 0;
        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                int click = event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED);
                if (click && event.y == y + 4) {
                    if (event.x >= x + 10 && event.x < x + 16) return 1;
                    if (event.x >= x + 20 && event.x < x + 25) return 0;
                }
            }
        }
    }
}

/* --- file operations (use shared functions) --- */

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
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
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
/* FIX: use rule_copy() BEFORE modifying the rule to avoid use-after-free */

static int edit_rule_modal(struct rule *r, int rule_index, struct history_stack *history, int scr_h, int scr_w) {
    int base_h = 20;
    int extras_h = r->extras_count > 0 ? (int)r->extras_count + 2 : 0;
    int h = base_h + extras_h;
    if (h > scr_h - 4) h = scr_h - 4;
    int w = 60;
    int y = (scr_h - h) / 2;
    int x = (scr_w - w) / 2;

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
        attron(COLOR_PAIR(COL_BORDER));
        for (int i = 0; i < h; i++) {
            mvhline(y + i, x, ' ', w);
        }
        draw_box(y, x, h, w, "Edit Rule");
        attroff(COLOR_PAIR(COL_BORDER));

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
                if (field == F_DERIVED) attron(COLOR_PAIR(COL_ACCENT));
                else attron(COLOR_PAIR(COL_DIM));
                mvprintw(row, x + 2, "  -> %-38.38s", derived);
                if (field == F_DERIVED) {
                    mvprintw(row, x + 44, "[Enter to use]");
                    attroff(COLOR_PAIR(COL_ACCENT));
                } else {
                    attroff(COLOR_PAIR(COL_DIM));
                }
                row++;
                continue;
            }

            if (i == field) attron(COLOR_PAIR(COL_SELECT));
            else attron(COLOR_PAIR(COL_NORMAL));

            char label[16];
            snprintf(label, sizeof(label), "%s%s", changed[i] ? "*" : " ", labels[i]);
            mvprintw(row, x + 1, "%-12s", label);

            if (i == F_FLOAT) {
                mvprintw(row, x + 14, "[%c] %s", float_val ? 'x' : ' ', float_val ? "Yes" : "No");
            } else if (i == F_CENTER) {
                mvprintw(row, x + 14, "[%c] %s", center_val ? 'x' : ' ', center_val ? "Yes" : "No");
            } else {
                if (editing && i == field) {
                    mvprintw(row, x + 14, "%s_", bufs[i]);
                    clrtoeol();
                } else {
                    mvprintw(row, x + 14, "%-40.40s", bufs[i]);
                }
            }

            if (i == field) attroff(COLOR_PAIR(COL_SELECT));
            else attroff(COLOR_PAIR(COL_NORMAL));
            row++;
        }

        if (r->extras_count > 0) {
            row++;
            attron(COLOR_PAIR(COL_ACCENT));
            mvprintw(row++, x + 2, "Other properties:");
            attroff(COLOR_PAIR(COL_ACCENT));

            attron(COLOR_PAIR(COL_DIM));
            for (size_t i = 0; i < r->extras_count && row < y + h - 4; i++) {
                mvprintw(row++, x + 4, "%-12.12s = %.30s", r->extras[i].key, r->extras[i].value);
            }
            attroff(COLOR_PAIR(COL_DIM));
        }

        attron(COLOR_PAIR(COL_DIM));
        if (editing) {
            mvprintw(y + h - 3, x + 2, "Type to edit, Backspace to delete");
            mvprintw(y + h - 2, x + 2, "Enter:Done  Esc:Cancel edit");
        } else {
            mvprintw(y + h - 3, x + 2, "Up/Down:Select  Enter:Edit  Space:Toggle");
            mvprintw(y + h - 2, x + 2, "s:Save     q:Cancel");
        }
        attroff(COLOR_PAIR(COL_DIM));

        refresh();

        int ch = getch();

        if (editing && bufs[field]) {
            char *buf = bufs[field];
            size_t len = strlen(buf);
            if (ch == '\n' || ch == KEY_ENTER) {
                editing = 0;
                curs_set(0);
            } else if (ch == 27) {
                editing = 0;
                curs_set(0);
            } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (len > 0) buf[len - 1] = '\0';
            } else if (ch >= 32 && ch < 127 && len < 126) {
                buf[len] = (char)ch;
                buf[len + 1] = '\0';
            }
            continue;
        }

        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                int click = event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED);
                int dblclick = event.bstate & BUTTON1_DOUBLE_CLICKED;

                if ((click || dblclick) && event.x >= x && event.x < x + w) {
                    int clicked_row = event.y - (y + 2);
                    if (clicked_row >= 0 && clicked_row < F_COUNT) {
                        field = clicked_row;
                        if (field == F_DERIVED) {
                            snprintf(name_buf, sizeof(name_buf), "%s", derived);
                            field = F_NAME;
                        }
                        else if (field == F_FLOAT) float_val = !float_val;
                        else if (field == F_CENTER) center_val = !center_val;
                        else if (dblclick && bufs[field]) {
                            editing = 1;
                            curs_set(1);
                        }
                    }
                }
                if (event.bstate & BUTTON4_PRESSED && field > 0) field--;
                if (event.bstate & BUTTON5_PRESSED && field < F_COUNT - 1) field++;
            }
            continue;
        }

        if (ch == KEY_UP && field > 0) field--;
        else if (ch == KEY_DOWN && field < F_COUNT - 1) field++;
        else if (ch == '\n' || ch == KEY_ENTER) {
            if (field == F_DERIVED) {
                snprintf(name_buf, sizeof(name_buf), "%s", derived);
                field = F_NAME;
            }
            else if (field == F_FLOAT) { float_val = !float_val; }
            else if (field == F_CENTER) { center_val = !center_val; }
            else if (bufs[field]) { editing = 1; curs_set(1); }
        }
        else if (ch == ' ') {
            if (field == F_FLOAT) { float_val = !float_val; }
            else if (field == F_CENTER) { center_val = !center_val; }
        }
        else if (ch == 's' || ch == 'S') {
            /* FIX: deep copy BEFORE modifying rule fields to avoid use-after-free */
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

            /* record in history (old_state is a deep copy, safe to use) */
            char desc[128];
            snprintf(desc, sizeof(desc), "Edit rule %d", rule_index);
            history_record(history, rule_index, &old_state, r, desc);
            rule_free(&old_state);

            curs_set(0);
            return 1;
        }
        else if (ch == 'q' || ch == 'Q' || ch == 27) {
            curs_set(0);
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

static void search_init(struct search_state *s) {
    if (!s) return;
    memset(s, 0, sizeof(struct search_state));
    s->matches = malloc(1000 * sizeof(int));
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

    /* FIX: properly do case-insensitive search using str_to_lower_inplace */
    char lower_query[256];
    snprintf(lower_query, sizeof(lower_query), "%s", s->query);
    str_to_lower_inplace(lower_query);

    for (size_t i = 0; i < rs->count && s->match_count < 999; i++) {
        struct rule *r = &rs->rules[i];

        const char *name = r->display_name ? r->display_name : "";
        const char *class_re = r->match.class_re ? r->match.class_re : "";
        const char *title_re = r->match.title_re ? r->match.title_re : "";
        const char *tag = r->actions.tag ? r->actions.tag : "";
        const char *workspace = r->actions.workspace ? r->actions.workspace : "";

        /* case-insensitive for name */
        char name_lower[128];
        snprintf(name_lower, sizeof(name_lower), "%s", name);
        str_to_lower_inplace(name_lower);

        /* case-insensitive for class pattern */
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

static int search_modal(struct search_state *s, struct ruleset *rs, int scr_h, int scr_w) {
    int h = 7, w = 60;
    int y = (scr_h - h) / 2;
    int x = (scr_w - w) / 2;

    while (1) {
        clear();
        attron(COLOR_PAIR(COL_BORDER));
        for (int i = 0; i < h; i++) mvhline(y + i, x, ' ', w);
        draw_box(y, x, h, w, "Search Rules");
        attroff(COLOR_PAIR(COL_BORDER));

        mvprintw(y + 2, x + 2, "Query: ");
        attron(COLOR_PAIR(COL_SELECT));
        mvprintw(y + 2, x + 10, "%-46s_", s->query);
        attroff(COLOR_PAIR(COL_SELECT));

        if (s->match_count > 0) {
            mvprintw(y + 4, x + 2, "Found %zu matches (n/N to navigate)", s->match_count);
        } else {
            mvprintw(y + 4, x + 2, "No matches");
        }

        mvprintw(y + 5, x + 2, "Enter to jump, Esc to close");

        refresh();
        curs_set(1);

        int ch = getch();
        size_t len = strlen(s->query);

        if (ch == '\n' || ch == KEY_ENTER) {
            curs_set(0);
            if (s->match_count > 0) {
                return s->matches[s->current_match];
            }
            return -1;
        } else if (ch == 27) {
            curs_set(0);
            return -1;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) {
                s->query[len - 1] = '\0';
                search_update(s, rs);
            }
        } else if (ch == 'n' || ch == 'N') {
            if (ch == 'n') search_next(s);
            else search_prev(s);
        } else if (ch >= 32 && ch < 127 && len < sizeof(s->query) - 1) {
            s->query[len] = ch;
            s->query[len + 1] = '\0';
            search_update(s, rs);
        }
    }
}

/* --- splash/loading screens --- */

static void draw_loading(int height, int width, const char *msg) {
    clear();
    attron(COLOR_PAIR(COL_TITLE));
    mvprintw(height / 2, (width - (int)strlen(msg)) / 2, "%s", msg);
    attroff(COLOR_PAIR(COL_TITLE));
    refresh();
}

/* --- main draw/input dispatch --- */

static void draw_ui(ui_state_machine_t *sm) {
    struct ui_state *st = sm->st;
    int height, width;
    getmaxyx(stdscr, height, width);

    if (height < UI_MIN_HEIGHT || width < UI_MIN_WIDTH) {
        clear();
        mvprintw(height / 2, 0, "Resize to %dx%d", UI_MIN_WIDTH, UI_MIN_HEIGHT);
        refresh();
        return;
    }

    clear();

    if (st->modified) {
        draw_header(width, "hyprwindows [*]");
    } else {
        draw_header(width, "hyprwindows");
    }

    draw_tabs(1, width, sm->current_state);

    int content_y = 2;
    int content_h = height - 4;

    switch (sm->current_state) {
    case VIEW_RULES:
        if (width > 100) {
            int list_w = width * 2 / 3;
            draw_rules_view(st, content_y, content_h, list_w);
            draw_rule_detail(st, content_y, list_w, content_h, width - list_w);
        } else {
            draw_rules_view(st, content_y, content_h, width);
        }
        break;
    case VIEW_WINDOWS:
        draw_windows_view(st, content_y, content_h, width);
        break;
    case VIEW_REVIEW:
        draw_review_view(st, content_y, content_h, width);
        break;
    }

    const char *help;
    switch (sm->current_state) {
    case VIEW_RULES:
        help = "Enter:Edit  d:Del  x:Disable  /:Search  ^S:Save  ^Z/Y:Undo  q:Quit";
        break;
    default:
        help = "1-3:Views  Up/Down:Nav  ^S:Save  ^Z/Y:Undo  r:Reload  q:Quit";
        break;
    }
    draw_statusbar(height - 1, width, st->status, help);

    refresh();
}

static void handle_global_keys(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;
    int height, width;
    getmaxyx(stdscr, height, width);

    if (ch == 'q' || ch == 'Q') {
        if (st->modified) {
            int choice = 0;
            while (1) {
                int dh = 9, dw = 50;
                int dy = (height - dh) / 2;
                int dx = (width - dw) / 2;

                attron(COLOR_PAIR(COL_BORDER));
                for (int i = 0; i < dh; i++) mvhline(dy + i, dx, ' ', dw);
                draw_box(dy, dx, dh, dw, "Unsaved Changes");
                attroff(COLOR_PAIR(COL_BORDER));

                mvprintw(dy + 2, dx + 3, "You have unsaved changes.");
                mvprintw(dy + 3, dx + 3, "What would you like to do?");

                const char *opts[] = {"Save and quit", "Quit without saving", "Cancel"};
                for (int i = 0; i < 3; i++) {
                    if (i == choice) attron(COLOR_PAIR(COL_SELECT));
                    else attron(COLOR_PAIR(COL_DIM));
                    mvprintw(dy + 5 + i, dx + 5, " %s ", opts[i]);
                    if (i == choice) attroff(COLOR_PAIR(COL_SELECT));
                    else attroff(COLOR_PAIR(COL_DIM));
                }
                refresh();

                int c = getch();
                if (c == KEY_UP && choice > 0) choice--;
                else if (c == KEY_DOWN && choice < 2) choice++;
                else if (c == '\n' || c == KEY_ENTER) break;
                else if (c == 27) { choice = 2; break; }
                else if (c == 's' || c == 'S') { choice = 0; break; }
                else if (c == 'q') { choice = 1; break; }
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

    if (ch == 's' || ch == 'S') {
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
        return;
    }

    if (ch == 'b' || ch == 'B') {
        if (create_backup(st) == 0) {
            set_status(st, "Backup created: %s", st->backup_path);
        } else {
            set_status(st, "Failed to create backup");
        }
        return;
    }

    if (ch == '1') { sm->current_state = VIEW_RULES; st->selected = 0; st->scroll = 0; return; }
    if (ch == '2') { sm->current_state = VIEW_WINDOWS; st->scroll = 0; return; }
    if (ch == '3') { sm->current_state = VIEW_REVIEW; return; }

    if (ch == 'r' || ch == 'R') {
        if (st->modified) {
            if (!confirm_dialog(height, width, "Reload", "Discard unsaved changes?")) {
                return;
            }
        }
        draw_loading(height, width, "Reloading...");
        load_rules(st);
        return;
    }
}

static void handle_rules_input(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;
    int height, width;
    getmaxyx(stdscr, height, width);

    if (ch == KEY_UP && st->selected > 0) st->selected--;
    else if (ch == KEY_DOWN && st->selected < (int)st->rules.count - 1) st->selected++;
    else if (ch == KEY_PPAGE) { st->selected -= 10; if (st->selected < 0) st->selected = 0; }
    else if (ch == KEY_NPAGE) { st->selected += 10; if (st->selected >= (int)st->rules.count) st->selected = (int)st->rules.count - 1; }
    else if (ch == KEY_HOME) st->selected = 0;
    else if (ch == KEY_END) st->selected = (int)st->rules.count - 1;
    else if (ch == '/') {
        struct search_state search;
        search_init(&search);
        int result = search_modal(&search, &st->rules, height, width);
        if (result >= 0) {
            st->selected = result;
        }
        search_free(&search);
    }
    else if ((ch == '\n' || ch == KEY_ENTER) && st->selected >= 0 && st->selected < (int)st->rules.count) {
        if (edit_rule_modal(&st->rules.rules[st->selected], st->selected, &st->history, height, width)) {
            st->modified = 1;
            set_status(st, "Rule modified (not saved to file)");
        }
    }
    /* delete rule */
    else if ((ch == 'd' || ch == KEY_DC) && st->selected >= 0 && st->selected < (int)st->rules.count) {
        struct rule *r = &st->rules.rules[st->selected];
        char msg[64];
        snprintf(msg, sizeof(msg), "Delete rule '%s'?", r->name ? r->name : "(unnamed)");
        if (confirm_dialog(height, width, "Delete Rule", msg)) {
            /* FIX: deep copy before array manipulation */
            struct rule deleted_copy = rule_copy(r);
            char desc[128];
            snprintf(desc, sizeof(desc), "Delete rule %d", st->selected);
            history_record(&st->history, st->selected, &deleted_copy, NULL, desc);
            rule_free(&deleted_copy);

            /* remove from array (shifts remaining rules) */
            rule_free(r);
            for (int i = st->selected; i < (int)st->rules.count - 1; i++) {
                st->rules.rules[i] = st->rules.rules[i + 1];
                if (st->rule_status) st->rule_status[i] = st->rule_status[i + 1];
            }
            st->rules.count--;
            if (st->selected >= (int)st->rules.count && st->selected > 0) st->selected--;
            st->modified = 1;
            set_status(st, "Rule deleted (not saved to file)");
        }
    }
    /* disable rule */
    else if (ch == 'x' && st->selected >= 0 && st->selected < (int)st->rules.count) {
        struct rule *r = &st->rules.rules[st->selected];
        char msg[64];
        snprintf(msg, sizeof(msg), "Disable rule '%s'?", r->name ? r->name : "(unnamed)");
        if (confirm_dialog(height, width, "Disable Rule", msg)) {
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
                /* FIX: deep copy before array manipulation */
                struct rule disabled_copy = rule_copy(r);
                char desc[128];
                snprintf(desc, sizeof(desc), "Disable rule %d", st->selected);
                history_record(&st->history, st->selected, &disabled_copy, NULL, desc);
                rule_free(&disabled_copy);

                rule_free(r);
                for (int i = st->selected; i < (int)st->rules.count - 1; i++) {
                    st->rules.rules[i] = st->rules.rules[i + 1];
                    if (st->rule_status) st->rule_status[i] = st->rule_status[i + 1];
                }
                st->rules.count--;
                if (st->selected >= (int)st->rules.count && st->selected > 0) st->selected--;
                st->modified = 1;
                set_status(st, "Rule disabled -> %s", disabled_path);
            } else {
                set_status(st, "Failed to write to %s", disabled_path);
            }
        }
    }
    /* Ctrl+S save */
    else if (ch == 19) {
        if (st->modified) {
            if (!st->backup_created) create_backup(st);
            if (save_rules(st) == 0) {
                set_status(st, "Rules saved to %s", st->rules_path);
            } else {
                set_status(st, "Failed to save rules");
            }
        } else {
            set_status(st, "No changes to save");
        }
    }
    /* Ctrl+Z undo */
    else if (ch == 26) {
        if (history_can_undo(&st->history)) {
            int rule_index = -1;
            struct rule *old_rule = history_undo(&st->history, &rule_index);
            if (old_rule && rule_index >= 0 && rule_index < (int)st->rules.count) {
                rule_free(&st->rules.rules[rule_index]);
                st->rules.rules[rule_index] = *old_rule;
                st->selected = rule_index;
                st->modified = 1;
                set_status(st, "Undo complete");
                free(old_rule);
            }
        } else {
            set_status(st, "Nothing to undo");
        }
    }
    /* Ctrl+Y redo */
    else if (ch == 25) {
        if (history_can_redo(&st->history)) {
            int rule_index = -1;
            struct rule *new_rule = history_redo(&st->history, &rule_index);
            if (new_rule && rule_index >= 0 && rule_index < (int)st->rules.count) {
                rule_free(&st->rules.rules[rule_index]);
                st->rules.rules[rule_index] = *new_rule;
                st->selected = rule_index;
                st->modified = 1;
                set_status(st, "Redo complete");
                free(new_rule);
            }
        } else {
            set_status(st, "Nothing to redo");
        }
    }
}

static void handle_windows_input(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;
    if (ch == KEY_UP || ch == KEY_PPAGE) { st->scroll -= (ch == KEY_PPAGE ? 10 : 1); if (st->scroll < 0) st->scroll = 0; }
    if (ch == KEY_DOWN || ch == KEY_NPAGE) st->scroll += (ch == KEY_NPAGE ? 10 : 1);
}

static void handle_review_input(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;
    if (ch == KEY_UP || ch == KEY_PPAGE) { st->scroll -= (ch == KEY_PPAGE ? 10 : 1); if (st->scroll < 0) st->scroll = 0; }
    if (ch == KEY_DOWN || ch == KEY_NPAGE) st->scroll += (ch == KEY_NPAGE ? 10 : 1);
}


static void handle_input(ui_state_machine_t *sm, int ch) {
    handle_global_keys(sm, ch);

    if (!sm->running) return;

    switch (sm->current_state) {
    case VIEW_RULES:
        handle_rules_input(sm, ch);
        break;
    case VIEW_WINDOWS:
        handle_windows_input(sm, ch);
        break;
    case VIEW_REVIEW:
        handle_review_input(sm, ch);
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
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();

    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0);

    init_pair(COL_TITLE, COLOR_CYAN, -1);
    init_pair(COL_BORDER, COLOR_CYAN, -1);
    init_pair(COL_STATUS, COLOR_WHITE, COLOR_BLUE);
    init_pair(COL_SELECT, COLOR_BLACK, COLOR_CYAN);
    init_pair(COL_NORMAL, COLOR_WHITE, -1);
    init_pair(COL_DIM, COLOR_BLUE, -1);
    init_pair(COL_ACCENT, COLOR_YELLOW, -1);
    init_pair(COL_WARN, COLOR_YELLOW, -1);
    init_pair(COL_ERROR, COLOR_RED, -1);

    int height, width;
    getmaxyx(stdscr, height, width);

    draw_loading(height, width, "Loading rules...");
    load_rules(&st);

    ui_state_machine_t sm;
    sm.current_state = VIEW_RULES;
    sm.running = 1;
    sm.st = &st;

    while (sm.running) {
        getmaxyx(stdscr, height, width);
        draw_ui(&sm);

        int ch = getch();
        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                int click = event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED);
                int dblclick = event.bstate & BUTTON1_DOUBLE_CLICKED;

                if (sm.current_state == VIEW_RULES && dblclick && event.y > 3) {
                    int content_y = 2;
                    int list_row = event.y - content_y - 2;
                    if (list_row >= 0) {
                        int clicked_idx = st.scroll + list_row;
                        if (clicked_idx >= 0 && clicked_idx < (int)st.rules.count) {
                            st.selected = clicked_idx;
                            if (edit_rule_modal(&st.rules.rules[st.selected], st.selected, &st.history, height, width)) {
                                st.modified = 1;
                                set_status(&st, "Rule modified (not saved to file)");
                            }
                        }
                    }
                }
                else if (event.y == 1 && click) {
                    if (event.x >= 2 && event.x < 14) {
                        sm.current_state = VIEW_RULES; st.selected = 0; st.scroll = 0;
                    } else if (event.x >= 15 && event.x < 29) {
                        sm.current_state = VIEW_WINDOWS; st.scroll = 0;
                    } else if (event.x >= 30 && event.x < 43) {
                        sm.current_state = VIEW_REVIEW;
                    }
                }
                else if (event.bstate & BUTTON4_PRESSED) {
                    if (sm.current_state == VIEW_RULES && st.selected > 0) st.selected--;
                    else if (sm.current_state == VIEW_WINDOWS && st.scroll > 0) st.scroll--;
                }
                else if (event.bstate & BUTTON5_PRESSED) {
                    if (sm.current_state == VIEW_RULES && st.selected < (int)st.rules.count - 1) st.selected++;
                    else if (sm.current_state == VIEW_WINDOWS) st.scroll++;
                }
                else if (sm.current_state == VIEW_RULES && click && event.y > 3) {
                    int content_y = 2;
                    int list_row = event.y - content_y - 2;
                    if (list_row >= 0 && event.x > 0 && event.x < width * 2 / 3) {
                        int clicked_idx = st.scroll + list_row;
                        if (clicked_idx >= 0 && clicked_idx < (int)st.rules.count) {
                            st.selected = clicked_idx;
                        }
                    }
                }
            }
            continue;
        }

        handle_input(&sm, ch);
    }

    endwin();
    ruleset_free(&st.rules);
    free(st.rule_status);
    missing_rules_free(&st.missing);
    free(st.review_text);

    history_free(&st.history);

    return 0;
}
