#pragma once

/**
 * @file api_support.hpp
 * @brief Building blocks shared by the Explorer API handlers.
 *
 * Every API answer is a JSON envelope: `{"ok":true,"data":...}` on success, or
 * `{"ok":false,"error":{...}}` on failure. A *view* (such as the PTZ tab's data)
 * gathers several camera queries; each one becomes a *section* that holds
 * either its value or its own error, so one failing query does not hide the
 * others.
 */

#include "encode.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wlonvif::examples::explorer
{
    /// An API answer: HTTP status plus the JSON envelope.
    struct Reply
    {
        unsigned status{200};
        json::value body;
    };

    /// A successful answer carrying @p data.
    Reply success(json::value data = nullptr);

    /// A device or library failure. Sent with HTTP 200, so the page shows the details.
    Reply failure(const Error& error);

    /// A request the API itself refuses, such as a missing field or an unknown path.
    Reply
    rejection(unsigned status, std::string message, std::string category = "invalid-argument");

    /// An invalid-argument Error, for checks the explorer makes before calling the camera.
    Error invalid(std::string message, OperationId operation = OperationId::custom);

    /// One query inside a larger view: `{"ok":true,"value":...}` or its error.
    template <class T, class Encode> json::object section(const Result<T>& result, Encode encode)
    {
        if (result)
        {
            return json::object{{"ok", true}, {"value", encode(result.value())}};
        }
        return json::object{{"ok", false}, {"error", error_json(result.error())}};
    }

    /// A section encoded with the matching to_json() overload.
    template <class T> json::object section(const Result<T>& result)
    {
        return section(
            result,
            [](const T& value)
            {
                return to_json(value);
            }
        );
    }

    // ---- Request fields --------------------------------------------------------
    //
    // Each returns nothing when the field is missing or has the wrong JSON type,
    // so handlers can apply their own defaults.

    std::optional<std::string> text_field(const json::object& object, std::string_view key);
    /// A finite number; NaN and infinity count as missing.
    std::optional<double> number_field(const json::object& object, std::string_view key);
    std::optional<bool> flag_field(const json::object& object, std::string_view key);

    /// Split `/api/cameras/camera-1/ptz` into `api`, `cameras`, `camera-1`, `ptz`.
    std::vector<std::string> split_path(std::string_view path);

    // ---- Camera descriptions ---------------------------------------------------

    /// How sure a scan is that a device speaks ONVIF, as the page labels it.
    std::string evidence_name(DeviceEvidence evidence);

    /// The value of the first `onvif://www.onvif.org/<category>/` scope, percent-decoded.
    std::string scope_value(const std::vector<std::string>& scopes, std::string_view category);

    /// Whether wlonvif implements the ONVIF service with this WSDL namespace.
    std::string service_support(std::string_view namespace_uri);

    /// Whether @p error means "the device does not offer this", rather than "it broke".
    bool unsupported(const Error& error);

    // ---- Recording files -------------------------------------------------------

    /// @p text with everything but letters, digits, `-` and `_` replaced, for file names.
    std::string safe_file_part(std::string text);

    /// The last @p bytes of a file, with credentials in URLs redacted.
    std::string read_tail(const std::filesystem::path& path, std::size_t bytes);

    /// A recording name the API accepts: a plain `.mkv` file name, never a path.
    bool recording_name_valid(std::string_view name);
} // namespace wlonvif::examples::explorer
