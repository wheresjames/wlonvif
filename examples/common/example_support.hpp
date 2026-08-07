#pragma once

/**
 * @file example_support.hpp
 * @brief Private command-line support shared by wlonvif examples.
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
    struct CommonOptions
    {
        int timeout_seconds{10};
        bool timeout_set{false};
        bool json{false};
        bool show_progress{true};
        bool allow_cleartext_basic{false};
        std::string username;
        std::string password;
        std::string password_file;
        std::string password_environment{"WLONVIF_PASSWORD"};
    };

    struct ParsedArguments
    {
        CommonOptions common;
        std::vector<std::string> remaining;
    };

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

    Result<ParsedArguments> parse_common_arguments(int argc, char** argv);
    Result<std::optional<Credentials>> load_credentials(const CommonOptions& options);
    Result<std::shared_ptr<DeviceSession>> connect_device(
        std::string url,
        const std::shared_ptr<Runtime>& runtime,
        std::optional<Credentials> credentials,
        int timeout_seconds,
        std::shared_ptr<HttpTransport> transport = {},
        std::optional<Credentials>* effective_credentials = nullptr
    );
    Result<ResolvedDeviceEndpoint> resolve_device_endpoint(
        const std::string& url,
        const std::string& address,
        const std::shared_ptr<Runtime>& runtime,
        int timeout_seconds
    );
    CallOptions call_options(int timeout_seconds);
    CallOptions safety_call_options(int timeout_seconds);
    bool parse_size(std::string_view text, std::size_t& output);
    bool progress_is_terminal();
    std::string json_escape(std::string_view input);
    std::string shell_quote(std::string_view input);
    std::string url_with_credentials(
        std::string_view url, const std::optional<Credentials>& credentials
    );
    std::string_view error_category_name(ErrorCategory category);
    std::string redact_url(std::string_view url);
} // namespace wlonvif::examples
