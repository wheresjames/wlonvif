/**
 * @file api_support.cpp
 * @brief Reply envelopes, request field readers, and small text helpers for the Explorer API.
 */

#include "api_support.hpp"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>

namespace wlonvif::examples::explorer
{
    Reply success(json::value data)
    {
        return {200, json::object{{"ok", true}, {"data", std::move(data)}}};
    }

    Reply failure(const Error& error)
    {
        return {200, json::object{{"ok", false}, {"error", error_json(error)}}};
    }

    Reply rejection(unsigned status, std::string message, std::string category)
    {
        json::object error{
            {"category", std::move(category)},
            {"operation", "Explorer API"},
            {"message", std::move(message)}
        };
        return {status, json::object{{"ok", false}, {"error", std::move(error)}}};
    }

    Error invalid(std::string message, OperationId operation)
    {
        return Error{ErrorCategory::invalid_argument, operation, std::move(message)};
    }

    // ---- Request fields --------------------------------------------------------

    std::optional<std::string> text_field(const json::object& object, std::string_view key)
    {
        const auto found = object.find(key);
        if (found == object.end() || !found->value().is_string())
        {
            return std::nullopt;
        }
        return std::string(found->value().as_string());
    }

    std::optional<double> number_field(const json::object& object, std::string_view key)
    {
        const auto found = object.find(key);
        if (found == object.end() || !found->value().is_number())
        {
            return std::nullopt;
        }
        const auto value = found->value().to_number<double>();
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    }

    std::optional<bool> flag_field(const json::object& object, std::string_view key)
    {
        const auto found = object.find(key);
        if (found == object.end() || !found->value().is_bool())
        {
            return std::nullopt;
        }
        return found->value().as_bool();
    }

    std::vector<std::string> split_path(std::string_view path)
    {
        std::vector<std::string> parts;
        while (!path.empty())
        {
            if (path.front() == '/')
            {
                path.remove_prefix(1);
                continue;
            }
            const auto end = path.find('/');
            parts.emplace_back(path.substr(0, end));
            if (end == std::string_view::npos)
            {
                break;
            }
            path.remove_prefix(end);
        }
        return parts;
    }

    // ---- Camera descriptions ---------------------------------------------------

    std::string evidence_name(DeviceEvidence evidence)
    {
        switch (evidence)
        {
            case DeviceEvidence::discovery:
                return "discovered";
            case DeviceEvidence::authentication_challenge:
                return "login-required";
            case DeviceEvidence::soap_fault:
                return "probable";
            case DeviceEvidence::onvif_response:
                return "confirmed";
        }
        return "unknown";
    }

    namespace
    {
        /// Decode `%41`-style escapes; malformed escapes are kept as written.
        std::string percent_decode(std::string_view text)
        {
            std::string output;
            for (std::size_t index = 0; index < text.size(); ++index)
            {
                const bool escape = text[index] == '%' && index + 2 < text.size() &&
                                    std::isxdigit(static_cast<unsigned char>(text[index + 1])) &&
                                    std::isxdigit(static_cast<unsigned char>(text[index + 2]));
                if (escape)
                {
                    const auto digits = std::string(text.substr(index + 1, 2));
                    output += static_cast<char>(std::stoi(digits, nullptr, 16));
                    index += 2;
                }
                else
                {
                    output += text[index];
                }
            }
            return output;
        }
    } // namespace

    std::string scope_value(const std::vector<std::string>& scopes, std::string_view category)
    {
        const std::string prefix = "onvif://www.onvif.org/" + std::string(category) + "/";
        for (const auto& scope : scopes)
        {
            if (scope.starts_with(prefix))
            {
                return percent_decode(std::string_view(scope).substr(prefix.size()));
            }
        }
        return {};
    }

    std::string service_support(std::string_view namespace_uri)
    {
        const auto has = [&](std::string_view text)
        {
            return namespace_uri.find(text) != std::string_view::npos;
        };
        if (has("ver20/media"))
        {
            return "Media2: not supported by wlonvif (Media1 is used)";
        }
        if (has("device/wsdl") || has("ver10/media") || has("ptz/wsdl") || has("events/wsdl") ||
            has("analytics/wsdl") || has("imaging/wsdl"))
        {
            return "supported";
        }
        if (has("recording") || has("replay") || has("search"))
        {
            return "Profile G recording: not supported by wlonvif";
        }
        return "not supported by wlonvif";
    }

    bool unsupported(const Error& error)
    {
        if (error.category == ErrorCategory::unsupported)
        {
            return true;
        }
        if (!error.soap_fault)
        {
            return false;
        }
        const auto& fault = *error.soap_fault;
        const auto& code = fault.subcode.empty() ? fault.code : fault.subcode;
        // ActionNotSupported is the usual answer; some devices use other *NotSupported
        // codes, or NoSuchService for a whole missing service.
        return code.find("NotSupported") != std::string::npos ||
               code.find("NoSuchService") != std::string::npos;
    }

    // ---- Recording files -------------------------------------------------------

    std::string safe_file_part(std::string text)
    {
        for (auto& c : text)
        {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            {
                c = '_';
            }
        }
        return text.empty() ? std::string("camera") : text;
    }

    std::string read_tail(const std::filesystem::path& path, std::size_t bytes)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return {};
        }
        file.seekg(0, std::ios::end);
        const auto size = static_cast<std::size_t>(file.tellg());
        file.seekg(static_cast<std::streamoff>(size > bytes ? size - bytes : 0));
        std::string content(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()
        );
        // FFmpeg logs echo the input URL, which carries the camera password.
        return redact_credentials(content);
    }

    bool recording_name_valid(std::string_view name)
    {
        return !name.empty() && name.size() < 200 && name.ends_with(".mkv") &&
               name.find_first_of("/\\") == std::string_view::npos && !name.starts_with('.');
    }
} // namespace wlonvif::examples::explorer
