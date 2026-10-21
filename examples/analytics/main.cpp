/**
 * @file main.cpp
 * @brief wlonvif-analytics: show and change a device's video analytics modules and rules.
 *
 * Analytics *modules* are the detectors (cell motion, tamper, ...) and *rules*
 * decide when they raise an event. Each has simple items (name = text value,
 * such as Sensitivity = 60), which this example can change, and structured
 * items (XML, such as a cell grid), which it shows and sends back unchanged.
 * Configurations are found through the media profiles that use them.
 */

#include <algorithm>
#include <example_support.hpp>
#include <iostream>
#include <map>
#include <optional>
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
            << "With no changes, show every analytics configuration's modules and rules.\n\n"
            << "Changes:\n"
            << "  --configuration TOKEN  Analytics configuration to change (default: the\n"
            << "                         first profile's)\n"
            << "  --module NAME          Select a module for the following --set options\n"
            << "  --rule NAME            Select a rule for the following --set options\n"
            << "  --set NAME=VALUE       Change a parameter of the selected module or rule\n\n"
            << "Example: --module MyCellMotionModule --set Sensitivity=80\n\n"
            << "Common options:\n"
            << "  --timeout SECONDS --username NAME --password PASSWORD --password-file FILE\n"
            << "  --password-env NAME --allow-cleartext-basic --json --help\n";
    }

    /// Parameter changes for one named module or rule.
    struct Change
    {
        bool rule{};
        std::string name;
        std::vector<wlonvif::SimpleItem> values;
    };

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

    void
    print_configs(std::string_view heading, const std::vector<wlonvif::AnalyticsConfig>& configs)
    {
        std::cout << "  " << heading << '\n';
        if (configs.empty())
        {
            std::cout << "    (none)\n";
        }
        for (const auto& config : configs)
        {
            std::cout << "    " << config.name << "  (" << config.type << ")\n";
            for (const auto& item : config.simple_items)
            {
                std::cout << "        " << item.name;
                for (auto pad = item.name.size(); pad < 16; ++pad)
                {
                    std::cout << ' ';
                }
                std::cout << item.value << '\n';
            }
            for (const auto& item : config.element_items)
            {
                std::cout << "        " << item.name;
                for (auto pad = item.name.size(); pad < 16; ++pad)
                {
                    std::cout << ' ';
                }
                std::cout << "(structured)\n";
            }
        }
    }

    std::string json_configs(const std::vector<wlonvif::AnalyticsConfig>& configs)
    {
        using wlonvif::examples::json_escape;
        std::string output = "[";
        for (std::size_t i = 0; i < configs.size(); ++i)
        {
            const auto& config = configs[i];
            output += std::string(i ? "," : "") + "{\"name\":\"" + json_escape(config.name) +
                      "\",\"type\":\"" + json_escape(config.type) + "\",\"parameters\":{";
            for (std::size_t j = 0; j < config.simple_items.size(); ++j)
            {
                output += std::string(j ? "," : "") + '"' +
                          json_escape(config.simple_items[j].name) + "\":\"" +
                          json_escape(config.simple_items[j].value) + '"';
            }
            output += "},\"structured\":[";
            for (std::size_t j = 0; j < config.element_items.size(); ++j)
            {
                output += std::string(j ? "," : "") + '"' +
                          json_escape(config.element_items[j].name) + '"';
            }
            output += "]}";
        }
        return output + ']';
    }

    /// Apply @p change to the matching entry of @p configs, reporting unknown names.
    std::optional<std::string>
    apply_change(std::vector<wlonvif::AnalyticsConfig>& configs, const Change& change)
    {
        const auto config = std::find_if(
            configs.begin(),
            configs.end(),
            [&](const auto& item)
            {
                return item.name == change.name;
            }
        );
        const std::string kind = change.rule ? "rule" : "module";
        if (config == configs.end())
        {
            std::string known;
            for (const auto& item : configs)
            {
                known += (known.empty() ? "" : ", ") + item.name;
            }
            return "no " + kind + " named " + change.name + " (available: " + known + ")";
        }
        for (const auto& value : change.values)
        {
            const auto item = std::find_if(
                config->simple_items.begin(),
                config->simple_items.end(),
                [&](const auto& entry)
                {
                    return entry.name == value.name;
                }
            );
            if (item == config->simple_items.end())
            {
                std::string known;
                for (const auto& entry : config->simple_items)
                {
                    known += (known.empty() ? "" : ", ") + entry.name;
                }
                return kind + " " + change.name + " has no parameter " + value.name +
                       " (available: " + known + ")";
            }
            item->value = value.value;
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
    std::string url, address, configuration;
    std::vector<Change> changes;
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::string_view argument(arguments[index]);
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        const bool needs_value = argument == "--url" || argument == "--ip" ||
                                 argument == "--configuration" || argument == "--module" ||
                                 argument == "--rule" || argument == "--set";
        if (needs_value && index + 1 >= arguments.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url")
        {
            url = arguments[++index];
        }
        else if (argument == "--ip")
        {
            address = arguments[++index];
        }
        else if (argument == "--configuration")
        {
            configuration = arguments[++index];
        }
        else if (argument == "--module" || argument == "--rule")
        {
            changes.push_back({argument == "--rule", arguments[++index], {}});
        }
        else if (argument == "--set")
        {
            const std::string value = arguments[++index];
            const auto equals = value.find('=');
            if (changes.empty() || equals == std::string::npos || equals == 0)
            {
                std::cerr << "--set NAME=VALUE must follow --module or --rule\n";
                return 2;
            }
            changes.back().values.push_back({value.substr(0, equals), value.substr(equals + 1)});
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
    for (const auto& change : changes)
    {
        if (change.values.empty())
        {
            std::cerr << (change.rule ? "--rule " : "--module ") << change.name
                      << " needs at least one --set NAME=VALUE\n";
            return 2;
        }
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
    auto analytics = device->analytics();
    if (!media || !analytics)
    {
        std::cerr << "Session closed\n";
        return finish(1);
    }
    auto profiles = wait_for_task(media.value()->get_profiles(call_options(timeout)));
    if (!profiles)
    {
        std::cerr << "Profile query failed: " << refusal_text(profiles.error()) << '\n';
        return finish(1);
    }
    // Profiles commonly share one analytics configuration; show each once.
    std::vector<std::string> configurations;
    std::map<std::string, std::string> users;
    for (const auto& profile : profiles.value())
    {
        const auto& token = profile.video_analytics_configuration;
        if (token.empty())
        {
            continue;
        }
        if (std::find(configurations.begin(), configurations.end(), token) == configurations.end())
        {
            configurations.push_back(token);
        }
        auto& names = users[token];
        names +=
            (names.empty() ? "" : ", ") + (profile.name.empty() ? profile.token : profile.name);
    }
    if (configurations.empty())
    {
        std::cerr << "No profile has a video analytics configuration\n";
        return finish(1);
    }

    int status = 0;
    // ---- Changes: read the current entries, edit them, send back only the edited ones ----
    if (!changes.empty())
    {
        const auto target = configuration.empty() ? configurations.front() : configuration;
        auto modules = wait_for_task(analytics.value()->get_modules(target, call_options(timeout)));
        auto rules = wait_for_task(analytics.value()->get_rules(target, call_options(timeout)));
        if (!modules || !rules)
        {
            std::cerr << "Analytics query failed: "
                      << refusal_text(!modules ? modules.error() : rules.error()) << '\n';
            return finish(1);
        }
        if (!common.json)
        {
            std::cout << "Applying changes to " << target << '\n';
        }
        for (const auto& change : changes)
        {
            auto& configs = change.rule ? rules.value() : modules.value();
            std::string description = (change.rule ? "rule " : "module ") + change.name;
            for (const auto& value : change.values)
            {
                description += ' ' + value.name + '=' + value.value;
            }
            if (const auto problem = apply_change(configs, change))
            {
                std::cerr << "  failed  " << description << ": " << *problem << '\n';
                status = 1;
                continue;
            }
            // Only the changed entry is sent, so other modules or rules are untouched.
            const auto selected = *std::find_if(
                configs.begin(),
                configs.end(),
                [&](const auto& item)
                {
                    return item.name == change.name;
                }
            );
            auto applied = change.rule ? wait_for_task(analytics.value()->modify_rules(
                                             target, {selected}, call_options(timeout)
                                         ))
                                       : wait_for_task(analytics.value()->modify_modules(
                                             target, {selected}, call_options(timeout)
                                         ));
            if (applied)
            {
                if (!common.json)
                {
                    std::cout << "  ok      " << description << '\n';
                }
            }
            else
            {
                std::cerr << "  failed  " << description << ": " << refusal_text(applied.error())
                          << '\n';
                status = 1;
            }
        }
        if (!common.json)
        {
            std::cout << '\n';
        }
    }

    if (common.json)
    {
        std::cout << '[';
    }
    for (std::size_t index = 0; index < configurations.size(); ++index)
    {
        const auto& token = configurations[index];
        auto modules = wait_for_task(analytics.value()->get_modules(token, call_options(timeout)));
        auto rules = wait_for_task(analytics.value()->get_rules(token, call_options(timeout)));
        if (!modules || !rules)
        {
            std::cerr << "Analytics query for " << token
                      << " failed: " << refusal_text(!modules ? modules.error() : rules.error())
                      << '\n';
            status = 1;
            continue;
        }
        if (common.json)
        {
            std::cout << (index ? "," : "") << "{\"configuration\":\""
                      << wlonvif::examples::json_escape(token)
                      << "\",\"modules\":" << json_configs(modules.value())
                      << ",\"rules\":" << json_configs(rules.value()) << '}';
            continue;
        }
        std::cout << (index ? "\n" : "") << "Analytics configuration " << token
                  << "  (profiles: " << users[token] << ")\n";
        print_configs("Modules", modules.value());
        print_configs("Rules", rules.value());
    }
    if (common.json)
    {
        std::cout << "]\n";
    }
    return finish(status);
}
