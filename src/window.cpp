#include "ui.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QShowEvent>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QVBoxLayout>

#include <QSizePolicy>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include "actions.h"
#include "history.h"
#include "hyprconf.h"
#include "hyprctl.h"
#include "rules.h"
#include "util.h"
}

/*
 * Qt 6 Widgets window over the C rule engine. No palette and no second
 * data model. Pipeline stage is derived from the rule, not stored.
 */

enum {
    PAGE_RULES = 0,
    PAGE_WINDOWS,
    PAGE_REVIEW,
    PAGE_PIPELINE,
    PAGE_ACTIONS
};

enum rule_stage {
    STAGE_DEFAULT = 0,
    STAGE_TAG,
    STAGE_OVERRIDE
};

static int field_set(const char *s) {
    return s && s[0];
}

static int is_match_extra(const char *key) {
    return key && strncmp(key, "match.", 6) == 0;
}

static int has_match(const struct rule *r) {
    if (!r) return 0;
    if (field_set(r->match.class_re) || field_set(r->match.title_re) ||
        field_set(r->match.initial_class_re) || field_set(r->match.initial_title_re) ||
        field_set(r->match.tag_re)) {
        return 1;
    }
    for (size_t i = 0; i < r->extras_count; i++) {
        if (is_match_extra(r->extras[i].key)) return 1;
    }
    return 0;
}

static int has_nontag_action(const struct rule *r) {
    if (!r) return 0;
    if (field_set(r->actions.workspace) || field_set(r->actions.opacity) ||
        field_set(r->actions.size) || field_set(r->actions.move)) {
        return 1;
    }
    if (r->actions.float_set || r->actions.center_set || r->actions.fullscreen_set) return 1;
    for (size_t i = 0; i < r->extras_count; i++) {
        if (r->extras[i].key && !is_match_extra(r->extras[i].key)) return 1;
    }
    return 0;
}

static int stage_of(const struct rule *r) {
    if (!has_match(r)) return STAGE_DEFAULT;
    if (has_nontag_action(r)) return STAGE_OVERRIDE;
    return STAGE_TAG;
}

static void clear_str(char **p) {
    free(*p);
    *p = nullptr;
}

static void drop_extras(struct rule *r, int drop_match) {
    size_t w = 0;
    for (size_t i = 0; i < r->extras_count; i++) {
        int match = is_match_extra(r->extras[i].key);
        if (drop_match ? match : !match) {
            free(r->extras[i].key);
            free(r->extras[i].value);
            continue;
        }
        r->extras[w++] = r->extras[i];
    }
    r->extras_count = w;
}

static void clear_matches(struct rule *r) {
    clear_str(&r->match.class_re);
    clear_str(&r->match.title_re);
    clear_str(&r->match.initial_class_re);
    clear_str(&r->match.initial_title_re);
    clear_str(&r->match.tag_re);
    drop_extras(r, 1);
}

static void strip_nontag(struct rule *r) {
    clear_str(&r->actions.workspace);
    clear_str(&r->actions.opacity);
    clear_str(&r->actions.size);
    clear_str(&r->actions.move);
    r->actions.float_set = 0;
    r->actions.float_val = 0;
    r->actions.center_set = 0;
    r->actions.center_val = 0;
    r->actions.fullscreen_set = 0;
    r->actions.fullscreen_val = 0;
    drop_extras(r, 0);
}

static int same_text(const char *a, const QString &b) {
    if (!field_set(a)) return b.isEmpty();
    return b == QString::fromUtf8(a);
}

static void set_owned(char **dst, const QString &text) {
    free(*dst);
    *dst = nullptr;
    if (text.isEmpty()) return;
    QByteArray utf = text.toUtf8();
    *dst = strdup(utf.constData());
}

static int take_str(char **dst, const char *src) {
    if (field_set(*dst) || !field_set(src)) return 0;
    char *copy = strdup(src);
    if (!copy) return 0;
    free(*dst);
    *dst = copy;
    return 1;
}

static int append_extra(struct rule *dst, const char *key, const char *value) {
    if (!key) return 0;
    struct rule_extra *next = (struct rule_extra *)realloc(
        dst->extras, (dst->extras_count + 1) * sizeof(*next));
    if (!next) return 0;
    dst->extras = next;
    char *k = strdup(key);
    char *v = strdup(value ? value : "");
    if (!k || !v) {
        free(k);
        free(v);
        return 0;
    }
    dst->extras[dst->extras_count].key = k;
    dst->extras[dst->extras_count].value = v;
    dst->extras_count++;
    return 1;
}

static int merge_into(struct rule *dst, const struct rule *src) {
    int changed = 0;
    changed |= take_str(&dst->name, src->name);
    changed |= take_str(&dst->actions.tag, src->actions.tag);
    changed |= take_str(&dst->actions.workspace, src->actions.workspace);
    changed |= take_str(&dst->actions.opacity, src->actions.opacity);
    changed |= take_str(&dst->actions.size, src->actions.size);
    changed |= take_str(&dst->actions.move, src->actions.move);
    if (!dst->actions.float_set && src->actions.float_set) {
        dst->actions.float_set = 1;
        dst->actions.float_val = src->actions.float_val;
        changed = 1;
    }
    if (!dst->actions.center_set && src->actions.center_set) {
        dst->actions.center_set = 1;
        dst->actions.center_val = src->actions.center_val;
        changed = 1;
    }
    if (!dst->actions.fullscreen_set && src->actions.fullscreen_set) {
        dst->actions.fullscreen_set = 1;
        dst->actions.fullscreen_val = src->actions.fullscreen_val;
        changed = 1;
    }
    for (size_t i = 0; i < src->extras_count; i++) {
        const char *key = src->extras[i].key;
        if (!key) continue;
        int have = 0;
        for (size_t j = 0; j < dst->extras_count; j++) {
            if (dst->extras[j].key && strcmp(dst->extras[j].key, key) == 0) {
                have = 1;
                break;
            }
        }
        if (have) continue;
        changed |= append_extra(dst, key, src->extras[i].value);
    }
    return changed;
}

static QString rule_title(const struct rule *r) {
    if (field_set(r->name)) return QString::fromUtf8(r->name);
    if (field_set(r->match.class_re)) return QString::fromUtf8(r->match.class_re);
    if (field_set(r->match.title_re)) return QString::fromUtf8(r->match.title_re);
    return QStringLiteral("(unnamed)");
}

static QString rule_subtitle(const struct rule *r) {
    if (field_set(r->name) && field_set(r->match.class_re)) {
        return QString::fromUtf8(r->match.class_re);
    }
    if (!field_set(r->name) && field_set(r->match.title_re) &&
        QString::fromUtf8(r->match.title_re) != rule_title(r)) {
        return QString::fromUtf8(r->match.title_re);
    }
    if (field_set(r->actions.tag)) return QString::fromUtf8(r->actions.tag);
    if (field_set(r->match.title_re)) return QString::fromUtf8(r->match.title_re);
    return {};
}

static QString one_line(const QString &title, const QString &sub) {
    if (sub.isEmpty() || sub == title) return title;
    return title + QStringLiteral("  ·  ") + sub;
}

static QString clean_pattern(const char *re) {
    if (!field_set(re)) return {};
    QString s = QString::fromUtf8(re).trimmed();
    int bar = s.indexOf('|');
    if (bar >= 0) s = s.left(bar);
    if (s.startsWith('^')) s.remove(0, 1);
    if (s.endsWith('$')) s.chop(1);
    if (s.size() >= 2 && s.startsWith('(') && s.endsWith(')')) s = s.mid(1, s.size() - 2);
    s.replace(QStringLiteral(".*"), QString());
    s.replace(QStringLiteral(".+"), QString());
    s.replace('\\', QString());
    QString out;
    for (QChar c : s) {
        if (c.isLetterOrNumber() || c == '-' || c == '_' || c == '.' || c == ' ') out += c;
    }
    return out.trimmed();
}

static QString name_from_match(const struct rule *r) {
    const char *fields[] = {
        r->match.class_re, r->match.title_re,
        r->match.initial_class_re, r->match.initial_title_re
    };
    for (const char *field : fields) {
        QString name = clean_pattern(field);
        if (!name.isEmpty()) return name;
    }
    return {};
}

static int readable(const char *path) {
    return path && access(path, R_OK) == 0;
}

static void find_appmap(char *out, size_t n) {
    if (readable("data/appmap.json")) {
        snprintf(out, n, "%s", "data/appmap.json");
        return;
    }
    char exe[512];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len > 0 && (size_t)len < sizeof(exe) - 1) {
        exe[len] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = '\0';
            char beside[1024];
            snprintf(beside, sizeof(beside), "%s/../share/hyprwindows/appmap.json", exe);
            if (readable(beside)) {
                snprintf(out, n, "%s", beside);
                return;
            }
        }
    }
    const char *prefixes[] = {
        "/usr/local/share/hyprwindows/appmap.json",
        "/usr/share/hyprwindows/appmap.json",
    };
    for (const char *path : prefixes) {
        if (readable(path)) {
            snprintf(out, n, "%s", path);
            return;
        }
    }
    snprintf(out, n, "%s", "data/appmap.json");
}

static int run_hyprctl_reload(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp("hyprctl", "hyprctl", "reload", (char *)nullptr);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    return -1;
}

static int insert_rule(struct ruleset *set, int idx, struct rule rule) {
    if (idx < 0) idx = 0;
    if (idx > (int)set->count) idx = (int)set->count;
    struct rule *nr = (struct rule *)realloc(set->rules, (set->count + 1) * sizeof(*nr));
    if (!nr) {
        rule_free(&rule);
        return -1;
    }
    set->rules = nr;
    set->count++;
    for (int i = (int)set->count - 1; i > idx; i--) set->rules[i] = set->rules[i - 1];
    set->rules[idx] = rule;
    return idx;
}

static void remove_at(struct ruleset *set, int idx) {
    if (idx < 0 || idx >= (int)set->count) return;
    rule_free(&set->rules[idx]);
    for (int i = idx; i < (int)set->count - 1; i++) set->rules[i] = set->rules[i + 1];
    set->count--;
}

class RulesWindow : public QMainWindow {
    Q_OBJECT

public:
    RulesWindow();
    ~RulesWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void onSave();
    void reloadRules();
    void newRule();
    void deleteRule();
    void undo();
    void redo();
    void onApply();
    void onRuleSelected(QListWidgetItem *current, QListWidgetItem *previous);
    void applyFilter();
    void onPageChanged(int index);
    void onPipelineSelected();
    void moveToDefault();
    void moveToTag();
    void moveToOverride();
    void renameFromMatch();
    void mergeDuplicates();
    void reloadHyprland();
    void focusFilter();

private:
    int saveRules();
    void loadRules(bool discarded);
    void findPaths();
    int applyForm();
    void fillForm();
    void rebuildRules(int pipe_keep);
    void rebuildPipeline(int keep);
    void refreshChrome();
    void refreshWindows();
    void refreshReview();
    void updatePipelineButtons();
    void afterChange(const QString &msg, int pipe_keep);
    int pipelineIndex() const;
    bool confirm(const QString &text);
    void setStatus(const QString &text);
    QLineEdit *addLine(QFormLayout *form, const char *label);
    QWidget *buildRulesPage();
    QWidget *buildWindowsPage();
    QWidget *buildReviewPage();
    QWidget *buildPipelinePage();
    QWidget *buildActionsPage();
    std::vector<int> stageOrder() const;

    QAction *save_act = nullptr;
    QAction *undo_act = nullptr;
    QAction *redo_act = nullptr;
    QTabWidget *tabs = nullptr;
    QLineEdit *filter_edit = nullptr;
    QListWidget *rules_list = nullptr;
    QListWidget *windows_list = nullptr;
    QListWidget *review_list = nullptr;
    QListWidget *pipeline_list = nullptr;
    QPushButton *delete_btn = nullptr;
    QPushButton *apply_btn = nullptr;
    QPushButton *to_default_btn = nullptr;
    QPushButton *to_tag_btn = nullptr;
    QPushButton *to_override_btn = nullptr;
    QWidget *form_root = nullptr;
    QLineEdit *name_edit = nullptr;
    QLineEdit *class_edit = nullptr;
    QLineEdit *title_edit = nullptr;
    QLineEdit *initial_class_edit = nullptr;
    QLineEdit *initial_title_edit = nullptr;
    QLineEdit *match_tag_edit = nullptr;
    QLineEdit *tag_edit = nullptr;
    QLineEdit *workspace_edit = nullptr;
    QLineEdit *size_edit = nullptr;
    QLineEdit *move_edit = nullptr;
    QLineEdit *opacity_edit = nullptr;
    QCheckBox *float_box = nullptr;
    QCheckBox *center_box = nullptr;
    QCheckBox *fullscreen_box = nullptr;
    QLabel *extras_label = nullptr;

    struct ruleset rules {};
    struct history_stack history {};
    struct clients clients {};
    struct missing_rules missing {};
    char rules_path[1024] {};
    char appmap_path[1024] {};
    char dotfiles_path[1024] {};
    int modified = 0;
    int selected = -1;
    int form_index = -1;
    int suppress = 0;
    int shown = 0;
    int clients_loaded = 0;
    int review_loaded = 0;
};

RulesWindow::RulesWindow() {
    setWindowTitle("hyprwindows");
    resize(1080, 680);

    auto *bar = addToolBar("Main");
    bar->setMovable(false);
    save_act = bar->addAction("Save");
    auto *reload_act = bar->addAction("Reload");
    undo_act = bar->addAction("Undo");
    redo_act = bar->addAction("Redo");
    save_act->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_S));
    reload_act->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    undo_act->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Z));
    redo_act->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Y));
    connect(save_act, &QAction::triggered, this, &RulesWindow::onSave);
    connect(reload_act, &QAction::triggered, this, &RulesWindow::reloadRules);
    connect(undo_act, &QAction::triggered, this, &RulesWindow::undo);
    connect(redo_act, &QAction::triggered, this, &RulesWindow::redo);

    tabs = new QTabWidget;
    setCentralWidget(tabs);
    tabs->addTab(buildRulesPage(), "Rules");
    tabs->addTab(buildWindowsPage(), "Windows");
    tabs->addTab(buildReviewPage(), "Review");
    tabs->addTab(buildPipelinePage(), "Pipeline");
    tabs->addTab(buildActionsPage(), "Actions");

    findPaths();
    history_init(&history);
    loadRules(false);

    connect(tabs, &QTabWidget::currentChanged, this, &RulesWindow::onPageChanged);
    auto *make = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_N), this);
    connect(make, &QShortcut::activated, this, &RulesWindow::newRule);
    auto *find = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_F), this);
    connect(find, &QShortcut::activated, this, &RulesWindow::focusFilter);
}

RulesWindow::~RulesWindow() {
    ruleset_free(&rules);
    history_free(&history);
    clients_free(&clients);
    missing_rules_free(&missing);
}

QLineEdit *RulesWindow::addLine(QFormLayout *form, const char *label) {
    auto *edit = new QLineEdit;
    form->addRow(label, edit);
    return edit;
}

QWidget *RulesWindow::buildRulesPage() {
    auto *page = new QWidget;
    auto *split = new QSplitter(Qt::Horizontal);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(split);

    auto *left = new QWidget;
    auto *left_lay = new QVBoxLayout(left);
    filter_edit = new QLineEdit;
    filter_edit->setPlaceholderText("Filter rules");
    connect(filter_edit, &QLineEdit::textChanged, this, &RulesWindow::applyFilter);
    rules_list = new QListWidget;
    rules_list->setSelectionMode(QAbstractItemView::SingleSelection);
    rules_list->setTextElideMode(Qt::ElideRight);
    rules_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(rules_list, &QListWidget::currentItemChanged, this, &RulesWindow::onRuleSelected);
    connect(rules_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        onRuleSelected(item, nullptr);
    });

    auto *buttons = new QHBoxLayout;
    auto *new_btn = new QPushButton("New");
    delete_btn = new QPushButton("Delete");
    connect(new_btn, &QPushButton::clicked, this, &RulesWindow::newRule);
    connect(delete_btn, &QPushButton::clicked, this, &RulesWindow::deleteRule);
    buttons->addWidget(new_btn);
    buttons->addWidget(delete_btn);
    buttons->addStretch();
    left_lay->addWidget(filter_edit);
    left_lay->addWidget(rules_list, 1);
    left_lay->addLayout(buttons);

    form_root = new QWidget;
    auto *form = new QFormLayout(form_root);
    name_edit = addLine(form, "Name");
    class_edit = addLine(form, "Class");
    title_edit = addLine(form, "Title");
    initial_class_edit = addLine(form, "Initial class");
    initial_title_edit = addLine(form, "Initial title");
    match_tag_edit = addLine(form, "Match tag");
    tag_edit = addLine(form, "Tag");
    workspace_edit = addLine(form, "Workspace");
    size_edit = addLine(form, "Size");
    move_edit = addLine(form, "Move");
    opacity_edit = addLine(form, "Opacity");

    auto *checks = new QHBoxLayout;
    float_box = new QCheckBox("Float");
    center_box = new QCheckBox("Center");
    fullscreen_box = new QCheckBox("Fullscreen");
    checks->addWidget(float_box);
    checks->addWidget(center_box);
    checks->addWidget(fullscreen_box);
    checks->addStretch();
    form->addRow(checks);

    extras_label = new QLabel("No extra fields");
    extras_label->setWordWrap(true);
    extras_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(extras_label);

    apply_btn = new QPushButton("Apply");
    connect(apply_btn, &QPushButton::clicked, this, &RulesWindow::onApply);
    form->addRow(apply_btn);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(form_root);
    split->addWidget(left);
    split->addWidget(scroll);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({340, 700});
    return page;
}

QWidget *RulesWindow::buildWindowsPage() {
    windows_list = new QListWidget;
    windows_list->setWordWrap(true);
    windows_list->setSelectionMode(QAbstractItemView::NoSelection);
    return windows_list;
}

QWidget *RulesWindow::buildReviewPage() {
    review_list = new QListWidget;
    review_list->setWordWrap(true);
    review_list->setSelectionMode(QAbstractItemView::NoSelection);
    return review_list;
}

QWidget *RulesWindow::buildPipelinePage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);
    auto *hint = new QLabel(
        "Stage is derived from the rule. Save writes default, then tag, then override.");
    hint->setWordWrap(true);
    pipeline_list = new QListWidget;
    pipeline_list->setWordWrap(true);
    connect(pipeline_list, &QListWidget::currentItemChanged, this, &RulesWindow::onPipelineSelected);

    to_default_btn = new QPushButton("Make default");
    to_tag_btn = new QPushButton("Make tag");
    to_override_btn = new QPushButton("Make override");
    to_default_btn->setToolTip("Clear match fields. The rule applies to every window.");
    to_tag_btn->setToolTip("Keep the match and the tag. Remove every other action.");
    to_override_btn->setToolTip(
        "Override is a match plus an action other than tag. "
        "A tag-only rule cannot be moved here without adding an action.");
    connect(to_default_btn, &QPushButton::clicked, this, &RulesWindow::moveToDefault);
    connect(to_tag_btn, &QPushButton::clicked, this, &RulesWindow::moveToTag);
    connect(to_override_btn, &QPushButton::clicked, this, &RulesWindow::moveToOverride);

    auto *row = new QHBoxLayout;
    row->addWidget(to_default_btn);
    row->addWidget(to_tag_btn);
    row->addWidget(to_override_btn);
    row->addStretch();
    lay->addWidget(hint);
    lay->addWidget(pipeline_list, 1);
    lay->addLayout(row);
    return page;
}

QWidget *RulesWindow::buildActionsPage() {
    auto *page = new QWidget;
    auto *lay = new QVBoxLayout(page);

    auto *rename_note = new QLabel(
        "Rename rules from the class or title pattern. A rule that already has that name is left alone.");
    rename_note->setWordWrap(true);
    auto *rename_btn = new QPushButton("Rename from class or title");
    connect(rename_btn, &QPushButton::clicked, this, &RulesWindow::renameFromMatch);

    auto *merge_note = new QLabel(
        "Merge rules with the same match fields. The first rule keeps its actions and fills blanks from the others.");
    merge_note->setWordWrap(true);
    auto *merge_btn = new QPushButton("Merge identical matches");
    connect(merge_btn, &QPushButton::clicked, this, &RulesWindow::mergeDuplicates);

    auto *reload_note = new QLabel(
        "Ask Hyprland to reload its config. This does not save the rules file.");
    reload_note->setWordWrap(true);
    auto *reload_btn = new QPushButton("hyprctl reload");
    connect(reload_btn, &QPushButton::clicked, this, &RulesWindow::reloadHyprland);

    lay->addWidget(rename_note);
    lay->addWidget(rename_btn);
    lay->addSpacing(12);
    lay->addWidget(merge_note);
    lay->addWidget(merge_btn);
    lay->addSpacing(12);
    lay->addWidget(reload_note);
    lay->addWidget(reload_btn);
    lay->addStretch();
    rename_btn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    merge_btn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    reload_btn->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return page;
}

void RulesWindow::setStatus(const QString &text) {
    statusBar()->showMessage(text);
}

bool RulesWindow::confirm(const QString &text) {
    return QMessageBox::question(this, "hyprwindows", text,
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
}

void RulesWindow::findPaths() {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char *detected = hypr_find_rules_config();
    if (detected) {
        snprintf(rules_path, sizeof(rules_path), "%s", detected);
        free(detected);
    } else {
        snprintf(rules_path, sizeof(rules_path), "%s/.config/hypr/rules.lua", home);
    }
    snprintf(dotfiles_path, sizeof(dotfiles_path), "%s/dotfiles", home);
    find_appmap(appmap_path, sizeof(appmap_path));
}

int RulesWindow::pipelineIndex() const {
    if (!pipeline_list) return -1;
    auto *item = pipeline_list->currentItem();
    if (!item) return -1;
    bool ok = false;
    int idx = item->data(Qt::UserRole).toInt(&ok);
    if (!ok || idx < 0 || idx >= (int)rules.count) return -1;
    return idx;
}

void RulesWindow::refreshChrome() {
    setWindowTitle(modified ? "hyprwindows *" : "hyprwindows");
    if (save_act) save_act->setEnabled(modified);
    if (undo_act) undo_act->setEnabled(history_can_undo(&history));
    if (redo_act) redo_act->setEnabled(history_can_redo(&history));
    int have = selected >= 0 && selected < (int)rules.count;
    if (delete_btn) delete_btn->setEnabled(have);
    if (apply_btn) apply_btn->setEnabled(have);
    if (form_root) form_root->setEnabled(have);
}

void RulesWindow::fillForm() {
    suppress = 1;
    auto set = [](QLineEdit *edit, const char *text) {
        edit->setText(text ? QString::fromUtf8(text) : QString());
    };
    if (selected < 0 || selected >= (int)rules.count) {
        set(name_edit, nullptr);
        set(class_edit, nullptr);
        set(title_edit, nullptr);
        set(initial_class_edit, nullptr);
        set(initial_title_edit, nullptr);
        set(match_tag_edit, nullptr);
        set(tag_edit, nullptr);
        set(workspace_edit, nullptr);
        set(size_edit, nullptr);
        set(move_edit, nullptr);
        set(opacity_edit, nullptr);
        float_box->setChecked(false);
        center_box->setChecked(false);
        fullscreen_box->setChecked(false);
        extras_label->setText("No extra fields");
        form_index = -1;
        suppress = 0;
        refreshChrome();
        return;
    }

    const struct rule *r = &rules.rules[selected];
    set(name_edit, r->name);
    set(class_edit, r->match.class_re);
    set(title_edit, r->match.title_re);
    set(initial_class_edit, r->match.initial_class_re);
    set(initial_title_edit, r->match.initial_title_re);
    set(match_tag_edit, r->match.tag_re);
    set(tag_edit, r->actions.tag);
    set(workspace_edit, r->actions.workspace);
    set(size_edit, r->actions.size);
    set(move_edit, r->actions.move);
    set(opacity_edit, r->actions.opacity);
    float_box->setChecked(r->actions.float_set && r->actions.float_val);
    center_box->setChecked(r->actions.center_set && r->actions.center_val);
    fullscreen_box->setChecked(r->actions.fullscreen_set && r->actions.fullscreen_val);

    QString extra;
    for (size_t i = 0; i < r->extras_count; i++) {
        if (!extra.isEmpty()) extra += '\n';
        extra += QString::fromUtf8(r->extras[i].key ? r->extras[i].key : "?");
        extra += " = ";
        extra += QString::fromUtf8(r->extras[i].value ? r->extras[i].value : "");
    }
    extras_label->setText(extra.isEmpty() ? QStringLiteral("No extra fields") : extra);
    form_index = selected;
    suppress = 0;
    refreshChrome();
}

int RulesWindow::applyForm() {
    if (suppress) return 0;
    if (form_index < 0 || form_index != selected) return 0;
    if (selected < 0 || selected >= (int)rules.count) return 0;
    struct rule *r = &rules.rules[selected];

    int float_on = float_box->isChecked();
    int center_on = center_box->isChecked();
    int full_on = fullscreen_box->isChecked();
    int changed = !same_text(r->name, name_edit->text()) ||
                  !same_text(r->match.class_re, class_edit->text()) ||
                  !same_text(r->match.title_re, title_edit->text()) ||
                  !same_text(r->match.initial_class_re, initial_class_edit->text()) ||
                  !same_text(r->match.initial_title_re, initial_title_edit->text()) ||
                  !same_text(r->match.tag_re, match_tag_edit->text()) ||
                  !same_text(r->actions.tag, tag_edit->text()) ||
                  !same_text(r->actions.workspace, workspace_edit->text()) ||
                  !same_text(r->actions.size, size_edit->text()) ||
                  !same_text(r->actions.move, move_edit->text()) ||
                  !same_text(r->actions.opacity, opacity_edit->text()) ||
                  float_on != (r->actions.float_set && r->actions.float_val) ||
                  center_on != (r->actions.center_set && r->actions.center_val) ||
                  full_on != (r->actions.fullscreen_set && r->actions.fullscreen_val);
    if (!changed) return 0;

    struct rule old = rule_copy(r);
    set_owned(&r->name, name_edit->text());
    set_owned(&r->match.class_re, class_edit->text());
    set_owned(&r->match.title_re, title_edit->text());
    set_owned(&r->match.initial_class_re, initial_class_edit->text());
    set_owned(&r->match.initial_title_re, initial_title_edit->text());
    set_owned(&r->match.tag_re, match_tag_edit->text());
    set_owned(&r->actions.tag, tag_edit->text());
    set_owned(&r->actions.workspace, workspace_edit->text());
    set_owned(&r->actions.size, size_edit->text());
    set_owned(&r->actions.move, move_edit->text());
    set_owned(&r->actions.opacity, opacity_edit->text());
    r->actions.float_set = r->actions.float_set || float_on;
    r->actions.float_val = float_on;
    r->actions.center_set = r->actions.center_set || center_on;
    r->actions.center_val = center_on;
    if (full_on) {
        if (!r->actions.fullscreen_set || r->actions.fullscreen_val == 0) {
            r->actions.fullscreen_set = 1;
            r->actions.fullscreen_val = 1;
        }
    } else if (r->actions.fullscreen_set) {
        r->actions.fullscreen_val = 0;
    }
    history_record(&history, CHANGE_EDIT, selected, selected, &old, r, "Edit rule");
    rule_free(&old);
    modified = 1;
    review_loaded = 0;
    refreshChrome();
    return 1;
}

void RulesWindow::rebuildPipeline(int keep) {
    if (keep < 0 || keep >= (int)rules.count) keep = -1;
    suppress = 1;
    pipeline_list->clear();
    const char *names[] = {"Default", "Tag", "Override"};
    const char *hints[] = {
        "No match. Applied to every window.",
        "Matches a window and only sets a tag.",
        "Matches a window and changes it.",
    };
    for (int stage = 0; stage < 3; stage++) {
        auto *header = new QListWidgetItem(QString("%1 — %2").arg(names[stage], hints[stage]));
        header->setFlags(Qt::ItemIsEnabled);
        QFont font = header->font();
        font.setBold(true);
        header->setFont(font);
        header->setData(Qt::UserRole, -1);
        pipeline_list->addItem(header);

        int n = 0;
        for (size_t i = 0; i < rules.count; i++) {
            if (stage_of(&rules.rules[i]) != stage) continue;
            auto *item = new QListWidgetItem(one_line(rule_title(&rules.rules[i]),
                                                      rule_subtitle(&rules.rules[i])));
            item->setData(Qt::UserRole, (int)i);
            pipeline_list->addItem(item);
            if ((int)i == keep) pipeline_list->setCurrentItem(item);
            n++;
        }
        if (n == 0) {
            auto *empty = new QListWidgetItem("  (none)");
            empty->setFlags(Qt::NoItemFlags);
            empty->setData(Qt::UserRole, -1);
            pipeline_list->addItem(empty);
        }
    }
    suppress = 0;
    updatePipelineButtons();
}

void RulesWindow::rebuildRules(int pipe_keep) {
    suppress = 1;
    rules_list->clear();
    QListWidgetItem *match = nullptr;
    if (rules.count == 0) {
        selected = -1;
    } else {
        if (selected < 0) selected = 0;
        if (selected >= (int)rules.count) selected = (int)rules.count - 1;
    }
    for (size_t i = 0; i < rules.count; i++) {
        const struct rule *r = &rules.rules[i];
        auto *item = new QListWidgetItem(one_line(rule_title(r), rule_subtitle(r)));
        item->setData(Qt::UserRole, (int)i);
        item->setToolTip(rule_subtitle(r));
        rules_list->addItem(item);
        if ((int)i == selected) match = item;
    }
    if (match) {
        rules_list->setCurrentItem(match);
        rules_list->scrollToItem(match);
    }
    suppress = 0;
    applyFilter();
    fillForm();
    rebuildPipeline(pipe_keep);
}

void RulesWindow::applyFilter() {
    if (!rules_list || !filter_edit) return;
    suppress = 1;
    const QString q = filter_edit->text();
    for (int i = 0; i < rules_list->count(); i++) {
        auto *item = rules_list->item(i);
        int idx = item->data(Qt::UserRole).toInt();
        bool show = false;
        if (idx >= 0 && idx < (int)rules.count) {
            if (q.isEmpty()) {
                show = true;
            } else {
                const struct rule *r = &rules.rules[idx];
                const char *fields[] = {
                    r->name, r->match.class_re, r->match.title_re,
                    r->match.initial_class_re, r->match.initial_title_re,
                    r->match.tag_re, r->actions.tag, r->actions.workspace
                };
                for (const char *field : fields) {
                    if (field && QString::fromUtf8(field).contains(q, Qt::CaseInsensitive)) {
                        show = true;
                        break;
                    }
                }
            }
        }
        item->setHidden(!show);
    }
    suppress = 0;
}

void RulesWindow::updatePipelineButtons() {
    int idx = pipelineIndex();
    bool have = idx >= 0;
    if (to_default_btn) to_default_btn->setEnabled(have && has_match(&rules.rules[idx]));
    if (to_tag_btn) {
        to_tag_btn->setEnabled(have && has_match(&rules.rules[idx]) &&
                               has_nontag_action(&rules.rules[idx]));
    }
    if (to_override_btn) to_override_btn->setEnabled(have);
}

void RulesWindow::afterChange(const QString &msg, int pipe_keep) {
    modified = 1;
    review_loaded = 0;
    rebuildRules(pipe_keep);
    setStatus(msg);
}

void RulesWindow::loadRules(bool discarded) {
    ruleset_free(&rules);
    history_free(&history);
    history_init(&history);
    package_cache_clear();
    clients_free(&clients);
    missing_rules_free(&missing);
    clients_loaded = 0;
    review_loaded = 0;
    selected = 0;

    char *path = expand_home(rules_path);
    int rc = ruleset_load(path ? path : rules_path, &rules);
    free(path);
    modified = 0;
    rebuildRules(-1);
    if (discarded) {
        setStatus(QString("Reloaded %1 — unsaved edits discarded").arg(rules_path));
    } else if (rc == 0) {
        setStatus(QString("Loaded %1 rules from %2")
                      .arg(static_cast<qulonglong>(rules.count))
                      .arg(rules_path));
    } else {
        setStatus(QString("No rules in %1").arg(rules_path));
    }
}

std::vector<int> RulesWindow::stageOrder() const {
    std::vector<int> order;
    order.reserve(rules.count);
    for (int stage = 0; stage < 3; stage++) {
        for (size_t i = 0; i < rules.count; i++) {
            if (stage_of(&rules.rules[i]) == stage) order.push_back((int)i);
        }
    }
    return order;
}

int RulesWindow::saveRules() {
    int edited = applyForm();
    char *expanded = expand_home(rules_path);
    const char *path = expanded ? expanded : rules_path;
    std::vector<int> order = stageOrder();
    const int *order_ptr = order.empty() ? nullptr : order.data();

    if (hyprconf_replace_rules_table(path, &rules, order_ptr) == 0) {
        free(expanded);
        modified = 0;
        if (edited) rebuildRules(pipelineIndex());
        else refreshChrome();
        setStatus(QString("Saved %1 rules to %2")
                      .arg(static_cast<qulonglong>(rules.count))
                      .arg(rules_path));
        return 0;
    }
    if (!hyprconf_file_is_rules_only(path)) {
        free(expanded);
        if (edited) rebuildRules(pipelineIndex());
        setStatus(QString("Refusing to overwrite %1 — not a rules file").arg(rules_path));
        return -1;
    }

    char tmp[1100];
    size_t plen = strlen(path);
    if (plen + 5 >= sizeof(tmp)) {
        free(expanded);
        setStatus("Rules path too long to save");
        return -1;
    }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(expanded);
        setStatus("Failed to save rules");
        return -1;
    }
    fputs("-- Window rules - managed by hyprwindows\n\n", f);
    for (int idx : order) rule_write(f, &rules.rules[idx]);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        unlink(tmp);
        free(expanded);
        setStatus("Failed to save rules");
        return -1;
    }
    fclose(f);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        free(expanded);
        setStatus(QString("Failed to replace %1").arg(rules_path));
        return -1;
    }
    free(expanded);
    modified = 0;
    if (edited) rebuildRules(pipelineIndex());
    else refreshChrome();
    setStatus(QString("Saved %1 rules to %2")
                  .arg(static_cast<qulonglong>(rules.count))
                  .arg(rules_path));
    return 0;
}

void RulesWindow::onSave() {
    saveRules();
}

void RulesWindow::reloadRules() {
    int dirty = modified;
    loadRules(dirty);
}

void RulesWindow::newRule() {
    applyForm();
    struct rule blank {};
    blank.name = strdup("new-rule");
    if (!blank.name) return;
    int idx = insert_rule(&rules, (int)rules.count, blank);
    if (idx < 0) return;
    selected = idx;
    afterChange("New rule — not saved", idx);
    tabs->setCurrentIndex(PAGE_RULES);
    name_edit->setFocus();
    name_edit->selectAll();
}

void RulesWindow::deleteRule() {
    if (selected < 0 || selected >= (int)rules.count) return;
    struct rule copy = rule_copy(&rules.rules[selected]);
    history_record(&history, CHANGE_DELETE, selected, selected, &copy, nullptr, "Delete rule");
    rule_free(&copy);
    remove_at(&rules, selected);
    if (selected >= (int)rules.count) selected = (int)rules.count - 1;
    afterChange("Rule deleted — not saved", -1);
}

void RulesWindow::undo() {
    int idx = -1;
    int file_order = -1;
    enum change_type type = CHANGE_EDIT;
    struct rule *restored = history_undo(&history, &idx, &type, &file_order);
    if (!restored) {
        setStatus("Nothing to undo");
        return;
    }
    if (type == CHANGE_DELETE) {
        int at = insert_rule(&rules, idx, *restored);
        free(restored);
        if (at < 0) return;
        selected = at;
    } else if (idx >= 0 && idx < (int)rules.count) {
        rule_free(&rules.rules[idx]);
        rules.rules[idx] = *restored;
        free(restored);
        selected = idx;
    } else {
        rule_free(restored);
        free(restored);
        return;
    }
    afterChange("Undo", selected);
}

void RulesWindow::redo() {
    int idx = -1;
    int file_order = -1;
    enum change_type type = CHANGE_EDIT;
    struct rule *restored = history_redo(&history, &idx, &type, &file_order);
    if (!restored) {
        setStatus("Nothing to redo");
        return;
    }
    if (type == CHANGE_DELETE) {
        rule_free(restored);
        free(restored);
        if (idx >= 0 && idx < (int)rules.count) remove_at(&rules, idx);
        if (selected >= (int)rules.count) selected = (int)rules.count - 1;
    } else if (idx >= 0 && idx < (int)rules.count) {
        rule_free(&rules.rules[idx]);
        rules.rules[idx] = *restored;
        free(restored);
        selected = idx;
    } else {
        rule_free(restored);
        free(restored);
        return;
    }
    afterChange("Redo", selected);
}

void RulesWindow::onApply() {
    if (!applyForm()) {
        setStatus("No changes");
        return;
    }
    rebuildRules(pipelineIndex());
    setStatus("Rule updated — not saved");
}

void RulesWindow::onRuleSelected(QListWidgetItem *current, QListWidgetItem *) {
    if (suppress) return;
    int prev = selected;
    int pipe = pipelineIndex();
    if (applyForm() && prev >= 0 && prev < (int)rules.count) {
        for (int i = 0; i < rules_list->count(); i++) {
            auto *item = rules_list->item(i);
            if (item->data(Qt::UserRole).toInt() == prev) {
                const struct rule *r = &rules.rules[prev];
                item->setText(one_line(rule_title(r), rule_subtitle(r)));
                item->setToolTip(rule_subtitle(r));
                break;
            }
        }
        rebuildPipeline(pipe);
    }
    if (!current) return;
    selected = current->data(Qt::UserRole).toInt();
    fillForm();
}

void RulesWindow::showEvent(QShowEvent *event) {
    QMainWindow::showEvent(event);
    if (shown) return;
    shown = 1;
    fillForm();
}

void RulesWindow::onPageChanged(int index) {
    if (index != PAGE_RULES && applyForm()) {
        int sel = selected;
        rebuildRules(pipelineIndex());
        selected = sel;
    }
    if (index == PAGE_WINDOWS) refreshWindows();
    else if (index == PAGE_REVIEW && !review_loaded) refreshReview();
}

void RulesWindow::onPipelineSelected() {
    if (suppress) return;
    updatePipelineButtons();
}

void RulesWindow::moveToDefault() {
    int idx = pipelineIndex();
    if (idx < 0) return;
    struct rule *r = &rules.rules[idx];
    if (!has_match(r)) {
        setStatus("Already a default rule");
        return;
    }
    struct rule old = rule_copy(r);
    clear_matches(r);
    history_record(&history, CHANGE_EDIT, idx, idx, &old, r, "Move to default");
    rule_free(&old);
    selected = idx;
    afterChange("Moved to default — not saved", idx);
}

void RulesWindow::moveToTag() {
    int idx = pipelineIndex();
    if (idx < 0) return;
    struct rule *r = &rules.rules[idx];
    if (!has_match(r)) {
        setStatus("A tag rule needs a match");
        return;
    }
    if (!has_nontag_action(r)) {
        setStatus("Already a tag rule");
        return;
    }
    struct rule old = rule_copy(r);
    strip_nontag(r);
    history_record(&history, CHANGE_EDIT, idx, idx, &old, r, "Move to tag");
    rule_free(&old);
    selected = idx;
    afterChange("Moved to tag — not saved", idx);
}

void RulesWindow::moveToOverride() {
    int idx = pipelineIndex();
    if (idx < 0) return;
    const struct rule *r = &rules.rules[idx];
    if (!has_match(r)) {
        setStatus("An override needs a match");
        return;
    }
    if (!has_nontag_action(r)) {
        setStatus("An override needs an action other than a tag. Add one on the Rules page.");
        return;
    }
    setStatus("Already an override");
}

void RulesWindow::renameFromMatch() {
    int count = 0;
    for (size_t i = 0; i < rules.count; i++) {
        QString name = name_from_match(&rules.rules[i]);
        if (name.isEmpty() || same_text(rules.rules[i].name, name)) continue;
        count++;
    }
    if (count == 0) {
        setStatus("No rules to rename");
        return;
    }
    if (!confirm(QString("Rename %1 rules from their class or title? Existing names are replaced. This is not saved until you save.")
                     .arg(count))) {
        return;
    }
    int renamed = 0;
    for (size_t i = 0; i < rules.count; i++) {
        QString name = name_from_match(&rules.rules[i]);
        if (name.isEmpty() || same_text(rules.rules[i].name, name)) continue;
        struct rule old = rule_copy(&rules.rules[i]);
        set_owned(&rules.rules[i].name, name);
        history_record(&history, CHANGE_EDIT, (int)i, (int)i, &old, &rules.rules[i], "Rename rule");
        rule_free(&old);
        renamed++;
    }
    afterChange(QString("Renamed %1 rules — not saved").arg(renamed), pipelineIndex());
}

void RulesWindow::mergeDuplicates() {
    std::vector<int> used(rules.count, 0);
    struct group {
        int keeper;
        std::vector<int> drop;
    };
    std::vector<group> groups;
    for (size_t i = 0; i < rules.count; i++) {
        if (used[i]) continue;
        group g;
        g.keeper = (int)i;
        for (size_t j = i + 1; j < rules.count; j++) {
            if (used[j]) continue;
            if (!rule_match_equal(&rules.rules[i], &rules.rules[j])) continue;
            g.drop.push_back((int)j);
            used[j] = 1;
        }
        if (!g.drop.empty()) groups.push_back(g);
    }
    if (groups.empty()) {
        setStatus("No duplicate match fields");
        return;
    }
    int drop_count = 0;
    for (const group &g : groups) drop_count += (int)g.drop.size();
    if (!confirm(QString("Merge %1 rules into %2? The first of each match keeps its actions and fills blanks from the others. This is not saved until you save.")
                     .arg(drop_count + (int)groups.size())
                     .arg((int)groups.size()))) {
        return;
    }

    for (const group &g : groups) {
        struct rule old = rule_copy(&rules.rules[g.keeper]);
        int changed = 0;
        for (int idx : g.drop) changed |= merge_into(&rules.rules[g.keeper], &rules.rules[idx]);
        if (changed) {
            history_record(&history, CHANGE_EDIT, g.keeper, g.keeper, &old, &rules.rules[g.keeper],
                           "Merge rules");
        }
        rule_free(&old);
    }

    std::vector<int> drops;
    for (const group &g : groups) drops.insert(drops.end(), g.drop.begin(), g.drop.end());
    std::sort(drops.begin(), drops.end(), [](int a, int b) { return a > b; });
    for (int idx : drops) {
        struct rule copy = rule_copy(&rules.rules[idx]);
        history_record(&history, CHANGE_DELETE, idx, idx, &copy, nullptr, "Merge rules");
        rule_free(&copy);
        remove_at(&rules, idx);
        if (selected > idx) selected--;
    }
    if (selected >= (int)rules.count) selected = (int)rules.count - 1;
    afterChange(QString("Merged %1 duplicate rules — not saved").arg(drop_count), -1);
}

void RulesWindow::reloadHyprland() {
    QString text = "Reload the running Hyprland config with hyprctl reload?";
    if (modified) text += " Unsaved rule edits are not included.";
    if (!confirm(text)) return;
    if (run_hyprctl_reload() == 0) setStatus("Hyprland reloaded");
    else setStatus("hyprctl reload failed");
}

void RulesWindow::focusFilter() {
    tabs->setCurrentIndex(PAGE_RULES);
    filter_edit->setFocus();
    filter_edit->selectAll();
}

void RulesWindow::refreshWindows() {
    clients_free(&clients);
    hyprctl_clients(&clients);
    clients_loaded = 1;
    windows_list->clear();
    if (clients.count == 0) {
        windows_list->addItem("No open windows");
        setStatus("No open windows");
        return;
    }
    for (size_t i = 0; i < clients.count; i++) {
        const struct client *c = &clients.items[i];
        QStringList names;
        for (size_t r = 0; r < rules.count; r++) {
            if (!rule_matches_client(&rules.rules[r], c)) continue;
            names << rule_title(&rules.rules[r]);
        }
        QString matches = names.isEmpty() ? QStringLiteral("no rule") : names.join(", ");
        if (matches.size() > 180) matches = matches.left(177) + "...";
        QString title = c->class_name ? QString::fromUtf8(c->class_name) : QStringLiteral("(no class)");
        QString sub = QString("%1  ·  %2").arg(c->title ? QString::fromUtf8(c->title) : QString(), matches);
        windows_list->addItem(title + "\n" + sub);
    }
    setStatus(QString("%1 open windows").arg(static_cast<qulonglong>(clients.count)));
}

void RulesWindow::refreshReview() {
    if (!clients_loaded) refreshWindows();
    missing_rules_free(&missing);
    setStatus("Scanning installed apps…");
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    char *appmap = expand_home(appmap_path);
    find_missing_rules(&rules, appmap ? appmap : appmap_path, dotfiles_path, &missing);
    free(appmap);
    review_loaded = 1;

    review_list->clear();
    int inactive = 0;
    for (size_t i = 0; i < rules.count; i++) {
        int hit = 0;
        for (size_t c = 0; c < clients.count; c++) {
            if (rule_matches_client(&rules.rules[i], &clients.items[c])) {
                hit = 1;
                break;
            }
        }
        if (hit) continue;
        inactive++;
        review_list->addItem(rule_title(&rules.rules[i]) + "\nNot matching an open window");
    }
    for (size_t i = 0; i < missing.count; i++) {
        const struct missing_rule *m = &missing.items[i];
        QString title = m->app_name ? QString::fromUtf8(m->app_name) : QStringLiteral("(app)");
        QString sub = QString("Missing  ·  %1  ·  %2")
                          .arg(m->source ? QString::fromUtf8(m->source) : QStringLiteral("app"),
                               m->class_pattern ? QString::fromUtf8(m->class_pattern) : QString());
        review_list->addItem(title + "\n" + sub);
    }
    if (inactive == 0 && missing.count == 0) {
        review_list->addItem(
            "Nothing to review\nEvery rule matches an open window, and no installed app is missing one");
    }
    setStatus(QString("%1 unmatched  ·  %2 missing")
                  .arg(inactive)
                  .arg(static_cast<qulonglong>(missing.count)));
}

void RulesWindow::closeEvent(QCloseEvent *event) {
    if (!modified) {
        event->accept();
        return;
    }
    auto choice = QMessageBox::question(
        this, "hyprwindows", "Save changes before quitting?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (choice == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    if (choice == QMessageBox::Save && saveRules() != 0) {
        event->ignore();
        return;
    }
    modified = 0;
    event->accept();
}

extern "C" int run_gui(void) {
    int argc = 1;
    char arg0[] = "hyprwindows";
    char *argv[] = {arg0, nullptr};
    QApplication app(argc, argv);
    app.setApplicationName("hyprwindows");
    RulesWindow win;
    win.show();
    return app.exec() == 0 ? 0 : 1;
}

#include "window.moc"
