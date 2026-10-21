/**
 * @file server_tests.cpp
 * @brief End-to-end tests of the ptz-web server over loopback: refused configurations, the
 * page, the session cookie, the control socket, hostile error text, and idle timeouts.
 */

#include "server.hpp"

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <poll.h>
#endif
using namespace wlonvif::examples::ptz_web;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            std::cerr << "check failed: " #x << " line " << __LINE__ << '\n';                      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main()
{
    WebServerConfig rejected;
    rejected.address = "0.0.0.0";
    rejected.port = 18990;
    rejected.protocol.allowed_origins = {"https://camera.test"};
    std::atomic_bool never{};
    WebServer remote(
        rejected,
        [](const auto&)
        {
            return wlonvif::Result<std::string>{"{}"};
        },
        [](auto) {}
    );
    CHECK(!remote.run(never));
    rejected.allow_remote = true;
    rejected.certificate_file = "/does/not/exist.pem";
    rejected.private_key_file = "/does/not/exist.key";
    WebServer bad_certificate(
        rejected,
        [](const auto&)
        {
            return wlonvif::Result<std::string>{"{}"};
        },
        [](auto) {}
    );
    CHECK(!bad_certificate.run(never));

    WebServerConfig config;
    config.port = 18991;
    config.protocol.allowed_origins = {"http://127.0.0.1:18991"};
    config.protocol.lease = std::chrono::milliseconds(300);
    config.handshake_timeout = std::chrono::milliseconds(300);
    std::atomic_int commands{}, disconnects{};
    std::atomic_bool stop{};
    // Error text is free-form (device faults, curl messages) and must never be
    // able to break out of the JSON reply.
    const std::string hostile = "bad \"quote\" \\ back\nslash\x01";
    WebServer server(
        config,
        [&](const WebCommand& command) -> wlonvif::Result<std::string>
        {
            ++commands;
            if (command.sequence == 2)
            {
                return wlonvif::Error{
                    wlonvif::ErrorCategory::soap_fault,
                    wlonvif::OperationId::continuous_move,
                    hostile
                };
            }
            return wlonvif::Result<std::string>{"{\"test\":true}"};
        },
        [&](auto)
        {
            ++disconnects;
        }
    );
    wlonvif::Result<void> server_result;
    std::thread thread(
        [&]
        {
            server_result = server.run(stop);
        }
    );
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    asio::io_context io;
    tcp::socket page_socket(io);
    page_socket.connect({asio::ip::make_address("127.0.0.1"), config.port});
    http::request<http::empty_body> page{http::verb::get, "/", 11};
    page.set(http::field::host, "127.0.0.1");
    http::write(page_socket, page);
    beast::flat_buffer page_buffer;
    http::response<http::string_body> page_response;
    http::read(page_socket, page_buffer, page_response);
    CHECK(page_response.result() == http::status::ok);
    CHECK(page_response.body().find("__PTZ_CONFIGURATION__") == std::string::npos);
    CHECK(page_response.body().find("\"profiles\":[]") != std::string::npos);

    tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), config.port});
    http::request<http::empty_body> health{http::verb::get, "/health", 11};
    health.set(http::field::host, "127.0.0.1");
    http::write(socket, health);
    beast::flat_buffer buffer;
    http::response<http::string_body> health_response;
    http::read(socket, buffer, health_response);
    CHECK(health_response.result() == http::status::ok);
    CHECK(health_response["X-Frame-Options"] == "DENY");

    tcp::socket auth_socket(io);
    auth_socket.connect({asio::ip::make_address("127.0.0.1"), config.port});
    http::request<http::empty_body> auth{http::verb::post, "/api/session", 11};
    auth.set(http::field::host, "127.0.0.1");
    auth.set(http::field::origin, config.protocol.allowed_origins.front());
    http::write(auth_socket, auth);
    beast::flat_buffer auth_buffer;
    http::response<http::string_body> auth_response;
    http::read(auth_socket, auth_buffer, auth_response);
    CHECK(auth_response.result() == http::status::no_content);
    std::string cookie = auth_response[http::field::set_cookie];
    cookie = cookie.substr(0, cookie.find(';'));
    CHECK(cookie.starts_with("wlonvif_ptz="));

    websocket::stream<tcp::socket> ws(io);
    ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), config.port});
    ws.set_option(
        websocket::stream_base::decorator(
            [&](websocket::request_type& request)
            {
                request.set(http::field::origin, config.protocol.allowed_origins.front());
                request.set(http::field::cookie, cookie);
            }
        )
    );
    ws.handshake("127.0.0.1", "/control");
    ws.write(
        asio::buffer(
            std::string(R"({"version":1,"type":"intent","sequence":1,"pan":0,"tilt":0,"zoom":0})")
        )
    );
    beast::flat_buffer reply;
    ws.read(reply);
    CHECK(beast::buffers_to_string(reply.data()).find("\"ok\":true") != std::string::npos);
    ws.write(
        asio::buffer(
            std::string(R"({"version":1,"type":"intent","sequence":2,"pan":0,"tilt":0,"zoom":0})")
        )
    );
    beast::flat_buffer error_reply;
    ws.read(error_reply);
    boost::system::error_code json_error;
    const auto parsed_reply =
        boost::json::parse(beast::buffers_to_string(error_reply.data()), json_error);
    CHECK(!json_error && parsed_reply.is_object());
    CHECK(parsed_reply.as_object().at("ok").as_bool() == false);
    CHECK(parsed_reply.as_object().at("error").as_string() == hostile);
    ws.close(websocket::close_code::normal);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(commands == 2);
    CHECK(disconnects == 1);
#ifndef _WIN32
    // Synchronous Beast calls ignore tcp_stream expiry, so the server enforces
    // deadlines itself: a client that never sends a request is disconnected after
    // the handshake timeout, and a silent control socket after its lease.
    const auto closed_within = [](auto& socket, std::chrono::milliseconds limit)
    {
        pollfd readable{socket.native_handle(), POLLIN, 0};
        if (::poll(&readable, 1, static_cast<int>(limit.count())) <= 0)
        {
            return false;
        }
        char byte;
        beast::error_code read_error;
        socket.read_some(asio::buffer(&byte, 1), read_error);
        return static_cast<bool>(read_error);
    };
    tcp::socket silent(io);
    silent.connect({asio::ip::make_address("127.0.0.1"), config.port});
    CHECK(closed_within(silent, std::chrono::milliseconds(2000)));
    websocket::stream<tcp::socket> silent_ws(io);
    silent_ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), config.port});
    auto second_auth = auth;
    tcp::socket second_auth_socket(io);
    second_auth_socket.connect({asio::ip::make_address("127.0.0.1"), config.port});
    http::write(second_auth_socket, second_auth);
    beast::flat_buffer second_auth_buffer;
    http::response<http::string_body> second_auth_response;
    http::read(second_auth_socket, second_auth_buffer, second_auth_response);
    std::string second_cookie = second_auth_response[http::field::set_cookie];
    second_cookie = second_cookie.substr(0, second_cookie.find(';'));
    silent_ws.set_option(
        websocket::stream_base::decorator(
            [&](websocket::request_type& request)
            {
                request.set(http::field::origin, config.protocol.allowed_origins.front());
                request.set(http::field::cookie, second_cookie);
            }
        )
    );
    silent_ws.handshake("127.0.0.1", "/control");
    CHECK(closed_within(silent_ws.next_layer(), std::chrono::milliseconds(2000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(disconnects == 2);
#endif
    websocket::stream<tcp::socket> idle_ws(io);
    idle_ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), config.port});
    idle_ws.set_option(
        websocket::stream_base::decorator(
            [&](websocket::request_type& request)
            {
                request.set(http::field::origin, config.protocol.allowed_origins.front());
                request.set(http::field::cookie, cookie);
            }
        )
    );
    idle_ws.handshake("127.0.0.1", "/control");
    // A client may connect and then send no HTTP bytes. Shutdown must interrupt
    // the worker's synchronous header read instead of joining it forever.
    tcp::socket idle_socket(io);
    idle_socket.connect({asio::ip::make_address("127.0.0.1"), config.port});
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    thread.join();
    idle_socket.close();
    beast::error_code ignored;
    idle_ws.next_layer().close(ignored);
    CHECK(server_result);
    std::cout << "PTZ web server tests passed\n";
}
