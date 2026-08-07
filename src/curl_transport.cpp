#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <curl/curl.h>
#include <openssl/crypto.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#include "curl_init.hpp"
#include "dispatch.hpp"
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    namespace
    {
        struct WriteContext
        {
            std::string* output;
            std::size_t maximum;
            bool exceeded{};
        };
        size_t write_cb(char* p, size_t s, size_t n, void* u)
        {
            auto& context = *static_cast<WriteContext*>(u);
            if (n != 0 && s > std::numeric_limits<std::size_t>::max() / n)
            {
                context.exceeded = true;
                return 0;
            }
            const auto bytes = s * n;
            if (bytes > context.maximum - std::min(context.output->size(), context.maximum))
            {
                context.exceeded = true;
                return 0;
            }
            context.output->append(p, bytes);
            return bytes;
        }
        int transfer_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
        {
            return static_cast<CancellationToken*>(user)->stop_requested() ? 1 : 0;
        }
        /// Collects the WWW-Authenticate challenge so the auth scheme can be chosen.
        struct HeaderContext
        {
            std::string challenge;
            static constexpr std::size_t maximum = 4096;
        };
        size_t header_cb(char* p, size_t s, size_t n, void* u)
        {
            auto& context = *static_cast<HeaderContext*>(u);
            if (n != 0 && s > std::numeric_limits<std::size_t>::max() / n) return 0;
            const auto bytes = s * n;
            constexpr std::string_view name = "www-authenticate:";
            std::string_view line(p, bytes);
            if (line.size() > name.size())
            {
                std::string prefix(line.substr(0, name.size()));
                std::transform(
                    prefix.begin(),
                    prefix.end(),
                    prefix.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
                );
                if (prefix == name && context.challenge.size() < HeaderContext::maximum)
                {
                    auto value = line.substr(name.size());
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    {
                        value.remove_prefix(1);
                    }
                    while (!value.empty() && (value.back() == '\r' || value.back() == '\n'))
                    {
                        value.remove_suffix(1);
                    }
                    const auto separator = context.challenge.empty() ? 0u : 2u;
                    const auto available = HeaderContext::maximum - context.challenge.size();
                    if (available <= separator) return bytes;
                    if (separator != 0)
                    {
                        context.challenge += ", ";
                    }
                    context.challenge.append(value.substr(0, available - separator));
                }
            }
            return bytes;
        }
        /// Lowercase copy used for case-insensitive scheme matching.
        std::string lowercase(std::string_view input)
        {
            std::string output(input);
            std::transform(
                output.begin(),
                output.end(),
                output.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
            );
            return output;
        }
        bool offers_auth_scheme(std::string_view challenge, std::string_view scheme)
        {
            for (std::size_t position = 0; position < challenge.size();)
            {
                while (position < challenge.size() &&
                       (challenge[position] == ',' || std::isspace(
                           static_cast<unsigned char>(challenge[position]))))
                    ++position;
                const auto begin = position;
                while (position < challenge.size() &&
                       (std::isalnum(static_cast<unsigned char>(challenge[position])) ||
                        challenge[position] == '-' || challenge[position] == '_'))
                    ++position;
                if (challenge.substr(begin, position - begin) == scheme &&
                    (position == challenge.size() ||
                     std::isspace(static_cast<unsigned char>(challenge[position]))))
                    return true;
                position = challenge.find(',', position);
                if (position == challenge.npos) break;
            }
            return false;
        }
        Result<std::vector<std::string>> resolve_host(
            std::string host,
            std::string port,
            std::chrono::steady_clock::time_point deadline,
            CancellationToken token
        )
        {
            struct State
            {
                std::mutex mutex;
                std::condition_variable ready;
                bool done{};
                int code{};
                std::vector<std::string> addresses;
            };
            auto state = std::make_shared<State>();
            try
            {
                std::thread([state, host = std::move(host), port = std::move(port)]
                {
                    addrinfo hints{};
                    hints.ai_socktype = SOCK_STREAM;
                    hints.ai_family = AF_UNSPEC;
                    addrinfo* results = nullptr;
                    const int code = getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
                    std::vector<std::string> addresses;
                    for (auto* item = results; item; item = item->ai_next)
                    {
                        char buffer[INET6_ADDRSTRLEN]{};
                        const void* address = item->ai_family == AF_INET
                            ? static_cast<const void*>(&reinterpret_cast<sockaddr_in*>(item->ai_addr)->sin_addr)
                            : item->ai_family == AF_INET6
                                ? static_cast<const void*>(&reinterpret_cast<sockaddr_in6*>(item->ai_addr)->sin6_addr)
                                : nullptr;
                        if (address && inet_ntop(item->ai_family, address, buffer, sizeof(buffer)) &&
                            std::find(addresses.begin(), addresses.end(), buffer) == addresses.end())
                            addresses.emplace_back(buffer);
                    }
                    if (results) freeaddrinfo(results);
                    {
                        std::lock_guard lock(state->mutex);
                        state->code = code;
                        state->addresses = std::move(addresses);
                        state->done = true;
                    }
                    state->ready.notify_one();
                }).detach();
            }
            catch (const std::exception& error)
            {
                return Error{ErrorCategory::internal, {}, error.what()};
            }
            std::unique_lock lock(state->mutex);
            while (!state->done && !token.stop_requested() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                state->ready.wait_until(
                    lock,
                    std::min(deadline, std::chrono::steady_clock::now() +
                                           std::chrono::milliseconds(20))
                );
            }
            if (token.stop_requested()) return Error{ErrorCategory::cancelled};
            if (!state->done) return Error{ErrorCategory::timeout};
            if (state->code != 0 || state->addresses.empty())
                return Error{ErrorCategory::network, {}, "DNS resolution failed", true};
            return std::move(state->addresses);
        }
        class CurlTransport final : public HttpTransport
        {
          public:
            CurlTransport(
                std::shared_ptr<Runtime> r, XmlLimits l,
                std::shared_ptr<CandidateAuthorizer> authorizer = {}
            ) : runtime_(std::move(r)), limits_(l), authorizer_(std::move(authorizer))
            {
                detail::initialize_curl();
            }
            Task<Result<HttpResponse>> send(HttpRequest req, CancellationToken token) override
            {
                auto rt = runtime_;
                auto lim = limits_;
                auto authorizer = authorizer_;
                return Task<Result<HttpResponse>>(
                    [rt, lim, authorizer, req = std::move(req), token](auto done) mutable
                    {
                        std::thread(
                            [rt, lim, authorizer, req = std::move(req), token, done = std::move(done)]() mutable
                            {
                                if (token.stop_requested())
                                {
                                    detail::dispatch(
                                        rt,
                                        [done = std::move(done)]() mutable
                                        {
                                            done(Error{ErrorCategory::cancelled});
                                        }
                                    );
                                    return;
                                }
                                curl_slist* pinned = nullptr;
                                if (authorizer)
                                {
                                    CURLU* parsed = curl_url();
                                    char* host_value = nullptr;
                                    char* port_value = nullptr;
                                    const auto url_code = parsed ? curl_url_set(parsed, CURLUPART_URL, req.url.c_str(), 0) : CURLUE_BAD_HANDLE;
                                    if (url_code == CURLUE_OK) curl_url_get(parsed, CURLUPART_HOST, &host_value, 0);
                                    if (url_code == CURLUE_OK) curl_url_get(parsed, CURLUPART_PORT, &port_value, CURLU_DEFAULT_PORT);
                                    if (!host_value || !port_value)
                                    {
                                        if (host_value) curl_free(host_value);
                                        if (port_value) curl_free(port_value);
                                        if (parsed) curl_url_cleanup(parsed);
                                        detail::dispatch(rt, [done = std::move(done)]() mutable { done(Error{ErrorCategory::invalid_argument, {}, "HTTP URL has no resolvable host"}); });
                                        return;
                                    }
                                    char* port_end = nullptr;
                                    const auto parsed_port = std::strtoul(port_value, &port_end, 10);
                                    if (!port_end || *port_end != '\0' || parsed_port > 65535)
                                    {
                                        curl_free(host_value); curl_free(port_value); curl_url_cleanup(parsed);
                                        detail::dispatch(rt, [done = std::move(done)]() mutable { done(Error{ErrorCategory::invalid_argument, {}, "HTTP URL has an invalid port"}); });
                                        return;
                                    }
                                    auto resolved = resolve_host(
                                        host_value, port_value, req.deadline, token
                                    );
                                    if (!resolved)
                                    {
                                        curl_free(host_value); curl_free(port_value); curl_url_cleanup(parsed);
                                        auto error = resolved.error();
                                        detail::dispatch(rt, [done = std::move(done), error = std::move(error)]() mutable { done(std::move(error)); });
                                        return;
                                    }
                                    auto addresses = std::move(resolved.value());
                                    static std::atomic_uint64_t attempt{0};
                                    CandidateAuthorizationRequest request;
                                    request.original_url = req.original_url.empty()
                                        ? req.url : req.original_url;
                                    request.effective_url = req.url;
                                    request.hostname = lowercase(host_value);
                                    request.port = static_cast<std::uint16_t>(parsed_port);
                                    request.resolved_addresses = addresses;
                                    request.proposed_address = addresses.front();
                                    request.purpose = req.purpose;
                                    request.provenance = req.provenance;
                                    request.resolution_attempt_id = std::to_string(++attempt);
                                    request.operation = req.operation;
                                    request.idempotent = req.idempotent;
                                    request.retry_allowed = req.retry_allowed;
                                    struct Approval { std::mutex mutex; std::condition_variable cv; std::optional<Result<void>> result; };
                                    auto approval = std::make_shared<Approval>();
                                    auto task = authorizer->authorize(request, {req.deadline, token});
                                    std::move(task).start([approval](Result<void> value) { std::lock_guard lock(approval->mutex); approval->result.emplace(std::move(value)); approval->cv.notify_one(); });
                                    std::unique_lock lock(approval->mutex);
                                    while (!approval->result && !token.stop_requested() && std::chrono::steady_clock::now() < req.deadline)
                                        approval->cv.wait_until(lock, req.deadline);
                                    Result<void> authorized = token.stop_requested()
                                        ? Result<void>(Error{ErrorCategory::cancelled})
                                        : !approval->result ? Result<void>(Error{ErrorCategory::timeout})
                                                           : std::move(*approval->result);
                                    if (!authorized)
                                    {
                                        auto error = authorized.error();
                                        if (error.category != ErrorCategory::cancelled && error.category != ErrorCategory::timeout)
                                        {
                                            error.category = ErrorCategory::host_permission_denied;
                                            error.host_permission = HostPermissionDetail{request.hostname, request.port, request.proposed_address, request.resolution_attempt_id};
                                        }
                                        curl_free(host_value); curl_free(port_value); curl_url_cleanup(parsed);
                                        detail::dispatch(rt, [done = std::move(done), error = std::move(error)]() mutable { done(std::move(error)); });
                                        return;
                                    }
                                    std::string pin = std::string(host_value) + ':' + port_value + ':';
                                    if (addresses.front().find(':') != std::string::npos) pin += '[' + addresses.front() + ']';
                                    else pin += addresses.front();
                                    pinned = curl_slist_append(nullptr, pin.c_str());
                                    curl_free(host_value); curl_free(port_value); curl_url_cleanup(parsed);
                                }
                                CURL* c = curl_easy_init();
                                if (!c)
                                {
                                    detail::dispatch(
                                        rt,
                                        [done = std::move(done)]() mutable
                                        {
                                            done(Error{
                                                ErrorCategory::internal,
                                                {},
                                                "unable to initialize HTTP transport"
                                            });
                                        }
                                    );
                                    return;
                                }
                                HttpResponse response;
                                curl_slist* hs = nullptr;
                                for (auto& h : req.headers)
                                {
                                    hs = curl_slist_append(hs, h.c_str());
                                }
                                curl_easy_setopt(c, CURLOPT_URL, req.url.c_str());
                                if (pinned) curl_easy_setopt(c, CURLOPT_RESOLVE, pinned);
                                if (req.method == HttpMethod::get)
                                {
                                    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
                                }
                                else
                                {
                                    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req.body.c_str());
                                    curl_easy_setopt(
                                        c,
                                        CURLOPT_POSTFIELDSIZE_LARGE,
                                        static_cast<curl_off_t>(req.body.size())
                                    );
                                }
                                curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
                                curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
                                WriteContext write_context{&response.body, lim.bytes};
                                curl_easy_setopt(c, CURLOPT_WRITEDATA, &write_context);
                                curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
                                curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
                                curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, transfer_progress);
                                curl_easy_setopt(c, CURLOPT_XFERINFODATA, &token);
                                curl_easy_setopt(
                                    c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(lim.bytes)
                                );
                                HeaderContext header_context;
                                curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header_cb);
                                curl_easy_setopt(c, CURLOPT_HEADERDATA, &header_context);
                                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                req.deadline - std::chrono::steady_clock::now()
                                )
                                                .count();
                                curl_easy_setopt(
                                    c, CURLOPT_TIMEOUT_MS, static_cast<long>(left > 0 ? left : 1)
                                );
                                const auto perform = [&]
                                {
                                    response.body.clear();
                                    write_context.exceeded = false;
                                    header_context.challenge.clear();
                                    return curl_easy_perform(c);
                                };
                                // The first attempt carries no HTTP credentials. ONVIF devices
                                // overwhelmingly authenticate with a WS-Security UsernameToken and
                                // never issue an HTTP challenge, and engaging libcurl's auth
                                // machinery unprompted makes it probe with a scheme the device
                                // never asked for -- which some embedded servers answer by
                                // dropping the connection.
                                auto code = perform();
                                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response.status);
                                bool attempted_cleartext_digest = false;
                                std::optional<Error> auth_error;
                                if (code == CURLE_OK && response.status == 401 &&
                                    (req.credentials || req.credential_lease))
                                {
                                    const bool allow_cleartext = req.credential_lease
                                        ? req.credential_lease->allow_cleartext_basic()
                                        : req.credentials->allow_cleartext_basic;
                                    const bool encrypted = lowercase(req.url).starts_with("https://");
                                    const auto challenge = lowercase(header_context.challenge);
                                    const bool basic_offered = offers_auth_scheme(challenge, "basic");
                                    long methods = 0;
                                    if (offers_auth_scheme(challenge, "digest"))
                                    {
                                        methods |= CURLAUTH_DIGEST;
                                    }
                                    if (offers_auth_scheme(challenge, "negotiate"))
                                    {
                                        methods |= CURLAUTH_NEGOTIATE;
                                    }
                                    if (offers_auth_scheme(challenge, "ntlm"))
                                    {
                                        methods |= CURLAUTH_NTLM;
                                    }
                                    // Basic reveals the password to anyone on the path, so it is
                                    // only offered back when the channel is encrypted or the
                                    // caller has explicitly accepted the exposure.
                                    if (basic_offered &&
                                        (encrypted || allow_cleartext))
                                    {
                                        methods |= CURLAUTH_BASIC;
                                    }
                                    if (methods != 0)
                                    {
                                        attempted_cleartext_digest =
                                            !encrypted && (methods & CURLAUTH_DIGEST) != 0;
                                        std::string auth_username = req.credential_lease
                                            ? std::string(req.credential_lease->username())
                                            : req.credentials->username;
                                        std::string auth_password = req.credential_lease
                                            ? std::string(req.credential_lease->password())
                                            : req.credentials->password;
                                        curl_easy_setopt(c, CURLOPT_USERNAME, auth_username.c_str());
                                        curl_easy_setopt(c, CURLOPT_PASSWORD, auth_password.c_str());
                                        curl_easy_setopt(c, CURLOPT_HTTPAUTH, methods);
                                        code = perform();
                                        OPENSSL_cleanse(auth_username.data(), auth_username.size());
                                        OPENSSL_cleanse(auth_password.data(), auth_password.size());
                                        curl_easy_getinfo(
                                            c, CURLINFO_RESPONSE_CODE, &response.status
                                        );
                                    }
                                    else if (basic_offered)
                                    {
                                        auth_error = Error{
                                            ErrorCategory::authentication,
                                            {},
                                            "the device offers only HTTP Basic authentication over "
                                            "an unencrypted connection; set "
                                            "Credentials::allow_cleartext_basic to permit sending "
                                            "the password in cleartext, or use an https:// endpoint",
                                            false
                                        };
                                    }
                                    else if (!challenge.empty())
                                    {
                                        auth_error = Error{
                                            ErrorCategory::authentication,
                                            {},
                                            "the device requested an unsupported HTTP "
                                            "authentication scheme",
                                            false
                                        };
                                    }
                                }
                                char* ct = nullptr;
                                curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
                                if (ct)
                                {
                                    response.content_type = ct;
                                }
                                curl_slist_free_all(hs);
                                curl_slist_free_all(pinned);
                                curl_easy_cleanup(c);
                                Result<HttpResponse> result = std::move(response);
                                if (auth_error)
                                {
                                    result = std::move(*auth_error);
                                }
                                else if (code != CURLE_OK)
                                {
                                    ErrorCategory category = ErrorCategory::network;
                                    std::string message = curl_easy_strerror(code);
                                    bool retryable = true;
                                    if (token.stop_requested())
                                    {
                                        category = ErrorCategory::cancelled;
                                        message = "HTTP request cancelled";
                                        retryable = false;
                                    }
                                    else if (code == CURLE_OPERATION_TIMEDOUT)
                                    {
                                        category = ErrorCategory::timeout;
                                    }
                                    else if (
                                        code == CURLE_PEER_FAILED_VERIFICATION ||
                                        code == CURLE_SSL_CONNECT_ERROR || code == CURLE_SSL_CERTPROBLEM
                                    )
                                    {
                                        category = ErrorCategory::tls;
                                    }
                                    else if (write_context.exceeded)
                                    {
                                        category = ErrorCategory::malformed_http;
                                        message = "HTTP response size limit exceeded";
                                        retryable = false;
                                    }
                                    // A peer that accepts the connection and then hangs up
                                    // mid-exchange, but only once credentials are attached, is
                                    // almost always an embedded HTTP server that cannot process
                                    // the Digest challenge. "Server returned nothing" gives the
                                    // caller no way to reach that conclusion on their own.
                                    else if (
                                        attempted_cleartext_digest &&
                                        (code == CURLE_GOT_NOTHING || code == CURLE_RECV_ERROR ||
                                         code == CURLE_SEND_ERROR ||
                                         code == CURLE_WEIRD_SERVER_REPLY ||
                                         code == CURLE_PARTIAL_FILE)
                                    )
                                    {
                                        message +=
                                            " -- the device dropped an authenticated request and "
                                            "may not support HTTP Digest authentication, which is "
                                            "all this client offers over cleartext HTTP. Set "
                                            "Credentials::allow_cleartext_basic to offer the full "
                                            "authentication set; devices using a WS-Security "
                                            "UsernameToken send no HTTP challenge, so no password "
                                            "is transmitted in cleartext.";
                                        // Repeating an identical request reproduces this exactly,
                                        // so it is not retryable. Callers also use the cleared
                                        // flag to tell this apart from a transient network fault.
                                        retryable = false;
                                    }
                                    result = Error{category, {}, std::move(message), retryable};
                                }
                                detail::dispatch(
                                    rt,
                                    [done = std::move(done), result = std::move(result)]() mutable
                                    {
                                        done(std::move(result));
                                    }
                                );
                            }
                        ).detach();
                    }
                );
            }

          private:
            std::shared_ptr<Runtime> runtime_;
            XmlLimits limits_;
            std::shared_ptr<CandidateAuthorizer> authorizer_;
        };
    } // namespace
    std::shared_ptr<HttpTransport> make_curl_transport(std::shared_ptr<Runtime> r, XmlLimits l)
    {
        return std::make_shared<CurlTransport>(std::move(r), l);
    }
    std::shared_ptr<HttpTransport> make_curl_transport(CurlTransportOptions options)
    {
        return std::make_shared<CurlTransport>(
            std::move(options.runtime), options.limits, std::move(options.candidate_authorizer)
        );
    }
} // namespace wlonvif
