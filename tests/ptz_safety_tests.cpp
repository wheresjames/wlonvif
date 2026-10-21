#include <future>
#include <iostream>
#include <wlonvif/wlonvif.hpp>

using namespace wlonvif;

template<class T> T wait(Task<T> task)
{
    std::promise<T> promise;
    auto future = promise.get_future();
    std::move(task).start([&](T value) { promise.set_value(std::move(value)); });
    return future.get();
}

#define CHECK(value) do { if (!(value)) { std::cerr << "check failed: " #value << " at " << __LINE__ << '\n'; return 1; } } while (0)

namespace
{
    struct Fixture
    {
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<VirtualDeviceTransport> device;
        std::shared_ptr<DeviceSession> session;
        std::shared_ptr<PtzClient> ptz;
        CallOptions call;
    };

    Fixture fixture()
    {
        auto runtime = Runtime::create().value();
        Capabilities capabilities;
        capabilities.ptz_xaddr = "http://camera/ptz";
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime, DeviceInformation{}, std::vector<std::string>{},
            std::vector<Service>{}, capabilities
        );
        PtzSpaces spaces;
        spaces.pan = spaces.tilt = spaces.zoom = {-1, 1};
        for (const auto* token : {"main", "aux"})
        {
            MediaProfile profile{token, token, {}};
            device->add_profile(profile, {}, {}, spaces);
            device->add_preset(token, Preset{"home", "Home", {0.0, 0.0, 0.0}});
        }
        CallOptions call{
            std::chrono::steady_clock::now() + std::chrono::seconds(5),
            CancellationSource{}.token()
        };
        auto connected = wait(DeviceSession::connect(
            "http://camera/device", device, make_same_origin_policy(), runtime, std::nullopt, call
        ));
        auto session = connected.value();
        return {runtime, device, session, session->ptz().value(), call};
    }
}

int main()
{
    {
        auto f = fixture();
        CHECK(wait(f.ptz->continuous_move("main", {0.2, {}, {}}, f.call)));
        CHECK(wait(f.ptz->relative_move("aux", {0.1, {}, {}}, f.call)));
        CHECK(wait(f.ptz->absolute_move("main", {0.3, {}, {}}, {}, f.call)));
        CHECK(wait(f.session->close(std::chrono::milliseconds(200))));
        CHECK(f.device->stop_attempts("main") == 1);
        CHECK(f.device->stop_attempts("aux") == 1);
        CHECK(!f.device->ptz_status("main").value().moving);
        CHECK(!f.device->ptz_status("aux").value().moving);
        CHECK(f.runtime->shutdown());
    }
    {
        auto f = fixture();
        CHECK(wait(f.ptz->continuous_move("main", {0.2, {}, {}}, f.call)));
        CHECK(wait(f.ptz->stop("main", f.call)));
        CHECK(f.device->stop_attempts("main") == 1);
        CHECK(wait(f.session->close(std::chrono::milliseconds(200))));
        CHECK(f.device->stop_attempts("main") == 1);
        CHECK(f.runtime->shutdown());
    }
    {
        auto f = fixture();
        CHECK(wait(f.ptz->relative_move("main", {0.2, {}, {}}, f.call)));
        CHECK(f.device->stop("main"));
        CHECK(wait(f.ptz->status("main", f.call)));
        CHECK(wait(f.session->close(std::chrono::milliseconds(200))));
        CHECK(f.device->stop_attempts("main") == 1);
        CHECK(f.runtime->shutdown());
    }
    {
        auto f = fixture();
        Error uncertain{ErrorCategory::network, OperationId::relative_move,
                        "delivery uncertain", true, true};
        f.device->fail_next_move(std::move(uncertain));
        Error stop_failure{ErrorCategory::soap_fault, OperationId::ptz_stop, "Stop rejected"};
        stop_failure.soap_fault = SoapFaultDetail{"s:Receiver", "ter:ActionFailed", "failed", {}};
        f.device->set_stop_failure(stop_failure);
        auto movement = wait(f.ptz->relative_move("main", {0.1, {}, {}}, f.call));
        CHECK(!movement);
        CHECK(movement.error().category == ErrorCategory::network);
        CHECK(movement.error().request_may_have_been_applied);
        CHECK(movement.error().safety_action);
        CHECK(movement.error().safety_action->attempted);
        CHECK(!movement.error().safety_action->succeeded);
        CHECK(movement.error().safety_action->failure_category == ErrorCategory::soap_fault);
        CHECK(movement.error().safety_action->failure_code == "ter:ActionFailed");
        f.device->set_stop_failure(std::nullopt);
        auto closed = wait(f.session->close(std::chrono::milliseconds(200)));
        CHECK(closed && closed.value().safety_complete);
        CHECK(f.device->stop_attempts("main") == 2);
        CHECK(f.runtime->shutdown());
    }
    {
        auto f = fixture();
        const auto before = f.device->stop_attempts("main");
        auto rejected = wait(f.ptz->continuous_move("main", {2.0, {}, {}}, f.call));
        CHECK(!rejected && rejected.error().category == ErrorCategory::invalid_argument);
        CHECK(f.device->stop_attempts("main") == before);
        CancellationSource cancelled;
        cancelled.request_stop();
        auto aborted = wait(f.ptz->relative_move(
            "main", {0.1, {}, {}}, {f.call.deadline, cancelled.token()}
        ));
        CHECK(!aborted && aborted.error().category == ErrorCategory::cancelled);
        CHECK(aborted.error().safety_action && aborted.error().safety_action->succeeded);
        CHECK(f.runtime->shutdown());
    }
    {
        auto f = fixture();
        CHECK(wait(f.ptz->continuous_move("main", {0.2, {}, {}}, f.call)));
        CHECK(wait(f.ptz->continuous_move("main", {0.3, {}, {}}, f.call)));
        f.device->set_stop_delay(std::chrono::milliseconds(40));
        auto bounded = wait(f.session->close(std::chrono::milliseconds::zero()));
        CHECK(bounded && !bounded.value().cleanup_complete && !bounded.value().safety_complete);
        auto terminal = wait(f.session->close(std::chrono::milliseconds(200)));
        CHECK(terminal && terminal.value().cleanup_complete && terminal.value().safety_complete);
        CHECK(f.device->stop_attempts("main") == 1);
        CHECK(f.runtime->shutdown());
    }
    {
        auto f = fixture();
        CHECK(wait(f.ptz->goto_preset("main", "home", f.call)));
        CHECK(f.runtime->shutdown());
        CHECK(f.device->stop_attempts("main") == 1);
        CHECK(f.session->is_closed());
    }
    {
        auto busy = read_soap_operation(
            "<s:Envelope xmlns:s='http://www.w3.org/2003/05/soap-envelope'><s:Body>"
            "<s:Fault><s:Code><s:Value>s:Receiver</s:Value><s:Subcode>"
            "<s:Value>ter:DeviceBusy</s:Value></s:Subcode></s:Code></s:Fault>"
            "</s:Body></s:Envelope>", "Response");
        CHECK(!busy && busy.error().category == ErrorCategory::device_busy);
        CHECK(busy.error().retryable);
    }
    std::cout << "PTZ safety tests passed\n";
}
