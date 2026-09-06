// desktop/main.cpp -- the UI entry point.
//
// P11Q-01.
//
// WHAT THIS IS: the first Qt card. A window, a live grid, and a replayer
// driving it. It exists to prove the toolchain and to put the P11-03 identity
// discipline into a real QAbstractTableModel, not to be a dashboard.
//
// WHAT THE DATA IS: SYNTHETIC, GENERATED HERE, AND NOT THE ENGINE'S.
//
// AND THE INSTRUMENTS ARE NAMED SO. An earlier version of this file put real
// tickers -- RELIANCE among them -- on a random walk. There is no Reliance
// data in this repository. Every synthetic instrument below now says SYNTH in
// its name, because a fabricated series wearing a real ticker is the one thing
// a trading window must never show.
//
// `app/session_file.hpp` has the engine's own `generate_session`, and this file
// deliberately does NOT include it. `app/` is the `altair` binary, not a
// library, and reaching into it from the UI would be exactly the kind of
// convenience header-crossing CLAUDE.md's layout rule exists to prevent.
//
// So the walk below is local, obvious, and labelled. It is a stand-in until a
// live or replayed feed is wired into this window, and it must not be mistaken
// for engine behaviour: it is a bounded random walk with a fixed LCG, nothing
// more. CLAUDE.md's reality check applies -- synthetic data exists because
// there is no captured tick data yet, not because it is a substitute for it.

#include <core/time/timestamp.hpp>
#include <QIcon>
#include <QPixmap>
#include <QSplashScreen>
#include <cstdio>
#include <core/types/units.hpp>
#include <feed/replay.hpp>

#include "auth.hpp"
#include "data/real_tape.hpp"
#include "main_window.hpp"
#include "panels.hpp"

#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace {

struct DemoInstrument {
    std::uint32_t token;
    const char* symbol;
    std::int64_t open_paise;
};

/// Four SYNTHETIC instruments, named so they cannot be mistaken for real ones.
///
/// THE FIRST VERSION OF THIS LIST USED REAL TICKERS -- "NIFTY 50",
/// "BANKNIFTY", "RELIANCE", "NIFTY26SEP24500CE" -- against a random walk.
/// There is no Reliance data in this repository and there never was;
/// `dataset/spot/` holds nifty and indiavix and nothing else. A real ticker on
/// a fabricated price, in a window that also displays real data, is
/// indistinguishable from the real thing at a glance. That is the exact
/// failure CLAUDE.md's rule 9 exists to prevent, and it was on screen.
///
/// So the names carry the word SYNTH and the prices are round numbers no
/// instrument trades at. The four still span two orders of magnitude, which is
/// what makes the formatter's Indian digit grouping visible, and they still
/// exercise every sort, filter and chart path -- which was the only thing the
/// real names were ever buying.
constexpr std::array<DemoInstrument, 4> kInstruments{{
    {900001, "SYNTH-A (index-scale)",   2'000'000},  // Rs 20,000.00
    {900002, "SYNTH-B (index-scale)",   5'000'000},  // Rs 50,000.00
    {900003, "SYNTH-C (stock-scale)",     300'000},  // Rs  3,000.00
    {900004, "SYNTH-D (option-scale)",     10'000},  // Rs    100.00
}};

/// A deterministic interleaved session. One global sequence and a strictly
/// increasing timestamp, because `Replayer` refuses a tape that moves either
/// backwards -- and refusing is the right behaviour, so the generator has to
/// respect it rather than the replayer being loosened.
std::vector<altair::ReplayTick> demo_session(std::size_t count,
                                             altair::Timestamp start,
                                             std::uint64_t seed) {
    std::vector<altair::ReplayTick> out;
    out.reserve(count);

    std::uint64_t rng = seed * 6364136223846793005ull + 1442695040888963407ull;
    const auto next = [&rng]() noexcept -> std::uint64_t {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        return rng >> 33;
    };

    std::array<std::int64_t, kInstruments.size()> px{};
    for (std::size_t i = 0; i < kInstruments.size(); ++i) {
        px[i] = kInstruments[i].open_paise;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t which = i % kInstruments.size();
        // A step proportional to the price, so the option ticks in paise and
        // the index ticks in rupees, as they actually do.
        const std::int64_t scale = px[which] / 20'000 + 1;
        const std::int64_t step =
            (static_cast<std::int64_t>(next() % 201) - 100) * scale / 100;
        px[which] += step;
        if (px[which] < 1) {
            px[which] = 1;   // a price never reaches zero
        }

        altair::ReplayTick t{};
        // 100 ms apart, so 40,000 ticks span 67 minutes of session rather than
        // 40 seconds. A chart of 40 seconds is four candles and tells you
        // nothing about whether the bucketing is right.
        t.ts = start
             + altair::duration::millis(static_cast<std::int64_t>(i) * 100);
        t.seqno = static_cast<std::uint64_t>(i) + 1;
        t.token = kInstruments[which].token;
        t.reserved = 0;
        t.last = altair::Price{px[which]};
        t.qty = altair::Qty{static_cast<std::int64_t>(next() % 50) + 1};
        out.push_back(t);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // THE MARK. Altair is the brightest star in Aquila, the Eagle, so the icon
    // is an eagle climbing with the star at its apex -- the name and the
    // constellation, not a decoration.
    //
    // Set on the APPLICATION, not on one window, so every dialog, the taskbar
    // button and the alt-tab entry all get it. An SVG rather than a set of
    // PNGs because the OS asks for sizes from 16 to 256 and five bitmaps drift
    // apart the first time one of them is edited.
    app.setWindowIcon(QIcon(QStringLiteral(":/altair_eagle.svg")));

    // THE SPLASH, and why it is not decoration.
    //
    // The window takes a moment to appear: the instrument master is 108,411
    // rows and the tape is twenty sessions across three instruments. Before
    // this there was a gap where nothing was on screen, which reads as a
    // failed launch -- the same class of problem as a blank cell that might
    // be a zero.
    //
    // So the mark goes up immediately with a line of text underneath saying
    // what is loading. It is closed by the login dialog, not by a timer: a
    // splash that vanishes on a timer while the app is still loading has
    // simply moved the blank gap later.
    QSplashScreen splash(QPixmap(QStringLiteral(":/altair_eagle.svg"))
                             .scaled(220, 220, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
    splash.showMessage(
        QStringLiteral("  Altair — loading instruments and tape..."),
        Qt::AlignBottom | Qt::AlignHCenter, QColor(0xD6, 0xDB, 0xDF));
    splash.show();
    app.processEvents();
    QApplication::setApplicationName(QStringLiteral("Altair"));
    QApplication::setOrganizationName(QStringLiteral("Altair"));

    // 2026-09-03 09:15 IST == 03:45 UTC.
    //
    // THE FIRST VERSION OF THIS LINE SAID 09:15 *UTC* and was wrong by five and
    // a half hours: the session window in market_clock.hpp is IST-ns-since-
    // midnight, so the tape opened at 14:45 IST and the status bar correctly
    // reported CLOSED an hour later. The clock was right and the fixture was
    // wrong, which is the more common direction.
    //
    // Timestamps are UTC everywhere in the engine (core/time/timestamp.hpp);
    // only the SESSION WINDOW is expressed in IST, and that asymmetry is worth
    // knowing about before writing a literal.
    // ---- REAL INSTRUMENTS FIRST, SYNTHETIC ONLY AS A FALLBACK ------------
    //
    // P11Q-09. The grid showed SYNTH-A..D because there was nothing else;
    // dataset/ now holds 2.8 million real bars. So it replays NIFTY 50, the
    // near NIFTY future and India VIX under their own names, and the
    // synthetic walk survives only for a tree with no dataset/ -- where it is
    // labelled "(syn)" so it can never be mistaken for the real thing.
    // HOW MANY SESSIONS, AND WHY IT IS NO LONGER FIVE.
    //
    // It was 5, and the grid looked nearly empty -- about 1,875 one-minute
    // bars across three instruments, most of one screen. The number was set
    // when loading MORE meant reading every monthly file in dataset/ and
    // discarding all but the last week: 2.2 million rows parsed to keep under
    // two thousand. `load_bars_dir` now takes only the newest files it needs,
    // so the cost that justified 5 is gone and the default rises to a month.
    //
    // Still bounded, and the reason in `real_tape.hpp` still holds: the grid
    // is a live view, not an archive, and the chart is where history belongs.
    // `--sessions N` exists because "how much tape do I want on screen" is a
    // preference, not a fact, and 0 means every session held.
    std::size_t want_sessions = 20;
    {
        const QStringList early = QApplication::arguments();
        for (int i = 1; i + 1 < early.size(); ++i) {
            if (early[i] != QStringLiteral("--sessions")) { continue; }
            bool okv = false;
            const int v = early[i + 1].toInt(&okv);
            // Validated, not coerced. `toInt` returns 0 on failure and 0 is a
            // MEANINGFUL value here ("all sessions"), so a typo would silently
            // load 36 years instead of being refused -- the same trap that
            // made `--page models` open page 0.
            if (okv && v >= 0) {
                want_sessions = static_cast<std::size_t>(v);
            } else {
                std::fprintf(stderr,
                             "--sessions \"%s\" is not a count; using %zu\n",
                             early[i + 1].toUtf8().constData(), want_sessions);
            }
        }
    }
    const altair::ui::RealTape real = altair::ui::load_real_tape(
        QStringLiteral(ALTAIR_DATASET_DIR), want_sessions);

    const altair::Timestamp open{1'788'407'100'000'000'000LL};
    const auto synth = demo_session(40'000, open, 42);
    const std::vector<altair::ReplayTick>& ticks =
        real.ok() ? real.ticks : synth;

    const QStringList args = QApplication::arguments();

    // --page IS VALIDATED HERE, BEFORE THE LOGIN DIALOG.
    //
    // It used to go through `QString::toInt()` after login, and toInt() returns
    // 0 for anything it cannot parse -- so `--page models` opened page 0, the
    // Live Grid, and looked like it had worked. A screenshot taken that way
    // shows the wrong panel and announces nothing, which is how a wrong image
    // reaches a review. Rule 9 again: refuse, do not guess.
    //
    // Before login, because a typo that costs you a password entry and THEN
    // refuses has spent the one thing the dialog was protecting.
    int requested_page = 0;
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] != QStringLiteral("--page")) {
            continue;
        }
        bool numeric = false;
        const int n = args[i + 1].toInt(&numeric);
        requested_page = numeric ? n : altair::ui::nav_page_index(args[i + 1]);
        if (requested_page < 0
            || requested_page >= altair::ui::nav_page_names().size()) {
            // BOTH, and not for symmetry. This is a GUI-subsystem binary, so
            // qCritical has no console attached when launched from Explorer or
            // from Start-Process -- the first version of this refusal exited 2
            // and printed nothing anywhere a person would look, which is a
            // silent failure wearing the costume of a loud one. The dialog is
            // what the user actually sees; the qCritical is for a launch that
            // does have a console, and for the debugger.
            const QString msg =
                QStringLiteral("--page \"%1\" matches no page.\n\nPages are:\n%2")
                    .arg(args[i + 1],
                         altair::ui::nav_page_names().join(QStringLiteral("\n")));
            qCritical().noquote() << msg;
            QMessageBox::critical(nullptr, QStringLiteral("Altair"), msg);
            return 2;
        }
    }

    // THE GATE. A build with no accounts provisioned lets nobody in; the
    // dialog says so rather than falling back to a default.
    altair::ui::UserStore users;
    altair::ui::seed_default_users(users);

    // --as admin|staff skips the dialog. DEV BUILDS ONLY, and the guard is
    // the same compile definition that supplies the accounts: a build with no
    // default users has nothing for this to bypass to. It exists so a
    // screenshot or a manual check can reach a panel without synthesising
    // keystrokes, and it is deliberately NOT a --password flag -- a credential
    // on a command line is a credential in the shell history and the process
    // list.
    altair::ui::Role role = altair::ui::Role::None;
    QString who;
#ifdef ALTAIR_DEV_CREDENTIALS
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] != QStringLiteral("--as")) {
            continue;
        }
        if (args[i + 1] == QStringLiteral("admin")) {
            role = altair::ui::Role::Admin;
            who = QStringLiteral("admin");
        } else if (args[i + 1] == QStringLiteral("staff")) {
            role = altair::ui::Role::Staff;
            who = QStringLiteral("staff");
        }
    }
#endif

    if (role == altair::ui::Role::None) {
        // The splash comes down HERE, when there is something to replace it,
        // rather than on a timer -- a timed splash that expires mid-load just
        // moves the blank gap later.
        splash.close();
        altair::ui::LoginDialog login(users);
        if (login.exec() != QDialog::Accepted
            || login.role() == altair::ui::Role::None) {
            return 0;
        }
        role = login.role();
        who = login.user();
    }

    altair::ui::MainWindow window(ticks.data(), ticks.size(), role, who);
    if (real.ok()) {
        for (const auto& ins : real.instruments) {
            // `trades` is false for an index: no turnover to report, so the
            // grid must leave the cell EMPTY rather than draw a zero.
            window.add_instrument(ins.token, ins.symbol, ins.trades);
        }
    } else {
        // "(syn)" on every name, so a fabricated price can never be read as a
        // real one. The prices are round numbers nothing trades at for the
        // same reason.
        for (const auto& ins : kInstruments) {
            window.add_instrument(
                ins.token,
                QString::fromUtf8(ins.symbol) + QStringLiteral(" (syn)"));
        }
    }
    window.set_tape_is_real(real.ok(), real.sessions,
                            real.ok() ? QString() : real.error,
                            real.first_ns, real.last_ns);

    // --page N opens on a nav page, --prime N drains that many ticks first,
    // --source N picks a chart data source. They exist so a screenshot or a
    // manual check starts where the work is, rather than requiring a click a
    // capture script cannot make -- and emphatically rather than synthesising
    // keystrokes, which go to whatever window happens to have focus.
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == QStringLiteral("--page")) {
            window.show_page(requested_page);   // validated before login
        } else if (args[i] == QStringLiteral("--prime")) {
            window.prime(static_cast<std::size_t>(args[i + 1].toLongLong()));
        } else if (args[i] == QStringLiteral("--source")) {
            window.show_source(args[i + 1].toInt());
        }
    }

    // --train runs the Models page's walk-forward, for the same reason
    // --page exists: a capture script cannot click a button, and synthesising
    // a click sends it to whatever window has focus. Standalone, so it is
    // handled outside the pair loop above.
    if (args.contains(QStringLiteral("--train"))) {
        window.train_selected();
    }

    // Belt and braces: the login path closes it, but a run with --user set
    // skips login entirely and would otherwise leave the splash on top of the
    // window. finish() ties it to the widget that replaces it, which is the
    // one thing a splash should be tied to.
    splash.finish(&window);
    window.showFullScreen();

    return QApplication::exec();
}
