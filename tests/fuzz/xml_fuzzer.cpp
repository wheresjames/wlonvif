#include <cstddef>
#include <cstdint>
#include <string_view>
#include <wlonvif/wlonvif.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto input = std::string_view(reinterpret_cast<const char*>(data), size);
    (void)wlonvif::read_soap_operation(
        input,
        "GetDeviceInformationResponse",
        wlonvif::XmlLimits{64 * 1024, 32, 4096, 64, 16 * 1024}
    );
    return 0;
}
