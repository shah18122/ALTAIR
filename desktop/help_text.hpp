// desktop/help_text.hpp -- pages show data, not paragraphs.
//
// Smit (2026-11-09): "there's a lot of description in your whole UI". Every
// page used to open with a paragraph or two on what it is and how it works.
// This keeps those paragraphs out of the way on EVERY page without rewriting
// each one: the shell sweeps the page on screen, hides each long block of
// prose, and the "? Help" button in the status bar shows them on request.
//
// What counts as help text: a visible word-wrapped QLabel whose plain text is
// at least kMinHelpChars long. Kept on screen whatever its length:
//   * monospace blocks (reports, inventories: data, not prose);
//   * labels with role error / warn / ok / pill-* and status lines with a ●;
//   * any label with the property altairKeep = true.
// A hidden label whose text later CHANGES is a status after all (the page
// reused it to say something new), so it comes back and is never hidden again.
#pragma once

#include <QLabel>
#include <QPointer>
#include <QRegularExpression>
#include <QStringList>
#include <QTextDocumentFragment>
#include <QWidget>

namespace altair::ui::help_text {

inline constexpr int kMinHelpChars = 110;

[[nodiscard]] inline QString plain(const QLabel* l) {
    return l->textFormat() == Qt::PlainText ? l->text()
                                            : QTextDocumentFragment::fromHtml(l->text()).toPlainText();
}

/// Is this label a paragraph of explanation (hide it) or something to read now?
[[nodiscard]] inline bool is_help(const QLabel* l) {
    if (!l->wordWrap() || l->property("altairKeep").toBool()) return false;
    const QString role = l->property("role").toString();
    if (role == QLatin1String("error") || role == QLatin1String("warn") || role == QLatin1String("ok")
        || role.startsWith(QLatin1String("pill")) || role == QLatin1String("title") || role == QLatin1String("kicker"))
        return false;
    const QString style = l->styleSheet() + l->text().left(400);
    if (style.contains(QLatin1String("monospace"), Qt::CaseInsensitive)
        || style.contains(QLatin1String("Consolas"), Qt::CaseInsensitive))
        return false;
    if (l->text().contains(QLatin1String("<table"), Qt::CaseInsensitive)) return false;   // a table is data
    const QString text = plain(l).simplified();
    if (text.contains(QChar(0x25CF))) return false;   // ● status lines
    return text.size() >= kMinHelpChars;
}

/// Hide the help paragraphs under `page`; bring back any that turned into a
/// status. Cheap enough to run every second on the page on screen.
inline void sweep(QWidget* page) {
    if (page == nullptr) return;
    for (QLabel* l : page->findChildren<QLabel*>()) {
        const QVariant held = l->property("altairHelp");
        if (held.isValid()) {
            const QVariant shown = l->property("altairShown");
            if (l->text() != (shown.isValid() ? shown.toString() : held.toString())) {
                // reused for a status: show it, for good
                l->setProperty("altairHelp", QVariant());
                l->setProperty("altairShown", QVariant());
                l->setProperty("altairKeep", true);
                l->setVisible(true);
            }
            continue;
        }
        if (l->isHidden() || l->text().isEmpty()) continue;
        // A label already judged "not help" with the same text is skipped:
        // is_help() parses HTML, and a page has hundreds of status labels.
        // Re-parsing them all every second was a visible hitch over DCV.
        const QVariant seen = l->property("altairNotHelp");
        if (seen.isValid() && seen.toString() == l->text()) continue;
        if (!is_help(l)) { l->setProperty("altairNotHelp", l->text()); continue; }
        l->setProperty("altairHelp", l->text());
        // A paragraph under its own heading keeps the heading.
        static const QRegularExpression head(QStringLiteral("^\\s*(?:<[^h>][^>]*>\\s*)*(<h[1-4][^>]*>.*?</h[1-4]>)"),
                                             QRegularExpression::CaseInsensitiveOption
                                                 | QRegularExpression::DotMatchesEverythingOption);
        const auto m = head.match(l->text());
        if (m.hasMatch()) {
            l->setText(m.captured(1));
            l->setProperty("altairShown", l->text());
        } else {
            l->setVisible(false);
        }
    }
}

/// The help paragraphs hidden on `page`, as one HTML document.
[[nodiscard]] inline QString collected(QWidget* page) {
    QStringList parts;
    if (page != nullptr)
        for (QLabel* l : page->findChildren<QLabel*>()) {
            const QVariant held = l->property("altairHelp");
            if (!held.isValid()) continue;
            QString html = held.toString();
            if (l->textFormat() == Qt::PlainText || (l->textFormat() == Qt::AutoText && !Qt::mightBeRichText(html)))
                html = html.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
            if (!parts.contains(html)) parts << html;
        }
    return parts.join(QStringLiteral("<hr>"));
}

} // namespace altair::ui::help_text
