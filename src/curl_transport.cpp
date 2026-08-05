#include <algorithm>
#include <curl/curl.h>
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
        class CurlTransport final : public HttpTransport
        {
          public:
            CurlTransport(std::shared_ptr<Runtime> r, XmlLimits l)
                : runtime_(std::move(r)), limits_(l)
            {
                curl_global_init(CURL_GLOBAL_DEFAULT);
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
                                    rt->post(
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
                                    rt->post(
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
                                if (req.credentials)
                                {
                                    curl_easy_setopt(
                                        c, CURLOPT_USERNAME, req.credentials->username.c_str()
                                    );
                                    curl_easy_setopt(
                                        c, CURLOPT_PASSWORD, req.credentials->password.c_str()
                                    );
                                    const bool encrypted = req.url.starts_with("https://");
                                    const long methods = encrypted ||
                                                                 req.credentials
                                                                     ->allow_cleartext_basic
                                                             ? CURLAUTH_ANY
                                                             : CURLAUTH_DIGEST | CURLAUTH_NTLM;
                                    curl_easy_setopt(c, CURLOPT_HTTPAUTH, methods);
                                }
                                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                req.deadline - std::chrono::steady_clock::now()
                                )
                                                .count();
                                curl_easy_setopt(
                                    c, CURLOPT_TIMEOUT_MS, static_cast<long>(left > 0 ? left : 1)
                                );
                                auto code = curl_easy_perform(c);
                                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &response.status);
                                char* ct = nullptr;
                                curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
                                if (ct)
                                {
                                    response.content_type = ct;
                                }
                                curl_slist_free_all(hs);
                                curl_easy_cleanup(c);
                                Result<HttpResponse> result = std::move(response);
                                if (code != CURLE_OK)
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
                                    result = Error{category, {}, std::move(message), retryable};
                                }
                                rt->post(
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
