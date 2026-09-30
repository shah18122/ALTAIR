// desktop/gets_workspace.hpp -- the GETS-style Terminal.
//
// Greeksoft GETS is the terminal Indian options desks run: a Greek market
// watch, portfolio Greeks, a what-if simulator, expense and margin reports,
// trade history, RMS, top movers and index information, all around one
// account. This is Altair's version of that surface, built on Altair's own
// engine (risk/option_book.hpp, risk/cost.hpp) and FYERS read-only data.
//
// DATA. Two read-only helpers write files and this widget reads them:
//   altair_fyers_account  -> data/fyers_account.json (funds, positions,
//                            orders, trade book)
//   altair_fyers_quotes   -> data/fyers_quotes.json  (LTP, change, OHLC)
// "Refresh FYERS" runs both, account first, so the quotes request can include
// every open position and its underlying. Nothing here opens a socket or
// reads a credential, and nothing here can place an order.
//
// NOT LIVE TICKS. Each refresh is a snapshot and every tab says how old it
// is. Auto-refresh (60 s) re-runs the helpers while the Terminal is open.
#pragma once

#include "gets_data.hpp"
#include "gets_tables.hpp"
#include "helper_process.hpp"

#if ALTAIR_HAVE_CHARGES_TOML
#include <risk/charges_toml.hpp>
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QShowEvent>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableView>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace altair::ui {

class GetsWorkspace final : public QWidget {
public:
    /// `account_page` is the Terminal's existing positions-and-funds surface;
    /// it becomes the first tab, unchanged.
    explicit GetsWorkspace(QWidget* account_page, QWidget* parent = nullptr)
        : QWidget(parent) {
        setObjectName(QStringLiteral("getsWorkspace"));
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        // ---- toolbar -------------------------------------------------------
        auto* bar = new QWidget(this);
        bar->setObjectName(QStringLiteral("getsBar"));
        auto* h = new QHBoxLayout(bar);
        h->setContentsMargins(10, 6, 10, 6);
        h->setSpacing(8);
        refresh_ = new QPushButton(QStringLiteral("Refresh FYERS"), bar);
        refresh_->setToolTip(QStringLiteral(
            "Run altair_fyers_account then altair_fyers_quotes (read-only GETs) and reload every tab"));
        quotes_btn_ = new QPushButton(QStringLiteral("Quotes only"), bar);
        quotes_btn_->setToolTip(QStringLiteral("Refresh market quotes without re-reading the account"));
        reread_ = new QPushButton(QStringLiteral("Re-read files"), bar);
        reread_->setToolTip(QStringLiteral("Reload the last snapshots from disk; no network"));
        auto_ = new QCheckBox(QStringLiteral("Auto 60 s"), bar);
        auto_->setToolTip(QStringLiteral("Re-run the helpers every 60 seconds while this Terminal is visible"));
        rate_ = new QDoubleSpinBox(bar);
        rate_->setRange(0.0, 20.0);
        rate_->setDecimals(2);
        rate_->setSingleStep(0.25);
        rate_->setValue(6.50);
        rate_->setSuffix(QStringLiteral(" %"));
        rate_->setToolTip(QStringLiteral("Risk-free rate for IV and Greeks (Black-Scholes on spot); same default as the option chain"));
        status_ = new QLabel(bar);
        status_->setObjectName(QStringLiteral("getsStatus"));
        status_->setTextFormat(Qt::PlainText);
        h->addWidget(refresh_);
        h->addWidget(quotes_btn_);
        h->addWidget(reread_);
        h->addWidget(auto_);
        h->addWidget(new QLabel(QStringLiteral("Rate"), bar));
        h->addWidget(rate_);
        h->addWidget(status_, 1);
        v->addWidget(bar);

        tabs_ = new QTabWidget(this);
        tabs_->setObjectName(QStringLiteral("getsTabs"));
        tabs_->setDocumentMode(true);
        if (account_page != nullptr) tabs_->addTab(account_page, QStringLiteral("Positions & Funds"));
        v->addWidget(tabs_, 1);

        build_watch_tab();
        build_summary_tab();
        build_simulation_tab();
        build_expense_tab();
        build_trades_tab();
        build_rms_tab();
        build_movers_tab();
        build_index_tab();

        paths_default();
        connect(refresh_, &QPushButton::clicked, this, [this] { refresh(true); });
        connect(quotes_btn_, &QPushButton::clicked, this, [this] { refresh(false); });
        connect(reread_, &QPushButton::clicked, this, [this] { reload(); });
        connect(rate_, &QDoubleSpinBox::valueChanged, this, [this](double) { recompute(); });
        timer_.setInterval(60'000);
        connect(&timer_, &QTimer::timeout, this, [this] {
            if (isVisible() && !runner_.active()) refresh(true);
        });
        connect(auto_, &QCheckBox::toggled, this, [this](bool on) {
            if (on) timer_.start(); else timer_.stop();
            if (settings_) settings_->setValue(QStringLiteral("gets/auto"), on);
        });
    }

    // ---- wiring ----------------------------------------------------------------

    /// Called with a typed snapshot after every reload that has one, so the
    /// Terminal's positions and funds tables stay in step with these tabs.
    std::function<void(const GetsTypedAccount&)> on_account;

    /// Persist the watch list, user IVs, RMS limits and auto-refresh here.
    /// Without settings nothing is written (tests, first run).
    void set_settings(QSettings* settings) {
        settings_ = settings;
        if (!settings_) return;
        watch_ = settings_->value(QStringLiteral("gets/watch")).toStringList();
        const auto ivs = settings_->value(QStringLiteral("gets/user_iv")).toMap();
        user_iv_.clear();
        for (auto it = ivs.begin(); it != ivs.end(); ++it) user_iv_.insert(it.key(), it.value().toDouble());
        warn_->setValue(settings_->value(QStringLiteral("gets/rms_warn"), 80.0).toDouble());
        stop_->setValue(settings_->value(QStringLiteral("gets/rms_stop"), 95.0).toDouble());
        max_open_->setValue(settings_->value(QStringLiteral("gets/rms_max_open"), 50).toInt());
        max_loss_->setValue(settings_->value(QStringLiteral("gets/rms_max_loss"), 0).toInt());
        auto_->setChecked(settings_->value(QStringLiteral("gets/auto"), false).toBool());
        recompute();
    }

    void set_paths(QString account, QString quotes, QString universe, QString master) {
        account_path_ = std::move(account);
        quotes_path_ = std::move(quotes);
        universe_path_ = std::move(universe);
        master_path_ = std::move(master);
    }

    /// Fix the valuation clock (tests). Unset: the wall clock at each recompute.
    void set_clock(std::optional<Timestamp> now, int year) {
        fixed_now_ = now;
        fixed_year_ = year;
    }

    /// Re-read both files and the universe, then recompute every tab.
    void reload() {
        loaded_once_ = true;
        load_master();
        account_ = load_gets_account(account_path_);
        quotes_ = load_gets_quotes(quotes_path_);
        universe_ = read_universe(universe_path_);
        recompute();
    }

    /// Feed data directly (tests).
    void load(GetsAccount account, GetsQuotes quotes, QStringList universe) {
        account_ = std::move(account);
        quotes_ = std::move(quotes);
        universe_ = std::move(universe);
        recompute();
    }

    void set_watch(QStringList watch) {
        watch_ = std::move(watch);
        if (settings_) settings_->setValue(QStringLiteral("gets/watch"), watch_);
        recompute();
    }
    void set_user_iv(const QString& symbol, std::optional<double> percent) {
        if (percent && *percent > 0.0) user_iv_.insert(symbol, *percent);
        else user_iv_.remove(symbol);
        if (settings_) {
            QVariantMap m;
            for (auto it = user_iv_.begin(); it != user_iv_.end(); ++it) m.insert(it.key(), it.value());
            settings_->setValue(QStringLiteral("gets/user_iv"), m);
        }
        recompute();
    }

    // ---- read access (tests, main window) ----------------------------------------

    [[nodiscard]] QTabWidget* tabs() const noexcept { return tabs_; }
    [[nodiscard]] GetsTableModel* watch_model() const noexcept { return watch_model_; }
    [[nodiscard]] GetsTableModel* summary_model() const noexcept { return summary_model_; }
    [[nodiscard]] GetsTableModel* simulation_model() const noexcept { return sim_model_; }
    [[nodiscard]] GetsTableModel* expense_model() const noexcept { return expense_model_; }
    [[nodiscard]] GetsTableModel* funds_model() const noexcept { return funds_model_; }
    [[nodiscard]] GetsTableModel* trades_model() const noexcept { return trades_model_; }
    [[nodiscard]] GetsTableModel* rms_model() const noexcept { return rms_model_; }
    [[nodiscard]] GetsTableModel* rejections_model() const noexcept { return reject_model_; }
    [[nodiscard]] GetsTableModel* movers_model() const noexcept { return movers_model_; }
    [[nodiscard]] GetsTableModel* index_model() const noexcept { return index_model_; }
    [[nodiscard]] QString status_text() const { return status_->text(); }
    [[nodiscard]] const GetsBook& book() const noexcept { return book_; }
    [[nodiscard]] QComboBox* movers_kind() const noexcept { return movers_kind_; }
    [[nodiscard]] QComboBox* movers_side() const noexcept { return movers_side_; }

    /// Run the helpers: the account (when `with_account`), then quotes.
    void refresh(bool with_account) {
        if (runner_.active()) { status_->setText(QStringLiteral("a refresh is already running")); return; }
        if (with_account) {
            const QString exe = find_helper(QStringLiteral("altair_fyers_account"));
            if (exe.isEmpty()) { report_missing(QStringLiteral("altair_fyers_account")); return; }
            set_busy(true, QStringLiteral("fetching FYERS account (read-only)..."));
            const auto started = runner_.start(exe, {QStringLiteral("--go"), QStringLiteral("--out"), account_path_},
                                               working_dir(), 60'000, [this](HelperProcessResult r) {
                if (!r.ran_to_completion() || r.exit_code != 0) {
                    set_busy(false, QStringLiteral("account fetch failed: %1").arg(last_line(r)));
                    reload();
                    return;
                }
                account_ = load_gets_account(account_path_);
                run_quotes();
            });
            if (!started) set_busy(false, QStringLiteral("could not start the account helper"));
            return;
        }
        run_quotes();
    }

protected:
    /// The first showing reads the snapshots (and the 8.8 MB instrument
    /// master) so opening the app does not pay for a tab nobody opened.
    void showEvent(QShowEvent* event) override {
        if (!loaded_once_) {
            loaded_once_ = true;
            reload();
        }
        QWidget::showEvent(event);
    }

private:
    // ---- tabs -----------------------------------------------------------------

    static QTableView* make_view(QWidget* parent, QAbstractItemModel* model, const QString& name) {
        auto* proxy = new QSortFilterProxyModel(parent);
        proxy->setSourceModel(model);
        proxy->setSortRole(GetsTableModel::SortRole);
        proxy->setFilterKeyColumn(-1);
        proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        auto* view = new QTableView(parent);
        view->setObjectName(name);
        view->setModel(proxy);
        view->setSortingEnabled(true);
        view->setAlternatingRowColors(true);
        view->setSelectionBehavior(QAbstractItemView::SelectRows);
        view->verticalHeader()->hide();
        view->verticalHeader()->setDefaultSectionSize(24);
        view->horizontalHeader()->setSectionsMovable(true);
        view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        view->horizontalHeader()->setStretchLastSection(true);
        view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        return view;
    }

    static QLabel* note(QWidget* parent, const QString& text) {
        auto* l = new QLabel(text, parent);
        l->setWordWrap(true);
        l->setStyleSheet(QStringLiteral("color:#8B949E;font-size:11px;"));
        return l;
    }

    void build_watch_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        auto* h = new QHBoxLayout;
        profile_ = new QComboBox(page);
        profile_->addItems({QStringLiteral("MAIN profile"), QStringLiteral("Full profile")});
        profile_->setToolTip(QStringLiteral("MAIN is GETS' ten-column compact profile; Full shows every column"));
        add_edit_ = new QLineEdit(page);
        add_edit_->setPlaceholderText(QStringLiteral("Add to watch: NSE:NIFTY26SEP25000CE"));
        add_edit_->setClearButtonEnabled(true);
        auto* add = new QPushButton(QStringLiteral("Add"), page);
        auto* remove = new QPushButton(QStringLiteral("Remove selected"), page);
        filter_ = new QLineEdit(page);
        filter_->setPlaceholderText(QStringLiteral("Filter…"));
        filter_->setClearButtonEnabled(true);
        h->addWidget(profile_);
        h->addWidget(add_edit_, 2);
        h->addWidget(add);
        h->addWidget(remove);
        h->addStretch();
        h->addWidget(filter_, 1);
        v->addLayout(h);
        watch_model_ = new GetsTableModel(gets_watch_columns(), this);
        watch_view_ = make_view(page, watch_model_, QStringLiteral("getsGreekWatch"));
        v->addWidget(watch_view_, 1);
        v->addWidget(note(page, QStringLiteral(
            "Open F&O positions plus your watch list. IV is solved from LTP (Black-Scholes on spot, "
            "rate above); a user IV overrides it for Greeks and theoretical price. Rows without a spot "
            "quote, an expiry or a solvable IV say why in Status and show — rather than a guess.")));
        watch_model_->set_edit([this](const QString& key, int column, const QVariant& value) {
            if (column != GwUserIv) return false;
            const QString t = value.toString().trimmed();
            bool ok = false;
            const double pct = t.toDouble(&ok);
            if (!t.isEmpty() && (!ok || pct <= 0.0 || pct > 500.0)) return false;
            set_user_iv(key, t.isEmpty() ? std::nullopt : std::optional<double>{pct});
            return true;
        });
        const auto apply_profile = [this] {
            const bool full = profile_->currentIndex() == 1;
            const auto cols = watch_model_->columns();
            for (int c = 0; c < cols.size(); ++c) watch_view_->setColumnHidden(c, !full && !cols[c].main_profile);
        };
        connect(profile_, &QComboBox::currentIndexChanged, this, apply_profile);
        apply_profile();
        connect(filter_, &QLineEdit::textChanged, this, [this](const QString& t) {
            static_cast<QSortFilterProxyModel*>(watch_view_->model())->setFilterFixedString(t);
        });
        const auto add_symbol = [this] {
            const QString s = add_edit_->text().trimmed().toUpper();
            const QByteArray u = s.toUtf8();
            if (s.isEmpty()) return;
            if (!instruments::parse_fyers_symbol(std::string_view{u.constData(), static_cast<std::size_t>(u.size())},
                                                 reference_year())) {
                status_->setText(QStringLiteral("not a FYERS ticker: %1").arg(s));
                return;
            }
            if (!watch_.contains(s)) {
                QStringList w = watch_;
                w << s;
                set_watch(w);
            }
            add_edit_->clear();
            status_->setText(QStringLiteral("%1 added; press Quotes only to price it").arg(s));
        };
        connect(add, &QPushButton::clicked, this, add_symbol);
        connect(add_edit_, &QLineEdit::returnPressed, this, add_symbol);
        connect(remove, &QPushButton::clicked, this, [this] {
            QStringList w = watch_;
            for (const QModelIndex& i : watch_view_->selectionModel()->selectedRows())
                w.removeAll(i.data(GetsTableModel::KeyRole).toString());
            if (w != watch_) set_watch(w);
        });
        tabs_->addTab(page, QStringLiteral("Greek Watch"));
    }

    void build_summary_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        summary_model_ = new GetsTableModel(gets_summary_columns(), this);
        v->addWidget(make_view(page, summary_model_, QStringLiteral("getsGreekSummary")), 1);
        v->addWidget(note(page, QStringLiteral(
            "Position Greeks by underlying and expiry, then Σ per underlying. Delta is in underlying "
            "units; Delta neutral is the number of futures lots that would flatten it. A group with a "
            "leg that has no LTP or no Greeks is marked incomplete rather than summed short.")));
        tabs_->addTab(page, QStringLiteral("Greek Summary"));
    }

    void build_simulation_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        auto* h = new QHBoxLayout;
        sim_under_ = new QComboBox(page);
        sim_under_->setMinimumWidth(220);
        sim_range_ = new QDoubleSpinBox(page);
        sim_range_->setRange(0.5, 50.0);
        sim_range_->setValue(5.0);
        sim_range_->setSuffix(QStringLiteral(" % range"));
        sim_step_ = new QDoubleSpinBox(page);
        sim_step_->setRange(0.1, 10.0);
        sim_step_->setValue(1.0);
        sim_step_->setSuffix(QStringLiteral(" % step"));
        sim_vol_ = new QDoubleSpinBox(page);
        sim_vol_->setRange(-50.0, 50.0);
        sim_vol_->setValue(0.0);
        sim_vol_->setSuffix(QStringLiteral(" vol pts"));
        sim_days_ = new QDoubleSpinBox(page);
        sim_days_->setRange(0.0, 365.0);
        sim_days_->setValue(0.0);
        sim_days_->setSuffix(QStringLiteral(" days fwd"));
        h->addWidget(new QLabel(QStringLiteral("Underlying"), page));
        h->addWidget(sim_under_);
        h->addWidget(sim_range_);
        h->addWidget(sim_step_);
        h->addWidget(sim_vol_);
        h->addWidget(sim_days_);
        h->addStretch();
        v->addLayout(h);
        sim_model_ = new GetsTableModel(gets_simulation_columns(), this);
        v->addWidget(make_view(page, sim_model_, QStringLiteral("getsSimulation")), 1);
        v->addWidget(note(page, QStringLiteral(
            "Every leg of the chosen underlying revalued at each spot move, with the IV shift and "
            "days forward applied. Futures and cash move one-for-one with spot. P&L is against current "
            "LTPs; P&L at expiry is the payoff. Model values, rounded to the paisa.")));
        const auto rerun = [this] { run_simulation(); };
        connect(sim_under_, &QComboBox::currentIndexChanged, this, rerun);
        for (auto* s : {sim_range_, sim_step_, sim_vol_, sim_days_})
            connect(s, &QDoubleSpinBox::valueChanged, this, rerun);
        tabs_->addTab(page, QStringLiteral("Simulation"));
    }

    void build_expense_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        auto* split = new QSplitter(Qt::Vertical, page);
        expense_model_ = new GetsTableModel(gets_expense_columns(), this);
        split->addWidget(make_view(split, expense_model_, QStringLiteral("getsExpense")));
        funds_model_ = new GetsTableModel({{QStringLiteral("Funds / margin")}, {QStringLiteral("Equity")},
                                           {QStringLiteral("Commodity")}, {QStringLiteral("FYERS id")}},
                                          this);
        split->addWidget(make_view(split, funds_model_, QStringLiteral("getsFunds")));
        v->addWidget(split, 1);
        expense_note_ = note(page, QString());
        v->addWidget(expense_note_);
        tabs_->addTab(page, QStringLiteral("Expense & Margin"));
    }

    void build_trades_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        auto* filter = new QLineEdit(page);
        filter->setPlaceholderText(QStringLiteral("Filter trades…"));
        filter->setClearButtonEnabled(true);
        v->addWidget(filter);
        trades_model_ = new GetsTableModel(gets_trade_columns(), this);
        auto* view = make_view(page, trades_model_, QStringLiteral("getsTrades"));
        v->addWidget(view, 1);
        trades_note_ = note(page, QString());
        v->addWidget(trades_note_);
        connect(filter, &QLineEdit::textChanged, this, [view](const QString& t) {
            static_cast<QSortFilterProxyModel*>(view->model())->setFilterFixedString(t);
        });
        tabs_->addTab(page, QStringLiteral("Trade History"));
    }

    void build_rms_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        auto* h = new QHBoxLayout;
        warn_ = new QDoubleSpinBox(page);
        warn_->setRange(1.0, 100.0);
        warn_->setValue(80.0);
        warn_->setSuffix(QStringLiteral(" % warn"));
        stop_ = new QDoubleSpinBox(page);
        stop_->setRange(1.0, 100.0);
        stop_->setValue(95.0);
        stop_->setSuffix(QStringLiteral(" % stop"));
        max_open_ = new QSpinBox(page);
        max_open_->setRange(1, 1000);
        max_open_->setValue(50);
        max_open_->setPrefix(QStringLiteral("max open "));
        max_loss_ = new QSpinBox(page);
        max_loss_->setRange(0, 100'000'000);
        max_loss_->setSingleStep(1000);
        max_loss_->setPrefix(QStringLiteral("max loss ₹"));
        max_loss_->setSpecialValueText(QStringLiteral("no loss limit"));
        h->addWidget(new QLabel(QStringLiteral("Thresholds"), page));
        h->addWidget(warn_);
        h->addWidget(stop_);
        h->addWidget(max_open_);
        h->addWidget(max_loss_);
        h->addStretch();
        v->addLayout(h);
        auto* split = new QSplitter(Qt::Vertical, page);
        rms_model_ = new GetsTableModel(gets_rms_columns(), this);
        split->addWidget(make_view(split, rms_model_, QStringLiteral("getsRms")));
        reject_model_ = new GetsTableModel(gets_rejection_columns(), this);
        split->addWidget(make_view(split, reject_model_, QStringLiteral("getsRejections")));
        v->addWidget(split, 1);
        v->addWidget(note(page, QStringLiteral(
            "Read-only risk view of the FYERS account: margin use, open positions, MtoM and rejected "
            "orders with the exchange/broker reason. Thresholds here only colour the view; the engine's "
            "own limits (risk/limits.hpp, config/altair.toml) and the kill switch are unchanged.")));
        const auto store = [this] {
            if (settings_) {
                settings_->setValue(QStringLiteral("gets/rms_warn"), warn_->value());
                settings_->setValue(QStringLiteral("gets/rms_stop"), stop_->value());
                settings_->setValue(QStringLiteral("gets/rms_max_open"), max_open_->value());
                settings_->setValue(QStringLiteral("gets/rms_max_loss"), max_loss_->value());
            }
            fill_rms();
        };
        connect(warn_, &QDoubleSpinBox::valueChanged, this, store);
        connect(stop_, &QDoubleSpinBox::valueChanged, this, store);
        connect(max_open_, &QSpinBox::valueChanged, this, store);
        connect(max_loss_, &QSpinBox::valueChanged, this, store);
        tabs_->addTab(page, QStringLiteral("RMS"));
    }

    void build_movers_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        auto* h = new QHBoxLayout;
        movers_kind_ = new QComboBox(page);
        movers_kind_->addItems({QStringLiteral("Scrip based"), QStringLiteral("Index based")});
        movers_side_ = new QComboBox(page);
        movers_side_->addItems({QStringLiteral("Top gainers"), QStringLiteral("Top losers")});
        movers_n_ = new QSpinBox(page);
        movers_n_->setRange(1, 100);
        movers_n_->setValue(10);
        movers_n_->setPrefix(QStringLiteral("top "));
        h->addWidget(movers_kind_);
        h->addWidget(movers_side_);
        h->addWidget(movers_n_);
        h->addStretch();
        v->addLayout(h);
        movers_model_ = new GetsTableModel(gets_mover_columns(), this);
        v->addWidget(make_view(page, movers_model_, QStringLiteral("getsMovers")), 1);
        movers_note_ = note(page, QString());
        v->addWidget(movers_note_);
        const auto again = [this] { fill_movers(); };
        connect(movers_kind_, &QComboBox::currentIndexChanged, this, again);
        connect(movers_side_, &QComboBox::currentIndexChanged, this, again);
        connect(movers_n_, &QSpinBox::valueChanged, this, again);
        tabs_->addTab(page, QStringLiteral("Top Movers"));
    }

    void build_index_tab() {
        auto* page = new QWidget(tabs_);
        auto* v = new QVBoxLayout(page);
        v->setContentsMargins(10, 8, 10, 8);
        index_model_ = new GetsTableModel(gets_index_columns(), this);
        v->addWidget(make_view(page, index_model_, QStringLiteral("getsIndices")), 1);
        v->addWidget(note(page, QStringLiteral(
            "Indices listed in config/market_watch.txt. Edit that file to add or remove any FYERS index.")));
        tabs_->addTab(page, QStringLiteral("Index Info"));
    }

    // ---- computing --------------------------------------------------------------

    [[nodiscard]] Timestamp now() const {
        return fixed_now_ ? *fixed_now_ : Timestamp{QDateTime::currentMSecsSinceEpoch() * 1'000'000LL};
    }
    [[nodiscard]] int reference_year() const {
        return fixed_year_ > 0 ? fixed_year_ : QDate::currentDate().year();
    }

    void recompute() {
        BookParams p{};
        p.now = now();
        p.rate = rate_->value() / 100.0;
        const MasterIndex* master = master_.loaded() ? &master_ : nullptr;
        book_ = build_gets_book(account_, quotes_, watch_, master, user_iv_, p, reference_year());

        QStringList keys;
        QVector<GetsRow> watch_rows = gets_watch_rows(book_, &keys);   // fills keys; call first
        watch_model_->set_rows(std::move(watch_rows), keys);
        summary_model_->set_rows(gets_summary_rows(book_, summarise_book(book_.legs, book_.views,
            [this](std::uint32_t u) { return u < book_.spots.size() ? book_.spots[u] : std::nullopt; })));

        const QString chosen = sim_under_->currentText();
        const QSignalBlocker block(sim_under_);
        sim_under_->clear();
        for (std::size_t u = 0; u < book_.underlyings.size(); ++u) {
            bool derivative = false;
            for (const auto& leg : book_.legs)
                derivative = derivative || (leg.underlying == u && leg.kind != BookLegKind::Cash);
            if (derivative && !book_.underlyings[u].isEmpty()) sim_under_->addItem(book_.underlyings[u]);
        }
        const int keep = sim_under_->findText(chosen);
        sim_under_->setCurrentIndex(keep >= 0 ? keep : 0);
        run_simulation();

        fill_expenses();
        trades_model_->set_rows(gets_trade_rows(account_));
        trades_note_->setText(account_.trades_ok
            ? QStringLiteral("%1 trade(s) today from the FYERS trade book.").arg(account_.trades.size())
            : QStringLiteral("No trade book in the snapshot (HTTP %1). Refresh FYERS; older helpers did not fetch it.")
                  .arg(account_.tradebook_status));
        fill_rms();
        fill_movers();
        index_model_->set_rows(gets_index_rows(quotes_, universe_));
        update_status();

        if (on_account && account_.loaded) {
            on_account(gets_typed_account(account_, master, reference_year()));
        }
    }

    void run_simulation() {
        const QString spot_symbol = sim_under_->currentText();
        std::uint32_t u = 0;
        std::optional<Price> spot;
        for (std::size_t i = 0; i < book_.underlyings.size(); ++i)
            if (book_.underlyings[i] == spot_symbol) { u = static_cast<std::uint32_t>(i); spot = book_.spots[i]; }
        if (spot_symbol.isEmpty() || !spot) {
            sim_model_->set_rows({});
            return;
        }
        std::vector<BookShift> shifts;
        const double range = sim_range_->value();
        const double step = sim_step_->value();
        const int n = static_cast<int>(std::floor(range / step + 1e-9));
        for (int i = -n; i <= n; ++i)
            shifts.push_back(BookShift{static_cast<double>(i) * step, sim_vol_->value(), sim_days_->value()});
        BookParams p = book_.params;
        sim_model_->set_rows(gets_simulation_rows(simulate_book(book_.legs, book_.views, u, *spot, p, shifts)));
    }

    void fill_expenses() {
        std::vector<GetsInstrument> instruments;
        const MasterIndex* master = master_.loaded() ? &master_ : nullptr;
        const auto fills = gets_fills(account_, instruments, master, reference_year());
        const auto brokerage = [](const BookFill& f) {
            // FYERS brokerage is commercial and not in charges.toml (see
            // desktop/cost_panel.hpp): Rs 20 per executed order; options flat,
            // intraday/futures the lower of Rs 20 and 0.03%, delivery free.
            BrokerageRule br{};
            if (f.delivery) return br;
            br.flat_per_order = Notional{2'000};
            if (f.segment == Segment::Opt) {
                br.take_lower = false;
            } else {
                br.pct = rate_from(0.0003L);
                br.take_lower = true;
            }
            return br;
        };
        const auto schedule = [this](Timestamp at) -> const ChargeSchedule* {
            return schedules_.empty() ? nullptr : schedule_for(schedules_.data(), schedules_.size(), at);
        };
        expense_model_->set_rows(gets_expense_rows(book_expenses(fills, schedule, brokerage), instruments));
        QVector<GetsRow> funds;
        for (const auto& f : account_.funds) {
            funds << GetsRow{gets_cell::text(f.title), gets_cell::money(f.equity),
                             gets_cell::money(f.commodity), gets_cell::integer(f.id)};
        }
        funds_model_->set_rows(funds);
        expense_note_->setText(QStringLiteral(
            "Expense per order from the FYERS trade book, priced by risk/cost.hpp with %1. Brokerage "
            "assumed: ₹20 per executed order (options flat; intraday and futures the lower of ₹20 and "
            "0.03%; delivery ₹0). Per-contract SPAN/exposure margin is not in FYERS' read-only APIs; the "
            "lower table is the account's fund limits as FYERS reports them.")
            .arg(schedules_.empty() ? QStringLiteral("NO charge schedule loaded in this build (expense shows —)")
                                    : QStringLiteral("config/charges.toml")));
    }

    void fill_rms() {
        GetsRmsLimits lim;
        lim.warn_utilisation_pct = warn_->value();
        lim.stop_utilisation_pct = stop_->value();
        lim.max_open_positions = max_open_->value();
        lim.max_loss_paise = static_cast<std::int64_t>(max_loss_->value()) * 100;
        rms_model_->set_rows(gets_rms_rows(account_, book_, lim));
        reject_model_->set_rows(gets_rejection_rows(account_));
    }

    void fill_movers() {
        int unquoted = 0;
        const bool indices = movers_kind_->currentIndex() == 1;
        movers_model_->set_rows(gets_mover_rows(quotes_, universe_, indices,
                                                movers_side_->currentIndex() == 0, movers_n_->value(), &unquoted));
        movers_note_->setText(QStringLiteral(
            "Ranked by % change from the previous close among the %1 in config/market_watch.txt. %2")
            .arg(indices ? QStringLiteral("indices") : QStringLiteral("stocks"))
            .arg(unquoted > 0 ? QStringLiteral("%1 symbol(s) had no quote and are left out.").arg(unquoted)
                              : QString()));
    }

    void update_status() {
        const qint64 now_s = now().ns_since_epoch() / 1'000'000'000LL;
        QStringList parts;
        if (account_.loaded) {
            parts << QStringLiteral("FYERS %1 · account %2 (%3 s old%4)")
                         .arg(account_.account_id,
                              QDateTime::fromSecsSinceEpoch(account_.fetched_at, QTimeZone(19800)).toString(QStringLiteral("HH:mm:ss")))
                         .arg(now_s - account_.fetched_at)
                         .arg(now_s >= account_.expires_at ? QStringLiteral(", STALE") : QString());
        } else {
            parts << account_.error;
        }
        if (quotes_.loaded) {
            parts << QStringLiteral("quotes %1 (%2 s old, %3 symbols%4)")
                         .arg(QDateTime::fromSecsSinceEpoch(quotes_.fetched_at, QTimeZone(19800)).toString(QStringLiteral("HH:mm:ss")))
                         .arg(now_s - quotes_.fetched_at)
                         .arg(quotes_.by_symbol.size())
                         .arg(quotes_.failed_responses > 0
                                  ? QStringLiteral(", %1 request(s) failed").arg(quotes_.failed_responses)
                                  : QString());
        } else {
            parts << quotes_.error;
        }
        if (!master_.loaded()) parts << QStringLiteral("no instrument master: monthly expiries unknown");
        if (!account_.warnings.isEmpty()) parts << QStringLiteral("%1 row(s) skipped").arg(account_.warnings.size());
        status_->setText(parts.join(QStringLiteral("  ·  ")));
        status_->setToolTip(account_.warnings.join(QLatin1Char('\n')));
    }

    // ---- helpers -----------------------------------------------------------------

    void paths_default() {
#ifdef ALTAIR_FYERS_ACCOUNT_FILE
        account_path_ = QStringLiteral(ALTAIR_FYERS_ACCOUNT_FILE);
#else
        account_path_ = QStringLiteral("data/fyers_account.json");
#endif
#ifdef ALTAIR_SOURCE_DIR
        quotes_path_ = QStringLiteral(ALTAIR_SOURCE_DIR "/data/fyers_quotes.json");
        universe_path_ = QStringLiteral(ALTAIR_SOURCE_DIR "/config/market_watch.txt");
        master_path_ = QStringLiteral(ALTAIR_SOURCE_DIR "/data/instruments.csv");
#else
        quotes_path_ = QStringLiteral("data/fyers_quotes.json");
        universe_path_ = QStringLiteral("config/market_watch.txt");
        master_path_ = QStringLiteral("data/instruments.csv");
#endif
#if ALTAIR_HAVE_CHARGES_TOML
        (void)load_charges_file(ALTAIR_CHARGES_TOML, schedules_);
#endif
    }

    void load_master() {
        if (!master_path_.isEmpty() && QFileInfo::exists(master_path_)) master_.load(master_path_);
    }

    [[nodiscard]] static QStringList read_universe(const QString& path) {
        QStringList out;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine();
            const qsizetype hash = line.indexOf(QLatin1Char('#'));
            if (hash >= 0) line.truncate(hash);
            line = line.trimmed();
            if (!line.isEmpty() && !out.contains(line)) out << line;
        }
        return out;
    }

    [[nodiscard]] static QString find_helper(const QString& name) {
#if defined(_WIN32)
        const QString exe = name + QStringLiteral(".exe");
#else
        const QString exe = name;
#endif
        QStringList candidates;
        candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/../app/") + exe;
        candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/../Helpers/") + exe;
        candidates << QCoreApplication::applicationDirPath() + QStringLiteral("/../../net/app/") + exe;
#ifdef ALTAIR_SOURCE_DIR
        candidates << QStringLiteral(ALTAIR_SOURCE_DIR "/build/net/app/") + exe;
#endif
        for (const QString& c : candidates)
            if (QFileInfo(c).isFile()) return QFileInfo(c).canonicalFilePath();
        return {};
    }

    [[nodiscard]] static QString working_dir() {
#ifdef ALTAIR_SOURCE_DIR
        return QStringLiteral(ALTAIR_SOURCE_DIR);
#else
        return {};
#endif
    }

    [[nodiscard]] static QString last_line(const HelperProcessResult& r) {
        const QString out = r.output.trimmed().section(QLatin1Char('\n'), -1).trimmed();
        return out.isEmpty() ? r.detail : out;
    }

    void report_missing(const QString& helper) {
        status_->setText(QStringLiteral("%1 is not in this build; build the net preset (build.bat net)").arg(helper));
    }

    void set_busy(bool busy, const QString& text) {
        refresh_->setEnabled(!busy);
        quotes_btn_->setEnabled(!busy);
        status_->setText(text);
    }

    void run_quotes() {
        const QString exe = find_helper(QStringLiteral("altair_fyers_quotes"));
        if (exe.isEmpty()) {
            report_missing(QStringLiteral("altair_fyers_quotes"));
            refresh_->setEnabled(true);
            quotes_btn_->setEnabled(true);
            reload();
            return;
        }
        QStringList args{QStringLiteral("--go"), QStringLiteral("--out"), quotes_path_};
        if (QFileInfo::exists(universe_path_)) args << QStringLiteral("--symbols-file") << universe_path_;
        const QStringList needed = gets_quote_symbols(account_, watch_, reference_year());
        for (qsizetype i = 0; i < needed.size(); i += 50)
            args << QStringLiteral("--symbols") << needed.mid(i, 50).join(QLatin1Char(','));
        if (!args.contains(QStringLiteral("--symbols")) && !args.contains(QStringLiteral("--symbols-file"))) {
            set_busy(false, QStringLiteral("nothing to quote: no positions, watch list or config/market_watch.txt"));
            reload();
            return;
        }
        set_busy(true, QStringLiteral("fetching FYERS quotes (read-only)..."));
        const auto started = runner_.start(exe, args, working_dir(), 60'000, [this](HelperProcessResult r) {
            const bool ok = r.ran_to_completion() && r.exit_code == 0;
            reload();
            refresh_->setEnabled(true);
            quotes_btn_->setEnabled(true);
            if (!ok) status_->setText(QStringLiteral("quotes failed: %1  ·  %2").arg(last_line(r), status_->text()));
        });
        if (!started) set_busy(false, QStringLiteral("could not start the quotes helper"));
    }

    // ---- state ---------------------------------------------------------------------

    QTabWidget* tabs_{};
    QPushButton* refresh_{};
    QPushButton* quotes_btn_{};
    QPushButton* reread_{};
    QCheckBox* auto_{};
    QDoubleSpinBox* rate_{};
    QLabel* status_{};

    QComboBox* profile_{};
    QLineEdit* add_edit_{};
    QLineEdit* filter_{};
    QTableView* watch_view_{};
    GetsTableModel* watch_model_{};
    GetsTableModel* summary_model_{};
    QComboBox* sim_under_{};
    QDoubleSpinBox* sim_range_{};
    QDoubleSpinBox* sim_step_{};
    QDoubleSpinBox* sim_vol_{};
    QDoubleSpinBox* sim_days_{};
    GetsTableModel* sim_model_{};
    GetsTableModel* expense_model_{};
    GetsTableModel* funds_model_{};
    QLabel* expense_note_{};
    GetsTableModel* trades_model_{};
    QLabel* trades_note_{};
    QDoubleSpinBox* warn_{};
    QDoubleSpinBox* stop_{};
    QSpinBox* max_open_{};
    QSpinBox* max_loss_{};
    GetsTableModel* rms_model_{};
    GetsTableModel* reject_model_{};
    QComboBox* movers_kind_{};
    QComboBox* movers_side_{};
    QSpinBox* movers_n_{};
    GetsTableModel* movers_model_{};
    QLabel* movers_note_{};
    GetsTableModel* index_model_{};

    QString account_path_;
    QString quotes_path_;
    QString universe_path_;
    QString master_path_;
    QSettings* settings_{};
    std::optional<Timestamp> fixed_now_;
    int fixed_year_{};
    bool loaded_once_{};
    QTimer timer_;

    GetsAccount account_;
    GetsQuotes quotes_;
    QStringList universe_;
    QStringList watch_;
    QHash<QString, double> user_iv_;
    MasterIndex master_;
    std::vector<ChargeSchedule> schedules_;
    GetsBook book_;
    // Destroyed first so a late helper completion cannot reach dead widgets.
    HelperProcess runner_;
};

} // namespace altair::ui
