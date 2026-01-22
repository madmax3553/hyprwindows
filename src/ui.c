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
#include "naming.h"
#include "cascade.h"
#include "analysis.h"
#include "history.h"
#include "export_rules.h"

#define UI_MIN_WIDTH 80
#define UI_MIN_HEIGHT 24

/* views */
enum view_mode {
    VIEW_RULES,
    VIEW_WINDOWS,
    VIEW_REVIEW,
    VIEW_SETTINGS,
};

/* rule status flags */
enum rule_status {
    RULE_OK = 0,
    RULE_UNUSED = 1,      /* no matching window */
    RULE_DUPLICATE = 2,   /* same pattern as another rule */
};

struct ui_state_machine;
typedef struct ui_state_machine ui_state_machine_t;

struct ui_state {
    enum view_mode mode;
    int selected;
    int scroll;

    /* loaded data */
    struct ruleset rules;
    enum rule_status *rule_status;  /* status for each rule */
    char rules_path[512];
    char dotfiles_path[512];
    char appmap_path[512];

    /* cached review data */
    struct missing_rules missing;
    char *review_text;
    int review_loaded;

    /* dirty tracking */
    int modified;           /* rules have been modified */
    int backup_created;     /* backup file was created this session */
    char backup_path[512];  /* path to backup file */

    /* options */
    int suggest_rules;
    int show_overlaps;

    /* analysis and cascade caching */
    struct analysis_report *analysis_report;
    int analysis_loaded;
    struct cascade_analysis **cascade_cache;  /* array of cascade analysis per window */
    size_t cascade_count;
    int cascade_loaded;

    /* undo/redo history */
    struct history_stack history;
    enum grouping_mode grouping;

    /* status message */
    char status[256];
};

struct ui_state_machine {
    enum view_mode current_state;
    int running;
    struct ui_state *st;

    void (*handle_rules_input)(ui_state_machine_t*, int);
    void (*handle_windows_input)(ui_state_machine_t*, int);
    void (*handle_review_input)(ui_state_machine_t*, int);
    void (*handle_settings_input)(ui_state_machine_t*, int);
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
static void handle_settings_input(ui_state_machine_t *sm, int ch);

static void set_status(struct ui_state *st, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(st->status, sizeof(st->status), fmt, args);
    va_end(args);
}

static char *expand_path(const char *path) {
    if (!path || path[0] != '~') {
        return path ? strdup(path) : NULL;
    }
    const char *home = getenv("HOME");
    if (!home) home = ".";
    size_t len = strlen(home) + strlen(path);
    char *out = malloc(len);
    if (out) {
        strcpy(out, home);
        strcat(out, path + 1);
    }
    return out;
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

/* check if two rules have the same match pattern */
static int rules_duplicate(const struct rule *a, const struct rule *b) {
    /* same class pattern */
    if (a->match.class_re && b->match.class_re) {
        if (strcmp(a->match.class_re, b->match.class_re) == 0) return 1;
    }
    /* same title pattern */
    if (a->match.title_re && b->match.title_re) {
        if (strcmp(a->match.title_re, b->match.title_re) == 0) return 1;
    }
    return 0;
}

static void compute_rule_status(struct ui_state *st) {
    free(st->rule_status);
    st->rule_status = calloc(st->rules.count, sizeof(enum rule_status));
    if (!st->rule_status) return;

    /* get active windows */
    struct clients clients = {0};
    int have_clients = (hyprctl_clients(&clients) == 0);

    for (size_t i = 0; i < st->rules.count; i++) {
        struct rule *r = &st->rules.rules[i];

        /* check for duplicates */
        for (size_t j = 0; j < st->rules.count; j++) {
            if (i != j && rules_duplicate(r, &st->rules.rules[j])) {
                st->rule_status[i] = RULE_DUPLICATE;
                break;
            }
        }

        /* check if unused (no matching window) */
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
    /* free old data */
    missing_rules_free(&st->missing);
    free(st->review_text);
    st->review_text = NULL;
    st->review_loaded = 0;

    /* load review text */
    struct outbuf out;
    outbuf_init(&out);
    char *path = expand_path(st->rules_path);
    review_rules_text(path ? path : st->rules_path, &out);

    /* load missing rules */
    char *appmap_path = expand_path(st->appmap_path);
    find_missing_rules(path ? path : st->rules_path,
                       appmap_path ? appmap_path : st->appmap_path,
                       st->dotfiles_path, &st->missing);
    free(appmap_path);
    free(path);

    st->review_text = out.data;  /* take ownership */
    out.data = NULL;
    st->review_loaded = 1;
}

/* Load and cache analysis report */
static void load_analysis_data(struct ui_state *st) {
    if (st->analysis_report) {
        analysis_free(st->analysis_report);
        st->analysis_report = NULL;
    }
    st->analysis_loaded = 0;
    
    /* Run analysis on current ruleset */
    st->analysis_report = analysis_run(&st->rules, NULL);
    st->analysis_loaded = 1;
}

static void load_rules(struct ui_state *st) {
    ruleset_free(&st->rules);
    free(st->rule_status);
    st->rule_status = NULL;
    st->review_loaded = 0;  /* invalidate review cache */
    st->analysis_loaded = 0;  /* invalidate analysis cache */
    
    /* free old analysis */
    if (st->analysis_report) {
        analysis_free(st->analysis_report);
        st->analysis_report = NULL;
    }

    char *path = expand_path(st->rules_path);
    if (ruleset_load(path ? path : st->rules_path, &st->rules) == 0) {
        /* compute display names */
        for (size_t i = 0; i < st->rules.count; i++) {
            update_display_name(&st->rules.rules[i]);
        }
        /* sort by tag for visual grouping */
        if (st->rules.count > 1) {
            qsort(st->rules.rules, st->rules.count, sizeof(struct rule), compare_rules_by_tag);
        }
        /* compute status for each rule */
        compute_rule_status(st);
        set_status(st, "Loaded %zu rules from %s", st->rules.count, st->rules_path);
    } else {
        set_status(st, "Failed to load rules from %s", st->rules_path);
    }
    free(path);
}

/* drawing helpers */
static void draw_box(int y, int x, int h, int w, const char *title) {
    attron(COLOR_PAIR(COL_BORDER));
    mvprintw(y, x, "┌");
    mvprintw(y, x + w - 1, "┐");
    mvprintw(y + h - 1, x, "└");
    mvprintw(y + h - 1, x + w - 1, "┘");
    for (int i = 1; i < w - 1; i++) {
        mvprintw(y, x + i, "─");
        mvprintw(y + h - 1, x + i, "─");
    }
    for (int i = 1; i < h - 1; i++) {
        mvprintw(y + i, x, "│");
        mvprintw(y + i, x + w - 1, "│");
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
    const char *tabs[] = {"[1] Rules", "[2] Windows", "[3] Review", "[4] Settings"};
    int x = 2;
    for (int i = 0; i < 4; i++) {
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

/* helper to extract readable class name from regex */
static void clean_class_name(const char *regex, char *out, size_t out_sz) {
    if (!regex || !out || out_sz == 0) {
        if (out && out_sz > 0) out[0] = '\0';
        return;
    }

    const char *p = regex;
    size_t o = 0;

    /* skip leading ^ */
    if (*p == '^') p++;
    /* skip leading ( */
    if (*p == '(') p++;
    /* skip [Xx] case patterns like [Gg] */
    if (*p == '[' && p[1] && p[2] == ']') {
        out[o++] = (p[1] >= 'a' && p[1] <= 'z') ? p[1] - 32 : p[1];
        p += 3;
    }

    while (*p && o < out_sz - 1) {
        /* stop at regex special chars that end the name */
        if (*p == '$' || *p == ')' || *p == '|') break;
        /* handle character classes like [.-] - just take first char */
        if (*p == '[') {
            if (p[1] && p[1] != ']') out[o++] = p[1];
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
            continue;
        }
        /* skip regex quantifiers and special sequences */
        if (*p == '+' || *p == '*' || *p == '?') { p++; continue; }
        if (*p == '.' && p[1] == '+') { p += 2; continue; }  /* .+ wildcard */
        if (*p == '\\' && p[1] == 'd') { p += 2; continue; } /* \d digit */
        /* unescape \. to . */
        if (*p == '\\' && p[1]) {
            p++;
            out[o++] = *p++;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';

    /* if empty, try to extract something useful */
    if (out[0] == '\0' && regex[0]) {
        /* just copy first word-like chars */
        p = regex;
        o = 0;
        while (*p && o < out_sz - 1) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') {
                out[o++] = *p;
            } else if (o > 0) {
                break;  /* stop at first non-word char after we have something */
            }
            p++;
        }
        out[o] = '\0';
    }

    /* capitalize first letter */
    if (out[0] >= 'a' && out[0] <= 'z') {
        out[0] -= 32;
    }

    /* leave empty if nothing extracted - caller handles fallback */
}

/* update rule's display_name from its match patterns */
static void update_display_name(struct rule *r) {
    char buf[64] = "";

    /* try class regex first */
    clean_class_name(r->match.class_re, buf, sizeof(buf));

    /* fall back to title regex */
    if (!buf[0]) {
        clean_class_name(r->match.title_re, buf, sizeof(buf));
    }

    /* fall back to rule name */
    if (!buf[0] && r->name) {
        snprintf(buf, sizeof(buf), "%s", r->name);
    }

    /* final fallback */
    if (!buf[0]) {
        snprintf(buf, sizeof(buf), "(unnamed)");
    }

    free(r->display_name);
    r->display_name = strdup(buf);
}

/* helper to clean tag (remove + prefix) */
static const char *clean_tag(const char *tag) {
    if (!tag) return "-";
    if (tag[0] == '+') return tag + 1;
    return tag;
}

/* helper to free a rule's allocated strings */
static void cleanup_rule(struct rule *r) {
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
    free(r->actions.opacity);
    free(r->actions.size);
    free(r->actions.move);
    for (size_t i = 0; i < r->extras_count; i++) {
        free(r->extras[i].key);
        free(r->extras[i].value);
    }
    free(r->extras);
}

/* rule list view */
static void draw_rules_view(struct ui_state *st, int y, int h, int w) {
    draw_box(y, 0, h, w, "Window Rules");

    if (st->rules.count == 0) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 20) / 2, "No rules loaded");
        attroff(COLOR_PAIR(COL_DIM));
        return;
    }

    int visible = h - 3;  /* leave room for header */
    int max_scroll = (int)st->rules.count > visible ? (int)st->rules.count - visible : 0;
    if (st->scroll > max_scroll) st->scroll = max_scroll;
    if (st->scroll < 0) st->scroll = 0;

    /* ensure selected is visible */
    if (st->selected < st->scroll) st->scroll = st->selected;
    if (st->selected >= st->scroll + visible) st->scroll = st->selected - visible + 1;

     /* column header with grouping info */
     attron(COLOR_PAIR(COL_DIM));
     const char *group_label = "";
     switch (st->grouping) {
     case GROUP_BY_WORKSPACE: group_label = "[Grouped by Workspace]"; break;
     case GROUP_BY_TAG: group_label = "[Grouped by Tag]"; break;
     case GROUP_BY_FLOAT: group_label = "[Grouped by Float]"; break;
     default: group_label = "[Ungrouped]"; break;
     }
     mvprintw(y + 1, 3, "%-16s %-12s %-6s %-8s %s %s", "Application", "Tag", "WS", "Status", "Options", group_label);
     attroff(COLOR_PAIR(COL_DIM));

     const char *last_tag = NULL;

     for (int i = 0; i < visible && (st->scroll + i) < (int)st->rules.count; i++) {
         int idx = st->scroll + i;
         struct rule *r = &st->rules.rules[idx];
         int row = y + 2 + i;

         /* get status */
         enum rule_status status = st->rule_status ? st->rule_status[idx] : RULE_OK;

         /* use pre-computed display name */
         const char *display = r->display_name ? r->display_name : "(unnamed)";
         const char *tag = clean_tag(r->actions.tag);
         const char *ws = r->actions.workspace ? r->actions.workspace : "-";

        /* build options string - include extras count if any */
        char opts[32] = "";
        if (r->actions.float_set && r->actions.float_val) strcat(opts, "F ");
        if (r->actions.center_set && r->actions.center_val) strcat(opts, "C ");
        if (r->actions.size) strcat(opts, "S ");
        if (r->actions.opacity) strcat(opts, "O ");
        if (r->extras_count > 0) {
            char extra_buf[24];
            snprintf(extra_buf, sizeof(extra_buf), "+%zu", r->extras_count);
            strcat(opts, extra_buf);
        }
        if (opts[0] == '\0') strcpy(opts, "-");

        /* show tag separator if tag changed */
        int show_tag = 1;
        if (last_tag && r->actions.tag && strcmp(last_tag, r->actions.tag) == 0) {
            show_tag = 0;
        }
        last_tag = r->actions.tag;

        if (idx == st->selected) {
            attron(COLOR_PAIR(COL_SELECT));
            mvhline(row, 1, ' ', w - 2);
        }

        /* format row */
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

        /* status column */
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

    /* footer hint */
    attron(COLOR_PAIR(COL_DIM));
    mvprintw(y + h - 1, 3, " Enter: Edit  /: Search  n: New ");
    attroff(COLOR_PAIR(COL_DIM));
}

/* rule detail panel */
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

    /* use pre-computed display name */
    const char *display = r->display_name ? r->display_name : "(unnamed)";

    attron(A_BOLD | COLOR_PAIR(COL_ACCENT));
    mvprintw(row++, col, "%s", display);
    attroff(A_BOLD | COLOR_PAIR(COL_ACCENT));

    row++;

    /* matching */
    attron(COLOR_PAIR(COL_DIM));
    mvprintw(row++, col, "Matching");
    attroff(COLOR_PAIR(COL_DIM));

    if (r->match.class_re) {
        mvprintw(row++, col + 2, "Class:  %.*s", w - 12, r->match.class_re);
    }
    if (r->match.title_re) {
        mvprintw(row++, col + 2, "Title:  %.*s", w - 12, r->match.title_re);
    }

    row++;

    /* actions */
    attron(COLOR_PAIR(COL_DIM));
    mvprintw(row++, col, "Actions");
    attroff(COLOR_PAIR(COL_DIM));

    if (r->actions.tag) {
        mvprintw(row++, col + 2, "Tag:       %s", clean_tag(r->actions.tag));
    }
    if (r->actions.workspace) {
        mvprintw(row++, col + 2, "Workspace: %s", r->actions.workspace);
    }
    if (r->actions.float_set) {
        mvprintw(row++, col + 2, "Float:     %s", r->actions.float_val ? "Yes" : "No");
    }
    if (r->actions.center_set) {
        mvprintw(row++, col + 2, "Center:    %s", r->actions.center_val ? "Yes" : "No");
    }
    if (r->actions.size) {
        mvprintw(row++, col + 2, "Size:      %s", r->actions.size);
    }
    if (r->actions.move) {
        mvprintw(row++, col + 2, "Position:  %s", r->actions.move);
    }
    if (r->actions.opacity) {
        mvprintw(row++, col + 2, "Opacity:   %s", r->actions.opacity);
    }

    /* show extras if any */
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

    /* hint at bottom */
    attron(COLOR_PAIR(COL_DIM));
    mvprintw(y + h - 2, col, "Press Enter to edit");
    attroff(COLOR_PAIR(COL_DIM));
}

/* active windows view */
static void draw_windows_view(struct ui_state *st, int y, int h, int w) {
    draw_box(y, 0, h, w, "Active Windows");

    /* get active clients */
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

    /* render output */
    int row = y + 1;
    int max_row = y + h - 1;

    for (size_t i = 0; i < clients.count && row < max_row - 2; i++) {
        struct client *c = &clients.items[i];
        
        /* window header */
        attron(A_BOLD | COLOR_PAIR(COL_ACCENT));
        mvprintw(row++, 2, "Window: %s", c->class_name ? c->class_name : "<unknown>");
        attroff(A_BOLD | COLOR_PAIR(COL_ACCENT));
        
        if (row >= max_row - 2) break;
        
        /* window details */
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
        
        /* cascade analysis */
        struct cascade_analysis *cascade = cascade_analyze(&st->rules, c);
        if (cascade) {
            if (cascade->step_count == 0) {
                attron(COLOR_PAIR(COL_DIM));
                mvprintw(row++, 4, "Matches: (none)");
                attroff(COLOR_PAIR(COL_DIM));
            } else {
                attron(A_BOLD | COLOR_PAIR(COL_WARN));
                mvprintw(row++, 4, "Cascade Analysis:");
                attroff(A_BOLD | COLOR_PAIR(COL_WARN));
                
                for (size_t j = 0; j < cascade->step_count && row < max_row - 2; j++) {
                    struct cascade_step *step = &cascade->steps[j];
                    
                    if (j < st->rules.count) {
                        const char *rule_name = st->rules.rules[step->rule_index].name;
                        mvprintw(row++, 6, "[%d] %s", step->rule_index,
                                 rule_name ? rule_name : "<unnamed>");
                    }
                }
            }
            cascade_free(cascade);
        }
        
        if (row < max_row - 1) {
            row++;  /* spacing between windows */
        }
    }

    clients_free(&clients);
}

/* settings view */
static void draw_settings_view(struct ui_state *st, int y, int h, int w) {
    draw_box(y, 0, h, w, "Settings");

    const char *labels[] = {"Rules path:", "Dotfiles:", "Appmap:", "Suggest rules:", "Show overlaps:"};
    const char *values[] = {st->rules_path, st->dotfiles_path, st->appmap_path, NULL, NULL};

    int row = y + 2;
    for (int i = 0; i < 5; i++) {
        if (i == st->selected) {
            attron(COLOR_PAIR(COL_SELECT));
            mvhline(row, 2, ' ', w - 4);
        }

        mvprintw(row, 4, "%-16s", labels[i]);

        if (i < 3) {
            mvprintw(row, 22, "%.*s", w - 26, values[i]);
        } else if (i == 3) {
            mvprintw(row, 22, "[%c]", st->suggest_rules ? 'x' : ' ');
        } else {
            mvprintw(row, 22, "[%c]", st->show_overlaps ? 'x' : ' ');
        }

        if (i == st->selected) {
            attroff(COLOR_PAIR(COL_SELECT));
        }
        row += 2;
    }
}

/* review view - uses cached data */
static void draw_review_view(struct ui_state *st, int y, int h, int w) {
    draw_box(y, 0, h, w, "Rules Review");

    /* load data if not cached */
    if (!st->review_loaded) {
        /* show loading message */
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 15) / 2, "Loading...");
        attroff(COLOR_PAIR(COL_DIM));
        refresh();
        load_review_data(st);
    }

    /* load analysis if not cached */
    if (!st->analysis_loaded) {
        load_analysis_data(st);
    }

    if (!st->review_text && st->missing.count == 0 && (!st->analysis_report || st->analysis_report->count == 0)) {
        attron(COLOR_PAIR(COL_DIM));
        mvprintw(y + h / 2, (w - 20) / 2, "Review unavailable");
        attroff(COLOR_PAIR(COL_DIM));
        return;
    }

    /* render output */
    int row = y + 1;
    int max_row = y + h - 1;

    /* show analysis results first */
    if (st->analysis_report && st->analysis_report->count > 0) {
        attron(A_BOLD | COLOR_PAIR(COL_ACCENT));
        mvprintw(row++, 2, "=== Conflict Analysis ===");
        attroff(A_BOLD | COLOR_PAIR(COL_ACCENT));
        
        if (st->analysis_report->errors > 0) {
            attron(A_BOLD | COLOR_PAIR(COL_ERROR));
            mvprintw(row++, 2, "Errors: %zu", st->analysis_report->errors);
            attroff(A_BOLD | COLOR_PAIR(COL_ERROR));
        }
        if (st->analysis_report->warnings > 0) {
            attron(A_BOLD | COLOR_PAIR(COL_WARN));
            mvprintw(row++, 2, "Warnings: %zu", st->analysis_report->warnings);
            attroff(A_BOLD | COLOR_PAIR(COL_WARN));
        }
        if (st->analysis_report->infos > 0) {
            attron(A_BOLD | COLOR_PAIR(COL_DIM));
            mvprintw(row++, 2, "Info: %zu", st->analysis_report->infos);
            attroff(A_BOLD | COLOR_PAIR(COL_DIM));
        }
        
        row++;
        
        /* show each issue */
        for (size_t i = 0; i < st->analysis_report->count && row < max_row - 5; i++) {
            struct rule_issue *issue = &st->analysis_report->issues[i];
            
            /* color by severity */
            int color = COL_WARN;
            if (issue->severity == SEVERITY_ERROR) color = COL_ERROR;
            else if (issue->severity == SEVERITY_INFO) color = COL_DIM;
            
            attron(COLOR_PAIR(color));
            mvprintw(row++, 4, "[%s] %s", 
                     analysis_severity_string(issue->severity),
                     issue->description);
            attroff(COLOR_PAIR(color));
            
            /* show affected rule indices */
            if (issue->affected_count > 0) {
                char rule_str[128] = "";
                for (size_t j = 0; j < issue->affected_count && j < 5; j++) {
                    snprintf(rule_str + strlen(rule_str), sizeof(rule_str) - strlen(rule_str),
                             "%d%s", issue->affected_rules[j], j < issue->affected_count - 1 ? ", " : "");
                }
                attron(COLOR_PAIR(COL_DIM));
                mvprintw(row++, 6, "Rules: %s", rule_str);
                attroff(COLOR_PAIR(COL_DIM));
            }
            
            if (issue->suggestion[0]) {
                mvprintw(row++, 6, "Suggestion: %.40s", issue->suggestion);
            }
            row++;
        }
    }

    if (st->review_text) {
        const char *p = st->review_text;
        while (*p && row < max_row - (int)st->missing.count - 3) {
            const char *end = strchr(p, '\n');
            int len = end ? (int)(end - p) : (int)strlen(p);

            /* highlight headers */
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

    /* show missing rules */
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

/* simple yes/no confirmation dialog */
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

        /* clickable buttons */
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
                    if (event.x >= x + 10 && event.x < x + 16) return 1;  /* Yes */
                    if (event.x >= x + 20 && event.x < x + 25) return 0;  /* No */
                }
            }
        }
    }
}

/* write a rule to file in hyprland format */
static int write_rule_to_file(const char *path, const struct rule *r, const char *mode) {
    FILE *f = fopen(path, mode);
    if (!f) return -1;

    fprintf(f, "windowrule {\n");
    if (r->name) fprintf(f, "    name = %s\n", r->name);
    if (r->match.class_re) fprintf(f, "    match:class = %s\n", r->match.class_re);
    if (r->match.title_re) fprintf(f, "    match:title = %s\n", r->match.title_re);
    if (r->match.initial_class_re) fprintf(f, "    match:initial_class = %s\n", r->match.initial_class_re);
    if (r->match.initial_title_re) fprintf(f, "    match:initial_title = %s\n", r->match.initial_title_re);
    if (r->match.tag_re) fprintf(f, "    match:tag = %s\n", r->match.tag_re);
    if (r->actions.tag) fprintf(f, "    tag = %s\n", r->actions.tag);
    if (r->actions.workspace) fprintf(f, "    workspace = %s\n", r->actions.workspace);
    if (r->actions.float_set) fprintf(f, "    float = %s\n", r->actions.float_val ? "true" : "false");
    if (r->actions.center_set) fprintf(f, "    center = %s\n", r->actions.center_val ? "true" : "false");
    if (r->actions.size) fprintf(f, "    size = %s\n", r->actions.size);
    if (r->actions.move) fprintf(f, "    move = %s\n", r->actions.move);
    if (r->actions.opacity) fprintf(f, "    opacity = %s\n", r->actions.opacity);
    /* write extras */
    for (size_t i = 0; i < r->extras_count; i++) {
        fprintf(f, "    %s = %s\n", r->extras[i].key, r->extras[i].value);
    }
    fprintf(f, "}\n\n");

    fclose(f);
    return 0;
}

/* create backup of rules file */
static int create_backup(struct ui_state *st) {
    char *expanded = expand_path(st->rules_path);
    const char *src = expanded ? expanded : st->rules_path;

    /* generate backup path with timestamp */
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

    /* copy file */
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
        fwrite(buf, 1, n, out);
    }

    fclose(in);
    fclose(out);
    free(expanded);

    st->backup_created = 1;
    return 0;
}

/* save all rules to file */
static int save_rules(struct ui_state *st) {
    char *expanded = expand_path(st->rules_path);
    const char *path = expanded ? expanded : st->rules_path;

    FILE *f = fopen(path, "w");
    if (!f) {
        free(expanded);
        return -1;
    }

    /* write header comment */
    fprintf(f, "# Window Rules - managed by hyprwindows\n");
    fprintf(f, "# See https://wiki.hyprland.org/Configuring/Window-Rules/\n\n");

    /* write each rule */
    for (size_t i = 0; i < st->rules.count; i++) {
        struct rule *r = &st->rules.rules[i];

        fprintf(f, "windowrule {\n");
        if (r->name) fprintf(f, "    name = %s\n", r->name);
        if (r->match.class_re) fprintf(f, "    match:class = %s\n", r->match.class_re);
        if (r->match.title_re) fprintf(f, "    match:title = %s\n", r->match.title_re);
        if (r->match.initial_class_re) fprintf(f, "    match:initial_class = %s\n", r->match.initial_class_re);
        if (r->match.initial_title_re) fprintf(f, "    match:initial_title = %s\n", r->match.initial_title_re);
        if (r->match.tag_re) fprintf(f, "    match:tag = %s\n", r->match.tag_re);
        if (r->actions.tag) fprintf(f, "    tag = %s\n", r->actions.tag);
        if (r->actions.workspace) fprintf(f, "    workspace = %s\n", r->actions.workspace);
        if (r->actions.float_set) fprintf(f, "    float = %s\n", r->actions.float_val ? "true" : "false");
        if (r->actions.center_set) fprintf(f, "    center = %s\n", r->actions.center_val ? "true" : "false");
        if (r->actions.size) fprintf(f, "    size = %s\n", r->actions.size);
        if (r->actions.move) fprintf(f, "    move = %s\n", r->actions.move);
        if (r->actions.opacity) fprintf(f, "    opacity = %s\n", r->actions.opacity);
        for (size_t j = 0; j < r->extras_count; j++) {
            fprintf(f, "    %s = %s\n", r->extras[j].key, r->extras[j].value);
        }
        fprintf(f, "}\n\n");
    }

    fclose(f);
    free(expanded);

    st->modified = 0;
    return 0;
}

/* get disabled rules file path (same dir as rules, with .disabled suffix) */
static void get_disabled_path(const char *rules_path, char *out, size_t out_sz) {
    if (out_sz < 20) return;  /* sanity check */
    /* find last dot or end */
    const char *dot = strrchr(rules_path, '.');
    const char *slash = strrchr(rules_path, '/');
    size_t path_len = strlen(rules_path);
    if (path_len > out_sz - 15) path_len = out_sz - 15;  /* leave room for .disabled + ext */

    if (dot && (!slash || dot > slash)) {
        size_t base_len = (size_t)(dot - rules_path);
        if (base_len > path_len) base_len = path_len;
        snprintf(out, out_sz, "%.*s.disabled%s", (int)base_len, rules_path, dot);
    } else {
        snprintf(out, out_sz, "%.*s.disabled", (int)path_len, rules_path);
    }
}

/* rule edit modal - returns 1 if rule was modified */
static int edit_rule_modal(struct rule *r, int rule_index, struct history_stack *history, int scr_h, int scr_w) {
    /* adjust height based on extras */
    int base_h = 20;  /* increased for name field + derived preview */
    int extras_h = r->extras_count > 0 ? (int)r->extras_count + 2 : 0;
    int h = base_h + extras_h;
    if (h > scr_h - 4) h = scr_h - 4;
    int w = 60;
    int y = (scr_h - h) / 2;
    int x = (scr_w - w) / 2;

    /* fields to edit - F_DERIVED is special (not a real field, just clickable) */
    enum { F_NAME, F_DERIVED, F_CLASS, F_TITLE, F_TAG, F_WORKSPACE, F_FLOAT, F_CENTER, F_SIZE, F_OPACITY, F_COUNT };
    int field = 0;

    char name_buf[128], class_buf[128], title_buf[128], tag_buf[64], ws_buf[32], size_buf[32], opacity_buf[32];
    /* original values for change detection */
    char orig_name[128], orig_class[128], orig_title[128], orig_tag[64], orig_ws[32], orig_size[32], orig_opacity[32];
    int float_val = r->actions.float_set ? r->actions.float_val : 0;
    int center_val = r->actions.center_set ? r->actions.center_val : 0;
    int orig_float = float_val, orig_center = center_val;

    /* init buffers from rule */
    snprintf(name_buf, sizeof(name_buf), "%s", r->name ? r->name : "");
    snprintf(class_buf, sizeof(class_buf), "%s", r->match.class_re ? r->match.class_re : "");
    snprintf(title_buf, sizeof(title_buf), "%s", r->match.title_re ? r->match.title_re : "");
    snprintf(tag_buf, sizeof(tag_buf), "%s", r->actions.tag ? r->actions.tag : "");
    snprintf(ws_buf, sizeof(ws_buf), "%s", r->actions.workspace ? r->actions.workspace : "");
    snprintf(size_buf, sizeof(size_buf), "%s", r->actions.size ? r->actions.size : "");
    snprintf(opacity_buf, sizeof(opacity_buf), "%s", r->actions.opacity ? r->actions.opacity : "");
    /* save originals */
    snprintf(orig_name, sizeof(orig_name), "%s", name_buf);
    snprintf(orig_class, sizeof(orig_class), "%s", class_buf);
    snprintf(orig_title, sizeof(orig_title), "%s", title_buf);
    snprintf(orig_tag, sizeof(orig_tag), "%s", tag_buf);
    snprintf(orig_ws, sizeof(orig_ws), "%s", ws_buf);
    snprintf(orig_size, sizeof(orig_size), "%s", size_buf);
    snprintf(orig_opacity, sizeof(orig_opacity), "%s", opacity_buf);

    int editing = 0;  /* 0 = navigating, 1 = editing text field */

    while (1) {
        /* draw modal box */
        attron(COLOR_PAIR(COL_BORDER));
        for (int i = 0; i < h; i++) {
            mvhline(y + i, x, ' ', w);
        }
        draw_box(y, x, h, w, "Edit Rule");
        attroff(COLOR_PAIR(COL_BORDER));

        /* compute derived name for preview - same logic as update_display_name */
        char derived[64] = "";
        clean_class_name(class_buf, derived, sizeof(derived));
        if (!derived[0]) {
            clean_class_name(title_buf, derived, sizeof(derived));
        }
        if (!derived[0]) snprintf(derived, sizeof(derived), "(unnamed)");

        /* check which fields changed */
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

        /* draw fields */
        int row = y + 2;
        const char *labels[] = {"Name:", "", "Class:", "Title:", "Tag:", "Workspace:", "Float:", "Center:", "Size:", "Opacity:"};
        char *bufs[] = {name_buf, NULL, class_buf, title_buf, tag_buf, ws_buf, NULL, NULL, size_buf, opacity_buf};

        for (int i = 0; i < F_COUNT; i++) {
            /* F_DERIVED is the derived name row */
            if (i == F_DERIVED) {
                if (field == F_DERIVED) attron(COLOR_PAIR(COL_ACCENT));
                else attron(COLOR_PAIR(COL_DIM));
                mvprintw(row, x + 2, "  → %-38.38s", derived);
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

            /* show * for changed fields */
            char label[16];
            snprintf(label, sizeof(label), "%s%s", changed[i] ? "*" : " ", labels[i]);
            mvprintw(row, x + 1, "%-12s", label);

            if (i == F_FLOAT) {
                mvprintw(row, x + 14, "[%c] %s", float_val ? 'x' : ' ', float_val ? "Yes" : "No");
            } else if (i == F_CENTER) {
                mvprintw(row, x + 14, "[%c] %s", center_val ? 'x' : ' ', center_val ? "Yes" : "No");
            } else {
                /* show field value, with cursor indicator if editing */
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

        /* show extras (read-only) */
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

        /* help - context sensitive */
        attron(COLOR_PAIR(COL_DIM));
        if (editing) {
            mvprintw(y + h - 3, x + 2, "Type to edit, Backspace to delete");
            mvprintw(y + h - 2, x + 2, "Enter:Done  Esc:Cancel edit");
        } else {
            mvprintw(y + h - 3, x + 2, "↑↓:Select  Enter:Edit  Space:Toggle");
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
            } else if (ch == 27) {  /* ESC - cancel edit */
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

        /* mouse events in edit modal */
        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                int click = event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED);
                int dblclick = event.bstate & BUTTON1_DOUBLE_CLICKED;

                /* click/double-click on a field - rows map directly to F_* enum now */
                if ((click || dblclick) && event.x >= x && event.x < x + w) {
                    int clicked_row = event.y - (y + 2);
                    if (clicked_row >= 0 && clicked_row < F_COUNT) {
                        field = clicked_row;
                        /* handle derived - copy to name */
                        if (field == F_DERIVED) {
                            snprintf(name_buf, sizeof(name_buf), "%s", derived);
                            field = F_NAME;  /* move back to name field */
                        }
                        /* toggle checkboxes on click */
                        else if (field == F_FLOAT) float_val = !float_val;
                        else if (field == F_CENTER) center_val = !center_val;
                        /* double-click to edit text */
                        else if (dblclick && bufs[field]) {
                            editing = 1;
                            curs_set(1);
                        }
                    }
                }
                /* scroll wheel to navigate fields */
                if (event.bstate & BUTTON4_PRESSED && field > 0) field--;
                if (event.bstate & BUTTON5_PRESSED && field < F_COUNT - 1) field++;
            }
            continue;
        }

        /* navigation mode */
        if (ch == KEY_UP && field > 0) field--;
        else if (ch == KEY_DOWN && field < F_COUNT - 1) field++;
        else if (ch == '\n' || ch == KEY_ENTER) {
            /* derived row - copy to name */
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
            /* record change in history before modifying */
            struct rule old_state = *r;  /* copy for history */
            
            /* save changes to rule */
            free(r->name); r->name = name_buf[0] ? strdup(name_buf) : NULL;
            free(r->match.class_re); r->match.class_re = class_buf[0] ? strdup(class_buf) : NULL;
            free(r->match.title_re); r->match.title_re = title_buf[0] ? strdup(title_buf) : NULL;
            free(r->actions.tag); r->actions.tag = tag_buf[0] ? strdup(tag_buf) : NULL;
            free(r->actions.workspace); r->actions.workspace = ws_buf[0] ? strdup(ws_buf) : NULL;
            free(r->actions.size); r->actions.size = size_buf[0] ? strdup(size_buf) : NULL;
            free(r->actions.opacity); r->actions.opacity = opacity_buf[0] ? strdup(opacity_buf) : NULL;
            r->actions.float_set = 1; r->actions.float_val = float_val;
            r->actions.center_set = 1; r->actions.center_val = center_val;
            update_display_name(r);  /* update derived display name */
            
            /* record in history */
            char desc[128];
            snprintf(desc, sizeof(desc), "Edit rule %d", rule_index);
            history_record(history, CHANGE_EDIT, rule_index, &old_state, r, desc);
            
            curs_set(0);
            return 1;
        }
        else if (ch == 'q' || ch == 'Q' || ch == 27) {
            curs_set(0);
            return 0;
        }
    }
}

/* search/filter helpers */
static void search_init(struct search_state *s) __attribute__((unused));
static void search_init(struct search_state *s) {
    if (!s) return;
    memset(s, 0, sizeof(struct search_state));
    s->matches = malloc(1000 * sizeof(int));  /* Max 1000 matches */
}

static void search_free(struct search_state *s) __attribute__((unused));
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
        return;  /* Empty query, no filtering */
    }
    
    s->active = 1;
    
    for (size_t i = 0; i < rs->count && s->match_count < 999; i++) {
        struct rule *r = &rs->rules[i];
        
        /* Search in name, class, title, tag, workspace */
        const char *name = r->display_name ? r->display_name : "";
        const char *class_re = r->match.class_re ? r->match.class_re : "";
        const char *title_re = r->match.title_re ? r->match.title_re : "";
        const char *tag = r->actions.tag ? r->actions.tag : "";
        const char *workspace = r->actions.workspace ? r->actions.workspace : "";
        
        /* Case-insensitive substring search */
        char *lower_query = strdup(s->query);
        str_to_lower(lower_query);
        
        char name_lower[128];
        snprintf(name_lower, sizeof(name_lower), "%s", name);
        str_to_lower(name_lower);
        
        if (strstr(name_lower, lower_query) != NULL ||
            strstr(class_re, s->query) != NULL ||
            strstr(title_re, s->query) != NULL ||
            strstr(tag, s->query) != NULL ||
            strstr(workspace, s->query) != NULL) {
            s->matches[s->match_count++] = i;
        }
        
        free(lower_query);
    }
    
    s->current_match = 0;  /* Reset to first match */
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

static int search_modal(struct search_state *s, struct ruleset *rs, int scr_h, int scr_w) __attribute__((unused));
static int search_modal(struct search_state *s, struct ruleset *rs, int scr_h, int scr_w) {
    /* Display search modal and handle input */
    int h = 7, w = 60;
    int y = (scr_h - h) / 2;
    int x = (scr_w - w) / 2;
    
    while (1) {
        clear();
        attron(COLOR_PAIR(COL_BORDER));
        for (int i = 0; i < h; i++) mvhline(y + i, x, ' ', w);
        draw_box(y, x, h, w, "Search Rules");
        attroff(COLOR_PAIR(COL_BORDER));
        
        /* Input field */
        mvprintw(y + 2, x + 2, "Query: ");
        attron(COLOR_PAIR(COL_SELECT));
        mvprintw(y + 2, x + 10, "%-46s_", s->query);
        attroff(COLOR_PAIR(COL_SELECT));
        
        /* Results summary */
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
                return s->matches[s->current_match];  /* Return selected rule index */
            }
            return -1;
        } else if (ch == 27) {  /* ESC */
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

/* splash screen */
static void draw_splash(int height, int width) {
    const char *lines[] = {
        " _                        _           _                    ",
        "| |__  _   _ _ __  _ __  (_)_ __   __| | _____      _____  ",
        "| '_ \\| | | | '_ \\| '__| | | '_ \\ / _` |/ _ \\ \\ /\\ / / __| ",
        "| | | | |_| | |_) | |    | | | | | (_| | (_) \\ V  V /\\__ \\ ",
        "|_| |_|\\__, | .__/|_|    |_|_| |_|\\__,_|\\___/ \\_/\\_/ |___/ ",
        "       |___/|_|                                            ",
        "",
        "Window Rules Manager",
        "",
        "Press any key to continue...",
    };
    int n = sizeof(lines) / sizeof(lines[0]);
    int start_y = (height - n) / 2;

    clear();
    for (int i = 0; i < n; i++) {
        int x = (width - (int)strlen(lines[i])) / 2;
        if (x < 0) x = 0;

        if (i < 6) {
            attron(A_BOLD | COLOR_PAIR(COL_TITLE));
        } else if (i == n - 1) {
            attron(COLOR_PAIR(COL_DIM));
        }

        mvprintw(start_y + i, x, "%s", lines[i]);

        if (i < 6) {
            attroff(A_BOLD | COLOR_PAIR(COL_TITLE));
        } else if (i == n - 1) {
            attroff(COLOR_PAIR(COL_DIM));
        }
    }
    refresh();
}

/* loading screen */
static void draw_loading(int height, int width, const char *msg) {
    clear();
    attron(COLOR_PAIR(COL_TITLE));
    mvprintw(height / 2, (width - (int)strlen(msg)) / 2, "%s", msg);
    attroff(COLOR_PAIR(COL_TITLE));
    refresh();
}

/* draw UI based on current state */
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

    /* header */
    if (st->modified) {
        draw_header(width, "hyprwindows [*]");
    } else {
        draw_header(width, "hyprwindows");
    }

    /* tabs */
    draw_tabs(1, width, sm->current_state);

    /* main content area */
    int content_y = 2;
    int content_h = height - 4;

    switch (sm->current_state) {
    case VIEW_RULES:
        if (width > 100) {
            /* split view: list + detail */
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
    case VIEW_SETTINGS:
        draw_settings_view(st, content_y, content_h, width);
        break;
    }

    /* status bar - view-specific help */
    const char *help;
    switch (sm->current_state) {
    case VIEW_RULES:
        help = "Enter:Edit  d:Del  x:Disable  /:Search  g:Group  ^S:Save  ^Z/Y:Undo  q:Quit";
        break;
    default:
        help = "1-4:Views  ↑↓:Nav  ^S:Save  ^Z/Y:Undo  r:Reload  q:Quit";
        break;
    }
    draw_statusbar(height - 1, width, st->status, help);

    refresh();
}

/* handle global keys (applicable to all views) */
static void handle_global_keys(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;
    int height, width;
    getmaxyx(stdscr, height, width);

    /* quit */
    if (ch == 'q' || ch == 'Q') {
        if (st->modified) {
            /* prompt to save */
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
                else if (c == 27) { choice = 2; break; }  /* ESC = cancel */
                else if (c == 's' || c == 'S') { choice = 0; break; }
                else if (c == 'q') { choice = 1; break; }
            }

            if (choice == 0) {
                /* save and quit */
                if (!st->backup_created) create_backup(st);
                if (save_rules(st) == 0) {
                    set_status(st, "Saved to %s", st->rules_path);
                }
                sm->running = 0;
            } else if (choice == 1) {
                /* quit without saving */
                sm->running = 0;
            }
            /* choice == 2: cancel, continue */
        } else {
            sm->running = 0;
        }
        return;
    }

    /* save */
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

    /* backup only */
    if (ch == 'b' || ch == 'B') {
        if (create_backup(st) == 0) {
            set_status(st, "Backup created: %s", st->backup_path);
        } else {
            set_status(st, "Failed to create backup");
        }
        return;
    }

    /* view switching */
    if (ch == '1') { sm->current_state = VIEW_RULES; st->selected = 0; st->scroll = 0; return; }
    if (ch == '2') { sm->current_state = VIEW_WINDOWS; st->scroll = 0; return; }
    if (ch == '3') { sm->current_state = VIEW_REVIEW; return; }
    if (ch == '4') { sm->current_state = VIEW_SETTINGS; st->selected = 0; return; }

    /* reload */
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

/* handle input for rules view */
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
    /* search with / key */
    else if (ch == '/') {
        struct search_state search;
        search_init(&search);
        int result = search_modal(&search, &st->rules, height, width);
        if (result >= 0) {
            st->selected = result;
        }
        search_free(&search);
    }
    /* cycle grouping mode with 'g' key */
    else if (ch == 'g' || ch == 'G') {
        st->grouping = (st->grouping + 1) % 4;
        switch (st->grouping) {
        case GROUP_BY_WORKSPACE:
            set_status(st, "Grouped by Workspace");
            break;
        case GROUP_BY_TAG:
            set_status(st, "Grouped by Tag");
            break;
        case GROUP_BY_FLOAT:
            set_status(st, "Grouped by Float");
            break;
        case GROUP_UNGROUPED:
            set_status(st, "Ungrouped");
            break;
        }
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
             /* record history before deletion */
             struct rule deleted_copy = *r;
             char desc[128];
             snprintf(desc, sizeof(desc), "Delete rule %d", st->selected);
             history_record(&st->history, CHANGE_DELETE, st->selected, &deleted_copy, NULL, desc);
             
             /* remove from array */
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
    /* disable rule - move to .disabled file */
    else if (ch == 'x' && st->selected >= 0 && st->selected < (int)st->rules.count) {
        struct rule *r = &st->rules.rules[st->selected];
        char msg[64];
        snprintf(msg, sizeof(msg), "Disable rule '%s'?", r->name ? r->name : "(unnamed)");
        if (confirm_dialog(height, width, "Disable Rule", msg)) {
            char disabled_path[512];
            char *expanded = expand_path(st->rules_path);
            get_disabled_path(expanded ? expanded : st->rules_path, disabled_path, sizeof(disabled_path));
            free(expanded);

             if (write_rule_to_file(disabled_path, r, "a") == 0) {
                 /* record history before disabling */
                 struct rule disabled_copy = *r;
                 char desc[128];
                 snprintf(desc, sizeof(desc), "Disable rule %d", st->selected);
                 history_record(&st->history, CHANGE_DISABLE, st->selected, &disabled_copy, NULL, desc);
                 
                 /* remove from array */
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
    /* save */
    else if (ch == 19) {  /* Ctrl+S */
        if (st->modified) {
            char *path = expand_path(st->rules_path);
            if (export_save_rules(path ? path : st->rules_path, path ? path : st->rules_path, &st->rules) == 0) {
                st->modified = 0;
                set_status(st, "Rules saved to %s", path ? path : st->rules_path);
            } else {
                set_status(st, "Failed to save rules");
            }
            free(path);
        } else {
            set_status(st, "No changes to save");
        }
    }
    /* undo */
    else if (ch == 26) {  /* Ctrl+Z */
        if (history_can_undo(&st->history)) {
            struct rule *old_rule = history_undo(&st->history);
            if (old_rule && st->selected >= 0 && st->selected < (int)st->rules.count) {
                /* cleanup old rule fields to prevent memory leak */
                cleanup_rule(&st->rules.rules[st->selected]);
                
                /* restore the old state */
                st->rules.rules[st->selected] = *old_rule;
                st->modified = 1;
                st->analysis_loaded = 0;  /* invalidate analysis cache */
                set_status(st, "Undo complete");
                free(old_rule);
            }
        } else {
            set_status(st, "Nothing to undo");
        }
    }
    /* redo */
    else if (ch == 25) {  /* Ctrl+Y */
        if (history_can_redo(&st->history)) {
            struct rule *new_rule = history_redo(&st->history);
            if (new_rule && st->selected >= 0 && st->selected < (int)st->rules.count) {
                /* cleanup old rule fields to prevent memory leak */
                cleanup_rule(&st->rules.rules[st->selected]);
                
                /* restore the new state */
                st->rules.rules[st->selected] = *new_rule;
                st->modified = 1;
                st->analysis_loaded = 0;  /* invalidate analysis cache */
                set_status(st, "Redo complete");
                free(new_rule);
            }
        } else {
            set_status(st, "Nothing to redo");
        }
    }
}

/* handle input for windows view */
static void handle_windows_input(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;

    if (ch == KEY_UP || ch == KEY_PPAGE) { st->scroll -= (ch == KEY_PPAGE ? 10 : 1); if (st->scroll < 0) st->scroll = 0; }
    if (ch == KEY_DOWN || ch == KEY_NPAGE) st->scroll += (ch == KEY_NPAGE ? 10 : 1);
}

/* handle input for review view */
static void handle_review_input(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;

    if (ch == KEY_UP || ch == KEY_PPAGE) { st->scroll -= (ch == KEY_PPAGE ? 10 : 1); if (st->scroll < 0) st->scroll = 0; }
    if (ch == KEY_DOWN || ch == KEY_NPAGE) st->scroll += (ch == KEY_NPAGE ? 10 : 1);
}

/* handle input for settings view */
static void handle_settings_input(ui_state_machine_t *sm, int ch) {
    struct ui_state *st = sm->st;

    if (ch == KEY_UP && st->selected > 0) st->selected--;
    if (ch == KEY_DOWN && st->selected < 4) st->selected++;
    if (ch == ' ' || ch == '\n') {
        if (st->selected == 3) st->suggest_rules = !st->suggest_rules;
        if (st->selected == 4) st->show_overlaps = !st->show_overlaps;
    }
}

/* dispatch input to appropriate handler based on view */
static void handle_input(ui_state_machine_t *sm, int ch) {
    /* global keys apply to all views */
    handle_global_keys(sm, ch);
    
    /* if still running, dispatch to view-specific handler */
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
    case VIEW_SETTINGS:
        handle_settings_input(sm, ch);
        break;
    }
}

/* main entry */
int run_tui(void) {
    struct ui_state st;
    memset(&st, 0, sizeof(st));
    st.mode = VIEW_RULES;
    st.suggest_rules = 1;
    st.show_overlaps = 1;
    st.grouping = GROUP_BY_WORKSPACE;
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

    /* enable mouse support */
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    mouseinterval(0);  /* no click delay */

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

    /* splash - wait for key */
    draw_splash(height, width);
    getch();

    /* loading */
    draw_loading(height, width, "Loading rules...");
    load_rules(&st);

    /* initialize state machine */
    ui_state_machine_t sm;
    sm.current_state = VIEW_RULES;
    sm.running = 1;
    sm.st = &st;
    sm.handle_rules_input = handle_rules_input;
    sm.handle_windows_input = handle_windows_input;
    sm.handle_review_input = handle_review_input;
    sm.handle_settings_input = handle_settings_input;

    /* main loop */
    while (sm.running) {
        getmaxyx(stdscr, height, width);
        draw_ui(&sm);
        
        /* handle mouse events */
        int ch = getch();
        if (ch == KEY_MOUSE) {
            MEVENT event;
            if (getmouse(&event) == OK) {
                int click = event.bstate & (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED);
                int dblclick = event.bstate & BUTTON1_DOUBLE_CLICKED;

                /* double-click to edit (check FIRST, before single click) */
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
                /* click on tabs (row 1) */
                else if (event.y == 1 && click) {
                    /* calculate tab positions based on actual tab strings */
                    /* [1] Rules  [2] Windows  [3] Review  [4] Settings */
                    if (event.x >= 2 && event.x < 14) {
                        sm.current_state = VIEW_RULES; st.selected = 0; st.scroll = 0;
                    } else if (event.x >= 15 && event.x < 29) {
                        sm.current_state = VIEW_WINDOWS; st.scroll = 0;
                    } else if (event.x >= 30 && event.x < 43) {
                        sm.current_state = VIEW_REVIEW;
                    } else if (event.x >= 44 && event.x < 60) {
                        sm.current_state = VIEW_SETTINGS; st.selected = 0;
                    }
                }
                /* scroll wheel */
                else if (event.bstate & BUTTON4_PRESSED) {  /* scroll up */
                    if (sm.current_state == VIEW_RULES && st.selected > 0) st.selected--;
                    else if (sm.current_state == VIEW_WINDOWS && st.scroll > 0) st.scroll--;
                }
                else if (event.bstate & BUTTON5_PRESSED) {  /* scroll down */
                    if (sm.current_state == VIEW_RULES && st.selected < (int)st.rules.count - 1) st.selected++;
                    else if (sm.current_state == VIEW_WINDOWS) st.scroll++;
                }
                /* click in rules list */
                else if (sm.current_state == VIEW_RULES && click && event.y > 3) {
                    int content_y = 2;
                    int list_row = event.y - content_y - 2;  /* -2 for box border and header */
                    if (list_row >= 0 && event.x > 0 && event.x < width * 2 / 3) {
                        int clicked_idx = st.scroll + list_row;
                        if (clicked_idx >= 0 && clicked_idx < (int)st.rules.count) {
                            st.selected = clicked_idx;
                        }
                    }
                }
                /* click in settings to toggle */
                else if (sm.current_state == VIEW_SETTINGS && click) {
                    int content_y = 2;
                    int row = (event.y - content_y - 2) / 2;  /* settings rows are spaced by 2 */
                    if (row >= 0 && row <= 4) {
                        st.selected = row;
                        if (row == 3) st.suggest_rules = !st.suggest_rules;
                        if (row == 4) st.show_overlaps = !st.show_overlaps;
                    }
                }
            }
            continue;
        }

        /* handle keyboard input */
        handle_input(&sm, ch);
    }

    endwin();
    ruleset_free(&st.rules);
    free(st.rule_status);
    missing_rules_free(&st.missing);
    free(st.review_text);
    
    /* cleanup analysis and cascade */
    if (st.analysis_report) {
        analysis_free(st.analysis_report);
    }
    if (st.cascade_cache) {
        for (size_t i = 0; i < st.cascade_count; i++) {
            if (st.cascade_cache[i]) {
                cascade_free(st.cascade_cache[i]);
            }
        }
        free(st.cascade_cache);
    }
    history_free(&st.history);
    
    return 0;
}
