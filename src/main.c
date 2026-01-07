#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actions.h"
#include "ui.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s            (default TUI)\n"
            "  %s --tui\n"
            "  %s summarize <rules.json|jsonc>\n"
            "  %s scan-dotfiles <dotfiles_dir> <rules.json|jsonc> [appmap.json]\n"
            "  %s active <rules.json|jsonc>\n"
            "  %s --help\n",
            prog, prog, prog, prog, prog, prog);
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
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        struct outbuf out;
        outbuf_init(&out);
        summarize_rules_text(argv[2], &out);
        if (out.data) {
            printf("%s", out.data);
        }
        outbuf_free(&out);
        return 0;
    }
    if (strcmp(cmd, "scan-dotfiles") == 0) {
        if (argc < 4) {
            usage(argv[0]);
            return 1;
        }
        const char *appmap = (argc >= 5) ? argv[4] : "data/appmap.json";
        struct outbuf out;
        struct action_opts opts = {1, 1};
        outbuf_init(&out);
        scan_dotfiles_text(argv[2], argv[3], appmap, &opts, &out);
        if (out.data) {
            printf("%s", out.data);
        }
        outbuf_free(&out);
        return 0;
    }
    if (strcmp(cmd, "active") == 0) {
        if (argc < 3) {
            usage(argv[0]);
            return 1;
        }
        struct outbuf out;
        struct action_opts opts = {1, 1};
        outbuf_init(&out);
        active_windows_text(argv[2], &opts, &out);
        if (out.data) {
            printf("%s", out.data);
        }
        outbuf_free(&out);
        return 0;
    }

    usage(argv[0]);
    return 1;
}
