#include <algorithm>
#include <cctype>
#include <ctime>
#include <future>
#include "device_clock.hpp"
#include "dispatch.hpp"
#include "network_services.hpp"
#include "ptz_safety.hpp"
#include "soap_response.hpp"
#include "xml_scan.hpp"
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    namespace
    {
        std::string origin(std::string_view u)
        {
            auto p = u.find("://");
            if (p == u.npos)
            {
                return {};
            }
            auto e = u.find('/', p + 3);
            std::string result(u.substr(0, e));
            std::transform(
                result.begin(),
                result.end(),
                result.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
            );
            return result;
        }
        struct UrlParts
        {
            std::string scheme;
            std::string host;
            bool user_info{};
        };
        std::optional<UrlParts> url_parts(std::string_view url)
        {
            const auto separator = url.find("://");
            if (separator == url.npos)
            {
                return std::nullopt;
            }
            UrlParts parts;
            parts.scheme = std::string(url.substr(0, separator));
            auto authority = url.substr(separator + 3);
            authority = authority.substr(0, authority.find_first_of("/?#"));
            parts.user_info = authority.find('@') != authority.npos;
            if (parts.user_info)
            {
                authority.remove_prefix(authority.rfind('@') + 1);
            }
            if (authority.empty())
            {
                return std::nullopt;
            }
            if (authority.front() == '[')
            {
                const auto closing = authority.find(']');
                if (closing == authority.npos)
                {
                    return std::nullopt;
                }
                parts.host = std::string(authority.substr(0, closing + 1));
            }
            else
            {
                parts.host = std::string(authority.substr(0, authority.find(':')));
            }
            const auto lowercase = [](unsigned char value)
            {
                return static_cast<char>(std::tolower(value));
            };
            std::transform(
                parts.scheme.begin(), parts.scheme.end(), parts.scheme.begin(), lowercase
            );
            std::transform(parts.host.begin(), parts.host.end(), parts.host.begin(), lowercase);
            return parts.host.empty() ? std::nullopt : std::optional<UrlParts>(std::move(parts));
        }
        class SameOrigin final : public EndpointPolicy
        {
            Result<void> authorize(std::string_view u, std::string_view o) const override
            {
                if (u.find('@') != u.npos)
                {
                    return Error{ErrorCategory::endpoint_rejected, {}, "URL user-info rejected"};
                }
                const auto candidate = origin(u);
                // A URL with no scheme has an empty origin, which would otherwise
                // compare equal to another empty origin and authorize itself.
                if (candidate.empty() || candidate != origin(o))
                {
                    return Error{
                        ErrorCategory::endpoint_rejected, {}, "cross-origin endpoint rejected"
                    };
                }
                return {};
            }
        };
        class SameHost final : public EndpointPolicy
        {
            Result<void> authorize(std::string_view url, std::string_view original) const override
            {
                const auto candidate = url_parts(url);
                const auto source = url_parts(original);
                if (!candidate || !source || candidate->user_info ||
                    (candidate->scheme != "http" && candidate->scheme != "https") ||
                    candidate->host != source->host)
                {
                    return Error{
                        ErrorCategory::endpoint_rejected,
                        {},
                        "endpoint is not an authorized HTTP(S) URL on the device host"
                    };
                }
                return {};
            }
        };
        /**
         * @brief Return the text of the first element with local name @p n.
         *
         * Devices vary the namespace prefix they emit for the same element
         * (`tds:`, `ns2:`, or none at all), so the prefix is ignored entirely.
         */
        std::string field(std::string_view x, std::string_view n, std::size_t from = 0)
        {
            return detail_xml::first_text(x, n, from);
        }
        template <class T, class Decode>
        Task<Result<T>> decoded(Task<Result<std::string>> source, Decode decode)
        {
            return Task<Result<T>>(
                [source = std::move(source), decode](auto done) mutable
                {
                    std::move(source).start(
                        [decode, done = std::move(done)](Result<std::string> r) mutable
                        {
                            if (!r)
                            {
                                done(r.error());
                                return;
                            }
                            done(decode(r.value()));
                        }
                    );
                }
            );
        }
    } // namespace
    std::shared_ptr<EndpointPolicy> make_same_origin_policy()
    {
        return std::make_shared<SameOrigin>();
    }
    std::shared_ptr<EndpointPolicy> make_same_host_policy()
    {
        return std::make_shared<SameHost>();
    }
    Task<Result<BinaryData>> download_binary(
        std::string url,
        std::string original_origin,
        std::shared_ptr<HttpTransport> transport,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        std::optional<Credentials> credentials,
        std::size_t maximum_bytes,
        CallOptions options
    )
    {
        return Task<Result<BinaryData>>(
            [url = std::move(url),
             original_origin = std::move(original_origin),
             transport = std::move(transport),
             endpoint_policy = std::move(endpoint_policy),
             credentials = std::move(credentials),
             maximum_bytes,
             options](auto completion) mutable
            {
                if (maximum_bytes == 0)
                {
                    completion(Error{
                        ErrorCategory::invalid_argument, {}, "maximum download size must be positive"
                    });
                    return;
                }
                auto authorized = endpoint_policy->authorize(url, original_origin);
                if (!authorized)
                {
                    completion(authorized.error());
                    return;
                }
                HttpRequest request{
                    std::move(url),
                    {},
                    {},
                    options.deadline,
                    std::move(credentials),
                    HttpMethod::get
                };
                auto source = transport->send(std::move(request), options.cancellation);
                std::move(source).start(
                    [maximum_bytes, completion = std::move(completion)](
                        Result<HttpResponse> response
                    ) mutable
                    {
                        if (!response)
                        {
                            completion(response.error());
                            return;
                        }
                        if (response.value().status == 401)
                        {
                            Error error{ErrorCategory::authentication, {}, "authentication failed"};
                            error.http = HttpErrorDetail{response.value().status};
                            completion(std::move(error));
                            return;
                        }
                        if (response.value().status == 403)
                        {
                            Error error{ErrorCategory::authorization, {}, "request was forbidden"};
                            error.http = HttpErrorDetail{response.value().status};
                            completion(std::move(error));
                            return;
                        }
                        if (response.value().status == 429)
                        {
                            Error error{ErrorCategory::rate_limited, {}, "HTTP rate limit exceeded", true};
                            error.http = HttpErrorDetail{response.value().status};
                            completion(std::move(error));
                            return;
                        }
                        if (response.value().status < 200 || response.value().status >= 300)
                        {
                            Error error{
                                ErrorCategory::protocol_violation,
                                {},
                                "unexpected HTTP status " +
                                    std::to_string(response.value().status)
                            };
                            error.http = HttpErrorDetail{response.value().status};
                            completion(std::move(error));
                            return;
                        }
                        if (response.value().body.size() > maximum_bytes)
                        {
                            completion(Error{
                                ErrorCategory::malformed_http, {}, "download size limit exceeded"
                            });
                            return;
                        }
                        completion(BinaryData{
                            std::move(response.value().body),
                            std::move(response.value().content_type)
                        });
                    }
                );
            }
        );
    }
    DeviceSession::DeviceSession(
        std::string u,
        std::shared_ptr<HttpTransport> t,
        std::shared_ptr<EndpointPolicy> p,
        std::shared_ptr<Runtime> r,
        std::shared_ptr<CredentialProvider> c
    )
        : url_(std::move(u)), transport_(std::move(t)), endpoint_policy_(std::move(p)),
          runtime_(std::move(r)), credential_provider_(std::move(c)),
          clock_(std::make_shared<detail::DeviceClock>())
    {
    }
    DeviceSession::~DeviceSession()
    {
        if (runtime_ && runtime_shutdown_registration_)
            runtime_->unregister_shutdown_participant(runtime_shutdown_registration_);
        session_cancellation_.request_stop();
        if (credential_provider_) credential_provider_->close();
    }
    Task<Result<std::shared_ptr<DeviceSession>>> DeviceSession::connect(
        std::string u,
        std::shared_ptr<HttpTransport> t,
        std::shared_ptr<EndpointPolicy> p,
        std::shared_ptr<Runtime> r,
        std::optional<Credentials> c,
        CallOptions options
    )
    {
        return Task<Result<std::shared_ptr<DeviceSession>>>(
            [u = std::move(u),
             t = std::move(t),
             p = std::move(p),
             r = std::move(r),
             c = std::move(c),
             options](auto done) mutable
            {
                auto work =
                    [u = std::move(u),
                     t = std::move(t),
                     p = std::move(p),
                     r,
                     c = std::move(c),
                     options,
                     done = std::move(done)]() mutable
                {
                    if (u.empty() || !t || !p || !r)
                    {
                        done(Error{ErrorCategory::invalid_argument, {}, "incomplete session options"});
                        return;
                    }
                    if (options.cancellation.stop_requested())
                    {
                        done(Error{ErrorCategory::cancelled});
                        return;
                    }
                    if (std::chrono::steady_clock::now() >= options.deadline)
                    {
                        done(Error{ErrorCategory::timeout});
                        return;
                    }
                    auto a = p->authorize(u, origin(u));
                    if (!a)
                    {
                        done(a.error());
                        return;
                    }
                    auto provider = c ? make_fixed_credential_provider(std::move(*c)) : nullptr;
                    auto session = std::shared_ptr<DeviceSession>(new DeviceSession(
                        std::move(u), std::move(t), std::move(p), std::move(r), std::move(provider)
                    ));
                    session->register_runtime_shutdown();
                    done(std::move(session));
                };
                auto runtime = r;
                detail::dispatch(runtime, std::move(work));
            }
        );
    }
    Task<Result<std::shared_ptr<DeviceSession>>> DeviceSession::connect(DeviceSessionOptions options)
    {
        return Task<Result<std::shared_ptr<DeviceSession>>>(
            [options = std::move(options)](auto done) mutable
            {
                if (options.device_url.empty() || !options.transport || !options.endpoint_policy ||
                    !options.runtime)
                {
                    done(Error{ErrorCategory::invalid_argument, {}, "incomplete session options"});
                    return;
                }
                if (options.call.cancellation.stop_requested())
                {
                    done(Error{ErrorCategory::cancelled});
                    return;
                }
                if (std::chrono::steady_clock::now() >= options.call.deadline)
                {
                    done(Error{ErrorCategory::timeout});
                    return;
                }
                auto authorized = options.endpoint_policy->authorize(
                    options.device_url, origin(options.device_url)
                );
                if (!authorized) { done(authorized.error()); return; }
                auto session = std::shared_ptr<DeviceSession>(new DeviceSession(
                    std::move(options.device_url), std::move(options.transport),
                    std::move(options.endpoint_policy), std::move(options.runtime),
                    std::move(options.credential_provider)
                ));
                session->register_runtime_shutdown();
                done(std::move(session));
            }
        );
    }
    void DeviceSession::register_runtime_shutdown()
    {
        std::weak_ptr<DeviceSession> weak = shared_from_this();
        runtime_shutdown_registration_ = runtime_->register_shutdown_participant([weak]
        {
            if (auto session = weak.lock()) session->shutdown_from_runtime();
        });
    }
    void DeviceSession::shutdown_from_runtime() noexcept
    {
        try
        {
            auto promise = std::make_shared<std::promise<Result<CloseResult>>>();
            auto future = promise->get_future();
            std::move(close(std::chrono::seconds(5))).start(
                [promise](Result<CloseResult> result) { promise->set_value(std::move(result)); }
            );
            (void)future.wait_for(std::chrono::seconds(6));
        }
        catch (...) {}
    }
    bool DeviceSession::is_closed() const noexcept
    {
        std::lock_guard lock(state_mutex_);
        return state_ != State::open;
    }
    Task<Result<CloseResult>> DeviceSession::close(std::chrono::milliseconds cleanup_budget)
    {
        return Task<Result<CloseResult>>(
            [self = shared_from_this(), cleanup_budget](auto done) mutable
            {
                std::shared_ptr<PtzSafetyState> safety_candidate;
                {
                    std::lock_guard lock(self->backend_mutex_);
                    safety_candidate = self->ptz_safety_;
                }
                auto completed = std::make_shared<std::atomic_bool>(false);
                auto terminal = [done = std::move(done), completed](Result<CloseResult> result) mutable
                {
                    if (!completed->exchange(true)) done(std::move(result));
                };
                bool immediate = false;
                bool run_safety = false;
                bool close_credentials = false;
                {
                    std::lock_guard lock(self->state_mutex_);
                    if (self->state_ == State::closed) immediate = true;
                    if (self->state_ == State::open)
                    {
                        self->state_ = State::closing;
                        self->session_cancellation_.request_stop();
                        close_credentials = static_cast<bool>(self->credential_provider_);
                        if (safety_candidate)
                        {
                            ++self->active_operations_;
                            self->close_safety_complete_ = false;
                            run_safety = true;
                        }
                    }
                    if (self->active_operations_ == 0)
                    {
                        self->state_ = State::closed;
                        immediate = true;
                    }
                    if (!immediate) self->close_waiters_.push_back(terminal);
                }
                // CredentialProvider is an extension point and may synchronously
                // complete pending acquisitions from close(). Never call it while
                // holding state_mutex_, because those completions finish operations
                // by acquiring the same mutex. The safety Stops below authenticate
                // with these credentials, so when they run the provider is closed
                // only after they finish; closing it first fails every Stop sent to
                // a camera that requires authentication.
                if (close_credentials && !run_safety) self->credential_provider_->close();
                if (run_safety)
                {
                    auto safety = std::move(safety_candidate);
                    std::thread([self, safety, close_credentials]() mutable
                    {
                        const bool complete = safety->close_all(
                            std::chrono::steady_clock::now() + std::chrono::seconds(5)
                        );
                        if (close_credentials) self->credential_provider_->close();
                        {
                            std::lock_guard lock(self->state_mutex_);
                            self->close_safety_complete_ = complete;
                        }
                        self->operation_finished();
                    }).detach();
                }
                if (immediate)
                {
                    bool safety_complete = false;
                    {
                        std::lock_guard lock(self->state_mutex_);
                        safety_complete = self->close_safety_complete_;
                    }
                    terminal(CloseResult{true, safety_complete});
                    return;
                }
                if (cleanup_budget <= std::chrono::milliseconds::zero())
                {
                    terminal(CloseResult{false, false});
                    return;
                }
                std::thread([terminal = std::move(terminal), cleanup_budget]() mutable
                {
                    std::this_thread::sleep_for(cleanup_budget);
                    terminal(CloseResult{false, false});
                }).detach();
            }
        );
    }
    void DeviceSession::operation_finished() noexcept
    {
        std::vector<std::function<void(Result<CloseResult>)>> waiters;
        bool safety_complete = false;
        {
            std::lock_guard lock(state_mutex_);
            if (active_operations_ != 0) --active_operations_;
            if (state_ == State::closing && active_operations_ == 0)
            {
                state_ = State::closed;
                waiters.swap(close_waiters_);
            }
            safety_complete = close_safety_complete_;
        }
        for (auto& waiter : waiters) waiter(CloseResult{true, safety_complete});
    }
    Task<Result<std::string>>
    DeviceSession::invoke(OperationId op, std::string_view response, CallOptions options)
    {
        return invoke(op, {}, response, options, true);
    }
    Task<Result<std::string>> DeviceSession::invoke(
        OperationId op, std::string body, std::string_view response, CallOptions options,
        bool idempotent
    )
    {
        return Task<Result<std::string>>(
            [self = shared_from_this(),
             op,
             body = std::move(body),
             response = std::string(response),
             options,
             idempotent](auto done) mutable
            {
                {
                    std::lock_guard lock(self->state_mutex_);
                    if (self->state_ != State::open)
                    {
                        done(Error{ErrorCategory::closed_handle, op, "session is closed"});
                        return;
                    }
                    ++self->active_operations_;
                }
                auto caller_done = std::move(done);
                auto settled = std::make_shared<std::atomic_bool>(false);
                done = [self, caller_done = std::move(caller_done), settled](Result<std::string> result) mutable
                {
                    if (settled->exchange(true)) return;
                    self->operation_finished();
                    caller_done(std::move(result));
                };
                auto linked = CancellationToken::linked(
                    options.cancellation, self->session_cancellation_.token()
                );
                auto send_signed = [self, op, body = std::move(body), response = std::move(response),
                                    options, linked, idempotent, done = std::move(done)](
                                       Result<std::shared_ptr<CredentialLease>> acquired
                                    ) mutable
                {
                    if (!acquired) { auto error = acquired.error(); error.operation = op; done(std::move(error)); return; }
                    auto lease = std::move(acquired.value());
                    std::string envelope;
                    try { envelope = soap_envelope(op, body, lease.get(), self->clock_->offset()); }
                    catch (const std::exception& error)
                    { done(Error{ErrorCategory::internal, op, error.what()}); return; }
                    HttpRequest q{
                    self->url_,
                    std::move(envelope),
                    {"Content-Type: application/soap+xml; charset=utf-8"},
                    options.deadline
                    };
                    q.credential_lease = std::move(lease);
                    q.original_url = self->url_;
                    q.operation = op;
                    q.idempotent = idempotent;
                    q.retry_allowed = false;
                    auto source = self->transport_->send(std::move(q), linked);
                    std::move(source).start(
                    [response = std::move(response),
                     op,
                     done = std::move(done)](Result<HttpResponse> r) mutable
                    {
                        if (!r)
                        {
                            auto e = r.error();
                            e.operation = op;
                            done(std::move(e));
                            return;
                        }
                        // Hand the decoders only the response element's own content.
                        // Searching the whole envelope would let a SOAP Header, or
                        // anything following the response, shadow the real fields.
                        done(detail::soap_response_content(r.value(), response, op));
                    }
                    );
                };
                auto send_with = [self, options, linked, send_signed = std::move(send_signed)](
                                     Result<std::shared_ptr<CredentialLease>> acquired
                                 ) mutable
                {
                    // Measure the device clock before the first signed request so
                    // the UsernameToken Created timestamp is in device time.
                    if (!acquired || !acquired.value() || !self->clock_->needs_measurement())
                    {
                        send_signed(std::move(acquired));
                        return;
                    }
                    auto measurement = self->transport_->send(
                        detail::clock_request(self->url_, options.deadline), linked
                    );
                    std::move(measurement).start(
                        [self, acquired = std::move(acquired),
                         send_signed = std::move(send_signed)](Result<HttpResponse> r) mutable
                        {
                            self->clock_->record(r, std::chrono::system_clock::now());
                            send_signed(std::move(acquired));
                        }
                    );
                };
                if (!self->credential_provider_)
                {
                    send_with(std::shared_ptr<CredentialLease>{});
                    return;
                }
                auto acquisition = self->credential_provider_->acquire({options.deadline, linked});
                std::move(acquisition).start(
                    [send_with = std::move(send_with)](Result<CredentialLease> result) mutable
                    {
                        if (!result) { send_with(result.error()); return; }
                        send_with(std::make_shared<CredentialLease>(std::move(result.value())));
                    }
                );
            }
        );
    }
    Task<Result<DeviceInformation>> DeviceSession::get_device_information(CallOptions o)
    {
        return decoded<DeviceInformation>(
            invoke(OperationId::get_device_information, "GetDeviceInformationResponse", o),
            [](auto& x)
            {
                return DeviceInformation{
                    field(x, "Manufacturer"),
                    field(x, "Model"),
                    field(x, "FirmwareVersion"),
                    field(x, "SerialNumber"),
                    field(x, "HardwareId")
                };
            }
        );
    }
    Task<Result<SystemDateAndTime>> DeviceSession::get_system_date_and_time(CallOptions o)
    {
        return decoded<SystemDateAndTime>(
            invoke(OperationId::get_system_date_and_time, "GetSystemDateAndTimeResponse", o),
            [](auto& x) { return detail::decode_system_date_and_time(x); }
        );
    }
    Task<Result<std::vector<std::string>>> DeviceSession::get_scopes(CallOptions o)
    {
        return decoded<std::vector<std::string>>(
            invoke(OperationId::get_scopes, "GetScopesResponse", o),
            [](auto& x)
            {
                std::vector<std::string> v;
                detail_xml::for_each_element(
                    x,
                    "ScopeItem",
                    [&](const detail_xml::Element& item)
                    {
                        v.push_back(detail_xml::text(x, item));
                    }
                );
                return v;
            }
        );
    }
    namespace
    {
        template <class T> Task<Result<T>> failed(Error error)
        {
            return Task<Result<T>>([error = std::move(error)](auto done) mutable
            {
                done(std::move(error));
            });
        }

        Error invalid_argument(OperationId operation, std::string message)
        {
            return Error{ErrorCategory::invalid_argument, operation, std::move(message)};
        }

        /// Serialize scope URIs as @p element children, rejecting unusable values.
        Result<std::string> scope_elements(
            const std::vector<std::string>& scopes, std::string_view element, OperationId operation
        )
        {
            if (scopes.empty()) return invalid_argument(operation, "no scopes given");
            std::string output;
            for (const auto& scope : scopes)
            {
                // Discovery advertises scopes as one whitespace-separated list, so a
                // scope containing whitespace would read back as several scopes.
                if (scope.empty() ||
                    std::any_of(scope.begin(), scope.end(),
                                [](unsigned char c) { return std::isspace(c) || std::iscntrl(c); }))
                {
                    return invalid_argument(
                        operation, "scope URIs must be non-empty and contain no whitespace"
                    );
                }
                output += "<tds:" + std::string(element) + '>' + detail_xml::escape(scope) +
                          "</tds:" + std::string(element) + '>';
            }
            return output;
        }

        std::string number_element(std::string_view name, long long value)
        {
            return "<tt:" + std::string(name) + '>' + std::to_string(value) + "</tt:" +
                   std::string(name) + '>';
        }

        std::string utc_date_time(std::chrono::system_clock::time_point value)
        {
            const auto seconds = std::chrono::floor<std::chrono::seconds>(value);
            const auto day = std::chrono::floor<std::chrono::days>(seconds);
            const std::chrono::year_month_day date{day};
            const std::chrono::hh_mm_ss time{seconds - day};
            return "<tds:UTCDateTime><tt:Time>" + number_element("Hour", time.hours().count()) +
                   number_element("Minute", time.minutes().count()) +
                   number_element("Second", time.seconds().count()) + "</tt:Time><tt:Date>" +
                   number_element("Year", static_cast<int>(date.year())) +
                   number_element("Month", static_cast<unsigned>(date.month())) +
                   number_element("Day", static_cast<unsigned>(date.day())) +
                   "</tt:Date></tds:UTCDateTime>";
        }

        /// Serialize an NTP server as a tt:NetworkHost of the matching type.
        std::string network_host(const std::string& host)
        {
            const bool ipv6 = host.find(':') != std::string::npos;
            const bool ipv4 = !ipv6 && std::count(host.begin(), host.end(), '.') == 3 &&
                              std::all_of(host.begin(), host.end(), [](unsigned char c)
                                          { return std::isdigit(c) || c == '.'; });
            const std::string_view type = ipv6 ? "IPv6" : ipv4 ? "IPv4" : "DNS";
            const std::string_view element = ipv6 ? "IPv6Address" : ipv4 ? "IPv4Address" : "DNSname";
            return "<tds:NTPManual><tt:Type>" + std::string(type) + "</tt:Type><tt:" +
                   std::string(element) + '>' + detail_xml::escape(host) + "</tt:" +
                   std::string(element) + "></tds:NTPManual>";
        }

        /// Read the address of every NetworkHost element named @p name.
        std::vector<std::string> network_hosts(std::string_view xml, std::string_view name)
        {
            std::vector<std::string> output;
            detail_xml::for_each_element(
                xml,
                name,
                [&](const detail_xml::Element& element)
                {
                    const auto host = std::string(detail_xml::inner(xml, element));
                    for (const auto field : {"DNSname", "IPv4Address", "IPv6Address"})
                    {
                        auto value = detail_xml::first_text(host, field);
                        if (!value.empty())
                        {
                            output.push_back(std::move(value));
                            break;
                        }
                    }
                }
            );
            return output;
        }
    } // namespace
    Task<Result<std::vector<ScopeEntry>>> DeviceSession::get_scope_entries(CallOptions o)
    {
        return decoded<std::vector<ScopeEntry>>(
            invoke(OperationId::get_scopes, "GetScopesResponse", o),
            [](auto& x)
            {
                std::vector<ScopeEntry> entries;
                detail_xml::for_each_element(
                    x,
                    "Scopes",
                    [&](const detail_xml::Element& element)
                    {
                        const auto scope = std::string(detail_xml::inner(x, element));
                        ScopeEntry entry;
                        entry.uri = field(scope, "ScopeItem");
                        entry.definition = field(scope, "ScopeDef") == "Configurable"
                                               ? ScopeDefinition::configurable
                                               : ScopeDefinition::fixed;
                        if (!entry.uri.empty()) entries.push_back(std::move(entry));
                    }
                );
                return entries;
            }
        );
    }
    Task<Result<void>> DeviceSession::set_scopes(std::vector<std::string> scopes, CallOptions o)
    {
        auto body = scope_elements(scopes, "Scopes", OperationId::set_scopes);
        if (!body) return failed<void>(body.error());
        return decoded<void>(
            invoke(OperationId::set_scopes, std::move(body.value()), "SetScopesResponse", o, true),
            [](auto&) { return Result<void>{}; }
        );
    }
    Task<Result<void>> DeviceSession::add_scopes(std::vector<std::string> scopes, CallOptions o)
    {
        auto body = scope_elements(scopes, "ScopeItem", OperationId::add_scopes);
        if (!body) return failed<void>(body.error());
        return decoded<void>(
            invoke(OperationId::add_scopes, std::move(body.value()), "AddScopesResponse", o, false),
            [](auto&) { return Result<void>{}; }
        );
    }
    Task<Result<void>> DeviceSession::remove_scopes(std::vector<std::string> scopes, CallOptions o)
    {
        auto body = scope_elements(scopes, "ScopeItem", OperationId::remove_scopes);
        if (!body) return failed<void>(body.error());
        return decoded<void>(
            invoke(
                OperationId::remove_scopes, std::move(body.value()), "RemoveScopesResponse", o, false
            ),
            [](auto&) { return Result<void>{}; }
        );
    }
    Task<Result<void>> DeviceSession::set_system_date_and_time(
        SystemDateAndTimeSettings settings, CallOptions o
    )
    {
        constexpr auto operation = OperationId::set_system_date_and_time;
        const bool manual = settings.type == DateTimeType::manual;
        if (manual != settings.utc.has_value())
        {
            return failed<void>(invalid_argument(
                operation, manual ? "manual time requires a UTC time"
                                  : "a UTC time can only be set with manual time"
            ));
        }
        if (settings.time_zone &&
            (settings.time_zone->empty() ||
             std::any_of(settings.time_zone->begin(), settings.time_zone->end(),
                         [](unsigned char c) { return std::iscntrl(c) || std::isspace(c); })))
        {
            return failed<void>(invalid_argument(operation, "time zone must be a POSIX TZ string"));
        }
        std::string body = std::string("<tds:DateTimeType>") + (manual ? "Manual" : "NTP") +
                           "</tds:DateTimeType><tds:DaylightSavings>" +
                           (settings.daylight_savings ? "true" : "false") +
                           "</tds:DaylightSavings>";
        if (settings.time_zone)
        {
            body += "<tds:TimeZone><tt:TZ>" + detail_xml::escape(*settings.time_zone) +
                    "</tt:TZ></tds:TimeZone>";
        }
        if (settings.utc) body += utc_date_time(*settings.utc);
        return decoded<void>(
            invoke(operation, std::move(body), "SetSystemDateAndTimeResponse", o, true),
            [self = shared_from_this()](auto&)
            {
                // Signed requests are stamped in device time; it has just moved.
                self->clock_->invalidate();
                return Result<void>{};
            }
        );
    }
    Task<Result<NtpConfiguration>> DeviceSession::get_ntp(CallOptions o)
    {
        return decoded<NtpConfiguration>(
            invoke(OperationId::get_ntp, "GetNTPResponse", o),
            [](auto& x)
            {
                NtpConfiguration configuration;
                configuration.from_dhcp = field(x, "FromDHCP") == "true";
                configuration.servers = network_hosts(x, "NTPManual");
                configuration.dhcp_servers = network_hosts(x, "NTPFromDHCP");
                return configuration;
            }
        );
    }
    Task<Result<void>> DeviceSession::set_ntp(NtpConfiguration configuration, CallOptions o)
    {
        constexpr auto operation = OperationId::set_ntp;
        if (configuration.from_dhcp != configuration.servers.empty())
        {
            return failed<void>(invalid_argument(
                operation, configuration.from_dhcp
                               ? "servers cannot be configured while using DHCP"
                               : "at least one NTP server is required without DHCP"
            ));
        }
        std::string body = std::string("<tds:FromDHCP>") +
                           (configuration.from_dhcp ? "true" : "false") + "</tds:FromDHCP>";
        for (const auto& server : configuration.servers)
        {
            if (server.empty() ||
                std::any_of(server.begin(), server.end(),
                            [](unsigned char c) { return std::isspace(c) || std::iscntrl(c); }))
            {
                return failed<void>(invalid_argument(operation, "invalid NTP server: " + server));
            }
            body += network_host(server);
        }
        return decoded<void>(
            invoke(operation, std::move(body), "SetNTPResponse", o, true),
            [](auto&) { return Result<void>{}; }
        );
    }
    Task<Result<HostnameInformation>> DeviceSession::get_hostname(CallOptions o)
    {
        return decoded<HostnameInformation>(
            invoke(OperationId::get_hostname, "GetHostnameResponse", o),
            [](auto& x)
            {
                return HostnameInformation{field(x, "FromDHCP") == "true", field(x, "Name")};
            }
        );
    }
    Task<Result<void>> DeviceSession::set_hostname(std::string name, CallOptions o)
    {
        constexpr auto operation = OperationId::set_hostname;
        // RFC 1123: dot-separated labels of letters, digits, and inner hyphens.
        bool valid = !name.empty() && name.size() <= 253;
        std::size_t label = 0;
        for (std::size_t index = 0; valid && index <= name.size(); ++index)
        {
            const char c = index < name.size() ? name[index] : '.';
            if (c == '.')
            {
                valid = label > 0 && label <= 63 && name[index - 1] != '-';
                label = 0;
            }
            else
            {
                valid = std::isalnum(static_cast<unsigned char>(c)) || (c == '-' && label > 0);
                ++label;
            }
        }
        if (!valid) return failed<void>(invalid_argument(operation, "invalid hostname: " + name));
        return decoded<void>(
            invoke(operation, "<tds:Name>" + name + "</tds:Name>", "SetHostnameResponse", o, true),
            [](auto&) { return Result<void>{}; }
        );
    }
    Task<Result<std::vector<NetworkInterface>>> DeviceSession::get_network_interfaces(CallOptions o)
    {
        return decoded<std::vector<NetworkInterface>>(
            invoke(OperationId::get_network_interfaces, "GetNetworkInterfacesResponse", o),
            [](auto& x)
            {
                std::vector<NetworkInterface> output;
                detail_xml::for_each_element(
                    x,
                    "NetworkInterfaces",
                    [&](const detail_xml::Element& element)
                    {
                        const auto body = std::string(detail_xml::inner(x, element));
                        NetworkInterface item;
                        item.token = detail_xml::attribute(detail_xml::attributes(x, element), "token");
                        item.enabled = field(body, "Enabled") == "true";
                        if (const auto info = detail_xml::find_element(body, "Info"))
                        {
                            const auto content = std::string(detail_xml::inner(body, *info));
                            item.name = field(content, "Name");
                            item.hardware_address = field(content, "HwAddress");
                            item.mtu = std::atoi(field(content, "MTU").c_str());
                        }
                        if (const auto ipv4 = detail_xml::find_element(body, "IPv4"))
                        {
                            const auto content = std::string(detail_xml::inner(body, *ipv4));
                            item.ipv4_enabled = field(content, "Enabled") == "true";
                            item.ipv4_dhcp = field(content, "DHCP") == "true";
                            // Manual, LinkLocal, and FromDHCP all hold Address/PrefixLength.
                            for (const auto kind : {"Manual", "LinkLocal", "FromDHCP"})
                                detail_xml::for_each_element(
                                    content, kind,
                                    [&](const detail_xml::Element& address)
                                    {
                                        const auto entry = std::string(detail_xml::inner(content, address));
                                        const auto value = field(entry, "Address");
                                        if (value.empty()) return;
                                        const auto prefix = field(entry, "PrefixLength");
                                        item.ipv4_addresses.push_back(prefix.empty() ? value : value + "/" + prefix);
                                    }
                                );
                        }
                        output.push_back(std::move(item));
                    }
                );
                return output;
            }
        );
    }
    Task<Result<DnsInformation>> DeviceSession::get_dns(CallOptions o)
    {
        return decoded<DnsInformation>(
            invoke(OperationId::get_dns, "GetDNSResponse", o),
            [](auto& x)
            {
                DnsInformation output;
                output.from_dhcp = field(x, "FromDHCP") == "true";
                detail_xml::for_each_element(
                    x, "SearchDomain",
                    [&](const detail_xml::Element& domain) { output.search_domains.push_back(detail_xml::text(x, domain)); }
                );
                output.servers = network_hosts(x, "DNSManual");
                output.dhcp_servers = network_hosts(x, "DNSFromDHCP");
                return output;
            }
        );
    }
    Task<Result<std::vector<std::string>>> DeviceSession::get_network_default_gateway(CallOptions o)
    {
        return decoded<std::vector<std::string>>(
            invoke(OperationId::get_network_default_gateway, "GetNetworkDefaultGatewayResponse", o),
            [](auto& x)
            {
                std::vector<std::string> output;
                for (const auto kind : {"IPv4Address", "IPv6Address"})
                    detail_xml::for_each_element(
                        x, kind,
                        [&](const detail_xml::Element& address)
                        {
                            auto value = detail_xml::text(x, address);
                            if (!value.empty()) output.push_back(std::move(value));
                        }
                    );
                return output;
            }
        );
    }
    Task<Result<std::vector<NetworkProtocol>>> DeviceSession::get_network_protocols(CallOptions o)
    {
        return decoded<std::vector<NetworkProtocol>>(
            invoke(OperationId::get_network_protocols, "GetNetworkProtocolsResponse", o),
            [](auto& x)
            {
                std::vector<NetworkProtocol> output;
                detail_xml::for_each_element(
                    x, "NetworkProtocols",
                    [&](const detail_xml::Element& element)
                    {
                        const auto body = std::string(detail_xml::inner(x, element));
                        NetworkProtocol protocol{field(body, "Name"), field(body, "Enabled") == "true", {}};
                        detail_xml::for_each_element(
                            body, "Port",
                            [&](const detail_xml::Element& port)
                            { protocol.ports.push_back(std::atoi(detail_xml::text(body, port).c_str())); }
                        );
                        output.push_back(std::move(protocol));
                    }
                );
                return output;
            }
        );
    }
    Task<Result<DiscoveryMode>> DeviceSession::get_discovery_mode(CallOptions o)
    {
        return decoded<DiscoveryMode>(
            invoke(OperationId::get_discovery_mode, "GetDiscoveryModeResponse", o),
            [](auto& x) -> Result<DiscoveryMode>
            {
                const auto mode = field(x, "DiscoveryMode");
                if (mode == "Discoverable") return DiscoveryMode::discoverable;
                if (mode == "NonDiscoverable") return DiscoveryMode::non_discoverable;
                return Error{ErrorCategory::protocol_violation, OperationId::get_discovery_mode,
                             "unknown discovery mode: " + mode};
            }
        );
    }
    Task<Result<void>> DeviceSession::set_discovery_mode(DiscoveryMode mode, CallOptions o)
    {
        return decoded<void>(
            invoke(OperationId::set_discovery_mode,
                   std::string("<tds:DiscoveryMode>") +
                       (mode == DiscoveryMode::discoverable ? "Discoverable" : "NonDiscoverable") +
                       "</tds:DiscoveryMode>",
                   "SetDiscoveryModeResponse", o, true),
            [](auto&) { return Result<void>{}; }
        );
    }
    Task<Result<std::vector<DeviceUser>>> DeviceSession::get_users(CallOptions o)
    {
        return decoded<std::vector<DeviceUser>>(
            invoke(OperationId::get_users, "GetUsersResponse", o),
            [](auto& x)
            {
                std::vector<DeviceUser> output;
                detail_xml::for_each_element(
                    x, "User",
                    [&](const detail_xml::Element& element)
                    {
                        const auto body = std::string(detail_xml::inner(x, element));
                        output.push_back({field(body, "Username"), field(body, "UserLevel")});
                    }
                );
                return output;
            }
        );
    }
    Task<Result<std::string>> DeviceSession::system_reboot(CallOptions o)
    {
        // Not idempotent: a repeated request could restart the device twice.
        return decoded<std::string>(
            invoke(OperationId::system_reboot, {}, "SystemRebootResponse", o, false),
            [](auto& x) { return field(x, "Message"); }
        );
    }
    Task<Result<std::vector<Service>>> DeviceSession::get_services(CallOptions o)
    {
        return decoded<std::vector<Service>>(
            invoke(OperationId::get_services, "GetServicesResponse", o),
            [](auto& x)
            {
                std::vector<Service> v;
                detail_xml::for_each_element(
                    x,
                    "Service",
                    [&](const detail_xml::Element& service)
                    {
                        const auto part = std::string(detail_xml::inner(x, service));
                        v.push_back({field(part, "Namespace"), field(part, "XAddr")});
                    }
                );
                return v;
            }
        );
    }
    Task<Result<Capabilities>> DeviceSession::get_capabilities(CallOptions o)
    {
        return decoded<Capabilities>(
            invoke(OperationId::get_capabilities, "GetCapabilitiesResponse", o),
            [](auto& x)
            {
                Capabilities c;
                // Each XAddr must be read from inside its own service element;
                // scanning forward from a position picks up the next service's
                // address whenever the preceding one omits it.
                const auto endpoint = [&](std::string_view service)
                    -> std::optional<std::string>
                {
                    const auto element = detail_xml::find_element(x, service);
                    if (!element)
                    {
                        return std::nullopt;
                    }
                    auto address =
                        field(std::string(detail_xml::inner(x, *element)), "XAddr");
                    if (address.empty())
                    {
                        return std::nullopt;
                    }
                    return address;
                };
                c.media_xaddr = endpoint("Media");
                c.ptz_xaddr = endpoint("PTZ");
                c.events_xaddr = endpoint("Events");
                c.analytics_xaddr = endpoint("Analytics");
                c.imaging_xaddr = endpoint("Imaging");
                return c;
            }
        );
    }
    /**
     * @brief Return the session's single service backend, creating it on demand.
     *
     * One backend per session is required, not merely an optimization: it owns the
     * PullPoint subscription and the cached capabilities, so handing out a fresh
     * one per call would open a new subscription on every events() call and repeat
     * GetCapabilities for every service client.
     */
    std::shared_ptr<ServiceBackend> DeviceSession::backend()
    {
        std::lock_guard lock(backend_mutex_);
        if (!backend_)
        {
            backend_ = std::dynamic_pointer_cast<ServiceBackend>(transport_);
        }
        if (!backend_)
        {
            backend_ = make_network_service_backend(
                url_, transport_, endpoint_policy_, runtime_, credential_provider_,
                session_cancellation_.token(), clock_
            );
        }
        return backend_;
    }
    Result<std::shared_ptr<MediaClient>> DeviceSession::media()
    {
        if (is_closed()) return Error{ErrorCategory::closed_handle, {}, "session is closed"};
        return std::make_shared<MediaClient>(runtime_, backend());
    }
    Result<std::shared_ptr<PtzClient>> DeviceSession::ptz()
    {
        if (is_closed()) return Error{ErrorCategory::closed_handle, {}, "session is closed"};
        return std::shared_ptr<PtzClient>(new PtzClient(runtime_, backend(), ptz_safety()));
    }
    Result<std::shared_ptr<EventClient>> DeviceSession::event_client()
    {
        if (is_closed()) return Error{ErrorCategory::closed_handle, {}, "session is closed"};
        return std::make_shared<EventClient>(runtime_, backend());
    }
    std::shared_ptr<PtzSafetyState> DeviceSession::ptz_safety()
    {
        std::lock_guard lock(backend_mutex_);
        if (!backend_)
        {
            backend_ = std::dynamic_pointer_cast<ServiceBackend>(transport_);
            if (!backend_)
                backend_ = make_network_service_backend(
                    url_, transport_, endpoint_policy_, runtime_, credential_provider_,
                    session_cancellation_.token(), clock_
                );
        }
        if (!ptz_safety_)
            ptz_safety_ = std::make_shared<PtzSafetyState>(
                backend_, session_cancellation_.token()
            );
        return ptz_safety_;
    }
    Result<std::shared_ptr<AnalyticsClient>> DeviceSession::analytics()
    {
        if (is_closed()) return Error{ErrorCategory::closed_handle, {}, "session is closed"};
        return std::make_shared<AnalyticsClient>(runtime_, backend());
    }
    Result<std::shared_ptr<ImagingClient>> DeviceSession::imaging()
    {
        if (is_closed()) return Error{ErrorCategory::closed_handle, {}, "session is closed"};
        return std::make_shared<ImagingClient>(runtime_, backend());
    }
    Result<std::shared_ptr<ManagedEventStream>> DeviceSession::events(std::size_t maximum)
    {
        if (is_closed()) return Error{ErrorCategory::closed_handle, {}, "session is closed"};
        return std::make_shared<ManagedEventStream>(runtime_, backend(), maximum);
    }
} // namespace wlonvif
