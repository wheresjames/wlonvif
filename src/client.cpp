#include <algorithm>
#include <cctype>
#include <ctime>
#include "dispatch.hpp"
#include "network_services.hpp"
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
                            completion(Error{ErrorCategory::authentication, {}, "authentication failed"});
                            return;
                        }
                        if (response.value().status == 403)
                        {
                            completion(Error{ErrorCategory::authorization, {}, "request was forbidden"});
                            return;
                        }
                        if (response.value().status < 200 || response.value().status >= 300)
                        {
                            completion(Error{
                                ErrorCategory::protocol_violation,
                                {},
                                "unexpected HTTP status " +
                                    std::to_string(response.value().status)
                            });
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
        std::optional<Credentials> c
    )
        : url_(std::move(u)), transport_(std::move(t)), endpoint_policy_(std::move(p)),
          runtime_(std::move(r)), credentials_(std::move(c))
    {
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
                    if (options.cancellation.stop_requested())
                    {
                        done(Error{ErrorCategory::cancelled});
                        return;
                    }
                    auto a = p->authorize(u, origin(u));
                    if (!a)
                    {
                        done(a.error());
                        return;
                    }
                    done(
                        std::shared_ptr<DeviceSession>(new DeviceSession(
                            std::move(u), std::move(t), std::move(p), std::move(r), std::move(c)
                        ))
                    );
                };
                auto runtime = r;
                detail::dispatch(runtime, std::move(work));
            }
        );
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
                std::string envelope;
                try
                {
                    // Envelope construction can fail when the CSPRNG backing the
                    // WS-Security nonce is unavailable, so it belongs inside the
                    // operation where the failure has an Error to travel in.
                    envelope = soap_envelope(op, "", self->credentials_);
                }
                catch (const std::exception& error)
                {
                    done(Error{ErrorCategory::internal, op, error.what()});
                    return;
                }
                HttpRequest q{
                    self->url_,
                    std::move(envelope),
                    {"Content-Type: application/soap+xml; charset=utf-8"},
                    options.deadline,
                    self->credentials_
                };
                auto source = self->transport_->send(std::move(q), options.cancellation);
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
                            done(Error{ErrorCategory::authentication, op, "authentication failed"});
                            return;
                        }
                        if (r.value().status == 403)
                        {
                            done(Error{ErrorCategory::authorization, op, "request was forbidden"});
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
                            done(
                                Error{
                                    ErrorCategory::protocol_violation,
                                    op,
                                    "unexpected HTTP status " +
                                        std::to_string(r.value().status)
                                }
                            );
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
                url_, transport_, endpoint_policy_, runtime_, credentials_
            );
        }
        return backend_;
    }
    Result<std::shared_ptr<MediaClient>> DeviceSession::media()
    {
        return std::make_shared<MediaClient>(runtime_, backend());
    }
    Result<std::shared_ptr<PtzClient>> DeviceSession::ptz()
    {
        return std::make_shared<PtzClient>(runtime_, backend());
    }
    Result<std::shared_ptr<ManagedEventStream>> DeviceSession::events(std::size_t maximum)
    {
        return std::make_shared<ManagedEventStream>(runtime_, backend(), maximum);
    }
} // namespace wlonvif
