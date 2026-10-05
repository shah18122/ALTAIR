// P37-01 acceptance tests for server/price_payload.hpp.
//
// THE BYTE VECTOR IS THE CONTRACT.
//
// server/ and any client share a wire format and never a header -- that is the
// rule in CLAUDE.md, and the reason is that two implementations sharing a
// struct definition drift silently while two implementations sharing a byte
// vector fail loudly. So the first check here is not a round trip. It is a
// hand-written hexadecimal string that the encoder must reproduce EXACTLY. A
// round trip passes even when both directions are wrong in the same way.
//
// THE SECOND THEME IS THAT A LENGTH READ OFF THE WIRE IS UNTRUSTED.
//
// `depth_levels` is a number an attacker, a bug, or a version mismatch
// controls, and believing it reads past the end of the buffer. Every path that
// consumes it is given a hostile value here.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <server/price_payload.hpp>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    if (ok) {
        std::printf("  ok  : %s\n", what);
    } else {
        ++failures;
        std::printf("  FAIL: %s\n", what);
    }
}

std::string hex(const std::uint8_t* p, std::size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0x0F]);
    }
    return s;
}

}  // namespace

int main()
{
    std::printf("P37-01 price payload\n\n");

    // ---- the vector -----------------------------------------------------
    //
    // An index tick: NIFTY 50 (token 256265), last 2343150 paise, no volume,
    // no open interest, no book. Written out by hand from the field order in
    // price_payload.hpp, little-endian throughout.
    //
    //   token          256265 = 0x0003E909 -> 09 e9 03 00
    //   flags               0              -> 00 00
    //   depth_levels        0              -> 00 00
    //   last_paise    2343150 = 0x0000000000023C0EE
    //   last_qty            0
    //   volume              0
    //   oi                  0
    //   exchange_ts   1789000000000000000 = 0x18D3CE057F2C8000
    //
    // THE FIRST DRAFT OF THIS VECTOR WAS WRONG IN THREE FIELDS and the
    // encoder disagreed with it, which is the check doing its job in the one
    // direction nobody plans for. Each was then converted independently
    // rather than copied from the failure message: 256265 = 0x0003E909
    // (3*65536 + 14*4096 + 9*256 + 9), 2343150 = 0x23C0EE, and
    // 0x18D3CE057F2C8000 = 416534021 * 2^32 + 0x7F2C8000, which is 1.789e18.
    //
    // Recording that here because the value of a hand vector rests entirely on
    // it having been derived from the format rather than from the output. If a
    // later reader assumes this was pasted from a failing run, the check
    // becomes a change detector and stops being evidence.
    {
        altair::PricePayload p;
        p.token = 256265;
        p.last_paise = 2343150;
        p.exchange_ts_ns = 1789000000000000000LL;

        std::uint8_t buf[256]{};
        const auto n = altair::encode_price(p, nullptr, nullptr, buf,
                                            sizeof buf);
        check(n.has_value() && *n == altair::kPricePayloadBytes,
              "an index tick encodes to exactly 48 bytes");
        const std::string got = hex(buf, altair::kPricePayloadBytes);
        const std::string want =
            "09e90300"          // token         256265
            "0000"              // flags
            "0000"              // depth_levels
            "eec0230000000000"  // last_paise    2343150
            "0000000000000000"  // last_qty
            "0000000000000000"  // volume
            "0000000000000000"  // oi
            "00802c7f05ced318"; // exchange_ts   1789000000000000000
        if (got != want) {
            std::printf("        got  %s\n        want %s\n",
                        got.c_str(), want.c_str());
        }
        check(got == want, "and reproduces the conformance vector byte for byte");
    }

    // ---- absence is not zero --------------------------------------------
    {
        altair::PricePayload idx;
        idx.token = 264969;          // INDIA VIX -- an index: no volume, no OI
        idx.last_paise = 1157;

        altair::PricePayload eq;
        eq.token = 738561;           // an equity that genuinely traded nothing
        eq.last_paise = 1157;
        eq.flags = altair::kPriceHasVolume;
        eq.volume = 0;

        std::uint8_t a[64]{}, b[64]{};
        (void)altair::encode_price(idx, nullptr, nullptr, a, sizeof a);
        (void)altair::encode_price(eq, nullptr, nullptr, b, sizeof b);
        const auto da = altair::decode_price(a, sizeof a);
        const auto db = altair::decode_price(b, sizeof b);
        check(da && db, "both decode");
        check(da && !da->payload.has(altair::kPriceHasVolume),
              "an index reports volume ABSENT");
        check(db && db->payload.has(altair::kPriceHasVolume)
                  && db->payload.volume == 0,
              "and a real zero volume is reported PRESENT and zero -- the two "
              "are different facts and the wire keeps them different");
    }

    // ---- a book round-trips, both sides, same count ----------------------
    {
        altair::PricePayload p;
        p.token = 17512194;
        p.last_paise = 2343150;
        p.flags = altair::kPriceHasVolume | altair::kPriceHasOi
                  | altair::kPriceHasBook;
        p.volume = 125000;
        p.oi = 9800;
        p.depth_levels = 5;

        altair::PriceLevel bids[5]{}, asks[5]{};
        for (int i = 0; i < 5; ++i) {
            bids[i] = {2343100 - i * 25, 50 + i, static_cast<std::uint32_t>(i + 1), 0};
            asks[i] = {2343175 + i * 25, 60 + i, static_cast<std::uint32_t>(i + 2), 0};
        }

        std::uint8_t buf[512]{};
        const auto n = altair::encode_price(p, bids, asks, buf, sizeof buf);
        check(n.has_value() && *n == altair::price_frame_bytes(5),
              "a five-level book encodes to 48 + 2*5*24 = 288 bytes");

        const auto d = altair::decode_price(buf, *n);
        check(d.has_value(), "and decodes");
        bool same = d.has_value();
        if (d) {
            same = d->payload.token == p.token
                   && d->payload.last_paise == p.last_paise
                   && d->payload.volume == p.volume
                   && d->payload.oi == p.oi
                   && d->payload.depth_levels == 5;
            for (int i = 0; i < 5 && same; ++i) {
                same = d->bids[i].price_paise == bids[i].price_paise
                       && d->bids[i].qty == bids[i].qty
                       && d->bids[i].orders == bids[i].orders
                       && d->asks[i].price_paise == asks[i].price_paise
                       && d->asks[i].qty == asks[i].qty
                       && d->asks[i].orders == asks[i].orders;
            }
        }
        check(same, "every field and every level survives the round trip");
    }

    // ---- hostile input ---------------------------------------------------
    {
        altair::PricePayload p;
        p.flags = altair::kPriceHasBook;
        p.depth_levels = 51;                // deeper than the format allows
        altair::PriceLevel lv[51]{};
        std::uint8_t buf[4096]{};
        check(!altair::encode_price(p, lv, lv, buf, sizeof buf).has_value(),
              "encoding a book deeper than fifty levels is refused, not "
              "truncated to fifty");
        p.depth_levels = 50;                // the FYERS 50-level book
        const auto n = altair::encode_price(p, lv, lv, buf, sizeof buf);
        check(n.has_value() && *n == altair::price_frame_bytes(50) && *n == 2448,
              "a fifty-level book encodes to 48 + 2*50*24 = 2448 bytes");
        check(n.has_value() && altair::decode_price(buf, *n).has_value()
                  && altair::decode_price(buf, *n)->payload.depth_levels == 50,
              "and decodes with all fifty levels");
    }
    {
        altair::PricePayload p;
        p.flags = altair::kPriceHasBook;
        p.depth_levels = 2;
        altair::PriceLevel lv[2]{};
        std::uint8_t buf[1024]{};
        const auto n = altair::encode_price(p, lv, lv, buf, sizeof buf);
        check(n.has_value(), "a two-level book encodes");
        // PLANT: corrupt the length on the wire to a value that would read
        // past the buffer.
        buf[6] = 0xFF;
        buf[7] = 0xFF;
        check(!altair::decode_price(buf, *n).has_value(),
              "a depth count read off the wire that exceeds the format is "
              "refused rather than believed");
    }
    {
        // A flag and a count that disagree, both ways round.
        altair::PricePayload p;
        p.flags = altair::kPriceHasBook;
        p.depth_levels = 1;
        altair::PriceLevel lv[1]{};
        std::uint8_t buf[256]{};
        const auto n = altair::encode_price(p, lv, lv, buf, sizeof buf);
        check(n.has_value(), "a one-level book encodes");
        buf[4] = 0x00; buf[5] = 0x00;     // clear the flags, keep the count
        check(!altair::decode_price(buf, *n).has_value(),
              "a depth count with the book flag clear is a contradiction and "
              "is refused");
    }
    {
        altair::PricePayload p;
        p.token = 1;
        std::uint8_t small[16]{};
        check(!altair::encode_price(p, nullptr, nullptr, small,
                                    sizeof small).has_value(),
              "encoding into a buffer that cannot hold the payload is refused");
        check(!altair::decode_price(small, sizeof small).has_value(),
              "and decoding fewer bytes than a payload is refused");
        check(!altair::decode_price(nullptr, 100).has_value(),
              "a null buffer is refused rather than dereferenced");
    }
    {
        // A book claimed but no levels supplied.
        altair::PricePayload p;
        p.flags = altair::kPriceHasBook;
        p.depth_levels = 3;
        std::uint8_t buf[512]{};
        check(!altair::encode_price(p, nullptr, nullptr, buf,
                                    sizeof buf).has_value(),
              "claiming a book while passing no levels is refused");
    }

    std::printf("\n%s\n", failures == 0 ? "all checks passed"
                                        : "THERE WERE FAILURES");
    return failures == 0 ? 0 : 1;
}
