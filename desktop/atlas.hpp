// desktop/atlas.hpp -- the Model Atlas page.
//
// P36-01. Renders atlas_data.hpp as something you can search, filter and
// press. The data file explains WHAT is in the table; this file is only the
// window onto it, and holds no facts of its own.
//
// THREE DESIGN CHOICES, EACH FOR A REASON.
//
// A TREE, not a wall of text. The other analysis pages are reports because
// they are read once, top to bottom. This one is a reference: you arrive
// wanting one family, or one word, and everything else is noise. Families
// collapse; the search box narrows to what you typed.
//
// STATUS IS A COLUMN AND A COLOUR, so the shape of the project is visible
// before a single row is read. Green built, amber partial, grey absent -- and
// the absences deliberately stay in the list rather than being filtered out by
// default, because a catalogue that shows only what exists teaches you that
// everything exists.
//
// AND THE LAST COLUMN IS A BUTTON. A row that names a nav page can open it, so
// the way to learn what GARCH does is to press it and watch it answer on real
// NIFTY data. That is the whole reason the `page` field exists in the data:
// the alternative -- reading a definition -- is what Smit already had and said
// was not working.

#pragma once

#include "atlas_data.hpp"
#include "atlas_info.hpp"

#include <QCheckBox>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>

namespace altair::ui {

class AtlasPanel final : public QWidget {
public:
    /// `open_model` receives the stable Atlas identity. The page host owns
    /// navigation; this panel never routes by the legacy integer page field.
    explicit AtlasPanel(std::function<void(QString)> open_model,
                        QWidget* parent = nullptr)
        : QWidget(parent), open_model_(std::move(open_model)) {
        auto* v = new QVBoxLayout(this);

        std::size_t built = 0, partial = 0, absent = 0;
        for (const auto& r : kAtlasRows) {
            switch (r.status) {
            case AtlasStatus::Implemented: ++built; break;
            case AtlasStatus::Partial:     ++partial; break;
            case AtlasStatus::Absent:      ++absent; break;
            }
        }

        auto* head = new QLabel(
            QStringLiteral(
                "<b>MODEL ATLAS</b> — the ten families, and what this engine "
                "actually has.<br>"
                "<span style='color:#3FB950'>%1 built</span> · "
                "<span style='color:#B9770B'>%2 partial</span> · "
                "<span style='color:#7F8C8D'>%3 absent</span> "
                "&nbsp;of %4 entries. "
                "The absences are listed on purpose: a catalogue that shows "
                "only what exists teaches you that everything exists.<br>"
                "<b>BUILT does not mean trained or live-approved.</b> It means "
                "the numerical engine and its deterministic tests exist. See "
                "models/TRAINING_MATRIX.md for fit, data and GPU gates.<br>"
                "<b>PARTIAL is the status that misleads</b> — it means the "
                "primitive is here and the named model is not, and the row "
                "says which part is missing.")
                .arg(built).arg(partial).arg(absent).arg(kAtlasCount),
            this);
        head->setWordWrap(true);
        head->setStyleSheet(QStringLiteral("padding:6px;"));
        v->addWidget(head);

        auto* row = new QHBoxLayout;
        row->addWidget(new QLabel(QStringLiteral("Search"), this));
        find_ = new QLineEdit(this);
        find_->setPlaceholderText(
            QStringLiteral("model, family, file, or a word in the "
                           "description — try \"volatility\""));
        row->addWidget(find_, 1);
        hide_absent_ = new QCheckBox(QStringLiteral("Hide absent"), this);
        row->addWidget(hide_absent_);
        auto* expand = new QPushButton(QStringLiteral("Expand all"), this);
        auto* collapse = new QPushButton(QStringLiteral("Collapse"), this);
        row->addWidget(expand);
        row->addWidget(collapse);
        info_ = new QPushButton(QStringLiteral("\u24D8  About this model"), this);
        info_->setObjectName(QStringLiteral("atlasInfoButton"));
        info_->setToolTip(QStringLiteral("What the selected model is for, how it was trained, on what data, and what it does in the real market"));
        info_->setEnabled(false);
        row->addWidget(info_);
        v->addLayout(row);

        tree_ = new QTreeWidget(this);
        tree_->setColumnCount(4);
        tree_->setHeaderLabels({QStringLiteral("Model"),
                                QStringLiteral("Status"),
                                QStringLiteral("Where it lives"),
                                QStringLiteral("What it answers")});
        tree_->setAlternatingRowColors(true);
        tree_->setUniformRowHeights(false);
        tree_->setWordWrap(true);
        build_tree();
        v->addWidget(tree_, 1);

        note_ = new QLabel(
            QStringLiteral(
                "Double-click a row to open its stable model detail state. Every file path "
                "above is checked at configure time, so this table cannot "
                "quietly describe a tree that has moved on."),
            this);
        note_->setWordWrap(true);
        note_->setStyleSheet(QStringLiteral("color:#7F8C8D;padding:4px;"));
        v->addWidget(note_);

        connect(find_, &QLineEdit::textChanged, this,
                [this](const QString&) { apply_filter(); });
        connect(hide_absent_, &QCheckBox::toggled, this,
                [this](bool) { apply_filter(); });
        connect(expand, &QPushButton::clicked, tree_, &QTreeWidget::expandAll);
        connect(collapse, &QPushButton::clicked, tree_,
                &QTreeWidget::collapseAll);
        connect(tree_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
            info_->setEnabled(row_of(item) != nullptr);
        });
        connect(info_, &QPushButton::clicked, this, [this] {
            if (const AtlasRow* r = row_of(tree_->currentItem())) show_atlas_info(this, *r);
        });
        connect(tree_, &QTreeWidget::itemDoubleClicked, this,
                [this](QTreeWidgetItem* item, int) {
                    if (item == nullptr) { return; }
                    if (!item->data(0, Qt::UserRole).isValid()) { return; }
                    const QString id = item->data(0, Qt::UserRole + 2).toString();
                    if (!id.isEmpty() && open_model_) { open_model_(id); }
                });
    }

    /// Drive the filter without synthesising a keystroke.
    ///
    /// A test that typed into the box would be testing Qt's event loop; these
    /// set the same state the widgets set and re-run the same filter, so what
    /// is under test is the filtering rule and nothing else.
    void set_search(const QString& q) { find_->setText(q); }
    void set_hide_absent(bool on) { hide_absent_->setChecked(on); }

    /// Show a catalogue entry selected from navigation, including absent models.
    void focus_model(const QString& model, bool focus_search = false) {
        hide_absent_->setChecked(false);
        find_->setText(model);
        apply_filter();
        if (focus_search) { find_->setFocus(); }
    }

    /// For the test: how many rows are visible under the current filter.
    [[nodiscard]] int visible_rows() const {
        int n = 0;
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* fam = tree_->topLevelItem(i);
            for (int j = 0; j < fam->childCount(); ++j) {
                if (!fam->child(j)->isHidden()) { ++n; }
            }
        }
        return n;
    }

    [[nodiscard]] QPushButton* info_button() const noexcept { return info_; }
    [[nodiscard]] QTreeWidget* tree() const noexcept { return tree_; }

private:
    [[nodiscard]] static const AtlasRow* row_of(const QTreeWidgetItem* item) {
        if (item == nullptr || !item->data(0, Qt::UserRole + 1).isValid()) return nullptr;
        const qulonglong id = item->data(0, Qt::UserRole + 1).toULongLong();
        for (const auto& r : kAtlasRows) if (atlas_model_id(r) == id) return &r;
        return nullptr;
    }
    void build_tree() {
        QTreeWidgetItem* fam = nullptr;
        QString current;
        for (const auto& r : kAtlasRows) {
            const QString family = QString::fromUtf8(r.family);
            if (family != current) {
                current = family;
                fam = new QTreeWidgetItem(tree_);
                fam->setText(0, family);
                fam->setFirstColumnSpanned(true);
                QFont f = fam->font(0);
                f.setBold(true);
                fam->setFont(0, f);
                fam->setExpanded(true);
            }
            auto* it = new QTreeWidgetItem(fam);
            it->setText(0, QString::fromUtf8(r.model));
            it->setText(1, QString::fromUtf8(atlas_status_text(r.status)));
            it->setText(2, QString::fromUtf8(r.file));
            it->setText(3, QString::fromUtf8(r.what));
            it->setData(0, Qt::UserRole, r.page);
            it->setData(0, Qt::UserRole + 1,
                        QVariant::fromValue<qulonglong>(atlas_model_id(r)));
            it->setData(0, Qt::UserRole + 2,
                        QStringLiteral("atlas.%1").arg(
                            QString::number(static_cast<qulonglong>(atlas_model_id(r)), 16)));

            switch (r.status) {
            case AtlasStatus::Implemented:
                it->setForeground(1, QColor(QStringLiteral("#3FB950")));
                break;
            case AtlasStatus::Partial:
                it->setForeground(1, QColor(QStringLiteral("#B9770B")));
                break;
            case AtlasStatus::Absent:
                it->setForeground(1, QColor(QStringLiteral("#7F8C8D")));
                it->setForeground(0, QColor(QStringLiteral("#7F8C8D")));
                break;
            }
            it->setToolTip(0, QStringLiteral(
                "Model ID: atlas.%1\nDouble-click to open this exact model detail state.")
                .arg(QString::number(
                    static_cast<qulonglong>(atlas_model_id(r)), 16)));
        }
        tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        tree_->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    }

    void apply_filter() {
        const QString q = find_->text().trimmed();
        const bool hide = hide_absent_->isChecked();
        for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
            QTreeWidgetItem* fam = tree_->topLevelItem(i);
            int shown = 0;
            for (int j = 0; j < fam->childCount(); ++j) {
                QTreeWidgetItem* it = fam->child(j);
                // Matched across EVERY column plus the family name, so
                // "volatility" finds GARCH by its description and India VIX by
                // its family without the reader knowing either name first.
                // That is the point of a search box on a reference.
                bool match = q.isEmpty();
                if (!match) {
                    match = fam->text(0).contains(q, Qt::CaseInsensitive);
                    for (int c = 0; c < 4 && !match; ++c) {
                        match = it->text(c).contains(q, Qt::CaseInsensitive);
                    }
                }
                if (match && hide
                    && it->text(1)
                           == QLatin1String(
                               atlas_status_text(AtlasStatus::Absent))) {
                    match = false;
                }
                it->setHidden(!match);
                if (match) { ++shown; }
            }
            fam->setHidden(shown == 0);
            if (shown > 0 && !q.isEmpty()) { fam->setExpanded(true); }
        }
    }

    std::function<void(QString)> open_model_;
    QPushButton* info_ = nullptr;
    QLineEdit* find_ = nullptr;
    QCheckBox* hide_absent_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QLabel* note_ = nullptr;
};

}  // namespace altair::ui
