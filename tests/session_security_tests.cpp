#include <future>
#include <iostream>
#include <thread>
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
    class DenyingAuthorizer final : public CandidateAuthorizer
    {
      public:
        Task<Result<void>> authorize(CandidateAuthorizationRequest value, CallOptions options) override
        {
            ++calls;
            request = std::move(value);
            saw_cancel_aware_options = options.deadline != std::chrono::steady_clock::time_point{};
            return Task<Result<void>>([](auto done) {
                done(Error{ErrorCategory::authorization, {}, "host policy denied candidate"});
            });
        }
        int calls{};
        CandidateAuthorizationRequest request;
        bool saw_cancel_aware_options{};
    };

    class EmptyTransport final : public HttpTransport
    {
      public:
        Task<Result<HttpResponse>> send(HttpRequest, CancellationToken) override
        {
            return Task<Result<HttpResponse>>([](auto done) { done(HttpResponse{200, {}, {}}); });
        }
    };

    class DelayedTransport final : public HttpTransport
    {
      public:
        Task<Result<HttpResponse>> send(HttpRequest, CancellationToken token) override
        {
            return Task<Result<HttpResponse>>([this, token](auto done) mutable {
                started.store(true);
                std::thread([token, done = std::move(done)]() mutable {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    done(token.stop_requested()
                        ? Result<HttpResponse>(Error{ErrorCategory::cancelled})
                        : Result<HttpResponse>(HttpResponse{200, {}, {}}));
                }).detach();
            });
        }
        std::atomic_bool started{};
    };
}

int main()
{
    auto created = Runtime::create();
    CHECK(created);
    auto runtime = created.value();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto authorizer = std::make_shared<DenyingAuthorizer>();
    auto transport = make_curl_transport(CurlTransportOptions{runtime, {}, authorizer});
    HttpRequest request{"http://localhost:9/device", {}, {}, deadline};
    request.original_url = "http://localhost:9/original";
    auto denied = wait(transport->send(std::move(request), CancellationSource{}.token()));
    CHECK(!denied);
    CHECK(denied.error().category == ErrorCategory::host_permission_denied);
    CHECK(denied.error().host_permission);
    CHECK(authorizer->calls == 1);
    CHECK(authorizer->request.original_url == "http://localhost:9/original");
    CHECK(authorizer->request.hostname == "localhost");
    CHECK(authorizer->request.port == 9);
    CHECK(!authorizer->request.resolved_addresses.empty());
    CHECK(authorizer->request.proposed_address == authorizer->request.resolved_addresses.front());
    CHECK(authorizer->saw_cancel_aware_options);

    Credentials source{"user", "secret"};
    auto provider = make_fixed_credential_provider(std::move(source));
    DeviceSessionOptions options{
        "http://camera/device", std::make_shared<EmptyTransport>(),
        make_same_origin_policy(), runtime, provider,
        {deadline, CancellationSource{}.token()}
    };
    auto connected = wait(DeviceSession::connect(std::move(options)));
    CHECK(connected);
    auto session = connected.value();
    auto first_close = wait(session->close(std::chrono::milliseconds(10)));
    auto second_close = wait(session->close(std::chrono::milliseconds(10)));
    CHECK(first_close && second_close && session->is_closed());
    auto after_close = wait(session->get_device_information({deadline, CancellationSource{}.token()}));
    CHECK(!after_close && after_close.error().category == ErrorCategory::closed_handle);
    auto acquisition = wait(provider->acquire({deadline, CancellationSource{}.token()}));
    CHECK(!acquisition && acquisition.error().category == ErrorCategory::closed_handle);

    auto delayed = std::make_shared<DelayedTransport>();
    DeviceSessionOptions delayed_options{
        "http://camera/device", delayed, make_same_origin_policy(), runtime, {},
        {deadline, CancellationSource{}.token()}
    };
    auto delayed_session_result = wait(DeviceSession::connect(std::move(delayed_options)));
    CHECK(delayed_session_result);
    auto delayed_session = delayed_session_result.value();
    std::promise<Result<DeviceInformation>> operation_promise;
    auto operation_future = operation_promise.get_future();
    std::move(delayed_session->get_device_information({deadline, CancellationSource{}.token()}))
        .start([&](auto result) { operation_promise.set_value(std::move(result)); });
    while (!delayed->started.load()) std::this_thread::yield();
    auto bounded_close = wait(delayed_session->close(std::chrono::milliseconds::zero()));
    CHECK(bounded_close && !bounded_close.value().cleanup_complete);
    auto cancelled_operation = operation_future.get();
    CHECK(!cancelled_operation && cancelled_operation.error().category == ErrorCategory::cancelled);
    auto terminal_close = wait(delayed_session->close(std::chrono::milliseconds(50)));
    CHECK(terminal_close && terminal_close.value().cleanup_complete);

    auto fault = read_soap_operation(
        "<s:Envelope xmlns:s='http://www.w3.org/2003/05/soap-envelope'><s:Body>"
        "<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:Busy</s:Value>"
        "</s:Subcode></s:Code><s:Reason><s:Text>busy</s:Text></s:Reason></s:Fault>"
        "</s:Body></s:Envelope>", "Response");
    CHECK(!fault && fault.error().soap_fault);
    CHECK(fault.error().soap_fault->code == "s:Sender");
    CHECK(fault.error().soap_fault->subcode == "ter:Busy");
    CHECK(fault.error().soap_fault->reason == "busy");
    CHECK(runtime->shutdown());
    std::cout << "session security tests passed\n";
}
