#include "server.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>
using namespace wlonvif::examples::ptz_web;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
#define CHECK(x) do { if (!(x)) { std::cerr << "check failed: " #x << " line " << __LINE__ << '\n'; return 1; } } while (0)

int main()
{
    WebServerConfig rejected;
    rejected.address = "0.0.0.0";
    rejected.port = 18990;
    rejected.protocol.allowed_origins = {"https://camera.test"};
    std::atomic_bool never{};
    WebServer remote(rejected, [](const auto&) { return wlonvif::Result<std::string>{"{}"}; }, [](auto) {});
    CHECK(!remote.run(never));
    rejected.allow_remote = true;
    rejected.certificate_file = "/does/not/exist.pem";
    rejected.private_key_file = "/does/not/exist.key";
    WebServer bad_certificate(rejected,
        [](const auto&) { return wlonvif::Result<std::string>{"{}"}; }, [](auto) {});
    CHECK(!bad_certificate.run(never));

    WebServerConfig config;
    config.port = 18991;
    config.protocol.allowed_origins = {"http://127.0.0.1:18991"};
    config.protocol.lease = std::chrono::milliseconds(300);
    std::atomic_int commands{}, disconnects{};
    std::atomic_bool stop{};
    WebServer server(config,
        [&](const auto&) { ++commands; return wlonvif::Result<std::string>{"{\"test\":true}"}; },
        [&](auto) { ++disconnects; });
    wlonvif::Result<void> server_result;
    std::thread thread([&] { server_result = server.run(stop); });
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
    ws.set_option(websocket::stream_base::decorator([&](websocket::request_type& request)
    {
        request.set(http::field::origin, config.protocol.allowed_origins.front());
        request.set(http::field::cookie, cookie);
    }));
    ws.handshake("127.0.0.1", "/control");
    ws.write(asio::buffer(std::string(
        R"({"version":1,"type":"intent","sequence":1,"pan":0,"tilt":0,"zoom":0})")));
    beast::flat_buffer reply;
    ws.read(reply);
    CHECK(beast::buffers_to_string(reply.data()).find("\"ok\":true") != std::string::npos);
    ws.close(websocket::close_code::normal);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(commands == 1);
    CHECK(disconnects == 1);
    stop.store(true);
    thread.join();
    CHECK(server_result);
    std::cout << "PTZ web server tests passed\n";
}
