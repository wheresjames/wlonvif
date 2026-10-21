/**
 * @file ptz_management_tests.cpp
 * @brief PTZ preset management, preset tours, and image orientation.
 */

#include "recorded_camera_transport.hpp"

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
            std::cerr << "ptz management check failed: " #expression " at line " << __LINE__     \
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

    std::string empty_reply(const std::string& operation)
    {
        return recorded_envelope("<tptz:" + operation + "Response></tptz:" + operation + "Response>");
    }
} // namespace

int main()
{
    auto runtime = Runtime::create().value();

    // ---- Recorded camera ----
    {
        auto device = std::make_shared<RecordedCameraTransport>();
        device->serve("GetCapabilities", "GetCapabilities.xml");
        device->serve("GetProfiles", "GetProfiles.xml");
        device->serve("GetPresets", "GetPresets.xml");
        device->serve("GetPresetTours", "GetPresetToursPatrol.xml");
        device->serve("GetConfiguration", "GetPTZConfiguration.xml");
        device->serve("GetConfigurationOptions", "GetPTZConfigurationOptions.xml");
        device->reply("SetPreset", 200, recorded_envelope(
            "<tptz:SetPresetResponse><tptz:PresetToken>1</tptz:PresetToken></tptz:SetPresetResponse>"));
        device->reply("CreatePresetTour", 200, recorded_envelope(
            "<tptz:CreatePresetTourResponse><tptz:PresetTourToken>1</tptz:PresetTourToken>"
            "</tptz:CreatePresetTourResponse>"));
        for (const auto operation : {"RemovePreset", "ModifyPresetTour", "OperatePresetTour",
                                     "RemovePresetTour", "SetConfiguration"})
            device->reply(operation, 200, empty_reply(operation));
        auto session = connect(runtime, device);
        auto ptz = session->ptz().value();

        auto presets = sync(ptz->get_presets("profile_1", options()));
        CHECK(presets && presets.value().size() == 1 && presets.value()[0].name == "Door");

        auto saved = sync(ptz->set_preset("profile_1", "Door", {}, options()));
        CHECK(saved && saved.value() == "1");
        CHECK(device->last("SetPreset").find("<tptz:ProfileToken>profile_1</tptz:ProfileToken>"
                                             "<tptz:PresetName>Door</tptz:PresetName></tptz:SetPreset>") !=
              std::string::npos);
        CHECK(sync(ptz->set_preset("profile_1", {}, "1", options())));
        CHECK(device->last("SetPreset").find("<tptz:PresetToken>1</tptz:PresetToken>") != std::string::npos);
        CHECK(device->last("SetPreset").find("PresetName") == std::string::npos);
        CHECK(sync(ptz->remove_preset("profile_1", "1", options())));
        CHECK(device->last("RemovePreset").find("<tptz:PresetToken>1</tptz:PresetToken>") != std::string::npos);

        auto tours = sync(ptz->get_preset_tours("profile_1", options()));
        CHECK(tours && tours.value().size() == 1);
        if (tours && tours.value().size() == 1)
        {
            const auto& tour = tours.value()[0];
            CHECK(tour.token == "1" && tour.name == "Patrol");
            CHECK(tour.state == PresetTourState::idle && tour.auto_start);
            CHECK(tour.spots.size() == 1 && tour.spots[0].preset == "1" && tour.spots[0].stay_time == 60s);
        }
        auto created = sync(ptz->create_preset_tour("profile_1", options()));
        CHECK(created && created.value() == "1");
        PresetTour tour{"1", "Patrol", PresetTourState::idle, false, {{"1", 10s}, {"2", 0s}}};
        CHECK(sync(ptz->modify_preset_tour("profile_1", tour, options())));
        const auto modify = device->last("ModifyPresetTour");
        CHECK(modify.find("<tptz:PresetTour token=\"1\"><tt:Name>Patrol</tt:Name><tt:Status><tt:State>Idle"
                          "</tt:State></tt:Status><tt:AutoStart>false</tt:AutoStart><tt:StartingCondition/>"
                          "<tt:TourSpot><tt:PresetDetail><tt:PresetToken>1</tt:PresetToken></tt:PresetDetail>"
                          "<tt:StayTime>PT10S</tt:StayTime></tt:TourSpot><tt:TourSpot><tt:PresetDetail>"
                          "<tt:PresetToken>2</tt:PresetToken></tt:PresetDetail></tt:TourSpot></tptz:PresetTour>") !=
              std::string::npos);
        CHECK(sync(ptz->operate_preset_tour("profile_1", "1", PresetTourOperation::start, options())));
        CHECK(device->last("OperatePresetTour").find("<tptz:Operation>Start</tptz:Operation>") != std::string::npos);
        CHECK(sync(ptz->operate_preset_tour("profile_1", "1", PresetTourOperation::pause, options())));
        CHECK(device->last("OperatePresetTour").find("<tptz:Operation>Pause</tptz:Operation>") != std::string::npos);
        CHECK(sync(ptz->remove_preset_tour("profile_1", "1", options())));

        auto configuration = sync(ptz->get_configuration("profile_1", options()));
        CHECK(configuration && configuration.value().token == "PTZTOKEN");
        if (configuration)
        {
            const auto& value = configuration.value();
            CHECK(value.name == "PTZ" && value.node_token == "PTZNODETOKEN");
            CHECK(value.default_timeout == 180s);
            CHECK(value.direction.eflip == "OFF" && value.direction.reverse == "OFF");
            CHECK(value.eflip_modes == std::vector<std::string>({"OFF", "ON"}));
            CHECK(value.reverse_modes == std::vector<std::string>({"OFF", "ON"}));
        }
        CHECK(sync(ptz->set_direction("profile_1", {"ON", std::nullopt}, true, options())));
        const auto set = device->last("SetConfiguration");
        CHECK(set.find("<tptz:PTZConfiguration token=\"PTZTOKEN\">") != std::string::npos);
        CHECK(set.find("<tt:EFlip><tt:Mode>ON</tt:Mode></tt:EFlip><tt:Reverse><tt:Mode>OFF</tt:Mode>") !=
              std::string::npos);
        // Everything else is the device's own configuration, unchanged.
        CHECK(set.find("<tt:DefaultPTZTimeout>PT180S</tt:DefaultPTZTimeout>") != std::string::npos);
        CHECK(set.find("<tt:NodeToken>PTZNODETOKEN</tt:NodeToken>") != std::string::npos);
        CHECK(set.find("</tptz:PTZConfiguration><tptz:ForcePersistence>true</tptz:ForcePersistence>") !=
              std::string::npos);

        // Requests that cannot be applied are refused locally.
        CHECK(!sync(ptz->set_preset("profile_1", {}, {}, options())));
        CHECK(!sync(ptz->remove_preset("profile_1", {}, options())));
        CHECK(!sync(ptz->modify_preset_tour("profile_1", PresetTour{}, options())));
        CHECK(!sync(ptz->modify_preset_tour(
            "profile_1", PresetTour{"1", "x", PresetTourState::idle, false, {{"", 1s}}}, options())));
        CHECK(!sync(ptz->operate_preset_tour("profile_1", {}, PresetTourOperation::stop, options())));
        CHECK(!sync(ptz->set_direction("profile_1", {}, true, options())));
    }

    // ---- Virtual device ----
    {
        auto device = std::make_shared<VirtualDeviceTransport>(
            runtime, DeviceInformation{}, std::vector<std::string>{}, std::vector<Service>{},
            Capabilities{}
        );
        device->add_profile({"main", "Main", {}}, MediaUri{}, MediaUri{}, PtzSpaces{});
        PtzConfiguration configuration;
        configuration.token = "ptz";
        configuration.direction = {"OFF", "OFF"};
        configuration.eflip_modes = {"OFF", "ON"};
        configuration.reverse_modes = {"OFF", "ON"};
        device->set_ptz_configuration("main", configuration);
        auto session = connect(runtime, device);
        auto ptz = session->ptz().value();

        auto first = sync(ptz->set_preset("main", "First", {}, options()));
        auto second = sync(ptz->set_preset("main", "Second", {}, options()));
        CHECK(first && second && first.value() != second.value());
        auto presets = sync(ptz->get_presets("main", options()));
        CHECK(presets && presets.value().size() == 2 && presets.value()[1].name == "Second");
        CHECK(sync(ptz->set_preset("main", "Renamed", first.value(), options())));
        presets = sync(ptz->get_presets("main", options()));
        CHECK(presets && presets.value()[0].name == "Renamed" && presets.value().size() == 2);

        auto tour = sync(ptz->create_preset_tour("main", options()));
        CHECK(tour);
        PresetTour value{tour.value(), "Loop", PresetTourState::idle, true,
                         {{first.value(), 5s}, {second.value(), 7s}}};
        CHECK(sync(ptz->modify_preset_tour("main", value, options())));
        auto bad = value;
        bad.spots.push_back({"missing", 1s});
        CHECK(!sync(ptz->modify_preset_tour("main", bad, options())));
        CHECK(sync(ptz->operate_preset_tour("main", tour.value(), PresetTourOperation::start, options())));
        auto tours = sync(ptz->get_preset_tours("main", options()));
        CHECK(tours && tours.value().size() == 1 && tours.value()[0].state == PresetTourState::touring);
        CHECK(tours && tours.value()[0].spots.size() == 2 && tours.value()[0].spots[1].stay_time == 7s);
        CHECK(sync(ptz->remove_preset_tour("main", tour.value(), options())));
        CHECK(!sync(ptz->remove_preset_tour("main", tour.value(), options())));
        CHECK(sync(ptz->remove_preset("main", second.value(), options())));
        presets = sync(ptz->get_presets("main", options()));
        CHECK(presets && presets.value().size() == 1);

        CHECK(sync(ptz->set_direction("main", {"ON", std::nullopt}, true, options())));
        auto stored = sync(ptz->get_configuration("main", options()));
        CHECK(stored && stored.value().direction.eflip == "ON" && stored.value().direction.reverse == "OFF");
        CHECK(!sync(ptz->set_direction("main", {"SIDEWAYS", std::nullopt}, true, options())));
    }

    CHECK(bool(runtime->shutdown()));
    if (failures != 0)
    {
        std::cerr << failures << " ptz management check(s) failed\n";
        return 1;
    }
    std::cout << "ptz management tests passed\n";
    return 0;
}
