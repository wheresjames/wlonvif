#include <string_view>
#include <wlonvif/version.hpp>
#include <wlonvif/wlonvif.hpp>

int main()
{
    auto runtime = wlonvif::Runtime::create();
    if (!runtime)
    {
        return 1;
    }
    auto provider = wlonvif::make_fixed_credential_provider({"user", "password"});
    const auto features = wlonvif::library_features();
    // PROJECT.txt, the installed version header, and the compiled library must agree.
    if (wlonvif::library_version() != std::string_view{WLONVIF_EXPECTED_VERSION} ||
        std::string_view{WLONVIF_VERSION_STRING} != WLONVIF_EXPECTED_VERSION)
    {
        return 3;
    }
    if ( !features.candidate_authorization ||
        !features.credential_providers || !features.deterministic_session_close ||
        !features.ptz_safety_ledger)
    {
        return 2;
    }
    wlonvif::CurlTransportOptions transport_options;
    transport_options.runtime = runtime.value();
    auto transport = wlonvif::make_curl_transport(std::move(transport_options));
    wlonvif::DeviceSessionOptions session_options;
    session_options.device_url = "http://127.0.0.1/device";
    session_options.transport = std::move(transport);
    session_options.endpoint_policy = wlonvif::make_same_origin_policy();
    session_options.runtime = runtime.value();
    session_options.credential_provider = std::move(provider);
    session_options.call = {
        std::chrono::steady_clock::now() + std::chrono::seconds(1),
        wlonvif::CancellationSource{}.token()
    };
    return runtime.value()->shutdown() ? 0 : 1;
}
