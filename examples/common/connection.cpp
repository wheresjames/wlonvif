/**
 * @file connection.cpp
 * @brief Connecting to a device, request deadlines, and Ctrl-C handling.
 *
 * Every request an example makes gets a deadline and a cancellation token.
 * The token is cancelled on SIGINT or SIGTERM, so Ctrl-C ends a long request
 * at once instead of waiting for its timeout. Safety commands, such as the
 * Stop sent while shutting down, use a token Ctrl-C does not cancel.
 */

#include "example_support.hpp"
#include "terminal.hpp"

#include <algorithm>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>

namespace wlonvif::examples
{
    namespace
    {
        volatile std::sig_atomic_t interrupted = 0;

        void signal_handler(int)
        {
            interrupted = 1;
        }

        /**
         * Turns a signal into a cancellation. A signal handler may only set a
         * flag, so a thread watches the flag and cancels the shared source.
         */
        struct SignalCancellation
        {
            CancellationSource source;
            std::jthread monitor;

            SignalCancellation()
                : monitor(
                      [this](std::stop_token stop)
                      {
                          while (!stop.stop_requested() && !interrupted)
                          {
                              std::this_thread::sleep_for(std::chrono::milliseconds(20));
                          }
                          if (interrupted)
                          {
                              source.request_stop();
                          }
                      }
                  )
            {
                std::signal(SIGINT, signal_handler);
#ifdef SIGTERM
                std::signal(SIGTERM, signal_handler);
#endif
            }
        };

        /// Created, and the handlers installed, on first use.
        SignalCancellation& signal_cancellation()
        {
            static SignalCancellation state;
            return state;
        }

        // Credentials typed at a prompt, kept for the other connections a
        // multi-camera example makes, so the user is asked only once.
        std::mutex credential_prompt_mutex;
        std::optional<Credentials> prompted_credentials;

        /**
         * @brief True when @p error looks like a device that cannot answer cleartext Digest.
         *
         * Over plain HTTP the client offers only Digest, and several camera families
         * accept the connection and then hang up rather than answering the challenge.
         *
         * The non-retryable flag is what separates this from an ordinary transport
         * fault: the transport clears it only for a peer that accepted the connection
         * and then dropped an authenticated exchange. Testing the category alone would
         * blame Digest for a refused connection or an unplugged cable.
         */
        bool suggests_cleartext_basic(
            const Error& error,
            const std::string& url,
            const std::optional<Credentials>& credentials
        )
        {
            if (!credentials || credentials->allow_cleartext_basic)
            {
                return false;
            }
            if (!url.starts_with("http://"))
            {
                return false;
            }
            if (error.retryable)
            {
                return false;
            }
            return error.category == ErrorCategory::network ||
                   error.category == ErrorCategory::malformed_http;
        }

        void warn_cleartext_basic(bool device_answered_anonymously)
        {
            std::cerr
                << '\n'
                << (device_answered_anonymously
                        ? "Warning: the device replied before credentials were supplied, then\n"
                          "         dropped the authenticated request.\n"
                        : "Warning: the device dropped the authenticated request.\n")
                << "         Over cleartext HTTP only Digest authentication is offered, and\n"
                   "         some cameras cannot process it.\n"
                   "\n"
                   "         Retry with --allow-cleartext-basic.\n"
                   "\n"
                   "         Devices that authenticate with a WS-Security UsernameToken send\n"
                   "         no HTTP challenge, so no password is transmitted in cleartext.\n\n";
        }
    } // namespace

    // ---- Deadlines and shutdown ------------------------------------------------

    CallOptions call_options(int timeout_seconds)
    {
        return {
            std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds),
            signal_cancellation().source.token()
        };
    }

    CallOptions safety_call_options(int timeout_seconds)
    {
        return {
            std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds),
            CancellationSource{}.token()
        };
    }

    bool shutdown_requested()
    {
        return signal_cancellation().source.token().stop_requested();
    }

    bool interruptible_wait(std::chrono::milliseconds duration)
    {
        const auto deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (shutdown_requested())
            {
                return false;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()
            );
            std::this_thread::sleep_for(std::min(std::chrono::milliseconds(20), left));
        }
        return !shutdown_requested();
    }

    // ---- Connecting ------------------------------------------------------------

    Result<std::shared_ptr<DeviceSession>> connect_device(
        std::string url,
        const std::shared_ptr<Runtime>& runtime,
        std::optional<Credentials> credentials,
        int timeout_seconds,
        std::shared_ptr<HttpTransport> transport,
        std::optional<Credentials>* effective_credentials
    )
    {
        if (!transport)
        {
            transport = make_curl_transport(runtime);
        }
        const auto connect = [&](std::optional<Credentials> selected)
        {
            return wait_for_task(
                DeviceSession::connect(
                    url,
                    transport,
                    make_same_host_policy(),
                    runtime,
                    std::move(selected),
                    call_options(timeout_seconds)
                )
            );
        };

        // Connecting succeeds without checking credentials, so make a few
        // requests that need them; a wrong password shows up here.
        const auto probe_access =
            [&](const std::shared_ptr<DeviceSession>& selected) -> Result<void>
        {
            auto services = wait_for_task(selected->get_services(call_options(timeout_seconds)));
            if (!services)
            {
                return services.error();
            }
            auto capabilities =
                wait_for_task(selected->get_capabilities(call_options(timeout_seconds)));
            if (!capabilities)
            {
                return capabilities.error();
            }
            // ONVIF lets devices answer GetServices and GetCapabilities without
            // credentials, so ask for something protected too: the Media profiles.
            auto media = selected->media();
            if (media)
            {
                auto profiles =
                    wait_for_task(media.value()->get_profiles(call_options(timeout_seconds)));
                if (!profiles && profiles.error().category == ErrorCategory::authentication)
                {
                    return profiles.error();
                }
            }
            return {};
        };

        auto session = connect(credentials);
        if (!session)
        {
            return session.error();
        }
        auto probe = probe_access(session.value());
        if (probe)
        {
            if (effective_credentials)
            {
                *effective_credentials = credentials;
            }
            return session;
        }
        if (probe.error().category != ErrorCategory::authentication || !detail::input_is_terminal())
        {
            if (suggests_cleartext_basic(probe.error(), url, credentials))
            {
                warn_cleartext_basic(false);
            }
            return probe.error();
        }

        // Refused, and someone is at the terminal: ask once, then try again.
        std::lock_guard lock(credential_prompt_mutex);
        if (!prompted_credentials)
        {
            std::string username;
            bool allow_cleartext_basic = false;
            if (credentials)
            {
                username = credentials->username;
                allow_cleartext_basic = credentials->allow_cleartext_basic;
            }
            if (username.empty())
            {
                auto entered = detail::prompt_username();
                if (!entered)
                {
                    return probe.error();
                }
                username = std::move(entered.value());
            }
            auto password = detail::prompt_password();
            if (!password)
            {
                return probe.error();
            }
            prompted_credentials = Credentials{
                std::move(username), std::move(password.value()), allow_cleartext_basic
            };
        }
        session = connect(prompted_credentials);
        if (!session)
        {
            return session.error();
        }
        probe = probe_access(session.value());
        if (!probe)
        {
            // The device answered the pre-credential probe with an authentication
            // error, so it is reachable and speaking ONVIF. A transport failure only
            // after credentials were attached points squarely at the Digest exchange.
            if (suggests_cleartext_basic(probe.error(), url, prompted_credentials))
            {
                warn_cleartext_basic(true);
            }
            prompted_credentials.reset();
            return probe.error();
        }
        if (effective_credentials)
        {
            *effective_credentials = prompted_credentials;
        }
        return session;
    }

    Result<ResolvedDeviceEndpoint> resolve_device_endpoint(
        const std::string& url,
        const std::string& address,
        const std::shared_ptr<Runtime>& runtime,
        int timeout_seconds
    )
    {
        if (url.empty() == address.empty())
        {
            return detail::argument_error("specify exactly one of --url or --ip");
        }
        if (!url.empty())
        {
            return ResolvedDeviceEndpoint{url, "explicit Device service URL"};
        }
        // Probe the one address directly: no multicast, the usual ONVIF ports and paths.
        NetworkDiscoveryOptions discovery;
        discovery.multicast = false;
        discovery.direct_probe = true;
        discovery.targets.push_back(address);
        // One known address: let a slow device use the whole budget rather than
        // the short per-address timeouts sized for sweeping a network.
        discovery.connect_timeout = std::chrono::seconds(std::max(1, timeout_seconds / 2));
        discovery.request_timeout = std::chrono::seconds(timeout_seconds);
        auto candidates = wait_for_task(discover_devices(
            runtime, make_same_host_policy(), std::move(discovery), call_options(timeout_seconds)
        ));
        if (!candidates)
        {
            return candidates.error();
        }
        for (const auto& candidate : candidates.value())
        {
            if (candidate.device_service)
            {
                return ResolvedDeviceEndpoint{
                    *candidate.device_service,
                    candidate.diagnostic.empty() ? "targeted discovery" : candidate.diagnostic
                };
            }
        }
        return Error{
            ErrorCategory::unsupported,
            OperationId::discovery_probe,
            "targeted discovery found no Device service URL"
        };
    }
} // namespace wlonvif::examples
