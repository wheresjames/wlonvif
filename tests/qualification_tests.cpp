#include <atomic>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <wlonvif/wlonvif.hpp>

using namespace wlonvif;

namespace
{
    template <class T> T wait_for_task(Task<T> task)
    {
        std::promise<T> promise;
        auto future = promise.get_future();
        std::move(task).start(
            [&promise](T result)
            {
                promise.set_value(std::move(result));
            }
        );
        return future.get();
    }

    CallOptions options()
    {
        return {
            std::chrono::steady_clock::now() + std::chrono::seconds(10),
            CancellationSource{}.token()
        };
    }

    class DenyingDiscoveryAuthorizer final : public DiscoveryAuthorizer
    {
      public:
        Task<Result<void>> authorize(
            DiscoveryAuthorizationRequest request, CallOptions
        ) override
        {
            actions.push_back(request.action);
            return Task<Result<void>>([](auto done)
            {
                done(Error{ErrorCategory::authorization, OperationId::discovery_probe,
                           "denied by test policy"});
            });
        }
        std::vector<DiscoveryNetworkAction> actions;
    };
} // namespace

#define REQUIRE(expression)                                                                        \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "requirement failed: " #expression " at line " << __LINE__ << '\n';       \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main()
{
    const auto malformed = read_soap_operation(
        "<Envelope><GetDeviceInformationResponse></Envelope>", "GetDeviceInformationResponse"
    );
    REQUIRE(!malformed);
    REQUIRE(malformed.error().category == ErrorCategory::malformed_xml);

    const auto dtd = read_soap_operation(
        "<!DOCTYPE x [<!ENTITY e 'boom'>]><GetDeviceInformationResponse>&e;"
        "</GetDeviceInformationResponse>",
        "GetDeviceInformationResponse"
    );
    REQUIRE(!dtd);

    std::string deep;
    for (int i = 0; i < 80; ++i)
    {
        deep += "<x>";
    }
    deep += "<GetDeviceInformationResponse/>";
    for (int i = 0; i < 80; ++i)
    {
        deep += "</x>";
    }
    REQUIRE(!read_soap_operation(deep, "GetDeviceInformationResponse"));

    auto runtime_result = Runtime::create();
    REQUIRE(runtime_result);
    auto runtime = runtime_result.value();

    // Discovery can be made fail-closed before any socket is created.
    {
        NetworkDiscoveryOptions discovery;
        discovery.multicast = false;
        discovery.targets = {"127.0.0.1"};
        discovery.require_authorization = true;
        auto unavailable = wait_for_task(discover_devices(
            runtime, make_same_host_policy(), discovery, options()
        ));
        REQUIRE(!unavailable);
        REQUIRE(unavailable.error().category == ErrorCategory::host_permission_denied);

        auto authorizer = std::make_shared<DenyingDiscoveryAuthorizer>();
        discovery.authorizer = authorizer;
        auto denied = wait_for_task(discover_devices(
            runtime, make_same_host_policy(), discovery, options()
        ));
        REQUIRE(!denied);
        REQUIRE(denied.error().category == ErrorCategory::host_permission_denied);
        REQUIRE(!authorizer->actions.empty());
        REQUIRE(authorizer->actions.front() == DiscoveryNetworkAction::bind_udp);
    }
    auto device = std::make_shared<VirtualDeviceTransport>(
        runtime,
        DeviceInformation{"Test", "Qualified", "0.1", "1", "1"},
        std::vector<std::string>{},
        std::vector<Service>{},
        Capabilities{}
    );
    device->add_profile(
        {"main", "Main"},
        {"rtsp://camera/main", std::chrono::seconds(60), false},
        {"http://camera/snapshot", std::chrono::seconds(60), false},
        {{-1, 1}, {-1, 1}, {-1, 1}}
    );

    auto connected = wait_for_task(
        DeviceSession::connect(
            "http://camera/device",
            device,
            make_same_origin_policy(),
            runtime,
            std::nullopt,
            options()
        )
    );
    REQUIRE(connected);
    auto media = connected.value()->media();
    REQUIRE(media);

    std::atomic_int failures{0};
    std::vector<std::thread> callers;
    for (int i = 0; i < 100; ++i)
    {
        callers.emplace_back(
            [&]
            {
                auto profiles = wait_for_task(media.value()->get_profiles(options()));
                if (!profiles || profiles.value().size() != 1)
                {
                    ++failures;
                }
            }
        );
    }
    for (auto& caller : callers)
    {
        caller.join();
    }
    REQUIRE(failures == 0);

    auto stream = connected.value()->events(32);
    REQUIRE(stream);
    for (std::uint64_t sequence = 0; sequence < 5000; ++sequence)
    {
        device->emit(
            {"tns1:Load", std::chrono::system_clock::now(), "qualification", "1", sequence}
        );
    }
    REQUIRE(wait_for_task(stream.value()->poll(5000, options())));
    std::size_t delivered = 0;
    for (;;)
    {
        auto item = wait_for_task(stream.value()->next(options()));
        if (!item)
        {
            REQUIRE(item.error().category == ErrorCategory::timeout);
            break;
        }
        ++delivered;
    }
    REQUIRE(delivered <= 32);

    DiscoverySession discovery(runtime, 2);
    REQUIRE(wait_for_task(discovery.inject(
        {DiscoveryKind::probe_match, "one", "192.0.2.1", "eth0", {}, {}, 1}, options()
    )));
    REQUIRE(wait_for_task(discovery.inject(
        {DiscoveryKind::probe_match, "two", "192.0.2.2", "eth0", {}, {}, 1}, options()
    )));
    auto overflow = wait_for_task(discovery.inject(
        {DiscoveryKind::probe_match, "three", "192.0.2.3", "eth0", {}, {}, 1}, options()
    ));
    REQUIRE(!overflow);
    REQUIRE(overflow.error().category == ErrorCategory::queue_overflow);

    NetworkDiscoveryOptions invalid_scan;
    invalid_scan.multicast = false;
    invalid_scan.retransmissions = 0;
    invalid_scan.scan_networks = {"192.0.2.0/24trailing"};
    auto malformed_network = wait_for_task(discover_devices(
        runtime, make_same_origin_policy(), std::move(invalid_scan), options()
    ));
    REQUIRE(!malformed_network);
    REQUIRE(malformed_network.error().category == ErrorCategory::invalid_argument);

    NetworkDiscoveryOptions excessive_scan;
    excessive_scan.multicast = false;
    excessive_scan.retransmissions = 0;
    excessive_scan.maximum_hosts = 4;
    excessive_scan.scan_networks = {"192.0.2.0/24"};
    auto excessive_network = wait_for_task(discover_devices(
        runtime, make_same_origin_policy(), std::move(excessive_scan), options()
    ));
    REQUIRE(!excessive_network);
    REQUIRE(excessive_network.error().category == ErrorCategory::invalid_argument);

    NetworkDiscoveryOptions invalid_concurrency;
    invalid_concurrency.multicast = false;
    invalid_concurrency.maximum_concurrency = 0;
    auto concurrency_error = wait_for_task(discover_devices(
        runtime, make_same_origin_policy(), std::move(invalid_concurrency), options()
    ));
    REQUIRE(!concurrency_error);
    REQUIRE(concurrency_error.error().category == ErrorCategory::invalid_argument);

    REQUIRE(wait_for_task(stream.value()->close(options())));
    REQUIRE(wait_for_task(discovery.close(options())));
    REQUIRE(runtime->shutdown());
    std::cout << "qualification tests passed\n";
}
