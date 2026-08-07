#include <algorithm>
#include <cctype>
#include <ctime>
#include <future>
#include "dispatch.hpp"
#include "network_services.hpp"
#include "ptz_safety.hpp"
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
          runtime_(std::move(r)), credential_provider_(std::move(c))
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
                // by acquiring the same mutex.
                if (close_credentials) self->credential_provider_->close();
                if (run_safety)
                {
                    auto safety = std::move(safety_candidate);
                    std::thread([self, safety]() mutable
                    {
                        const bool complete = safety->close_all(
                            std::chrono::steady_clock::now() + std::chrono::seconds(5)
                        );
                        {
                            std::lock_guard lock(self->state_mutex_);
                            self->close_safety_complete_ = complete;
                        }
                        self->operation_finished();
                    }).detach();
                }
                if (immediate) { terminal(CloseResult{true, self->close_safety_complete_}); return; }
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
        {
            std::lock_guard lock(state_mutex_);
            if (active_operations_ != 0) --active_operations_;
            if (state_ == State::closing && active_operations_ == 0)
            {
                state_ = State::closed;
                waiters.swap(close_waiters_);
            }
        }
        for (auto& waiter : waiters) waiter(CloseResult{true, close_safety_complete_});
    }
    Task<Result<std::string>>
    DeviceSession::invoke(OperationId op, std::string_view response, CallOptions options)
    {
        return Task<Result<std::string>>(
            [self = shared_from_this(),
             op,
             response = std::string(response),
             options](auto done) mutable
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
                auto send_with = [self, op, response = std::move(response), options, linked,
                                  done = std::move(done)](
                                     Result<std::shared_ptr<CredentialLease>> acquired
                                  ) mutable
                {
                    if (!acquired) { auto error = acquired.error(); error.operation = op; done(std::move(error)); return; }
                    auto lease = std::move(acquired.value());
                    std::string envelope;
                    try { envelope = soap_envelope(op, "", lease.get()); }
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
                    q.idempotent = true;
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
                        if (r.value().status == 401)
                        {
                            Error error{ErrorCategory::authentication, op, "authentication failed"};
                            error.http = HttpErrorDetail{r.value().status};
                            done(std::move(error));
                            return;
                        }
                        if (r.value().status == 403)
                        {
                            Error error{ErrorCategory::authorization, op, "request was forbidden"};
                            error.http = HttpErrorDetail{r.value().status};
                            done(std::move(error));
                            return;
                        }
                        if (r.value().status == 429)
                        {
                            Error error{ErrorCategory::rate_limited, op, "HTTP rate limit exceeded", true};
                            error.http = HttpErrorDetail{r.value().status};
                            done(std::move(error));
                            return;
                        }
                        if (r.value().status < 200 || r.value().status >= 300)
                        {
                            auto fault = read_soap_operation(r.value().body, response);
                            if (!fault && (fault.error().category == ErrorCategory::soap_fault ||
                                           fault.error().category == ErrorCategory::authentication))
                            {
                                auto error = fault.error();
                                error.operation = op;
                                done(std::move(error));
                                return;
                            }
                            Error error{
                                    ErrorCategory::protocol_violation,
                                    op,
                                    "unexpected HTTP status " +
                                        std::to_string(r.value().status)
                            };
                            error.http = HttpErrorDetail{r.value().status};
                            done(std::move(error));
                            return;
                        }
                        auto parsed = read_soap_operation(r.value().body, response);
                        if (!parsed)
                        {
                            auto e = parsed.error();
                            e.operation = op;
                            done(std::move(e));
                            return;
                        }
                        // Hand the decoders only the response element's own content.
                        // Searching the whole envelope would let a SOAP Header, or
                        // anything following the response, shadow the real fields.
                        const auto element =
                            detail_xml::find_element(r.value().body, response);
                        if (!element)
                        {
                            done(Error{
                                ErrorCategory::protocol_violation,
                                op,
                                "Expected SOAP response element missing"
                            });
                            return;
                        }
                        done(std::string(detail_xml::inner(r.value().body, *element)));
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
            [](auto& x) -> Result<SystemDateAndTime>
            {
                const auto invalid = [](std::string message)
                {
                    return Error{
                        ErrorCategory::protocol_violation,
                        OperationId::get_system_date_and_time,
                        std::move(message)
                    };
                };
                // The response also carries LocalDateTime with identically named
                // children, so the UTC subtree has to be selected explicitly rather
                // than taking whichever Year element happens to appear first.
                const auto utc = detail_xml::find_element(x, "UTCDateTime");
                if (!utc)
                {
                    return invalid("device did not report a UTC date and time");
                }
                const auto subtree = std::string(detail_xml::inner(x, *utc));
                const auto integer = [&](std::string_view name)
                {
                    const auto value = field(subtree, name);
                    return value.empty() ? -1 : std::atoi(value.c_str());
                };
                const auto year = integer("Year");
                const auto month = integer("Month");
                const auto day = integer("Day");
                const auto hour = integer("Hour");
                const auto minute = integer("Minute");
                const auto second = integer("Second");
                const auto date = std::chrono::year(year) /
                                  std::chrono::month(static_cast<unsigned>(month)) /
                                  std::chrono::day(static_cast<unsigned>(day));
                if (!date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
                    second < 0 || second > 60)
                {
                    return invalid("device reported an invalid UTC date and time");
                }
                return SystemDateAndTime{
                    std::chrono::sys_days(date) + std::chrono::hours(hour) +
                    std::chrono::minutes(minute) + std::chrono::seconds(second)
                };
            }
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
                session_cancellation_.token()
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
                    session_cancellation_.token()
                );
        }
        if (!ptz_safety_)
            ptz_safety_ = std::make_shared<PtzSafetyState>(
                backend_, session_cancellation_.token()
            );
        return ptz_safety_;
    }
    Result<std::shared_ptr<ManagedEventStream>> DeviceSession::events(std::size_t maximum)
    {
        if (is_closed()) return Error{ErrorCategory::closed_handle, {}, "session is closed"};
        return std::make_shared<ManagedEventStream>(runtime_, backend(), maximum);
    }
} // namespace wlonvif
