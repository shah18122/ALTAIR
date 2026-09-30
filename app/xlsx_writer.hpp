// app/xlsx_writer.hpp -- a minimal, dependency-free .xlsx writer.
//
// The data audit (app/data_audit_main.cpp) produces a workbook a person opens
// in Excel: one sheet per instrument plus a summary. The system has no Python
// and no zip library, so this writes the Office Open XML package directly:
// a ZIP archive of STORED (uncompressed) entries, which every spreadsheet
// program reads, holding inline-string cells, one bold style, frozen header
// rows and column widths. Nothing else: no formulas, no shared strings, no
// charts.
//
// RULE 11: ZIP32 only. A package over 4 GiB, or more than 65535 entries, is
// refused rather than written as a corrupt archive.
#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace altair::xlsx {

struct XlsxCell {
    enum class Kind : std::uint8_t { Empty, Text, Number };
    Kind kind{Kind::Empty};
    std::string text;
    double number{};
    bool bold{};

    [[nodiscard]] static XlsxCell str(std::string t, bool b = false) {
        XlsxCell c;
        c.kind = Kind::Text;
        c.text = std::move(t);
        c.bold = b;
        return c;
    }
    [[nodiscard]] static XlsxCell num(double v, bool b = false) {
        XlsxCell c;
        c.kind = std::isfinite(v) ? Kind::Number : Kind::Text;
        c.number = v;
        if (!std::isfinite(v)) c.text = "—";
        c.bold = b;
        return c;
    }
};

using XlsxRow = std::vector<XlsxCell>;

struct XlsxSheet {
    std::string name;                 ///< 1..31 chars, none of []:*?/\ (checked)
    std::vector<XlsxRow> rows;
    std::vector<double> widths;       ///< per column, in characters; 0 = default
    int freeze_rows{};                ///< rows kept visible when scrolling
};

enum class XlsxError : std::uint8_t { BadSheetName, DuplicateSheetName, NoSheets, TooLarge };

namespace xlsx_detail {

[[nodiscard]] inline std::uint32_t crc32(std::string_view data) noexcept {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFu;
    for (const char ch : data) c = table[(c ^ static_cast<std::uint8_t>(ch)) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline void put16(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
}
inline void put32(std::string& out, std::uint32_t v) {
    put16(out, v & 0xFFFFu);
    put16(out, (v >> 16) & 0xFFFFu);
}

/// XML text: escape markup and drop characters XML 1.0 forbids.
inline void escape(std::string& out, std::string_view text) {
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default:
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
            out.push_back(ch);
        }
    }
}

[[nodiscard]] inline std::string column_name(std::size_t index) {
    std::string name;
    std::size_t n = index + 1;
    while (n > 0) {
        const std::size_t r = (n - 1) % 26;
        name.insert(name.begin(), static_cast<char>('A' + r));
        n = (n - 1) / 26;
    }
    return name;
}

[[nodiscard]] inline bool valid_sheet_name(std::string_view n) {
    if (n.empty() || n.size() > 31 || n.front() == '\'' || n.back() == '\'') return false;
    for (const char c : n)
        if (c == '[' || c == ']' || c == ':' || c == '*' || c == '?' || c == '/' || c == '\\')
            return false;
    return true;
}

[[nodiscard]] inline std::string number_text(double v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v);
    return r.ec == std::errc{} ? std::string{buf, r.ptr} : std::string{"0"};
}

[[nodiscard]] inline std::string sheet_xml(const XlsxSheet& s) {
    std::string x;
    x.reserve(64 + s.rows.size() * 96);
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
         "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
         "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">";
    if (s.freeze_rows > 0) {
        const std::string top = "A" + std::to_string(s.freeze_rows + 1);
        x += "<sheetViews><sheetView workbookViewId=\"0\"><pane ySplit=\"" + std::to_string(s.freeze_rows)
           + "\" topLeftCell=\"" + top + "\" activePane=\"bottomLeft\" state=\"frozen\"/>"
             "</sheetView></sheetViews>";
    }
    bool any_width = false;
    for (const double w : s.widths) any_width = any_width || w > 0.0;
    if (any_width) {
        x += "<cols>";
        for (std::size_t i = 0; i < s.widths.size(); ++i) {
            if (!(s.widths[i] > 0.0)) continue;
            const std::string idx = std::to_string(i + 1);
            x += "<col min=\"" + idx + "\" max=\"" + idx + "\" width=\"" + number_text(s.widths[i])
               + "\" customWidth=\"1\"/>";
        }
        x += "</cols>";
    }
    x += "<sheetData>";
    for (std::size_t r = 0; r < s.rows.size(); ++r) {
        const std::string rn = std::to_string(r + 1);
        x += "<row r=\"" + rn + "\">";
        for (std::size_t c = 0; c < s.rows[r].size(); ++c) {
            const XlsxCell& cell = s.rows[r][c];
            if (cell.kind == XlsxCell::Kind::Empty) continue;
            const std::string ref = column_name(c) + rn;
            x += "<c r=\"" + ref + "\"";
            if (cell.bold) x += " s=\"1\"";
            if (cell.kind == XlsxCell::Kind::Number) {
                x += "><v>" + number_text(cell.number) + "</v></c>";
            } else {
                x += " t=\"inlineStr\"><is><t xml:space=\"preserve\">";
                escape(x, cell.text);
                x += "</t></is></c>";
            }
        }
        x += "</row>";
    }
    x += "</sheetData></worksheet>";
    return x;
}

struct ZipEntry {
    std::string name;
    std::string data;
};

[[nodiscard]] inline std::expected<std::string, XlsxError> zip_store(const std::vector<ZipEntry>& entries) {
    if (entries.size() > 0xFFFFu) return std::unexpected(XlsxError::TooLarge);
    std::string out;
    std::string central;
    std::uint64_t offset = 0;
    constexpr std::uint32_t kDosDate = (0u << 9) | (1u << 5) | 1u;   // 1980-01-01
    for (const auto& e : entries) {
        if (e.data.size() > 0xFFFFFFFEull || offset > 0xFFFFFFFEull)
            return std::unexpected(XlsxError::TooLarge);
        const std::uint32_t crc = crc32(e.data);
        const auto size = static_cast<std::uint32_t>(e.data.size());
        const auto at = static_cast<std::uint32_t>(offset);
        std::string local;
        put32(local, 0x04034b50u);
        put16(local, 20);                // version needed
        put16(local, 0x0800u);           // UTF-8 names
        put16(local, 0);                 // stored
        put16(local, 0);                 // time
        put16(local, kDosDate);
        put32(local, crc);
        put32(local, size);
        put32(local, size);
        put16(local, static_cast<std::uint32_t>(e.name.size()));
        put16(local, 0);
        local += e.name;
        out += local;
        out += e.data;
        offset += local.size() + e.data.size();

        put32(central, 0x02014b50u);
        put16(central, 20);              // made by
        put16(central, 20);              // needed
        put16(central, 0x0800u);
        put16(central, 0);
        put16(central, 0);
        put16(central, kDosDate);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, static_cast<std::uint32_t>(e.name.size()));
        put16(central, 0);               // extra
        put16(central, 0);               // comment
        put16(central, 0);               // disk
        put16(central, 0);               // internal attributes
        put32(central, 0);               // external attributes
        put32(central, at);
        central += e.name;
    }
    if (offset + central.size() > 0xFFFFFFFEull) return std::unexpected(XlsxError::TooLarge);
    const auto cd_offset = static_cast<std::uint32_t>(offset);
    out += central;
    put32(out, 0x06054b50u);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put16(out, static_cast<std::uint32_t>(entries.size()));
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, cd_offset);
    put16(out, 0);
    return out;
}

} // namespace xlsx_detail

/// The whole .xlsx package as bytes.
[[nodiscard]] inline std::expected<std::string, XlsxError> build_workbook(const std::vector<XlsxSheet>& sheets) {
    namespace d = xlsx_detail;
    if (sheets.empty()) return std::unexpected(XlsxError::NoSheets);
    for (std::size_t i = 0; i < sheets.size(); ++i) {
        if (!d::valid_sheet_name(sheets[i].name)) return std::unexpected(XlsxError::BadSheetName);
        for (std::size_t j = 0; j < i; ++j)
            if (sheets[j].name == sheets[i].name) return std::unexpected(XlsxError::DuplicateSheetName);
    }
    std::vector<d::ZipEntry> entries;
    std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>";
    for (std::size_t i = 0; i < sheets.size(); ++i)
        types += "<Override PartName=\"/xl/worksheets/sheet" + std::to_string(i + 1)
               + ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    types += "</Types>";
    entries.push_back({"[Content_Types].xml", types});
    entries.push_back({"_rels/.rels",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>"});
    std::string workbook =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets>";
    std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (std::size_t i = 0; i < sheets.size(); ++i) {
        const std::string n = std::to_string(i + 1);
        workbook += "<sheet name=\"";
        d::escape(workbook, sheets[i].name);
        workbook += "\" sheetId=\"" + n + "\" r:id=\"rId" + n + "\"/>";
        rels += "<Relationship Id=\"rId" + n
              + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet"
              + n + ".xml\"/>";
    }
    const std::string styles_id = std::to_string(sheets.size() + 1);
    workbook += "</sheets></workbook>";
    rels += "<Relationship Id=\"rId" + styles_id
          + "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
            "</Relationships>";
    entries.push_back({"xl/workbook.xml", workbook});
    entries.push_back({"xl/_rels/workbook.xml.rels", rels});
    entries.push_back({"xl/styles.xml",
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
        "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
        "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
        "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
        "</styleSheet>"});
    for (std::size_t i = 0; i < sheets.size(); ++i)
        entries.push_back({"xl/worksheets/sheet" + std::to_string(i + 1) + ".xml", d::sheet_xml(sheets[i])});
    return d::zip_store(entries);
}

} // namespace altair::xlsx
