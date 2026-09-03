// P11-01b -- emit the protocol conformance vectors.
//
// server/ and client/ share a wire protocol and nothing else. The C++ encoder
// here is the reference implementation; the TypeScript decoder in client/ is a
// second implementation of the same bytes, and nothing but a shared test file
// keeps them honest. Reading a checked-in data file is not #including a
// header: the client still cannot link an engine header, and still cannot be
// made to trade.
//
// EVERY VECTOR EXISTS TO CATCH A SPECIFIC DECODER BUG. They are not a random
// sample of frames. A decoder can pass a round-trip test against itself and
// still be wrong in all four of the ways below, because each one is a place
// where JavaScript's defaults silently differ from C++'s:
//
//   js_number_loss   an engine timestamp whose low digits a double discards.
//                    Decode to Number and the value comes back changed.
//
//   negative_i64     a losing P&L. Two's complement little-endian. A decoder
//                    that ORs bytes together without sign-extending returns a
//                    very large POSITIVE number -- a loss displayed as a gain.
//
//   high_bit_u32     topic and payload_len with bit 31 set. JavaScript's
//                    bitwise operators are 32-bit SIGNED: `b[3] << 24` is
//                    negative for any byte >= 0x80, so the idiomatic
//                    `a | b << 8 | c << 16 | d << 24` decoder returns a
//                    negative length. This is the single most common bug in
//                    hand-written JS binary parsers.
//
//   seq_near_u64_max a sequence number above 2^53. Number cannot hold it, and
//                    the gap detector compares sequence numbers -- so a
//                    decoder that loses precision here stops detecting gaps
//                    at exactly the point where the stream is longest.
//
// Plus three frames that must be REFUSED: a zeroed buffer, a wrong version,
// and an ordinal-zero frame kind.
//
// THE FILE FORMAT CANNOT USE JSON NUMBERS. Every 64-bit value is written as a
// decimal STRING. A vector file that stored `1788393600123456789` as a JSON
// number would be corrupted by the very bug the vectors exist to catch, at the
// moment the client parsed it -- and every decoder would then agree with the
// broken expectation.

#include <server/protocol.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifndef ALTAIR_VECTOR_DIR
#  define ALTAIR_VECTOR_DIR "."
#endif

using namespace altair;

namespace {

int written = 0;

/// std::fopen, portably. MSVC deprecates fopen and warns at /W4, which gate 1
/// treats as a failure. Same shape as app/session_file.hpp -- handled rather
/// than suppressed with _CRT_SECURE_NO_WARNINGS, because the suppression
/// would apply to the whole translation unit.
[[nodiscard]] std::FILE* xfopen(const char* path, const char* mode) noexcept
{
#if defined(_MSC_VER)
    std::FILE* f = nullptr;
    if (::fopen_s(&f, path, mode) != 0) { return nullptr; }
    return f;
#else
    return std::fopen(path, mode);
#endif
}

/// One hex digit, or -1. Explicit rather than sscanf: the parse is four lines
/// and this way there is no Annex K variant to reach for.
[[nodiscard]] int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

void emit(std::FILE* f, const char* name, const FrameHeader& h)
{
    std::uint8_t buf[kFrameHeaderBytes] = {};
    const auto w = encode_header(h, buf, sizeof buf);
    if (!w) {
        std::printf("  could not encode %s\n", name);
        return;
    }
    std::fprintf(f, "%s|", name);
    for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
        std::fprintf(f, "%02x", buf[i]);
    }
    // accept=1 means a conforming decoder must accept and produce these
    // fields. Every 64-bit value is a decimal string, deliberately.
    std::fprintf(f,
                 "|accept=1;kind=%u;channel=%u;seq=%llu;topic=%u;"
                 "payload_len=%u;engine_time_ns=%lld;server_time_ns=%lld\n",
                 static_cast<unsigned>(h.kind),
                 static_cast<unsigned>(h.channel),
                 static_cast<unsigned long long>(h.seq),
                 h.topic, h.payload_len,
                 static_cast<long long>(h.engine_time_ns),
                 static_cast<long long>(h.server_time_ns));
    ++written;
}

void emit_raw(std::FILE* f, const char* name, const std::uint8_t* b,
              const char* reason)
{
    std::fprintf(f, "%s|", name);
    for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
        std::fprintf(f, "%02x", b[i]);
    }
    std::fprintf(f, "|accept=0;reason=%s\n", reason);
    ++written;
}

FrameHeader base()
{
    FrameHeader h{};
    h.kind = FrameKind::Delta;
    h.channel = Channel::State;
    h.seq = 1;
    h.topic = 1;
    h.payload_len = 0;
    h.engine_time_ns = 1788393600000000000LL;
    h.server_time_ns = 1788393600000000000LL;
    return h;
}

} // namespace

int main()
{
    const char* path = ALTAIR_VECTOR_DIR "/frames.txt";
    std::FILE* f = xfopen(path, "wb");
    if (f == nullptr) {
        std::printf("could not open %s for writing\n", path);
        return 1;
    }

    std::fprintf(f,
        "# Altair wire protocol conformance vectors -- wire version %u.\n"
        "# GENERATED by server/tests/emit_vectors.cpp. Do not hand-edit.\n"
        "#\n"
        "# name|hex bytes of the 48-byte frame header|expectations\n"
        "#\n"
        "# Every 64-bit value is a DECIMAL STRING. Storing one as a JSON\n"
        "# number would corrupt this file with the bug it exists to catch.\n",
        static_cast<unsigned>(kWireVersion));

    // 1. A timestamp a double cannot hold. The low digits are the test.
    {
        FrameHeader h = base();
        h.engine_time_ns = 1788393600123456789LL;
        h.server_time_ns = 1788393600123456790LL;   // one ns later
        emit(f, "js_number_loss", h);
    }

    // 2. A negative int64. A losing session, and the sign-extension test.
    {
        FrameHeader h = base();
        h.kind = FrameKind::Snapshot;
        h.channel = Channel::Event;
        h.engine_time_ns = -1LL;                    // all bytes 0xff
        h.server_time_ns = -4207500000000LL;        // a real pre-epoch instant
        emit(f, "negative_i64", h);
    }

    // 3. u32 fields with bit 31 set. The 32-bit-signed-shift trap.
    {
        FrameHeader h = base();
        h.topic = 0x80000001u;
        h.payload_len = 0xFFFFFFFEu;
        emit(f, "high_bit_u32", h);
    }

    // 4. A sequence number beyond 2^53, where a Number stops counting.
    {
        FrameHeader h = base();
        h.seq = 18446744073709551615ULL;            // 2^64 - 1
        emit(f, "seq_u64_max", h);
    }
    {
        FrameHeader h = base();
        h.seq = 9007199254740993ULL;                // 2^53 + 1
        emit(f, "seq_above_2_53", h);
    }

    // 5. Three frames that must be refused.
    {
        std::uint8_t zeros[kFrameHeaderBytes] = {};
        emit_raw(f, "all_zero", zeros, "BadMagic");
    }
    {
        std::uint8_t b[kFrameHeaderBytes] = {};
        (void)encode_header(base(), b, sizeof b);
        b[4] = static_cast<std::uint8_t>(kWireVersion + 1);
        emit_raw(f, "wrong_version", b, "VersionMismatch");
    }
    {
        std::uint8_t b[kFrameHeaderBytes] = {};
        (void)encode_header(base(), b, sizeof b);
        b[6] = 0;                                   // FrameKind::Unspecified
        emit_raw(f, "zero_frame_kind", b, "Unspecified");
    }

    std::fclose(f);
    std::printf("P11-01b -- wrote %d conformance vectors to %s\n",
                written, path);

    // Re-read and re-decode every accepted vector, so a broken emitter is
    // caught here rather than in client/ where it would look like a decoder
    // bug. The reference implementation has to agree with its own output.
    std::FILE* r = xfopen(path, "rb");
    if (r == nullptr) { return 1; }
    char line[1024];
    int checked = 0, bad = 0;
    while (std::fgets(line, sizeof line, r) != nullptr) {
        if (line[0] == '#' || line[0] == '\n') { continue; }
        const char* bar = std::strchr(line, '|');
        if (bar == nullptr) { continue; }
        const char* hex = bar + 1;
        std::uint8_t buf[kFrameHeaderBytes] = {};
        for (std::size_t i = 0; i < kFrameHeaderBytes; ++i) {
            const int hi = hex_digit(hex[2 * i]);
            const int lo = hex_digit(hex[2 * i + 1]);
            buf[i] = (hi < 0 || lo < 0)
                   ? 0 : static_cast<std::uint8_t>(hi * 16 + lo);
        }
        const bool should_accept = std::strstr(line, "accept=1") != nullptr;
        const auto d = decode_header(buf, sizeof buf);
        if (d.has_value() != should_accept) { ++bad; }
        ++checked;
    }
    std::fclose(r);
    std::printf("  re-decoded %d vectors, %d disagreed with their own"
                " expectation\n", checked, bad);
    return bad == 0 ? 0 : 1;
}
