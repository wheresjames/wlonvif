#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>

#include <iostream>
#include <thread>

int main()
{
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    using tcp = asio::ip::tcp;
    asio::io_context io;
    asio::ssl::context tls(asio::ssl::context::tls_server);
    (void)tls;
    tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    std::thread server(
        [&]
        {
            tcp::socket socket(io);
            acceptor.accept(socket);
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(socket, buffer, request);
            http::response<http::string_body> response{http::status::ok, request.version()};
            response.set(http::field::content_type, "text/plain");
            response.body() = "wlonvif";
            response.prepare_payload();
            http::write(socket, response);
            tcp::socket websocket_socket(io);
            acceptor.accept(websocket_socket);
            websocket::stream<tcp::socket> ws(std::move(websocket_socket));
            ws.accept();
            beast::flat_buffer websocket_buffer;
            ws.read(websocket_buffer);
            ws.text(ws.got_text());
            ws.write(websocket_buffer.data());
            ws.close(websocket::close_code::normal);
        }
    );
    tcp::socket socket(io);
    socket.connect(endpoint);
    http::request<http::empty_body> request{http::verb::get, "/health", 11};
    request.set(http::field::host, "127.0.0.1");
    http::write(socket, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(socket, buffer, response);
    websocket::stream<tcp::socket> ws(io);
    ws.next_layer().connect(endpoint);
    ws.handshake("127.0.0.1", "/control");
    ws.write(asio::buffer(std::string("intent")));
    beast::flat_buffer websocket_buffer;
    ws.read(websocket_buffer);
    const auto echoed = beast::buffers_to_string(websocket_buffer.data());
    ws.close(websocket::close_code::normal);
    server.join();
    if (response.result() != http::status::ok || response.body() != "wlonvif" ||
        echoed != "intent") return 1;
    std::cout << "Boost.Beast HTTP/WebSocket and OpenSSL linkage accepted\n";
}
