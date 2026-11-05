// Daily findings, the Work log, the command palette's matching and saved
// layouts, on temporary files only.

#include "../command_palette.hpp"
#include "../daily_findings_page.hpp"
#include "../saved_layouts.hpp"
#include "../work_log_page.hpp"
#include "../server_page.hpp"
#include "../pipeline_page.hpp"

#include <QApplication>
#include <QTemporaryDir>

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
void write(const QString& path, const QByteArray& text) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(text);
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    using namespace altair::ui;
    QTemporaryDir dir;
    const QString root = dir.path();

    // ---- Daily findings -----------------------------------------------------
    write(root + "/data/verified/forecast_log/nifty_daily.csv",
          "time,stage,learned_days,model,last_price,next_price,forecast_price,p_up,call,moved,result,net_bp\n"
          "2026-10-23 15:30:00,1,500,HAR,24000,24100,24050,0.6,UP,UP,RIGHT,4.5\n"
          "2026-10-24 15:30:00,1,500,\"ARMA(1,1)\",24100,24050,24150,0.6,UP,DOWN,WRONG,-6\n"
          "2026-10-24 15:30:00,1,500,HAR,24100,24050,24080,0.4,DOWN,DOWN,RIGHT,5\n");
    write(root + "/data/verified/forecast_log/india_vix_daily.csv",
          "time,stage,learned_days,model,last_price,next_price,forecast_price,p_up,call,moved,result,net_bp\n"
          "2026-10-24 15:30:00,1,500,HAR,12.1,12.4,12.3,,UP,UP,RIGHT,\n");
    write(root + "/" + kDemoTradesFile,
          "date,model,symbol,token,side,qty,entry_time,entry,exit_time,exit,gross,expenses,net,why_in,why_out,source,costs\n"
          "2026-10-24,OHL,NIFTY FUT,1,long,75,2026-10-24 09:20:01,24110,2026-10-24 15:20:01,24150,3000,80,2920,open = low,square off,SIM,\n");
    write(root + "/data/live/paper/decisions.csv",
          "time,ns,model,decision\n"
          "2026-10-24 09:20:00,1,OHL,open = low: buy\n"
          "2026-10-24 09:25:00,2,Vol band (HAR),IV 11.9 < forecast RV 12.3: no straddle\n"
          "2026-10-25 09:16:00,3,OHL,watching\n");
    write(root + "/data/logs/2026-10-24/recorder.log", "recorded 1,234,567 ticks\n");
    const QStringList days = findings_days(root);
    check(days.size() == 3 && days.front() == "2026-10-25" && days.contains("2026-10-23"),
          "every day with a forecast, a trade or a decision, newest first");
    const DayFindings f = day_findings(root, "2026-10-24");
    check(f.html.contains("ARMA(1,1)") && f.html.contains("1 of 2 scored right") && f.html.contains("india_vix_daily")
              && !f.html.contains("24000"),
          "that day's forecasts only, every model, scored");
    check(f.html.contains("open = low") && f.html.contains("2920.00") && f.html.contains("Real orders (FYERS)</b>: no trades"),
          "paper trades with their reasons; no real orders said so");
    check(f.html.contains("IV 11.9 &lt; forecast RV 12.3: no straddle") && !f.html.contains("watching"),
          "each model's last decision: the reason a model stood aside");
    check(f.html.contains("data/logs/2026-10-24/recorder.log"), "the day's logs are listed");
    check(f.markdown.startsWith("# ALTAIR findings, 2026-10-24") && f.markdown.contains("nifty_daily: 2 calls, 1 of 2 scored right"),
          "the same day as Markdown");
    DailyFindingsPage page(root);
    page.rescan();
    page.day_box()->setCurrentText("2026-10-24");
    check(page.text().contains("What each model decided"), "the page shows the chosen day");
    const QString saved = page.save();
    check(saved.endsWith("data/logs/findings/2026-10-24.md") && QFile::exists(saved), "Save findings writes data/logs/findings/<date>.md");
    check(day_findings(root, "2026-10-22").html.contains("No forecasts logged"), "an empty day says what is missing");

    // ---- Work log -----------------------------------------------------------
    write(root + "/docs/STATUS.md", "# Status\n\nEverything **works**.\n");
    write(root + "/docs/research/ideas.md", "# Ideas\n");
    write(root + "/research/reference/someone/README.md", "# not ours\n");
    write(root + "/ops/go-live.md", "# Go live\n");
    write(root + "/data/verified/vol_premium/summary.csv", "variant,net\nhedged,100\n");
    const auto work = find_work(root);
    const auto has = [&work](const char* section, const char* label) {
        return std::any_of(work.begin(), work.end(), [&](const WorkItem& w) {
            return w.section == QLatin1String(section) && w.label == QLatin1String(label);
        });
    };
    check(has("Status", "docs/STATUS.md") && has("Research notes", "docs/research/ideas.md") && has("Runbooks", "ops/go-live.md")
              && has("Results", "data/verified/vol_premium/summary.csv") && has("Daily logs", "data/logs/2026-10-24/recorder.log")
              && has("Daily logs", "data/logs/findings/2026-10-24.md"),
          "status, notes, runbooks, results and logs are all found");
    check(!has("Research notes", "research/reference/someone/README.md"), "reference clones are not listed as our work");
    WorkLogPage log(root);
    log.rescan();
    check(log.item_count() == static_cast<int>(work.size()), "the page lists them");
    check(log.select("docs/STATUS.md") && log.shown_text().contains("Everything works"), "Markdown is rendered");
    check(log.select("summary.csv") && log.shown_text() == "table:1", "a CSV opens as a table");
    QString why;
    (void)git_history(root, &why);
    check(!why.isEmpty(), "outside a repository the history says why it is empty");

    // ---- the Server page -----------------------------------------------------
    {
        const auto m = parse_systemctl_show("LoadState=loaded\nActiveState=inactive\nResult=exit-code\nExecMainStatus=3\n"
                                            "ExecMainStartTimestamp=Tue 2026-11-03 08:30:00 IST\n");
        check(m.value("Result") == "exit-code" && m.size() == 5, "systemctl show parses to keys and values");
        check(job_outcome(m) == "ended: exit-code (exit 3)", "a failed job says how it ended");
        auto ok = m;
        ok["Result"] = "success";
        check(job_outcome(ok) == "ok", "a good run says ok");
        check(job_outcome(parse_systemctl_show("LoadState=not-found\n")).startsWith("not installed"), "a missing unit says how to install it");
        auto never = ok;
        never["ExecMainStartTimestamp"] = "";
        check(job_outcome(never) == "not run yet", "a timer that has not fired yet says so");
        const QStringList cmd = update_command("/srv/altair");
        check(cmd.first() == "x-terminal-emulator" && cmd.last().contains("sudo '/srv/altair/ops/setup.sh'"),
              "Install or update runs sudo ops/setup.sh in a terminal");
        check(kServerJobs.size() == 6 && QString(kServerJobs[0].unit) == "altair-fyers-login",
              "the six scheduled jobs, the login first");
        ServerPage page(root);
        page.refresh_all();
        check(page.jobs()->rowCount() == 6 && page.detail()->toPlainText().contains("Latency tuning in effect"),
              "the page lists the jobs and the latency tuning");
    }

    // ---- palette matching ---------------------------------------------------
    check(palette_score("atl", "Model Atlas") > 0 && palette_score("xyz", "Model Atlas") < 0, "letters in order match; others do not");
    check(palette_score("ma", "Model Atlas") > palette_score("ma", "Demo Trading Pairs Alpha"),
          "word starts beat letters scattered through a long name");
    std::vector<PaletteEntry> entries{{"Terminal", "Page", {}, {}}, {"Threshold", "Page", {}, {}}, {"Backtests", "Page", {}, {}}};
    const auto ranked = palette_rank(entries, "th");
    check(!ranked.empty() && ranked.front() == 1, "Threshold first for \"th\"");
    check(palette_rank(entries, "").size() == 3, "an empty query lists everything");

    // ---- saved layouts ------------------------------------------------------
    QSettings s(root + "/layouts.ini", QSettings::IniFormat);
    save_layout(s, {"Evening", "strategies.backtests", {}, true, false, 1, {"300", "700"}});
    save_layout(s, {"a/b=c", "market.terminal", "chain", false, true, -1, {}});
    auto ls = load_layouts(s);
    check(ls.size() == 2 && ls[0].name == "Evening" && ls[0].sidebar_compact && !ls[0].news && ls[0].simple == 1
              && ls[0].terminal_sizes == QStringList({"300", "700"}) && ls[1].name == "a/b=c" && ls[1].terminal_view == "chain",
          "layouts save and load, any name");
    remove_layout(s, "Evening");
    check(load_layouts(s).size() == 1, "a saved layout can be deleted");
    check(builtin_layouts().size() == 3 && builtin_layouts()[0].page == "models.findings", "Morning, Trading and Review are built in");

    // ---- Who does what (desktop/pipeline_page.hpp) ------------------------------
    {
        // 2026-11-05 10:00 IST = 04:30 UTC. 20:00 UTC is already the next IST day.
        const QDateTime now(QDate(2026, 11, 5), QTime(4, 30), QTimeZone::UTC);
        check(ist_today(now) == QDate(2026, 11, 5) && ist_today(QDateTime(QDate(2026, 11, 4), QTime(20, 0), QTimeZone::UTC)) == QDate(2026, 11, 5),
              "the trading day is IST's date, not the server's UTC date");
        const QString pr = root + "/pipe";
        check(evidence_of(pr, "data/fyers_session.json", 0, now).health == StepHealth::Never, "no token file: never");
        write(pr + "/data/fyers_session.json", "{}");
        write(pr + "/data/live/feed_status.json", "{}");
        QFile tok(pr + "/data/fyers_session.json");
        tok.open(QIODevice::ReadWrite);
        tok.setFileTime(QDateTime(QDate(2026, 11, 5), QTime(3, 1), QTimeZone::UTC), QFileDevice::FileModificationTime);
        tok.close();
        QFile feed(pr + "/data/live/feed_status.json");
        feed.open(QIODevice::ReadWrite);
        feed.setFileTime(now.addSecs(-10), QFileDevice::FileModificationTime);
        feed.close();
        const auto p = pipeline_pills(pr, now);
        check(p.token.health == StepHealth::Today && token_pill_text(p.token).contains("today 08:31"),
              "a token written at 08:31 IST today reads as today's");
        check(evidence_of(pr, "data/fyers_session.json", 0, now.addDays(1)).health == StepHealth::NotToday
              && token_pill_text(evidence_of(pr, "data/fyers_session.json", 0, now.addDays(1))).contains("old"),
              "yesterday's token reads as old");
        check(p.feed.health == StepHealth::Fresh && p.engine.health == StepHealth::Never, "a feed written 10 s ago is running; no engine file");
        check(recorder_pill_text("active", p.feed).contains("recording") && recorder_pill_text("inactive", p.feed).contains("desktop feed on")
              && recorder_pill_text("", Evidence{}).contains("unknown") && engine_pill_text(p.engine).contains("off"),
              "the recorder pill combines systemd's state with the feed's file");
        QDir().mkpath(pr + "/data/store/tapes");
        check(evidence_of(pr, "data/store/tapes/{day}", 120, now).health == StepHealth::NotToday,
              "a {day} folder not made yet, under a folder that exists, is not today");
        PipelinePage page(pr);
        auto* steps = page.findChild<QTableWidget*>("pipelineSteps");
        auto* models = page.findChild<QTableWidget*>("pipelineModels");
        check(steps && models && steps->rowCount() == static_cast<int>(kPipelineSteps.size())
              && models->rowCount() == static_cast<int>(kPipelineModels.size())
              && !steps->item(1, 1)->text().isEmpty(),
              "the page lists every step with a status and every model");
    }

    std::printf("%s\n", failures == 0 ? "all workstation checks passed" : "workstation checks did not pass");
    return failures == 0 ? 0 : 1;
}
