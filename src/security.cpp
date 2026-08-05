#include <array>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <sstream>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    static std::string b64(const unsigned char* data, std::size_t size)
    {
        std::string out(4 * ((size + 2) / 3), '\0');
        auto n = EVP_EncodeBlock(
            reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(size)
        );
        out.resize(static_cast<std::size_t>(n));
        return out;
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
    static std::string escape(std::string_view s)
    {
        std::string o;
        for (char c : s)
        {
            switch (c)
            {
                case '&':
                    o += "&amp;";
                    break;
                case '<':
                    o += "&lt;";
                    break;
                case '>':
                    o += "&gt;";
                    break;
                case '\"':
                    o += "&quot;";
                    break;
                default:
                    o += c;
            }
        }
        return o;
    }
    std::string soap_envelope(
        OperationId op, std::string_view body, const std::optional<Credentials>& credentials
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
        }
        std::string security;
        if (credentials)
        {
            std::array<unsigned char, 16> nonce{};
            (void)RAND_bytes(nonce.data(), static_cast<int>(nonce.size()));
            const auto now = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()
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
                       escape(credentials->username) + "</wsse:Username>"
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
               "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
               "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
               "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-wssecurity-utility-1.0.xsd\" "
               "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
               "oasis-200401-wss-wssecurity-secext-1.0.xsd\"><s:Header>" +
               security + "</s:Header><s:Body><" + std::string(operation.prefix) + ':' +
               std::string(operation.name) + '>' + std::string(body) + "</" +
               std::string(operation.prefix) + ':' + std::string(operation.name) +
               "></s:Body></s:Envelope>";
    }
} // namespace wlonvif
