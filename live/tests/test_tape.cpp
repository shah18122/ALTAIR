// The session tape and the fingerprint: records read back exactly as written,
// a tape cut short reads up to its last whole record and says how much it
// lost, a record of no known kind stops the reader rather than being guessed
// at, the start record's sections survive any bytes, and the digest is FNV-1a.
//
// No check description here may contain the substring "F" "AIL" joined.

#include <live/fingerprint.hpp>
#include <live/tape.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) { ++failures; }
}

using namespace altair::live;
namespace fs = std::filesystem;

std::uintmax_t size_of(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0 : n;
}

} // namespace

int main() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / ("altair_tape_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                                                      + "_" + std::to_string(std::random_device{}()));
    fs::create_directories(dir, ec);
    const fs::path path = dir / "t.tape";

    // ---- every kind, read back exactly ----------------------------------------
    std::vector<std::uint8_t> blob(300000);
    for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::uint8_t>(i * 131 + 7);
    {
        TapeWriter w(path.string());
        check(w.ok(), "a tape opens for writing");
        w.write(TapeKind::Start, 1, tape_pack({{"args", "a=1\nb=2\n"}, {"empty", ""}}));
        w.write(TapeKind::Connected, 2);
        w.write(TapeKind::Data, 3, blob.data(), blob.size());
        w.write(TapeKind::Stale, 4, "1");
        w.write(TapeKind::Kill, 5, "0");
        w.write(TapeKind::Halt, 6, "cannot write journal.csv");
        w.write(TapeKind::Watchdog, -7);
        w.write(TapeKind::Disconnected, 8, "End of file");
        check(w.flush() && w.bytes_written() + std::string(kTapeMagic).size() == size_of(path),
              "every byte reaches the file, and the count says how many");
    }
    {
        TapeReader r(path.string());
        TapeRecord rec;
        std::vector<TapeKind> kinds;
        std::vector<std::int64_t> walls;
        bool data_ok = false, text_ok = true;
        TapeSections start;
        while (r.next(rec)) {
            kinds.push_back(rec.kind);
            walls.push_back(rec.wall_ns);
            if (rec.kind == TapeKind::Data) data_ok = rec.bytes == blob;
            if (rec.kind == TapeKind::Start) text_ok = text_ok && tape_unpack(rec.text(), start);
            if (rec.kind == TapeKind::Halt) text_ok = text_ok && rec.text() == "cannot write journal.csv";
            if (rec.kind == TapeKind::Connected) text_ok = text_ok && rec.bytes.empty();
        }
        check(r.ok() && r.records() == 8 && r.truncated() == 0, "eight records, none cut short");
        check(kinds == std::vector<TapeKind>{TapeKind::Start, TapeKind::Connected, TapeKind::Data, TapeKind::Stale, TapeKind::Kill,
                                             TapeKind::Halt, TapeKind::Watchdog, TapeKind::Disconnected},
              "in the order written");
        check(walls == std::vector<std::int64_t>{1, 2, 3, 4, 5, 6, -7, 8}, "each with its own wall time (sign kept)");
        check(data_ok, "a 300 kB chunk of the bus comes back byte for byte");
        check(text_ok && start.size() == 2 && tape_section(start, "args") != nullptr && *tape_section(start, "args") == "a=1\nb=2\n"
                  && tape_section(start, "empty") != nullptr && tape_section(start, "empty")->empty()
                  && tape_section(start, "missing") == nullptr,
              "the start record's sections, an empty one included, and text records");
    }

    // ---- a tape cut short (a crash mid-write) --------------------------------------
    {
        const auto full = size_of(path);
        fs::resize_file(path, full - 5, ec);   // into the last record's text
        TapeReader r(path.string());
        TapeRecord rec;
        std::uint64_t n = 0;
        while (r.next(rec)) ++n;
        check(n == 7 && r.truncated() == 13 + std::string("End of file").size() - 5,
              "a tape cut inside its last record reads the seven before it and counts the bytes lost");
        fs::resize_file(path, full - 5 - (13 + 11 - 5) - 6, ec);   // now inside the watchdog record's header
        TapeReader r2(path.string());
        n = 0;
        while (r2.next(rec)) ++n;
        check(n == 6 && r2.truncated() == 7, "and one cut inside a record's header reads the six before it");
    }

    // ---- not a tape; a record of no known kind --------------------------------------
    {
        { std::ofstream(dir / "plain.txt") << "time,ns,model\n"; }
        check(!TapeReader((dir / "plain.txt").string()).ok(), "a file without the magic line is not a tape");
        check(!TapeReader((dir / "absent.tape").string()).ok(), "nor is a file that is not there");
        {
            TapeWriter w((dir / "odd.tape").string());
            w.write(TapeKind::Connected, 1);
            w.write(static_cast<TapeKind>(42), 2, "?");
            w.write(TapeKind::Disconnected, 3);
        }
        TapeReader r((dir / "odd.tape").string());
        TapeRecord rec;
        std::uint64_t n = 0;
        while (r.next(rec)) ++n;
        check(n == 1 && !r.ok(), "a record of no known kind stops the reader there: nothing after it is guessed at");
    }

    // ---- sections with any bytes; malformed payloads -------------------------------
    {
        std::string binary;
        for (int i = 0; i < 256; ++i) binary.push_back(static_cast<char>(i));
        binary += "\n12\nnot a header";
        const TapeSections in{{"bundle", "{\"a\": 1}\n"}, {"bin", binary}, {"journal.csv", "time,model\n1,\"x\"\n"}};
        TapeSections out;
        check(tape_unpack(tape_pack(in), out) && out == in, "sections round-trip whatever bytes they hold, newlines included");
        check(tape_unpack("", out) && out.empty(), "an empty payload is no sections");
        check(!tape_unpack("args\n10\nshort", out), "a section longer than the payload is refused");
        check(!tape_unpack("args\nten\n0123456789", out), "a length that is not a number is refused");
        check(!tape_unpack("args\n", out), "a name with no length is refused");
    }

    // ---- the fingerprint is FNV-1a 64 -------------------------------------------------
    {
        check(Fingerprint{}.hex() == "cbf29ce484222325", "the empty digest is the FNV offset basis");
        check(Fingerprint{}.bytes("a", 1).hex() == "af63dc4c8601ec8c", "FNV-1a of \"a\"");
        check(Fingerprint{}.bytes("foobar", 6).hex() == "85944171f73967e8", "FNV-1a of \"foobar\"");
        check(Fingerprint{}.text("ab").text("c").value() != Fingerprint{}.text("a").text("bc").value(),
              "texts are separated: \"ab\",\"c\" is not \"a\",\"bc\"");
        check(Fingerprint{}.f64(0.0).value() != Fingerprint{}.f64(-0.0).value(), "doubles by bit pattern: 0 and -0 differ");
        check(Fingerprint{}.f64(0.1 + 0.2).value() != Fingerprint{}.f64(0.3).value(), "and so do 0.1+0.2 and 0.3");
    }

    fs::remove_all(dir, ec);
    std::printf("%s\n", failures == 0 ? "all tape checks passed" : "tape checks did not pass");
    return failures == 0 ? 0 : 1;
}
