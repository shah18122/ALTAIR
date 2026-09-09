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
#include "model_status.hpp"
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
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QApplication>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QWidget>

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

        auto* title = new QLabel(QStringLiteral("<h2>Altair</h2>"), this);
        title->setAlignment(Qt::AlignCenter);
        v->addWidget(title);

        if (dev_credentials_active()) {
            // PERMANENT and undismissable. A warning with an X on it is a
            // warning that gets clicked away on the second day.
            auto* warn = new QLabel(
                QStringLiteral(
                    "<b>DEVELOPMENT BUILD</b><br>Default accounts "
                    "<code>admin/admin</code> and <code>staff/staff</code> are "
                    "compiled in. The <code>prod</code> preset does not define "
                    "ALTAIR_DEV_CREDENTIALS, so this build cannot ship with "
                    "them."),
                this);
            warn->setWordWrap(true);
            warn->setStyleSheet(QStringLiteral(
                "background:#4A3410;color:#F0C674;padding:8px;"
                "border:1px solid #B9770B;"));
            v->addWidget(warn);
        }

        if (users_.empty()) {
            auto* none = new QLabel(
                QStringLiteral(
                    "No accounts are provisioned in this build. There is no "
                    "default account to fall back to."),
                this);
            none->setWordWrap(true);
            none->setStyleSheet(QStringLiteral("color:#C0392B;"));
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
        error_->setStyleSheet(QStringLiteral("color:#C0392B;"));
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
            QStringLiteral("<h3>Broker wiring — what is actually built</h3>"),
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
                "This is not a toggle between two equal options. Kite has a "
                "parser, a decoder, order <i>translation</i>, an HTTPS "
                "transport and %1. <b>XTS is WITHDRAWN</b> — Smit removed it from the plan on 2026-09-04 and Kite is the only venue. <b>Kite still cannot carry an order today</b>: "
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
        // ONE VENUE NOW. XTS was withdrawn 2026-09-04, so asking whether it
        // can trade is asking about something that is not coming -- and a
        // second "NO" on this line reads as a second gap to close.
        const bool kite = can_trade(QStringLiteral("Kite"));
        verdict->setText(
            QStringLiteral("<b>Can place an order today — Kite: %1</b>"
                           "  <span style='color:#7F8C8D'>(Kite is the only "
                           "venue; XTS withdrawn)</span>")
                .arg(kite ? QStringLiteral("yes") : QStringLiteral("NO")));
        verdict->setStyleSheet(
            QStringLiteral("color:%1;padding:6px;")
                .arg(kite ? QStringLiteral("#1B8A4B")
                          : QStringLiteral("#C0392B")));
        v->addWidget(verdict);
    }
};

// ---------------------------------------------------------------------------
// Models
// ---------------------------------------------------------------------------

class ModelPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ModelPanel(Role role, QWidget* parent = nullptr)
        : QWidget(parent) {
        auto* v = new QVBoxLayout(this);
        v->addWidget(new QLabel(
            QStringLiteral("<h3>Models — what each one has, and what is "
                           "missing</h3>"),
            this));

        const auto rows = model_catalogue();
        int trained = 0;
        for (const ModelRow& r : rows) {
            if (r.state == ModelState::TrainedOnRealData) ++trained;
        }

        auto* summary = new QLabel(
            QStringLiteral(
                "<b>%1 of %2 models are fitted on real data.</b> The binding "
                "constraint is <i>data</i>, not compute: <code>dataset/</code> "
                "holds 8,756 daily NIFTY bars, 3,153 sixty-minute, 1,207 "
                "one-minute across four partial days, 527 daily India VIX — "
                "and no tick data at all. LibTorch is also absent, and that is "
                "the smaller problem. Every fit here is IN-SAMPLE: it shows the "
                "model can be estimated, not that it works out of sample.")
                .arg(trained)
                .arg(rows.size()),
            this);
        summary->setWordWrap(true);
        summary->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(summary);

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

        // THE FIT PANE. Selecting a row runs the model, if it has data.
        //
        // The alternative -- a static string per row -- is what the catalogue
        // already is, and a dashboard that only ever shows strings somebody
        // typed cannot tell you when a fit stops working. These numbers come
        // out of the model on the click.
        connect(t, &QTableWidget::currentCellChanged, this,
                [this](int row, int, int, int) { show_fit(row); });
        table_ = t;

        detail_ = new QPlainTextEdit(this);
        detail_->setReadOnly(true);
        detail_->setMinimumHeight(190);
        detail_->setStyleSheet(QStringLiteral(
            "background:#12161A;color:#D6DBDF;font-family:Consolas,monospace;"));
        v->addWidget(detail_);

        auto* controls = new QHBoxLayout;
        train_ = new QPushButton(QStringLiteral("Train selected model"), this);
        // A capability, not a role check written out at the call site.
        train_->setEnabled(may(role, Capability::TrainModel));
        train_->setToolTip(
            may(role, Capability::TrainModel)
                ? QStringLiteral(
                      "Runs the training harness. Only the Markov chain has "
                      "data behind it; every other row would train on nothing.")
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

        status_->setText(QStringLiteral("Running 26 folds over the real "
                                        "series\u2026"));
        QApplication::processEvents();

        const QString root = QStringLiteral(ALTAIR_DATASET_DIR);
        const LoadResult d = load_bars_csv(
            root + QStringLiteral("/spot/nifty/1d/all.csv"),
            24LL * 3600 * 1'000'000'000LL, DailyStamp::SessionClose, true);
        if (!d.ok()) {
            status_->setText(d.error);
            return;
        }
        // 2,000 bars of initial training and 500-bar test blocks, alpha 0.5.
        // Every one is a modelling choice and every one is passed explicitly
        // rather than defaulted inside the model -- see markov_eval.hpp.
        const WalkForwardFit w = walk_forward_markov(d.bars, 5, 2000, 500, 0.5);
        if (!w.ok) {
            status_->setText(w.error);
            return;
        }

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
        detail_->setPlainText(o);

        status_->setText(
            w.no_directional_edge
                ? QStringLiteral(
                      "<b style='color:#C0392B'>No directional edge under "
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
                      "rule 5 has not been applied."));
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
        }
        detail_->setPlainText(out);
    }

    QTableWidget* table_ = nullptr;
    QPlainTextEdit* detail_ = nullptr;
    QPushButton* train_ = nullptr;
    QLabel* status_ = nullptr;
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
        refresh();
    }

    /// The Kite master, loaded ONCE. 108,411 rows and 8.8 MB -- scanning it
    /// per row is a freeze that arrives at about the tenth instrument, late
    /// enough to read as a different bug.
    MasterIndex master_;

    [[nodiscard]] const Watchlist& list() const noexcept { return list_; }

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

private Q_SLOTS:
    void on_add() {
        const AddResult r =
            list_.add(static_cast<std::uint32_t>(token_->value()),
                      symbol_->text());
        message_->setText(add_result_label(r));
        message_->setStyleSheet(
            QStringLiteral("color:%1;")
                .arg(r == AddResult::Added ? QStringLiteral("#1B8A4B")
                                           : QStringLiteral("#C0392B")));
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
                         : QStringLiteral("<b style='color:#C0392B'>instrument "
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
