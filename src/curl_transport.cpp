#include <algorithm>
#include <curl/curl.h>
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
                    if (!context.challenge.empty())
                    {
                        context.challenge += ", ";
                    }
                    context.challenge += value;
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
        class CurlTransport final : public HttpTransport
        {
          public:
            CurlTransport(std::shared_ptr<Runtime> r, XmlLimits l)
                : runtime_(std::move(r)), limits_(l)
            {
                detail::initialize_curl();
            }
            Task<Result<HttpResponse>> send(HttpRequest req, CancellationToken token) override
            {
                auto rt = runtime_;
                auto lim = limits_;
                return Task<Result<HttpResponse>>(
                    [rt, lim, req = std::move(req), token](auto done) mutable
                    {
                        std::thread(
                            [rt, lim, req = std::move(req), token, done = std::move(done)]() mutable
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
                                if (code == CURLE_OK && response.status == 401 && req.credentials)
                                {
                                    const bool encrypted = req.url.starts_with("https://");
                                    const auto challenge = lowercase(header_context.challenge);
                                    const bool basic_offered =
                                        challenge.find("basic") != std::string::npos;
                                    long methods = 0;
                                    if (challenge.find("digest") != std::string::npos)
                                    {
                                        methods |= CURLAUTH_DIGEST;
                                    }
                                    if (challenge.find("negotiate") != std::string::npos)
                                    {
                                        methods |= CURLAUTH_NEGOTIATE;
                                    }
                                    if (challenge.find("ntlm") != std::string::npos)
                                    {
                                        methods |= CURLAUTH_NTLM;
                                    }
                                    // Basic reveals the password to anyone on the path, so it is
                                    // only offered back when the channel is encrypted or the
                                    // caller has explicitly accepted the exposure.
                                    if (basic_offered &&
                                        (encrypted || req.credentials->allow_cleartext_basic))
                                    {
                                        methods |= CURLAUTH_BASIC;
                                    }
                                    if (methods != 0)
                                    {
                                        attempted_cleartext_digest =
                                            !encrypted && (methods & CURLAUTH_DIGEST) != 0;
                                        curl_easy_setopt(
                                            c, CURLOPT_USERNAME, req.credentials->username.c_str()
                                        );
                                        curl_easy_setopt(
                                            c, CURLOPT_PASSWORD, req.credentials->password.c_str()
                                        );
                                        curl_easy_setopt(c, CURLOPT_HTTPAUTH, methods);
                                        code = perform();
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
        };
    } // namespace
    std::shared_ptr<HttpTransport> make_curl_transport(std::shared_ptr<Runtime> r, XmlLimits l)
    {
        return std::make_shared<CurlTransport>(std::move(r), l);
    }
} // namespace wlonvif
