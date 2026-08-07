#include "example_support.hpp"

#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace wlonvif::examples
{
    namespace
    {
        volatile std::sig_atomic_t interrupted = 0;

        void signal_handler(int)
        {
            interrupted = 1;
        }

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

        SignalCancellation& signal_cancellation()
        {
            static SignalCancellation state;
            return state;
        }

        Error argument_error(std::string message)
        {
            return Error{ErrorCategory::invalid_argument, {}, std::move(message)};
        }

        Result<std::string> read_password_file(const std::string& path)
        {
#ifndef _WIN32
            struct stat status{};
            if (stat(path.c_str(), &status) != 0)
            {
                return argument_error("unable to inspect password file");
            }
            if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0)
            {
                return argument_error("password file must not be accessible by group or others");
            }
#endif
            std::ifstream input(path);
            std::string password;
            if (!input || !std::getline(input, password))
            {
                return argument_error("unable to read password file");
            }
            if (!password.empty() && password.back() == '\r')
            {
                password.pop_back();
            }
            return password;
        }

        Result<std::string> prompt_password()
        {
            std::cerr << "Password: " << std::flush;
#ifdef _WIN32
            HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD mode = 0;
            if (input == INVALID_HANDLE_VALUE || !GetConsoleMode(input, &mode))
            {
                return argument_error("no terminal available for password prompt");
            }
            SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT);
            std::string password;
            std::getline(std::cin, password);
            SetConsoleMode(input, mode);
#else
            if (!isatty(fileno(stdin)))
            {
                return argument_error("no terminal available for password prompt");
            }
            termios original{};
            if (tcgetattr(fileno(stdin), &original) != 0)
            {
                return argument_error("unable to configure password prompt");
            }
            auto hidden = original;
            hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
            (void)tcsetattr(fileno(stdin), TCSAFLUSH, &hidden);
            std::string password;
            std::getline(std::cin, password);
            (void)tcsetattr(fileno(stdin), TCSAFLUSH, &original);
#endif
            std::cerr << '\n';
            return password;
        }

        Result<std::string> prompt_username()
        {
#ifdef _WIN32
            if (!_isatty(_fileno(stdin)))
#else
            if (!isatty(fileno(stdin)))
#endif
            {
                return argument_error("no terminal available for username prompt");
            }
            std::cerr << "Username: " << std::flush;
            std::string username;
            if (!std::getline(std::cin, username) || username.empty())
            {
                return argument_error("username is required when a password is supplied");
            }
            return username;
        }

        bool input_is_terminal()
        {
#ifdef _WIN32
            return _isatty(_fileno(stdin)) != 0;
#else
            return isatty(fileno(stdin)) != 0;
#endif
        }

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

    Result<ParsedArguments> parse_common_arguments(int argc, char** argv)
    {
        ParsedArguments parsed;
        for (int index = 1; index < argc; ++index)
        {
            const std::string argument(argv[index]);
            const bool needs_value = argument == "--timeout" || argument == "--username" ||
                                     argument == "--password" ||
                                     argument == "--password-file" ||
                                     argument == "--password-env";
            if (needs_value && index + 1 >= argc)
            {
                return argument_error("missing value for " + argument);
            }
            if (argument == "--timeout")
            {
                std::size_t timeout = 0;
                if (!parse_size(argv[++index], timeout) || timeout > 3600)
                {
                    return argument_error("timeout must be between 1 and 3600 seconds");
                }
                parsed.common.timeout_seconds = static_cast<int>(timeout);
                parsed.common.timeout_set = true;
            }
            else if (argument == "--username")
            {
                parsed.common.username = argv[++index];
            }
            else if (argument == "--password")
            {
                parsed.common.password = argv[++index];
            }
            else if (argument == "--password-file")
            {
                parsed.common.password_file = argv[++index];
            }
            else if (argument == "--password-env")
            {
                parsed.common.password_environment = argv[++index];
            }
            else if (argument == "--allow-cleartext-basic")
            {
                parsed.common.allow_cleartext_basic = true;
            }
            else if (argument == "--json")
            {
                parsed.common.json = true;
            }
            else if (argument == "--no-progress")
            {
                parsed.common.show_progress = false;
            }
            else
            {
                parsed.remaining.push_back(argument);
            }
        }
        return parsed;
    }

    Result<std::optional<Credentials>> load_credentials(const CommonOptions& options)
    {
        std::string username = options.username;
        if (username.empty())
        {
            if (const char* environment_username = std::getenv("WLONVIF_USERNAME"))
            {
                username = environment_username;
            }
        }
        std::optional<std::string> password;
        if (!options.password.empty())
        {
            password = options.password;
        }
        else if (!options.password_file.empty())
        {
            auto loaded = read_password_file(options.password_file);
            if (!loaded)
            {
                return loaded.error();
            }
            password = std::move(loaded.value());
        }
        else if (!options.password_environment.empty())
        {
            if (const char* value = std::getenv(options.password_environment.c_str()))
            {
                password = value;
            }
        }
        if (username.empty() && !password)
        {
            return std::optional<Credentials>{};
        }
        if (username.empty())
        {
            auto prompted = prompt_username();
            if (!prompted)
            {
                return prompted.error();
            }
            username = std::move(prompted.value());
        }
        if (!password)
        {
            auto prompted = prompt_password();
            if (!prompted)
            {
                return prompted.error();
            }
            password = std::move(prompted.value());
        }
        return std::optional<Credentials>(Credentials{
            std::move(username), std::move(*password), options.allow_cleartext_basic
        });
    }

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
            if (shutdown_requested()) return false;
            std::this_thread::sleep_for(std::min(
                std::chrono::milliseconds(20),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()
                )
            ));
        }
        return !shutdown_requested();
    }

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
            return wait_for_task(DeviceSession::connect(
                url,
                transport,
                make_same_host_policy(),
                runtime,
                std::move(selected),
                call_options(timeout_seconds)
            ));
        };
        auto session = connect(credentials);
        if (!session)
        {
            return session.error();
        }
        const auto probe_access = [&](const std::shared_ptr<DeviceSession>& selected)
            -> Result<void>
        {
            auto services = wait_for_task(selected->get_services(call_options(timeout_seconds)));
            if (!services)
            {
                return services.error();
            }
            auto capabilities = wait_for_task(
                selected->get_capabilities(call_options(timeout_seconds))
            );
            if (!capabilities)
            {
                return capabilities.error();
            }
            auto media = selected->media();
            if (media)
            {
                auto profiles = wait_for_task(
                    media.value()->get_profiles(call_options(timeout_seconds))
                );
                if (!profiles && profiles.error().category == ErrorCategory::authentication)
                {
                    return profiles.error();
                }
            }
            return {};
        };
        auto probe = probe_access(session.value());
        if (probe)
        {
            if (effective_credentials) *effective_credentials = credentials;
            return session;
        }
        if (probe.error().category != ErrorCategory::authentication || !input_is_terminal())
        {
            if (suggests_cleartext_basic(probe.error(), url, credentials))
            {
                warn_cleartext_basic(false);
            }
            return probe.error();
        }

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
                auto entered = prompt_username();
                if (!entered) return probe.error();
                username = std::move(entered.value());
            }
            auto password = prompt_password();
            if (!password) return probe.error();
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
        if (effective_credentials) *effective_credentials = prompted_credentials;
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
            return argument_error("specify exactly one of --url or --ip");
        }
        if (!url.empty())
        {
            return ResolvedDeviceEndpoint{url, "explicit Device service URL"};
        }
        NetworkDiscoveryOptions discovery;
        discovery.multicast = false;
        discovery.direct_probe = true;
        discovery.targets.push_back(address);
        auto candidates = wait_for_task(discover_devices(
            runtime,
            make_same_host_policy(),
            std::move(discovery),
            call_options(timeout_seconds)
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

    bool parse_size(std::string_view text, std::size_t& output)
    {
        std::size_t value = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
        {
            return false;
        }
        output = value;
        return true;
    }

    bool progress_is_terminal()
    {
#ifdef _WIN32
        return _isatty(_fileno(stderr)) != 0;
#else
        return isatty(fileno(stderr)) != 0;
#endif
    }

    std::string json_escape(std::string_view input)
    {
        std::string output;
        for (const unsigned char value : input)
        {
            switch (value)
            {
                case '\\':
                case '"':
                    output += '\\';
                    output += static_cast<char>(value);
                    break;
                case '\b':
                    output += "\\b";
                    break;
                case '\f':
                    output += "\\f";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '\r':
                    output += "\\r";
                    break;
                case '\t':
                    output += "\\t";
                    break;
                default:
                    if (value < 0x20)
                    {
                        constexpr char digits[] = "0123456789abcdef";
                        output += "\\u00";
                        output += digits[value >> 4];
                        output += digits[value & 0x0f];
                    }
                    else
                    {
                        output += static_cast<char>(value);
                    }
            }
        }
        return output;
    }

    std::string shell_quote(std::string_view input)
    {
#ifdef _WIN32
        std::string output{"\""};
        for (const char value : input)
        {
            if (value == '\"' || value == '\\') output += '\\';
            output += value;
        }
        return output + '\"';
#else
        std::string output{"'"};
        for (const char value : input)
        {
            output += value == '\'' ? "'\\''" : std::string(1, value);
        }
        return output + '\'';
#endif
    }

    std::string url_with_credentials(
        std::string_view url, const std::optional<Credentials>& credentials
    )
    {
        if (!credentials || credentials->username.empty())
        {
            return std::string(url);
        }
        const auto scheme = url.find("://");
        if (scheme == url.npos)
        {
            return std::string(url);
        }
        const auto encode = [](std::string_view value)
        {
            constexpr char digits[] = "0123456789ABCDEF";
            std::string output;
            for (const auto character : value)
            {
                const auto byte = static_cast<unsigned char>(character);
                if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                    (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                    byte == '_' || byte == '~')
                {
                    output += static_cast<char>(byte);
                }
                else
                {
                    output += '%';
                    output += digits[byte >> 4];
                    output += digits[byte & 0x0f];
                }
            }
            return output;
        };
        const auto authority_start = scheme + 3;
        const auto authority_end = url.find_first_of("/?#", authority_start);
        const auto at = url.find('@', authority_start);
        const auto host_start = at != url.npos &&
                                        (authority_end == url.npos || at < authority_end)
                                    ? at + 1
                                    : authority_start;
        return std::string(url.substr(0, authority_start)) + encode(credentials->username) + ':' +
               encode(credentials->password) + '@' + std::string(url.substr(host_start));
    }

    std::string_view error_category_name(ErrorCategory category)
    {
        switch (category)
        {
            case ErrorCategory::cancelled: return "cancelled";
            case ErrorCategory::timeout: return "timeout";
            case ErrorCategory::network: return "network";
            case ErrorCategory::tls: return "tls";
            case ErrorCategory::authentication: return "authentication";
            case ErrorCategory::authorization: return "authorization";
            case ErrorCategory::device_busy: return "device-busy";
            case ErrorCategory::rate_limited: return "rate-limited";
            case ErrorCategory::malformed_http: return "malformed-http";
            case ErrorCategory::malformed_xml: return "malformed-xml";
            case ErrorCategory::soap_fault: return "soap-fault";
            case ErrorCategory::unsupported: return "unsupported";
            case ErrorCategory::invalid_argument: return "invalid-argument";
            case ErrorCategory::endpoint_rejected: return "endpoint-rejected";
            case ErrorCategory::host_permission_denied: return "host-permission-denied";
            case ErrorCategory::closed_handle: return "closed-handle";
            case ErrorCategory::protocol_violation: return "protocol-violation";
            case ErrorCategory::closed: return "closed";
            case ErrorCategory::queue_overflow: return "queue-overflow";
            case ErrorCategory::internal: return "internal";
        }
        return "unknown";
    }

    std::string redact_url(std::string_view url)
    {
        const auto scheme = url.find("://");
        if (scheme == url.npos)
        {
            return std::string(url);
        }
        const auto authority_start = scheme + 3;
        const auto authority_end = url.find_first_of("/?#", authority_start);
        const auto at = url.find('@', authority_start);
        if (at == url.npos || (authority_end != url.npos && at > authority_end))
        {
            return std::string(url);
        }
        return std::string(url.substr(0, authority_start)) + "[redacted]@" +
               std::string(url.substr(at + 1));
    }
} // namespace wlonvif::examples
