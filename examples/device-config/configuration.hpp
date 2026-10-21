#pragma once

/**
 * @file configuration.hpp
 * @brief Reading, printing, and describing a device's clock, NTP, network, and scopes.
 */

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <wlonvif/wlonvif.hpp>

namespace device_config
{
    /// Scopes under this prefix hold the device's location, percent-encoded.
    inline constexpr std::string_view LocationPrefix = "onvif://www.onvif.org/location/";

    /// Everything the example shows. Only the clock and scopes are required;
    /// cameras implement the rest unevenly, so missing parts are left out.
    struct Configuration
    {
        wlonvif::SystemDateAndTime clock;
        std::chrono::seconds skew{}; ///< Device clock minus this computer's clock.
        std::optional<wlonvif::NtpConfiguration> ntp;
        std::vector<wlonvif::ScopeEntry> scopes;
        std::optional<wlonvif::HostnameInformation> hostname;
        std::vector<wlonvif::NetworkInterface> interfaces;
        std::vector<std::string> gateways;
        std::optional<wlonvif::DnsInformation> dns;
        std::vector<wlonvif::NetworkProtocol> protocols;
        std::optional<wlonvif::DiscoveryMode> discovery;
        std::vector<wlonvif::DeviceUser> users;
    };

    /// Read the configuration; fails only when the clock or scopes cannot be read.
    wlonvif::Result<Configuration>
    read_configuration(const std::shared_ptr<wlonvif::DeviceSession>& device, int timeout);

    /// Print as aligned text, or as one JSON object with @p json.
    void print_configuration(const Configuration& configuration, bool json);

    /// ISO 8601 UTC with second precision, such as `2026-10-20T16:08:21Z`.
    std::string utc_text(std::chrono::system_clock::time_point value);

    /// How far the device clock is from this computer's, such as `1h 2m 5s behind this computer`.
    std::string skew_text(std::chrono::seconds skew);

    /// Percent-encode everything but unreserved characters, for scope URIs.
    std::string percent_encode(std::string_view text);
    std::string percent_decode(std::string_view text);

    /// Explain the device's reason for refusing a change, where ONVIF defines one.
    std::string refusal_text(const wlonvif::Error& error);

    /// "a, b, c".
    std::string joined(const std::vector<std::string>& values);
} // namespace device_config
