/**
 * @file protocol_fuzzer.cpp
 * @brief libFuzzer target: arbitrary bytes as a control message from a valid session.
 *
 * The protocol is the boundary a hostile page can reach, so it must reject
 * anything without crashing. Built with WLONVIF_BUILD_FUZZERS.
 */

#include "protocol.hpp"

#include <cstddef>
#include <cstdint>

using namespace wlonvif::examples::ptz_web;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    ProtocolConfig config{{"http://127.0.0.1:8080"}};
    config.maximum_message_bytes = 4096;
    config.maximum_messages_per_second = 100000;
    ControlProtocol protocol(config);
    const auto now = std::chrono::steady_clock::time_point{};
    const std::string session(32, 'a');
    (void)protocol.create_session(config.allowed_origins.front(), now, session);
    (void)protocol.handle(
        session,
        config.allowed_origins.front(),
        std::string_view(reinterpret_cast<const char*>(data), size),
        now
    );
    return 0;
}
