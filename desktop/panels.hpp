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
#include "model_status.hpp"
#include "watchlist.hpp"

#include <QComboBox>
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

        auto* note = new QLabel(
            QStringLiteral(
                "This is not a toggle between two equal options. Kite has a "
                "parser, a decoder and order <i>translation</i>; it has no "
                "transport and no session. XTS has nothing — P1-05, P2-03 and "
                "P4-06 are all deferred. Neither can carry an order today."),
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
        const bool kite = can_trade(QStringLiteral("Kite"));
        const bool xts = can_trade(QStringLiteral("XTS"));
        verdict->setText(
            QStringLiteral("<b>Can place an order today — Kite: %1 · XTS: %2"
                           "</b>")
                .arg(kite ? QStringLiteral("yes") : QStringLiteral("NO"),
                     xts ? QStringLiteral("yes") : QStringLiteral("NO")));
        verdict->setStyleSheet(
            QStringLiteral("color:%1;padding:6px;")
                .arg((kite || xts) ? QStringLiteral("#1B8A4B")
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

        connect(train_, &QPushButton::clicked, this, [this] {
            // Deliberately does NOT start a run yet. The harness is P8-04 and
            // is tested; wiring a button to it before the walk-forward split
            // is chosen per model would train something on everything, which
            // is the mistake CLAUDE.md's "markets are not ergodic" rule names.
            status_->setText(QStringLiteral(
                "<span style='color:#B9770B'>Not started.</span> Training is "
                "wired to the harness in P11Q-08, which has to choose the "
                "walk-forward split per model first — a run over the whole "
                "history would be exactly the look-ahead P8-13 measured at "
                "15.1% of state labels."));
        });
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
            out += QStringLiteral(
                "\n\nIN-SAMPLE. This says the chain can be estimated and what "
                "it looks like.\nIt says nothing about out-of-sample behaviour, "
                "nothing about magnitude,\nand nothing about survival after "
                "costs. That is P11Q-08 and rule 5.");
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
        v->addLayout(add_row);

        message_ = new QLabel(this);
        message_->setWordWrap(true);
        v->addWidget(message_);

        table_ = fact_table({QStringLiteral("Token"), QStringLiteral("Name"),
                             QStringLiteral("Spec"), QStringLiteral("Lot"),
                             QStringLiteral("Tick"), QStringLiteral("Note")},
                            this);
        v->addWidget(table_, 1);

        summary_ = new QLabel(this);
        v->addWidget(summary_);

        connect(add_, &QPushButton::clicked, this, &WatchlistPanel::on_add);
        connect(remove_, &QPushButton::clicked, this,
                &WatchlistPanel::on_remove);
        refresh();
    }

    [[nodiscard]] const Watchlist& list() const noexcept { return list_; }

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
    void refresh() {
        const auto& rows = list_.rows();
        table_->setRowCount(static_cast<int>(rows.size()));
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const WatchRow& r = rows[static_cast<std::size_t>(i)];
            put(table_, i, 0, QString::number(r.token));
            put(table_, i, 1, r.symbol);
            const QColor c = r.spec == SpecState::Resolved
                               ? QColor(0x1B, 0x8A, 0x4B)
                             : r.spec == SpecState::Blocked
                               ? QColor(0xC0, 0x39, 0x2B)
                               : QColor(0xB9, 0x77, 0x0B);
            put(table_, i, 2, spec_state_label(r.spec), c);
            // A zero lot renders BLANK, not "0". A zero lot size is the bug
            // that silently scaled every P&L number in the predecessor, and
            // showing it as a number is how it gets used as one.
            put(table_, i, 3,
                r.lot_size > 0 ? QString::number(r.lot_size) : QString());
            put(table_, i, 4,
                r.tick_size_paise > 0 ? format_paise(r.tick_size_paise)
                                      : QString());
            put(table_, i, 5, r.note);
        }
        table_->resizeColumnsToContents();
        summary_->setText(
            QStringLiteral("%1 instruments watched · %2 tradeable")
                .arg(list_.size())
                .arg(list_.tradeable_count()));
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
