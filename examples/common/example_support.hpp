#pragma once

/**
 * @file example_support.hpp
 * @brief Private command-line support shared by the wlonvif examples.
 *
 * Not part of the library: these are conveniences that keep each example
 * focused on the ONVIF calls it demonstrates. The definitions are in
 * command_line.cpp (options and credentials), connection.cpp (connecting,
 * deadlines, Ctrl-C), and text.cpp (formatting).
 */

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::examples
{
    /// Options every example accepts.
    struct CommonOptions
    {
        int timeout_seconds{10}; ///< `--timeout`: per-request deadline.
        bool timeout_set{
            false
        }; ///< Whether `--timeout` was given, for examples with other defaults.
        bool json{false};                  ///< `--json`: machine-readable output.
        bool show_progress{true};          ///< Cleared by `--no-progress`.
        bool allow_cleartext_basic{false}; ///< `--allow-cleartext-basic`.
        std::string username;
        std::string password;      ///< Discouraged: visible to other users of the machine.
        std::string password_file; ///< First line of a file only its owner can read.
        std::string password_environment{"WLONVIF_PASSWORD"}; ///< Variable holding the password.
    };

    struct ParsedArguments
    {
        CommonOptions common;
        std::vector<std::string> remaining; ///< Arguments for the example itself, in order.
    };

    /// A Device service URL, and how it was found (for the example to report).
    struct ResolvedDeviceEndpoint
    {
        std::string url;
        std::string evidence;
    };

    /**
     * @brief Run a cold task to completion on the calling thread.
     *
     * The promise is shared-owned rather than left on the stack: future::get()
     * returns as soon as the value is set, so a stack promise can be destroyed
     * while the completing thread is still inside set_value().
     */
    template <class T> T wait_for_task(Task<T> task)
    {
        auto promise = std::make_shared<std::promise<T>>();
        auto future = promise->get_future();
        std::move(task).start(
            [promise](T value)
            {
                promise->set_value(std::move(value));
            }
        );
        return future.get();
    }

    // ---- Options and credentials (command_line.cpp) ----------------------------

    /// Take the common options out of argv; the rest is left for the example.
    Result<ParsedArguments> parse_common_arguments(int argc, char** argv);

    /**
     * @brief Credentials from the options, the environment, or a terminal prompt.
     *
     * Empty when neither a username nor a password was given anywhere, which
     * means anonymous access. When only one was given, the other is prompted for.
     */
    Result<std::optional<Credentials>> load_credentials(const CommonOptions& options);

    /// Parse a positive decimal integer; false for zero, signs, or trailing text.
    bool parse_size(std::string_view text, std::size_t& output);

    /// Whether progress messages go to a terminal, where they can redraw in place.
    bool progress_is_terminal();

    // ---- Connecting and deadlines (connection.cpp) -----------------------------

    /**
     * @brief Connect to a Device service and check that the credentials work.
     *
     * When the device refuses the credentials and a terminal is available, the
     * user is asked for a password, once per process. @p effective_credentials,
     * if given, receives the credentials that worked, for building stream URLs.
     */
    Result<std::shared_ptr<DeviceSession>> connect_device(
        std::string url,
        const std::shared_ptr<Runtime>& runtime,
        std::optional<Credentials> credentials,
        int timeout_seconds,
        std::shared_ptr<HttpTransport> transport = {},
        std::optional<Credentials>* effective_credentials = nullptr
    );

    /// Use @p url as given, or find the Device service on @p address; exactly one must be set.
    Result<ResolvedDeviceEndpoint> resolve_device_endpoint(
        const std::string& url,
        const std::string& address,
        const std::shared_ptr<Runtime>& runtime,
        int timeout_seconds
    );

    /// A deadline @p timeout_seconds from now, cancelled by Ctrl-C.
    CallOptions call_options(int timeout_seconds);
    /// A deadline that Ctrl-C does not cancel, for the Stop sent while shutting down.
    CallOptions safety_call_options(int timeout_seconds);
    /// Return true after SIGINT or SIGTERM is observed by the shared example handler.
    bool shutdown_requested();
    /// Wait for a duration, returning early (and false) when shutdown is requested.
    bool interruptible_wait(std::chrono::milliseconds duration);

    // ---- Text (text.cpp) -------------------------------------------------------

    /// @p input escaped for use inside a JSON string.
    std::string json_escape(std::string_view input);
    /// @p input quoted for pasting into this platform's shell.
    std::string shell_quote(std::string_view input);
    /// @p url with the credentials as percent-encoded user information, for players and FFmpeg.
    std::string
    url_with_credentials(std::string_view url, const std::optional<Credentials>& credentials);
    /// @p url with any user information replaced by `[redacted]`, for printing.
    std::string redact_url(std::string_view url);
    /// The kebab-case name of @p category, such as `soap-fault`.
    std::string_view error_category_name(ErrorCategory category);
    /// ONVIF operation name of @p operation, such as `GetProfiles`.
    std::string_view operation_name(OperationId operation);
} // namespace wlonvif::examples
