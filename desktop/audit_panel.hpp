// desktop/audit_panel.hpp -- rule 10 on screen.
//
// P11Q-05b.
//
// "EVERY LIVE DECISION IS REPRODUCIBLE FROM {model_hash, feature_version,
// config_hash, spec_version, tick_seqno}."
//
// That is CLAUDE.md's tenth hard rule and until now it existed only as a
// sentence. The machinery is real -- `models/registry.hpp` hashes weights AND
// architecture AND feature version, `core/config/config.hpp` carries a content
// hash, `features/registry.hpp` produces the feature version -- but nothing put
// the five together where a person could look at them.
//
// SO THIS PAGE ANSWERS ONE QUESTION: IF A TRADE HAPPENED RIGHT NOW, COULD YOU
// REBUILD THE DECISION THAT CAUSED IT?
//
// Today the answer is no, and the useful part is WHICH of the five is missing
// and why. Three of them can be computed from what is on disk this second, and
// they ARE computed here rather than described:
//
//   config_hash    FNV-1a over the bytes of every file in config/. Changes if
//                  a rate or a limit changes, which is the whole point of it.
//   spec_version   the same over the instrument master, which is what the
//                  spec store is built from.
//   tick_seqno     live from the replayer, moving while you watch.
//
// The other two have no value because the thing that produces them is not
// running: there is no trained artefact anywhere in the tree, so no model_hash,
// and no populated FeatureRegistry in this process, so no feature_version.
// Both say so rather than showing a zero -- a zero hash is indistinguishable
// from a real one at a glance, and `ModelKey::valid()` refuses zero for exactly
// that reason.
//
// A HASH THAT NEVER CHANGES IS NOT AN AUDIT TRAIL, SO CHANGE ONE AND WATCH.
//
// The point of showing these live is that editing `config/charges.toml` and
// reopening this page gives a different config_hash. That is the property rule
// 10 depends on, and it is checkable in ten seconds rather than taken on
// trust.

#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>

namespace altair::ui {

/// FNV-1a over a file's bytes, or 0 if it cannot be read.
///
/// The SAME function core/config/config.hpp uses for its content hash
/// (kFnvOffset / kFnvPrime there), reproduced over bytes rather than a C
/// string. Deliberately not a cryptographic digest: this detects CHANGE, not
/// tampering, and a 64-bit value fits the audit tuple the engine already
/// carries. If it ever needs to resist an adversary, that is a different
/// field and should be named differently.
[[nodiscard]] inline std::uint64_t fnv1a_file(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return 0;
    }
    std::uint64_t h = 1469598103934665603ULL;
    const QByteArray data = f.readAll();
    for (const char c : data) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    return h;
}

/// Combined hash over every file in a directory, in NAME order.
///
/// Name order, not directory order: the filesystem is free to enumerate in any
/// order it likes and a hash that depends on that is a hash that changes when
/// nothing did. The file NAME is folded in too, so renaming a file changes the
/// result -- otherwise swapping two configs' names would go undetected.
[[nodiscard]] inline std::uint64_t fnv1a_dir(const QString& dir,
                                             const QStringList& filters,
                                             int& files_seen) {
    QDir d(dir);
    const QFileInfoList list =
        d.entryInfoList(filters, QDir::Files, QDir::Name);
    files_seen = static_cast<int>(list.size());
    if (list.isEmpty()) {
        return 0;
    }
    std::uint64_t h = 1469598103934665603ULL;
    for (const QFileInfo& fi : list) {
        for (const QChar c : fi.fileName()) {
            h ^= static_cast<std::uint64_t>(c.unicode());
            h *= 1099511628211ULL;
        }
        const std::uint64_t fh = fnv1a_file(fi.absoluteFilePath());
        h ^= fh;
        h *= 1099511628211ULL;
    }
    return h;
}

class AuditPanel final : public QWidget {
    Q_OBJECT

public:
    explicit AuditPanel(QWidget* parent = nullptr) : QWidget(parent) {
        auto* v = new QVBoxLayout(this);

        v->addWidget(new QLabel(
            QStringLiteral("<h3>Audit trail — could you rebuild this "
                           "decision?</h3>"),
            this));

        auto* note = new QLabel(
            QStringLiteral(
                "Rule 10: <i>every live decision is reproducible from "
                "{model_hash, feature_version, config_hash, spec_version, "
                "tick_seqno}</i>. Three of those are computed below from what "
                "is on disk right now. Two have no value because the thing "
                "that produces them is not running, and they say so rather "
                "than showing a zero — a zero hash looks like a real one at a "
                "glance, which is why <code>ModelKey::valid()</code> refuses "
                "it."),
            this);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(note);

        table_ = new QTableWidget(0, 4, this);
        table_->setHorizontalHeaderLabels({QStringLiteral("Field"),
                                           QStringLiteral("Value now"),
                                           QStringLiteral("Comes from"),
                                           QStringLiteral("Status")});
        table_->verticalHeader()->setVisible(false);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setAlternatingRowColors(true);
        table_->horizontalHeader()->setStretchLastSection(true);
        v->addWidget(table_);

        decisions_ = new QLabel(this);
        decisions_->setWordWrap(true);
        v->addWidget(decisions_);

        auto* how = new QLabel(
            QStringLiteral(
                "<b>Check it yourself:</b> edit <code>config/charges.toml</code>"
                " — change a rate, or even add a comment — and reopen this "
                "page. <code>config_hash</code> will be different. A hash that "
                "never moves is not an audit trail, and this is the ten-second "
                "version of proving it does."),
            this);
        how->setWordWrap(true);
        how->setStyleSheet(QStringLiteral("color:#7F8C8D;"));
        v->addWidget(how);
        v->addStretch();

        refresh();
        // The seqno is the only field that moves on its own, so the table is
        // rebuilt on a timer rather than once. Slow on purpose: this is a
        // reference page, not a tape.
        auto* t = new QTimer(this);
        t->setInterval(500);
        connect(t, &QTimer::timeout, this, [this] { refresh(); });
        t->start();
    }

    /// The replayer's position, pushed in by the shell. The UI does not own a
    /// clock and does not read one -- rule 7 says time comes off the tick.
    void set_tick_seqno(std::uint64_t n) { seqno_ = n; }

private:
    void put(int row, int col, const QString& text, const QColor& fg) {
        auto* item = new QTableWidgetItem(text);
        if (fg.isValid()) { item->setForeground(fg); }
        table_->setItem(row, col, item);
    }

    void refresh() {
        const QColor green(0x1B, 0x8A, 0x4B);
        const QColor grey(0x7F, 0x8C, 0x8D);
        const QColor amber(0xB9, 0x77, 0x0B);

        const QString src = QStringLiteral(ALTAIR_SOURCE_DIR);
        int cfg_files = 0;
        const std::uint64_t cfg =
            fnv1a_dir(src + QStringLiteral("/config"),
                      {QStringLiteral("*.toml")}, cfg_files);

        const QString master = src + QStringLiteral("/data/instruments.csv");
        const bool has_master = QFileInfo::exists(master);
        const std::uint64_t spec = has_master ? fnv1a_file(master) : 0;

        table_->setRowCount(5);
        int r = 0;

        put(r, 0, QStringLiteral("model_hash"), QColor());
        put(r, 1, QStringLiteral("— none —"), amber);
        put(r, 2, QStringLiteral("models/registry.hpp — weights + architecture "
                                 "+ feature version"), grey);
        put(r, 3, QStringLiteral("NO ARTEFACT: there is no .pt or .onnx "
                                 "anywhere in the tree"), amber);
        ++r;

        put(r, 0, QStringLiteral("feature_version"), QColor());
        put(r, 1, QStringLiteral("— none —"), amber);
        put(r, 2, QStringLiteral("features/registry.hpp — hash of every "
                                 "registered feature's name and band"), grey);
        put(r, 3, QStringLiteral("NO REGISTRY in this process; the UI does not "
                                 "build one"), amber);
        ++r;

        put(r, 0, QStringLiteral("config_hash"), QColor());
        put(r, 1, QStringLiteral("0x%1").arg(cfg, 16, 16, QLatin1Char('0')),
            green);
        put(r, 2, QStringLiteral("FNV-1a over %1 file(s) in config/, in NAME "
                                 "order, names folded in").arg(cfg_files),
            grey);
        put(r, 3, cfg != 0 ? QStringLiteral("LIVE")
                           : QStringLiteral("config/ is empty or unreadable"),
            cfg != 0 ? green : amber);
        ++r;

        put(r, 0, QStringLiteral("spec_version"), QColor());
        put(r, 1, has_master
                      ? QStringLiteral("0x%1").arg(spec, 16, 16,
                                                   QLatin1Char('0'))
                      : QStringLiteral("— none —"),
            has_master ? green : amber);
        put(r, 2, QStringLiteral("FNV-1a over data/instruments.csv, which the "
                                 "spec store is built from"), grey);
        put(r, 3, has_master
                      ? QStringLiteral("LIVE")
                      : QStringLiteral("no instrument master; fetch it with "
                                       "altair_kite_fetch --dump-instruments"),
            has_master ? green : amber);
        ++r;

        put(r, 0, QStringLiteral("tick_seqno"), QColor());
        put(r, 1, QString::number(seqno_), green);
        put(r, 2, QStringLiteral("the replayer's position. Rule 7: read off "
                                 "the TICK, never a wall clock"), grey);
        put(r, 3, QStringLiteral("LIVE — moving while you watch"), green);

        table_->resizeColumnsToContents();

        // THE COUNT IS ZERO AND THAT IS THE HONEST NUMBER.
        //
        // A decisions table with placeholder rows would be indistinguishable
        // from a real one, which is rule 9 and is the reason every blocked
        // page in this window names its blocker instead of drawing plausible
        // nothing.
        decisions_->setText(QStringLiteral(
            "<b>0 decisions recorded.</b> Nothing has traded: "
            "<code>oms/</code> builds an order body and does not send it, and "
            "the Broker Wiring page says the same. When that changes, each "
            "decision is one row carrying all five fields above — and two of "
            "them have to stop reading <i>none</i> first, or the row would be "
            "a record of something that cannot be rebuilt."));
    }

    QTableWidget* table_ = nullptr;
    QLabel* decisions_ = nullptr;
    std::uint64_t seqno_ = 0;
};

} // namespace altair::ui
