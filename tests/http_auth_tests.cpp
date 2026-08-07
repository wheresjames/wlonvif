/**
 * @file http_auth_tests.cpp
 * @brief Verifies HTTP authentication negotiation in the real libcurl transport.
 *
 * These exercise make_curl_transport() against a loopback HTTP server rather than
 * a stub, because the behaviour under test belongs to libcurl's auth state machine
 * and is invisible to an injected transport.
 *
 * The rule being enforced: never engage HTTP authentication until the device has
 * actually asked for it. ONVIF devices overwhelmingly authenticate with a
 * WS-Security UsernameToken and issue no HTTP challenge at all; probing them with
 * a scheme they never requested is what made one camera family drop the
 * connection outright.
 */

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <wlonvif/wlonvif.hpp>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace wlonvif;

namespace
{
    int failures = 0;

    template <class T> T sync(Task<T> task)
    {
        struct State
        {
            std::mutex mutex;
            std::condition_variable ready;
            std::optional<T> value;
        };
        auto state = std::make_shared<State>();
        std::move(task).start(
            [state](T value)
            {
                std::lock_guard lock(state->mutex);
                state->value.emplace(std::move(value));
                state->ready.notify_one();
            }
        );
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock, [&] { return state->value.has_value(); });
        return std::move(*state->value);
    }

#ifndef _WIN32
    /// How the test server answers the first request of each connection.
    enum class Challenge
    {
        none,   ///< Always 200; never asks for HTTP authentication.
        digest, ///< 401 with a Digest challenge, then 200 once authenticated.
        basic   ///< 401 with a Basic challenge, then 200 once authenticated.
    };

    /**
     * @brief Minimal single-threaded HTTP/1.1 server for one client at a time.
     *
     * Records every Authorization header it receives so the test can assert on
     * what the transport actually put on the wire.
     */
    class TestServer
    {
      public:
        explicit TestServer(Challenge challenge) : challenge_(challenge)
        {
            listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listener_ < 0)
            {
                return;
            }
            int reuse = 1;
            setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port = htons(0);
            inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
            if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
                listen(listener_, 8) != 0)
            {
                close(listener_);
                listener_ = -1;
                return;
            }
            sockaddr_in bound{};
            socklen_t bound_size = sizeof(bound);
            getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &bound_size);
            port_ = ntohs(bound.sin_port);
            worker_ = std::thread([this] { serve(); });
        }

        ~TestServer()
        {
            stopping_ = true;
            if (listener_ >= 0)
            {
                shutdown(listener_, SHUT_RDWR);
                close(listener_);
            }
            if (worker_.joinable())
            {
                worker_.join();
            }
        }

        bool valid() const noexcept { return listener_ >= 0; }

        std::string url() const
        {
            return "http://127.0.0.1:" + std::to_string(port_) + "/onvif/device_service";
        }

        std::vector<std::string> authorizations()
        {
            std::lock_guard lock(mutex_);
            return authorizations_;
        }

        int requests() const noexcept { return requests_.load(); }

      private:
        void serve()
        {
            while (!stopping_)
            {
                const int client = accept(listener_, nullptr, nullptr);
                if (client < 0)
                {
                    return;
                }
                handle(client);
                close(client);
            }
        }

        /// Read one request (headers plus any body) and answer it.
        void handle(int client)
        {
            std::string request;
            bool authenticated = false;
            for (;;)
            {
                const auto headers_end = request.find("\r\n\r\n");
                if (headers_end != std::string::npos)
                {
                    // Honour Content-Length so the body is drained before replying;
                    // leaving it unread would make libcurl see a broken connection.
                    std::size_t length = 0;
                    const auto lowered = lowercase(request.substr(0, headers_end));
                    const auto marker = lowered.find("content-length:");
                    if (marker != std::string::npos)
                    {
                        length = static_cast<std::size_t>(
                            std::strtoul(request.c_str() + marker + 15, nullptr, 10)
                        );
                    }
                    if (request.size() >= headers_end + 4 + length)
                    {
                        authenticated = lowered.find("authorization:") != std::string::npos;
                        record(request, headers_end);
                        break;
                    }
                }
                char buffer[4096];
                const auto received = recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0)
                {
                    return;
                }
                request.append(buffer, static_cast<std::size_t>(received));
            }
            ++requests_;

            // libcurl replays the request on the same connection after a 401, so
            // "expect continue" must be answered before the challenge is sent.
            if (lowercase(request).find("expect: 100-continue") != std::string::npos)
            {
                const std::string proceed = "HTTP/1.1 100 Continue\r\n\r\n";
                send(client, proceed.data(), proceed.size(), 0);
            }

            const bool wants_challenge = challenge_ != Challenge::none && !authenticated;
            std::string response;
            if (wants_challenge)
            {
                const std::string header =
                    challenge_ == Challenge::digest
                        ? "WWW-Authenticate: Digest realm=\"wlonvif\", qop=\"auth\", "
                          "nonce=\"4f1a9c8b2d\", opaque=\"7c3\"\r\n"
                        : "WWW-Authenticate: Basic realm=\"wlonvif\"\r\n";
                response = "HTTP/1.1 401 Unauthorized\r\n" + header +
                           "Content-Length: 0\r\nConnection: keep-alive\r\n\r\n";
            }
            else
            {
                const std::string body =
                    "<SOAP-ENV:Envelope "
                    "xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
                    "xmlns:ns2=\"http://www.onvif.org/ver10/device/wsdl\"><SOAP-ENV:Body>"
                    "<ns2:GetServicesResponse/></SOAP-ENV:Body></SOAP-ENV:Envelope>";
                response = "HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\n"
                           "Content-Length: " +
                           std::to_string(body.size()) +
                           "\r\nConnection: keep-alive\r\n\r\n" + body;
            }
            send(client, response.data(), response.size(), 0);
        }

        void record(const std::string& request, std::size_t headers_end)
        {
            const auto headers = request.substr(0, headers_end);
            std::size_t position = 0;
            while (position < headers.size())
            {
                const auto line_end = headers.find("\r\n", position);
                const auto line = headers.substr(
                    position, line_end == std::string::npos ? std::string::npos : line_end - position
                );
                if (lowercase(line).rfind("authorization:", 0) == 0)
                {
                    std::lock_guard lock(mutex_);
                    authorizations_.push_back(line);
                }
                if (line_end == std::string::npos)
                {
                    break;
                }
                position = line_end + 2;
            }
        }

        static std::string lowercase(std::string value)
        {
            for (auto& character : value)
            {
                character = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(character))
                );
            }
            return value;
        }

        Challenge challenge_;
        int listener_{-1};
        std::uint16_t port_{};
        std::atomic_bool stopping_{false};
        std::atomic_int requests_{0};
        std::thread worker_;
        std::mutex mutex_;
        std::vector<std::string> authorizations_;
    };

    bool contains(const std::vector<std::string>& lines, std::string_view needle)
    {
        for (const auto& line : lines)
        {
            if (line.find(needle) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    HttpRequest soap_request(const std::string& url, std::optional<Credentials> credentials)
    {
        return HttpRequest{
            url,
            soap_envelope(OperationId::get_services, "", credentials),
            {"Content-Type: application/soap+xml; charset=utf-8"},
            std::chrono::steady_clock::now() + std::chrono::seconds(10),
            std::move(credentials),
            HttpMethod::post
        };
    }
#endif
} // namespace

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "http auth check failed: " #expression " at line " << __LINE__ << '\n';   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

int main()
{
#ifdef _WIN32
    std::cout << "http auth tests skipped on this platform\n";
    return 0;
#else
    auto runtime_result = Runtime::create();
    CHECK(runtime_result);
    if (!runtime_result)
    {
        return 1;
    }
    auto runtime = runtime_result.value();
    auto transport = make_curl_transport(runtime);
    const Credentials credentials{"camadmin", "secret", false};
    Credentials permissive{"camadmin", "secret", true};

    // ---- A device that never challenges must never see credentials ----
    {
        TestServer server(Challenge::none);
        CHECK(server.valid());
        if (server.valid())
        {
            auto response = sync(
                transport->send(soap_request(server.url(), credentials), CancellationSource{}.token())
            );
            CHECK(response);
            CHECK(response && response.value().status == 200);
            // The decisive assertion: no Authorization header was ever sent, and no
            // extra probe request was made.
            CHECK(server.authorizations().empty());
            CHECK(server.requests() == 1);
        }
    }

    // ---- A Digest challenge is answered with Digest ----
    {
        TestServer server(Challenge::digest);
        CHECK(server.valid());
        if (server.valid())
        {
            auto response = sync(
                transport->send(soap_request(server.url(), credentials), CancellationSource{}.token())
            );
            CHECK(response);
            CHECK(response && response.value().status == 200);
            CHECK(contains(server.authorizations(), "Digest"));
            CHECK(!contains(server.authorizations(), "Basic"));
        }
    }

    // ---- Basic over cleartext is refused unless explicitly permitted ----
    {
        TestServer server(Challenge::basic);
        CHECK(server.valid());
        if (server.valid())
        {
            auto response = sync(
                transport->send(soap_request(server.url(), credentials), CancellationSource{}.token())
            );
            CHECK(!response);
            if (!response)
            {
                CHECK(response.error().category == ErrorCategory::authentication);
                // The message has to say what to change, not merely that it failed.
                CHECK(
                    response.error().message.find("allow_cleartext_basic") != std::string::npos
                );
            }
            // The password must not have been sent while deciding to refuse.
            CHECK(server.authorizations().empty());
        }
    }

    // ---- ...and used once the caller accepts the exposure ----
    {
        TestServer server(Challenge::basic);
        CHECK(server.valid());
        if (server.valid())
        {
            auto response = sync(
                transport->send(soap_request(server.url(), permissive), CancellationSource{}.token())
            );
            CHECK(response);
            CHECK(response && response.value().status == 200);
            CHECK(contains(server.authorizations(), "Basic"));
        }
    }

    // ---- No credentials means no authentication attempt at all ----
    {
        TestServer server(Challenge::digest);
        CHECK(server.valid());
        if (server.valid())
        {
            auto response = sync(
                transport->send(soap_request(server.url(), std::nullopt), CancellationSource{}.token())
            );
            CHECK(response);
            // The caller sees the 401 and decides; the transport does not guess.
            CHECK(response && response.value().status == 401);
            CHECK(server.authorizations().empty());
        }
    }

    CHECK(bool(runtime->shutdown()));
    if (failures != 0)
    {
        std::cerr << failures << " http auth check(s) failed\n";
        return 1;
    }
    std::cout << "http auth tests passed\n";
    return 0;
#endif
}
