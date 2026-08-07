#include <future>
#include <iostream>
#include <wlonvif/wlonvif.hpp>

using namespace wlonvif;
namespace
{
    class FixedTransport final : public HttpTransport
    {
      public:
        explicit FixedTransport(HttpResponse response) : response_(std::move(response)) {}
        Task<Result<HttpResponse>> send(HttpRequest request, CancellationToken) override
        {
            credentials = std::move(request.credentials);
            credential_lease = std::move(request.credential_lease);
            return Task<Result<HttpResponse>>(
                [response = response_](auto completion) mutable
                {
                    completion(std::move(response));
                }
            );
        }
        std::optional<Credentials> credentials;
        std::shared_ptr<CredentialLease> credential_lease;

      private:
        HttpResponse response_;
    };
}
template <class T> T wait(Task<T> task)
{
    std::promise<T> p;
    auto f = p.get_future();
    std::move(task).start(
        [&](T v)
        {
            p.set_value(std::move(v));
        }
    );
    return f.get();
}
#define CHECK(x)                                                                                   \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
        {                                                                                          \
            std::cerr << "check failed: " #x " at " << __LINE__ << '\n';                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main()
{
    auto runtime_result = Runtime::create();
    CHECK(runtime_result);
    auto runtime = runtime_result.value();
    DeviceInformation expected{"Acme", "VirtualCam", "1.2.3", "serial", "hardware"};
    std::vector<std::string> scopes{"onvif://www.onvif.org/name/test"};
    std::vector<Service> services{{"http://www.onvif.org/ver10/media/wsdl", "http://camera/media"}};
    Capabilities caps;
    caps.media_xaddr = "http://camera/media";
    caps.ptz_xaddr = "http://camera/ptz";
    auto transport =
        std::make_shared<VirtualDeviceTransport>(runtime, expected, scopes, services, caps);
    CallOptions options{
        std::chrono::steady_clock::now() + std::chrono::seconds(5), CancellationSource{}.token()
    };
    auto connected = wait(
        DeviceSession::connect(
            "http://camera/device",
            transport,
            make_same_origin_policy(),
            runtime,
            std::nullopt,
            options
        )
    );
    CHECK(connected);
    auto session = connected.value();
    auto info = wait(session->get_device_information(options));
    CHECK(info);
    CHECK(info.value().manufacturer == "Acme");
    CHECK(info.value().hardware_id == "hardware");
    auto time = wait(session->get_system_date_and_time(options));
    CHECK(time);
    auto got_scopes = wait(session->get_scopes(options));
    CHECK(got_scopes);
    CHECK(got_scopes.value() == scopes);
    auto got_services = wait(session->get_services(options));
    CHECK(got_services);
    CHECK(got_services.value().size() == 1);
    CHECK(got_services.value()[0].xaddr == "http://camera/media");
    auto got_caps = wait(session->get_capabilities(options));
    CHECK(got_caps);
    CHECK(got_caps.value().ptz_xaddr == caps.ptz_xaddr);
    CancellationSource stopped;
    stopped.request_stop();
    options.cancellation = stopped.token();
    auto cancelled = wait(session->get_scopes(options));
    CHECK(!cancelled);
    CHECK(cancelled.error().category == ErrorCategory::cancelled);
    auto rejected = wait(
        DeviceSession::connect(
            "http://user@camera/device",
            transport,
            make_same_origin_policy(),
            runtime,
            std::nullopt,
            CallOptions{
                std::chrono::steady_clock::now() + std::chrono::seconds(1),
                CancellationSource{}.token()
            }
        )
    );
    CHECK(!rejected);
    CHECK(rejected.error().category == ErrorCategory::endpoint_rejected);
    auto same_host = make_same_host_policy();
    CHECK(same_host->authorize(
        "http://camera:8080/onvif/media", "http://camera/onvif/device_service"
    ));
    CHECK(!same_host->authorize(
        "http://other-camera/onvif/media", "http://camera/onvif/device_service"
    ));
    CHECK(!same_host->authorize(
        "http://user@camera/onvif/media", "http://camera/onvif/device_service"
    ));
    CHECK(!same_host->authorize(
        "ftp://camera/onvif/media", "http://camera/onvif/device_service"
    ));
    auto oversized = read_soap_operation("<x/>", "x", XmlLimits{2});
    CHECK(!oversized);
    auto fault = read_soap_operation(
        "<s:Envelope xmlns:s='http://www.w3.org/2003/05/soap-envelope'><s:Body>"
        "<s:Fault><s:Reason><s:Text>Rejected</s:Text></s:Reason></s:Fault>"
        "</s:Body></s:Envelope>",
        "GetDeviceInformationResponse"
    );
    CHECK(!fault);
    CHECK(fault.error().category == ErrorCategory::soap_fault);
    const auto authentication_fault = read_soap_operation(
        "<s:Envelope xmlns:s='http://www.w3.org/2003/05/soap-envelope'>"
        "<s:Body><s:Fault><s:Code><s:Subcode><s:Value>ter:NotAuthorized</s:Value>"
        "</s:Subcode></s:Code></s:Fault></s:Body></s:Envelope>",
        "GetDeviceInformationResponse"
    );
    CHECK(!authentication_fault);
    CHECK(authentication_fault.error().category == ErrorCategory::authentication);

    auto unauthorized_transport = std::make_shared<FixedTransport>(HttpResponse{401, {}, {}});
    auto unauthorized_session = wait(DeviceSession::connect(
        "http://camera/device",
        unauthorized_transport,
        make_same_origin_policy(),
        runtime,
        Credentials{"example-user", "example-password"},
        CallOptions{
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            CancellationSource{}.token()
        }
    ));
    CHECK(unauthorized_session);
    auto unauthorized = wait(unauthorized_session.value()->get_device_information(CallOptions{
        std::chrono::steady_clock::now() + std::chrono::seconds(1),
        CancellationSource{}.token()
    }));
    CHECK(!unauthorized);
    CHECK(unauthorized.error().category == ErrorCategory::authentication);
    CHECK(!unauthorized_transport->credentials);
    CHECK(unauthorized_transport->credential_lease);
    CHECK(unauthorized_transport->credential_lease->username() == "example-user");
    CHECK(unauthorized.error().http && unauthorized.error().http->status == 401);

    auto forbidden_transport = std::make_shared<FixedTransport>(HttpResponse{403, {}, {}});
    auto forbidden_session = wait(DeviceSession::connect(
        "http://camera/device",
        forbidden_transport,
        make_same_origin_policy(),
        runtime,
        std::nullopt,
        CallOptions{
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            CancellationSource{}.token()
        }
    ));
    CHECK(forbidden_session);
    auto forbidden = wait(forbidden_session.value()->get_device_information(CallOptions{
        std::chrono::steady_clock::now() + std::chrono::seconds(1),
        CancellationSource{}.token()
    }));
    CHECK(!forbidden);
    CHECK(forbidden.error().category == ErrorCategory::authorization);
    CHECK(!username_token_digest("nonce", "created", "password").empty());
    CHECK(runtime->shutdown());
    std::cout << "device service tests passed\n";
}
