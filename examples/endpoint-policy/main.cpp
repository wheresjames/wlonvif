/**
 * @file main.cpp
 * @brief wlonvif-endpoint-policy: show which service addresses a device may send you to.
 *
 * A device tells clients where its services are (GetServices, GetCapabilities).
 * A compromised or misconfigured device could name another host, so the
 * library checks every advertised address against an endpoint policy before
 * sending credentials there. This example runs the policy offline:
 *
 *     wlonvif-endpoint-policy --origin http://192.0.2.10/onvif/device_service \
 *         --candidate http://192.0.2.10:8080/onvif/media --candidate http://198.51.100.5/
 *
 * The default (same host) policy allows another port on the same host;
 * --strict (same origin) also requires the same scheme and port. Both reject
 * URLs carrying user information. The exit status is 1 if any is rejected.
 */

#include <example_support.hpp>
#include <iostream>

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    std::string origin;
    std::vector<std::string> candidates;
    bool strict = false;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            std::cout << "Usage: " << argv[0]
                      << " --origin URL [--strict] --candidate URL [--candidate URL...]\n";
            return 0;
        }
        if ((argument == "--origin" || argument == "--candidate") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--origin")
        {
            origin = parsed.value().remaining[++index];
        }
        else if (argument == "--candidate")
        {
            candidates.push_back(parsed.value().remaining[++index]);
        }
        else if (argument == "--strict")
        {
            strict = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    if (origin.empty() || candidates.empty())
    {
        std::cerr << "--origin and at least one --candidate are required\n";
        return 2;
    }
    // The same policies DeviceSession::connect() takes.
    const auto policy =
        strict ? wlonvif::make_same_origin_policy() : wlonvif::make_same_host_policy();
    bool rejected = false;
    if (parsed.value().common.json)
    {
        std::cout << '[';
    }
    for (std::size_t index = 0; index < candidates.size(); ++index)
    {
        const auto result = policy->authorize(candidates[index], origin);
        rejected = rejected || !result;
        if (parsed.value().common.json)
        {
            std::cout << (index ? "," : "") << "{\"url\":\""
                      << wlonvif::examples::json_escape(
                             // Never print a password that was part of a candidate URL.
                             wlonvif::examples::redact_url(candidates[index])
                         )
                      << "\",\"authorized\":" << (result ? "true" : "false") << '}';
        }
        else
        {
            std::cout << (result ? "ACCEPT " : "REJECT ")
                      << wlonvif::examples::redact_url(candidates[index]) << '\n';
        }
    }
    if (parsed.value().common.json)
    {
        std::cout << "]\n";
    }
    return rejected ? 1 : 0;
}
