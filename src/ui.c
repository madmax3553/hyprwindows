#include "ui.h"

#include <ctype.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actions.h"

#define UI_MIN_WIDTH 70
#define UI_MIN_HEIGHT 20

struct ui_state {
    int selected;
    int scroll;
    int content_lines;
    char *content;

    char rules_path[512];
    char dotfiles_path[512];
    char appmap_path[512];

    int suggest_rules;
    int show_overlaps;
};

struct rule_editor {
    char name[128];
    char match_class[256];
    char match_title[256];
    char tag[64];
    char workspace[32];
    char opacity[32];
    char size[32];
    char move[32];
    int set_float;
    int set_center;
};

static const char *menu_items[] = {
    "Summarize Rules",
    "Scan Dotfiles",
    "Active Windows",
    "Rule Editor",
    "Settings",
    "Quit",
};

static int menu_count(void) {
    return (int)(sizeof(menu_items) / sizeof(menu_items[0]));
}

static void set_default_paths(struct ui_state *st) {
    const char *home = getenv("HOME");
    if (!home) {
        home = ".";
    }
    snprintf(st->rules_path, sizeof(st->rules_path), "%s/.config/hypr/configs/windowrules.jsonc", home);
    snprintf(st->dotfiles_path, sizeof(st->dotfiles_path), "%s/dotfiles", home);
    snprintf(st->appmap_path, sizeof(st->appmap_path), "data/appmap.json");
}

static char *expand_home(const char *path) {
    if (!path) {
        return NULL;
    }
    if (path[0] != '~') {
        return strdup(path);
    }
    const char *home = getenv("HOME");
    if (!home) {
        home = ".";
    }
    size_t hlen = strlen(home);
    size_t plen = strlen(path);
    char *out = (char *)malloc(hlen + plen);
    if (!out) {
        return NULL;
    }
    strcpy(out, home);
    if (path[1] == '/') {
        strcat(out, path + 1);
    } else {
        strcat(out, path + 1);
    }
    return out;
}

static void content_set(struct ui_state *st, const char *text) {
    free(st->content);
    st->content = NULL;
    st->content_lines = 0;
    st->scroll = 0;
    if (!text) {
        return;
    }
    st->content = strdup(text);
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            st->content_lines++;
        }
    }
    if (st->content_lines == 0) {
        st->content_lines = 1;
    } else {
        st->content_lines++;
    }
}

static int content_total_lines(const struct ui_state *st) {
    return st->content_lines;
}

static const char *content_line_at(const char *content, int line) {
    if (!content || line <= 0) {
        return content;
    }
    int cur = 0;
    const char *p = content;
    while (*p) {
        if (*p == '\n') {
            cur++;
            if (cur == line) {
                return p + 1;
            }
        }
        p++;
    }
    return NULL;
}

static void draw_title(int width) {
    attron(A_BOLD | COLOR_PAIR(1));
    mvhline(0, 0, ' ', width);
    mvprintw(0, 2, "hyprwindows — window rule helper");
    attroff(A_BOLD | COLOR_PAIR(1));
}

static void draw_status(int height, int width, const char *msg) {
    attron(COLOR_PAIR(3));
    mvhline(height - 1, 0, ' ', width);
    if (msg) {
        mvprintw(height - 1, 2, "%s", msg);
    }
    attroff(COLOR_PAIR(3));
}

static void draw_menu(int start_y, int width, int height, int selected) {
    attron(COLOR_PAIR(2));
    mvhline(start_y, 0, ' ', width);
    mvprintw(start_y, 2, "Menu");
    attroff(COLOR_PAIR(2));

    for (int i = 0; i < menu_count(); i++) {
        int y = start_y + 2 + i;
        if (y >= start_y + height - 1) {
            break;
        }
        if (i == selected) {
            attron(A_BOLD | COLOR_PAIR(4));
            mvhline(y, 1, ' ', width - 2);
            mvprintw(y, 2, "%s", menu_items[i]);
            attroff(A_BOLD | COLOR_PAIR(4));
        } else {
            mvprintw(y, 2, "%s", menu_items[i]);
        }
    }
}

static void draw_content_box(int start_y, int start_x, int height, int width) {
    attron(COLOR_PAIR(2));
    mvhline(start_y, start_x, ' ', width);
    mvprintw(start_y, start_x + 2, "Output");
    attroff(COLOR_PAIR(2));

    attron(COLOR_PAIR(5));
    for (int i = 1; i < height - 1; i++) {
        mvhline(start_y + i, start_x, ' ', width);
    }
    attroff(COLOR_PAIR(5));
}

static void draw_content(int start_y, int start_x, int height, int width,
                         const struct ui_state *st) {
    draw_content_box(start_y, start_x, height, width);

    if (!st->content) {
        attron(COLOR_PAIR(6));
        mvprintw(start_y + 2, start_x + 2, "No output yet. Choose a menu item.");
        attroff(COLOR_PAIR(6));
        return;
    }

    int view_lines = height - 3;
    int total = content_total_lines(st);
    int max_scroll = total > view_lines ? total - view_lines : 0;
    int scroll = st->scroll;
    if (scroll > max_scroll) {
        scroll = max_scroll;
    }

    for (int i = 0; i < view_lines; i++) {
        int line_idx = scroll + i;
        if (line_idx >= total) {
            break;
        }
        const char *line = content_line_at(st->content, line_idx);
        if (!line) {
            break;
        }
        char buf[1024];
        int j = 0;
        while (line[j] && line[j] != '\n' && j < (int)sizeof(buf) - 1) {
            buf[j] = line[j];
            j++;
        }
        buf[j] = '\0';
        mvprintw(start_y + 1 + i, start_x + 1, "%.*s", width - 2, buf);
    }

    if (max_scroll > 0) {
        int bar_height = view_lines;
        int thumb = (bar_height * view_lines) / total;
        if (thumb < 1) {
            thumb = 1;
        }
        int thumb_pos = (bar_height * scroll) / total;
        for (int i = 0; i < bar_height; i++) {
            int y = start_y + 1 + i;
            int x = start_x + width - 2;
            if (i >= thumb_pos && i < thumb_pos + thumb) {
                attron(COLOR_PAIR(4));
                mvaddch(y, x, ' ');
                attroff(COLOR_PAIR(4));
            } else {
                mvaddch(y, x, ' ');
            }
        }
    }
}

static void draw_footer(int start_y, int width) {
    attron(COLOR_PAIR(2));
    mvhline(start_y, 0, ' ', width);
    mvprintw(start_y, 2, "Keys: ↑↓ menu  Enter run  PgUp/PgDn scroll  s settings  q quit");
    attroff(COLOR_PAIR(2));
}

static void run_action(struct ui_state *st, int action) {
    struct outbuf out;
    struct action_opts opts = {st->suggest_rules, st->show_overlaps};
    outbuf_init(&out);

    char *rules = expand_home(st->rules_path);
    char *dotfiles = expand_home(st->dotfiles_path);
    char *appmap = expand_home(st->appmap_path);

    switch (action) {
    case 0:
        summarize_rules_text(rules ? rules : st->rules_path, &out);
        break;
    case 1:
        scan_dotfiles_text(dotfiles ? dotfiles : st->dotfiles_path,
                           rules ? rules : st->rules_path,
                           appmap ? appmap : st->appmap_path,
                           &opts, &out);
        break;
    case 2:
        active_windows_text(rules ? rules : st->rules_path, &opts, &out);
        break;
    default:
        outbuf_printf(&out, "No action selected.\n");
        break;
    }

    content_set(st, out.data ? out.data : "(no output)\n");

    free(rules);
    free(dotfiles);
    free(appmap);
    outbuf_free(&out);
}

static int edit_line(WINDOW *win, int y, int x, int width, char *buf, size_t cap) {
    int len = (int)strlen(buf);
    int pos = len;
    keypad(win, TRUE);
    curs_set(1);
    while (1) {
        mvwprintw(win, y, x, "%*s", width, "");
        mvwprintw(win, y, x, "%.*s", width, buf);
        wmove(win, y, x + pos);
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 27 || ch == '\n' || ch == KEY_ENTER) {
            curs_set(0);
            return ch == 27 ? 0 : 1;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, (size_t)(len - pos + 1));
                pos--;
                len--;
            }
            continue;
        }
        if (ch == KEY_LEFT) {
            if (pos > 0) {
                pos--;
            }
            continue;
        }
        if (ch == KEY_RIGHT) {
            if (pos < len) {
                pos++;
            }
            continue;
        }
        if (isprint(ch) && (size_t)len + 1 < cap) {
            memmove(buf + pos + 1, buf + pos, (size_t)(len - pos + 1));
            buf[pos] = (char)ch;
            pos++;
            len++;
        }
    }
}

static void rule_editor_init(struct rule_editor *ed) {
    memset(ed, 0, sizeof(*ed));
    snprintf(ed->name, sizeof(ed->name), "rule-new");
}

static char *rule_editor_snippet(const struct rule_editor *ed) {
    struct outbuf out;
    outbuf_init(&out);

    outbuf_printf(&out, "{\n");
    if (ed->name[0]) {
        outbuf_printf(&out, "  \"name\": \"%s\"", ed->name);
    } else {
        outbuf_printf(&out, "  \"name\": \"rule-new\"");
    }

    if (ed->match_class[0]) {
        outbuf_printf(&out, ",\n  \"match:class\": \"%s\"", ed->match_class);
    }
    if (ed->match_title[0]) {
        outbuf_printf(&out, ",\n  \"match:title\": \"%s\"", ed->match_title);
    }
    if (ed->tag[0]) {
        outbuf_printf(&out, ",\n  \"tag\": \"%s\"", ed->tag);
    }
    if (ed->workspace[0]) {
        outbuf_printf(&out, ",\n  \"workspace\": \"%s\"", ed->workspace);
    }
    if (ed->opacity[0]) {
        outbuf_printf(&out, ",\n  \"opacity\": \"%s\"", ed->opacity);
    }
    if (ed->size[0]) {
        outbuf_printf(&out, ",\n  \"size\": \"%s\"", ed->size);
    }
    if (ed->move[0]) {
        outbuf_printf(&out, ",\n  \"move\": \"%s\"", ed->move);
    }
    if (ed->set_float) {
        outbuf_printf(&out, ",\n  \"float\": \"yes\"");
    }
    if (ed->set_center) {
        outbuf_printf(&out, ",\n  \"center\": \"yes\"");
    }
    outbuf_printf(&out, "\n}\n");

    char *snippet = out.data ? strdup(out.data) : NULL;
    outbuf_free(&out);
    return snippet;
}

static int rule_editor_write_snippet(const char *path, const char *snippet) {
    if (!path || !snippet) {
        return -1;
    }
    FILE *f = fopen(path, "a");
    if (!f) {
        return -1;
    }
    fprintf(f, "%s\n", snippet);
    fclose(f);
    return 0;
}

static void rule_editor_screen(struct ui_state *st) {
    int height, width;
    getmaxyx(stdscr, height, width);

    int win_h = 22;
    int win_w = width - 6;
    if (win_w < 70) {
        win_w = 70;
    }
    int start_y = (height - win_h) / 2;
    int start_x = (width - win_w) / 2;

    WINDOW *win = newwin(win_h, win_w, start_y, start_x);
    keypad(win, TRUE);

    struct rule_editor ed;
    rule_editor_init(&ed);
    int selected = 0;
    int total_items = 10;
    const char *labels[] = {
        "Name",
        "Match class",
        "Match title",
        "Tag",
        "Workspace",
        "Opacity",
        "Size",
        "Move",
        "Float",
        "Center",
    };

    while (1) {
        werase(win);
        box(win, 0, 0);
        wattron(win, A_BOLD | COLOR_PAIR(1));
        mvwprintw(win, 0, 2, " Rule Editor ");
        wattroff(win, A_BOLD | COLOR_PAIR(1));

        for (int i = 0; i < total_items; i++) {
            int y = 2 + i;
            if (i == selected) {
                wattron(win, COLOR_PAIR(4));
                mvwhline(win, y, 1, ' ', win_w - 2);
                wattroff(win, COLOR_PAIR(4));
            }
            mvwprintw(win, y, 2, "%-12s", labels[i]);
            if (i == 0) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.name);
            } else if (i == 1) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.match_class);
            } else if (i == 2) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.match_title);
            } else if (i == 3) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.tag);
            } else if (i == 4) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.workspace);
            } else if (i == 5) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.opacity);
            } else if (i == 6) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.size);
            } else if (i == 7) {
                mvwprintw(win, y, 16, "%.*s", win_w - 18, ed.move);
            } else if (i == 8) {
                mvwprintw(win, y, 16, "[%c]", ed.set_float ? 'x' : ' ');
            } else if (i == 9) {
                mvwprintw(win, y, 16, "[%c]", ed.set_center ? 'x' : ' ');
            }
        }

        mvwprintw(win, win_h - 3, 2, "Enter=edit  Space=toggle  g=generate  w=save snippet  Esc=close");
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 27 || ch == 'q') {
            break;
        }
        if (ch == KEY_UP) {
            if (selected > 0) {
                selected--;
            }
            continue;
        }
        if (ch == KEY_DOWN) {
            if (selected < total_items - 1) {
                selected++;
            }
            continue;
        }
        if (ch == ' ' && (selected == 8 || selected == 9)) {
            if (selected == 8) {
                ed.set_float = !ed.set_float;
            } else if (selected == 9) {
                ed.set_center = !ed.set_center;
            }
            continue;
        }
        if (ch == 'g' || ch == 'G') {
            char *snippet = rule_editor_snippet(&ed);
            if (snippet) {
                content_set(st, snippet);
                free(snippet);
            }
            continue;
        }
        if (ch == 'w' || ch == 'W') {
            char *snippet = rule_editor_snippet(&ed);
            if (snippet) {
                const char *snippet_path = "data/rule_snippet.jsonc";
                if (rule_editor_write_snippet(snippet_path, snippet) == 0) {
                    content_set(st, "Snippet saved to data/rule_snippet.jsonc\n");
                } else {
                    content_set(st, "Failed to save snippet to data/rule_snippet.jsonc\n");
                }
                free(snippet);
            }
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            if (selected == 0) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.name, sizeof(ed.name));
            } else if (selected == 1) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.match_class, sizeof(ed.match_class));
            } else if (selected == 2) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.match_title, sizeof(ed.match_title));
            } else if (selected == 3) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.tag, sizeof(ed.tag));
            } else if (selected == 4) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.workspace, sizeof(ed.workspace));
            } else if (selected == 5) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.opacity, sizeof(ed.opacity));
            } else if (selected == 6) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.size, sizeof(ed.size));
            } else if (selected == 7) {
                edit_line(win, 2 + selected, 16, win_w - 18, ed.move, sizeof(ed.move));
            }
        }
    }

    delwin(win);
}

static void settings_screen(struct ui_state *st) {
    int height, width;
    getmaxyx(stdscr, height, width);

    int win_h = 14;
    int win_w = width - 10;
    if (win_w < 60) {
        win_w = 60;
    }
    int start_y = (height - win_h) / 2;
    int start_x = (width - win_w) / 2;

    WINDOW *win = newwin(win_h, win_w, start_y, start_x);
    keypad(win, TRUE);

    int selected = 0;
    int total_items = 5;
    const char *labels[] = {
        "Rules path",
        "Dotfiles path",
        "Appmap path",
        "Suggest rules",
        "Show overlaps",
    };

    while (1) {
        werase(win);
        box(win, 0, 0);
        wattron(win, A_BOLD | COLOR_PAIR(1));
        mvwprintw(win, 0, 2, " Settings ");
        wattroff(win, A_BOLD | COLOR_PAIR(1));

        for (int i = 0; i < total_items; i++) {
            int y = 2 + i * 2;
            if (i == selected) {
                wattron(win, COLOR_PAIR(4));
                mvwhline(win, y, 1, ' ', win_w - 2);
                wattroff(win, COLOR_PAIR(4));
            }
            mvwprintw(win, y, 2, "%s", labels[i]);
            if (i == 0) {
                mvwprintw(win, y, 18, "%.*s", win_w - 22, st->rules_path);
            } else if (i == 1) {
                mvwprintw(win, y, 18, "%.*s", win_w - 22, st->dotfiles_path);
            } else if (i == 2) {
                mvwprintw(win, y, 18, "%.*s", win_w - 22, st->appmap_path);
            } else if (i == 3) {
                mvwprintw(win, y, 18, "[%c]", st->suggest_rules ? 'x' : ' ');
            } else if (i == 4) {
                mvwprintw(win, y, 18, "[%c]", st->show_overlaps ? 'x' : ' ');
            }
        }

        mvwprintw(win, win_h - 2, 2, "Enter=edit  Space=toggle  Esc=close");
        wrefresh(win);

        int ch = wgetch(win);
        if (ch == 27 || ch == 'q') {
            break;
        }
        if (ch == KEY_UP) {
            if (selected > 0) {
                selected--;
            }
            continue;
        }
        if (ch == KEY_DOWN) {
            if (selected < total_items - 1) {
                selected++;
            }
            continue;
        }
        if (ch == ' ' && selected >= 3) {
            if (selected == 3) {
                st->suggest_rules = !st->suggest_rules;
            } else if (selected == 4) {
                st->show_overlaps = !st->show_overlaps;
            }
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            if (selected == 0) {
                edit_line(win, 2 + selected * 2, 18, win_w - 22, st->rules_path, sizeof(st->rules_path));
            } else if (selected == 1) {
                edit_line(win, 2 + selected * 2, 18, win_w - 22, st->dotfiles_path, sizeof(st->dotfiles_path));
            } else if (selected == 2) {
                edit_line(win, 2 + selected * 2, 18, win_w - 22, st->appmap_path, sizeof(st->appmap_path));
            }
        }
    }

    delwin(win);
}

static void draw_splash(int height, int width) {
    const char *lines[] = {
        "██╗  ██╗██╗   ██╗██████╗ ██████╗ ██╗    ██╗██╗███╗   ██╗",
        "██║  ██║██║   ██║██╔══██╗██╔══██╗██║    ██║██║████╗  ██║",
        "███████║██║   ██║██████╔╝██████╔╝██║ █╗ ██║██║██╔██╗ ██║",
        "██╔══██║██║   ██║██╔══██╗██╔══██╗██║███╗██║██║██║╚██╗██║",
        "██║  ██║╚██████╔╝██║  ██║██║  ██║╚███╔███╔╝██║██║ ╚████║",
        "╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝ ╚══╝╚══╝ ╚═╝╚═╝  ╚═══╝",
        "",
        "Hyprland Window Rules",
    };
    int lines_count = (int)(sizeof(lines) / sizeof(lines[0]));
    int start_y = (height - lines_count) / 2;
    int start_x = (width - (int)strlen(lines[0])) / 2;
    clear();
    attron(A_BOLD | COLOR_PAIR(1));
    for (int i = 0; i < lines_count; i++) {
        int x = (i == lines_count - 1) ? (width - (int)strlen(lines[i])) / 2 : start_x;
        mvprintw(start_y + i, x, "%s", lines[i]);
    }
    attroff(A_BOLD | COLOR_PAIR(1));
    refresh();
    napms(650);
}

static void update_content_lines(struct ui_state *st) {
    st->content_lines = 0;
    if (!st->content) {
        return;
    }
    for (const char *p = st->content; *p; p++) {
        if (*p == '\n') {
            st->content_lines++;
        }
    }
    st->content_lines++;
}

int run_tui(void) {
    struct ui_state st;
    memset(&st, 0, sizeof(st));
    st.selected = 0;
    st.suggest_rules = 1;
    st.show_overlaps = 1;
    set_default_paths(&st);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    use_default_colors();

    init_pair(1, COLOR_CYAN, -1);
    init_pair(2, COLOR_BLACK, COLOR_CYAN);
    init_pair(3, COLOR_BLACK, COLOR_WHITE);
    init_pair(4, COLOR_BLACK, COLOR_YELLOW);
    init_pair(5, COLOR_WHITE, -1);
    init_pair(6, COLOR_BLUE, -1);

    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);

    int height, width;
    getmaxyx(stdscr, height, width);
    if (height >= 10 && width >= 50) {
        draw_splash(height, width);
    }

    int running = 1;
    while (running) {
        getmaxyx(stdscr, height, width);
        if (height < UI_MIN_HEIGHT || width < UI_MIN_WIDTH) {
            clear();
            mvprintw(0, 0, "Resize terminal to at least %dx%d", UI_MIN_WIDTH, UI_MIN_HEIGHT);
            refresh();
            int ch = getch();
            if (ch == 'q') {
                break;
            }
            continue;
        }

        int menu_w = 24;
        int content_x = menu_w + 1;
        int content_w = width - content_x - 1;
        int content_h = height - 4;
        int menu_h = height - 4;

        clear();
        draw_title(width);
        draw_menu(1, menu_w, menu_h, st.selected);
        draw_content(1, content_x, content_h, content_w, &st);
        draw_footer(height - 2, width);
        draw_status(height, width, "Enter run • s settings • q quit");
        refresh();

        int ch = getch();
        if (ch == 'q') {
            running = 0;
            continue;
        }
        if (ch == KEY_UP) {
            if (st.selected > 0) {
                st.selected--;
            }
            continue;
        }
        if (ch == KEY_DOWN) {
            if (st.selected < menu_count() - 1) {
                st.selected++;
            }
            continue;
        }
        if (ch == 's' || ch == 'S') {
            settings_screen(&st);
            update_content_lines(&st);
            continue;
        }
        if (ch == '\n' || ch == KEY_ENTER) {
            if (st.selected == menu_count() - 1) {
                running = 0;
                continue;
            }
            if (st.selected == 3) {
                rule_editor_screen(&st);
                update_content_lines(&st);
            } else if (st.selected == 4) {
                settings_screen(&st);
                update_content_lines(&st);
            } else {
                run_action(&st, st.selected);
            }
            continue;
        }
        if (ch == KEY_NPAGE) {
            st.scroll += 5;
            continue;
        }
        if (ch == KEY_PPAGE) {
            st.scroll -= 5;
            if (st.scroll < 0) {
                st.scroll = 0;
            }
            continue;
        }
        if (ch == KEY_MOUSE) {
            MEVENT ev;
            if (getmouse(&ev) == OK) {
                if (ev.y >= 1 && ev.y < 1 + menu_h && ev.x < menu_w) {
                    int idx = ev.y - 3;
                    if (idx >= 0 && idx < menu_count()) {
                        st.selected = idx;
                        if (ev.bstate & BUTTON1_CLICKED) {
                            if (st.selected == menu_count() - 1) {
                                running = 0;
                            } else {
                                if (st.selected == 3) {
                                    rule_editor_screen(&st);
                                    update_content_lines(&st);
                                } else if (st.selected == 4) {
                                    settings_screen(&st);
                                    update_content_lines(&st);
                                } else {
                                    run_action(&st, st.selected);
                                }
                            }
                        }
                    }
                }
                if (ev.bstate & BUTTON4_PRESSED) {
                    if (st.scroll > 0) {
                        st.scroll--;
                    }
                }
                if (ev.bstate & BUTTON5_PRESSED) {
                    st.scroll++;
                }
            }
            continue;
        }
        if (ch == KEY_RIGHT || ch == KEY_LEFT) {
            continue;
        }
    }

    endwin();
    free(st.content);
    return 0;
}
