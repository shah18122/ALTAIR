// desktop/account_widgets.hpp -- the presentation pieces the account page
// needed and did not have.
//
// P32-02. Smit asked for the Kite Account page to stop being five panes of
// monospace text and become buttons and cards.
//
// WHY A MONOSPACE DUMP WAS THE WRONG SHAPE HERE SPECIFICALLY.
//
// The quant pages ARE text and should stay text: they are arguments, read once
// end to end, and the reasoning between the numbers is the deliverable. An
// account page is the opposite. It is scanned, repeatedly, for one number at a
// time -- what can I deploy, am I flat, did that order fill -- and a reader
// doing that has to parse a column of aligned digits to find the one row that
// matters. Form should carry state when a page is scanned rather than read.
//
// SO: the numbers that get looked at become cards with their own colour, and
// the lists become tables that can be sorted and selected. Nothing about the
// DATA changes, and every rule the text version kept, it still keeps -- age
// before value, absent distinguished from zero, and no button that mutates.
//
// COLOUR IS SEMANTIC AND IS NOT THE ACCENT.
//
// Green and red here mean profit and loss, and nothing else uses them. A card
// tinted because it is "important" would make the two indistinguishable at a
// glance, which is the only speed at which this page is read.

#pragma once

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QString>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace altair::ui {

/// One number, its label, and an optional footnote.
///
/// `tone`: 0 neutral, +1 good, -1 bad, 2 "this is the headline one".
class StatCard final : public QFrame {
public:
    StatCard(const QString& label, const QString& value, int tone = 0,
             const QString& foot = QString(), QWidget* parent = nullptr)
        : QFrame(parent) {
        setFrameShape(QFrame::NoFrame);
        QString bg = QStringLiteral("#161C22");
        QString fg = QStringLiteral("#E6EDF3");
        QString edge = QStringLiteral("#2C3E50");
        if (tone > 0 && tone != 2) {
            fg = QStringLiteral("#3FB950");
            edge = QStringLiteral("#1E6B3A");
        } else if (tone < 0) {
            fg = QStringLiteral("#F85149");
            edge = QStringLiteral("#8C2F2F");
        } else if (tone == 2) {
            bg = QStringLiteral("#1B2733");
            edge = QStringLiteral("#3D6E9E");
        }
        setStyleSheet(QStringLiteral(
            "QFrame{background:%1;border:1px solid %2;border-radius:4px;}")
                          .arg(bg, edge));

        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 10, 12, 10);
        v->setSpacing(2);

        auto* l = new QLabel(label.toUpper(), this);
        l->setStyleSheet(QStringLiteral(
            "color:#7F8C8D;font-size:10px;letter-spacing:1px;border:none;"));
        v->addWidget(l);

        auto* n = new QLabel(value, this);
        // Tabular figures, so a column of cards does not jitter as values
        // change width. Without it the rupee amounts dance on every refresh.
        n->setStyleSheet(QStringLiteral(
            "color:%1;font-size:%2px;font-weight:600;border:none;"
            "font-family:'Segoe UI',sans-serif;"
            "font-variant-numeric:tabular-nums;")
                             .arg(fg).arg(tone == 2 ? 22 : 17));
        n->setTextInteractionFlags(Qt::TextSelectableByMouse);
        v->addWidget(n);

        if (!foot.isEmpty()) {
            auto* f = new QLabel(foot, this);
            f->setWordWrap(true);
            f->setStyleSheet(QStringLiteral(
                "color:#6E7B8B;font-size:10px;border:none;"));
            v->addWidget(f);
        }
    }
};

/// A grid of cards that reflows, so the page does not need a fixed column
/// count decided here for a window whose width nobody knows.
class CardGrid final : public QWidget {
public:
    explicit CardGrid(int columns = 4, QWidget* parent = nullptr)
        : QWidget(parent), columns_(columns) {
        grid_ = new QGridLayout(this);
        grid_->setContentsMargins(0, 0, 0, 0);
        grid_->setSpacing(8);
    }

    void add(StatCard* c) {
        grid_->addWidget(c, next_ / columns_, next_ % columns_);
        ++next_;
    }

    /// Span the full row -- for the headline card.
    void add_wide(StatCard* c) {
        if (next_ % columns_ != 0) { next_ += columns_ - (next_ % columns_); }
        grid_->addWidget(c, next_ / columns_, 0, 1, columns_);
        next_ += columns_;
    }

    void clear() {
        while (QLayoutItem* it = grid_->takeAt(0)) {
            if (QWidget* w = it->widget()) { w->deleteLater(); }
            delete it;
        }
        next_ = 0;
    }

private:
    QGridLayout* grid_ = nullptr;
    int columns_ = 4;
    int next_ = 0;
};

/// A dark, read-only table with the header styled to match the rest.
[[nodiscard]] inline QTableWidget* account_table(const QStringList& headers,
                                                 QWidget* parent = nullptr) {
    auto* t = new QTableWidget(0, headers.size(), parent);
    t->setHorizontalHeaderLabels(headers);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setAlternatingRowColors(true);
    t->verticalHeader()->setVisible(false);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setSortingEnabled(true);
    t->setStyleSheet(QStringLiteral(
        "QTableWidget{background:#11171C;alternate-background-color:#141B21;"
        "color:#D6DBDF;gridline-color:#232C35;border:none;"
        "font-family:'Segoe UI',sans-serif;font-size:12px;}"
        "QHeaderView::section{background:#1B2733;color:#9FB3C8;"
        "padding:6px;border:none;border-right:1px solid #232C35;"
        "font-size:11px;letter-spacing:1px;}"
        "QTableWidget::item{padding:4px 6px;}"
        "QTableWidget::item:selected{background:#2C3E50;color:#FFFFFF;}"));
    return t;
}

/// Put a cell in, right-aligned when it is a number, tinted when it is signed.
///
/// `signed_tint` is opt-in per cell rather than inferred from the text: a
/// quantity of -50 is a short position and not a loss, and colouring it red
/// would say something false about a perfectly ordinary book.
inline void account_cell(QTableWidget* t, int row, int col,
                         const QString& text, bool numeric = false,
                         double signed_tint = 0.0, bool tint = false) {
    auto* it = new QTableWidgetItem(text);
    if (numeric) {
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (tint) {
        it->setForeground(signed_tint > 0.0   ? QColor(0x3F, 0xB9, 0x50)
                          : signed_tint < 0.0 ? QColor(0xF8, 0x51, 0x49)
                                              : QColor(0xD6, 0xDB, 0xDF));
    }
    t->setItem(row, col, it);
}

/// A row of buttons styled as actions rather than as form controls.
[[nodiscard]] inline QString action_button_css(bool primary) {
    return primary
        ? QStringLiteral(
              "QPushButton{background:#1F6FEB;color:#FFFFFF;border:none;"
              "padding:8px 16px;border-radius:4px;font-weight:600;}"
              "QPushButton:hover{background:#388BFD;}"
              "QPushButton:disabled{background:#30363D;color:#6E7B8B;}")
        : QStringLiteral(
              "QPushButton{background:#21262D;color:#C9D1D9;"
              "border:1px solid #30363D;padding:8px 16px;border-radius:4px;}"
              "QPushButton:hover{background:#30363D;}"
              "QPushButton:disabled{color:#6E7B8B;}");
}

} // namespace altair::ui
