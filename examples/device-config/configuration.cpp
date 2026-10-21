/**
 * @file configuration.cpp
 * @brief Reading and printing the configuration that device-config shows and changes.
 */

#include "configuration.hpp"

#include <cctype>
#include <cstdlib>
#include <example_support.hpp>
#include <iostream>

namespace device_config
{
    std::string utc_text(std::chrono::system_clock::time_point value)
    {
        const auto seconds = std::chrono::floor<std::chrono::seconds>(value);
        const auto day = std::chrono::floor<std::chrono::days>(seconds);
        const std::chrono::year_month_day date{day};
        const std::chrono::hh_mm_ss time{seconds - day};
        const auto two = [](long long number)
        {
            return (number < 10 ? "0" : "") + std::to_string(number);
        };
        return std::to_string(static_cast<int>(date.year())) + '-' +
               two(static_cast<unsigned>(date.month())) + '-' +
               two(static_cast<unsigned>(date.day())) + 'T' + two(time.hours().count()) + ':' +
               two(time.minutes().count()) + ':' + two(time.seconds().count()) + 'Z';
    }

    std::string skew_text(std::chrono::seconds skew)
    {
        const auto magnitude = skew.count() < 0 ? -skew.count() : skew.count();
        if (magnitude <= 2)
        {
            return "in step with this computer";
        }
        std::string amount;
        if (magnitude >= 86400)
        {
            amount = std::to_string(magnitude / 86400) + "d ";
        }
        if (magnitude >= 3600)
        {
            amount += std::to_string(magnitude % 86400 / 3600) + "h ";
        }
        if (magnitude >= 60)
        {
            amount += std::to_string(magnitude % 3600 / 60) + "m ";
        }
        amount += std::to_string(magnitude % 60) + "s";
        return amount + (skew.count() > 0 ? " ahead of" : " behind") + " this computer";
    }

    std::string percent_encode(std::string_view text)
    {
        constexpr std::string_view hex = "0123456789ABCDEF";
        std::string output;
        for (const unsigned char c : text)
        {
            if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
            {
                output.push_back(static_cast<char>(c));
            }
            else
            {
                output += {'%', hex[c >> 4], hex[c & 15]};
            }
        }
        return output;
    }

    std::string percent_decode(std::string_view text)
    {
        std::string output;
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            if (text[index] == '%' && index + 2 < text.size() &&
                std::isxdigit(static_cast<unsigned char>(text[index + 1])) &&
                std::isxdigit(static_cast<unsigned char>(text[index + 2])))
            {
                output.push_back(
                    static_cast<char>(
                        std::strtol(std::string(text.substr(index + 1, 2)).c_str(), nullptr, 16)
                    )
                );
                index += 2;
            }
            else
            {
                output.push_back(text[index]);
            }
        }
        return output;
    }

    std::string refusal_text(const wlonvif::Error& error)
    {
        std::string text = "[" +
                           std::string(wlonvif::examples::error_category_name(error.category)) +
                           "] " + error.message;
        if (!error.soap_fault)
        {
            return text;
        }
        const auto& subcode = error.soap_fault->subcode;
        text += " (" + (subcode.empty() ? error.soap_fault->code : subcode) + ")";
        if (subcode.ends_with("ScopeOverwrite") || subcode.ends_with("FixedScope"))
        {
            text += ": fixed scopes are built into the device and cannot be changed";
        }
        else if (subcode.ends_with("NoScope"))
        {
            text += ": the device has no such scope";
        }
        else if (subcode.ends_with("ActionNotSupported"))
        {
            text += ": the device does not support this operation";
        }
        else if (subcode.ends_with("InvalidTimeZone"))
        {
            text += ": the device rejected the time zone";
        }
        return text;
    }

    wlonvif::Result<Configuration>
    read_configuration(const std::shared_ptr<wlonvif::DeviceSession>& device, int timeout)
    {
        using wlonvif::examples::call_options;
        using wlonvif::examples::wait_for_task;
        Configuration configuration;
        auto clock = wait_for_task(device->get_system_date_and_time(call_options(timeout)));
        if (!clock)
        {
            return clock.error();
        }
        configuration.clock = std::move(clock.value());
        configuration.skew = std::chrono::round<std::chrono::seconds>(
            configuration.clock.utc - std::chrono::system_clock::now()
        );
        // NTP is optional in ONVIF; its absence should not hide the rest.
        auto ntp = wait_for_task(device->get_ntp(call_options(timeout)));
        if (ntp)
        {
            configuration.ntp = std::move(ntp.value());
        }
        auto scopes = wait_for_task(device->get_scope_entries(call_options(timeout)));
        if (!scopes)
        {
            return scopes.error();
        }
        configuration.scopes = std::move(scopes.value());
        if (auto value = wait_for_task(device->get_hostname(call_options(timeout))))
        {
            configuration.hostname = std::move(value.value());
        }
        if (auto value = wait_for_task(device->get_network_interfaces(call_options(timeout))))
        {
            configuration.interfaces = std::move(value.value());
        }
        if (auto value = wait_for_task(device->get_network_default_gateway(call_options(timeout))))
        {
            configuration.gateways = std::move(value.value());
        }
        if (auto value = wait_for_task(device->get_dns(call_options(timeout))))
        {
            configuration.dns = std::move(value.value());
        }
        if (auto value = wait_for_task(device->get_network_protocols(call_options(timeout))))
        {
            configuration.protocols = std::move(value.value());
        }
        if (auto value = wait_for_task(device->get_discovery_mode(call_options(timeout))))
        {
            configuration.discovery = value.value();
        }
        if (auto value = wait_for_task(device->get_users(call_options(timeout))))
        {
            configuration.users = std::move(value.value());
        }
        return configuration;
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

    void print_configuration(const Configuration& configuration, bool json)
    {
        using wlonvif::examples::json_escape;
        const auto& clock = configuration.clock;
        const bool ntp_mode = clock.type == wlonvif::DateTimeType::ntp;
        if (json)
        {
            std::cout << "{\"clock\":{\"mode\":\"" << (ntp_mode ? "ntp" : "manual")
                      << "\",\"utc\":\"" << utc_text(clock.utc)
                      << "\",\"skew_seconds\":" << configuration.skew.count() << ",\"time_zone\":\""
                      << json_escape(clock.time_zone)
                      << "\",\"daylight_savings\":" << (clock.daylight_savings ? "true" : "false")
                      << '}';
            if (configuration.ntp)
            {
                const auto list = [](const std::vector<std::string>& values)
                {
                    std::string output = "[";
                    for (std::size_t i = 0; i < values.size(); ++i)
                    {
                        output += (i ? ",\"" : "\"") + json_escape(values[i]) + '"';
                    }
                    return output + ']';
                };
                std::cout << ",\"ntp\":{\"from_dhcp\":"
                          << (configuration.ntp->from_dhcp ? "true" : "false")
                          << ",\"servers\":" << list(configuration.ntp->servers)
                          << ",\"dhcp_servers\":" << list(configuration.ntp->dhcp_servers) << '}';
            }
            std::cout << ",\"scopes\":[";
            for (std::size_t i = 0; i < configuration.scopes.size(); ++i)
            {
                const auto& scope = configuration.scopes[i];
                std::cout << (i ? "," : "") << "{\"uri\":\"" << json_escape(scope.uri)
                          << "\",\"definition\":\""
                          << (scope.definition == wlonvif::ScopeDefinition::fixed ? "fixed"
                                                                                  : "configurable")
                          << "\"}";
            }
            std::cout << ']';
            if (configuration.hostname)
            {
                std::cout << ",\"hostname\":\"" << json_escape(configuration.hostname->name) << '"';
            }
            if (configuration.discovery)
            {
                std::cout << ",\"discoverable\":"
                          << (*configuration.discovery == wlonvif::DiscoveryMode::discoverable
                                  ? "true"
                                  : "false");
            }
            std::cout << ",\"interfaces\":[";
            for (std::size_t i = 0; i < configuration.interfaces.size(); ++i)
            {
                const auto& item = configuration.interfaces[i];
                std::cout << (i ? "," : "") << "{\"name\":\"" << json_escape(item.name)
                          << "\",\"mac\":\"" << json_escape(item.hardware_address)
                          << "\",\"dhcp\":" << (item.ipv4_dhcp ? "true" : "false") << ",\"ipv4\":[";
                for (std::size_t j = 0; j < item.ipv4_addresses.size(); ++j)
                {
                    std::cout << (j ? ",\"" : "\"") << json_escape(item.ipv4_addresses[j]) << '"';
                }
                std::cout << "]}";
            }
            std::cout << "],\"users\":[";
            for (std::size_t i = 0; i < configuration.users.size(); ++i)
            {
                std::cout << (i ? "," : "") << "{\"username\":\""
                          << json_escape(configuration.users[i].username) << "\",\"level\":\""
                          << json_escape(configuration.users[i].level) << "\"}";
            }
            std::cout << "]}\n";
            return;
        }
        const auto row = [](std::string_view name, const std::string& value)
        {
            std::cout << "  " << name;
            for (auto pad = name.size(); pad < 14; ++pad)
            {
                std::cout << ' ';
            }
            std::cout << value << '\n';
        };
        std::cout << "Clock\n";
        row("Mode", ntp_mode ? "NTP" : "Manual");
        row("UTC", utc_text(clock.utc) + "  (" + skew_text(configuration.skew) + ")");
        row("Time zone", clock.time_zone.empty() ? "(not reported)" : clock.time_zone);
        row("Daylight", clock.daylight_savings ? "on" : "off");
        if (configuration.ntp)
        {
            std::cout << "\nNTP\n";
            row("Source", configuration.ntp->from_dhcp ? "DHCP" : "Manual");
            if (!configuration.ntp->servers.empty())
            {
                row("Servers", joined(configuration.ntp->servers));
            }
            if (!configuration.ntp->dhcp_servers.empty())
            {
                row("From DHCP", joined(configuration.ntp->dhcp_servers));
            }
        }
        std::cout << "\nNetwork\n";
        if (configuration.hostname)
        {
            row("Hostname",
                configuration.hostname->name +
                    (configuration.hostname->from_dhcp ? "  (DHCP)" : ""));
        }
        for (const auto& item : configuration.interfaces)
        {
            row("Interface",
                item.name + "  " + item.hardware_address + "  MTU " + std::to_string(item.mtu) +
                    (item.enabled ? "" : "  (disabled)"));
            row("  IPv4",
                (item.ipv4_addresses.empty() ? std::string("(none)")
                                             : joined(item.ipv4_addresses)) +
                    (item.ipv4_dhcp ? "  (DHCP)" : ""));
        }
        if (!configuration.gateways.empty())
        {
            row("Gateway", joined(configuration.gateways));
        }
        if (configuration.dns)
        {
            const auto& dns = *configuration.dns;
            const auto& servers = dns.from_dhcp ? dns.dhcp_servers : dns.servers;
            row("DNS",
                (servers.empty() ? std::string("(none)") : joined(servers)) +
                    (dns.from_dhcp ? "  (DHCP)" : ""));
            if (!dns.search_domains.empty())
            {
                row("  Search", joined(dns.search_domains));
            }
        }
        for (const auto& protocol : configuration.protocols)
        {
            std::string ports;
            for (const auto port : protocol.ports)
            {
                ports += (ports.empty() ? "" : ", ") + std::to_string(port);
            }
            row(protocol.name, (protocol.enabled ? "port " : "disabled, port ") + ports);
        }
        if (configuration.discovery)
        {
            row("Discoverable",
                *configuration.discovery == wlonvif::DiscoveryMode::discoverable ? "yes" : "no");
        }
        if (!configuration.users.empty())
        {
            std::cout << "\nAccounts\n";
            for (const auto& user : configuration.users)
            {
                row(user.level, user.username);
            }
        }
        std::cout << "\nScopes\n";
        for (const auto& scope : configuration.scopes)
        {
            std::string value = scope.uri;
            if (scope.uri.starts_with(LocationPrefix))
            {
                value +=
                    "  (location: " + percent_decode(scope.uri.substr(LocationPrefix.size())) + ")";
            }
            row(scope.definition == wlonvif::ScopeDefinition::fixed ? "fixed" : "configurable",
                value);
        }
    }
} // namespace device_config
