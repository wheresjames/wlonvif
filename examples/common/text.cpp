/**
 * @file text.cpp
 * @brief Text helpers for example output: JSON and shell quoting, URLs, and names.
 */

#include "example_support.hpp"

namespace wlonvif::examples
{
    std::string json_escape(std::string_view input)
    {
        std::string output;
        for (const unsigned char value : input)
        {
            switch (value)
            {
                case '\\':
                case '"':
                    output += '\\';
                    output += static_cast<char>(value);
                    break;
                case '\b':
                    output += "\\b";
                    break;
                case '\f':
                    output += "\\f";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (value < 0x20)
                    {
                        // Other control characters as \u00XX; JSON allows none raw.
                        constexpr char digits[] = "0123456789abcdef";
                        output += "\\u00";
                        output += digits[value >> 4];
                        output += digits[value & 0x0f];
                    }
                    else
                    {
                        output += static_cast<char>(value);
                    }
            }
        }
        return output;
    }

    std::string shell_quote(std::string_view input)
    {
#ifdef _WIN32
        std::string output{"\""};
        for (const char value : input)
        {
            if (value == '\"' || value == '\\')
            {
                output += '\\';
            }
            output += value;
        }
        return output + '\"';
#else
        // Single quotes keep everything literal; a quote itself is closed,
        // escaped, and reopened: it's -> 'it'\''s'.
        std::string output{"'"};
        for (const char value : input)
        {
            output += value == '\'' ? "'\\''" : std::string(1, value);
        }
        return output + '\'';
#endif
    }

    namespace
    {
        /// Percent-encode everything but RFC 3986 unreserved characters.
        std::string percent_encode(std::string_view value)
        {
            constexpr char digits[] = "0123456789ABCDEF";
            std::string output;
            for (const auto character : value)
            {
                const auto byte = static_cast<unsigned char>(character);
                const bool unreserved = (byte >= 'a' && byte <= 'z') ||
                                        (byte >= 'A' && byte <= 'Z') ||
                                        (byte >= '0' && byte <= '9') || byte == '-' ||
                                        byte == '.' || byte == '_' || byte == '~';
                if (unreserved)
                {
                    output += static_cast<char>(byte);
                }
                else
                {
                    output += '%';
                    output += digits[byte >> 4];
                    output += digits[byte & 0x0f];
                }
            }
            return output;
        }
    } // namespace

    std::string
    url_with_credentials(std::string_view url, const std::optional<Credentials>& credentials)
    {
        if (!credentials || credentials->username.empty())
        {
            return std::string(url);
        }
        const auto scheme = url.find("://");
        if (scheme == url.npos)
        {
            return std::string(url);
        }
        // Replace any user information already in the authority.
        const auto authority_start = scheme + 3;
        const auto authority_end = url.find_first_of("/?#", authority_start);
        const auto at = url.find('@', authority_start);
        const bool has_user = at != url.npos && (authority_end == url.npos || at < authority_end);
        const auto host_start = has_user ? at + 1 : authority_start;
        return std::string(url.substr(0, authority_start)) + percent_encode(credentials->username) +
               ':' + percent_encode(credentials->password) + '@' +
               std::string(url.substr(host_start));
    }

    std::string redact_url(std::string_view url)
    {
        const auto scheme = url.find("://");
        if (scheme == url.npos)
        {
            return std::string(url);
        }
        const auto authority_start = scheme + 3;
        const auto authority_end = url.find_first_of("/?#", authority_start);
        const auto at = url.find('@', authority_start);
        if (at == url.npos || (authority_end != url.npos && at > authority_end))
        {
            return std::string(url);
        }
        return std::string(url.substr(0, authority_start)) + "[redacted]@" +
               std::string(url.substr(at + 1));
    }

    std::string_view operation_name(OperationId operation)
    {
        switch (operation)
        {
            case OperationId::get_device_information:
                return "GetDeviceInformation";
            case OperationId::get_system_date_and_time:
                return "GetSystemDateAndTime";
            case OperationId::get_scopes:
                return "GetScopes";
            case OperationId::get_services:
                return "GetServices";
            case OperationId::get_capabilities:
                return "GetCapabilities";
            case OperationId::get_profiles:
                return "GetProfiles";
            case OperationId::get_stream_uri:
                return "GetStreamUri";
            case OperationId::get_snapshot_uri:
                return "GetSnapshotUri";
            case OperationId::ptz_status:
                return "GetStatus";
            case OperationId::get_ptz_configuration_options:
                return "GetConfigurationOptions";
            case OperationId::continuous_move:
                return "ContinuousMove";
            case OperationId::relative_move:
                return "RelativeMove";
            case OperationId::absolute_move:
                return "AbsoluteMove";
            case OperationId::ptz_stop:
                return "Stop";
            case OperationId::get_presets:
                return "GetPresets";
            case OperationId::goto_preset:
                return "GotoPreset";
            case OperationId::discovery_probe:
                return "Probe";
            case OperationId::event_subscribe:
                return "CreatePullPointSubscription";
            case OperationId::event_pull:
                return "PullMessages";
            case OperationId::event_renew:
                return "Renew";
            case OperationId::event_close:
                return "Unsubscribe";
            case OperationId::set_system_date_and_time:
                return "SetSystemDateAndTime";
            case OperationId::get_ntp:
                return "GetNTP";
            case OperationId::set_ntp:
                return "SetNTP";
            case OperationId::set_scopes:
                return "SetScopes";
            case OperationId::add_scopes:
                return "AddScopes";
            case OperationId::remove_scopes:
                return "RemoveScopes";
            case OperationId::get_event_properties:
                return "GetEventProperties";
            case OperationId::get_analytics_modules:
                return "GetAnalyticsModules";
            case OperationId::modify_analytics_modules:
                return "ModifyAnalyticsModules";
            case OperationId::get_analytics_rules:
                return "GetRules";
            case OperationId::modify_analytics_rules:
                return "ModifyRules";
            case OperationId::get_video_sources:
                return "GetVideoSources";
            case OperationId::get_imaging_settings:
                return "GetImagingSettings";
            case OperationId::set_imaging_settings:
                return "SetImagingSettings";
            case OperationId::get_imaging_options:
                return "GetOptions";
            case OperationId::get_video_encoder_options:
                return "GetVideoEncoderConfigurationOptions";
            case OperationId::set_video_encoder_configuration:
                return "SetVideoEncoderConfiguration";
            case OperationId::set_preset:
                return "SetPreset";
            case OperationId::remove_preset:
                return "RemovePreset";
            case OperationId::get_preset_tours:
                return "GetPresetTours";
            case OperationId::create_preset_tour:
                return "CreatePresetTour";
            case OperationId::modify_preset_tour:
                return "ModifyPresetTour";
            case OperationId::operate_preset_tour:
                return "OperatePresetTour";
            case OperationId::remove_preset_tour:
                return "RemovePresetTour";
            case OperationId::get_ptz_configuration:
                return "GetConfiguration";
            case OperationId::set_ptz_configuration:
                return "SetConfiguration";
            case OperationId::get_hostname:
                return "GetHostname";
            case OperationId::set_hostname:
                return "SetHostname";
            case OperationId::get_network_interfaces:
                return "GetNetworkInterfaces";
            case OperationId::get_dns:
                return "GetDNS";
            case OperationId::get_network_default_gateway:
                return "GetNetworkDefaultGateway";
            case OperationId::get_network_protocols:
                return "GetNetworkProtocols";
            case OperationId::get_discovery_mode:
                return "GetDiscoveryMode";
            case OperationId::set_discovery_mode:
                return "SetDiscoveryMode";
            case OperationId::get_users:
                return "GetUsers";
            case OperationId::system_reboot:
                return "SystemReboot";
            case OperationId::custom:
                return "Custom request";
        }
        return "Unknown operation";
    }

    std::string_view error_category_name(ErrorCategory category)
    {
        switch (category)
        {
            case ErrorCategory::cancelled:
                return "cancelled";
            case ErrorCategory::timeout:
                return "timeout";
            case ErrorCategory::network:
                return "network";
            case ErrorCategory::tls:
                return "tls";
            case ErrorCategory::authentication:
                return "authentication";
            case ErrorCategory::authorization:
                return "authorization";
            case ErrorCategory::device_busy:
                return "device-busy";
            case ErrorCategory::rate_limited:
                return "rate-limited";
            case ErrorCategory::malformed_http:
                return "malformed-http";
            case ErrorCategory::malformed_xml:
                return "malformed-xml";
            case ErrorCategory::soap_fault:
                return "soap-fault";
            case ErrorCategory::unsupported:
                return "unsupported";
            case ErrorCategory::invalid_argument:
                return "invalid-argument";
            case ErrorCategory::endpoint_rejected:
                return "endpoint-rejected";
            case ErrorCategory::host_permission_denied:
                return "host-permission-denied";
            case ErrorCategory::closed_handle:
                return "closed-handle";
            case ErrorCategory::protocol_violation:
                return "protocol-violation";
            case ErrorCategory::closed:
                return "closed";
            case ErrorCategory::queue_overflow:
                return "queue-overflow";
            case ErrorCategory::internal:
                return "internal";
        }
        return "unknown";
    }
} // namespace wlonvif::examples
