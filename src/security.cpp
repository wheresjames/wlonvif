#include "xml_scan.hpp"

#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    static std::string b64(const unsigned char* data, std::size_t size)
    {
        // EVP_EncodeBlock also writes a terminating NUL, so the buffer needs room
        // for it rather than relying on the string's own terminator storage.
        std::vector<char> buffer(4 * ((size + 2) / 3) + 1, '\0');
        auto n = EVP_EncodeBlock(
            reinterpret_cast<unsigned char*>(buffer.data()), data, static_cast<int>(size)
        );
        if (n < 0)
        {
            return {};
        }
        return std::string(buffer.data(), static_cast<std::size_t>(n));
    }
    std::string username_token_digest(
        std::string_view nonce, std::string_view created, std::string_view password
    )
    {
        std::vector<unsigned char> input;
        input.reserve(nonce.size() + created.size() + password.size());
        input.insert(input.end(), nonce.begin(), nonce.end());
        input.insert(input.end(), created.begin(), created.end());
        input.insert(input.end(), password.begin(), password.end());
        std::array<unsigned char, SHA_DIGEST_LENGTH> digest{};
        SHA1(input.data(), input.size(), digest.data());
        OPENSSL_cleanse(input.data(), input.size());
        return b64(digest.data(), digest.size());
    }
    std::string soap_envelope(
        OperationId op,
        std::string_view body,
        const std::optional<Credentials>& credentials,
        std::chrono::system_clock::duration clock_offset
    )
    {
        struct Operation
        {
            std::string_view prefix;
            std::string_view name;
        };
        Operation operation{"tds", "GetDeviceInformation"};
        switch (op)
        {
            case OperationId::get_device_information: break;
            case OperationId::get_system_date_and_time:
                operation = {"tds", "GetSystemDateAndTime"}; break;
            case OperationId::get_scopes: operation = {"tds", "GetScopes"}; break;
            case OperationId::get_services: operation = {"tds", "GetServices"}; break;
            case OperationId::get_capabilities: operation = {"tds", "GetCapabilities"}; break;
            case OperationId::get_profiles: operation = {"trt", "GetProfiles"}; break;
            case OperationId::get_stream_uri: operation = {"trt", "GetStreamUri"}; break;
            case OperationId::get_snapshot_uri: operation = {"trt", "GetSnapshotUri"}; break;
            case OperationId::ptz_status: operation = {"tptz", "GetStatus"}; break;
            case OperationId::get_ptz_configuration_options:
                operation = {"tptz", "GetConfigurationOptions"}; break;
            case OperationId::continuous_move: operation = {"tptz", "ContinuousMove"}; break;
            case OperationId::relative_move: operation = {"tptz", "RelativeMove"}; break;
            case OperationId::absolute_move: operation = {"tptz", "AbsoluteMove"}; break;
            case OperationId::ptz_stop: operation = {"tptz", "Stop"}; break;
            case OperationId::get_presets: operation = {"tptz", "GetPresets"}; break;
            case OperationId::goto_preset: operation = {"tptz", "GotoPreset"}; break;
            case OperationId::event_subscribe:
                operation = {"tev", "CreatePullPointSubscription"}; break;
            case OperationId::event_pull: operation = {"tev", "PullMessages"}; break;
            case OperationId::event_renew: operation = {"wsnt", "Renew"}; break;
            case OperationId::event_close: operation = {"wsnt", "Unsubscribe"}; break;
            case OperationId::discovery_probe: operation = {"tds", "Probe"}; break;
            case OperationId::set_system_date_and_time:
                operation = {"tds", "SetSystemDateAndTime"}; break;
            case OperationId::get_ntp: operation = {"tds", "GetNTP"}; break;
            case OperationId::set_ntp: operation = {"tds", "SetNTP"}; break;
            case OperationId::set_scopes: operation = {"tds", "SetScopes"}; break;
            case OperationId::add_scopes: operation = {"tds", "AddScopes"}; break;
            case OperationId::remove_scopes: operation = {"tds", "RemoveScopes"}; break;
            case OperationId::get_event_properties:
                operation = {"tev", "GetEventProperties"}; break;
            case OperationId::get_analytics_modules:
                operation = {"tan", "GetAnalyticsModules"}; break;
            case OperationId::modify_analytics_modules:
                operation = {"tan", "ModifyAnalyticsModules"}; break;
            case OperationId::get_analytics_rules: operation = {"tan", "GetRules"}; break;
            case OperationId::modify_analytics_rules: operation = {"tan", "ModifyRules"}; break;
            case OperationId::get_video_sources: operation = {"trt", "GetVideoSources"}; break;
            case OperationId::get_imaging_settings:
                operation = {"timg", "GetImagingSettings"}; break;
            case OperationId::set_imaging_settings:
                operation = {"timg", "SetImagingSettings"}; break;
            case OperationId::get_imaging_options: operation = {"timg", "GetOptions"}; break;
            case OperationId::get_video_encoder_options:
                operation = {"trt", "GetVideoEncoderConfigurationOptions"}; break;
            case OperationId::set_video_encoder_configuration:
                operation = {"trt", "SetVideoEncoderConfiguration"}; break;
            case OperationId::set_preset: operation = {"tptz", "SetPreset"}; break;
            case OperationId::remove_preset: operation = {"tptz", "RemovePreset"}; break;
            case OperationId::get_preset_tours: operation = {"tptz", "GetPresetTours"}; break;
            case OperationId::create_preset_tour: operation = {"tptz", "CreatePresetTour"}; break;
            case OperationId::modify_preset_tour: operation = {"tptz", "ModifyPresetTour"}; break;
            case OperationId::operate_preset_tour: operation = {"tptz", "OperatePresetTour"}; break;
            case OperationId::remove_preset_tour: operation = {"tptz", "RemovePresetTour"}; break;
            case OperationId::get_ptz_configuration: operation = {"tptz", "GetConfiguration"}; break;
            case OperationId::set_ptz_configuration: operation = {"tptz", "SetConfiguration"}; break;
            case OperationId::get_hostname: operation = {"tds", "GetHostname"}; break;
            case OperationId::set_hostname: operation = {"tds", "SetHostname"}; break;
            case OperationId::get_network_interfaces:
                operation = {"tds", "GetNetworkInterfaces"}; break;
            case OperationId::get_dns: operation = {"tds", "GetDNS"}; break;
            case OperationId::get_network_default_gateway:
                operation = {"tds", "GetNetworkDefaultGateway"}; break;
            case OperationId::get_network_protocols: operation = {"tds", "GetNetworkProtocols"}; break;
            case OperationId::get_discovery_mode: operation = {"tds", "GetDiscoveryMode"}; break;
            case OperationId::set_discovery_mode: operation = {"tds", "SetDiscoveryMode"}; break;
            case OperationId::get_users: operation = {"tds", "GetUsers"}; break;
            case OperationId::system_reboot: operation = {"tds", "SystemReboot"}; break;
            case OperationId::custom: break;
        }
        std::string security;
        if (credentials)
        {
            std::array<unsigned char, 16> nonce{};
            // A zero nonce would defeat the UsernameToken replay protection the
            // digest exists to provide, so a failing CSPRNG must fail the request.
            if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
            {
                throw std::runtime_error("secure random nonce unavailable");
            }
            // Created is checked against the device's clock, not ours.
            const auto now = std::chrono::system_clock::to_time_t(
                std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    std::chrono::system_clock::now() + clock_offset
                )
            );
            std::tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &now);
#else
            gmtime_r(&now, &utc);
#endif
            std::ostringstream timestamp;
            timestamp << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
            const std::string raw_nonce(
                reinterpret_cast<const char*>(nonce.data()), nonce.size()
            );
            security = "<wsse:Security><wsse:UsernameToken><wsse:Username>" +
                       detail_xml::escape(credentials->username) + "</wsse:Username>"
                       "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
                       "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" +
                       username_token_digest(raw_nonce, timestamp.str(), credentials->password) +
                       "</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/"
                       "wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" +
                       b64(nonce.data(), nonce.size()) + "</wsse:Nonce><wsu:Created>" +
                       timestamp.str() +
                       "</wsu:Created></wsse:UsernameToken></wsse:Security>";
        }
        return "<?xml version=\"1.0\"?><s:Envelope "
               "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
               "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
               "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" "
               "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
               "xmlns:tan=\"http://www.onvif.org/ver20/analytics/wsdl\" "
               "xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" "
               "xmlns:tns1=\"http://www.onvif.org/ver10/topics\" "
               "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
               "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
               "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-wssecurity-utility-1.0.xsd\" "
               "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><s:Header>" +
               security + "</s:Header><s:Body>" +
               // A custom request's body is already the complete operation element.
               (op == OperationId::custom
                    ? std::string(body)
                    : '<' + std::string(operation.prefix) + ':' + std::string(operation.name) + '>' +
                          std::string(body) + "</" + std::string(operation.prefix) + ':' +
                          std::string(operation.name) + '>') +
               "</s:Body></s:Envelope>";
    }
    std::string soap_envelope(
        OperationId operation,
        std::string_view body,
        const CredentialLease* credentials,
        std::chrono::system_clock::duration clock_offset
    )
    {
        if (!credentials) return soap_envelope(operation, body, std::optional<Credentials>{});
        // Constructed in place and passed by reference: wrapping a named value in
        // a temporary optional would make a second copy that is never cleansed.
        std::optional<Credentials> materialized{
            std::in_place,
            std::string(credentials->username()), std::string(credentials->password()),
            credentials->allow_cleartext_basic()
        };
        const auto cleanse = [&]
        {
            OPENSSL_cleanse(materialized->username.data(), materialized->username.size());
            OPENSSL_cleanse(materialized->password.data(), materialized->password.size());
        };
        try
        {
            auto envelope = soap_envelope(operation, body, materialized, clock_offset);
            cleanse();
            return envelope;
        }
        catch (...)
        {
            cleanse();
            throw;
        }
    }
} // namespace wlonvif
