// live/fyers_names.hpp -- FYERS's own name for every NSE and BSE cash scrip.
//
// WHY THIS EXISTS. The Kite master names a scrip by its trading symbol; FYERS
// streams it under "<EX>:<symbol>-<series or group>", and the suffix is not in
// the Kite master. Building it ("NSE:<SYM>-EQ", "BSE:<SYM>-A") was right for
// about 3,300 of the 21,800 cash scrips in the 2026-10 masters: an NSE scrip
// outside the EQ series (BE, SM, ST, SG, GS, N0..) and a BSE scrip outside
// group A (B, T, X, XT, Z, M ...) got a name FYERS does not know, so the
// symbol-token call refused it and it never streamed -- the row stayed empty.
//
// THE FIX. Both masters carry the exchange's own token (Kite: exchange_token;
// FYERS: column 12, exToken; for BSE it is the scrip code), so the name is
// looked up, not built. FYERS publishes the cash masters with no login at
// https://public.fyers.in/sym_details/NSE_CM.csv and BSE_CM.csv (the 21-column
// layout of instruments/fyers_master.hpp: col 9 symTicker, col 10 exchange
// 10 = NSE / 12 = BSE, col 11 segment 10 = cash, col 12 exToken). The price
// service caches them under data/live/fyers_masters/ once a day.
//
// Without the masters, `fyers_cash_guess` keeps the old rule with one
// improvement: an NSE trading symbol ending in a two-letter series ("-BE",
// "-SM", "-N1") already carries it ("NSE:XYZ-BE"); a longer tail is part of
// the symbol ("BAJAJ-AUTO" -> "NSE:BAJAJ-AUTO-EQ"). That matched 9,459 of the
// 9,461 NSE cash scrips; BSE has no such rule and stays group A.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace altair::live {

class FyersCashNames {
public:
    /// Take the cash rows of one FYERS master CSV (NSE_CM or BSE_CM); other
    /// rows and lines that do not parse are skipped. Returns rows taken.
    std::size_t add_csv(std::istream& in) {
        std::size_t n = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string_view col[13];
            std::size_t k = 0, start = 0;
            for (std::size_t i = 0; i <= line.size() && k < 13; ++i) {
                if (i == line.size() || line[i] == ',') {
                    col[k++] = std::string_view(line).substr(start, i - start);
                    start = i + 1;
                }
            }
            if (k < 13 || col[11] != "10") continue;              // cash only
            const bool bse = col[10] == "12";
            if (!bse && col[10] != "10") continue;
            const std::string_view ticker = col[9];
            if (ticker.substr(0, 4) != (bse ? "BSE:" : "NSE:")) continue;
            const std::string tok(col[12]);
            char* end = nullptr;
            const unsigned long long t = std::strtoull(tok.c_str(), &end, 10);
            if (tok.empty() || end == nullptr || *end != '\0' || t == 0 || t > 0xFFFFFFFFull) continue;
            names_[key(bse, static_cast<std::uint32_t>(t))] = std::string(ticker);
            ++n;
        }
        return n;
    }

    /// Read `dir`/NSE_CM.csv and `dir`/BSE_CM.csv (either may be missing).
    std::size_t load(const std::string& dir) {
        std::size_t n = 0;
        for (const char* f : {"/NSE_CM.csv", "/BSE_CM.csv"}) {
            std::ifstream in(dir + f, std::ios::binary);
            if (in) n += add_csv(in);
        }
        return n;
    }

    /// FYERS's name for the exchange token, or nullptr when it has none.
    [[nodiscard]] const std::string* find(bool bse, std::uint32_t exchange_token) const {
        const auto it = names_.find(key(bse, exchange_token));
        return it == names_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }
    [[nodiscard]] bool empty() const noexcept { return names_.empty(); }

private:
    [[nodiscard]] static std::uint64_t key(bool bse, std::uint32_t t) noexcept {
        return (std::uint64_t{bse ? 1u : 0u} << 32) | t;
    }
    std::unordered_map<std::uint64_t, std::string> names_;
};

/// The FYERS name when the masters are not at hand (see the header).
[[nodiscard]] inline std::string fyers_cash_guess(bool bse, const std::string& symbol) {
    if (bse) return "BSE:" + symbol + "-A";
    const auto dash = symbol.rfind('-');
    if (dash != std::string::npos && symbol.size() - dash == 3) return "NSE:" + symbol;
    return "NSE:" + symbol + "-EQ";
}

/// FYERS's name for a Kite cash row: looked up by exchange token, else guessed.
[[nodiscard]] inline std::string fyers_cash_name(bool bse, const std::string& symbol, std::uint32_t exchange_token,
                                                 const FyersCashNames* names) {
    if (names != nullptr && exchange_token != 0)
        if (const std::string* n = names->find(bse, exchange_token)) return *n;
    return fyers_cash_guess(bse, symbol);
}

} // namespace altair::live
