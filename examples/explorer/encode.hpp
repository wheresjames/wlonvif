#pragma once

/**
 * @file encode.hpp
 * @brief JSON views of wlonvif results for the Explorer API.
 */

#include <boost/json.hpp>
#include <chrono>
#include <string>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples::explorer
{
    namespace json = boost::json;

    /// Everything an Error carries, for display and bug reports.
    json::object error_json(const Error& error);

    /// ISO 8601 UTC text with second precision.
    std::string utc_text(std::chrono::system_clock::time_point time);

    /// Replace user information in every URL inside @p text with `***`.
    std::string redact_credentials(std::string text);

    json::value to_json(const DeviceInformation& value);
    json::value to_json(const SystemDateAndTime& value);
    json::value to_json(const NtpConfiguration& value);
    json::value to_json(const std::vector<ScopeEntry>& value);
    json::value to_json(const std::vector<Service>& value);
    json::value to_json(const Capabilities& value);
    json::value to_json(const ProfileConfiguration& value);
    json::value to_json(const VideoEncoderConfiguration& value);
    json::value to_json(const VideoEncoderOptions& value);
    json::value to_json(const VideoSource& value);
    json::value to_json(const ImagingSettings& value);
    json::value to_json(const ImagingOptions& value);
    json::value to_json(const PtzConfiguration& value);
    json::value to_json(const PtzStatus& value);
    json::value to_json(const PtzSpaces& value);
    json::value to_json(const std::vector<Preset>& value);
    json::value to_json(const std::vector<PresetTour>& value);
    json::value to_json(const std::vector<AnalyticsConfig>& value);
    json::value to_json(const std::vector<EventTopic>& value);
    json::value to_json(const Event& value);
    json::value to_json(const HostnameInformation& value);
    json::value to_json(const std::vector<NetworkInterface>& value);
    json::value to_json(const DnsInformation& value);
    json::value to_json(const std::vector<NetworkProtocol>& value);
    json::value to_json(const std::vector<DeviceUser>& value);
    json::value to_json(DiscoveryMode value);
    json::value to_json(const std::vector<std::string>& value);

    /// Decode the imaging fields present in @p object; numbers must be finite.
    Result<ImagingSettings> imaging_settings_from(const json::object& object);
} // namespace wlonvif::examples::explorer
