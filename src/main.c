#include <stdio.h>
#include <string.h>

#include "ui.h"

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s          Launch TUI\n"
            "  %s --help   Show this help\n",
            prog, prog);
}

int main(int argc, char **argv) {
    if (argc == 1) {
        return run_tui();
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "--tui") == 0 || strcmp(argv[1], "-t") == 0) {
        return run_tui();
    }

    fprintf(stderr, "Unknown option: %s\n", argv[1]);
    usage(argv[0]);
    return 1;
}
