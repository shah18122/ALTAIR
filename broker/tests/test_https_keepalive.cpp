// broker/https_keepalive.hpp against a local HTTPS/1.1 server
// (ops/linux/bypass_https_server.py, self-signed for "localhost"): many
// requests on one connection, Connection: close honoured, the next request
// reconnecting before it writes, and a server that went away detected before
// a request is written. Skipped (passes) without python3 and openssl.

#include <broker/https_keepalive.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}
} // namespace

int main() {
    std::printf("https keep-alive\n");
    if (std::system("command -v python3 >/dev/null && command -v openssl >/dev/null") != 0) {
        std::printf("  skipped: no python3 or openssl\n");
        return 0;
    }
    const std::string dir = "/tmp/altair_keepalive_test_" + std::to_string(::getpid());
    const std::string server = std::string(ALTAIR_SOURCE_DIR) + "/ops/linux/bypass_https_server.py";
    const unsigned short port = static_cast<unsigned short>(20000 + ::getpid() % 20000);
    const std::string setup = "mkdir -p " + dir + " && openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost "
                              "-addext subjectAltName=DNS:localhost -keyout " + dir + "/key.pem -out " + dir + "/cert.pem >/dev/null 2>&1";
    if (std::system(setup.c_str()) != 0) { std::printf("  skipped: openssl could not make a certificate\n"); return 0; }
    const std::string start = "python3 -I " + server + " 127.0.0.1 " + std::to_string(port) + " " + dir + "/cert.pem " + dir
                              + "/key.pem >/dev/null 2>&1 & echo $! > " + dir + "/pid";
    (void)std::system(start.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    const auto stop_server = [&] { (void)std::system(("kill $(cat " + dir + "/pid) 2>/dev/null; sleep 0.3").c_str()); };
    {
        altair::HttpsKeepAlive h("localhost", std::chrono::seconds{50}, port, dir + "/cert.pem");
        check(h.warm(), "warm: connected and the certificate verified");
        int good = 0;
        for (int i = 0; i < 20; ++i) {
            const std::string body = "{\"i\":" + std::to_string(i) + "}";
            const auto r = h.request(i % 2 ? "POST" : "GET", "/api/v3/echo", i % 2 ? body : "", "app:token");
            if (r && r->status == 200 && (i % 2 == 0 || r->body.find(body) != std::string::npos)) ++good;
        }
        check(good == 20, "20 requests answered, Content-Length and chunked");
        check(h.stats().connects == 1, "all on one connection");
        std::printf("    %s\n", h.json().c_str());
        check(h.request("GET", "/close", "", "app:token").has_value(), "a reply with Connection: close is read");
        check(!h.connected(), "and the connection is dropped");
        check(h.request("GET", "/api/v3/echo", "", "app:token").has_value() && h.stats().connects == 2,
              "the next request reconnects first");
        stop_server();
        const auto gone = h.request("GET", "/api/v3/echo", "", "app:token");
        check(!gone && gone.error() == altair::HttpError::ConnectFailed && h.stats().replaced_stale == 1,
              "a server that went away is seen BEFORE the request is written (ConnectFailed: nothing was sent)");
    }
    stop_server();
    (void)std::system(("rm -rf " + dir).c_str());
    std::printf("%s\n", failures ? "https keep-alive checks did not pass" : "all https keep-alive checks passed");
    return failures ? 1 : 0;
}
