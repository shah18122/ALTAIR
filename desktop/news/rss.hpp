// desktop/news/rss.hpp -- headlines out of an RSS 2.0 or Atom document.
//
// Pure: bytes in, items out. No network here; altair_news (news_main.cpp)
// fetches, and the ticker (desktop/news_ticker.hpp) only reads what it
// prints. Headlines are display text and nothing else: nothing in ALTAIR
// trades on them.
#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QXmlStreamReader>

#include <algorithm>
#include <vector>

namespace altair::ui::news {

struct NewsItem {
    QString title;
    QString link;     ///< http(s) only; empty when the feed gave something else
    QString source;   ///< the feed's own name, or the name it was configured with
    qint64 ts = 0;    ///< publication time, Unix seconds; 0 when unknown
};

/// Only web links may be opened from the ticker.
[[nodiscard]] inline QString safe_link(const QString& raw) {
    const QUrl u(raw.trimmed());
    return u.isValid() && (u.scheme() == QLatin1String("https") || u.scheme() == QLatin1String("http"))
        ? u.toString() : QString{};
}

[[nodiscard]] inline qint64 parse_time(const QString& raw) {
    const QString t = raw.trimmed();
    if (t.isEmpty()) return 0;
    QDateTime d = QDateTime::fromString(t, Qt::RFC2822Date);
    // Some feeds print a weekday that does not match the date; Qt then
    // refuses the whole stamp. The date itself is what matters.
    if (!d.isValid() && t.size() > 5 && t.at(3) == QLatin1Char(','))
        d = QDateTime::fromString(t.mid(5).trimmed(), Qt::RFC2822Date);
    if (!d.isValid()) d = QDateTime::fromString(t, Qt::ISODate);
    if (!d.isValid()) d = QDateTime::fromString(t, Qt::ISODateWithMs);
    return d.isValid() ? d.toSecsSinceEpoch() : 0;
}

/// Headlines in `xml`, in feed order. A malformed document yields what was
/// read before the error. `fallback_source` names items when the feed has
/// no <title> of its own.
[[nodiscard]] inline std::vector<NewsItem> parse_feed(const QByteArray& xml, const QString& fallback_source = {}) {
    std::vector<NewsItem> out;
    QXmlStreamReader r(xml);
    QString channel;
    bool in_item = false;
    NewsItem cur;
    while (!r.atEnd()) {
        r.readNext();
        const auto name = r.name();
        if (r.isStartElement()) {
            if (name == QLatin1String("item") || name == QLatin1String("entry")) {
                in_item = true;
                cur = NewsItem{};
            } else if (name == QLatin1String("title")) {
                const QString text = r.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
                if (in_item) cur.title = text;
                else if (channel.isEmpty()) channel = text;
            } else if (in_item && name == QLatin1String("link")) {
                // RSS: <link>url</link>. Atom: <link href="url"/>.
                const QString href = r.attributes().value(QLatin1String("href")).toString();
                const QString rel = r.attributes().value(QLatin1String("rel")).toString();
                const QString text = r.readElementText();
                if (cur.link.isEmpty() && (rel.isEmpty() || rel == QLatin1String("alternate")))
                    cur.link = safe_link(href.isEmpty() ? text : href);
            } else if (in_item && (name == QLatin1String("pubDate") || name == QLatin1String("published")
                                   || name == QLatin1String("updated") || name == QLatin1String("date"))) {
                const qint64 t = parse_time(r.readElementText());
                if (cur.ts == 0) cur.ts = t;
            }
        } else if (r.isEndElement() && (name == QLatin1String("item") || name == QLatin1String("entry"))) {
            in_item = false;
            if (!cur.title.isEmpty()) out.push_back(cur);
        }
    }
    const QString source = channel.isEmpty() ? fallback_source : channel;
    for (auto& i : out) if (i.source.isEmpty()) i.source = fallback_source.isEmpty() ? source : fallback_source;
    return out;
}

/// Newest first, one line per headline (same title from two feeds = one),
/// at most `limit`.
[[nodiscard]] inline std::vector<NewsItem> merge(std::vector<NewsItem> items, std::size_t limit = 60) {
    std::stable_sort(items.begin(), items.end(), [](const NewsItem& a, const NewsItem& b) { return a.ts > b.ts; });
    QSet<QString> seen;
    std::vector<NewsItem> out;
    for (auto& i : items) {
        const QString key = i.title.toCaseFolded();
        if (seen.contains(key)) continue;
        seen.insert(key);
        out.push_back(std::move(i));
        if (out.size() >= limit) break;
    }
    return out;
}

[[nodiscard]] inline QByteArray to_json(const std::vector<NewsItem>& items) {
    QJsonArray a;
    for (const auto& i : items)
        a.append(QJsonObject{{QStringLiteral("title"), i.title}, {QStringLiteral("link"), i.link},
                             {QStringLiteral("source"), i.source}, {QStringLiteral("ts"), i.ts}});
    return QJsonDocument(a).toJson(QJsonDocument::Compact);
}

[[nodiscard]] inline std::vector<NewsItem> from_json(const QByteArray& json) {
    std::vector<NewsItem> out;
    for (const auto& v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        NewsItem i{o.value(QStringLiteral("title")).toString().simplified(),
                   safe_link(o.value(QStringLiteral("link")).toString()),
                   o.value(QStringLiteral("source")).toString().simplified(),
                   static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble())};
        if (!i.title.isEmpty()) out.push_back(std::move(i));
    }
    return out;
}

/// Free, keyless Indian market feeds. config/news_feeds.txt overrides them:
/// one `Name|https://url` per line, `#` comments.
struct Feed { QString name; QString url; };
[[nodiscard]] inline std::vector<Feed> default_feeds() {
    return {
        {QStringLiteral("ET Markets"), QStringLiteral("https://economictimes.indiatimes.com/markets/rssfeeds/1977021501.cms")},
        {QStringLiteral("Moneycontrol"), QStringLiteral("https://www.moneycontrol.com/rss/marketreports.xml")},
        {QStringLiteral("Mint Markets"), QStringLiteral("https://www.livemint.com/rss/markets")},
        {QStringLiteral("Business Standard"), QStringLiteral("https://www.business-standard.com/rss/markets-106.rss")},
    };
}

[[nodiscard]] inline std::vector<Feed> parse_feed_list(const QString& text) {
    std::vector<Feed> out;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const int bar = line.indexOf(QLatin1Char('|'));
        Feed f{bar > 0 ? line.left(bar).trimmed() : QString{}, safe_link(bar > 0 ? line.mid(bar + 1) : line)};
        if (f.url.isEmpty()) continue;
        if (f.name.isEmpty()) f.name = QUrl(f.url).host();
        out.push_back(std::move(f));
    }
    return out;
}

} // namespace altair::ui::news
