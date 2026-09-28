#include "../chain_panel.hpp"

#include <QApplication>
#include <QComboBox>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    altair::ui::ChainPanel panel;
    int failures = 0;
    const auto check = [&failures](bool ok, const char* text) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    check(panel.instrument_a() == QStringLiteral("NIFTY"),
          "pair A starts on the canonical NIFTY underlying");
    check(panel.instrument_b() == QStringLiteral("BANKNIFTY"),
          "pair B starts on a different canonical underlying");
    check(panel.instrument_a() != panel.instrument_b(),
          "the initial pair cannot be accidentally identical");
    check(panel.side_a() == QStringLiteral("BUY") && panel.ratio_a() == 2
              && panel.side_b() == QStringLiteral("SELL") && panel.ratio_b() == 1,
          "default pair is buy two NIFTY against sell one BANKNIFTY");
    const auto boxes = panel.findChildren<QComboBox*>();
    check(boxes.size() >= 2, "two independent instrument selectors exist");
    if (boxes.size() >= 2) {
        boxes.at(1)->setCurrentIndex(0);
        check(panel.instrument_a() != panel.instrument_b(),
              "duplicate selection is refused");
        boxes.at(1)->setCurrentIndex(2);
        check(panel.instrument_b() == QStringLiteral("CIPLA"),
              "the second leg can select CIPLA without a token assumption");
    }
    check(panel.pair_status().contains(QStringLiteral("Awaiting both legs")),
          "pair status refuses comparison until both legs have data");
    check(!panel.set_pair_price(QStringLiteral("A"), 0, true, 100),
          "zero price or timestamp is refused without clearing state");
    check(panel.set_pair_price(QStringLiteral("A"), 100'00, true, 1'000'000'000),
          "replay price with timestamp is accepted for leg A");
    check(panel.set_pair_price(QStringLiteral("B"), 200'00, true, 2'000'000'000),
          "replay price with timestamp is accepted for leg B");
    check(panel.pair_status().contains(QStringLiteral("Aligned REPLAY")),
          "same-source legs within skew report aligned data");
    check(panel.set_pair_price(QStringLiteral("B"), 200'00, false, 2'000'000'000),
          "live source update is accepted");
    check(panel.pair_status().contains(QStringLiteral("MIXED SOURCE")),
          "mixed replay/live legs are refused visibly");
    check(panel.set_pair_price(QStringLiteral("A"), 100'00, false, 20'000'000'000),
          "live leg can be refreshed explicitly");
    check(panel.pair_status().contains(QStringLiteral("STALE SKEW")),
          "stale timestamp skew is refused visibly");

    QTemporaryDir directory;
    QSettings settings(directory.filePath(QStringLiteral("pair.ini")), QSettings::IniFormat);
    panel.save_pair(settings);
    altair::ui::ChainPanel restored;
    check(restored.restore_pair(settings)
              && restored.instrument_a() == panel.instrument_a()
              && restored.instrument_b() == panel.instrument_b()
              && restored.side_a() == panel.side_a()
              && restored.side_b() == panel.side_b()
              && restored.ratio_a() == panel.ratio_a()
              && restored.ratio_b() == panel.ratio_b(),
          "canonical identities, sides and integer ratios persist and restore");
    settings.setValue(QStringLiteral("pairTrading/instrumentB"),
                      settings.value(QStringLiteral("pairTrading/instrumentA")));
    check(!restored.restore_pair(settings),
          "persisted duplicate A/B identities are refused");
    return failures == 0 ? 0 : 1;
}
