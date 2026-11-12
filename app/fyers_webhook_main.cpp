#include <broker/fyers_webhook.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

int main(int argc, char** argv) {
    std::uint16_t port = 8787;
    if (argc == 2) port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
    std::string configured;
    if (const char* raw_secret = std::getenv("ALTAIR_FYERS_WEBHOOK_SECRET")) {
        configured = raw_secret;
    }
    if (configured.empty()) {
        std::ifstream secret_file{"data/fyers_webhook_secret.txt", std::ios::binary};
        if (secret_file) {
            std::getline(secret_file, configured);
            while (!configured.empty() && (configured.back() == '\r' || configured.back() == '\n'))
                configured.pop_back();
        }
    }
    if (configured.empty()) {
        std::cerr << "ALTAIR_FYERS_WEBHOOK_SECRET is required\n";
        return 2;
    }
    net::io_context ioc;
    tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), port}};
    std::cout << "FYERS webhook listening on 127.0.0.1:" << port
              << " path /fyers/callback\n";
    for (;;) {
        tcp::socket socket{ioc};
        acceptor.accept(socket);
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        beast::error_code ec;
        http::read(socket, buffer, request, ec);
        http::response<http::string_body> response{http::status::bad_request,
                                                    request.version()};
        response.set(http::field::server, "altair-fyers-webhook");
        response.keep_alive(false);
        const auto target = altair::fyers_webhook::split_target(
            std::string_view{request.target().data(), request.target().size()});
        if (!ec && (request.method() == http::verb::get
                    || request.method() == http::verb::head)
            && target.path == "/fyers/callback") {
            response.result(http::status::ok);
            response.body() = request.method() == http::verb::head ? "" : "ok\n";
        } else if (!ec && request.method() == http::verb::post
                   && target.path == "/fyers/callback") {
            const auto header = request["X-Fyers-Webhook-Secret"];
            const std::string& body = request.body();
            if (body.empty() || body.size() > altair::fyers_webhook::kMaxBodyBytes) {
                response.result(http::status::bad_request);
                response.body() = "rejected\n";
            } else if (!altair::fyers_webhook::authorised(configured, target.key,
                                                         std::string_view{header.data(), header.size()}, body)) {
                response.result(http::status::unauthorized);
                response.body() = "rejected\n";
            } else {
                // A nudge for the order router (it watches this file and reads
                // FYERS's order book at once); the book, not this post, is what
                // the router believes. Only the order id and a status word are
                // kept -- never the body.
                const auto st = altair::fyers_webhook::parse_status(altair::fyers_webhook::json_value(body, "status"));
                const char* word = st == altair::fyers_webhook::Status::Pending ? "pending"
                                 : st == altair::fyers_webhook::Status::Rejected ? "rejected"
                                 : st == altair::fyers_webhook::Status::Cancelled ? "cancelled"
                                 : st == altair::fyers_webhook::Status::Traded ? "traded" : "other";
                const auto at = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::ofstream audit{"data/fyers_webhook_events.jsonl", std::ios::app};
                if (audit) {
                    audit << "{\"at_ms\":" << at << ",\"status\":\"" << word << "\",\"id\":\""
                          << altair::fyers_webhook::order_id_of(body) << "\"}\n";
                }
                response.result(http::status::ok);
                response.body() = "ok\n";
            }
        } else {
            response.body() = "not found\n";
            response.result(http::status::not_found);
        }
        response.prepare_payload();
        http::write(socket, response, ec);
        socket.shutdown(tcp::socket::shutdown_both, ec);
    }
}