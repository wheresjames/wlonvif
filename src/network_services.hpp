#pragma once

#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    void set_network_call_options(const CallOptions& options);
    void clear_network_call_options();

    std::shared_ptr<ServiceBackend> make_network_service_backend(
        std::string device_url,
        std::shared_ptr<HttpTransport> transport,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<CredentialProvider> credential_provider,
        CancellationToken session_cancellation
    );
}
