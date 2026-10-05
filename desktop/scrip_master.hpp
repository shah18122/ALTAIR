// desktop/scrip_master.hpp -- the instrument master the Terminal adds scrips
// from, and the GETS-style Add Scrip window.
//
// GETS / ODIN add a scrip by narrowing, not by typing a contract name:
// Exchange (NSE, BSE, NFO) -> Instrument (EQ; FUTIDX, FUTSTK, OPTIDX, OPTSTK)
// -> Symbol -> Expiry -> Option type (CE/PE) -> Strike. Each choice lists
// only what exists under the ones before it, read from data/instruments.csv
// (the Kite master), and the window shows the one contract they name -- its
// trading symbol, lot, tick and token -- before Add puts it in the watch.
#pragma once

#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
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

/// The NSE and BSE equities and the NSE F&O contracts of data/instruments.csv
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
        const bool fo = ex == QLatin1String("NFO") && (seg == QLatin1String("NFO-FUT") || seg == QLatin1String("NFO-OPT"));
        if (!eq && !fo) continue;
        MasterScrip m;
        m.token = c[c_tok].toUInt();
        if (m.token == 0) continue;
        m.symbol = c[c_sym];
        m.exchange = ex;
        m.segment = seg;
        m.name = c_name >= 0 ? c[c_name] : QString();
        m.type = seg == QLatin1String("NFO-FUT") ? QStringLiteral("FUT") : type;
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

/// The GETS Add Scrip window over a master.
class AddScripDialog final : public QDialog {
public:
    explicit AddScripDialog(const std::vector<MasterScrip>& master, QWidget* parent = nullptr)
        : QDialog(parent), master_(master) {
        setObjectName(QStringLiteral("addScripDialog"));
        setWindowTitle(QStringLiteral("Add scrip"));
        setModal(true);
        setMinimumWidth(720);
        auto* v = new QVBoxLayout(this);
        auto* g = new QGridLayout;
        const auto head = [this, g](int c, const char* t) {
            auto* l = new QLabel(QString::fromLatin1(t), this);
            l->setStyleSheet(QStringLiteral("color:#8B949E;font-size:10px;font-weight:700;"));
            g->addWidget(l, 0, c);
        };
        head(0, "EXCHANGE"); head(1, "INSTRUMENT"); head(2, "SYMBOL"); head(3, "EXPIRY"); head(4, "OPT TYPE"); head(5, "STRIKE");
        exchange_ = new QComboBox(this);
        exchange_->setObjectName(QStringLiteral("addExchange"));
        exchange_->addItems({QStringLiteral("NSE"), QStringLiteral("BSE"), QStringLiteral("NFO")});
        instrument_ = new QComboBox(this);
        instrument_->setObjectName(QStringLiteral("addInstrument"));
        symbol_ = new QComboBox(this);
        symbol_->setObjectName(QStringLiteral("addSymbol"));
        symbol_->setEditable(true);
        symbol_->setInsertPolicy(QComboBox::NoInsert);
        symbol_->setMinimumWidth(170);
        expiry_ = new QComboBox(this);
        expiry_->setObjectName(QStringLiteral("addExpiry"));
        option_ = new QComboBox(this);
        option_->setObjectName(QStringLiteral("addOption"));
        option_->addItems({QStringLiteral("CE"), QStringLiteral("PE")});
        strike_ = new QComboBox(this);
        strike_->setObjectName(QStringLiteral("addStrike"));
        strike_->setEditable(true);
        strike_->setInsertPolicy(QComboBox::NoInsert);
        g->addWidget(exchange_, 1, 0);
        g->addWidget(instrument_, 1, 1);
        g->addWidget(symbol_, 1, 2);
        g->addWidget(expiry_, 1, 3);
        g->addWidget(option_, 1, 4);
        g->addWidget(strike_, 1, 5);
        v->addLayout(g);
        resolved_ = new QLabel(this);
        resolved_->setObjectName(QStringLiteral("addResolved"));
        resolved_->setTextFormat(Qt::RichText);
        v->addWidget(resolved_);
        auto* buttons = new QDialogButtonBox(this);
        add_ = buttons->addButton(QStringLiteral("Add (Enter)"), QDialogButtonBox::AcceptRole);
        add_->setDefault(true);
        buttons->addButton(QStringLiteral("Close (Esc)"), QDialogButtonBox::RejectRole);
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (token_ != 0) accept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        connect(exchange_, &QComboBox::currentIndexChanged, this, [this](int) { fill_instruments(); });
        connect(instrument_, &QComboBox::currentIndexChanged, this, [this](int) { fill_symbols(); });
        connect(symbol_, &QComboBox::currentTextChanged, this, [this](const QString&) { fill_expiries(); });
        connect(expiry_, &QComboBox::currentIndexChanged, this, [this](int) { fill_strikes(); });
        connect(option_, &QComboBox::currentIndexChanged, this, [this](int) { fill_strikes(); });
        connect(strike_, &QComboBox::currentTextChanged, this, [this](const QString&) { resolve(); });
        fill_instruments();
    }

    /// The contract the choices name; 0 while they name none.
    [[nodiscard]] quint32 token() const noexcept { return token_; }
    [[nodiscard]] QString resolved_text() const { return resolved_->text(); }

    /// Drive the choices without a keyboard (tests; a pre-filled window).
    void choose(const QString& exchange, const QString& instrument, const QString& symbol, const QString& expiry = {},
                const QString& option = {}, const QString& strike = {}) {
        exchange_->setCurrentText(exchange);
        instrument_->setCurrentText(instrument);
        symbol_->setCurrentText(symbol);
        if (!expiry.isEmpty()) expiry_->setCurrentText(expiry);
        if (!option.isEmpty()) option_->setCurrentText(option);
        if (!strike.isEmpty()) strike_->setCurrentText(strike);
        resolve();
    }
    [[nodiscard]] QStringList items(const QComboBox* c) const {
        QStringList out;
        for (int i = 0; i < c->count(); ++i) out << c->itemText(i);
        return out;
    }
    [[nodiscard]] QStringList instruments() const { return items(instrument_); }
    [[nodiscard]] QStringList expiries() const { return items(expiry_); }
    [[nodiscard]] QStringList strikes() const { return items(strike_); }

private:
    [[nodiscard]] static QString strike_text(double s) {
        return std::fabs(s - std::round(s)) < 1e-9 ? QString::number(static_cast<qint64>(std::llround(s)))
                                                    : QString::number(s, 'f', 2);
    }
    [[nodiscard]] QString kind() const { return instrument_->currentText(); }
    [[nodiscard]] bool in_kind(const MasterScrip& m) const {
        const QString k = kind();
        if (k == QLatin1String("EQ")) return m.type == QLatin1String("EQ") && m.exchange == exchange_->currentText();
        if (m.exchange != QLatin1String("NFO")) return false;
        const bool idx = master_index_name(m.name);
        if (k == QLatin1String("FUTIDX")) return m.type == QLatin1String("FUT") && idx;
        if (k == QLatin1String("FUTSTK")) return m.type == QLatin1String("FUT") && !idx;
        if (k == QLatin1String("OPTIDX")) return (m.type == QLatin1String("CE") || m.type == QLatin1String("PE")) && idx;
        if (k == QLatin1String("OPTSTK")) return (m.type == QLatin1String("CE") || m.type == QLatin1String("PE")) && !idx;
        return false;
    }
    [[nodiscard]] QString key_of(const MasterScrip& m) const { return kind() == QLatin1String("EQ") ? m.symbol : m.name; }

    void fill_instruments() {
        const QSignalBlocker b(instrument_);
        instrument_->clear();
        if (exchange_->currentText() == QLatin1String("NFO"))
            instrument_->addItems({QStringLiteral("FUTIDX"), QStringLiteral("FUTSTK"), QStringLiteral("OPTIDX"), QStringLiteral("OPTSTK")});
        else
            instrument_->addItem(QStringLiteral("EQ"));
        fill_symbols();
    }
    void fill_symbols() {
        std::set<QString> names;
        for (const auto& m : master_) if (in_kind(m)) names.insert(key_of(m));
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
            symbol_->setCurrentText(list.contains(keep) ? keep : list.isEmpty() ? QString() : list.front());
        }
        const bool fo = exchange_->currentText() == QLatin1String("NFO");
        const bool opt = kind().startsWith(QLatin1String("OPT"));
        expiry_->setEnabled(fo);
        option_->setEnabled(opt);
        strike_->setEnabled(opt);
        fill_expiries();
    }
    void fill_expiries() {
        std::set<QString> ex;
        const QString sym = symbol_->currentText().trimmed().toUpper();
        for (const auto& m : master_) if (in_kind(m) && key_of(m) == sym && !m.expiry.isEmpty()) ex.insert(m.expiry);
        const QString keep = expiry_->currentText();
        {
            const QSignalBlocker b(expiry_);
            expiry_->clear();
            for (const auto& e : ex) expiry_->addItem(e);   // ISO dates sort nearest first
            if (ex.count(keep) != 0) expiry_->setCurrentText(keep);
        }
        fill_strikes();
    }
    void fill_strikes() {
        std::set<double> st;
        const QString sym = symbol_->currentText().trimmed().toUpper();
        if (kind().startsWith(QLatin1String("OPT")))
            for (const auto& m : master_)
                if (in_kind(m) && key_of(m) == sym && m.expiry == expiry_->currentText() && m.type == option_->currentText())
                    st.insert(m.strike);
        const QString keep = strike_->currentText();
        {
            const QSignalBlocker b(strike_);
            strike_->clear();
            for (const double s : st) strike_->addItem(strike_text(s));
            if (strike_->findText(keep) >= 0) strike_->setCurrentText(keep);
            else if (!st.empty()) strike_->setCurrentIndex(static_cast<int>(st.size() / 2));   // the middle of the chain
        }
        resolve();
    }
    void resolve() {
        token_ = 0;
        const QString sym = symbol_->currentText().trimmed().toUpper();
        const bool opt = kind().startsWith(QLatin1String("OPT"));
        const bool fut = kind().startsWith(QLatin1String("FUT"));
        for (const auto& m : master_) {
            if (!in_kind(m) || key_of(m) != sym) continue;
            if ((fut || opt) && m.expiry != expiry_->currentText()) continue;
            if (opt && (m.type != option_->currentText() || strike_text(m.strike) != strike_->currentText())) continue;
            token_ = m.token;
            resolved_->setText(QStringLiteral("<b>%1</b> &nbsp;·&nbsp; %2 &nbsp;·&nbsp; lot %3 &nbsp;·&nbsp; tick %4 &nbsp;·&nbsp; token %5")
                                   .arg(m.symbol.toHtmlEscaped(), m.exchange)
                                   .arg(m.lot)
                                   .arg(QString::number(m.tick, 'f', 2))
                                   .arg(m.token));
            add_->setEnabled(true);
            return;
        }
        resolved_->setText(QStringLiteral("<span style='color:#E3B341'>No contract matches these choices in the instrument master.</span>"));
        add_->setEnabled(false);
    }

    const std::vector<MasterScrip>& master_;
    QComboBox* exchange_ = nullptr;
    QComboBox* instrument_ = nullptr;
    QComboBox* symbol_ = nullptr;
    QComboBox* expiry_ = nullptr;
    QComboBox* option_ = nullptr;
    QComboBox* strike_ = nullptr;
    QLabel* resolved_ = nullptr;
    QPushButton* add_ = nullptr;
    quint32 token_ = 0;
};

} // namespace altair::ui
