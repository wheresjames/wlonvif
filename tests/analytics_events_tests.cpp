/**
 * @file analytics_events_tests.cpp
 * @brief Event topics, topic filters, and analytics modules and rules.
 */

#include "recorded_camera_transport.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <optional>

using namespace wlonvif;
using namespace std::chrono_literals;
using testing::RecordedCameraTransport;
using testing::recorded_envelope;

namespace
{
    int failures = 0;

#define CHECK(expression)                                                                          \
    do                                                                                             \
    {                                                                                              \
        if (!(expression))                                                                         \
        {                                                                                          \
            std::cerr << "analytics/events check failed: " #expression " at line " << __LINE__  \
                      << '\n';                                                                     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

    template <class T> T sync(Task<T> task)
    {
        struct State
        {
            std::mutex mutex;
            std::condition_variable ready;
            std::optional<T> value;
        };
        auto state = std::make_shared<State>();
        std::move(task).start(
            [state](T value)
            {
                std::lock_guard lock(state->mutex);
                state->value.emplace(std::move(value));
                state->ready.notify_one();
            }
        );
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock, [&] { return state->value.has_value(); });
        return std::move(*state->value);
    }

    CallOptions options()
    {
        return {std::chrono::steady_clock::now() + 5s};
    }

    std::shared_ptr<DeviceSession> connect(
        const std::shared_ptr<Runtime>& runtime, std::shared_ptr<HttpTransport> transport
    )
    {
        auto session = sync(DeviceSession::connect(
            "http://192.0.2.10:2020/onvif/device_service", std::move(transport),
            make_same_host_policy(), runtime, std::nullopt, options()
        ));
        CHECK(session);
        return session.value();
    }

    const SimpleItem* item(const AnalyticsConfig& config, std::string_view name)
    {
        for (const auto& entry : config.simple_items)
            if (entry.name == name) return &entry;
        return nullptr;
    }

    Event event(std::string topic)
    {
        Event output;
        output.topic = std::move(topic);
        return output;
    }
} // namespace

int main()
{
    auto runtime = Runtime::create().value();

    // ---- Recorded camera: topics, filters, profiles, analytics ----
    {
        auto device = std::make_shared<RecordedCameraTransport>();
        device->serve("GetCapabilities", "GetCapabilities.xml");
        device->serve("GetEventProperties", "GetEventProperties.xml");
        device->serve("GetProfiles", "GetProfiles.xml");
        device->serve("GetAnalyticsModules", "GetAnalyticsModules.xml");
        device->serve("GetRules", "GetRules.xml");
        device->reply("CreatePullPointSubscription", 200, recorded_envelope(
            "<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference><wsa5:Address>"
            "http://192.0.2.10:1024/event-1024_1024</wsa5:Address></tev:SubscriptionReference>"
            "<wsnt:CurrentTime>2026-10-20T11:16:31Z</wsnt:CurrentTime><wsnt:TerminationTime>"
            "2026-10-20T11:17:31Z</wsnt:TerminationTime></tev:CreatePullPointSubscriptionResponse>"));
        device->reply("ModifyAnalyticsModules", 200,
                      recorded_envelope("<tan:ModifyAnalyticsModulesResponse></tan:ModifyAnalyticsModulesResponse>"));
        device->reply("ModifyRules", 200, recorded_envelope("<tan:ModifyRulesResponse></tan:ModifyRulesResponse>"));
        auto session = connect(runtime, device);

        auto topics = sync(session->event_client().value()->get_topics(options()));
        CHECK(topics && topics.value().size() == 6);
        if (topics && !topics.value().empty())
        {
            const auto& motion = topics.value().front();
            CHECK(motion.topic == "tns1:RuleEngine/CellMotionDetector/Motion");
            CHECK(motion.property);
            CHECK(motion.source == std::vector<std::string>(
                {"VideoSourceConfigurationToken", "VideoAnalyticsConfigurationToken", "Rule"}));
            CHECK(motion.data == std::vector<std::string>{"IsMotion"});
            CHECK(topics.value().back().topic == "tns1:RuleEngine/TPSmartEventDetector/TPSmartEvent");
        }

        PullPointSubscriptionOptions filtered;
        filtered.topics = {"tns1:RuleEngine/CellMotionDetector/Motion",
                           "tns1:RuleEngine/TamperDetector/Tamper"};
        auto subscription = sync(session->event_client().value()->create_pullpoint(filtered, options()));
        CHECK(subscription);
        const auto request = device->last("CreatePullPointSubscription");
        CHECK(request.find("<wsnt:TopicExpression Dialect=\"http://www.onvif.org/ver10/tev/"
                           "topicExpression/ConcreteSet\">tns1:RuleEngine/CellMotionDetector/Motion|"
                           "tns1:RuleEngine/TamperDetector/Tamper</wsnt:TopicExpression>") !=
              std::string::npos);
        // The topic prefix must be declared for the device to resolve it.
        CHECK(request.find("xmlns:tns1=\"http://www.onvif.org/ver10/topics\"") != std::string::npos);
        // Filter precedes InitialTerminationTime, as the WSDL sequence requires.
        CHECK(request.find("<tev:Filter>") < request.find("<tev:InitialTerminationTime>"));
        auto unfiltered = sync(session->event_client().value()->create_pullpoint({60s, {}}, options()));
        CHECK(unfiltered && device->last("CreatePullPointSubscription").find("Filter") == std::string::npos);

        auto profiles = sync(session->media().value()->get_profiles(options()));
        CHECK(profiles && profiles.value().size() == 3);
        CHECK(profiles && std::all_of(profiles.value().begin(), profiles.value().end(),
                                      [](const auto& profile)
                                      { return profile.video_analytics_configuration == "VideoAnalyticsToken"; }));

        auto analytics = session->analytics().value();
        auto modules = sync(analytics->get_modules("VideoAnalyticsToken", options()));
        CHECK(modules && modules.value().size() == 2);
        if (modules && modules.value().size() == 2)
        {
            const auto& motion = modules.value()[0];
            CHECK(motion.name == "MyCellMotionModule" && motion.type == "tt:CellMotionEngine");
            CHECK(item(motion, "Sensitivity") && item(motion, "Sensitivity")->value == "60");
            CHECK(motion.element_items.size() == 1 && motion.element_items[0].name == "Layout");
            CHECK(motion.element_items[0].xml.find("<tt:CellLayout Rows=\"18\" Columns=\"22\">") == 0);
            const auto& tamper = modules.value()[1];
            CHECK(tamper.simple_items.size() == 1 && tamper.element_items.size() == 2);

            // A change keeps every structured parameter exactly as received.
            auto changed = motion;
            const_cast<SimpleItem*>(item(changed, "Sensitivity"))->value = "80";
            CHECK(sync(analytics->modify_modules("VideoAnalyticsToken", {changed}, options())));
            const auto body = device->last("ModifyAnalyticsModules");
            CHECK(body.find("<tan:ConfigurationToken>VideoAnalyticsToken</tan:ConfigurationToken>") !=
                  std::string::npos);
            CHECK(body.find("<tan:AnalyticsModule Name=\"MyCellMotionModule\" Type=\"tt:CellMotionEngine\">") !=
                  std::string::npos);
            CHECK(body.find("<tt:SimpleItem Name=\"Sensitivity\" Value=\"80\"/>") != std::string::npos);
            CHECK(body.find("<tt:ElementItem Name=\"Layout\">" + motion.element_items[0].xml +
                            "</tt:ElementItem>") != std::string::npos);
        }

        auto rules = sync(analytics->get_rules("VideoAnalyticsToken", options()));
        CHECK(rules && rules.value().size() == 1);
        if (rules && rules.value().size() == 1)
        {
            const auto& rule = rules.value()[0];
            CHECK(rule.name == "MyMotionDetectorRule" && rule.type == "tt:CellMotionDetector");
            CHECK(rule.simple_items.size() == 4);
            CHECK(item(rule, "ActiveCells") && item(rule, "ActiveCells")->value == "0P8A8A==");
            auto changed = rule;
            changed.simple_items[1].value = "3";
            CHECK(sync(analytics->modify_rules("VideoAnalyticsToken", {changed}, options())));
            CHECK(device->last("ModifyRules").find(
                      "<tan:Rule Name=\"MyMotionDetectorRule\" Type=\"tt:CellMotionDetector\">") !=
                  std::string::npos);
        }

        // Requests that cannot be applied are refused before reaching the device.
        auto no_token = sync(analytics->modify_modules("", {AnalyticsConfig{"m", "tt:T", {}, {}}}, options()));
        CHECK(!no_token && no_token.error().category == ErrorCategory::invalid_argument);
        auto nothing = sync(analytics->modify_rules("VideoAnalyticsToken", {}, options()));
        CHECK(!nothing && nothing.error().category == ErrorCategory::invalid_argument);
        auto unnamed = sync(analytics->modify_rules("VideoAnalyticsToken", {AnalyticsConfig{}}, options()));
        CHECK(!unnamed && unnamed.error().category == ErrorCategory::invalid_argument);
    }

    // ---- Virtual device: filtered delivery and analytics round trip ----
    {
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime, DeviceInformation{}, std::vector<std::string>{}, std::vector<Service>{},
            Capabilities{}
        );
        device->set_event_topics({{"tns1:RuleEngine/CellMotionDetector/Motion", true, {}, {"IsMotion"}}});
        device->set_analytics(
            "analytics",
            {AnalyticsConfig{"Motion", "tt:CellMotionEngine", {{"Sensitivity", "50"}}, {}}},
            {AnalyticsConfig{"MotionRule", "tt:CellMotionDetector", {{"MinCount", "5"}}, {}}}
        );
        auto session = connect(runtime, device);
        auto events = session->event_client().value();
        auto topics = sync(events->get_topics(options()));
        CHECK(topics && topics.value().size() == 1 && topics.value()[0].data[0] == "IsMotion");

        auto subscription = sync(events->create_pullpoint(
            {60s, {"tns1:RuleEngine/CellMotionDetector/Motion", "tns1:VideoSource//."}}, options()
        ));
        CHECK(subscription);
        device->emit(event("tns1:RuleEngine/TamperDetector/Tamper"));
        device->emit(event("tns1:RuleEngine/CellMotionDetector/Motion"));
        device->emit(event("tns1:VideoSource/GlobalSceneChange/ImagingService"));
        device->emit(event("tns1:VideoSourceX/Other"));
        auto pulled = sync(subscription.value()->pull(100ms, 10, options()));
        CHECK(pulled && pulled.value().size() == 2);
        CHECK(pulled && pulled.value().size() == 2 &&
              pulled.value()[0].topic == "tns1:RuleEngine/CellMotionDetector/Motion" &&
              pulled.value()[1].topic == "tns1:VideoSource/GlobalSceneChange/ImagingService");

        // A managed stream keeps its filter.
        ManagedEventOptions managed;
        managed.topics = {"tns1:RuleEngine/TamperDetector/Tamper"};
        managed.pull_timeout = 100ms;
        auto stream = sync(events->open_managed_stream(managed, options()));
        CHECK(stream);
        device->emit(event("tns1:RuleEngine/CellMotionDetector/Motion"));
        device->emit(event("tns1:RuleEngine/TamperDetector/Tamper"));
        auto next = sync(stream.value()->next(options()));
        CHECK(next && std::holds_alternative<Event>(next.value()) &&
              std::get<Event>(next.value()).topic == "tns1:RuleEngine/TamperDetector/Tamper");
        (void)sync(stream.value()->close(options()));

        auto analytics = session->analytics().value();
        auto modules = sync(analytics->get_modules("analytics", options()));
        CHECK(modules && modules.value().size() == 1);
        auto changed = modules.value()[0];
        changed.simple_items[0].value = "90";
        CHECK(sync(analytics->modify_modules("analytics", {changed}, options())));
        modules = sync(analytics->get_modules("analytics", options()));
        CHECK(modules && modules.value()[0].simple_items[0].value == "90");
        auto unknown = sync(analytics->modify_rules(
            "analytics", {AnalyticsConfig{"Missing", "tt:T", {}, {}}}, options()
        ));
        CHECK(!unknown && unknown.error().category == ErrorCategory::invalid_argument);
        auto rules = sync(analytics->get_rules("analytics", options()));
        CHECK(rules && rules.value()[0].simple_items[0].value == "5");
        auto missing = sync(analytics->get_rules("other", options()));
        CHECK(!missing);
    }

    CHECK(bool(runtime->shutdown()));
    if (failures != 0)
    {
        std::cerr << failures << " analytics/events check(s) failed\n";
        return 1;
    }
    std::cout << "analytics and events tests passed\n";
    return 0;
}
