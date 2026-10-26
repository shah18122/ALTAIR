// The news bar's parser: RSS 2.0, Atom, the feed list, merge and the JSON
// the ticker reads back. No network.

#include "../news/rss.hpp"

#include <cstdio>

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

} // namespace

int main()
{
    using namespace altair::ui::news;

    const QByteArray rss = R"(<?xml version="1.0"?>
<rss version="2.0"><channel><title>ET Markets</title>
<item><title>Nifty ends higher</title><link>https://example.com/a</link>
<pubDate>Fri, 24 Oct 2026 10:00:00 +0530</pubDate></item>
<item><title><![CDATA[Sensex  slips   on  IT]]></title><link>javascript:alert(1)</link>
<pubDate>Fri, 24 Oct 2026 11:00:00 +0530</pubDate></item>
<item><link>https://example.com/no-title</link></item>
</channel></rss>)";
    const auto items = parse_feed(rss);
    check(items.size() == 2, "RSS: two titled items, the untitled one dropped");
    check(items.size() == 2 && items[0].title == QStringLiteral("Nifty ends higher")
              && items[0].link == QStringLiteral("https://example.com/a") && items[0].source == QStringLiteral("ET Markets"),
          "RSS: title, link and the channel as source");
    check(items.size() == 2 && items[1].title == QStringLiteral("Sensex slips on IT"), "RSS: CDATA read, whitespace collapsed");
    check(items.size() == 2 && items[1].link.isEmpty(), "RSS: a non-web link is never kept");
    check(items.size() == 2 && items[0].ts > 0 && items[1].ts - items[0].ts == 3600, "RSS: RFC 2822 dates parsed");

    const QByteArray atom = R"(<?xml version="1.0"?>
<feed xmlns="http://www.w3.org/2005/Atom"><title>Mint</title>
<entry><title>Bank Nifty record</title><link rel="alternate" href="https://example.com/b"/>
<updated>2026-10-24T12:00:00+05:30</updated></entry></feed>)";
    const auto a = parse_feed(atom, QStringLiteral("Mint Markets"));
    check(a.size() == 1 && a[0].link == QStringLiteral("https://example.com/b") && a[0].ts > 0
              && a[0].source == QStringLiteral("Mint Markets"),
          "Atom: href link, ISO date, configured name wins");

    check(parse_feed(QByteArray("<rss><channel><item><title>half")).empty(), "malformed XML yields nothing, no crash");

    std::vector<NewsItem> both = items;
    both.insert(both.end(), a.begin(), a.end());
    both.push_back({QStringLiteral("NIFTY ENDS HIGHER"), {}, QStringLiteral("Dup"), 1});
    const auto merged = merge(both);
    check(merged.size() == 3 && merged[0].title == QStringLiteral("Bank Nifty record"),
          "merge: newest first, the same headline once");

    const auto back = from_json(to_json(merged));
    check(back.size() == merged.size() && back[0].title == merged[0].title && back[0].ts == merged[0].ts,
          "JSON round trip");
    check(from_json(QByteArray("not json")).empty(), "bad JSON yields nothing");

    const auto feeds = parse_feed_list(QStringLiteral("# comment\nET|https://x.example/rss\n\nhttps://y.example/feed\nftp://z/feed\n"));
    check(feeds.size() == 2 && feeds[0].name == QStringLiteral("ET") && feeds[1].name == QStringLiteral("y.example"),
          "feed list: comments skipped, name defaults to the host, non-web URLs refused");
    check(!default_feeds().empty(), "there are default feeds");

    std::printf("%s\n", failures == 0 ? "all news checks passed" : "news checks did not pass");
    return failures == 0 ? 0 : 1;
}
