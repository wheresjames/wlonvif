/**
 * @file main.cpp
 * @brief wlonvif-imaging: show and change a device's imaging settings (brightness, contrast, ...).
 *
 * Imaging settings belong to a video source, which Media lists. Before a
 * change is sent it is checked against the ranges and modes the camera reports
 * (GetOptions), so an out-of-range value is explained here rather than
 * refused by the camera with a generic fault. Afterwards the settings are read
 * back and printed, which shows whether the camera kept them.
 */

#include <algorithm>
#include <charconv>
#include <cmath>
#include <example_support.hpp>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable
            << " (--url URL | --ip ADDRESS) [changes] [common options]\n\n"
            << "With no changes, show each video source's settings and accepted ranges.\n\n"
            << "Changes:\n"
            << "  --source TOKEN         Video source to change (default: the first)\n"
            << "  --brightness N         Set brightness\n"
            << "  --contrast N           Set contrast\n"
            << "  --saturation N         Set colour saturation\n"
            << "  --sharpness N          Set sharpness\n"
            << "  --ir-cut MODE          IR cut filter: ON, OFF, or AUTO\n"
            << "  --backlight MODE       Backlight compensation: ON or OFF\n"
            << "  --wdr MODE             Wide dynamic range: ON or OFF\n"
            << "  --white-balance MODE   AUTO or MANUAL\n"
            << "  --no-persist           Do not keep the change across a reboot\n\n"
            << "Values are checked against the ranges the device reports.\n\n"
            << "Common options:\n"
            << "  --timeout SECONDS --username NAME --password PASSWORD --password-file FILE\n"
            << "  --password-env NAME --allow-cleartext-basic --json --help\n";
    }

    std::string refusal_text(const wlonvif::Error& error)
    {
        std::string text = "[" +
                           std::string(wlonvif::examples::error_category_name(error.category)) +
                           "] " + error.message;
        if (error.soap_fault)
        {
            const auto& fault = *error.soap_fault;
            text += " (" + (fault.subcode.empty() ? fault.code : fault.subcode) + ")";
            if (!fault.reason.empty())
            {
                text += ": " + fault.reason;
            }
        }
        return text;
    }

    std::string number_text(double value)
    {
        std::ostringstream output;
        output << value;
        return output.str();
    }

    std::optional<double> parse_number(std::string_view text)
    {
        double value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            !std::isfinite(value))
        {
            return std::nullopt;
        }
        return value;
    }

    std::string joined(const std::vector<std::string>& values)
    {
        std::string output;
        for (const auto& value : values)
        {
            output += (output.empty() ? "" : ", ") + value;
        }
        return output;
    }

    void print_source(
        const wlonvif::VideoSource& source,
        const wlonvif::ImagingSettings& settings,
        const wlonvif::ImagingOptions& options
    )
    {
        std::cout << "Video source " << source.token << "  (" << source.width << 'x'
                  << source.height << " @ " << number_text(source.framerate) << " fps)\n";
        const auto row =
            [](std::string_view name, const std::string& value, const std::string& range)
        {
            std::cout << "  " << name;
            for (auto pad = name.size(); pad < 24; ++pad)
            {
                std::cout << ' ';
            }
            std::cout << value;
            if (!range.empty())
            {
                for (auto pad = value.size(); pad < 10; ++pad)
                {
                    std::cout << ' ';
                }
                std::cout << '(' << range << ')';
            }
            std::cout << '\n';
        };
        const auto numeric = [&](std::string_view name,
                                 const std::optional<double>& value,
                                 const std::optional<wlonvif::ImagingRange>& range)
        {
            if (!value && !range)
            {
                return;
            }
            row(name,
                value ? number_text(*value) : "-",
                range ? number_text(range->minimum) + " to " + number_text(range->maximum) : "");
        };
        const auto mode = [&](std::string_view name,
                              const std::optional<std::string>& value,
                              const std::vector<std::string>& modes)
        {
            if (!value && modes.empty())
            {
                return;
            }
            row(name, value.value_or("-"), joined(modes));
        };
        numeric("Brightness", settings.brightness, options.brightness);
        numeric("Contrast", settings.contrast, options.contrast);
        numeric("Colour saturation", settings.color_saturation, options.color_saturation);
        numeric("Sharpness", settings.sharpness, options.sharpness);
        mode("IR cut filter", settings.ir_cut_filter, options.ir_cut_filter_modes);
        mode(
            "Backlight compensation",
            settings.backlight_compensation,
            options.backlight_compensation_modes
        );
        mode("Wide dynamic range", settings.wide_dynamic_range, options.wide_dynamic_range_modes);
        mode("White balance", settings.white_balance, options.white_balance_modes);
    }

    std::string
    json_source(const wlonvif::VideoSource& source, const wlonvif::ImagingSettings& settings)
    {
        using wlonvif::examples::json_escape;
        std::string output = "{\"source\":\"" + json_escape(source.token) + '"';
        const auto number = [&](std::string_view name, const std::optional<double>& value)
        {
            if (value)
            {
                output += ",\"" + std::string(name) + "\":" + number_text(*value);
            }
        };
        const auto text = [&](std::string_view name, const std::optional<std::string>& value)
        {
            if (value)
            {
                output += ",\"" + std::string(name) + "\":\"" + json_escape(*value) + '"';
            }
        };
        number("brightness", settings.brightness);
        number("contrast", settings.contrast);
        number("color_saturation", settings.color_saturation);
        number("sharpness", settings.sharpness);
        text("ir_cut_filter", settings.ir_cut_filter);
        text("backlight_compensation", settings.backlight_compensation);
        text("wide_dynamic_range", settings.wide_dynamic_range);
        text("white_balance", settings.white_balance);
        return output + '}';
    }

    /// Explain why @p settings cannot be applied with @p options, if it cannot.
    std::optional<std::string>
    check(const wlonvif::ImagingSettings& settings, const wlonvif::ImagingOptions& options)
    {
        const auto numeric =
            [](std::string_view name,
               const std::optional<double>& value,
               const std::optional<wlonvif::ImagingRange>& range) -> std::optional<std::string>
        {
            if (!value)
            {
                return std::nullopt;
            }
            if (!range)
            {
                return std::string(name) + " is not adjustable on this device";
            }
            if (*value < range->minimum || *value > range->maximum)
            {
                return std::string(name) + " " + number_text(*value) + " is outside " +
                       number_text(range->minimum) + " to " + number_text(range->maximum);
            }
            return std::nullopt;
        };
        const auto mode = [](std::string_view name,
                             const std::optional<std::string>& value,
                             const std::vector<std::string>& modes) -> std::optional<std::string>
        {
            if (!value)
            {
                return std::nullopt;
            }
            if (modes.empty())
            {
                return std::string(name) + " is not adjustable on this device";
            }
            if (std::find(modes.begin(), modes.end(), *value) == modes.end())
            {
                return std::string(name) + " " + *value + " is not one of " + joined(modes);
            }
            return std::nullopt;
        };
        for (const auto& problem :
             {numeric("brightness", settings.brightness, options.brightness),
              numeric("contrast", settings.contrast, options.contrast),
              numeric("saturation", settings.color_saturation, options.color_saturation),
              numeric("sharpness", settings.sharpness, options.sharpness),
              mode("IR cut filter", settings.ir_cut_filter, options.ir_cut_filter_modes),
              mode(
                  "backlight compensation",
                  settings.backlight_compensation,
                  options.backlight_compensation_modes
              ),
              mode(
                  "wide dynamic range",
                  settings.wide_dynamic_range,
                  options.wide_dynamic_range_modes
              ),
              mode("white balance", settings.white_balance, options.white_balance_modes)})
        {
            if (problem)
            {
                return problem;
            }
        }
        return std::nullopt;
    }
} // namespace

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    const auto& arguments = parsed.value().remaining;
    std::string url, address, selected;
    wlonvif::ImagingSettings change;
    bool changing = false, persist = true;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument(arguments[index]);
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--no-persist")
        {
            persist = false;
            continue;
        }
        if (index + 1 >= arguments.size())
        {
            std::cerr << (argument.starts_with("--") ? "Missing value for " : "Unknown argument: ")
                      << argument << '\n';
            return 2;
        }
        const std::string value = arguments[++index];
        std::optional<double>* number = argument == "--brightness"   ? &change.brightness
                                        : argument == "--contrast"   ? &change.contrast
                                        : argument == "--saturation" ? &change.color_saturation
                                        : argument == "--sharpness"  ? &change.sharpness
                                                                     : nullptr;
        std::optional<std::string>* mode = argument == "--ir-cut" ? &change.ir_cut_filter
                                           : argument == "--backlight"
                                               ? &change.backlight_compensation
                                           : argument == "--wdr" ? &change.wide_dynamic_range
                                           : argument == "--white-balance" ? &change.white_balance
                                                                           : nullptr;
        if (argument == "--url")
        {
            url = value;
        }
        else if (argument == "--ip")
        {
            address = value;
        }
        else if (argument == "--source")
        {
            selected = value;
        }
        else if (number)
        {
            *number = parse_number(value);
            if (!*number)
            {
                std::cerr << "Invalid number for " << argument << ": " << value << '\n';
                return 2;
            }
            changing = true;
        }
        else if (mode)
        {
            std::string upper = value;
            std::transform(
                upper.begin(),
                upper.end(),
                upper.begin(),
                [](unsigned char c)
                {
                    return static_cast<char>(std::toupper(c));
                }
            );
            *mode = upper;
            changing = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            usage(argv[0]);
            return 2;
        }
    }
    if (url.empty() == address.empty())
    {
        std::cerr << "Specify exactly one of --url or --ip\n";
        return 2;
    }

    auto credentials = wlonvif::examples::load_credentials(common);
    if (!credentials)
    {
        std::cerr << credentials.error().message << '\n';
        return 2;
    }
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result)
    {
        return 1;
    }
    auto runtime = runtime_result.value();
    const auto finish = [&](int status)
    {
        (void)runtime->shutdown();
        return status;
    };
    auto endpoint =
        wlonvif::examples::resolve_device_endpoint(url, address, runtime, common.timeout_seconds);
    if (!endpoint)
    {
        std::cerr << "Endpoint discovery failed: " << endpoint.error().message << '\n';
        return finish(1);
    }
    auto connected = wlonvif::examples::connect_device(
        endpoint.value().url, runtime, std::move(credentials.value()), common.timeout_seconds
    );
    if (!connected)
    {
        std::cerr << "Connection failed: " << refusal_text(connected.error()) << '\n';
        return finish(1);
    }
    using wlonvif::examples::call_options;
    using wlonvif::examples::wait_for_task;
    const auto timeout = common.timeout_seconds;
    auto device = connected.value();
    auto media = device->media();
    auto imaging = device->imaging();
    if (!media || !imaging)
    {
        std::cerr << "Session closed\n";
        return finish(1);
    }
    auto sources = wait_for_task(media.value()->get_video_sources(call_options(timeout)));
    if (!sources || sources.value().empty())
    {
        std::cerr << "Video source query failed: "
                  << (sources ? std::string("device reported none") : refusal_text(sources.error()))
                  << '\n';
        return finish(1);
    }

    // ---- Change one source (the first, unless --source picks one) -------------------------
    int status = 0;
    if (changing)
    {
        const auto source = selected.empty() ? sources.value().front().token : selected;
        auto options = wait_for_task(imaging.value()->get_options(source, call_options(timeout)));
        if (!options)
        {
            std::cerr << "Imaging options query failed: " << refusal_text(options.error()) << '\n';
            return finish(1);
        }
        if (const auto problem = check(change, options.value()))
        {
            std::cerr << "Not applied: " << *problem << '\n';
            return finish(2);
        }
        auto applied = wait_for_task(
            imaging.value()->set_settings(source, change, persist, call_options(timeout))
        );
        if (!applied)
        {
            std::cerr << "  failed  imaging settings: " << refusal_text(applied.error()) << '\n';
            status = 1;
        }
        else if (!common.json)
        {
            std::cout << "  ok      imaging settings on " << source << "\n\n";
        }
    }

    // ---- Show every source, after any change ---------------------------------------------
    if (common.json)
    {
        std::cout << '[';
    }
    for (std::size_t index = 0; index < sources.value().size(); ++index)
    {
        const auto& source = sources.value()[index];
        auto settings =
            wait_for_task(imaging.value()->get_settings(source.token, call_options(timeout)));
        auto options =
            wait_for_task(imaging.value()->get_options(source.token, call_options(timeout)));
        if (!settings || !options)
        {
            std::cerr << "Imaging query for " << source.token
                      << " failed: " << refusal_text(!settings ? settings.error() : options.error())
                      << '\n';
            status = 1;
            continue;
        }
        if (common.json)
        {
            std::cout << (index ? "," : "") << json_source(source, settings.value());
            continue;
        }
        if (index)
        {
            std::cout << '\n';
        }
        print_source(source, settings.value(), options.value());
    }
    if (common.json)
    {
        std::cout << "]\n";
    }
    return finish(status);
}
