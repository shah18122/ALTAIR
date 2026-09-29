// desktop/panels.hpp -- the pages behind the nav.
//
// P11Q-05.
//
// EVERY PANEL HERE RENDERS A FACT OR SAYS IT CANNOT.
//
// These replace the "named blocker" placeholders. The temptation at this point
// is to fill them: a sparkline on the model page, a progress bar on the
// training row, a plausible number in the aggregator box. Every one of those
// would be indistinguishable from a real one, and this is the window somebody
// reads a position off.
//
// So each panel draws its table from the catalogues in `model_status.hpp` and
// `feed_status.hpp`, which carry state as an enum whose ordinal zero is the
// pessimistic answer. A row nobody has updated reads "never trained" or "not
// built", never "ready".
//
// THE DATAFLOW DIAGRAM DRAWS THE WALL.
//
// An architecture picture that shows the whole path with arrows implies the
// whole path runs. This one colours each stage by its real state and draws a
// hard stop where a tick can no longer get through, so the picture and the
// table cannot disagree -- they are the same vector.

#pragma once

#include "auth.hpp"
#include "feed_status.hpp"
#include "format.hpp"
#include "data/fits.hpp"
#include "data/master_lookup.hpp"
#include "model_job.hpp"
#include "model_status.hpp"
#include "atlas_data.hpp"
#include "navigation_registry.hpp"
#include "watchlist.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QFile>
#include <QHash>
#include <QJsonParseError>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <QComboBox>
#include <QDateTime>
#include <QTimeZone>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QApplication>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <stdexcept>
#include <utility>

namespace altair::ui {

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

class LoginDialog final : public QDialog {
    Q_OBJECT

public:
    LoginDialog(const UserStore& users, QWidget* parent = nullptr)
        : QDialog(parent), users_(users) {
        setWindowTitle(QStringLiteral("Altair — sign in"));
        setModal(true);
        setMinimumWidth(360);

        auto* v = new QVBoxLayout(this);

        // THE MARK, AND THEN THE NAME. Aquila, the eagle Altair sits in --
        // the same SVG the window and the taskbar use, so the sign-in screen
        // and the running application are recognisably one program.
        //
        // Rendered from the SVG rather than a bitmap: it is asked for at
        // 72 px here and 16 px in the taskbar, and a PNG picked for one of
        // those is soft at the other.
        auto* mark = new QLabel(this);
        // The mark came up EMPTY the first time, and the reason was not
        // here: desktop/assets/altair.qrc was in the target's source list and
        // AUTORCC was never on, so no resource existed to find. Both QIcon
        // and QPixmap answer a missing resource with a null object and no
        // diagnostic, which is why this looked like a layout bug for as long
        // as it did. See qt_add_resources in desktop/CMakeLists.txt.
        //
        // QPixmap rather than QIcon::pixmap now that the resource is real:
        // this is one fixed size on one surface, so the icon engine's
        // state-and-mode machinery buys nothing.
        QPixmap logo(QStringLiteral(":/altair_eagle.svg"));
        if (!logo.isNull()) {
            mark->setPixmap(logo.scaled(72, 72, Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation));
        } else {
            // VISIBLE, not silent. A missing mark is cosmetic; a label that
            // collapses to nothing looks like a layout that was never
            // written, and that is what sent me looking in the wrong place.
            mark->setText(QStringLiteral("[mark unavailable]"));
            mark->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        }
        mark->setAlignment(Qt::AlignCenter);
        v->addWidget(mark);

        auto* title = new QLabel(QStringLiteral("<h2>Altair</h2>"), this);
        title->setAlignment(Qt::AlignCenter);
        v->addWidget(title);

        // P34-02. THE DEVELOPMENT-BUILD BANNER IS GONE, AT SMIT'S REQUEST,
        // AND WHAT IT PROTECTED IS NOT.
        //
        // It said the default accounts are compiled in. That was true and it
        // was worth saying while this was the only thing standing between a
        // stranger and the window -- but it said it on every sign-in for
        // months to the one person who already knew, which is how a warning
        // stops being read.
        //
        // The property itself is unchanged and is still enforced where it
        // matters rather than by a label: `prod` does not define
        // ALTAIR_DEV_CREDENTIALS, so a shipped binary has NO accounts
        // provisioned and the message below says so and lets nobody in.
        // desktop/CMakeLists.txt still prints "DEV CREDENTIALS COMPILED IN"
        // at configure time, where the person choosing the preset sees it.

        if (users_.empty()) {
            auto* none = new QLabel(
                QStringLiteral(
                    "No accounts are provisioned in this build. There is no "
                    "default account to fall back to."),
                this);
            none->setWordWrap(true);
            none->setStyleSheet(QStringLiteral("color:#F85149;"));
            v->addWidget(none);
        }

        auto* form = new QFormLayout;
        user_ = new QLineEdit(this);
        pass_ = new QLineEdit(this);
        pass_->setEchoMode(QLineEdit::Password);
        form->addRow(QStringLiteral("User"), user_);
        form->addRow(QStringLiteral("Password"), pass_);
        v->addLayout(form);

        error_ = new QLabel(this);
        error_->setStyleSheet(QStringLiteral("color:#F85149;"));
        error_->setVisible(false);
        v->addWidget(error_);

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Sign in"));
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this,
                &LoginDialog::attempt);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(pass_, &QLineEdit::returnPressed, this, &LoginDialog::attempt);
    }

    [[nodiscard]] Role role() const noexcept { return role_; }
    [[nodiscard]] QString user() const { return user_->text(); }

private Q_SLOTS:
    void attempt() {
        const Role r = users_.verify(user_->text(), pass_->text());
        if (r == Role::None) {
            // ONE message for both "no such user" and "wrong password", so
            // the screen cannot be used to enumerate which accounts exist.
            error_->setText(QStringLiteral("Sign-in failed."));
            error_->setVisible(true);
            pass_->clear();
            return;
        }
        role_ = r;
        accept();
    }

private:
    const UserStore& users_;
    QLineEdit* user_ = nullptr;
    QLineEdit* pass_ = nullptr;
    QLabel* error_ = nullptr;
    Role role_ = Role::None;
};

// ---------------------------------------------------------------------------
// A small helper for the read-only fact tables below
// ---------------------------------------------------------------------------

[[nodiscard]] inline QTableWidget* fact_table(const QStringList& headers,
                                              QWidget* parent) {
    auto* t = new QTableWidget(0, headers.size(), parent);
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setAlternatingRowColors(true);
    t->horizontalHeader()->setStretchLastSection(true);
    return t;
}

inline void put(QTableWidget* t, int row, int col, const QString& text,
                const QColor& fg = QColor()) {
    auto* item = new QTableWidgetItem(text);
    if (fg.isValid()) {
        item->setForeground(fg);
    }
    t->setItem(row, col, item);
}

// ---------------------------------------------------------------------------
// Broker wiring
// ---------------------------------------------------------------------------

class WiringPanel final : public QWidget {
    Q_OBJECT

public:
    explicit WiringPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);

        auto* head = new QLabel(
            QStringLiteral("<h3>Broker wiring — FYERS primary / Kite secondary</h3>"),
            this);
        v->addWidget(head);

        // THE PROSE HAS TO TRACK THE TABLE, OR IT IS THE STALE HALF.
        //
        // This paragraph said "it has no transport and no session" while the
        // rows beneath it — which now detect both — said "built". The rows
        // were fixed and the summary above them was not, which is the worst
        // arrangement of the two: a reader takes the sentence and skims the
        // table. So the changing clause is built from the same detected state
        // the rows use, and only the parts that cannot change are literal.
        auto* note = new QLabel(
            QStringLiteral(
                "FYERS is the configured primary route and Kite is the explicit "
                "secondary. FYERS has a paper-safe auth vocabulary, but its live "
                "transport is not yet wired into this desktop. Kite has a "
                "parser, decoder, order <i>translation</i>, HTTPS transport and %1. "
                "<b>XTS is withdrawn</b>; it is not a fallback venue. <b>No broker "
                "can carry an order from this read-only desktop today</b>: "
                "<code>oms/kite_adapter.hpp</code> builds the POST body and "
                "nothing in <code>oms/</code> sends it, which is P4-05's "
                "design rather than an omission.")
                .arg(session_present()
                         ? QStringLiteral("a session on disk")
                         : QStringLiteral("no session")),
            this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(note);

        auto* t = fact_table({QStringLiteral("Broker"), QStringLiteral("Stage"),
                              QStringLiteral("State"), QStringLiteral("Detail")},
                             this);
        const auto rows = wiring();
        t->setRowCount(static_cast<int>(rows.size()));
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const WiringRow& r = rows[static_cast<std::size_t>(i)];
            put(t, i, 0, r.broker);
            put(t, i, 1, r.stage);
            put(t, i, 2, wiring_label(r.state), wiring_colour(r.state));
            put(t, i, 3, r.detail);
        }
        t->resizeColumnsToContents();
        v->addWidget(t, 1);

        auto* verdict = new QLabel(this);
        verdict->setWordWrap(true);
        const bool fyers = can_trade(QStringLiteral("FYERS"));
        const bool kite = can_trade(QStringLiteral("Kite"));
        verdict->setText(
            QStringLiteral("<b>Broker priority — FYERS: PRIMARY (%1) · Kite: SECONDARY (%2)</b>"
                           "  <span style='color:#7F8C8D'>(XTS withdrawn; read-only UI)</span>")
                .arg(fyers ? QStringLiteral("ready") : QStringLiteral("not ready"),
                     kite ? QStringLiteral("ready") : QStringLiteral("not ready")));
        verdict->setStyleSheet(
            QStringLiteral("color:%1;padding:6px;")
                .arg(fyers && kite ? QStringLiteral("#3FB950")
                                   : QStringLiteral("#B9770B")));
        v->addWidget(verdict);
    }
};

/// A dedicated page makes the configured primary broker discoverable without
/// forcing an operator to infer it from the wiring table or a status pill.
class FyersPanel final : public QWidget {
public:
    explicit FyersPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->addWidget(new QLabel(QStringLiteral("<h2>FYERS — Primary broker</h2>"), this));

        const FyersState state = probe_fyers();
        auto* status = new QLabel(
            QStringLiteral("<b>%1</b><br>%2")
                .arg(fyers_label(state.link), state.detail), this);
        status->setWordWrap(true);
        status->setStyleSheet(QStringLiteral("padding:8px;color:%1;")
                                   .arg(fyers_colour(state.link).name()));
        v->addWidget(status);

        auto* table = fact_table({QStringLiteral("Property"), QStringLiteral("Value")}, this);
        const QList<QPair<QString, QString>> rows{
            {QStringLiteral("Configured role"), QStringLiteral("PRIMARY")},
            {QStringLiteral("Secondary broker"), QStringLiteral("Zerodha Kite")},
            {QStringLiteral("1-minute history"), QStringLiteral("FYERS resolution 1; authenticated export required")},
            {QStringLiteral("Live transport"), QStringLiteral("Not enabled in this desktop build")},
            {QStringLiteral("Order submission"), QStringLiteral("Read-only / paper-safe; no order is sent")},
        };
        table->setRowCount(rows.size());
        for (int i = 0; i < rows.size(); ++i) {
            put(table, i, 0, rows[i].first);
            put(table, i, 1, rows[i].second);
        }
        table->resizeColumnsToContents();
        v->addWidget(table, 1);

        auto* note = new QLabel(
            QStringLiteral("Use this page to confirm broker priority. Credentials are read from the local environment only; token values are never rendered here. The Kite account remains available under Accounts as the secondary route."), this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(note);
    }
};

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

class ModelPanel final : public QWidget {
    Q_OBJECT

public:
    /// Set by the main window: open a navigation page by its stable id.
    std::function<void(const QString&)> on_open_page;

    explicit ModelPanel(Role role, QWidget* parent = nullptr)
        : QWidget(parent), jobs_(this), can_train_(may(role, Capability::TrainModel)) {
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(16, 12, 16, 12);
        v->setSpacing(10);
        auto* title = new QLabel(
            QStringLiteral("<span style='font-size:18px;font-weight:700'>Models</span>"
                           "<br><span style='color:#8B949E'>What each model has, what "
                           "is missing, and what it says when fitted on the data "
                           "that is here.</span>"),
            this);
        title->setWordWrap(true);
        v->addWidget(title);

        const auto rows = model_catalogue();
        int fitted = 0, no_edge = 0, synthetic = 0, blocked = 0;
        for (const ModelRow& r : rows) {
            if (r.state == ModelState::TrainedOnRealData) ++fitted;
            else if (r.state == ModelState::TrainedNoEdge) ++no_edge;
            else if (r.state == ModelState::ValidatedOnSyntheticOnly) ++synthetic;
            else if (r.state == ModelState::BlockedOnData) ++blocked;
        }
        const int never = static_cast<int>(rows.size()) - fitted - no_edge
                          - synthetic - blocked;

        // One card per readiness state, in the state's own table colour, so
        // the counts and the rows below read as the same thing.
        auto* kpis = new QHBoxLayout;
        kpis->setSpacing(10);
        const auto kpi = [this, kpis](int n, const QString& label, ModelState s) {
            auto* card = new QLabel(
                QStringLiteral("<span style='font-size:20px;font-weight:700;"
                               "color:%1'>%2</span><br>"
                               "<span style='color:#8B949E'>%3</span>")
                    .arg(model_state_colour(s).name()).arg(n).arg(label),
                this);
            card->setStyleSheet(QStringLiteral(
                "background:#161B22;border:1px solid #30363D;"
                "border-radius:10px;padding:10px 14px;"));
            kpis->addWidget(card, 1);
        };
        kpi(fitted, QStringLiteral("real-data fit"), ModelState::TrainedOnRealData);
        kpi(no_edge, QStringLiteral("fitted, no edge"), ModelState::TrainedNoEdge);
        kpi(synthetic, QStringLiteral("fixture-only"), ModelState::ValidatedOnSyntheticOnly);
        kpi(blocked, QStringLiteral("blocked on data"), ModelState::BlockedOnData);
        kpi(never, QStringLiteral("never trained"), ModelState::NeverTrained);
        v->addLayout(kpis);

        auto* summary = new QLabel(
            QStringLiteral(
                "<b>MODEL READINESS — %1 catalogue entries.</b> "
                "%2 have a retained real-data fit; %3 were evaluated on real "
                "data and found no edge; %4 are fixture-only; %5 are blocked "
                "on missing inputs. This page reports fit readiness. Model Atlas "
                "reports whether the numerical engine is built; neither label "
                "by itself means live-approved.")
                .arg(rows.size()).arg(fitted).arg(no_edge).arg(synthetic).arg(blocked),
            this);
        summary->setWordWrap(true);
        summary->setStyleSheet(QStringLiteral("color:#8B949E;"));
        v->addWidget(summary);

        filter_ = new QLineEdit(this);
        filter_->setPlaceholderText(
            QStringLiteral("Filter by model, card, instrument, state or data\u2026"));
        filter_->setClearButtonEnabled(true);
        v->addWidget(filter_);

        auto* t = fact_table({QStringLiteral("Model"), QStringLiteral("Card"),
                              QStringLiteral("Instrument"),
                              QStringLiteral("State"), QStringLiteral("Needs"),
                              QStringLiteral("Has")},
                             this);
        t->setRowCount(static_cast<int>(rows.size()));
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const ModelRow& r = rows[static_cast<std::size_t>(i)];
            put(t, i, 0, r.name);
            put(t, i, 1, r.card);
            put(t, i, 2, r.instrument);
            put(t, i, 3, model_state_label(r.state),
                model_state_colour(r.state));
            put(t, i, 4, r.needs);
            put(t, i, 5, r.has);
        }
        t->resizeColumnsToContents();
        v->addWidget(t, 1);
        connect(filter_, &QLineEdit::textChanged, this,
                [this](const QString& text) { apply_filter(text); });

        // THE FIT PANE. Selecting a row runs the model, if it has data.
        //
        // The alternative -- a static string per row -- is what the catalogue
        // already is, and a dashboard that only ever shows strings somebody
        // typed cannot tell you when a fit stops working. These numbers come
        // out of the model on the click.
        connect(t, &QTableWidget::currentCellChanged, this,
                [this](int row, int, int, int) {
                    // A manual pick replaces any Atlas card from a deep link.
                    if (!focusing_ && atlas_box_ != nullptr) atlas_box_->hide();
                    show_fit(row);
                });
        table_ = t;

        // THE ATLAS CARD. Model Atlas routes a model with no workspace of its
        // own here; the card states its four gates separately so "the engine
        // is built" is never read as "trained" or "approved".
        atlas_box_ = new QWidget(this);
        auto* atlas_row = new QHBoxLayout(atlas_box_);
        atlas_row->setContentsMargins(0, 0, 0, 0);
        atlas_card_ = new QLabel(atlas_box_);
        atlas_card_->setWordWrap(true);
        atlas_card_->setTextFormat(Qt::RichText);
        atlas_card_->setStyleSheet(QStringLiteral(
            "background:#161B22;border:1px solid #30363D;"
            "border-radius:10px;padding:10px 14px;"));
        atlas_row->addWidget(atlas_card_, 1);
        auto* open_atlas = new QPushButton(QStringLiteral("Open in Model Atlas"), atlas_box_);
        connect(open_atlas, &QPushButton::clicked, this, [this] {
            if (on_open_page) on_open_page(QStringLiteral("models.atlas"));
        });
        atlas_row->addWidget(open_atlas, 0, Qt::AlignTop);
        atlas_box_->hide();
        v->addWidget(atlas_box_);

        detail_ = new QPlainTextEdit(this);
        detail_->setReadOnly(true);
        detail_->setMinimumHeight(190);
        detail_->setStyleSheet(QStringLiteral(
            "background:#12161A;color:#D6DBDF;font-family:Consolas,monospace;"));
        v->addWidget(detail_);

        auto* controls = new QHBoxLayout;
        train_ = new QPushButton(QStringLiteral("Train selected model"), this);
        // A capability, not a role check written out at the call site.
        train_->setEnabled(can_train_);
        train_->setToolTip(
            may(role, Capability::TrainModel)
                ? QStringLiteral(
                      "Runs the selected model's available evaluation harness. "
                      "Rows without a wired evaluator are refused explicitly.")
                : QStringLiteral("Requires the admin role."));
        controls->addWidget(train_);

        status_ = new QLabel(this);
        status_->setWordWrap(true);
        controls->addWidget(status_, 1);
        v->addLayout(controls);

        // Select the first row so the fit pane shows something on open. An
        // empty black box reads as "broken", and the first row is the one
        // model with a fit to show.
        if (t->rowCount() > 0) {
            t->setCurrentCell(0, 0);
        }

        connect(train_, &QPushButton::clicked, this,
                [this] { run_walk_forward(); });
    }

public:
    /// Select a stable Atlas identity in the canonical Models workspace.
    /// Unknown identities are rejected without changing the current selection.
    [[nodiscard]] bool focus_atlas_model(const QString& id) {
        if (detail_ == nullptr) return false;
        const AtlasRow* row = atlas_row_by_id(id);
        if (row == nullptr) return false;
        atlas_id_ = id;
        const QString route = row->page >= 0
            ? nav_page_id(nav_destination(row->page))
            : QStringLiteral("models.overview");

        // Training and evaluation come from the readiness catalogue, matched
        // on the engine header; the Atlas knows only whether the engine is built.
        const auto rows = model_catalogue();
        int match = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            if (!rows[static_cast<std::size_t>(i)].header.isEmpty()
                && rows[static_cast<std::size_t>(i)].header == QString::fromUtf8(row->file)) {
                match = i;
                break;
            }
        }
        const ModelRow* m = match >= 0 ? &rows[static_cast<std::size_t>(match)] : nullptr;
        const QString training = m != nullptr
            ? model_state_label(m->state)
            : QStringLiteral("no fitted artefact (not in the readiness catalogue)");
        const QString evaluation = m != nullptr && m->card.startsWith(QStringLiteral("P8-13"))
            ? QStringLiteral("walk-forward available: press \u201cTrain selected model\u201d")
            : QStringLiteral("no out-of-sample evaluator wired");
        atlas_card_->setText(QStringLiteral(
            "<b>%1</b> <span style='color:#8B949E'>\u00b7 %2 \u00b7 %3</span><br>"
            "<span style='color:#8B949E'>%4</span>"
            "<table cellspacing='0' cellpadding='3' style='margin-top:6px'>"
            "<tr><td style='color:#8B949E'>Implementation</td><td>%5</td></tr>"
            "<tr><td style='color:#8B949E'>Training</td><td>%6</td></tr>"
            "<tr><td style='color:#8B949E'>Evaluation</td><td>%7</td></tr>"
            "<tr><td style='color:#8B949E'>Live</td><td>not approved \u2014 live "
            "order dispatch is disabled by design</td></tr></table>")
            .arg(QString::fromUtf8(row->model).toHtmlEscaped(),
                 QString::fromUtf8(row->family).toHtmlEscaped(), id.toHtmlEscaped(),
                 QString::fromUtf8(row->what).toHtmlEscaped(),
                 QString::fromUtf8(atlas_status_text(row->status)),
                 training.toHtmlEscaped(), evaluation));
        atlas_box_->show();

        if (m != nullptr && table_ != nullptr) {
            // Show its real fit, in the pane that already knows how to.
            if (filter_ != nullptr) filter_->clear();
            focusing_ = true;
            table_->setCurrentCell(match, 0);
            focusing_ = false;
            show_fit(match);
            return true;
        }
        focusing_ = true;
        if (table_ != nullptr) table_->clearSelection();
        focusing_ = false;
        detail_->setPlainText(QStringLiteral(
            "MODEL ID: %1\n\n"
            "%2\n"
            "Family: %3\n"
            "Engine: %4\n"
            "Build state: %5\n"
            "Workspace: %6\n\n"
            "%7\n\n"
            "BUILT describes the numerical implementation and deterministic tests. "
            "It does not claim a fitted market artefact or live approval; those "
            "are separate gates in the card above.")
            .arg(id, QString::fromUtf8(row->model), QString::fromUtf8(row->family),
                 QString::fromUtf8(row->file),
                 QString::fromUtf8(atlas_status_text(row->status)), route,
                 QString::fromUtf8(row->what)));
        return true;
    }

    [[nodiscard]] QString selected_atlas_id() const { return atlas_id_; }
    [[nodiscard]] QString atlas_detail_text() const {
        return detail_ != nullptr ? detail_->toPlainText() : QString{};
    }

    /// P11Q-08. What the Train button does.
    ///
    /// PUBLIC so `--train` can invoke it, for the same reason `--page` exists:
    /// a capture script cannot click, and synthesising a click sends it to
    /// whatever window has focus -- which in this session put three arrow keys
    /// into the user's browser. A named entry point is the honest version of
    /// that shortcut.
    ///
    /// IT DOES NOT FIT A MODEL. IT TRIES TO BREAK ONE.
    ///
    /// "Train" on a row that is already fitted would produce the same numbers
    /// the pane already shows, and a button that reproduces the display is a
    /// button that teaches nobody anything. The question a person clicking
    /// Train actually has is "does this work", and P8-13's chi-square does not
    /// answer it -- 298.07 against a critical 26.30 is an IN-SAMPLE statement
    /// about serial dependence, and it reads like a working model.
    ///
    /// So this runs P8-14's walk-forward evaluation and prints the answer,
    /// including the part that says no strategy may trade it.
    void run_walk_forward() {
        const auto rows = model_catalogue();
        const int row = table_ != nullptr ? table_->currentRow() : -1;
        if (row < 0 || row >= static_cast<int>(rows.size())) {
            return;
        }
        const ModelRow& m = rows[static_cast<std::size_t>(row)];
        if (!m.card.startsWith(QStringLiteral("P8-13"))) {
            status_->setText(QStringLiteral(
                "<span style='color:#B9770B'>Nothing to run for %1.</span> "
                "Walk-forward needs a model that can be fitted from data that "
                "is here, and only the Markov chain is. Every other row would "
                "be evaluated on data it does not have \u2014 which produces a "
                "number, and the number would be about nothing.").arg(m.name));
            return;
        }

        status_->setText(QStringLiteral("Running 26 walk-forward folds over the "
                                        "real series in the background\u2026"));
        train_->setEnabled(false);
        const QString path = QStringLiteral(ALTAIR_DATASET_DIR)
                           + QStringLiteral("/spot/nifty/1d/all.csv");
        // ModelJobController fits on a worker thread and publishes only the
        // newest job's result: the window stays responsive, and a stale run
        // can never overwrite a newer one.
        (void)jobs_.submit(
            QStringLiteral("walk-forward Markov, %1").arg(path),
            [path](const ModelJobContext&) -> ModelJobPayload {
                const LoadResult d = load_bars_csv(
                    path, 24LL * 3600 * 1'000'000'000LL,
                    DailyStamp::SessionClose, true);
                if (!d.ok()) throw std::runtime_error(d.error.toStdString());
                // 2,000 bars of initial training and 500-bar test blocks,
                // alpha 0.5. Every one is a modelling choice and every one is
                // passed explicitly -- see markov_eval.hpp.
                const WalkForwardFit w = walk_forward_markov(d.bars, 5, 2000, 500, 0.5);
                if (!w.ok) throw std::runtime_error(w.error.toStdString());
                auto [report, verdict] = format_walk_forward(w);
                ModelJobPayload out{std::move(report)};
                out.rows.push_back({QStringLiteral("verdict"), std::move(verdict)});
                return out;
            },
            [this](int, const QString& text) {
                if (!text.isEmpty()) status_->setText(text + QStringLiteral("\u2026"));
            },
            [this](const ModelJobResult& r) {
                train_->setEnabled(can_train_);
                if (!r.error.isEmpty()) { status_->setText(r.error); return; }
                if (r.cancelled) { status_->setText(QStringLiteral("Cancelled.")); return; }
                detail_->setPlainText(r.payload.report);
                status_->setText(r.payload.rows.empty() ? QString()
                                                        : r.payload.rows.front().value);
            });
    }

private:
    /// The walk-forward report and its one-line verdict. Pure: it runs on the
    /// job's worker thread and touches no widget.
    [[nodiscard]] static std::pair<QString, QString>
    format_walk_forward(const WalkForwardFit& w) {
        QString o;
        o += QStringLiteral("WALK-FORWARD \u2014 the out-of-sample answer "
                            "(P8-14), computed now\n\n");
        o += QStringLiteral("  EXPANDING window, %1 folds, %2 scored "
                            "transitions\n").arg(w.folds).arg(w.scored);
        o += QStringLiteral("    distributional edge  %1 nats/obs, %2 of %3 "
                            "folds positive\n")
                 .arg(w.mean_edge, 0, 'f', 5).arg(w.folds_positive)
                 .arg(w.folds);
        o += QStringLiteral("    t                    %1  (OPTIMISTIC: "
                            "expanding folds share training data)\n")
                 .arg(w.t_stat, 0, 'f', 2);
        o += QStringLiteral("    impossible           %1 transitions the chain "
                            "ruled out, that happened\n").arg(w.impossible);
        o += QStringLiteral("\n  AND THAT EDGE IS NOT A DECISION.\n");
        o += QStringLiteral("    SIGN       chain %1  vs  %2 constant   %3\n")
                 .arg(w.sign_chain, 0, 'f', 4).arg(w.sign_base, 0, 'f', 4)
                 .arg(w.sign_chain - w.sign_base, 0, 'f', 4);
        o += QStringLiteral("    MAGNITUDE  chain %1  vs  %2 constant   %3\n")
                 .arg(w.mag_chain, 0, 'f', 4).arg(w.mag_base, 0, 'f', 4)
                 .arg(w.mag_chain - w.mag_base, 0, 'f', 4);
        o += QStringLiteral("\n  ROLLING window (old data falls out)\n");
        o += QStringLiteral("    distributional edge  %1, %2 of %3 positive\n")
                 .arg(w.roll_mean_edge, 0, 'f', 5).arg(w.roll_folds_positive)
                 .arg(w.folds);
        o += QStringLiteral("    SIGN       chain %1  vs  %2 constant   %3\n")
                 .arg(w.roll_sign_chain, 0, 'f', 4)
                 .arg(w.roll_sign_base, 0, 'f', 4)
                 .arg(w.roll_sign_chain - w.roll_sign_base, 0, 'f', 4);
        o += QStringLiteral("    MAGNITUDE  chain %1  vs  %2 constant   %3\n")
                 .arg(w.roll_mag_chain, 0, 'f', 4)
                 .arg(w.roll_mag_base, 0, 'f', 4)
                 .arg(w.roll_mag_chain - w.roll_mag_base, 0, 'f', 4);
        o += QStringLiteral(
            "\n  A state here is a RETURN QUANTILE, so states 0 and 4 are both "
            "large moves\n  differing in sign. Volatility clusters, so serial "
            "dependence appears from\n  GARCH alone with nothing directional "
            "in it \u2014 which is why the two lines\n  above exist and the "
            "single log-score does not settle anything.\n");
        o += QStringLiteral(
            "\n  The expanding window's MAGNITUDE loss is an artefact: the "
            "training median\n  |return| stays inflated by the volatile 1990s, "
            "so \"never large\" is free. Let\n  the old data fall out and the "
            "chain wins that comparison instead.\n");
        const QString verdict =
            w.no_directional_edge
                ? QStringLiteral(
                      "<b style='color:#F85149'>No directional edge under "
                      "either window.</b> The chain beats a constant on "
                      "distributional fit and loses to it on sign (%1 vs %2 "
                      "expanding, %3 vs %4 rolling). Usable as a REGIME "
                      "CONDITIONER \u2014 report per regime, size differently "
                      "within one. Not as a direction.")
                      .arg(w.sign_chain, 0, 'f', 4).arg(w.sign_base, 0, 'f', 4)
                      .arg(w.roll_sign_chain, 0, 'f', 4)
                      .arg(w.roll_sign_base, 0, 'f', 4)
                : QStringLiteral(
                      "<b style='color:#B9770B'>Sign accuracy is above the "
                      "constant baseline.</b> CLAUDE.md puts the ceiling at "
                      "52\u201355%% and anything above it is overfit until "
                      "proven otherwise \u2014 and this is still pre-cost, so "
                      "rule 5 has not been applied.");
        return {o, verdict};
    }

private:
    /// Run the selected model on the real series, or say why not.
    ///
    /// EVERY NUMBER BELOW IS COMPUTED HERE, NOW. Nothing is cached from a
    /// previous run and nothing is a literal, so a fit that stops converging
    /// shows up as a changed number rather than as a stale string that still
    /// says what it said in September.
    void show_fit(int row) {
        const auto rows = model_catalogue();
        if (row < 0 || row >= static_cast<int>(rows.size())) {
            return;
        }
        const ModelRow& m = rows[static_cast<std::size_t>(row)];
        QString out;
        out += QStringLiteral("%1   (%2, %3)\n")
                   .arg(m.name, m.card, m.header);
        out += QStringLiteral("state : %1\n").arg(model_state_label(m.state));
        out += QStringLiteral("needs : %1\n").arg(m.needs);
        out += QStringLiteral("has   : %1\n\n").arg(m.has);

        if (m.state != ModelState::TrainedOnRealData) {
            out += QStringLiteral(
                "No fit is run for this model, because the data it needs is "
                "not here. A panel that showed a number anyway would be "
                "showing a number about nothing.");
            detail_->setPlainText(out);
            return;
        }

        const QString root = QStringLiteral(ALTAIR_DATASET_DIR);
        if (m.card.startsWith(QStringLiteral("P8-13"))) {
            const LoadResult d = load_bars_csv(
                root + QStringLiteral("/spot/nifty/1d/all.csv"),
                24LL * 3600 * 1'000'000'000LL, DailyStamp::SessionClose, true);
            if (!d.ok()) {
                detail_->setPlainText(out + d.error);
                return;
            }
            const MarkovFit f = fit_markov(d.bars);
            if (!f.ok) {
                detail_->setPlainText(out + f.error);
                return;
            }
            out += QStringLiteral("FITTED NOW on %1 daily returns\n\n")
                       .arg(f.returns);
            out += QStringLiteral("  transitions        %1 over %2 states\n")
                       .arg(f.transitions).arg(f.states);
            out += QStringLiteral("  thinnest cell      %1   empty cells %2\n")
                       .arg(f.thinnest_cell).arg(f.empty_cells);
            out += QStringLiteral("  look-ahead         %1 labels change under "
                                  "an expanding boundary (%2%)\n")
                       .arg(f.relabelled)
                       .arg(f.relabelled_pct, 0, 'f', 1);
            out += QStringLiteral("\n  chi2 real          %1\n")
                       .arg(f.chi_square, 0, 'f', 2);
            // "one draw" is not a hedge, it is the reading instruction. The
            // engine test shuffles differently and gets 19.25; this gets 13.08.
            // Both are far under the 26.30 critical value, so both say the same
            // thing -- but a reader who sees the two numbers without being told
            // they are separate permutations will think one of them is stale.
            out += QStringLiteral(
                       "  chi2 shuffled      %1   <- the control, one draw\n")
                       .arg(f.chi_square_shuffled, 0, 'f', 2);
            out += QStringLiteral("  critical (5%)      %1\n")
                       .arg(f.critical_5pct, 0, 'f', 2);
            out += QStringLiteral("  verdict            %1\n")
                       .arg(f.rejects && !f.shuffled_rejects
                                ? QStringLiteral("real rejects, shuffled does "
                                                 "not -> serial dependence")
                                : QStringLiteral("inconclusive"));
            out += QStringLiteral("\n  stationary         ");
            for (double x : f.stationary_dist) {
                out += QStringLiteral("%1 ").arg(x, 0, 'f', 3);
            }
            // The pointer used to say "that is P11Q-08", which was true until
            // P11Q-08 shipped. A forward reference to a card that has landed
            // is a stale literal of the sort this panel already got wrong
            // once, so it now names the button instead -- and states the
            // answer, because a reader who never clicks would otherwise leave
            // with the chi-square and nothing qualifying it.
            out += QStringLiteral(
                "\n\nIN-SAMPLE. This says the chain can be estimated and what "
                "it looks like.\nIt says nothing about whether it predicts, "
                "and 298 against a critical 26\nreads like it does.\n\n"
                "PRESS \"Train selected model\" FOR THE OUT-OF-SAMPLE ANSWER. "
                "Briefly: the\nchain beats a constant on distributional fit "
                "and loses to one on sign,\nso there is no directional edge to "
                "take a cost off and rule 5 never\nengages. Usable as a regime "
                "conditioner, not as a direction.");
        } else if (m.card.startsWith(QStringLiteral("P10-07"))) {
            const LoadResult d = load_bars_csv(
                root + QStringLiteral("/spot/indiavix/1d/all.csv"),
                24LL * 3600 * 1'000'000'000LL, DailyStamp::SessionClose, true);
            if (!d.ok()) {
                detail_->setPlainText(out + d.error);
                return;
            }
            const VixFit f = fit_vix_both_spaces(d.bars);
            if (!f.ok) {
                detail_->setPlainText(out + f.error);
                return;
            }
            out += QStringLiteral("FITTED NOW on %1 daily India VIX bars\n\n")
                       .arg(f.observations);
            out += QStringLiteral("  level  b %1  half-life %2 obs  sd %3\n")
                       .arg(f.level_b, 0, 'f', 4)
                       .arg(f.level_half_life, 0, 'f', 1)
                       .arg(f.level_sd, 0, 'f', 4);
            out += QStringLiteral("  log    b %1  half-life %2 obs  sd %3\n")
                       .arg(f.log_b, 0, 'f', 4)
                       .arg(f.log_half_life, 0, 'f', 1)
                       .arg(f.log_sd, 0, 'f', 4);
            out += QStringLiteral("\n  regimes split at VIX %1 / %2 (series "
                                  "quartiles, not hard-coded levels)\n")
                       .arg(f.quiet_threshold, 0, 'f', 2)
                       .arg(f.stressed_threshold, 0, 'f', 2);
            out += QStringLiteral("  LEVEL residual sd  %1 quiet (%2 obs)  vs  "
                                  "%3 stressed (%4 obs)  = %5x\n")
                       .arg(f.level_sd_quiet, 0, 'f', 4).arg(f.quiet_n)
                       .arg(f.level_sd_stressed, 0, 'f', 4).arg(f.stressed_n)
                       .arg(f.level_sd_quiet > 0.0
                                ? f.level_sd_stressed / f.level_sd_quiet
                                : 0.0, 0, 'f', 2);
            out += QStringLiteral("\n  coverage of a nominal 95.4% band\n");
            out += QStringLiteral("    level   %1% quiet   %2% stressed\n")
                       .arg(f.level_cover_quiet, 0, 'f', 1)
                       .arg(f.level_cover_stressed, 0, 'f', 1);
            out += QStringLiteral("    log     %1% quiet   %2% stressed\n")
                       .arg(f.log_cover_quiet, 0, 'f', 1)
                       .arg(f.log_cover_stressed, 0, 'f', 1);
            out += QStringLiteral(
                "\nThe level band UNDER-covers when stressed -- too narrow "
                "exactly where the\nnumber is needed, which is the dangerous "
                "direction. P10-07 measured this on\nsynthetic data at 2.0x; "
                "on the real series it is larger.\n\nIN-SAMPLE.");
        } else {
            out += QStringLiteral(
                "Fitted on real data by its own test (%1, %2). This page has no "
                "in-app fit view for it yet, so nothing is recomputed here; run "
                "that test for the numbers rather than trusting a stale string.")
                       .arg(m.card, m.header);
        }
        detail_->setPlainText(out);
    }

    /// Hide rows whose text does not contain `text` (case-insensitive).
    void apply_filter(const QString& text) {
        if (table_ == nullptr) return;
        const QString needle = text.trimmed();
        for (int r = 0; r < table_->rowCount(); ++r) {
            bool hit = needle.isEmpty();
            for (int c = 0; !hit && c < table_->columnCount(); ++c) {
                const QTableWidgetItem* it = table_->item(r, c);
                hit = it != nullptr && it->text().contains(needle, Qt::CaseInsensitive);
            }
            table_->setRowHidden(r, !hit);
        }
    }

    ModelJobController jobs_;
    bool can_train_ = false;
    bool focusing_ = false;
    QTableWidget* table_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QWidget* atlas_box_ = nullptr;
    QLabel* atlas_card_ = nullptr;
    QPlainTextEdit* detail_ = nullptr;
    QPushButton* train_ = nullptr;
    QLabel* status_ = nullptr;
    QString atlas_id_;
};

// ---------------------------------------------------------------------------
// The dataflow diagram
// ---------------------------------------------------------------------------

class DataflowWidget final : public QWidget {
    Q_OBJECT

public:
    explicit DataflowWidget(FeedSource source, QWidget* parent = nullptr)
        : QWidget(parent), source_(source) {
        setMinimumHeight(220);
        setAutoFillBackground(true);
    }

    void set_source(FeedSource s) {
        source_ = s;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(0x1A, 0x1F, 0x24));

        const auto stages = pipeline(source_);
        if (stages.empty()) {
            return;
        }
        const std::size_t wall = furthest_working_stage(source_);

        const int margin = 14;
        const int box_h = 46;
        const int gap = 10;
        const int cols = 4;
        const int box_w =
            (width() - 2 * margin - (cols - 1) * gap) / cols;
        const int top = margin + 22;

        p.setPen(QColor(0xD6, 0xDB, 0xDF));
        p.drawText(margin, margin + 12,
                   QStringLiteral("Data path on %1 — a tick reaches %2 of %3 "
                                  "stages, stopping at %4")
                       .arg(feed_name(source_))
                       .arg(wall)
                       .arg(stages.size())
                       .arg(wall < stages.size() ? stages[wall].name
                                                 : QStringLiteral("nothing")));

        for (std::size_t i = 0; i < stages.size(); ++i) {
            const int c = static_cast<int>(i) % cols;
            const int r = static_cast<int>(i) / cols;
            const int x = margin + c * (box_w + gap);
            const int y = top + r * (box_h + gap + 14);

            const QColor col = wiring_colour(stages[i].state);
            const bool past_wall = i >= wall;

            // A stage past the wall is HOLLOW. The picture and the table are
            // the same vector, so they cannot drift apart.
            p.setPen(QPen(col, past_wall ? 1 : 2,
                          past_wall ? Qt::DashLine : Qt::SolidLine));
            p.setBrush(past_wall ? Qt::NoBrush
                                 : QBrush(QColor(col.red(), col.green(),
                                                 col.blue(), 40)));
            p.drawRoundedRect(x, y, box_w, box_h, 4, 4);

            p.setPen(QColor(0xEC, 0xF0, 0xF1));
            p.drawText(x + 8, y + 18, stages[i].name);
            p.setPen(QColor(0x7F, 0x8C, 0x8D));
            const QFontMetrics fm(p.font());
            p.drawText(x + 8, y + 34,
                       fm.elidedText(stages[i].directory, Qt::ElideRight,
                                     box_w - 16));

            // The arrow to the next stage.
            if (i + 1 < stages.size() && c + 1 < cols) {
                p.setPen(QPen(QColor(0x4A, 0x54, 0x5C), 1));
                p.drawLine(x + box_w, y + box_h / 2, x + box_w + gap,
                           y + box_h / 2);
            }
            // The wall itself.
            if (i == wall) {
                p.setPen(QPen(QColor(0xC0, 0x39, 0x2B), 2, Qt::DotLine));
                p.drawLine(x - gap / 2, y - 6, x - gap / 2, y + box_h + 6);
            }
        }
    }

private:
    FeedSource source_ = FeedSource::Unspecified;
};

// ---------------------------------------------------------------------------
// P29-02 — the quote snapshot the watchlist reads
//
// `altair_kite_quote` writes it; this window reads it and never calls the API.
// Same shape as the account snapshot and for the same reason: desktop/ does
// not link broker/, so the network half lives in a separate process and the
// UI reads a file.
//
// AN INDEX HAS NO BOOK, AND THAT IS NOT THE SAME AS NO SNAPSHOT.
//
// NIFTY 50 is computed from things that trade; it does not trade itself, so
// there is nothing to bid for. `broker/kite_quote.hpp` says so in its own
// tests -- "an index does not trade ... `has_touch()` says so rather than
// returning zeros" -- and the watchlist must render that differently from a
// row it simply has no data for. Blank in both cases would collapse two facts
// into one.
// ---------------------------------------------------------------------------

struct QuoteRow {
    std::int64_t last_paise = 0;
    std::int64_t bid_paise = 0;
    std::int64_t ask_paise = 0;
    bool has_touch = false;
    bool present = false;      ///< the snapshot carried this token at all
};

struct QuoteSnapshot {
    QHash<std::uint32_t, QuoteRow> by_token;
    qint64 fetched_at_unix = 0;
    bool loaded = false;
    QString error;

    /// Age in words. A bid with no age gets believed.
    [[nodiscard]] QString age_text() const {
        if (!loaded) { return error.isEmpty()
                              ? QStringLiteral("no quote snapshot") : error; }
        const qint64 secs =
            QDateTime::currentSecsSinceEpoch() - fetched_at_unix;
        if (secs < 0) { return QStringLiteral("timestamped in the FUTURE"); }
        if (secs < 90) { return QStringLiteral("%1 s old").arg(secs); }
        if (secs < 5400) { return QStringLiteral("%1 min old").arg(secs / 60); }
        return QStringLiteral("%1 h old — STALE").arg(secs / 3600);
    }
};

[[nodiscard]] inline QuoteSnapshot load_quotes(const QString& path) {
    QuoteSnapshot s;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        s.error = QStringLiteral("no quote snapshot — run altair_kite_quote");
        return s;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull() || !doc.isObject()) {
        // A malformed snapshot is NOT "no snapshot": one means nobody has
        // fetched, the other means something wrote a broken file, and only
        // the second is a bug.
        s.error = QStringLiteral("quote snapshot is malformed: %1")
                      .arg(err.errorString());
        return s;
    }
    const QJsonObject o = doc.object();
    s.fetched_at_unix =
        static_cast<qint64>(o.value(QStringLiteral("fetched_at_unix")).toDouble());
    const QJsonObject qs = o.value(QStringLiteral("quotes")).toObject();
    for (auto it = qs.begin(); it != qs.end(); ++it) {
        if (!it.value().isObject()) { continue; }   // null = not quoted
        const QJsonObject q = it.value().toObject();
        QuoteRow r;
        r.present = true;
        r.last_paise = static_cast<std::int64_t>(
            q.value(QStringLiteral("last_paise")).toDouble());
        r.has_touch = q.value(QStringLiteral("has_touch")).toBool();
        const QJsonArray buy = q.value(QStringLiteral("buy")).toArray();
        const QJsonArray sell = q.value(QStringLiteral("sell")).toArray();
        if (!buy.isEmpty()) {
            r.bid_paise = static_cast<std::int64_t>(
                buy.at(0).toObject().value(QStringLiteral("price_paise"))
                    .toDouble());
        }
        if (!sell.isEmpty()) {
            r.ask_paise = static_cast<std::int64_t>(
                sell.at(0).toObject().value(QStringLiteral("price_paise"))
                    .toDouble());
        }
        const auto token = static_cast<std::uint32_t>(
            q.value(QStringLiteral("token")).toDouble());
        if (token != 0) { s.by_token.insert(token, r); }
    }
    s.loaded = true;
    return s;
}

// ---------------------------------------------------------------------------
// Watchlist
// ---------------------------------------------------------------------------

class WatchlistPanel final : public QWidget {
    Q_OBJECT

public:
    explicit WatchlistPanel(Role role, QWidget* parent = nullptr)
        : QWidget(parent), role_(role) {
        auto* v = new QVBoxLayout(this);
        v->addWidget(new QLabel(
            QStringLiteral("<h3>Watchlist</h3>"), this));

        auto* note = new QLabel(
            QStringLiteral(
                "A row can be added with just a token and a name — that is all "
                "a <i>price</i> needs. Lot size and tick size come from the "
                "point-in-time spec store and are never guessed, so an "
                "unresolved row is marked <b>watch only</b> and cannot be "
                "sized or ordered."),
            this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(note);

        auto* add_row = new QHBoxLayout;
        token_ = new QSpinBox(this);
        token_->setRange(1, 2'000'000'000);
        token_->setValue(256265);
        symbol_ = new QLineEdit(this);
        symbol_->setPlaceholderText(QStringLiteral("instrument name"));
        add_ = new QPushButton(QStringLiteral("Add"), this);
        add_->setEnabled(may(role_, Capability::EditWatchlist));
        remove_ = new QPushButton(QStringLiteral("Remove"), this);
        remove_->setEnabled(may(role_, Capability::EditWatchlist));

        add_row->addWidget(new QLabel(QStringLiteral("Token")));
        add_row->addWidget(token_);
        add_row->addWidget(new QLabel(QStringLiteral("Name")));
        add_row->addWidget(symbol_, 1);
        add_row->addWidget(add_);
        add_row->addWidget(remove_);
        // P29-02. Runs altair_kite_quote as a SUBPROCESS. desktop/ does not
        // link broker/ and this button does not change that -- it launches a
        // read-only fetcher and then re-reads the file it wrote.
        quotes_ = new QPushButton(QStringLiteral("Refresh quotes"), this);
        add_row->addWidget(quotes_);
        v->addLayout(add_row);

        message_ = new QLabel(this);
        message_->setWordWrap(true);
        v->addWidget(message_);

        // P11Q-11. The profile columns come from the Kite master; the QUOTE
        // columns cannot, and are here so their absence is visible rather
        // than implied. A watchlist with no bid column looks complete; one
        // with an empty bid column says what is missing.
        table_ = fact_table({QStringLiteral("Token"), QStringLiteral("Name"),
                             QStringLiteral("Seg"), QStringLiteral("Expiry"),
                             QStringLiteral("Strike"), QStringLiteral("Type"),
                             QStringLiteral("Lot"), QStringLiteral("Tick"),
                             QStringLiteral("LTP"), QStringLiteral("Bid"),
                             QStringLiteral("Ask"), QStringLiteral("Spread"),
                             QStringLiteral("Spec"), QStringLiteral("Note")},
                            this);
        v->addWidget(table_, 1);

        quote_age_ = new QLabel(this);
        quote_age_->setWordWrap(true);
        quote_age_->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(quote_age_);

        summary_ = new QLabel(this);
        summary_->setWordWrap(true);
        
        v->addWidget(summary_);

        master_.load(QStringLiteral(ALTAIR_SOURCE_DIR
                                    "/data/instruments.csv"));

        // SEEDED WITH FOUR REAL INSTRUMENTS, in Smit's stated priority order
        // plus one option, so the profile columns show something rather than
        // an empty table that looks like a broken panel.
        //
        // The names here are placeholders: `refresh()` prefers the MASTER's
        // symbol, which is authoritative, so what appears is what Kite calls
        // the contract and not what this line guessed. The option is included
        // because it is the only one of the four that exercises strike,
        // expiry and CE/PE at once.
        struct Seed { std::uint32_t token; const char* name; };
        for (const Seed& sd : {Seed{256265,   "NIFTY 50"},
                               Seed{17512194, "NIFTY near future"},
                               Seed{264969,   "INDIA VIX"},
                               Seed{10915586, "NIFTY 24000 CE"}}) {
            (void)list_.add(sd.token, QString::fromUtf8(sd.name));
        }

        connect(quotes_, &QPushButton::clicked, this,
                [this] { on_refresh_quotes(); });
        connect(add_, &QPushButton::clicked, this, &WatchlistPanel::on_add);
        connect(remove_, &QPushButton::clicked, this,
                &WatchlistPanel::on_remove);
        connect(table_, &QTableWidget::currentCellChanged, this,
                [this](int row, int, int, int) { emit_pick(row); });
        refresh();
    }

    /// The Kite master, loaded ONCE. 108,411 rows and 8.8 MB -- scanning it
    /// per row is a freeze that arrives at about the tenth instrument, late
    /// enough to read as a different bug.
    MasterIndex master_;

Q_SIGNALS:
    /// A row was selected. P32-01: the terminal's one wire.
    ///
    /// The SYMBOL sent is the master's, not the typed one, for the same
    /// reason the table shows the master's: the typed name is whatever
    /// somebody entered and the master's is what Kite calls the contract.
    void instrumentPicked(unsigned token, const QString& symbol);
    /// P39. The same pick WITH the contract's spec, for the order ticket,
    /// which now refuses any contract whose lot, tick and exchange it has not
    /// been given. Emitted only for a row the master resolved: an unresolved
    /// row still emits instrumentPicked, and the ticket then says plainly
    /// that it cannot request it.
    void contractPicked(unsigned token, const QString& symbol, qint64 lot,
                        qint64 tick_paise, const QString& exchange);

public:

    [[nodiscard]] const Watchlist& list() const noexcept { return list_; }

    /// The spec store refused this instrument. The row stays, visibly
    /// BLOCKED, and emit_pick will never send its spec to the ticket.
    void block_instrument(unsigned token, const QString& why) {
        if (list_.block(token, why)) { refresh(); }
    }

private:
    QPushButton* quotes_ = nullptr;
    QLabel* quote_age_ = nullptr;
    QuoteSnapshot quotes_snap_{};
#ifdef ALTAIR_QUOTE_FILE
    QString quote_path_ = QStringLiteral(ALTAIR_QUOTE_FILE);
#else
    QString quote_path_ = QStringLiteral("data/kite_quotes.json");
#endif

public:

private:
    /// Emit the selected row's identity, if it has one.
    ///
    /// Guarded on the row index rather than trusted: `currentCellChanged`
    /// fires during setRowCount() while the model is being rebuilt, when the
    /// current row can point past the end of `list_.rows()`. Reading it there
    /// is a crash in a process that is holding positions.
    void emit_pick(int row) {
        const auto& rows = list_.rows();
        if (row < 0 || row >= static_cast<int>(rows.size())) { return; }
        const WatchRow& r = rows[static_cast<std::size_t>(row)];
        const std::uint32_t token = r.token;
        const SpecState state = r.spec;
        const InstrumentProfile p = master_.find(token);
        Q_EMIT instrumentPicked(token, p.found ? p.symbol : r.symbol);
        // CX02-B4b (C17-021). A BLOCKED row never reaches the ticket with a
        // spec -- rule 9: the spec store said no, and a master lookup does not
        // overrule it. It sends an EMPTY spec instead (lot 0, tick 0, no
        // exchange), which the ticket takes as a revocation: a row picked
        // while resolvable and blocked afterwards must not leave its old spec
        // in the ticket. And a row the master can price is marked FromMaster
        // before its spec is sent, so the Spec column says WHERE the numbers
        // came from instead of "watch only" beside a contract the ticket will
        // accept.
        if (state == SpecState::Blocked) {
            Q_EMIT contractPicked(token, p.found ? p.symbol : r.symbol, 0, 0,
                                  QString());
            return;
        }
        if (p.found && p.lot_size > 0 && p.tick_paise > 0
            && !p.kite_exchange.isEmpty()) {
            // CX02-B4d (R-AB-041). NOT resolve(): that word means the
            // point-in-time spec store agreed, and `desktop/` cannot even
            // link it. These numbers are the instrument master's, and the row
            // now says so rather than claiming more than was checked.
            if (state != SpecState::Resolved && state != SpecState::FromMaster
                && list_.from_master(token, p.lot_size, p.tick_paise)) {
                put(table_, row, 12, spec_state_label(SpecState::FromMaster),
                    QColor(0xB9, 0x77, 0x0B));
            }
            Q_EMIT contractPicked(token, p.symbol, p.lot_size, p.tick_paise,
                                  p.kite_exchange);
        }
    }

private Q_SLOTS:
    void on_add() {
        const AddResult r =
            list_.add(static_cast<std::uint32_t>(token_->value()),
                      symbol_->text());
        message_->setText(add_result_label(r));
        message_->setStyleSheet(
            QStringLiteral("color:%1;")
                .arg(r == AddResult::Added ? QStringLiteral("#3FB950")
                                           : QStringLiteral("#F85149")));
        if (r == AddResult::Added) {
            symbol_->clear();
        }
        refresh();
    }

    void on_remove() {
        const int row = table_->currentRow();
        if (row < 0 || row >= static_cast<int>(list_.rows().size())) {
            return;
        }
        list_.remove(list_.rows()[static_cast<std::size_t>(row)].token);
        refresh();
    }

private:
    /// Run the read-only quote fetcher, then re-read what it wrote.
    ///
    /// The working directory is pinned for the same reason P26-02b had to pin
    /// it for the login: the fetcher writes a RELATIVE path, and a subprocess
    /// launched from a desktop shortcut would otherwise drop the snapshot in
    /// the build folder where nothing reads it, while reporting success.
    void on_refresh_quotes() {
        QString exe =
#if defined(_WIN32)
            QStringLiteral("altair_kite_quote.exe");
#else
            QStringLiteral("altair_kite_quote");
#endif
        QStringList tried;
        tried << QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../app/") + exe;
        tried << QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../Helpers/") + exe;
        tried << QCoreApplication::applicationDirPath()
                     + QStringLiteral("/../../net/app/") + exe;
#ifdef ALTAIR_SOURCE_DIR
        tried << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
        QString found;
        for (const QString& c : tried) {
            if (QFileInfo(c).isFile()) {
                found = QFileInfo(c).canonicalFilePath();
                break;
            }
        }
        if (found.isEmpty()) {
            quote_age_->setText(QStringLiteral(
                "altair_kite_quote was not found. It needs an HTTPS client "
                "and is built by the `net` preset only."));
            return;
        }
        // FETCH WHAT THE WATCHLIST ACTUALLY HOLDS, not a fixed default.
        //
        // The fetcher's default --keys is the three indices the live grid
        // carries. A watchlist with a future and an option in it then showed
        // two populated rows and two blank ones, which reads as "those two
        // are not quoting" rather than "nobody asked about them".
        //
        // The exchange comes from the SEGMENT, not from a guess: cash is NSE,
        // futures and options are NFO. A future sent to NSE is a 400 from
        // Kite, and a row that silently asks the wrong venue looks exactly
        // like a row that does not trade.
        QStringList keys;
        for (const WatchRow& r : list_.rows()) {
            const InstrumentProfile p = master_.find(r.token);
            const QString sym = p.found ? p.symbol : r.symbol;
            if (sym.isEmpty()) { continue; }
            const QString ex =
                (p.segment == QStringLiteral("FUT")
                 || p.segment == QStringLiteral("OPT"))
                    ? QStringLiteral("NFO") : QStringLiteral("NSE");
            keys << (ex + QLatin1Char(':') + sym);
        }
        if (keys.isEmpty()) {
            quote_age_->setText(QStringLiteral("nothing on the watchlist to "
                                               "quote"));
            return;
        }

        quote_age_->setText(QStringLiteral("fetching %1 instruments...")
                                .arg(keys.size()));
        QProcess proc;
        proc.setProgram(found);
        proc.setArguments({QStringLiteral("--keys"), keys.join(QLatin1Char(',')),
                           QStringLiteral("--go")});
        proc.setProcessChannelMode(QProcess::MergedChannels);
#ifdef ALTAIR_SOURCE_DIR
        proc.setWorkingDirectory(QStringLiteral(ALTAIR_SOURCE_DIR));
#endif
        proc.start();
        if (!proc.waitForStarted(5000) || !proc.waitForFinished(30000)) {
            proc.kill();
            quote_age_->setText(QStringLiteral("the fetcher did not finish"));
            return;
        }
        if (proc.exitCode() != 0) {
            // Kite's own words, not a paraphrase. A daily token that expired
            // and a symbol Kite does not know are different problems.
            quote_age_->setText(
                QStringLiteral("fetch failed: %1")
                    .arg(QString::fromUtf8(proc.readAll()).trimmed()
                             .section(QChar('\n'), -3)));
            return;
        }
        refresh();
    }

    void refresh() {
        quotes_snap_ = load_quotes(quote_path_);
        const auto& rows = list_.rows();
        table_->setRowCount(static_cast<int>(rows.size()));
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const WatchRow& r = rows[static_cast<std::size_t>(i)];
            const InstrumentProfile p = master_.find(r.token);
            const QColor grey(0x7F, 0x8C, 0x8D);

            put(table_, i, 0, QString::number(r.token));
            // The master's own symbol when it has one -- it is authoritative
            // and the typed name is not.
            put(table_, i, 1, p.found ? p.symbol : r.symbol);
            put(table_, i, 2, p.found ? p.segment : QString(), grey);
            // EXPIRY IS AN IST DATE AND MUST BE RENDERED IN IST.
            //
            // The parser stores it as an instant, and formatting that instant
            // in UTC showed NIFTY26SEPFUT expiring 2026-09-28 when the master
            // says the 29th -- IST is UTC+5:30, so an IST-dated contract read
            // in UTC lands on the previous evening and prints the day before.
            //
            // An expiry displayed a day early is not cosmetic: it is the date
            // a roll is planned around, and it would put the roll on the last
            // trading day rather than before it.
            put(table_, i, 3,
                p.expiry_ns > 0
                    ? QDateTime::fromMSecsSinceEpoch(
                          p.expiry_ns / 1'000'000,
                          QTimeZone(5 * 3600 + 30 * 60))
                          .toString(QStringLiteral("yyyy-MM-dd"))
                    : QString(),
                grey);
            // Strike blank for anything that is not an option, rather than
            // "0.00" -- a strike of zero is a price, and no strike is not.
            put(table_, i, 4,
                p.strike > 0 ? format_paise(p.strike) : QString());
            put(table_, i, 5, p.opt_type,
                p.opt_type == QStringLiteral("CE") ? QColor(0x1B, 0x8A, 0x4B)
                : p.opt_type == QStringLiteral("PE") ? QColor(0xC0, 0x39, 0x2B)
                                                     : QColor());
            // A zero lot renders BLANK, not "0". A zero lot size is the bug
            // that silently scaled every P&L number in the predecessor, and
            // showing it as a number is how it gets used as one. Same for the
            // master's value.
            const std::int64_t lot = r.lot_size > 0 ? r.lot_size : p.lot_size;
            const std::int64_t tick =
                r.tick_size_paise > 0 ? r.tick_size_paise : p.tick_paise;
            put(table_, i, 6, lot > 0 ? QString::number(lot) : QString());
            put(table_, i, 7, tick > 0 ? format_paise(tick) : QString());

            // LTP, BID, ASK AND SPREAD, FROM THE QUOTE SNAPSHOT. P29-02.
            //
            // These were empty until now, deliberately -- the master is
            // reference data and carries no quote. `altair_kite_quote` fetches
            // one and this reads the file it writes.
            //
            // THREE STATES, RENDERED DIFFERENTLY, BECAUSE THEY ARE THREE
            // DIFFERENT FACTS:
            //
            //   no snapshot        blank, and the age line says why
            //   quoted, no book    "index" -- NIFTY 50 does not trade, it is
            //                      computed from things that do, so there is
            //                      nothing to bid for. Blank here would be
            //                      indistinguishable from "not fetched".
            //   quoted with a book the numbers
            //
            // Rendering 0.00 in any of them would be a price, which is the one
            // thing none of these is.
            const auto qit = quotes_snap_.by_token.constFind(r.token);
            const bool have_q = qit != quotes_snap_.by_token.constEnd();
            {
                QString ltp, bidt, askt, spr, tip;
                if (!quotes_snap_.loaded) {
                    tip = quotes_snap_.age_text();
                } else if (!have_q) {
                    tip = QStringLiteral(
                        "The last quote snapshot did not carry this "
                        "instrument. Add it to --keys and refresh.");
                } else {
                    ltp = qit->last_paise > 0 ? format_paise(qit->last_paise)
                                              : QString();
                    if (qit->has_touch) {
                        bidt = format_paise(qit->bid_paise);
                        askt = format_paise(qit->ask_paise);
                        spr = format_paise(qit->ask_paise - qit->bid_paise);
                        tip = QStringLiteral("quote %1")
                                  .arg(quotes_snap_.age_text());
                    } else {
                        // "no book", NOT "index".
                        //
                        // The first version of this said "index", which was
                        // wrong for half its rows: NIFTY26SEPFUT is a FUTURE
                        // and does trade, and it showed "index" at 08:50
                        // simply because the market opens at 09:15. Two
                        // different facts wear the same empty touch --
                        //
                        //   an index NEVER has a book: it is computed from
                        //   things that trade and does not trade itself;
                        //   a tradeable instrument has no book WHILE CLOSED.
                        //
                        // The snapshot cannot tell them apart -- `has_touch`
                        // is false in both -- and the instrument master's
                        // INDICES flag does not survive into the segment this
                        // panel sees. So the cell states what is observed and
                        // the tooltip names both readings, rather than the
                        // cell asserting the one that happened to be true for
                        // the row somebody looked at first.
                        bidt = askt = QStringLiteral("no book");
                        tip = QStringLiteral(
                            "Quoted, but the book is empty. Either this is an "
                            "index -- computed from things that trade, so it "
                            "never has a bid -- or it is tradeable and the "
                            "market is closed. This snapshot cannot tell the "
                            "two apart, and it is not a missing quote.");
                    }
                }
                put(table_, i, 8, ltp);
                auto* bid = new QTableWidgetItem(bidt);
                bid->setToolTip(tip);
                table_->setItem(i, 9, bid);
                auto* ask = new QTableWidgetItem(askt);
                ask->setToolTip(tip);
                table_->setItem(i, 10, ask);
                put(table_, i, 11, spr);
            }

            const QColor c = r.spec == SpecState::Resolved
                               ? QColor(0x1B, 0x8A, 0x4B)
                             : r.spec == SpecState::Blocked
                               ? QColor(0xC0, 0x39, 0x2B)
                               : QColor(0xB9, 0x77, 0x0B);
            put(table_, i, 12, spec_state_label(r.spec), c);
            put(table_, i, 13,
                p.found ? r.note
                        : (r.note.isEmpty()
                               ? QStringLiteral("not in the instrument master")
                               : r.note));
        }
        table_->resizeColumnsToContents();
        // THE MASTER'S STATE IS ON SCREEN, NOT INFERRED FROM BLANK CELLS.
        //
        // If it did not load, every profile column is empty -- and an empty
        // Expiry cell is indistinguishable from "this contract has no
        // expiry". A panel that renders the same thing for "no data" and "no
        // value" is the failure this project keeps finding, so the load says
        // so itself.
        quote_age_->setText(
            QStringLiteral("Quotes: %1.  altair_kite_quote writes the "
                           "snapshot; this window reads it and never calls "
                           "the API.").arg(quotes_snap_.age_text()));

        summary_->setText(
            QStringLiteral("%1 instruments watched · %2 tradeable · %3")
                .arg(list_.size())
                .arg(list_.tradeable_count())
                .arg(master_.loaded()
                         ? QStringLiteral("instrument master: %1 contracts")
                               .arg(master_.size())
                         : QStringLiteral("<b style='color:#F85149'>instrument "
                                          "master NOT loaded — every profile "
                                          "column below is blank for that "
                                          "reason, not because the field is "
                                          "empty</b>: %1")
                               .arg(master_.error())));
    }

    Role role_ = Role::None;
    Watchlist list_;
    QSpinBox* token_ = nullptr;
    QLineEdit* symbol_ = nullptr;
    QPushButton* add_ = nullptr;
    QPushButton* remove_ = nullptr;
    QLabel* message_ = nullptr;
    QLabel* summary_ = nullptr;
    QTableWidget* table_ = nullptr;
};

} // namespace altair::ui
