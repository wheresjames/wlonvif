/**
 * @file backend_regression_tests.cpp
 * @brief Regression coverage for defects found in the network backend, safety
 *        ledger, transports, and lifetime handling.
 *
 * Most of the existing suite drives PTZ and events through VirtualDeviceTransport,
 * which is itself a ServiceBackend, so the real NetworkServiceBackend path was
 * never exercised. These tests use ScriptedTransport instead: a plain
 * HttpTransport that, like the curl transport, runs each request on its own
 * thread and delivers its completion through the Runtime. That is what makes the
 * cancellation, lock-ordering, and Runtime-thread deadlock defects observable.
 */

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <wlonvif/version.hpp>
#include <wlonvif/wlonvif.hpp>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace wlonvif;
using namespace std::chrono_literals;

namespace
{
    int failures = 0;

#define CHECK(condition)                                                                   \
    do                                                                                     \
    {                                                                                      \
        if (!(condition))                                                                  \
        {                                                                                  \
            std::cerr << "check failed: " #condition " at line " << __LINE__ << '\n';      \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

    using Clock = std::chrono::steady_clock;

    CallOptions options(std::chrono::milliseconds budget = 5s)
    {
        return {Clock::now() + budget, CancellationSource{}.token()};
    }

    /// Start @p task and wait up to @p limit; nullopt when it did not complete.
    template <class T>
    std::optional<T> wait_for(Task<T> task, std::chrono::milliseconds limit = 10s)
    {
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        std::move(task).start([promise](T value) { promise->set_value(std::move(value)); });
        if (future.wait_for(limit) != std::future_status::ready) return std::nullopt;
        return future.get();
    }

    /// A hung wait means a deadlock; abandon the process instead of hanging ctest.
    template <class T> T must_finish(std::optional<T> value, const char* what)
    {
        if (!value)
        {
            std::cerr << "deadlock or hang: " << what << '\n';
            std::_Exit(1);
        }
        return std::move(*value);
    }

    std::string envelope(std::string_view name, std::string_view body, std::string_view header = {})
    {
        return "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:ns2=\"http://www.onvif.org/ver10/schema\"><SOAP-ENV:Header>" +
               std::string(header) + "</SOAP-ENV:Header><SOAP-ENV:Body><ns2:" +
               std::string(name) + "Response>" + std::string(body) + "</ns2:" +
               std::string(name) + "Response></SOAP-ENV:Body></SOAP-ENV:Envelope>";
    }

    struct Recorded
    {
        std::string body;
        bool cancelled_on_arrival{};
    };

    /**
     * @brief HttpTransport answering a scripted PTZ/media/event device.
     *
     * Each request runs on its own thread and its completion is posted to the
     * Runtime, matching the curl transport. A request whose token is already
     * cancelled is refused before reaching the "wire", as the real transport does.
     */
    class ScriptedTransport final : public HttpTransport,
                                    public std::enable_shared_from_this<ScriptedTransport>
    {
      public:
        using Handler = std::function<std::optional<Result<HttpResponse>>(
            const HttpRequest&, const CancellationToken&)>;

        explicit ScriptedTransport(std::shared_ptr<Runtime> runtime)
            : runtime_(std::move(runtime)) {}

        /// Install a handler consulted before the default device.
        void script(Handler handler)
        {
            std::lock_guard lock(mutex_);
            handler_ = std::move(handler);
        }

        Task<Result<HttpResponse>> send(HttpRequest request, CancellationToken token) override
        {
            // Request threads keep the transport alive: some (a detached
            // controller's final Stop, for example) outlive the test section.
            return Task<Result<HttpResponse>>(
                [self = shared_from_this(), request = std::move(request), token](auto done) mutable
                {
                    std::thread([self, request = std::move(request), token,
                                 done = std::move(done)]() mutable
                    {
                        auto result = self->handle(request, token);
                        auto runtime = self->runtime_;
                        auto deliver = [done = std::move(done), result = std::move(result)]() mutable
                        {
                            done(std::move(result));
                        };
                        if (!runtime->post(deliver)) deliver();
                    }).detach();
                }
            );
        }

        std::size_t delivered(std::string_view needle)
        {
            std::lock_guard lock(mutex_);
            std::size_t total = 0;
            for (const auto& entry : requests_)
                if (!entry.cancelled_on_arrival && entry.body.find(needle) != std::string::npos)
                    ++total;
            return total;
        }

        std::size_t refused(std::string_view needle)
        {
            std::lock_guard lock(mutex_);
            std::size_t total = 0;
            for (const auto& entry : requests_)
                if (entry.cancelled_on_arrival && entry.body.find(needle) != std::string::npos)
                    ++total;
            return total;
        }

        std::string last(std::string_view needle)
        {
            std::lock_guard lock(mutex_);
            for (auto entry = requests_.rbegin(); entry != requests_.rend(); ++entry)
                if (entry->body.find(needle) != std::string::npos) return entry->body;
            return {};
        }

        /// Default answers for a PTZ camera with distinct absolute and velocity spaces.
        static HttpResponse device(const std::string& request)
        {
            const auto has = [&](std::string_view needle)
            { return request.find(needle) != std::string::npos; };
            const auto ok = [](std::string_view name, std::string_view body = {})
            { return HttpResponse{200, envelope(name, body), "application/soap+xml"}; };
            if (has("GetCapabilities"))
                return ok("GetCapabilities",
                    "<ns2:Capabilities>"
                    "<ns2:Media><ns2:XAddr>http://camera/media</ns2:XAddr></ns2:Media>"
                    "<ns2:PTZ><ns2:XAddr>http://camera/ptz</ns2:XAddr></ns2:PTZ>"
                    "<ns2:Events><ns2:XAddr>http://camera/events</ns2:XAddr></ns2:Events>"
                    "</ns2:Capabilities>");
            if (has("GetProfiles"))
                return ok("GetProfiles",
                    "<ns2:Profiles token=\"main\"><ns2:Name>Main</ns2:Name>"
                    "<ns2:PTZConfiguration token=\"cfg\"/></ns2:Profiles>");
            if (has("GetConfigurationOptions"))
                return ok("GetConfigurationOptions",
                    "<ns2:Spaces>"
                    "<ns2:AbsolutePanTiltPositionSpace><ns2:URI>abs</ns2:URI>"
                    "<ns2:XRange><ns2:Min>-10</ns2:Min><ns2:Max>10</ns2:Max></ns2:XRange>"
                    "<ns2:YRange><ns2:Min>-10</ns2:Min><ns2:Max>10</ns2:Max></ns2:YRange>"
                    "</ns2:AbsolutePanTiltPositionSpace>"
                    "<ns2:AbsoluteZoomPositionSpace><ns2:URI>absz</ns2:URI>"
                    "<ns2:XRange><ns2:Min>0</ns2:Min><ns2:Max>20</ns2:Max></ns2:XRange>"
                    "</ns2:AbsoluteZoomPositionSpace>"
                    "<ns2:ContinuousPanTiltVelocitySpace><ns2:URI>vel</ns2:URI>"
                    "<ns2:XRange><ns2:Min>-1</ns2:Min><ns2:Max>1</ns2:Max></ns2:XRange>"
                    "<ns2:YRange><ns2:Min>-1</ns2:Min><ns2:Max>1</ns2:Max></ns2:YRange>"
                    "</ns2:ContinuousPanTiltVelocitySpace>"
                    "<ns2:ContinuousZoomVelocitySpace><ns2:URI>velz</ns2:URI>"
                    "<ns2:XRange><ns2:Min>-1</ns2:Min><ns2:Max>1</ns2:Max></ns2:XRange>"
                    "</ns2:ContinuousZoomVelocitySpace></ns2:Spaces>");
            if (has("GetPresets"))
                return ok("GetPresets",
                    "<ns2:Preset token=\"p1\"><ns2:Name>Door</ns2:Name><ns2:PTZPosition>"
                    "<ns2:PanTilt x=\"5\" y=\"-10\"/><ns2:Zoom x=\"10\"/>"
                    "</ns2:PTZPosition></ns2:Preset>");
            if (has("tptz:GetStatus"))
                return ok("GetStatus",
                    "<ns2:PTZStatus><ns2:Position><ns2:PanTilt x=\"0\" y=\"0\"/>"
                    "<ns2:Zoom x=\"0\"/></ns2:Position><ns2:MoveStatus>"
                    "<ns2:PanTilt>IDLE</ns2:PanTilt><ns2:Zoom>IDLE</ns2:Zoom>"
                    "</ns2:MoveStatus></ns2:PTZStatus>");
            if (has("ContinuousMove")) return ok("ContinuousMove");
            if (has("RelativeMove")) return ok("RelativeMove");
            if (has("tptz:Stop")) return ok("Stop");
            if (has("GetStreamUri"))
                return ok("GetStreamUri", "<ns2:MediaUri><ns2:Uri>rtsp://camera/real</ns2:Uri>"
                                          "</ns2:MediaUri>");
            if (has("CreatePullPointSubscription"))
                return ok("CreatePullPointSubscription",
                    "<ns2:SubscriptionReference><ns2:Address>http://camera/subscription"
                    "</ns2:Address></ns2:SubscriptionReference>");
            if (has("PullMessages")) return ok("PullMessages");
            if (has("wsnt:Renew")) return ok("Renew");
            if (has("wsnt:Unsubscribe")) return ok("Unsubscribe");
            return HttpResponse{500, "unknown request", "text/plain"};
        }

      private:
        Result<HttpResponse> handle(const HttpRequest& request, const CancellationToken& token)
        {
            const bool cancelled = token.stop_requested();
            Handler handler;
            {
                std::lock_guard lock(mutex_);
                requests_.push_back({request.body, cancelled});
                handler = handler_;
            }
            if (cancelled) return Error{ErrorCategory::cancelled, {}, "HTTP request cancelled"};
            if (handler)
                if (auto scripted = handler(request, token)) return std::move(*scripted);
            return device(request.body);
        }

        std::shared_ptr<Runtime> runtime_;
        std::mutex mutex_;
        Handler handler_;
        std::vector<Recorded> requests_;
    };

    /// Block like a long-poll until the request is cancelled or @p limit passes.
    Result<HttpResponse> hold_until_cancelled(
        const CancellationToken& token, std::chrono::milliseconds limit = 10s
    )
    {
        const auto end = Clock::now() + limit;
        while (!token.stop_requested() && Clock::now() < end) std::this_thread::sleep_for(5ms);
        if (token.stop_requested()) return Error{ErrorCategory::cancelled, {}, "cancelled"};
        return Error{ErrorCategory::timeout, {}, "timed out"};
    }

    struct Fixture
    {
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<ScriptedTransport> transport;
        std::shared_ptr<DeviceSession> session;
    };

    Fixture connect(std::optional<Credentials> credentials = std::nullopt)
    {
        Fixture fixture;
        fixture.runtime = Runtime::create().value();
        fixture.transport = std::make_shared<ScriptedTransport>(fixture.runtime);
        fixture.session = must_finish(wait_for(DeviceSession::connect(
            "http://camera/onvif/device_service", fixture.transport, make_same_host_policy(),
            fixture.runtime, std::move(credentials), options()
        )), "connect").value();
        return fixture;
    }
} // namespace

int main()
{
    // ---- PROJECT.txt is the single source of the version ----
    {
        CHECK(library_version() == std::string_view{WLONVIF_EXPECTED_VERSION});
        CHECK(std::string_view{WLONVIF_VERSION_STRING} == WLONVIF_EXPECTED_VERSION);
    }

    // ---- Linking an already linked token keeps every source ----
    {
        CancellationSource a, b, c;
        const auto inner = CancellationToken::linked(a.token(), b.token());
        const auto outer = CancellationToken::linked(inner, c.token());
        const auto reversed = CancellationToken::linked(c.token(), inner);
        CHECK(!outer.stop_requested() && !reversed.stop_requested());
        b.request_stop();
        CHECK(outer.stop_requested());
        CHECK(reversed.stop_requested());
    }

    // ---- Session close delivers the PTZ Stop, with credentials, over a plain transport ----
    {
        auto f = connect(Credentials{"operator", "secret"});
        auto ptz = f.session->ptz().value();
        auto moved = must_finish(wait_for(ptz->continuous_move("main", {0.5, {}, {}}, options())),
                                 "continuous move");
        CHECK(moved);
        auto closed = must_finish(wait_for(f.session->close(3s)), "session close");
        CHECK(closed && closed.value().cleanup_complete);
        CHECK(closed && closed.value().safety_complete);
        CHECK(f.transport->delivered("tptz:Stop") == 1);
        CHECK(f.transport->refused("tptz:Stop") == 0);
        CHECK(f.transport->last("tptz:Stop").find("wsse:UsernameToken") != std::string::npos);
        CHECK(f.runtime->shutdown());
    }

    // ---- A Stop is not queued behind an in-flight move, and a raced move is re-stopped ----
    {
        auto f = connect();
        auto ptz = f.session->ptz().value();
        CHECK(must_finish(wait_for(ptz->spaces("main", options())), "spaces"));
        f.transport->script([](const HttpRequest& request, const CancellationToken&)
            -> std::optional<Result<HttpResponse>>
        {
            if (request.body.find("ContinuousMove") == std::string::npos) return std::nullopt;
            std::this_thread::sleep_for(1200ms);
            return ScriptedTransport::device(request.body);
        });
        auto move_promise = std::make_shared<std::promise<Result<MoveResult>>>();
        auto move_future = move_promise->get_future();
        ptz->continuous_move("main", {0.5, {}, {}}, options()).start(
            [move_promise](Result<MoveResult> value) { move_promise->set_value(std::move(value)); });
        std::this_thread::sleep_for(150ms);
        const auto started = Clock::now();
        auto stopped = must_finish(wait_for(ptz->emergency_stop("main", options())), "stop");
        CHECK(stopped);
        CHECK(Clock::now() - started < 800ms);
        CHECK(move_future.wait_for(5s) == std::future_status::ready);
        auto moved = move_future.get();
        CHECK(moved);
        // The move reached the device after the Stop, so it must be stopped again.
        CHECK(moved && moved.value().safety_action && moved.value().safety_action->succeeded);
        CHECK(f.transport->delivered("tptz:Stop") == 2);
        CHECK(must_finish(wait_for(f.session->close(2s)), "close"));
        CHECK(f.runtime->shutdown());
    }

    // ---- Presets normalize against the absolute position space ----
    {
        auto f = connect();
        auto presets = must_finish(
            wait_for(f.session->ptz().value()->get_presets("main", options())), "presets");
        CHECK(presets && presets.value().size() == 1);
        if (presets && presets.value().size() == 1)
        {
            const auto& position = presets.value()[0].position;
            CHECK(position.pan && std::abs(*position.pan - 0.5) < 1e-12);
            CHECK(position.tilt && std::abs(*position.tilt + 1.0) < 1e-12);
            CHECK(position.zoom && std::abs(*position.zoom) < 1e-12);
        }
        CHECK(f.runtime->shutdown());
    }

    // ---- Backend responses: status checked, decoders scoped to the response element ----
    {
        auto f = connect();
        auto media = f.session->media().value();
        f.transport->script([](const HttpRequest& request, const CancellationToken&)
            -> std::optional<Result<HttpResponse>>
        {
            if (request.body.find("GetProfiles") != std::string::npos)
                return HttpResponse{404, ScriptedTransport::device(request.body).body, {}};
            if (request.body.find("GetStreamUri") != std::string::npos)
                return HttpResponse{200, envelope("GetStreamUri",
                    "<ns2:MediaUri><ns2:Uri>rtsp://camera/real</ns2:Uri></ns2:MediaUri>",
                    "<ns2:Uri>rtsp://decoy/header</ns2:Uri>"), {}};
            return std::nullopt;
        });
        auto profiles = must_finish(wait_for(media->get_profiles(options())), "profiles");
        CHECK(!profiles && profiles.error().category == ErrorCategory::protocol_violation);
        CHECK(!profiles && profiles.error().http && profiles.error().http->status == 404);
        auto uri = must_finish(wait_for(media->get_stream_uri("main", options())), "stream uri");
        CHECK(uri && uri.value().uri == "rtsp://camera/real");
        CHECK(f.runtime->shutdown());
    }

    // ---- SOAP fault fields stay within their own elements ----
    {
        const std::string soap12 =
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
            "xmlns:ter=\"http://www.onvif.org/ver10/error\"><s:Body><s:Fault>"
            "<s:Code><s:Value>s:Receiver</s:Value><s:Subcode><s:Value>ter:ActionNotSupported"
            "</s:Value></s:Subcode></s:Code>"
            "<s:Reason><s:Text xml:lang=\"en\">Not supported</s:Text></s:Reason>"
            "<s:Detail><s:Text>inner detail</s:Text></s:Detail>"
            "</s:Fault><trailer>junk</trailer></s:Body></s:Envelope>";
        auto fault = read_soap_operation(soap12, "GetProfilesResponse");
        CHECK(!fault && fault.error().category == ErrorCategory::soap_fault);
        CHECK(!fault && fault.error().soap_fault);
        if (!fault && fault.error().soap_fault)
        {
            const auto& detail = *fault.error().soap_fault;
            CHECK(detail.code == "s:Receiver");
            CHECK(detail.subcode == "ter:ActionNotSupported");
            CHECK(detail.reason == "Not supported");
            CHECK(detail.detail == "inner detail");
        }
        const std::string soap11 =
            "<soap:Envelope xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<soap:Body><soap:Fault><faultcode>soap:Client</faultcode>"
            "<faultstring>Bad request</faultstring><detail>why</detail>"
            "</soap:Fault></soap:Body></soap:Envelope>";
        auto legacy = read_soap_operation(soap11, "GetProfilesResponse");
        CHECK(!legacy && legacy.error().soap_fault);
        if (!legacy && legacy.error().soap_fault)
        {
            CHECK(legacy.error().soap_fault->code == "soap:Client");
            CHECK(legacy.error().soap_fault->reason == "Bad request");
            CHECK(legacy.error().soap_fault->detail == "why");
        }
    }

    // ---- Unsubscribe cancels an in-flight long-poll instead of waiting it out ----
    {
        auto f = connect();
        f.transport->script([](const HttpRequest& request, const CancellationToken& token)
            -> std::optional<Result<HttpResponse>>
        {
            if (request.body.find("PullMessages") == std::string::npos) return std::nullopt;
            return hold_until_cancelled(token);
        });
        auto events = f.session->event_client().value();
        auto subscription = must_finish(
            wait_for(events->create_pullpoint({std::chrono::seconds(60), {}}, options())),
            "create pullpoint");
        CHECK(subscription);
        if (subscription)
        {
            auto pull_promise = std::make_shared<std::promise<Result<std::vector<Event>>>>();
            auto pull_future = pull_promise->get_future();
            subscription.value()->pull(10s, 8, options(15s)).start(
                [pull_promise](auto value) { pull_promise->set_value(std::move(value)); });
            std::this_thread::sleep_for(150ms);
            const auto started = Clock::now();
            auto closed = must_finish(
                wait_for(subscription.value()->unsubscribe(options(2s)), 5s), "unsubscribe");
            CHECK(closed);
            CHECK(Clock::now() - started < 1s);
            CHECK(pull_future.wait_for(2s) == std::future_status::ready);
            if (pull_future.valid())
            {
                auto pulled = pull_future.get();
                CHECK(!pulled && pulled.error().category == ErrorCategory::cancelled);
            }
        }
        CHECK(f.runtime->shutdown());
    }

    // ---- A subscription the device faulted on is not leaked or retried forever ----
    {
        auto f = connect();
        f.transport->script([](const HttpRequest& request, const CancellationToken&)
            -> std::optional<Result<HttpResponse>>
        {
            if (request.body.find("wsnt:Unsubscribe") == std::string::npos) return std::nullopt;
            return HttpResponse{500,
                "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body>"
                "<s:Fault><s:Code><s:Value>s:Receiver</s:Value></s:Code>"
                "<s:Reason><s:Text>gone</s:Text></s:Reason></s:Fault></s:Body></s:Envelope>",
                {}};
        });
        auto events = f.session->event_client().value();
        auto subscription = must_finish(
            wait_for(events->create_pullpoint({std::chrono::seconds(60), {}}, options())), "create");
        CHECK(subscription);
        if (subscription)
        {
            auto first = must_finish(wait_for(subscription.value()->unsubscribe(options())), "u1");
            CHECK(!first && first.error().category == ErrorCategory::soap_fault);
            auto second = must_finish(wait_for(subscription.value()->unsubscribe(options())), "u2");
            CHECK(second);
            CHECK(f.transport->delivered("wsnt:Unsubscribe") == 1);
        }
        CHECK(f.runtime->shutdown());
    }

    // ---- A managed stream reconnect releases the abandoned subscription ----
    {
        auto f = connect();
        auto pulls = std::make_shared<std::atomic_int>(0);
        f.transport->script([pulls](const HttpRequest& request, const CancellationToken& token)
            -> std::optional<Result<HttpResponse>>
        {
            if (request.body.find("PullMessages") == std::string::npos) return std::nullopt;
            if (pulls->fetch_add(1) == 0)
                return Error{ErrorCategory::network, {}, "connection reset", true};
            std::this_thread::sleep_for(20ms);
            if (token.stop_requested()) return Error{ErrorCategory::cancelled};
            return ScriptedTransport::device(request.body);
        });
        ManagedEventOptions managed;
        managed.minimum_backoff = 10ms;
        managed.maximum_backoff = 20ms;
        managed.pull_timeout = 200ms;
        auto stream = must_finish(wait_for(f.session->event_client().value()->open_managed_stream(
            managed, options())), "open managed stream");
        CHECK(stream);
        if (stream)
        {
            auto item = must_finish(wait_for(stream.value()->next(options(3s))), "next");
            CHECK(item && std::holds_alternative<StreamHealth>(item.value()) &&
                  std::get<StreamHealth>(item.value()) == StreamHealth::possible_event_loss);
            CHECK(f.transport->delivered("CreatePullPointSubscription") == 2);
            CHECK(f.transport->delivered("wsnt:Unsubscribe") == 1);
            CHECK(must_finish(wait_for(stream.value()->close(options())), "close"));
        }
        CHECK(f.runtime->shutdown());
    }

    // ---- next() on a managed stream notices cancellation before its deadline ----
    {
        auto f = connect();
        f.transport->script([](const HttpRequest& request, const CancellationToken& token)
            -> std::optional<Result<HttpResponse>>
        {
            if (request.body.find("PullMessages") == std::string::npos) return std::nullopt;
            return hold_until_cancelled(token);
        });
        auto stream = must_finish(wait_for(f.session->event_client().value()->open_managed_stream(
            {}, options())), "open managed stream").value();
        CancellationSource cancel;
        std::thread canceller([&] { std::this_thread::sleep_for(100ms); cancel.request_stop(); });
        const auto started = Clock::now();
        auto item = must_finish(
            wait_for(stream->next({Clock::now() + 10s, cancel.token()})), "cancelled next");
        canceller.join();
        CHECK(!item);
        CHECK(Clock::now() - started < 1s);
        CHECK(must_finish(wait_for(stream->close(options())), "close"));
        CHECK(f.runtime->shutdown());
    }

    // ---- Closing or destroying from a completion does not deadlock the Runtime ----
    {
        auto f = connect();
        f.transport->script([](const HttpRequest& request, const CancellationToken& token)
            -> std::optional<Result<HttpResponse>>
        {
            if (request.body.find("PullMessages") != std::string::npos)
                return hold_until_cancelled(token);
            if (request.body.find("tptz:GetStatus") != std::string::npos)
                std::this_thread::sleep_for(300ms);
            return std::nullopt;
        });
        auto events = f.session->event_client().value();

        // close() started on the Runtime thread while the worker is mid-pull.
        auto stream = must_finish(wait_for(events->open_managed_stream({}, options())),
                                  "open managed stream").value();
        std::this_thread::sleep_for(150ms);
        auto closed = std::make_shared<std::promise<Result<void>>>();
        auto closed_future = closed->get_future();
        f.runtime->post([stream, closed]
        {
            stream->close(options()).start(
                [closed](Result<void> value) { closed->set_value(std::move(value)); });
        });
        must_finish(closed_future.wait_for(5s) == std::future_status::ready
                        ? std::optional<bool>(true) : std::nullopt,
                    "ManagedEventStream::close from the Runtime thread");
        CHECK(closed_future.get());

        // The last reference to a managed stream released on the Runtime thread.
        struct Holder { std::shared_ptr<ManagedEventStream> stream; };
        auto holder = std::make_shared<Holder>();
        holder->stream = must_finish(wait_for(events->open_managed_stream({}, options())),
                                     "open managed stream").value();
        std::this_thread::sleep_for(150ms);
        auto released = std::make_shared<std::promise<void>>();
        auto released_future = released->get_future();
        f.runtime->post([holder, released]
        {
            holder->stream.reset();
            released->set_value();
        });
        must_finish(released_future.wait_for(5s) == std::future_status::ready
                        ? std::optional<bool>(true) : std::nullopt,
                    "~ManagedEventStream on the Runtime thread");

        // The last reference to a PtzController released on the Runtime thread,
        // while its status worker waits on a slow GetStatus.
        struct ControllerHolder { std::shared_ptr<PtzController> controller; };
        auto controller_holder = std::make_shared<ControllerHolder>();
        auto controller_client = f.session->ptz().value();
        // Only the controller's workers hold this client, so its expiry shows
        // that the detached workers have really finished. That ordering also
        // keeps them from racing process-exit cleanup in libxml2.
        std::weak_ptr<PtzClient> controller_workers = controller_client;
        controller_holder->controller =
            PtzController::create(std::move(controller_client), "main").value();
        std::this_thread::sleep_for(100ms);
        auto destroyed = std::make_shared<std::promise<void>>();
        auto destroyed_future = destroyed->get_future();
        f.runtime->post([controller_holder, destroyed]
        {
            controller_holder->controller.reset();
            destroyed->set_value();
        });
        must_finish(destroyed_future.wait_for(5s) == std::future_status::ready
                        ? std::optional<bool>(true) : std::nullopt,
                    "~PtzController on the Runtime thread");
        const auto workers_deadline = Clock::now() + 5s;
        while (!controller_workers.expired() && Clock::now() < workers_deadline)
            std::this_thread::sleep_for(10ms);
        CHECK(controller_workers.expired());
        CHECK(must_finish(wait_for(f.session->close(2s)), "close"));
        CHECK(f.runtime->shutdown());
    }

    // ---- An observation rejected for overflow is not later dropped as a duplicate ----
    {
        auto runtime = Runtime::create().value();
        DiscoverySession discovery(runtime, 1);
        DiscoveryObservation first;
        first.endpoint_reference = "urn:uuid:first";
        first.source = "192.0.2.1";
        auto second = first;
        second.endpoint_reference = "urn:uuid:second";
        second.source = "192.0.2.2";
        CHECK(must_finish(wait_for(discovery.inject(first, options())), "inject first"));
        auto overflow = must_finish(wait_for(discovery.inject(second, options())), "overflow");
        CHECK(!overflow && overflow.error().category == ErrorCategory::queue_overflow);
        CHECK(must_finish(wait_for(discovery.next(options())), "next"));
        CHECK(must_finish(wait_for(discovery.inject(second, options())), "retransmission"));
        auto delivered = must_finish(wait_for(discovery.next(options())), "next second");
        CHECK(delivered && delivered.value().endpoint_reference == "urn:uuid:second");
        CHECK(runtime->shutdown());
    }

#ifndef _WIN32
    // ---- Curl transport: authorizer receives IPv6 literals without brackets ----
    {
        struct Recorder final : CandidateAuthorizer
        {
            std::mutex mutex;
            std::optional<CandidateAuthorizationRequest> seen;
            Task<Result<void>> authorize(CandidateAuthorizationRequest request, CallOptions) override
            {
                {
                    std::lock_guard lock(mutex);
                    seen = request;
                }
                return Task<Result<void>>([](auto done) { done(Result<void>{}); });
            }
        };
        auto runtime = Runtime::create().value();
        auto authorizer = std::make_shared<Recorder>();
        CurlTransportOptions transport_options;
        transport_options.runtime = runtime;
        transport_options.candidate_authorizer = authorizer;
        auto transport = make_curl_transport(transport_options);
        auto result = must_finish(wait_for(transport->send(
            HttpRequest{"http://[::1]:9/onvif", "x", {}, Clock::now() + 2s}, CancellationToken::linked(
                CancellationSource{}.token(), CancellationSource{}.token()))), "ipv6 send");
        CHECK(!result);
        CHECK(!result && result.error().message != "DNS resolution failed");
        std::lock_guard lock(authorizer->mutex);
        CHECK(authorizer->seen && authorizer->seen->proposed_address == "::1");
        CHECK(authorizer->seen && authorizer->seen->hostname == "::1");
        CHECK(runtime->shutdown());
    }

    // ---- Curl transport: cancellation is honoured while awaiting authorization ----
    {
        struct Silent final : CandidateAuthorizer
        {
            Task<Result<void>> authorize(CandidateAuthorizationRequest, CallOptions) override
            {
                return Task<Result<void>>([](auto) {});
            }
        };
        auto runtime = Runtime::create().value();
        CurlTransportOptions transport_options;
        transport_options.runtime = runtime;
        transport_options.candidate_authorizer = std::make_shared<Silent>();
        auto transport = make_curl_transport(transport_options);
        CancellationSource cancel;
        std::thread canceller([&] { std::this_thread::sleep_for(100ms); cancel.request_stop(); });
        const auto started = Clock::now();
        auto result = must_finish(wait_for(transport->send(
            HttpRequest{"http://127.0.0.1:9/onvif", "x", {}, Clock::now() + 10s}, cancel.token()
        )), "authorizer cancellation");
        canceller.join();
        CHECK(!result && result.error().category == ErrorCategory::cancelled);
        CHECK(Clock::now() - started < 1s);
        CHECK(runtime->shutdown());
    }

    // ---- Curl transport: the authenticated retry stays within the request deadline ----
    {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        socklen_t length = sizeof(address);
        CHECK(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        CHECK(::listen(listener, 4) == 0);
        CHECK(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        std::atomic_bool finished{false};
        std::thread server([&]
        {
            std::vector<int> clients;
            const auto drain = [](int client)
            {
                char buffer[4096];
                pollfd readable{client, POLLIN, 0};
                if (::poll(&readable, 1, 2000) > 0) (void)::recv(client, buffer, sizeof(buffer), 0);
            };
            // First request: answer slowly with a Digest challenge.
            const int first = ::accept(listener, nullptr, nullptr);
            clients.push_back(first);
            drain(first);
            std::this_thread::sleep_for(700ms);
            const std::string challenge =
                "HTTP/1.1 401 Unauthorized\r\n"
                "WWW-Authenticate: Digest realm=\"cam\", nonce=\"abc\", qop=\"auth\"\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";
            (void)::send(first, challenge.data(), challenge.size(), 0);
            ::shutdown(first, SHUT_WR);
            // Authenticated retry: accept and never answer.
            pollfd incoming{listener, POLLIN, 0};
            if (::poll(&incoming, 1, 3000) > 0) clients.push_back(::accept(listener, nullptr, nullptr));
            while (!finished.load()) std::this_thread::sleep_for(10ms);
            for (const int client : clients) ::close(client);
        });
        auto runtime = Runtime::create().value();
        auto transport = make_curl_transport(runtime, XmlLimits{});
        const auto url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/onvif";
        const auto started = Clock::now();
        auto result = must_finish(wait_for(transport->send(
            HttpRequest{url, "x", {}, started + 1200ms, Credentials{"u", "p"}},
            CancellationSource{}.token()
        )), "auth retry");
        const auto elapsed = Clock::now() - started;
        finished.store(true);
        server.join();
        ::close(listener);
        CHECK(!result && result.error().category == ErrorCategory::timeout);
        CHECK(elapsed < 1600ms);
        CHECK(runtime->shutdown());
    }
#endif

    if (failures != 0)
    {
        std::cerr << failures << " backend regression check(s) failed\n";
        return 1;
    }
    std::cout << "backend regression tests passed\n";
    return 0;
}
