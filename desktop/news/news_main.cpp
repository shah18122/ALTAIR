// altair_news -- fetch the market news feeds once and print the headlines.
//
// The news ticker runs this every few minutes and reads its stdout. It is a
// separate process ON PURPOSE: the desktop process opens sockets to
// 127.0.0.1 only and has no TLS client (desktop/CMakeLists.txt, the
// Qt6::Network paragraph). Fetching https feeds inside the window would spend
// that property; a helper that prints JSON keeps it.
//
// Usage: altair_news [--feeds config/news_feeds.txt] [--timeout-ms 15000]
// Output: one JSON array of {title, link, source, ts}, newest first.
// Exit 0 when at least one feed answered, 2 when none did.

#include "rss.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace altair::ui::news;
    QString feeds_path;
    int timeout_ms = 15000;
    const QStringList args = app.arguments();
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == QLatin1String("--feeds")) feeds_path = args[++i];
        else if (args[i] == QLatin1String("--timeout-ms")) timeout_ms = std::max(1000, args[++i].toInt());
    }
    std::vector<Feed> feeds;
    if (!feeds_path.isEmpty()) {
        QFile f(feeds_path);
        if (f.open(QIODevice::ReadOnly)) feeds = parse_feed_list(QString::fromUtf8(f.readAll()));
    }
    if (feeds.empty()) feeds = default_feeds();

    QNetworkAccessManager net;
    std::vector<NewsItem> all;
    int pending = static_cast<int>(feeds.size());
    int answered = 0;
    for (const Feed& feed : feeds) {
        QNetworkRequest req{QUrl(feed.url)};
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (ALTAIR news ticker)"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        req.setTransferTimeout(timeout_ms);
        QNetworkReply* reply = net.get(req);
        QObject::connect(reply, &QNetworkReply::finished, &app, [&, reply, name = feed.name] {
            if (reply->error() == QNetworkReply::NoError) {
                auto items = parse_feed(reply->read(4 * 1024 * 1024), name);
                if (!items.empty()) ++answered;
                for (auto& i : items) { i.source = name; all.push_back(std::move(i)); }
            } else {
                std::fprintf(stderr, "%s: %s\n", qPrintable(name), qPrintable(reply->errorString()));
            }
            reply->deleteLater();
            if (--pending == 0) app.quit();
        });
    }
    // A hard stop in case a reply never finishes.
    QTimer::singleShot(timeout_ms + 5000, &app, &QCoreApplication::quit);
    app.exec();
    const QByteArray json = to_json(merge(std::move(all)));
    std::fwrite(json.constData(), 1, static_cast<std::size_t>(json.size()), stdout);
    std::fputc('\n', stdout);
    return answered > 0 ? 0 : 2;
}
