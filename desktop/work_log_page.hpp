// desktop/work_log_page.hpp -- all the work, readable inside ALTAIR.
//
// Smit asked that all the work done be visible. It is spread over the tree:
// the status page and research notes (docs/), the runbooks (ops/), every
// run's results (data/verified/), the daily logs and findings (data/logs/),
// and the history of every change (git). This page lists them in one tree,
// searchable, and shows the one picked: Markdown rendered, text as text, a
// CSV as a table. The change history comes from `git log` when git is
// installed; without it that section says so.
//
// Reads; changes nothing. Opening a file outside the page uses the system's
// own viewer.
#pragma once

#include "demo_trading_page.hpp"
#include "theme.hpp"

#include <QDateTime>
#include <QDesktopServices>
#include <QDirIterator>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QShowEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableView>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <vector>

namespace altair::ui {

/// One readable piece of work.
struct WorkItem {
    QString section;   ///< "Status", "Research notes", "Runbooks", "Results", "Daily logs"
    QString label;     ///< relative path, as shown
    QString path;      ///< absolute
    QDateTime when;
};

/// Everything readable under `root`, by section, newest first inside a section.
[[nodiscard]] inline std::vector<WorkItem> find_work(const QString& root) {
    std::vector<WorkItem> out;
    const QDir base(root);
    const auto add_dir = [&](const QString& section, const QString& rel, const QStringList& patterns, int limit) {
        std::vector<WorkItem> part;
        QDirIterator it(root + QLatin1Char('/') + rel, patterns, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString p = it.next();
            // Third-party clones kept for reference are not ALTAIR's work.
            if (p.contains(QLatin1String("/research/reference/"))) continue;
            const QFileInfo f(p);
            part.push_back({section, base.relativeFilePath(p), f.absoluteFilePath(), f.lastModified()});
        }
        std::sort(part.begin(), part.end(), [](const WorkItem& a, const WorkItem& b) {
            return a.when != b.when ? a.when > b.when : a.label < b.label;
        });
        if (static_cast<int>(part.size()) > limit) part.resize(static_cast<std::size_t>(limit));
        for (auto& w : part) out.push_back(std::move(w));
    };
    const auto add_file = [&](const QString& section, const QString& rel) {
        const QFileInfo f(root + QLatin1Char('/') + rel);
        if (f.exists()) out.push_back({section, rel, f.absoluteFilePath(), f.lastModified()});
    };
    add_file(QStringLiteral("Status"), QStringLiteral("docs/STATUS.md"));
    add_file(QStringLiteral("Status"), QStringLiteral("README.md"));
    add_file(QStringLiteral("Status"), QStringLiteral("docs/README.md"));
    add_dir(QStringLiteral("Research notes"), QStringLiteral("docs/research"), {QStringLiteral("*.md")}, 200);
    add_dir(QStringLiteral("Research notes"), QStringLiteral("research"), {QStringLiteral("*.md")}, 200);
    add_dir(QStringLiteral("Runbooks"), QStringLiteral("ops"), {QStringLiteral("*.md")}, 200);
    add_dir(QStringLiteral("Results"), QStringLiteral("data/verified"),
            {QStringLiteral("*.md"), QStringLiteral("*.txt"), QStringLiteral("*.csv"), QStringLiteral("*.json")}, 400);
    add_dir(QStringLiteral("Daily logs"), QStringLiteral("data/logs"),
            {QStringLiteral("*.md"), QStringLiteral("*.txt"), QStringLiteral("*.log"), QStringLiteral("*.csv")}, 400);
    return out;
}

/// The change history: one line per commit, newest first, from git. Empty
/// with `why` set when git is missing or the tree is not a repository.
[[nodiscard]] inline QStringList git_history(const QString& root, QString* why, int limit = 400) {
    const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
    if (git.isEmpty()) {
        if (why != nullptr) *why = QStringLiteral("git is not installed on this PC, so the change history cannot be read.");
        return {};
    }
    QProcess p;
    p.start(git, {QStringLiteral("-C"), root, QStringLiteral("log"), QStringLiteral("--date=short"),
                  QStringLiteral("--format=%h|%ad|%an|%s"), QStringLiteral("-n"), QString::number(limit)});
    if (!p.waitForFinished(8000) || p.exitCode() != 0) {
        if (why != nullptr) *why = QStringLiteral("git log did not answer: %1").arg(QString::fromUtf8(p.readAllStandardError()).trimmed());
        p.kill();
        return {};
    }
    return QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

class WorkLogPage final : public QWidget {
public:
    explicit WorkLogPage(QString root, QWidget* parent = nullptr) : QWidget(parent), root_(std::move(root)) {
        setObjectName(QStringLiteral("workLog"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(16, 14, 16, 12);
        v->setSpacing(10);
        auto* head = new QHBoxLayout;
        auto* title = new QLabel(QStringLiteral("Work log"), this);
        set_role(title, QStringLiteral("title"));
        head->addWidget(title);
        head->addStretch();
        find_ = new QLineEdit(this);
        find_->setObjectName(QStringLiteral("workFind"));
        find_->setPlaceholderText(QStringLiteral("Filter by name or change…"));
        find_->setClearButtonEnabled(true);
        find_->setMinimumWidth(260);
        head->addWidget(find_);
        auto* refresh = new QPushButton(QStringLiteral("Refresh"), this);
        head->addWidget(refresh);
        open_ = new QPushButton(QStringLiteral("Open file"), this);
        open_->setEnabled(false);
        head->addWidget(open_);
        v->addLayout(head);
        auto* note = new QLabel(QStringLiteral(
            "All the work in one place: the status page, research notes, runbooks, every run's results, the daily logs and "
            "findings, and every change made to ALTAIR. Pick one to read it here."), this);
        note->setWordWrap(true);
        set_role(note, QStringLiteral("note"));
        v->addWidget(note);

        auto* split = new QSplitter(Qt::Horizontal, this);
        tree_ = new QTreeWidget(split);
        tree_->setObjectName(QStringLiteral("workTree"));
        tree_->setHeaderLabels({QStringLiteral("Item"), QStringLiteral("Updated")});
        tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree_->setMinimumWidth(420);
        tree_->setTextElideMode(Qt::ElideMiddle);
        split->addWidget(tree_);
        viewer_ = new QStackedWidget(split);
        text_ = new QTextBrowser(viewer_);
        text_->setObjectName(QStringLiteral("workText"));
        text_->setOpenExternalLinks(true);
        viewer_->addWidget(text_);
        csv_model_ = new demo_detail::CsvModel(this);
        table_ = demo_detail::table_view(csv_model_, viewer_);
        table_->setObjectName(QStringLiteral("workTable"));
        viewer_->addWidget(table_);
        split->addWidget(viewer_);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 5);
        v->addWidget(split, 1);

        QObject::connect(refresh, &QPushButton::clicked, this, [this] { rescan(); });
        QObject::connect(find_, &QLineEdit::textChanged, this, [this] { apply_filter(); });
        QObject::connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* it) { show_item(it); });
        QObject::connect(open_, &QPushButton::clicked, this, [this] {
            if (!current_path_.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(current_path_));
        });
        text_->setHtml(QStringLiteral("<p>Pick something on the left.</p>"));
    }

    void rescan() {
        const QString keep = current_path_;
        tree_->clear();
        items_ = find_work(root_);
        QMap<QString, QTreeWidgetItem*> sections;
        const auto section = [&](const QString& name) {
            auto*& s = sections[name];
            if (s == nullptr) {
                s = new QTreeWidgetItem(tree_, {name});
                QFont f = s->font(0);
                f.setBold(true);
                s->setFont(0, f);
                s->setFlags(Qt::ItemIsEnabled);
                s->setExpanded(true);
            }
            return s;
        };
        // History first: it is the list of everything that was done.
        QString why;
        history_ = git_history(root_, &why);
        auto* changes = section(QStringLiteral("Changes to ALTAIR"));
        if (history_.isEmpty()) {
            auto* it = new QTreeWidgetItem(changes, {why.isEmpty() ? QStringLiteral("no history") : why});
            it->setData(0, Qt::UserRole, QString{});
        }
        for (const QString& line : history_) {
            const QStringList p = line.split(QLatin1Char('|'));
            if (p.size() < 4) continue;
            auto* it = new QTreeWidgetItem(changes, {p.mid(3).join(QLatin1Char('|')), p[1]});
            it->setData(0, Qt::UserRole, QStringLiteral("git:") + p[0]);
            it->setToolTip(0, QStringLiteral("%1 · %2 · %3").arg(p[0], p[2], p[1]));
        }
        changes->setExpanded(false);
        for (const auto& w : items_) {
            auto* it = new QTreeWidgetItem(section(w.section), {w.label, w.when.toString(QStringLiteral("dd MMM yyyy HH:mm"))});
            it->setData(0, Qt::UserRole, w.path);
            it->setToolTip(0, w.path);
            if (w.path == keep) tree_->setCurrentItem(it);
        }
        apply_filter();
        if (tree_->currentItem() == nullptr && !items_.empty()) {
            // Open on the status page when there is one.
            for (QTreeWidgetItemIterator i(tree_); *i; ++i)
                if ((*i)->data(0, Qt::UserRole).toString().endsWith(QLatin1String("STATUS.md"))) { tree_->setCurrentItem(*i); break; }
        }
    }

    [[nodiscard]] int item_count() const noexcept { return static_cast<int>(items_.size()); }
    [[nodiscard]] int change_count() const noexcept { return static_cast<int>(history_.size()); }
    [[nodiscard]] QString shown_text() const {
        return viewer_->currentWidget() == text_ ? text_->toPlainText() : QStringLiteral("table:%1").arg(csv_model_->rowCount());
    }
    /// Select the item whose path ends with `suffix`; false when none does.
    bool select(const QString& suffix) {
        for (QTreeWidgetItemIterator i(tree_); *i; ++i)
            if ((*i)->data(0, Qt::UserRole).toString().endsWith(suffix)) { tree_->setCurrentItem(*i); return true; }
        return false;
    }

protected:
    void showEvent(QShowEvent* e) override {
        QWidget::showEvent(e);
        if (!scanned_) {
            scanned_ = true;
            rescan();
        }
    }

private:
    void apply_filter() {
        const QString q = find_->text().trimmed();
        for (int s = 0; s < tree_->topLevelItemCount(); ++s) {
            QTreeWidgetItem* sec = tree_->topLevelItem(s);
            int shown = 0;
            for (int c = 0; c < sec->childCount(); ++c) {
                QTreeWidgetItem* it = sec->child(c);
                const bool hit = q.isEmpty() || it->text(0).contains(q, Qt::CaseInsensitive)
                                 || it->toolTip(0).contains(q, Qt::CaseInsensitive);
                it->setHidden(!hit);
                shown += hit ? 1 : 0;
            }
            sec->setText(0, sec->text(0).section(QStringLiteral("  ("), 0, 0) + QStringLiteral("  (%1)").arg(shown));
            if (!q.isEmpty()) sec->setExpanded(shown > 0);
        }
    }

    void show_item(QTreeWidgetItem* it) {
        const QString ref = it != nullptr ? it->data(0, Qt::UserRole).toString() : QString{};
        current_path_.clear();
        open_->setEnabled(false);
        if (ref.isEmpty()) return;
        if (ref.startsWith(QLatin1String("git:"))) {
            viewer_->setCurrentWidget(text_);
            const QString git = QStandardPaths::findExecutable(QStringLiteral("git"));
            QProcess p;
            p.start(git, {QStringLiteral("-C"), root_, QStringLiteral("show"), QStringLiteral("--stat"),
                          QStringLiteral("--format=%h  %ad  %an%n%n%B"), QStringLiteral("--date=format:%d %b %Y %H:%M"), ref.mid(4)});
            QString out;
            if (!git.isEmpty() && p.waitForFinished(8000)) out = QString::fromUtf8(p.readAllStandardOutput());
            else p.kill();
            text_->setPlainText(out.isEmpty() ? QStringLiteral("This change could not be read.") : out);
            return;
        }
        current_path_ = ref;
        open_->setEnabled(true);
        const QFileInfo f(ref);
        if (f.size() > 8 * 1024 * 1024) {
            viewer_->setCurrentWidget(text_);
            text_->setPlainText(QStringLiteral("%1 is %2 MB: too big to show here. Open file shows it in its own viewer.")
                                    .arg(f.fileName())
                                    .arg(static_cast<double>(f.size()) / (1024.0 * 1024.0), 0, 'f', 1));
            return;
        }
        const QString ext = f.suffix().toLower();
        if (ext == QLatin1String("csv")) {
            csv_model_->set(demo_detail::read_csv(ref));
            viewer_->setCurrentWidget(table_);
            return;
        }
        QFile file(ref);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            viewer_->setCurrentWidget(text_);
            text_->setPlainText(QStringLiteral("Could not read %1.").arg(ref));
            return;
        }
        const QString body = QString::fromUtf8(file.readAll());
        viewer_->setCurrentWidget(text_);
        if (ext == QLatin1String("md")) text_->setMarkdown(body);
        else text_->setPlainText(body);
    }

    QString root_;
    std::vector<WorkItem> items_;
    QStringList history_;
    QLineEdit* find_ = nullptr;
    QPushButton* open_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QStackedWidget* viewer_ = nullptr;
    QTextBrowser* text_ = nullptr;
    QTableView* table_ = nullptr;
    demo_detail::CsvModel* csv_model_ = nullptr;
    QString current_path_;
    bool scanned_ = false;
};

} // namespace altair::ui
