/**
 * @file xml_scan_fuzzer.cpp
 * @brief Fuzz target for the internal XML scanner.
 *
 * The scanner is hand-written and reads unauthenticated input: WS-Discovery
 * datagrams arrive over UDP from anyone on the local network, before any
 * authentication happens. It is also the code that replaced std::regex on those
 * paths, so it is worth continuous coverage in its own right.
 *
 * Every entry point is driven with the same buffer, including the ones that walk
 * a located element's interior, so nesting and truncation bugs surface as
 * out-of-range access under ASan rather than as silently wrong output.
 */

#include "../../src/xml_scan.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    using namespace wlonvif::detail_xml;
    const auto input = std::string_view(reinterpret_cast<const char*>(data), size);

    // Names drawn from what the discovery and SOAP paths actually look for.
    for (const auto* name : {"XAddrs", "Address", "Scopes", "PanTilt", "Profiles", "a"})
    {
        (void)first_text(input, name);
        if (const auto element = find_element(input, name))
        {
            (void)inner(input, *element);
            (void)text(input, *element);
            const auto raw = attributes(input, *element);
            (void)attribute(raw, "token");
            (void)attribute(raw, "x");
            (void)attribute(raw, "UtcTime");
            // Re-scan the interior: nested lookups are how the real decoders work.
            (void)first_text(inner(input, *element), "Min");
        }
        (void)numeric_attribute(input, name, "x");
        std::size_t seen = 0;
        for_each_element(input, name, [&](const Element&) { ++seen; });
    }

    (void)unescape(input);
    (void)escape(input);
    (void)parse_double(input);
    if (!input.empty())
    {
        // Offsets past the end must be rejected rather than indexed.
        (void)find_element(input, "a", size);
        (void)find_element(input, "a", size / 2);
    }
    return 0;
}
