// The forecast page's leak test: time order, purge, embargo, train-only
// fitting, and depth features that never see the bar after the one they describe.
#include <models/depth_features.hpp>
#include <models/stream_forecast.hpp>

#include <cstdio>
#include <random>

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
constexpr std::int64_t minute = 60'000'000'000LL;
constexpr std::int64_t day = 86'400'000'000'000LL;
constexpr std::int64_t first_open = 1'704'080'700'000'000'000LL;   // 2024-01-01 09:15 IST

void sessions(std::size_t days, std::int64_t width, std::uint32_t seed, std::vector<double>& c,
              std::vector<std::int64_t>& e) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> z(0.0, 8.0);
    c.clear(); e.clear();
    double p = 23000.0;
    std::size_t made = 0;
    for (std::int64_t d = 0; made < days; ++d) {
        if (d % 7 >= 5) continue;
        ++made;
        const std::int64_t open = first_open + d * day;
        p *= std::exp(z(rng) * 4.0 / 1e4);
        for (std::int64_t t = open + width; t <= open + 375 * minute; t += width) {
            p *= std::exp(z(rng) / 1e4);
            c.push_back(p);
            e.push_back(t);
        }
    }
}

struct Book { altair::depth::BookRung b[5], a[5]; };
Book book(std::int64_t mid_paise, std::int64_t bias) {
    Book k;
    for (int i = 0; i < 5; ++i) {
        k.b[i] = {mid_paise - 5 - 5 * i, 1000 + bias + 100 * i};
        k.a[i] = {mid_paise + 5 + 5 * i, 1000 - bias + 100 * i};
    }
    return k;
}
}

int main() {
    namespace dp = altair::depth;
    constexpr std::int64_t q = 15 * minute;
    std::vector<double> c;
    std::vector<std::int64_t> e;
    sessions(300, q, 7, c, e);
    constexpr std::size_t h = 4;

    // 1. FEATURES ARE CAUSAL. Replace everything after bar k with noise: every
    //    row anchored at or before k keeps its features, and every row whose
    //    label ends at or before k keeps its label.
    const auto a = altair::build_stream_rows(c, e, q, h);
    const std::size_t k = c.size() / 2;
    auto future = c;
    std::mt19937 rng(99);
    std::uniform_real_distribution<double> u(0.5, 2.0);
    for (std::size_t i = k + 1; i < future.size(); ++i) future[i] = c[i] * u(rng);
    const auto b = altair::build_stream_rows(future, e, q, h);
    bool same_features = a && b, same_labels = a && b, checked = false;
    if (a && b) {
        for (std::size_t i = 0; i < a->rows.size() && i < b->rows.size(); ++i) {
            const auto& x = a->rows[i];
            const auto& y = b->rows[i];
            if (x.bar > k) break;
            checked = true;
            same_features = same_features && x.bar == y.bar && x.returns == y.returns;
            if (x.label_end_ns <= e[k]) same_labels = same_labels && x.label_bps == y.label_bps;
        }
    }
    check(checked && same_features, "rewriting every bar after the anchor cannot change any earlier row's features");
    check(checked && same_labels, "a label that ended before the rewrite is unchanged by it");

    // 2. LABELS ARE STRICTLY AFTER THE ANCHOR, inside one session.
    bool after = a.has_value();
    if (a)
        for (const auto& r : a->rows)
            after = after && r.label_end_ns == r.anchor_ns + static_cast<std::int64_t>(h) * q &&
                    e[r.bar] == r.anchor_ns && (r.anchor_ns - first_open) % day + 4 * q <= 375 * minute;
    check(after, "every label starts at its anchor bar's close and ends four bars later, same session");

    // 3. PURGE AND EMBARGO. No training label reaches the first validation
    //    anchor, and the next partition starts at least four bars later.
    if (a) {
        const std::int64_t embargo = static_cast<std::int64_t>(h) * q;
        const auto s = altair::split_stream_rows(a->rows, embargo);
        std::int64_t train_end = 0, valid_end = 0, valid_first = INT64_MAX, test_first = INT64_MAX;
        std::int64_t train_last_anchor = 0, valid_last_anchor = 0;
        for (auto i : s.train) { train_end = std::max(train_end, a->rows[i].label_end_ns); train_last_anchor = std::max(train_last_anchor, a->rows[i].anchor_ns); }
        for (auto i : s.valid) { valid_end = std::max(valid_end, a->rows[i].label_end_ns); valid_first = std::min(valid_first, a->rows[i].anchor_ns); valid_last_anchor = std::max(valid_last_anchor, a->rows[i].anchor_ns); }
        for (auto i : s.test) test_first = std::min(test_first, a->rows[i].anchor_ns);
        check(!s.train.empty() && !s.valid.empty() && !s.test.empty(), "three non-empty partitions");
        check(train_last_anchor < valid_first && valid_last_anchor < test_first, "partitions are in strict time order");
        check(train_end <= valid_first && valid_end <= test_first,
              "purged: no training label ends after validation starts, no validation label after calibration starts");
        check(s.purged > 0 && s.embargoed > 0, "overlapping rows were purged and boundary rows embargoed");
        bool gap = true;
        for (auto i : s.valid) gap = gap && a->rows[i].anchor_ns >= a->rows[s.valid.front()].anchor_ns;
        check(gap && valid_first - train_last_anchor >= embargo && test_first - valid_last_anchor >= embargo,
              "embargoed: at least one horizon between the last row of a partition and the first of the next");
    }

    // 4. NOTHING IS FITTED ON THE FUTURE. Rewriting only the calibration
    //    period (prices after the validation rows' last label) leaves the
    //    fitted trees, their bin edges and every prediction unchanged.
    const auto fit = altair::fit_stream_forecast(c, e, q, h);
    check(fit.has_value(), "the fixture fits");
    if (fit && a) {
        const auto s = altair::split_stream_rows(a->rows, static_cast<std::int64_t>(h) * q);
        const std::size_t cut = a->rows[s.test.front()].bar + 1;   // calibration labels start here
        auto later = c;
        for (std::size_t i = cut; i < later.size(); ++i) later[i] = c[i] * u(rng);
        const auto refit = altair::fit_stream_forecast(later, e, q, h);
        bool same = refit.has_value() && refit->chosen_depth == fit->chosen_depth &&
                    refit->model.binner.edge == fit->model.binner.edge;
        std::array<double, 20> probe{};
        for (int t = 0; t < 50 && same; ++t) {
            for (std::size_t j = 0; j < probe.size(); ++j) probe[j] = std::sin(0.3 * (static_cast<double>(t) * 20.0 + static_cast<double>(j))) * 15.0;
            const auto x = fit->predict(probe, 23000.0, e.back());
            const auto y = refit->predict(probe, 23000.0, e.back());
            same = x && y && x->predicted == y->predicted;
        }
        check(same, "bin edges, chosen tree depth and predictions ignore every calibration-period price");
    }

    // 5. DEPTH: the bar ending at E uses minutes in (E - width, E] only.
    std::vector<dp::DepthMinute> mins;
    for (std::int64_t t = first_open + minute; t <= first_open + 375 * minute; t += minute) {
        dp::DepthMinute m;
        m.end_ns = t;
        m.books = 10;
        const double v = std::sin(static_cast<double>(t / minute));
        m.obi1_sum = m.obi5_sum = m.obi5w_sum = 10 * v * 0.5;
        m.spread_bps_sum = 10 * 0.3;
        m.micro_bps_last = v;
        m.ofi5 = v * 100;
        m.ofi5_abs = 100;
        mins.push_back(m);
    }
    std::vector<std::int64_t> bar_ends;
    for (std::int64_t t = first_open + q; t <= first_open + 375 * minute; t += q) bar_ends.push_back(t);
    const auto d1 = dp::depth_for_bars(mins, bar_ends, q);
    bool causal = d1.size() == bar_ends.size();
    for (std::size_t j = 0; j + 1 < bar_ends.size() && causal; ++j) {
        auto moved = mins;
        for (auto& m : moved)
            if (m.end_ns > bar_ends[j]) { m.obi5_sum = -m.obi5_sum + 3.0; m.micro_bps_last += 7.0; m.ofi5 = -m.ofi5; }
        const auto d2 = dp::depth_for_bars(moved, bar_ends, q);
        for (std::size_t i = 0; i <= j; ++i) causal = causal && d1[i] == d2[i];
        causal = causal && !(d1[j + 1] == d2[j + 1]);   // and the change is seen where it belongs
    }
    check(causal, "rewriting depth minutes after a bar's end changes no earlier bar and does change the next");
    {
        // The minute ending exactly at E counts; the one ending at E - width does not.
        std::vector<dp::DepthMinute> two(2);
        two[0].end_ns = first_open; two[0].books = 1; two[0].micro_bps_last = 5.0;
        two[1].end_ns = first_open + minute; two[1].books = 1; two[1].micro_bps_last = 9.0;
        const std::vector<std::int64_t> one_bar{first_open + minute};
        const auto r = dp::depth_for_bars(two, one_bar, minute);
        check(r.size() == 1 && r[0][3] == 9.0, "a bar includes the minute ending at its end and not the one before");
        const std::vector<std::int64_t> earlier{first_open};
        const auto r2 = dp::depth_for_bars(std::span<const dp::DepthMinute>(two.data() + 1, 1), earlier, minute);
        check(r2.size() == 1 && std::isnan(r2[0][0]), "a bar with no covering minute is missing (NaN), never zero");
    }

    // 6. A PLANTED LEAK IS VISIBLE. If a "depth" column secretly held the
    //    future return, the fit's own returns-only comparison exposes it.
    {
        const auto rows = altair::build_stream_rows(c, e, q, h);
        std::vector<dp::DepthBar> leak(c.size(), dp::missing_bar());
        if (rows)
            for (const auto& r : rows->rows) leak[r.bar].fill(r.label_bps);
        const auto leaked = altair::fit_stream_forecast(c, e, q, h, leak);
        check(leaked && leaked->returns_only_rmse_bps > 2.0 * leaked->rmse_bps,
              "planted look-ahead depth beats returns-only by a mile: the side-by-side score would flag it");
        std::vector<dp::DepthBar> honest(c.size(), dp::missing_bar());
        std::mt19937 noise(5);
        std::normal_distribution<double> n01;
        for (auto& row : honest) for (auto& v : row) v = n01(noise);
        const auto plain = altair::fit_stream_forecast(c, e, q, h, honest);
        check(plain && plain->rmse_bps > 0.9 * plain->returns_only_rmse_bps,
              "noise depth does not beat returns-only (no hidden channel from the future)");
    }

    // 7. THE BUILDER: known book, minute stamps, OFI direction, CSV round trip.
    {
        dp::DepthMinuteBuilder builder;
        std::vector<dp::DepthMinute> done;
        const std::int64_t t0 = first_open + 10'000'000'000LL;   // 09:15:10 IST
        const Book k0 = book(2'300'000, 0), k1 = book(2'300'000, 400);
        check(builder.on_book(k0.b, 5, k0.a, 5, t0, done) && builder.on_book(k1.b, 5, k1.a, 5, t0 + 1, done),
              "books in time order are accepted");
        check(!builder.on_book(k0.b, 5, k0.a, 5, t0, done), "a book older than the last one is refused");
        const auto cur = builder.current();
        check(cur && cur->books == 2 && cur->end_ns == first_open + minute, "a minute is stamped at its END");
        check(cur && cur->ofi5 > 0.0 && cur->ofi5_abs >= cur->ofi5, "more bid and less ask size reads as positive OFI");
        check(cur && std::fabs(cur->obi1_sum - (1400.0 - 600.0) / 2000.0) < 1e-12, "touch OBI = (B1 - A1) / (B1 + A1)");
        check(cur && cur->micro_bps_last > 0.0, "heavy bids pull the microprice above the mid");
        (void)builder.on_book(k0.b, 5, k0.a, 5, first_open + minute + 1, done);
        check(done.size() == 1 && done[0].books == 2, "the next minute's first book finishes the last minute");
        const Book crossed = book(2'300'000, 0);
        Book x = crossed;
        x.a[0].px = x.b[0].px;
        const auto before = builder.current()->books;
        (void)builder.on_book(x.b, 5, x.a, 5, first_open + minute + 2, done);
        check(builder.current()->books == before, "a locked or crossed book is not a reading");
        const auto line = dp::csv_row(done[0], "NSE:NIFTY26NOVFUT");
        std::string sym;
        const auto back = dp::parse_csv_row(line, &sym);
        check(back && sym == "NSE:NIFTY26NOVFUT" && back->end_ns == done[0].end_ns && back->books == done[0].books &&
                  back->ofi5 == done[0].ofi5 && back->obi5w_sum == done[0].obi5w_sum,
              "depth_1m.csv round-trips exactly");
        check(dp::format_ist(first_open) == "2024-01-01T09:15:00+05:30" &&
                  dp::parse_ist("2024-01-01T09:15:00+05:30") == first_open,
              "IST stamps convert both ways");
    }
    return failures ? 1 : 0;
}
