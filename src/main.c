#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actions.h"
#include "rules.h"
#include "ui.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s            (default TUI)\n"
            "  %s --tui\n"
            "  %s summarize [rules.conf]\n"
            "  %s scan-dotfiles <dotfiles_dir> [rules.conf] [appmap.json]\n"
            "  %s active [rules.conf]\n"
            "  %s --help\n\n"
            "If rules.conf is omitted, auto-detects from ~/.config/hypr/hyprland.conf\n",
            prog, prog, prog, prog, prog, prog);
}

static char *get_rules_path(const char *arg) {
    if (arg) {
        return strdup(arg);
    }
    char *path = hypr_find_rules_config();
    if (!path) {
        fprintf(stderr, "Could not auto-detect rules config. Specify path manually.\n");
    }
    return path;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        return run_tui();
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "help") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "--tui") == 0) {
        return run_tui();
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "summarize") == 0) {
        char *rules = get_rules_path(argc >= 3 ? argv[2] : NULL);
        if (!rules) {
            return 1;
        }
        struct outbuf out;
        outbuf_init(&out);
        summarize_rules_text(rules, &out);
        if (out.data) {
            printf("%s", out.data);
        }
        outbuf_free(&out);
        free(rules);
        return 0;
    }
    if (strcmp(cmd, "scan-dotfiles") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        char *rules = get_rules_path(argc >= 4 ? argv[3] : NULL);
        if (!rules) {
            return 1;
        }
        const char *appmap = (argc >= 5) ? argv[4] : "data/appmap.json";
        struct outbuf out;
        struct action_opts opts = {1, 1};
        outbuf_init(&out);
        scan_dotfiles_text(argv[2], rules, appmap, &opts, &out);
        if (out.data) {
            printf("%s", out.data);
        }
        outbuf_free(&out);
        free(rules);
        return 0;
    }
    if (strcmp(cmd, "active") == 0) {
        char *rules = get_rules_path(argc >= 3 ? argv[2] : NULL);
        if (!rules) {
            return 1;
        }
        struct outbuf out;
        struct action_opts opts = {1, 1};
        outbuf_init(&out);
        active_windows_text(rules, &opts, &out);
        if (out.data) {
            printf("%s", out.data);
        }
        outbuf_free(&out);
        free(rules);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
