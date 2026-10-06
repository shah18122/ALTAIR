// desktop/scrip_master.hpp -- the instrument master the Terminal adds scrips
// from, and the GETS-style loader that adds them.
//
// GETS adds a scrip by narrowing, not by typing a contract name. Here it is a
// row of dropdowns above the watch:
//   Exchange (NSE, BSE) -> Segment (E = equity, FO = futures and options)
//   -> Symbol -> Expiry -> Type (FUT, CE, PE; FUT unless you pick an option)
//   -> Strike (options only).
// Each choice lists only what exists under the ones before it, read from
// data/instruments.csv (the Kite master: NSE/BSE equity, NFO and BFO), and
// the bar shows the one contract they name -- its trading symbol, lot, tick
// and token -- before Add (or Enter) puts it in the watch.
#pragma once

#include <QComboBox>
#include <QDate>
#include <QCompleter>
#include <QAbstractItemView>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QStandardItemModel>
#include <QFile>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace altair::ui {

/// One row of the instrument master.
struct MasterScrip {
    quint32 token = 0;
    QString symbol, exchange, segment, display;
    QString name;              ///< the underlying (F&O) or the company (equity)
    QString type;              ///< EQ, FUT, CE, PE
    QString expiry;            ///< YYYY-MM-DD; empty for equity
    double strike = 0.0;
    qint64 lot = 1;
    double tick = 0.05;
};

/// Index underlyings: FUTIDX / OPTIDX rather than FUTSTK / OPTSTK.
[[nodiscard]] inline bool master_index_name(const QString& n) {
    return n == QLatin1String("NIFTY") || n == QLatin1String("BANKNIFTY") || n == QLatin1String("FINNIFTY")
        || n == QLatin1String("MIDCPNIFTY") || n == QLatin1String("NIFTYNXT50");
}

/// The NSE and BSE equities and the NSE and BSE F&O contracts of data/instruments.csv
/// (the Kite master). Read on first use: it is ~9 MB.
[[nodiscard]] inline std::vector<MasterScrip> load_master_scrips(const QString& path) {
    std::vector<MasterScrip> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    QTextStream in(&f);
    const auto split = [](const QString& line) {
        QStringList fields;
        QString cur;
        bool quoted = false;
        for (const QChar c : line) {
            if (c == QLatin1Char('"')) { quoted = !quoted; continue; }
            if (c == QLatin1Char(',') && !quoted) { fields << cur; cur.clear(); continue; }
            cur += c;
        }
        fields << cur;
        return fields;
    };
    const QStringList head = split(in.readLine());
    const auto col = [&head](const char* n) { return static_cast<int>(head.indexOf(QString::fromLatin1(n))); };
    const int c_tok = col("instrument_token"), c_sym = col("tradingsymbol"), c_name = col("name"), c_exp = col("expiry"),
              c_type = col("instrument_type"), c_seg = col("segment"), c_ex = col("exchange"), c_strike = col("strike"),
              c_lot = col("lot_size"), c_tick = col("tick_size");
    if (c_tok < 0 || c_sym < 0 || c_seg < 0 || c_ex < 0 || c_type < 0) return out;
    const int need = std::max({c_tok, c_sym, c_seg, c_ex, c_type, c_name, c_exp, c_strike, c_lot, c_tick}) + 1;
    out.reserve(120000);
    while (!in.atEnd()) {
        const QStringList c = split(in.readLine());
        if (c.size() < need) continue;
        const QString ex = c[c_ex], seg = c[c_seg], type = c[c_type];
        const bool eq = (ex == QLatin1String("NSE") || ex == QLatin1String("BSE")) && seg == ex && type == QLatin1String("EQ");
        const bool fo = (ex == QLatin1String("NFO") && (seg == QLatin1String("NFO-FUT") || seg == QLatin1String("NFO-OPT")))
                     || (ex == QLatin1String("BFO") && (seg == QLatin1String("BFO-FUT") || seg == QLatin1String("BFO-OPT")));
        if (!eq && !fo) continue;
        MasterScrip m;
        m.token = c[c_tok].toUInt();
        if (m.token == 0) continue;
        m.symbol = c[c_sym];
        m.exchange = ex;
        m.segment = seg;
        m.name = c_name >= 0 ? c[c_name] : QString();
        m.type = seg.endsWith(QLatin1String("-FUT")) ? QStringLiteral("FUT") : type;
        m.expiry = c_exp >= 0 ? c[c_exp] : QString();
        m.strike = c_strike >= 0 ? c[c_strike].toDouble() : 0.0;
        m.lot = c_lot >= 0 ? std::max<qint64>(1, c[c_lot].toLongLong()) : 1;
        m.tick = c_tick >= 0 && c[c_tick].toDouble() > 0.0 ? c[c_tick].toDouble() : 0.05;
        m.display = eq ? QStringLiteral("%1  ·  %2 EQ  ·  %3").arg(m.symbol, ex, m.name)
                       : QStringLiteral("%1  ·  %2  ·  %3").arg(m.symbol, m.type, m.expiry);
        out.push_back(std::move(m));
    }
    return out;
}

/// Which master exchange an Exchange + Segment choice means.
[[nodiscard]] inline QString master_exchange(const QString& exchange, const QString& segment) {
    if (segment == QLatin1String("FO")) return exchange == QLatin1String("BSE") ? QStringLiteral("BFO") : QStringLiteral("NFO");
    return exchange;
}

/// The GETS loader: one row of dropdowns over a master.
class AddScripBar final : public QWidget {
public:
    /// Add pressed (or Enter) on a resolved contract.
    std::function<void(quint32)> on_add;

    explicit AddScripBar(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("addScripBar"));
        auto* h = new QHBoxLayout(this);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        const auto combo = [this, h](const char* name, const char* tip, int width, bool editable = false) {
            auto* c = new QComboBox(this);
            c->setObjectName(QString::fromLatin1(name));
            c->setToolTip(QString::fromLatin1(tip));
            if (width > 0) c->setMinimumWidth(width);
            if (editable) {
                c->setEditable(true);
                c->setInsertPolicy(QComboBox::NoInsert);
            }
            h->addWidget(c);
            return c;
        };
        exchange_ = combo("addExchange", "Exchange", 64);
        exchange_->addItems({QStringLiteral("NSE"), QStringLiteral("BSE")});
        segment_ = combo("addSegment", "Segment: E = equity, FO = futures and options", 52);
        segment_->addItems({QStringLiteral("E"), QStringLiteral("FO")});
        symbol_ = combo("addSymbol", "Symbol (type to search)", 150, true);
        expiry_ = combo("addExpiry", "Expiry", 104);
        type_ = combo("addType", "FUT unless you choose an option", 60);
        type_->addItems({QStringLiteral("FUT"), QStringLiteral("CE"), QStringLiteral("PE")});
        strike_ = combo("addStrike", "Strike (options)", 84, true);
        add_ = new QPushButton(QStringLiteral("Add"), this);
        add_->setObjectName(QStringLiteral("addScripButton"));
        add_->setToolTip(QStringLiteral("Add the contract to the watch (Enter)"));
        h->addWidget(add_);
        resolved_ = new QLabel(this);
        resolved_->setObjectName(QStringLiteral("addResolved"));
        resolved_->setTextFormat(Qt::RichText);
        h->addWidget(resolved_, 1);

        connect(exchange_, &QComboBox::currentIndexChanged, this, [this](int) { fill_symbols(); });
        connect(segment_, &QComboBox::currentIndexChanged, this, [this](int) { fill_symbols(); });
        connect(symbol_, &QComboBox::currentTextChanged, this, [this](const QString&) { fill_types(); });
        connect(type_, &QComboBox::currentIndexChanged, this, [this](int) { fill_expiries(); });
        connect(expiry_, &QComboBox::currentIndexChanged, this, [this](int) { fill_strikes(); });
        connect(strike_, &QComboBox::currentTextChanged, this, [this](const QString&) { resolve(); });
        connect(add_, &QPushButton::clicked, this, [this] { fire(); });
        for (QComboBox* c : {exchange_, segment_, symbol_, expiry_, type_, strike_}) c->installEventFilter(this);
        fill_symbols();
    }

    /// The master to choose from (read once by the watch).
    void set_master(const std::vector<MasterScrip>* master) { master_ = master; fill_symbols(); }
    /// Expiries before this day (YYYY-MM-DD) are not offered; today by default.
    void set_today(const QString& iso) { today_ = iso; fill_expiries(); }

    /// The contract the choices name; 0 while they name none.
    [[nodiscard]] quint32 token() const noexcept { return token_; }
    [[nodiscard]] QString resolved_text() const { return resolved_->text(); }
    void focus() { symbol_->setFocus(); symbol_->lineEdit()->selectAll(); }

    /// Drive the choices without a keyboard (tests; a pre-filled bar). An
    /// empty `type` leaves the default (FUT, or CE where there is no future).
    void choose(const QString& exchange, const QString& segment, const QString& symbol, const QString& expiry = {},
                const QString& type = {}, const QString& strike = {}) {
        exchange_->setCurrentText(exchange);
        segment_->setCurrentText(segment);
        symbol_->setCurrentText(symbol);
        if (!type.isEmpty()) type_->setCurrentText(type);
        if (!expiry.isEmpty()) expiry_->setCurrentText(expiry);
        if (!strike.isEmpty()) strike_->setCurrentText(strike);
        resolve();
    }
    [[nodiscard]] static QStringList items(const QComboBox* c) {
        QStringList out;
        for (int i = 0; i < c->count(); ++i) out << c->itemText(i);
        return out;
    }
    [[nodiscard]] QStringList symbols() const { return items(symbol_); }
    [[nodiscard]] QStringList expiries() const { return items(expiry_); }
    [[nodiscard]] QStringList strikes() const { return items(strike_); }
    [[nodiscard]] QString type() const { return type_->currentText(); }
    [[nodiscard]] bool type_enabled(const QString& t) const {
        const int i = type_->findText(t);
        auto* m = qobject_cast<const QStandardItemModel*>(type_->model());
        return i >= 0 && type_->isEnabled() && (m == nullptr || m->item(i)->isEnabled());
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        if (e->type() == QEvent::KeyPress) {
            const auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
                auto* c = qobject_cast<QComboBox*>(obj);
                if (c != nullptr && c->view() != nullptr && c->view()->isVisible()) return false;   // choosing in a list
                fire();
                return true;
            }
        }
        return QWidget::eventFilter(obj, e);
    }

private:
    [[nodiscard]] static QString strike_text(double s) {
        return std::fabs(s - std::round(s)) < 1e-9 ? QString::number(static_cast<qint64>(std::llround(s)))
                                                    : QString::number(s, 'f', 2);
    }
    [[nodiscard]] bool fo() const { return segment_->currentText() == QLatin1String("FO"); }
    [[nodiscard]] QString mex() const { return master_exchange(exchange_->currentText(), segment_->currentText()); }
    [[nodiscard]] bool in_segment(const MasterScrip& m) const {
        if (m.exchange != mex()) return false;
        return fo() ? m.type != QLatin1String("EQ") : m.type == QLatin1String("EQ");
    }
    [[nodiscard]] QString key_of(const MasterScrip& m) const { return fo() ? m.name : m.symbol; }
    [[nodiscard]] QString sym() const { return symbol_->currentText().trimmed().toUpper(); }

    void fire() {
        resolve();
        if (token_ != 0 && on_add) on_add(token_);
    }
    void fill_symbols() {
        std::set<QString> names;
        if (master_ != nullptr)
            for (const auto& m : *master_) if (in_segment(m) && (m.expiry.isEmpty() || m.expiry >= today_)) names.insert(key_of(m));
        QStringList list(names.begin(), names.end());
        const QString keep = symbol_->currentText();
        {
            const QSignalBlocker b(symbol_);
            symbol_->clear();
            symbol_->addItems(list);
            auto* c = new QCompleter(list, symbol_);
            c->setCaseSensitivity(Qt::CaseInsensitive);
            c->setFilterMode(Qt::MatchStartsWith);
            symbol_->setCompleter(c);
            const QString fallback = fo() ? (list.contains(QStringLiteral("NIFTY")) ? QStringLiteral("NIFTY")
                                             : list.contains(QStringLiteral("SENSEX")) ? QStringLiteral("SENSEX") : QString())
                                          : QString();
            symbol_->setCurrentText(list.contains(keep) ? keep : !fallback.isEmpty() ? fallback : list.isEmpty() ? QString() : list.front());
        }
        fill_types();
    }
    void fill_types() {
        // FUT is the default; where the symbol has no future (BSE F&O lists
        // options only) FUT is greyed out and CE is chosen.
        bool has_fut = false, has_opt = false;
        if (master_ != nullptr && fo())
            for (const auto& m : *master_) {
                if (!in_segment(m) || key_of(m) != sym() || m.expiry < today_) continue;
                (m.type == QLatin1String("FUT") ? has_fut : has_opt) = true;
            }
        {
            const QSignalBlocker b(type_);
            type_->setEnabled(fo());
            if (auto* model = qobject_cast<QStandardItemModel*>(type_->model())) {
                model->item(0)->setEnabled(has_fut || !fo());
                model->item(1)->setEnabled(has_opt);
                model->item(2)->setEnabled(has_opt);
            }
            if (!fo()) type_->setCurrentIndex(0);
            else if (!has_fut && has_opt && type_->currentIndex() == 0) type_->setCurrentIndex(1);
            else if (has_fut && !has_opt) type_->setCurrentIndex(0);
        }
        fill_expiries();
    }
    void fill_expiries() {
        std::set<QString> ex;
        const QString t = type_->currentText();
        // A master a few days old still lists contracts that have expired: not offered.
        if (master_ != nullptr && fo())
            for (const auto& m : *master_)
                if (in_segment(m) && key_of(m) == sym() && !m.expiry.isEmpty() && m.expiry >= today_
                    && (t == QLatin1String("FUT") ? m.type == QLatin1String("FUT") : m.type != QLatin1String("FUT")))
                    ex.insert(m.expiry);
        const QString keep = expiry_->currentText();
        {
            const QSignalBlocker b(expiry_);
            expiry_->clear();
            for (const auto& e : ex) expiry_->addItem(e);   // ISO dates sort nearest first
            if (ex.count(keep) != 0) expiry_->setCurrentText(keep);
            expiry_->setEnabled(fo());
        }
        fill_strikes();
    }
    void fill_strikes() {
        std::set<double> st;
        const bool opt = fo() && type_->currentText() != QLatin1String("FUT");
        if (master_ != nullptr && opt)
            for (const auto& m : *master_)
                if (in_segment(m) && key_of(m) == sym() && m.expiry == expiry_->currentText() && m.type == type_->currentText())
                    st.insert(m.strike);
        const QString keep = strike_->currentText();
        {
            const QSignalBlocker b(strike_);
            strike_->clear();
            for (const double s : st) strike_->addItem(strike_text(s));
            if (strike_->findText(keep) >= 0) strike_->setCurrentText(keep);
            else if (!st.empty()) strike_->setCurrentIndex(static_cast<int>(st.size() / 2));   // the middle of the chain
            strike_->setEnabled(opt);
        }
        resolve();
    }
    void resolve() {
        token_ = 0;
        const bool opt = fo() && type_->currentText() != QLatin1String("FUT");
        if (master_ != nullptr)
            for (const auto& m : *master_) {
                if (!in_segment(m) || key_of(m) != sym()) continue;
                if (fo() && (m.expiry != expiry_->currentText() || m.type != type_->currentText())) continue;
                if (opt && strike_text(m.strike) != strike_->currentText()) continue;
                token_ = m.token;
                resolved_->setText(QStringLiteral("<b>%1</b> <span style='color:#8B949E'>%2 · lot %3 · tick %4</span>")
                                       .arg(m.symbol.toHtmlEscaped(), m.exchange)
                                       .arg(m.lot)
                                       .arg(QString::number(m.tick, 'f', 2)));
                add_->setEnabled(true);
                return;
            }
        resolved_->setText(master_ == nullptr || master_->empty()
                               ? QStringLiteral("<span style='color:#E3B341'>No instrument master (data/instruments.csv).</span>")
                               : QStringLiteral("<span style='color:#E3B341'>No such contract.</span>"));
        add_->setEnabled(false);
    }

    const std::vector<MasterScrip>* master_ = nullptr;
    QComboBox* exchange_ = nullptr;
    QComboBox* segment_ = nullptr;
    QComboBox* symbol_ = nullptr;
    QComboBox* expiry_ = nullptr;
    QComboBox* type_ = nullptr;
    QComboBox* strike_ = nullptr;
    QLabel* resolved_ = nullptr;
    QPushButton* add_ = nullptr;
    quint32 token_ = 0;
    QString today_ = QDate::currentDate().toString(Qt::ISODate);
};

} // namespace altair::ui
