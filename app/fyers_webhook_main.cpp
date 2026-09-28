#include <broker/fyers_webhook.hpp>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = net::ip::tcp;

int main(int argc, char** argv) {
    std::uint16_t port = 8787;
    if (argc == 2) port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));
    std::string configured;
#if defined(_MSC_VER)
    char* raw_secret = nullptr;
    std::size_t raw_length = 0;
    (void)_dupenv_s(&raw_secret, &raw_length, "ALTAIR_FYERS_WEBHOOK_SECRET");
    if (raw_secret != nullptr) {
        configured.assign(raw_secret, raw_length == 0 ? std::strlen(raw_secret) : raw_length);
        std::free(raw_secret);
    }
#else
    if (const char* raw_secret = std::getenv("ALTAIR_FYERS_WEBHOOK_SECRET")) {
        configured = raw_secret;
    }
#endif
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
        if (!ec && (request.method() == http::verb::get
                    || request.method() == http::verb::head)
            && request.target() == "/fyers/callback") {
            response.result(http::status::ok);
            response.body() = request.method() == http::verb::head ? "" : "ok\n";
        } else if (!ec && request.method() == http::verb::post
                   && request.target() == "/fyers/callback") {
            const auto supplied = request["X-Fyers-Webhook-Secret"];
            const auto event = altair::fyers_webhook::validate(
                configured, std::string_view{supplied.data(), supplied.size()}, request.body());
            if (event) {
                std::ofstream audit{"data/fyers_webhook_events.jsonl", std::ios::app};
                if (audit) {
                    audit << "{\"status\":\"" << request.body().substr(
                        request.body().find("\"status\"") + 10,
                        request.body().find('"', request.body().find("\"status\"") + 11)
                          - (request.body().find("\"status\"") + 10))
                          << "\",\"order_id_present\":"
                          << (event->order_id.empty() ? "false" : "true") << "}\n";
                }
                response.result(http::status::ok);
                response.body() = "ok\n";
            } else {
                response.result(event.error() == altair::fyers_webhook::WebhookError::SecretMismatch
                                    ? http::status::unauthorized : http::status::bad_request);
                response.body() = "rejected\n";
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