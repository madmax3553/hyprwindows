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

int main(int argc, char **argv) {
    if (argc == 1) {
        return run_tui();
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
        strcmp(argv[1], "help") == 0) {
        usage(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "--tui") == 0|| strcmp(argv[1], "-t") == 0) {
        return run_tui();
    }
    // TODO: implement other commands
    printf("Command not implemented yet.\n");
    return 1;
}
