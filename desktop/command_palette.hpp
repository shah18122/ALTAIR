// desktop/command_palette.hpp -- Ctrl+K: type where you want to go.
//
// Forty-odd pages, ninety Atlas models, the Terminal's views and the window's
// own actions, behind one box. Type a few letters of any of them; the best
// matches come first (letters in order, word starts and runs scoring higher);
// Enter goes there, Esc closes. The entries are given by the window, so the
// palette knows nothing about what they do.
//
// Nothing that trades is an entry. Halting stays on F11+F12 and the status
// bar's switch, which ask first.
#pragma once

#include "theme.hpp"

#include <QDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <vector>

namespace altair::ui {

struct PaletteEntry {
    QString title;   ///< what is matched and shown
    QString kind;    ///< "Page", "Model", "View", "Action", "Layout"
    QString hint;    ///< a second, faint line (where it lives, its shortcut)
    std::function<void()> run;
};

/// How well `query` matches `text`: the letters of the query must appear in
/// order (case-insensitive); a match at a word start or right after the last
/// one scores more, and a shorter text breaks ties. -1 when it does not match.
[[nodiscard]] inline int palette_score(const QString& query, const QString& text) {
    const QString q = query.simplified().toCaseFolded(), t = text.toCaseFolded();
    if (q.isEmpty()) return 0;
    int score = 0, last = -2, at = 0;
    for (const QChar c : q) {
        if (c == QLatin1Char(' ')) continue;
        const int i = static_cast<int>(t.indexOf(c, at));
        if (i < 0) return -1;
        const bool word_start = i == 0 || !t[i - 1].isLetterOrNumber();
        score += 1 + (word_start ? 8 : 0) + (i == last + 1 ? 5 : 0);
        last = i;
        at = i + 1;
    }
    if (t.startsWith(q)) score += 20;
    else if (t.contains(q)) score += 10;
    return score * 100 - static_cast<int>(t.size());
}

/// The entries that match `query`, best first; all of them, in order, when it is empty.
[[nodiscard]] inline std::vector<int> palette_rank(const std::vector<PaletteEntry>& entries, const QString& query) {
    std::vector<std::pair<int, int>> scored;
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        const auto& e = entries[static_cast<std::size_t>(i)];
        const int s = std::max(palette_score(query, e.title), palette_score(query, e.kind + QLatin1Char(' ') + e.title) - 50);
        if (s >= 0) scored.push_back({s, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<int> out;
    out.reserve(scored.size());
    for (const auto& [s, i] : scored) out.push_back(i);
    return out;
}

class CommandPalette final : public QDialog {
public:
    explicit CommandPalette(QWidget* parent = nullptr) : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint) {
        setObjectName(QStringLiteral("commandPalette"));
        setStyleSheet(QStringLiteral("#commandPalette{background:%1;border:1px solid %2;border-radius:10px;}"
                                     "#commandPalette QLineEdit{font-size:15px;padding:8px 10px;}"
                                     "#commandPalette QListWidget{border:none;background:transparent;}"
                                     "#commandPalette QListWidget::item{padding:5px 8px;border-radius:6px;}"
                                     "#commandPalette QListWidget::item:selected{background:%3;}")
                          .arg(QLatin1String(theme_token::kSurface), QLatin1String(theme_token::kBorderHi),
                               QLatin1String(theme_token::kSelection)));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(10, 10, 10, 10);
        v->setSpacing(6);
        edit_ = new QLineEdit(this);
        edit_->setObjectName(QStringLiteral("paletteQuery"));
        edit_->setPlaceholderText(QStringLiteral("Go to a page, model, view or action…"));
        v->addWidget(edit_);
        list_ = new QListWidget(this);
        list_->setObjectName(QStringLiteral("paletteList"));
        list_->setUniformItemSizes(true);
        v->addWidget(list_, 1);
        auto* foot = new QLabel(QStringLiteral("↑↓ choose · Enter go · Esc close"), this);
        set_role(foot, QStringLiteral("faint"));
        v->addWidget(foot);
        resize(620, 440);
        edit_->installEventFilter(this);
        QObject::connect(edit_, &QLineEdit::textChanged, this, [this] { refill(); });
        QObject::connect(list_, &QListWidget::itemActivated, this, [this] { run_current(); });
    }

    void set_entries(std::vector<PaletteEntry> entries) {
        entries_ = std::move(entries);
        refill();
    }

    /// Open centred near the top of `over`, empty and focused.
    void open_over(QWidget* over) {
        edit_->clear();
        refill();
        if (over != nullptr) {
            const QRect r = over->geometry();
            move(r.x() + (r.width() - width()) / 2, r.y() + std::max(40, r.height() / 8));
        }
        show();
        raise();
        activateWindow();
        edit_->setFocus();
    }

    /// For tests: type, and read what would run.
    void type(const QString& q) { edit_->setText(q); }
    [[nodiscard]] QString first_title() const {
        return shown_.empty() ? QString{} : entries_[static_cast<std::size_t>(shown_.front())].title;
    }
    [[nodiscard]] int shown_count() const noexcept { return static_cast<int>(shown_.size()); }
    bool run_first() {
        if (shown_.empty()) return false;
        list_->setCurrentRow(0);
        run_current();
        return true;
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == edit_ && e->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(e)->key();
            if (key == Qt::Key_Down || key == Qt::Key_Up) {
                const int n = list_->count();
                if (n > 0) list_->setCurrentRow(std::clamp(list_->currentRow() + (key == Qt::Key_Down ? 1 : -1), 0, n - 1));
                return true;
            }
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                run_current();
                return true;
            }
            if (key == Qt::Key_Escape) {
                hide();
                return true;
            }
        }
        return QDialog::eventFilter(o, e);
    }

private:
    void refill() {
        shown_ = palette_rank(entries_, edit_->text());
        if (shown_.size() > 60) shown_.resize(60);
        list_->clear();
        for (const int i : shown_) {
            const auto& e = entries_[static_cast<std::size_t>(i)];
            auto* it = new QListWidgetItem(QStringLiteral("%1    —  %2%3").arg(e.title, e.kind,
                                               e.hint.isEmpty() ? QString{} : QStringLiteral(" · ") + e.hint), list_);
            it->setToolTip(e.hint);
        }
        if (list_->count() > 0) list_->setCurrentRow(0);
    }

    void run_current() {
        const int row = list_->currentRow();
        if (row < 0 || row >= static_cast<int>(shown_.size())) return;
        const auto run = entries_[static_cast<std::size_t>(shown_[static_cast<std::size_t>(row)])].run;
        hide();
        if (run) run();
    }

    QLineEdit* edit_ = nullptr;
    QListWidget* list_ = nullptr;
    std::vector<PaletteEntry> entries_;
    std::vector<int> shown_;
};

} // namespace altair::ui
