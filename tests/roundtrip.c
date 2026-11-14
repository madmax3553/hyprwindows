#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/actions.h"
#include "src/hyprconf.h"
#include "src/hyprctl.h"
#include "src/rules.h"
#include "src/util.h"

static int failures;

static void expect(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) {
        perror(path);
        exit(1);
    }
    fputs(text, f);
    fclose(f);
}

static void test_expand_home(void) {
    const char *home = getenv("HOME");
    if (!home) home = ".";

    char *tilde = expand_home("~/hypr");
    char *dollar = expand_home("$HOME/hypr");
    char *plain = expand_home("/tmp/hypr");
    expect(tilde && strncmp(tilde, home, strlen(home)) == 0, "tilde expands HOME");
    expect(tilde && strcmp(tilde + strlen(home), "/hypr") == 0, "tilde keeps the rest");
    expect(dollar && strcmp(dollar, tilde) == 0, "$HOME expands like tilde");
    expect(plain && strcmp(plain, "/tmp/hypr") == 0, "absolute path is unchanged");
    free(tilde);
    free(dollar);
    free(plain);
}

static void test_parser(void) {
    const char *path = "/tmp/opencode/hyprwindows-rules.lua";
    write_text(path,
               "-- header\n"
               "hl.window_rule({\n"
               "    name = \"keep-hash\",\n"
               "    match = { class = \"foo#bar\", title = \"Hello\", xwayland = true },\n"
               "    float = true,\n"
               "    size = { 800, 600 },\n"
               "})\n"
               "\n"
               "local commented = hl.window_rule({\n"
               "    name = \"commented\",\n"
               "    match = { class = \"^ok$\" }, -- trailing comment\n"
               "    workspace = 2,\n"
               "})\n");

    expect(hyprconf_file_is_rules_only(path) == 1, "pure rules file is safe to rewrite");
    expect(hyprconf_file_is_rules_only("/tmp/opencode/does-not-exist.lua") == 1,
           "missing file is safe to create");

    struct ruleset set;
    expect(hyprconf_parse_file(path, &set) == 0, "parse rules file");
    expect(set.count == 2, "two window_rule calls");
    if (set.count >= 1) {
        expect(set.rules[0].match.class_re &&
                   strcmp(set.rules[0].match.class_re, "foo#bar") == 0,
               "hash inside a string is preserved");
        expect(set.rules[0].actions.float_set && set.rules[0].actions.float_val,
               "true parses as float");
        expect(set.rules[0].actions.size && strcmp(set.rules[0].actions.size, "800 600") == 0,
               "size table flattens to a string");
        int found_extra = 0;
        for (size_t i = 0; i < set.rules[0].extras_count; i++) {
            if (set.rules[0].extras[i].key &&
                strcmp(set.rules[0].extras[i].key, "match.xwayland") == 0) {
                found_extra = 1;
            }
        }
        expect(found_extra, "unknown match field is kept in extras");
    }
    if (set.count >= 2) {
        expect(set.rules[1].match.class_re &&
                   strcmp(set.rules[1].match.class_re, "^ok$") == 0,
               "trailing comment is ignored");
        expect(set.rules[1].actions.workspace && strcmp(set.rules[1].actions.workspace, "2") == 0,
               "numeric workspace is kept");
    }

    const char *out_path = "/tmp/opencode/hyprwindows-roundtrip.lua";
    FILE *out = fopen(out_path, "w");
    expect(out != NULL, "open roundtrip output");
    if (out) {
        for (size_t i = 0; i < set.count; i++) rule_write(out, &set.rules[i]);
        fclose(out);
        struct ruleset again;
        expect(hyprconf_parse_file(out_path, &again) == 0, "reparse written rules");
        expect(again.count == set.count, "roundtrip keeps the rule count");
        if (again.count >= 1 && set.count >= 1) {
            expect(rule_match_equal(&set.rules[0], &again.rules[0]),
                   "roundtrip keeps match fields");
            expect(again.rules[0].actions.float_set && again.rules[0].actions.float_val,
                   "roundtrip keeps float");
            expect(again.rules[0].actions.size && strcmp(again.rules[0].actions.size, "800 600") == 0,
                   "roundtrip keeps size");
        }
        ruleset_free(&again);
        unlink(out_path);
    }
    ruleset_free(&set);

    const char *mixed = "/tmp/opencode/hyprwindows-mixed.lua";
    write_text(mixed,
               "hl.window_rule({\n"
               "    name = \"one\",\n"
               "    match = { class = \"^a$\" },\n"
               "})\n"
               "hl.bind(\"SUPER + Q\", hl.dsp.window.close())\n");
    expect(hyprconf_file_is_rules_only(mixed) == 0, "mixed config is not rules-only");
    unlink(mixed);
    unlink(path);
}

static void test_match(void) {
    struct rule tag_only = {0};
    tag_only.match.tag_re = strdup("+browser");
    struct rule open = {0};
    struct rule exact = {0};
    exact.match.class_re = strdup("^Firefox$");
    struct rule other = {0};
    other.match.class_re = strdup("^Firefox$");
    other.match.title_re = strdup("Private");

    struct client client = {0};
    client.class_name = strdup("Firefox");
    client.title = strdup("Mozilla Firefox");

    expect(rule_matches_client(&tag_only, &client) == 0, "tag-only rule does not match every window");
    expect(rule_matches_client(&open, &client) == 1, "rule with no match fields matches");
    expect(rule_matches_client(&exact, &client) == 1, "exact class matches");
    expect(rule_matches_client(&other, &client) == 0, "title mismatch does not match");
    expect(rule_match_equal(&exact, &exact), "a rule matches itself");
    expect(!rule_match_equal(&exact, &other), "different title is not a duplicate");
    expect(!regex_match("^Firefox$", "firefox"), "class match stays case-sensitive");

    expect(package_installed("bad;name") == 0, "package check rejects shell metacharacters");
    expect(package_installed("") == 0, "package check rejects an empty name");

    rule_free(&tag_only);
    rule_free(&exact);
    rule_free(&other);
    free(client.class_name);
    free(client.title);
}

static void test_rule_table(void) {
    const char *path = "/tmp/opencode/hyprwindows-table.lua";
    write_text(path,
               "local M = {}\n"
               "function M.setup()\n"
               "  local rules = {\n"
               "    {\n"
               "      name = \"Ghostty\",\n"
               "      match = { class = [[^([Gg]hostty|com\\.mitchellh\\.ghostty)$]] },\n"
               "      tag = \"+terminal\",\n"
               "      workspace = \"1\",\n"
               "      size = { \"70%\", \"60%\" },\n"
               "    },\n"
               "    {\n"
               "      name = \"Picture-in-Picture\",\n"
               "      match = { title = [[^(Picture-in-Picture)$]] },\n"
               "      float = true,\n"
               "      move = { \"72%\", \"7%\" },\n"
               "    },\n"
               "  }\n"
               "  for _, rule in ipairs(rules) do\n"
               "    hl.window_rule(rule)\n"
               "  end\n"
               "end\n"
               "return M.setup\n");

    struct ruleset set;
    expect(hyprconf_parse_file(path, &set) == 0, "parse rules-table module");
    expect(set.count == 2, "rules table yields two rules");
    if (set.count >= 1) {
        expect(set.rules[0].name && strcmp(set.rules[0].name, "Ghostty") == 0, "first rule name");
        expect(set.rules[0].match.class_re &&
                   strstr(set.rules[0].match.class_re, "ghostty") != NULL,
               "long-string class is kept");
        expect(set.rules[0].actions.size && strcmp(set.rules[0].actions.size, "70% 60%") == 0,
               "percent size table flattens");
    }
    if (set.count >= 2) {
        expect(set.rules[1].actions.float_set && set.rules[1].actions.float_val, "table rule float");
        expect(set.rules[1].actions.move && strcmp(set.rules[1].actions.move, "72% 7%") == 0,
               "move table flattens");
    }

    const char *copy = "/tmp/opencode/hyprwindows-table-save.lua";
    write_text(copy,
               "local M = {}\n"
               "function M.setup()\n"
               "  local rules = {\n"
               "    { name = \"Old\", match = { class = [[^old$]] } },\n"
               "  }\n"
               "  for _, rule in ipairs(rules) do\n"
               "    hl.window_rule(rule)\n"
               "  end\n"
               "end\n"
               "return M.setup\n");
    expect(hyprconf_replace_rules_table(copy, &set, NULL) == 0, "splice rules table");
    struct ruleset saved;
    expect(hyprconf_parse_file(copy, &saved) == 0, "reparse spliced module");
    expect(saved.count == set.count, "splice keeps the rule count");
    if (saved.count >= 1 && set.count >= 1) {
        expect(saved.rules[0].name && strcmp(saved.rules[0].name, "Ghostty") == 0,
               "splice writes the edited rules");
    }
    size_t slen = 0;
    char *sbuf = read_file(copy, &slen);
    expect(sbuf && strstr(sbuf, "function M.setup") && strstr(sbuf, "hl.window_rule(rule)"),
           "splice leaves the module wrapper");
    free(sbuf);
    ruleset_free(&saved);
    unlink(copy);

    ruleset_free(&set);
    unlink(path);

    char *found = hypr_find_rules_config();
    if (found && strstr(found, "windowrules.lua")) {
        expect(1, "detects configs/windowrules.lua");
    } else if (access("/home/groot/.config/hypr/hyprland.lua", R_OK) == 0) {
        expect(found && strstr(found, "windowrules.lua"), "detects configs/windowrules.lua");
    }
    free(found);

    const char *real = "/home/groot/.config/hypr/configs/windowrules.lua";
    if (access(real, R_OK) == 0) {
        struct ruleset live;
        expect(hyprconf_parse_file(real, &live) == 0, "parse live windowrules.lua");
        expect(live.count >= 20, "live rules table is not empty");
        ruleset_free(&live);
    }
}

static void test_official_shape(void) {
    const char *path = "/tmp/opencode/hyprwindows-official.lua";
    write_text(path,
               "local suppressMaximizeRule = hl.window_rule({\n"
               "    -- Ignore maximize requests from all apps.\n"
               "    name  = \"suppress-maximize-events\",\n"
               "    match = { class = \".*\" },\n"
               "    suppress_event = \"maximize\",\n"
               "})\n"
               "hl.window_rule({\n"
               "    name = \"fix-xwayland-drags\",\n"
               "    match = {\n"
               "        class = \"^$\",\n"
               "        title = \"^$\",\n"
               "        xwayland = true,\n"
               "        float = true,\n"
               "        fullscreen = false,\n"
               "    },\n"
               "    no_focus = true,\n"
               "})\n");

    struct ruleset set;
    expect(hyprconf_parse_file(path, &set) == 0, "parse official-shaped rules");
    expect(set.count == 2, "official example yields two rules");
    if (set.count >= 2) {
        expect(!set.rules[1].actions.float_set, "match.float is not the float action");
        int saw_match_float = 0;
        int saw_no_focus = 0;
        for (size_t i = 0; i < set.rules[1].extras_count; i++) {
            if (set.rules[1].extras[i].key && strcmp(set.rules[1].extras[i].key, "match.float") == 0)
                saw_match_float = 1;
            if (set.rules[1].extras[i].key && strcmp(set.rules[1].extras[i].key, "no_focus") == 0)
                saw_no_focus = 1;
        }
        expect(saw_match_float, "match.float is preserved");
        expect(saw_no_focus, "unknown action is preserved");
    }
    ruleset_free(&set);
    unlink(path);
}

static void test_detect(void) {
    const char *root = "/tmp/opencode/hypr-home";
    mkdir(root, 0755);
    mkdir("/tmp/opencode/hypr-home/.config", 0755);
    mkdir("/tmp/opencode/hypr-home/.config/hypr", 0755);
    mkdir("/tmp/opencode/hypr-home/.config/hypr/config", 0755);
    write_text("/tmp/opencode/hypr-home/.config/hypr/hyprland.lua",
               "require(\"config.rules\")\n"
               "hl.bind(\"SUPER + Q\", hl.dsp.window.close())\n");
    write_text("/tmp/opencode/hypr-home/.config/hypr/config/rules.lua",
               "hl.window_rule({ name = \"from-require\", match = { class = \"^kitty$\" } })\n");

    setenv("HOME", root, 1);
    char *found = hypr_find_rules_config();
    expect(found && strstr(found, "config/rules.lua") != NULL, "require() resolves the rules module");
    free(found);
}

int main(void) {
    test_expand_home();
    test_parser();
    test_rule_table();
    test_official_shape();
    test_match();
    test_detect();
    if (failures) {
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    printf("ok\n");
    return 0;
}
