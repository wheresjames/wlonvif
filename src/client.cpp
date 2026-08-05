#include <algorithm>
#include <cctype>
#include <ctime>
#include "network_services.hpp"
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
            return std::string(u.substr(0, e));
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
                if (origin(u) != o)
                {
                    return Error{
                        ErrorCategory::endpoint_rejected, {}, "cross-origin endpoint rejected"
                    };
                }
                if (u.find('@') != u.npos)
                {
                    return Error{ErrorCategory::endpoint_rejected, {}, "URL user-info rejected"};
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
        std::string field(std::string_view x, std::string_view n, std::size_t from = 0)
        {
            auto a = x.find("<tds:" + std::string(n) + ">", from);
            std::size_t skip = 6 + n.size();
            if (a == x.npos)
            {
                a = x.find("<tt:" + std::string(n) + ">", from);
                skip = 5 + n.size();
            }
            if (a == x.npos)
            {
                return {};
            }
            a += skip;
            auto b = x.find("</", a);
            return std::string(x.substr(a, b - a));
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
                r->post(
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
                                std::move(u),
                                std::move(t),
                                std::move(p),
                                std::move(r),
                                std::move(c)
                            ))
                        );
                    }
                );
            }
        );
    }
    Task<Result<std::string>>
    DeviceSession::invoke(OperationId op, std::string_view response, CallOptions options)
    {
        HttpRequest q{
            url_,
            soap_envelope(op, "", credentials_),
            {"Content-Type: application/soap+xml; charset=utf-8"},
            options.deadline,
            credentials_
        };
        auto source = transport_->send(std::move(q), options.cancellation);
        return Task<Result<std::string>>(
            [source = std::move(source), response = std::string(response), op](auto done) mutable
            {
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
                            if (!fault && fault.error().category == ErrorCategory::soap_fault)
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
                        done(std::move(r.value().body));
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
            [](auto& x)
            {
                const auto integer = [&](std::string_view name)
                {
                    const auto value = field(x, name);
                    return value.empty() ? 0 : std::atoi(value.c_str());
                };
                const auto date = std::chrono::year(integer("Year")) /
                                  std::chrono::month(static_cast<unsigned>(integer("Month"))) /
                                  std::chrono::day(static_cast<unsigned>(integer("Day")));
                if (!date.ok())
                {
                    return SystemDateAndTime{};
                }
                return SystemDateAndTime{
                    std::chrono::sys_days(date) + std::chrono::hours(integer("Hour")) +
                    std::chrono::minutes(integer("Minute")) +
                    std::chrono::seconds(integer("Second"))
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
                std::size_t p = 0;
                while ((p = x.find("<tt:ScopeItem>", p)) != x.npos)
                {
                    p += 14;
                    auto e = x.find("</tt:ScopeItem>", p);
                    v.emplace_back(x.substr(p, e - p));
                    p = e;
                }
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
                std::size_t p = 0;
                while ((p = x.find("<tds:Service>", p)) != x.npos)
                {
                    auto e = x.find("</tds:Service>", p);
                    auto part = std::string_view(x).substr(p, e - p);
                    v.push_back({field(part, "Namespace"), field(part, "XAddr")});
                    p = e;
                }
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
                auto m = field(x, "XAddr", x.find("<tt:Media"));
                if (!m.empty())
                {
                    c.media_xaddr = m;
                }
                auto p = field(x, "XAddr", x.find("<tt:PTZ"));
                if (!p.empty())
                {
                    c.ptz_xaddr = p;
                }
                auto e = field(x, "XAddr", x.find("<tt:Events"));
                if (!e.empty())
                {
                    c.events_xaddr = e;
                }
                return c;
            }
        );
    }
    Result<std::shared_ptr<MediaClient>> DeviceSession::media()
    {
        auto b = std::dynamic_pointer_cast<ServiceBackend>(transport_);
        if (!b)
        {
            b = make_network_service_backend(
                url_, transport_, endpoint_policy_, runtime_, credentials_
            );
        }
        return std::make_shared<MediaClient>(runtime_, std::move(b));
    }
    Result<std::shared_ptr<PtzClient>> DeviceSession::ptz()
    {
        auto b = std::dynamic_pointer_cast<ServiceBackend>(transport_);
        if (!b)
        {
            b = make_network_service_backend(
                url_, transport_, endpoint_policy_, runtime_, credentials_
            );
        }
        return std::make_shared<PtzClient>(runtime_, std::move(b));
    }
    Result<std::shared_ptr<ManagedEventStream>> DeviceSession::events(std::size_t maximum)
    {
        auto b = std::dynamic_pointer_cast<ServiceBackend>(transport_);
        if (!b)
        {
            b = make_network_service_backend(
                url_, transport_, endpoint_policy_, runtime_, credentials_
            );
        }
        return std::make_shared<ManagedEventStream>(runtime_, std::move(b), maximum);
    }
} // namespace wlonvif
