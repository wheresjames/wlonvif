#pragma once

#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    /// Replace this thread's network call options, returning the previous value.
    std::optional<CallOptions> exchange_network_call_options(std::optional<CallOptions> options);

    /**
     * @brief Apply call options to synchronous backend calls made on this thread.
     *
     * The previous options are restored on destruction, so a nested safety Stop
     * issued in the middle of an operation does not strip the enclosing
     * operation's deadline and cancellation for the rest of its work.
     */
    class ScopedNetworkCallOptions
    {
      public:
        explicit ScopedNetworkCallOptions(const CallOptions& options)
            : previous_(exchange_network_call_options(options)) {}
        ~ScopedNetworkCallOptions() { exchange_network_call_options(std::move(previous_)); }
        ScopedNetworkCallOptions(const ScopedNetworkCallOptions&) = delete;
        ScopedNetworkCallOptions& operator=(const ScopedNetworkCallOptions&) = delete;

      private:
        std::optional<CallOptions> previous_;
    };

    std::shared_ptr<ServiceBackend> make_network_service_backend(
        std::string device_url,
        std::shared_ptr<HttpTransport> transport,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<CredentialProvider> credential_provider,
        CancellationToken session_cancellation,
        std::shared_ptr<detail::DeviceClock> clock = {}
    );
}
