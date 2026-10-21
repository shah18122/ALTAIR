// desktop/verdicts.hpp -- every strategy's test result in one plain line.
//
// What it does, what it made after costs, and what to do with it. Each
// verdict names its source document and when it was measured; the numbers
// are the documents', not recomputed here. Shown on Start here and on Demo
// Trading's Simple view.

#pragma once

#include <QString>

#include <vector>

namespace altair::ui {

/// One line of "what the tests found".
struct StartVerdict {
    QString what;      ///< the strategy or model, in plain words
    QString found;     ///< what it made, after costs
    QString verdict;   ///< what to do with it
    int tone = 0;      ///< +1 promising, 0 unproven, -1 does not work
    QString source;    ///< where the numbers come from, and when
};

[[nodiscard]] inline std::vector<StartVerdict> start_verdicts() {
    return {
        {QStringLiteral("Guessing whether NIFTY / BANKNIFTY goes up or down (12 model families, 5 timeframes)"),
         QStringLiteral("No model beats \"no change\" after costs, with or without implied/realised vol as inputs; "
                        "a shuffled-data placebo scores the same"),
         QStringLiteral("Do not trade direction from these. They are useful as inputs (volatility, regime)"), -1,
         QStringLiteral("ops/forecast-curriculum.md, 2026-10")},
        {QStringLiteral("altair_trader: one model choosing among all the others (CNN, similar days, ridge)"),
         QStringLiteral("NIFTY daily -Rs 2.69 lakh over 107 trips (buy and hold +Rs 9.74 lakh); BANKNIFTY daily +Rs 0.87 "
                        "lakh on 16 trips, inside what shuffled data gives; intraday both negative. Mostly chooses "
                        "\"no change\" -- the honest answer when nothing works"),
         QStringLiteral("No edge yet; paper only"), -1, QStringLiteral("ops/demo-trading.md, 2026-10-21")},
        {QStringLiteral("Selling the monthly ATM straddle, hedged with futures (variance premium)"),
         QStringLiteral("Real NSE closes 2015-2026: NIFTY +Rs 91,804 over 58 months (Rs 1,583 a month, t 1.5); "
                        "BANKNIFTY +Rs 74,820 (t 0.6). Implied vol averaged 14.2 % against 12.9 % realised"),
         QStringLiteral("A real premium, but small and not yet proven (t under 2). The best lead. Paper only"), 0,
         QStringLiteral("altair_vol_premium --source bhavcopy, 2026-10-21")},
        {QStringLiteral("Selling vol-of-vol: short the wings, long the money (vanna-volga)"),
         QStringLiteral("A small premium before slippage (NIFTY Rs 3.1 lakh gross over 562 weeks); any realistic "
                        "slippage turns every variant negative"),
         QStringLiteral("Not tradable as built: needs passive fills at mid"), -1,
         QStringLiteral("altair_vol_of_vol; ops/research-ideas.md, 2026-10-21")},
        {QStringLiteral("Selling options at a forecast band each morning (strangle at 09:20)"),
         QStringLiteral("Tested on synthetic premiums only; the \"sell at the touch\" rule lost and was dropped"),
         QStringLiteral("Unproven on real prices"), 0, QStringLiteral("ops/demo-trading.md")},
        {QStringLiteral("NSE vs BSE arbitrage (the same stock on two exchanges)"),
         QStringLiteral("Real but small gaps; profit depends on both legs filling and on expenses"),
         QStringLiteral("Paper first; then one small pair under the LIVE caps"), 0, QStringLiteral("ops/live-terminal.md")},
        {QStringLiteral("NIFTY / BANKNIFTY pair hedged with futures"),
         QStringLiteral("Rs 46,193 net over 2000-2026 (UNVERIFIED expenses) -- tiny for 26 years"),
         QStringLiteral("Not worth the capital"), -1, QStringLiteral("ops/demo-trading.md")},
        {QStringLiteral("NIFTY 50 stat-arb (stocks against market and sector)"),
         QStringLiteral("Tested on synthetic data only; real results need the FYERS stock history"),
         QStringLiteral("Unproven"), 0, QStringLiteral("ops/demo-trading.md")},
    };
}

[[nodiscard]] inline QString start_verdicts_html(const std::vector<StartVerdict>& v) {
    QString h = QStringLiteral("<table cellspacing='0' cellpadding='5' style='border-collapse:collapse'>"
                               "<tr style='color:#8FA3AE'><td><b>What</b></td><td><b>What it made, after costs</b></td>"
                               "<td><b>So</b></td></tr>");
    for (const auto& x : v) {
        const char* c = x.tone > 0 ? "#7FD17F" : x.tone < 0 ? "#F07A6A" : "#F4C95D";
        h += QStringLiteral("<tr><td style='border-top:1px solid #222A33'>%1<br><span style='color:#8FA3AE;font-size:11px'>%4</span></td>"
                            "<td style='border-top:1px solid #222A33'>%2</td>"
                            "<td style='border-top:1px solid #222A33;color:%5'><b>%3</b></td></tr>")
                 .arg(x.what.toHtmlEscaped(), x.found.toHtmlEscaped(), x.verdict.toHtmlEscaped(), x.source.toHtmlEscaped(),
                      QString::fromLatin1(c));
    }
    return h + QStringLiteral("</table>");
}

} // namespace altair::ui
