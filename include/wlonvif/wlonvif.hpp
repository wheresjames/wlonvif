#pragma once

/**
 * @file wlonvif.hpp
 * @brief Public C++ API for ONVIF clients, discovery, events, and virtual devices.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace wlonvif
{

    /// Broad, stable classification for an operation failure.
    enum class ErrorCategory
    {
        cancelled,          ///< Cancellation won the operation race.
        timeout,            ///< The absolute deadline expired.
        network,            ///< Socket, DNS, or HTTP transport failure.
        tls,                ///< TLS negotiation or verification failure.
        authentication,     ///< Supplied credentials were not accepted.
        authorization,      ///< Authenticated identity lacks permission.
        device_busy,        ///< Device explicitly reported a documented busy signal.
        rate_limited,       ///< Peer explicitly requested rate limiting.
        malformed_http,     ///< HTTP metadata or framing was invalid.
        malformed_xml,      ///< XML was invalid or exceeded a structural limit.
        soap_fault,         ///< The peer returned a SOAP Fault.
        unsupported,        ///< The requested feature or dialect is unsupported.
        invalid_argument,   ///< A caller-provided value is invalid.
        endpoint_rejected,  ///< EndpointPolicy rejected the destination.
        host_permission_denied, ///< Embedding host denied a resolved network candidate.
        closed_handle,      ///< An operation was requested on a closed public handle.
        protocol_violation, ///< The peer response violated the operation contract.
        closed,             ///< The owning session, stream, or runtime is closed.
        queue_overflow,     ///< A bounded queue cannot accept more data.
        internal            ///< Unexpected library failure.
    };

    /// Owned HTTP response detail attached to an Error.
    struct HttpErrorDetail
    {
        long status{};
    };

    /// Bounded, sanitized SOAP Fault fields attached to an Error.
    struct SoapFaultDetail
    {
        std::string code;
        std::string subcode;
        std::string reason;
        std::string detail;
    };

    /// Audit context for a host network permission denial.
    struct HostPermissionDetail
    {
        std::string hostname;
        std::uint16_t port{};
        std::string proposed_address;
        std::string resolution_attempt_id;
    };

    /// Identifies the ONVIF operation associated with a result or diagnostic.
    enum class OperationId
    {
        get_device_information,   ///< Device identity query.
        get_system_date_and_time, ///< Device clock query.
        get_scopes,               ///< Device scope query.
        get_services,             ///< Advertised service query.
        get_capabilities,         ///< Device capability query.
        get_profiles,             ///< Media profile query.
        get_stream_uri,           ///< Stream URI query.
        get_snapshot_uri,         ///< Snapshot URI query.
        ptz_status,               ///< PTZ status or space query.
        get_ptz_configuration_options, ///< PTZ coordinate space query.
        continuous_move,          ///< Continuous PTZ movement.
        relative_move,            ///< Relative PTZ movement.
        absolute_move,            ///< Absolute PTZ movement.
        ptz_stop,                 ///< PTZ Stop operation.
        get_presets,              ///< PTZ preset query.
        goto_preset,              ///< PTZ preset movement.
        discovery_probe,          ///< WS-Discovery operation.
        event_subscribe,          ///< Create a PullPoint subscription.
        event_pull,               ///< PullPoint event retrieval.
        event_renew,              ///< Event subscription renewal.
        event_close,              ///< Event subscription closure.
        set_system_date_and_time, ///< Device clock configuration.
        get_ntp,                  ///< NTP configuration query.
        set_ntp,                  ///< NTP configuration change.
        set_scopes,               ///< Replace configurable scopes.
        add_scopes,               ///< Add configurable scopes.
        remove_scopes,            ///< Remove configurable scopes.
        get_event_properties,     ///< Event topic discovery.
        get_analytics_modules,    ///< Analytics module query.
        modify_analytics_modules, ///< Analytics module change.
        get_analytics_rules,      ///< Analytics rule query.
        modify_analytics_rules,   ///< Analytics rule change.
        get_video_sources,        ///< Media video source query.
        get_imaging_settings,     ///< Imaging settings query.
        set_imaging_settings,     ///< Imaging settings change.
        get_imaging_options,      ///< Imaging setting ranges query.
        get_video_encoder_options,     ///< Video encoder option query.
        set_video_encoder_configuration, ///< Video encoder change.
        set_preset,               ///< Save the current position as a preset.
        remove_preset,            ///< Delete a preset.
        get_preset_tours,         ///< Preset tour query.
        create_preset_tour,       ///< Create an empty preset tour.
        modify_preset_tour,       ///< Change a preset tour.
        operate_preset_tour,      ///< Start, stop, or pause a preset tour.
        remove_preset_tour,       ///< Delete a preset tour.
        get_ptz_configuration,    ///< PTZ configuration query.
        set_ptz_configuration,    ///< PTZ configuration change.
        get_hostname,             ///< Hostname query.
        set_hostname,             ///< Hostname change.
        get_network_interfaces,   ///< Network interface query.
        get_dns,                  ///< DNS configuration query.
        get_network_default_gateway, ///< Default gateway query.
        get_network_protocols,    ///< HTTP/RTSP port query.
        get_discovery_mode,       ///< WS-Discovery visibility query.
        set_discovery_mode,       ///< WS-Discovery visibility change.
        get_users,                ///< Device account query.
        system_reboot,            ///< Device restart.
        custom                    ///< Caller-composed request sent with DeviceSession::call().
    };

    /// Outcome of a bounded best-effort safety action.
    struct SafetyActionDetail
    {
        OperationId operation{OperationId::ptz_stop}; ///< Safety operation attempted.
        bool attempted{false}; ///< Whether the safety action was issued.
        bool succeeded{false}; ///< Whether the device accepted the safety action.
        std::optional<ErrorCategory> failure_category; ///< Redacted failure classification.
        std::string failure_code; ///< Bounded SOAP or HTTP failure code.
    };

    /// Compile-time and runtime capabilities used by embedding adapters.
    struct LibraryFeatures
    {
        bool candidate_authorization{true};
        bool credential_providers{true};
        bool deterministic_session_close{true};
        bool ptz_safety_ledger{true};
        bool pullpoint_subscription{true};
        bool discovery_streaming{false};
    };

    /// Native library semantic version.
    std::string_view library_version() noexcept;
    /// Native contracts available in this build.
    LibraryFeatures library_features() noexcept;

    /**
     * @brief Structured error returned by public asynchronous operations.
     *
     * The message is safe diagnostic prose and is not a stable programmatic value.
     * Use category and operation for control flow.
     */
    struct Error
    {
        ErrorCategory category{ErrorCategory::internal};            ///< Stable failure category.
        OperationId operation{OperationId::get_device_information}; ///< Failed operation.
        std::string message;   ///< Human-readable, redacted diagnostic message.
        bool retryable{false}; ///< Whether a policy may safely retry the operation.
        bool request_may_have_been_applied{false}; ///< Device outcome may be uncertain.
        std::optional<HttpErrorDetail> http;
        std::optional<SoapFaultDetail> soap_fault;
        std::optional<HostPermissionDetail> host_permission;
        std::optional<SafetyActionDetail> safety_action;

        /// Construct an internal error with default values.
        Error() = default;

        /**
         * @brief Construct a structured error.
         * @param category_value Broad failure category.
         * @param operation_value Associated ONVIF operation.
         * @param message_value Redacted diagnostic prose.
         * @param retryable_value Whether retry can be considered.
         * @param may_have_applied Whether the device may have applied the request.
         */
        Error(
            ErrorCategory category_value,
            OperationId operation_value = OperationId::get_device_information,
            std::string message_value = {},
            bool retryable_value = false,
            bool may_have_applied = false
        )
            : category(category_value), operation(operation_value),
              message(std::move(message_value)), retryable(retryable_value),
              request_may_have_been_applied(may_have_applied)
        {
        }
    };

    /**
     * @brief Expected-like success or Error value for C++20 consumers.
     * @tparam T Successful value type.
     */
    template <class T> class Result
    {
      public:
        /// Construct a successful result.
        Result(T value) : value_(std::move(value))
        {
        }
        /// Construct a failed result.
        Result(Error error) : value_(std::move(error))
        {
        }
        /// Return true when the result contains a successful value.
        explicit operator bool() const noexcept
        {
            return std::holds_alternative<T>(value_);
        }
        /// Access the successful value.
        T& value()
        {
            return std::get<T>(value_);
        }
        /// Access the successful value.
        const T& value() const
        {
            return std::get<T>(value_);
        }
        /// Access the error value.
        Error& error()
        {
            return std::get<Error>(value_);
        }
        /// Access the error value.
        const Error& error() const
        {
            return std::get<Error>(value_);
        }

      private:
        std::variant<T, Error> value_;
    };

    /// Result specialization for operations that return no value.
    template <> class Result<void>
    {
      public:
        /// Construct a successful result.
        Result() = default;
        /// Construct a failed result.
        Result(Error error) : error_(std::move(error))
        {
        }
        /// Return true when the operation succeeded.
        explicit operator bool() const noexcept
        {
            return !error_;
        }
        /// Access the error value.
        Error& error()
        {
            return *error_;
        }
        /// Access the error value.
        const Error& error() const
        {
            return *error_;
        }

      private:
        std::optional<Error> error_;
    };

    /**
     * @brief Abstract destination for asynchronous completion work.
     */
    class Executor
    {
      public:
        virtual ~Executor() = default;
        /**
         * @brief Queue a function for later execution.
         * @param fn Work item to execute exactly once.
         * @return True when accepted, or false when the executor is closed.
         */
        virtual bool post(std::function<void()> fn) noexcept = 0;
    };

    /**
     * @brief Owns asynchronous execution resources used by clients and virtual devices.
     *
     * Runtime has one worker in the current implementation. Call shutdown() before
     * releasing the final shared pointer when deterministic teardown is required.
     */
    class Runtime final : public Executor, public std::enable_shared_from_this<Runtime>
    {
      public:
        /// Create and start a runtime.
        static Result<std::shared_ptr<Runtime>> create();
        /// Stop and release runtime resources as a non-throwing backstop.
        ~Runtime();
        /// Queue work unless shutdown has begun.
        bool post(std::function<void()> fn) noexcept override;
        /// Stop accepting work, drain accepted work, and join the worker.
        Result<void> shutdown();
        /// Register bounded native cleanup to run before worker shutdown.
        std::size_t register_shutdown_participant(std::function<void()> participant);
        /// Remove a previously registered shutdown participant.
        void unregister_shutdown_participant(std::size_t identifier) noexcept;
        /**
         * @brief Return true when called from any Runtime's worker thread.
         *
         * Completions run on the worker, so code that would block waiting for
         * another completion (for example joining a thread that is itself waiting
         * on one) must not do so here; it uses this to choose a non-blocking path.
         */
        static bool on_worker_thread() noexcept;

      private:
        struct State;
        Runtime();
        static void run(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
        std::mutex shutdown_mutex_;
        std::thread worker_;
    };

    /// Copyable view of shared cancellation state.
    class CancellationToken
    {
      public:
        /// Return true after the associated source requests cancellation.
        bool stop_requested() const noexcept;

        /**
         * @brief Return a token cancelled when either input token is cancelled.
         *
         * Linking is transitive: every source observed by either input, including
         * sources it was itself linked from, is observed by the result. Keeping
         * only one state per input would silently drop a link whenever an already
         * linked token is linked again.
         */
        static CancellationToken linked(CancellationToken first, CancellationToken second)
        {
            CancellationToken result;
            result.states_ = std::move(first.states_);
            for (auto& state : second.states_)
            {
                if (std::find(result.states_.begin(), result.states_.end(), state) ==
                    result.states_.end())
                {
                    result.states_.push_back(std::move(state));
                }
            }
            return result;
        }

      private:
        friend class CancellationSource;
        CancellationToken() = default;
        explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
        {
            if (state) states_.push_back(std::move(state));
        }
        std::vector<std::shared_ptr<std::atomic_bool>> states_;
    };
    /// Owner that can request cancellation for associated CancellationToken values.
    class CancellationSource
    {
      public:
        /// Create an independent cancellation source.
        CancellationSource() : state_(std::make_shared<std::atomic_bool>(false))
        {
        }
        /// Return a token observing this source.
        CancellationToken token() const
        {
            return CancellationToken(state_);
        }
        /// Atomically request cancellation. This operation is idempotent.
        void request_stop() noexcept
        {
            state_->store(true);
        }

      private:
        std::shared_ptr<std::atomic_bool> state_;
    };

    /**
     * @brief Move-only, cold, single-consumer asynchronous operation.
     * @tparam T Completion value passed to the callback.
     *
     * Work begins only when start() consumes the task. Dropping an unstarted task has
     * no side effects.
     */
    template <class T> class Task
    {
      public:
        using Completion = std::function<void(T)>;       ///< Completion callback type.
        using Starter = std::function<void(Completion)>; ///< Type-erased operation starter.

        /// Construct a task from a callable accepting Completion.
        template <class F, std::enable_if_t<!std::is_same_v<std::decay_t<F>, Task>, int> = 0>
        explicit Task(F&& starter)
        {
            auto owned = std::make_shared<std::decay_t<F>>(std::forward<F>(starter));
            starter_ = [owned = std::move(owned)](Completion completion) mutable
            {
                (*owned)(std::move(completion));
            };
        }
        /// Move a task and transfer its single-consumer right.
        Task(Task&&) noexcept = default;
        /// Move-assign a task and transfer its single-consumer right.
        Task& operator=(Task&&) noexcept = default;
        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        /// Return true while this task has not been consumed.
        bool valid() const noexcept
        {
            return static_cast<bool>(starter_);
        }
        /**
         * @brief Start the operation and consume this task.
         * @param completion Callback receiving the operation result exactly once.
         * @throws std::logic_error if the task was already consumed.
         */
        void start(Completion completion) &&
        {
            auto starter = std::exchange(starter_, {});
            if (!starter)
            {
                throw std::logic_error("Task already consumed");
            }
            starter(std::move(completion));
        }

      private:
        Starter starter_;
    };

    /** @brief Structural and byte limits applied while reading untrusted XML. */
    struct XmlLimits
    {
        std::size_t bytes{4u * 1024u * 1024u};   ///< Maximum document bytes.
        std::size_t depth{64};                   ///< Maximum element nesting depth.
        std::size_t nodes{100000};               ///< Maximum reader nodes.
        std::size_t attributes_per_element{128}; ///< Maximum attributes on one element.
        std::size_t text_bytes{1024u * 1024u};   ///< Maximum cumulative text bytes.
    };

    /// Deadline and cancellation state supplied to an asynchronous operation.
    struct CallOptions
    {
        std::chrono::steady_clock::time_point deadline; ///< Absolute monotonic deadline.
        CancellationToken cancellation{CancellationSource{}.token()}; ///< Cancellation view.
    };

    /// Convenience username/password credential value.
    struct Credentials
    {
        std::string username; ///< ONVIF username.
        std::string password; ///< Secret password; never include in diagnostics.
        bool allow_cleartext_basic{false}; ///< Permit HTTP Basic authentication without TLS.
    };

    /// Move-only secret material scoped to one operation and authentication sequence.
    class CredentialLease
    {
      public:
        CredentialLease(std::string username, std::string password, bool allow_cleartext_basic = false);
        ~CredentialLease();
        CredentialLease(CredentialLease&&) noexcept;
        CredentialLease& operator=(CredentialLease&&) noexcept;
        CredentialLease(const CredentialLease&) = delete;
        CredentialLease& operator=(const CredentialLease&) = delete;
        std::string_view username() const noexcept { return username_; }
        std::string_view password() const noexcept { return password_; }
        bool allow_cleartext_basic() const noexcept { return allow_cleartext_basic_; }
      private:
        void erase() noexcept;
        std::string username_;
        std::string password_;
        bool allow_cleartext_basic_{};
    };

    /// Asynchronous source of independent operation credential leases.
    class CredentialProvider
    {
      public:
        virtual ~CredentialProvider() = default;
        virtual Task<Result<CredentialLease>> acquire(CallOptions options) = 0;
        virtual void close() noexcept = 0;
    };

    std::shared_ptr<CredentialProvider> make_fixed_credential_provider(Credentials credentials);

    enum class EndpointPurpose
    {
        device, media, ptz, events, download, analytics, imaging
    };

    enum class EndpointProvenance
    {
        caller_supplied, device_advertised, redirect, replacement
    };

    /// Post-resolution context supplied before any socket is created.
    struct CandidateAuthorizationRequest
    {
        std::string original_url;
        std::string effective_url;
        std::string hostname;
        std::uint16_t port{};
        std::vector<std::string> resolved_addresses;
        std::string proposed_address;
        EndpointPurpose purpose{EndpointPurpose::device};
        EndpointProvenance provenance{EndpointProvenance::caller_supplied};
        std::string resolution_attempt_id;
        std::optional<OperationId> operation; ///< Operation metadata, when applicable.
        bool idempotent{true}; ///< Whether repeating the request is semantically safe.
        bool retry_allowed{false}; ///< Whether native retry policy may repeat the request.
    };

    /// Embedding-host authorization boundary for resolved connection candidates.
    class CandidateAuthorizer
    {
      public:
        virtual ~CandidateAuthorizer() = default;
        virtual Task<Result<void>> authorize(
            CandidateAuthorizationRequest request, CallOptions options
        ) = 0;
    };

    /// Configuration for the default HTTP transport.
    struct CurlTransportOptions
    {
        std::shared_ptr<Runtime> runtime;
        XmlLimits limits{};
        std::shared_ptr<CandidateAuthorizer> candidate_authorizer;
    };

    /// Owned HTTP request passed to HttpTransport.
    enum class HttpMethod
    {
        post, ///< Send the request body with HTTP POST.
        get   ///< Retrieve response bytes with HTTP GET.
    };

    /// Owned HTTP request passed to HttpTransport.
    struct HttpRequest
    {
        HttpRequest() = default;
        HttpRequest(
            std::string url_value, std::string body_value,
            std::vector<std::string> headers_value,
            std::chrono::steady_clock::time_point deadline_value,
            std::optional<Credentials> credentials_value = {},
            HttpMethod method_value = HttpMethod::post
        ) : url(std::move(url_value)), body(std::move(body_value)),
            headers(std::move(headers_value)), deadline(deadline_value),
            credentials(std::move(credentials_value)), method(method_value) {}
        std::string url;                                ///< Authorized destination URL.
        std::string body;                               ///< Request body bytes.
        std::vector<std::string> headers;               ///< Complete request header lines.
        std::chrono::steady_clock::time_point deadline; ///< Absolute transport deadline.
        std::optional<Credentials> credentials; ///< Compatibility credential value.
        HttpMethod method{HttpMethod::post};             ///< HTTP request method.
        std::shared_ptr<CredentialLease> credential_lease; ///< Operation-bounded authentication.
        std::string original_url;
        EndpointPurpose purpose{EndpointPurpose::device};
        EndpointProvenance provenance{EndpointProvenance::caller_supplied};
        std::optional<OperationId> operation; ///< Operation metadata, when applicable.
        bool idempotent{true}; ///< Whether repeating the request is semantically safe.
        bool retry_allowed{false}; ///< Whether native retry policy may repeat the request.
    };

    /// Bounded HTTP response returned by HttpTransport.
    struct HttpResponse
    {
        long status{};            ///< HTTP status code.
        std::string body;         ///< Response body bytes.
        std::string content_type; ///< Parsed Content-Type value.
    };

    /// Injectable asynchronous HTTP transport boundary.
    class HttpTransport
    {
      public:
        virtual ~HttpTransport() = default;
        /**
         * @brief Send one bounded HTTP request.
         * @param request Owned request including its deadline.
         * @param cancellation Cancellation state for the transfer.
         * @return Cold task producing an HTTP response or Error.
         */
        virtual Task<Result<HttpResponse>>
        send(HttpRequest request, CancellationToken cancellation) = 0;
    };

    /**
     * @brief Create the default libcurl HTTP transport.
     * @param runtime Runtime used for completion dispatch.
     * @param limits Response XML and byte limits.
     * @return Shared injectable transport.
     */
    std::shared_ptr<HttpTransport>
    make_curl_transport(std::shared_ptr<Runtime> runtime, XmlLimits limits = {});
    std::shared_ptr<HttpTransport> make_curl_transport(CurlTransportOptions options);

    /// Authorizes every endpoint before a transport connects to it.
    class EndpointPolicy
    {
      public:
        virtual ~EndpointPolicy() = default;
        /**
         * @brief Authorize a candidate endpoint.
         * @param url Candidate destination URL.
         * @param original_origin Origin of the caller-supplied Device URL.
         * @return Success when authorized, otherwise endpoint_rejected.
         */
        virtual Result<void>
        authorize(std::string_view url, std::string_view original_origin) const = 0;
    };

    /// Create a policy accepting the original origin and rejecting URL user-info.
    std::shared_ptr<EndpointPolicy> make_same_origin_policy();

    /**
     * @brief Create a policy accepting HTTP(S) endpoints on the original device host.
     *
     * Unlike make_same_origin_policy(), this permits a device to advertise services
     * on a different port while continuing to reject user-info and unrelated hosts.
     */
    std::shared_ptr<EndpointPolicy> make_same_host_policy();

    /// Bounded opaque HTTP payload, such as a camera snapshot.
    struct BinaryData
    {
        std::string bytes;        ///< Unmodified response body bytes.
        std::string content_type; ///< Server-provided media type.
    };

    /**
     * @brief Download bounded opaque bytes from an authorized endpoint.
     *
     * The transport's own response byte limit applies first and is the smaller
     * bound in practice: make_curl_transport() defaults to XmlLimits::bytes, so a
     * @p maximum_bytes above that value cannot be reached. Construct the transport
     * with matching limits when larger downloads are required.
     */
    Task<Result<BinaryData>> download_binary(
        std::string url,
        std::string original_origin,
        std::shared_ptr<HttpTransport> transport,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        std::optional<Credentials> credentials,
        std::size_t maximum_bytes,
        CallOptions options
    );

    /// Device identity returned by GetDeviceInformation.
    struct DeviceInformation
    {
        std::string manufacturer;     ///< Manufacturer name.
        std::string model;            ///< Product model.
        std::string firmware_version; ///< Firmware version string.
        std::string serial_number;    ///< Manufacturer serial number.
        std::string hardware_id;      ///< Hardware identifier.
    };

    /// How a device keeps its clock.
    enum class DateTimeType
    {
        manual, ///< Set explicitly with SetSystemDateAndTime.
        ntp     ///< Synchronized from the NTP servers in GetNTP/SetNTP.
    };

    /// Normalized UTC device time and the clock configuration behind it.
    struct SystemDateAndTime
    {
        std::chrono::system_clock::time_point utc; ///< Device UTC time.
        DateTimeType type{DateTimeType::manual};   ///< Current clock source.
        bool daylight_savings{};                   ///< Whether daylight saving is applied.
        std::string time_zone; ///< POSIX TZ string as reported; empty when absent.
    };

    /// Arguments for DeviceSession::set_system_date_and_time().
    struct SystemDateAndTimeSettings
    {
        DateTimeType type{DateTimeType::ntp}; ///< Clock source to use.
        bool daylight_savings{};              ///< Whether daylight saving is applied.
        /// POSIX TZ string such as `CET-1CEST,M3.5.0,M10.5.0/3`; omitted when unset,
        /// which leaves the device's time zone unchanged.
        std::optional<std::string> time_zone;
        /// UTC time to set. Required for DateTimeType::manual, rejected for NTP.
        std::optional<std::chrono::system_clock::time_point> utc;
    };

    /// Device NTP configuration (GetNTP/SetNTP).
    struct NtpConfiguration
    {
        bool from_dhcp{}; ///< Whether servers are taken from DHCP.
        /// Configured servers: IPv4 or IPv6 addresses, or DNS names.
        std::vector<std::string> servers;
        /// Servers learned from DHCP; reported only, never sent by set_ntp().
        std::vector<std::string> dhcp_servers;
    };

    /// Whether a device lets a scope be changed.
    enum class ScopeDefinition
    {
        fixed,       ///< Built into the device; SetScopes and RemoveScopes refuse it.
        configurable ///< Added by a client; may be replaced or removed.
    };

    /// One scope URI and its definition, from GetScopes.
    struct ScopeEntry
    {
        std::string uri;                                     ///< Scope URI.
        ScopeDefinition definition{ScopeDefinition::fixed}; ///< Fixed or configurable.
    };

    /// Advertised ONVIF service endpoint.
    struct Service
    {
        std::string namespace_uri; ///< Service namespace URI.
        std::string xaddr;         ///< Untrusted advertised endpoint URL.
    };

    /// Selected service capability endpoints.
    struct Capabilities
    {
        std::optional<std::string> media_xaddr;  ///< Advertised Media endpoint.
        std::optional<std::string> ptz_xaddr;    ///< Advertised PTZ endpoint.
        std::optional<std::string> events_xaddr; ///< Advertised Event endpoint.
        std::optional<std::string> analytics_xaddr; ///< Advertised Analytics endpoint.
        std::optional<std::string> imaging_xaddr;   ///< Advertised Imaging endpoint.
    };

    using ProfileToken = std::string; ///< Opaque Media profile token.
    using PresetToken = std::string;  ///< Opaque PTZ preset token.

    /// Named Media profile.
    struct MediaProfile
    {
        ProfileToken token; ///< Opaque profile token used by later operations.
        std::string name;   ///< Human-readable profile name.
        /// Token of the profile's video analytics configuration; empty when none.
        std::string video_analytics_configuration;
    };

    /// Width and height in pixels.
    struct Resolution
    {
        int width{};  ///< Width in pixels.
        int height{}; ///< Height in pixels.
        friend bool operator==(const Resolution&, const Resolution&) = default;
    };

    /// Inclusive integer range reported by a device.
    struct IntegerRange
    {
        int minimum{}; ///< Smallest accepted value.
        int maximum{}; ///< Largest accepted value.
    };

    /**
     * @brief A Media1 video encoder configuration (tt:VideoEncoderConfiguration).
     *
     * Every field is read back and sent again by
     * MediaClient::set_video_encoder_configuration(), so change the fields of a
     * configuration obtained from the device rather than building one from scratch.
     */
    struct VideoEncoderConfiguration
    {
        std::string token;        ///< Configuration token.
        std::string name;         ///< Configuration name.
        int use_count{};          ///< Number of profiles using it.
        std::string encoding;     ///< `H264`, `JPEG`, or `MPEG4`.
        Resolution resolution;    ///< Encoded size.
        double quality{};         ///< Device quality level; see VideoEncoderOptions.
        int frame_rate_limit{};   ///< Maximum frames per second.
        int encoding_interval{};  ///< Encode every Nth frame.
        int bitrate_limit{};      ///< Maximum bitrate in kbit/s.
        std::optional<int> gov_length; ///< Frames between key frames (H.264, MPEG-4).
        std::string profile;      ///< H.264 or MPEG-4 profile, such as `Main`.
        std::string multicast_address;    ///< Multicast group, or `0.0.0.0` when unused.
        int multicast_port{};             ///< Multicast port.
        int multicast_ttl{};              ///< Multicast time-to-live.
        bool multicast_auto_start{};      ///< Whether multicast starts without a client.
        std::string session_timeout;      ///< RTSP session timeout, an xs:duration.
    };

    /// A Media1 audio encoder configuration.
    struct AudioEncoderConfiguration
    {
        std::string token;    ///< Configuration token.
        std::string name;     ///< Configuration name.
        std::string encoding; ///< `G711`, `G726`, or `AAC`.
        int bitrate{};        ///< Bitrate as reported (kbit/s per ONVIF; some devices send bit/s).
        int sample_rate{};    ///< Sample rate as reported (kHz per ONVIF; some devices send Hz).
    };

    /// The part of a video source a profile uses.
    struct VideoSourceConfiguration
    {
        std::string token;        ///< Configuration token.
        std::string name;         ///< Configuration name.
        std::string source_token; ///< Video source token, as used by ImagingClient.
        int x{};                  ///< Left edge of the used area.
        int y{};                  ///< Top edge of the used area.
        int width{};              ///< Width of the used area.
        int height{};             ///< Height of the used area.
    };

    /// Everything a Media profile combines, from Media GetProfiles.
    struct ProfileConfiguration
    {
        ProfileToken token; ///< Profile token.
        std::string name;   ///< Profile name.
        bool fixed{};       ///< Whether the device prevents deleting the profile.
        std::optional<VideoSourceConfiguration> video_source;   ///< Video input, if any.
        std::optional<VideoEncoderConfiguration> video_encoder; ///< Video encoder, if any.
        std::string audio_source;                               ///< Audio source token, if any.
        std::optional<AudioEncoderConfiguration> audio_encoder; ///< Audio encoder, if any.
        std::string ptz_configuration;             ///< PTZ configuration token, if any.
        std::string video_analytics_configuration; ///< Analytics configuration token, if any.
    };

    /// What one encoding accepts, within VideoEncoderOptions.
    struct VideoEncodingOptions
    {
        std::string encoding;                ///< `H264`, `JPEG`, or `MPEG4`.
        std::vector<Resolution> resolutions; ///< Selectable sizes.
        IntegerRange frame_rate;             ///< Frame rate limit range.
        IntegerRange encoding_interval;      ///< Encoding interval range.
        std::optional<IntegerRange> gov_length; ///< GOP length range (H.264, MPEG-4).
        std::optional<IntegerRange> bitrate;    ///< Bitrate range in kbit/s, when reported.
        std::vector<std::string> profiles;      ///< Supported H.264 or MPEG-4 profiles.
    };

    /// What a video encoder configuration accepts.
    struct VideoEncoderOptions
    {
        IntegerRange quality;                         ///< Quality range.
        std::vector<VideoEncodingOptions> encodings;  ///< One entry per supported encoding.
    };

    /// A physical video input, from Media GetVideoSources.
    struct VideoSource
    {
        std::string token;   ///< Token used by the Imaging service.
        double framerate{};  ///< Native frame rate.
        int width{};         ///< Native width in pixels.
        int height{};        ///< Native height in pixels.
    };

    /**
     * @brief Image adjustments of one video source (tt:ImagingSettings20).
     *
     * Every field is optional: a device reports only what it supports, and
     * ImagingClient::set_settings() sends only the fields that are set.
     */
    struct ImagingSettings
    {
        std::optional<double> brightness;       ///< Device units; see ImagingOptions.
        std::optional<double> color_saturation; ///< Device units; see ImagingOptions.
        std::optional<double> contrast;         ///< Device units; see ImagingOptions.
        std::optional<double> sharpness;        ///< Device units; see ImagingOptions.
        std::optional<std::string> ir_cut_filter;          ///< `ON`, `OFF`, or `AUTO`.
        std::optional<std::string> backlight_compensation; ///< Mode, `ON` or `OFF`.
        std::optional<std::string> wide_dynamic_range;     ///< Mode, `ON` or `OFF`.
        std::optional<std::string> white_balance;          ///< Mode, `AUTO` or `MANUAL`.
    };

    /// Inclusive range of a numeric imaging setting.
    struct ImagingRange
    {
        double minimum{}; ///< Smallest accepted value.
        double maximum{}; ///< Largest accepted value.
    };

    /// What a video source lets ImagingClient::set_settings() change.
    struct ImagingOptions
    {
        std::optional<ImagingRange> brightness;       ///< Absent when unsupported.
        std::optional<ImagingRange> color_saturation; ///< Absent when unsupported.
        std::optional<ImagingRange> contrast;         ///< Absent when unsupported.
        std::optional<ImagingRange> sharpness;        ///< Absent when unsupported.
        std::vector<std::string> ir_cut_filter_modes;          ///< Empty when unsupported.
        std::vector<std::string> backlight_compensation_modes; ///< Empty when unsupported.
        std::vector<std::string> wide_dynamic_range_modes;     ///< Empty when unsupported.
        std::vector<std::string> white_balance_modes;          ///< Empty when unsupported.
    };

    /// Device-supplied stream or snapshot URI and its validity metadata.
    struct MediaUri
    {
        std::string uri;                   ///< Untrusted media URI.
        std::chrono::seconds valid_for{};  ///< Advertised lifetime, or zero if unspecified.
        bool invalid_after_connect{false}; ///< URI expires after the first connection.
    };

    /// Native numeric range advertised for one PTZ axis.
    struct NativeRange
    {
        double minimum{-1}; ///< Inclusive native minimum.
        double maximum{1};  ///< Inclusive native maximum.
    };

    /// ONVIF two-axis coordinate space and its device-native ranges.
    struct PtzSpace2D
    {
        std::string uri; ///< Space URI sent back with PTZ vectors when non-empty.
        NativeRange x;  ///< Pan/translation X range.
        NativeRange y;  ///< Tilt/translation Y range.
    };

    /// ONVIF one-axis coordinate space and its device-native range.
    struct PtzSpace1D
    {
        std::string uri; ///< Space URI sent back with PTZ vectors when non-empty.
        NativeRange x;  ///< Zoom range.
    };

    /// Native ranges for the pan, tilt, and zoom axes.
    struct PtzSpaces
    {
        PtzSpaces() = default;
        /// Construct legacy convenience ranges; detailed spaces remain unknown.
        PtzSpaces(NativeRange pan_value, NativeRange tilt_value, NativeRange zoom_value)
            : pan(pan_value), tilt(tilt_value), zoom(zoom_value)
        {
        }
        NativeRange pan;  ///< Pan axis range.
        NativeRange tilt; ///< Tilt axis range.
        NativeRange zoom; ///< Zoom axis range.
        std::vector<PtzSpace2D> absolute_pan_tilt; ///< Absolute position spaces.
        std::vector<PtzSpace1D> absolute_zoom; ///< Absolute zoom spaces.
        std::vector<PtzSpace2D> relative_pan_tilt; ///< Relative translation spaces.
        std::vector<PtzSpace1D> relative_zoom; ///< Relative zoom spaces.
        std::vector<PtzSpace2D> continuous_pan_tilt; ///< Continuous velocity spaces.
        std::vector<PtzSpace1D> continuous_zoom; ///< Continuous zoom spaces.
    };

    /// Optional normalized PTZ components in the interval [-1, +1].
    struct PtzVector
    {
        std::optional<double> pan;  ///< Pan component, or omitted.
        std::optional<double> tilt; ///< Tilt component, or omitted.
        std::optional<double> zoom; ///< Zoom component, or omitted.
    };

    /// Current normalized PTZ position and movement state.
    struct PtzStatus
    {
        PtzVector position; ///< Last reported normalized position.
        bool moving{false}; ///< True when any axis reports movement.
        std::optional<bool> pan_tilt_moving; ///< Per-axis-group status when advertised.
        std::optional<bool> zoom_moving; ///< Zoom status when advertised.
        bool position_reliable{true}; ///< False when position could not be normalized.
    };

    /// Named PTZ preset.
    struct Preset
    {
        PresetToken token;  ///< Opaque preset token.
        std::string name;   ///< Human-readable preset name.
        PtzVector position; ///< Normalized preset position when advertised.
    };

    /// Outcome metadata for a movement request.
    struct MoveResult
    {
        bool request_may_have_been_applied{false}; ///< Delivery outcome is uncertain.
        std::optional<Error> stop_error;           ///< Failure of a best-effort safety Stop.
        std::optional<SafetyActionDetail> safety_action; ///< Safety Stop outcome, when attempted.
    };

    /// Select whether velocity ramps are produced by the host or camera firmware.
    enum class PtzRampMode
    {
        host,    ///< Emit intermediate velocity commands using ramp_up/ramp_down.
        firmware ///< Emit target velocities directly and let the camera ramp.
    };

    /// Normalized rectangular pan/tilt region which manual motion may not enter.
    struct PtzKeepOutZone
    {
        double minimum_pan{-1};  ///< Inclusive normalized left boundary.
        double maximum_pan{1};   ///< Inclusive normalized right boundary.
        double minimum_tilt{-1}; ///< Inclusive normalized lower boundary.
        double maximum_tilt{1};  ///< Inclusive normalized upper boundary.
    };

    /// One point in a calibrated zoom-position to pan/tilt-speed scale curve.
    struct PtzZoomSpeedPoint
    {
        double zoom{-1}; ///< Normalized zoom position in ascending order.
        double scale{1}; ///< Pan/tilt multiplier at this zoom position.
    };

    /// Clockwise orientation of displayed video relative to camera coordinates.
    enum class ViewportRotation { degrees_0, degrees_90, degrees_180, degrees_270 };

    /// Geometry and camera-specific gains used for video-relative PTZ operations.
    /// Direction-specific click-to-center gains measured at one zoom position.
    struct ViewportCalibrationPoint
    {
        double zoom{};            ///< Normalized zoom position in [-1,+1].
        double pan_left_gain{0.5};///< Relative translation at the left image edge.
        double pan_right_gain{0.5};///< Relative translation at the right image edge.
        double tilt_up_gain{0.5}; ///< Relative translation at the top image edge.
        double tilt_down_gain{0.5};///< Relative translation at the bottom image edge.
    };

    /// Version-independent camera and displayed-video geometry used by ViewportMapper.
    struct ViewportCalibration
    {
        double content_x{};      ///< Left edge of video content as viewport fraction.
        double content_y{};      ///< Top edge of video content as viewport fraction.
        double content_width{1}; ///< Video width as viewport fraction (excludes letterbox).
        double content_height{1};///< Video height as viewport fraction (excludes letterbox).
        ViewportRotation rotation{ViewportRotation::degrees_0}; ///< Display rotation.
        bool mirror_horizontal{}; ///< Display is horizontally mirrored.
        bool mirror_vertical{};   ///< Display is vertically mirrored.
        double pan_gain{0.5};     ///< Relative pan translation at a horizontal edge.
        double tilt_gain{0.5};    ///< Relative tilt translation at a vertical edge.
        double box_zoom_gain{0.8};///< Zoom translation for a point-sized box.
        double maximum_translation{0.5}; ///< Per-operation movement safety bound.
        bool calibrated{};        ///< False selects conservative approximate behavior.
        std::chrono::milliseconds maximum_age{std::chrono::hours(24)}; ///< Calibration freshness.
        std::vector<ViewportCalibrationPoint> zoom_points; ///< Ordered direction-specific samples.
    };

    /// Pixel-space pointer or touch location and the viewport dimensions it belongs to.
    struct ViewportPoint
    {
        double x{};
        double y{};
        double width{};
        double height{};
    };

    /// Bounded relative movement produced from video geometry.
    struct ViewportMove
    {
        PtzVector translation; ///< Relative normalized pan/tilt/zoom translation.
        bool calibrated{};     ///< True only when camera-specific calibration was used.
    };

    /// Deterministic mapping for click-to-center and box-zoom operations.
    class ViewportMapper
    {
      public:
        explicit ViewportMapper(ViewportCalibration calibration = {});
        Result<ViewportMove> click_to_center(
            ViewportPoint point,
            std::chrono::milliseconds calibration_age = std::chrono::milliseconds{0},
            std::optional<double> normalized_zoom = std::nullopt
        ) const;
        Result<ViewportMove> box_zoom(
            ViewportPoint first,
            ViewportPoint second,
            std::chrono::milliseconds calibration_age = std::chrono::milliseconds{0},
            std::optional<double> normalized_zoom = std::nullopt
        ) const;

      private:
        ViewportCalibration calibration_;
    };

    enum class PtzArrival { moving, settled, timed_out, degraded };

    /// Deterministic arrival/settling detector for bounded automatic operations.
    class PtzSettlingDetector
    {
      public:
        PtzSettlingDetector(
            std::chrono::milliseconds stable_duration = std::chrono::milliseconds{300},
            std::chrono::milliseconds timeout = std::chrono::seconds{8},
            double position_epsilon = 0.01
        );
        PtzArrival update(std::chrono::milliseconds elapsed, const PtzStatus& status);
        void reset() noexcept;

      private:
        std::chrono::milliseconds stable_duration_;
        std::chrono::milliseconds timeout_;
        double epsilon_;
        std::chrono::milliseconds total_{};
        std::chrono::milliseconds stable_{};
        std::optional<PtzVector> previous_;
    };

    /// Tunable, device-independent manual PTZ control policy.
    struct PtzControllerConfig
    {
        std::chrono::milliseconds tick_interval{20}; ///< Internal control-law interval.
        std::chrono::milliseconds command_interval{100}; ///< Minimum device update interval.
        std::chrono::milliseconds command_refresh{500}; ///< Resend active velocity; zero disables.
        std::chrono::milliseconds status_interval{1000}; ///< Status polling interval.
        std::chrono::milliseconds ramp_up{300}; ///< Time from zero to full-scale velocity.
        std::chrono::milliseconds ramp_down{200}; ///< Time from full-scale velocity to zero.
        std::chrono::milliseconds input_watchdog{750}; ///< Stop after input lease expires.
        std::chrono::milliseconds maximum_run{30000}; ///< Stop an uninterrupted manual run.
        double dead_zone{0.08}; ///< Magnitudes at or below this value become zero.
        double response_exponent{1.5}; ///< Exponential response after the dead zone.
        double maximum_pan{0.6}; ///< Maximum normalized pan velocity.
        double maximum_tilt{0.6}; ///< Maximum normalized tilt velocity.
        double maximum_zoom{0.5}; ///< Maximum normalized zoom velocity.
        double minimum_effective_speed{0.04}; ///< Smallest nonzero output magnitude.
        double command_epsilon{0.01}; ///< Minimum change worth transmitting.
        double zoomed_pan_tilt_scale{0.18}; ///< Pan/tilt scale at normalized zoom +1.
        PtzRampMode ramp_mode{PtzRampMode::host}; ///< Host or camera-firmware ramping.
        std::optional<NativeRange> pan_limits; ///< Optional normalized pan soft limits.
        std::optional<NativeRange> tilt_limits; ///< Optional normalized tilt soft limits.
        std::optional<NativeRange> zoom_limits; ///< Optional normalized zoom soft limits.
        std::vector<PtzKeepOutZone> keep_out_zones; ///< Forbidden normalized regions.
        std::vector<PtzZoomSpeedPoint> zoom_speed_curve; ///< Calibrated zoom scaling curve.
        bool invert_pan{false}; ///< Reverse the pan input axis.
        bool invert_tilt{false}; ///< Reverse the tilt input axis.
        bool invert_zoom{false}; ///< Reverse the zoom input axis.
    };

    /// Observable state of a PtzController.
    enum class PtzControllerMode
    {
        idle,    ///< No motion intent and the camera has been stopped.
        manual,  ///< Manual input owns motion.
        stopping,///< Velocity is ramping down or Stop is in flight.
        degraded,///< A device operation failed; Stop remains available.
        closed   ///< The controller accepts no further input.
    };

    /// Snapshot suitable for UI telemetry and diagnostics.
    struct PtzControllerState
    {
        PtzControllerMode mode{PtzControllerMode::idle}; ///< Current state-machine mode.
        PtzVector desired; ///< Shaped target velocity.
        PtzVector commanded; ///< Latest velocity calculated by the control law.
        std::optional<PtzStatus> reported; ///< Most recent device status.
        std::optional<Error> last_error; ///< Most recent controller/device error.
        std::uint64_t accepted_sequence{}; ///< Latest accepted owner input sequence.
        std::size_t coalesced_inputs{}; ///< Inputs superseded before transmission.
        std::size_t commands_sent{}; ///< ContinuousMove requests issued.
        std::size_t stops_sent{}; ///< Stop requests issued.
        std::chrono::microseconds last_input_to_command{}; ///< Latest accepted-input latency.
        std::chrono::microseconds last_request_latency{}; ///< Latest device request duration.
        std::size_t peak_pending_inputs{}; ///< Maximum latest-intent queue depth (at most one).
    };

    /**
     * @brief Deterministic PTZ input shaping and acceleration model.
     *
     * advance() depends only on its arguments and retained model state, allowing
     * exact tests with an injected duration instead of wall-clock sleeps.
     */
    class PtzControlLaw
    {
      public:
        explicit PtzControlLaw(PtzControllerConfig config = {});
        /// Set the newest raw normalized operator intent.
        Result<void> set_input(PtzVector input);
        /// Release all input axes and begin configured deceleration.
        void release() noexcept;
        /// Advance by @p elapsed using the latest normalized zoom position.
        PtzVector advance(std::chrono::milliseconds elapsed, double normalized_zoom);
        /// Advance using the complete reported position for limits and zoom scaling.
        PtzVector advance(std::chrono::milliseconds elapsed, PtzVector reported_position);
        /// Return the shaped target before ramping.
        PtzVector desired() const;
        /// Return the current ramped velocity.
        PtzVector commanded() const;
        /// Immediately clear target and commanded velocity.
        void reset() noexcept;

      private:
        PtzControllerConfig config_;
        PtzVector input_;
        PtzVector desired_;
        PtzVector commanded_;
        bool configured_{true};
    };

    /**
     * @brief Convert a native PTZ coordinate to [-1, +1].
     * @param native_value Value in the advertised native range.
     * @param range Advertised native range.
     * @return Normalized value, or invalid_argument for invalid input or ranges.
     */
    Result<double> normalize_ptz(double native_value, NativeRange range);

    /**
     * @brief Convert a normalized PTZ coordinate to its native range.
     * @param normalized_value Value in [-1, +1].
     * @param range Advertised native range.
     * @return Native value, or invalid_argument for invalid input or ranges.
     */
    Result<double> denormalize_ptz(double normalized_value, NativeRange range);

    /// Normalized event notification.
    struct Event
    {
        std::string topic;                              ///< ONVIF topic expression.
        std::chrono::system_clock::time_point utc_time; ///< Device event timestamp.
        std::string source;                             ///< Normalized event source data.
        std::string data;                               ///< Normalized event payload data.
        std::uint64_t sequence{};                       ///< Local or device sequence number.
    };

    /// One event topic a device can emit, from GetEventProperties.
    struct EventTopic
    {
        /// Topic expression usable as a subscription filter, such as
        /// `tns1:RuleEngine/CellMotionDetector/Motion`.
        std::string topic;
        /// Property events report a state (initialized, changed, deleted) rather
        /// than a one-off occurrence.
        bool property{};
        std::vector<std::string> source; ///< Names of the Source simple items.
        std::vector<std::string> data;   ///< Names of the Data simple items.
    };

    /// Observable managed-stream continuity condition.
    enum class StreamHealth
    {
        possible_event_loss, ///< Reconnect or resubscribe may have lost events.
        events_dropped       ///< Managed queue discarded old events at its bound.
    };
    using EventOrHealth = std::variant<Event, StreamHealth>; ///< Stream output item.

    /// Opaque backend identity and timing returned by PullPoint creation or renewal.
    struct PullPointSubscriptionInfo
    {
        std::string token; ///< Backend-owned opaque subscription identity.
        std::string endpoint; ///< Authorized subscription endpoint, when network-backed.
        std::chrono::system_clock::time_point termination_time; ///< Advertised expiry.
    };

    /// Type of WS-Discovery observation.
    enum class DiscoveryKind
    {
        probe_match, ///< Device matched an active Probe.
        hello,       ///< Device announced its availability.
        bye          ///< Device announced its departure.
    };

    /// One bounded, untrusted WS-Discovery observation.
    struct DiscoveryObservation
    {
        DiscoveryKind kind{DiscoveryKind::probe_match}; ///< Observation kind.
        std::string endpoint_reference;                 ///< Device endpoint reference UUID/URI.
        std::string source;                             ///< Datagram source address.
        std::string interface_id;                       ///< Receiving interface identifier.
        std::vector<std::string> scopes;                ///< Advertised scope URIs.
        std::vector<std::string> xaddrs;                ///< Untrusted advertised service URLs.
        std::uint64_t message_number{}; ///< WS-Discovery application message number.
    };

    /// Probe filter variants used for ONVIF interoperability.
    enum class DiscoveryProbeType
    {
        any,                      ///< Omit Types and accept any discoverable target service.
        device,                   ///< Request the current ONVIF tds:Device type.
        network_video_transmitter ///< Request the legacy dn:NetworkVideoTransmitter type.
    };

    /// Strength of evidence that an address hosts an ONVIF Device service.
    enum class DeviceEvidence
    {
        discovery,                ///< A valid WS-Discovery ProbeMatch was received.
        authentication_challenge, ///< The Device service returned HTTP 401 or 403.
        soap_fault,               ///< The candidate returned a SOAP Fault.
        onvif_response            ///< The candidate returned an ONVIF operation response.
    };

    /// Aggregated evidence for one discovered or directly inspected device.
    struct DeviceCandidate
    {
        std::string address;                       ///< IPv4 address associated with the candidate.
        std::optional<std::string> device_service; ///< Candidate Device service URL.
        DeviceEvidence evidence{DeviceEvidence::discovery}; ///< Strongest observed evidence.
        std::optional<DiscoveryObservation> discovery;      ///< ProbeMatch evidence when available.
        std::string diagnostic; ///< Safe summary of the successful detection method.
    };

    /// Active discovery operation represented by a progress notification.
    enum class DiscoveryProgressStage
    {
        ws_discovery, ///< Sending and receiving WS-Discovery messages.
        direct_probe  ///< Inspecting Device service candidates over HTTP or HTTPS.
    };

    /// Snapshot emitted while a bounded discovery operation is running.
    struct DiscoveryProgress
    {
        DiscoveryProgressStage stage{DiscoveryProgressStage::ws_discovery}; ///< Current method.
        std::size_t completed{}; ///< Completed interfaces or addresses.
        std::size_t total{};     ///< Total interfaces or addresses in this method.
        std::string current;     ///< Current interface or address.
    };

    /**
     * @brief Policy and resource bounds for active device discovery.
     *
     * Network ranges are never inferred or scanned unless scan_networks contains
     * explicit CIDR values. Use local_ipv4_networks() to offer an application-level
     * `auto` choice.
     */
    struct NetworkDiscoveryOptions
    {
        bool multicast{true};                ///< Send multicast WS-Discovery Probes.
        bool direct_probe{false};            ///< Probe common Device service HTTP endpoints.
        std::vector<std::string> interfaces; ///< IPv4 interface addresses; empty means enumerate.
        std::vector<std::string> targets; ///< Explicit IPv4 addresses for unicast/direct probing.
        std::vector<std::string> scan_networks; ///< Explicit IPv4 CIDRs to inspect.
        std::vector<DiscoveryProbeType> probe_types{
            DiscoveryProbeType::any,
            DiscoveryProbeType::device,
            DiscoveryProbeType::network_video_transmitter
        }; ///< Probe compatibility variants.
        /// Direct-probe ports; 2020 is used by some consumer cameras.
        std::vector<std::uint16_t> ports{80, 443, 2020, 8000, 8080, 8443, 8899};
        std::vector<std::string> device_paths{"/onvif/device_service"};    ///< Direct paths.
        std::size_t retransmissions{3};   ///< Copies of each Probe using one MessageID.
        std::size_t maximum_hosts{1024};  ///< Hard bound for expanded scan addresses.
        std::size_t maximum_results{256}; ///< Hard bound for returned candidates.
        std::size_t maximum_concurrency{16}; ///< Maximum simultaneous direct HTTP probes.
        std::chrono::milliseconds initial_retry_delay{50}; ///< Retry backoff base.
        std::chrono::milliseconds listen_duration{3000};   ///< ProbeMatch receive window.
        std::chrono::milliseconds connect_timeout{500};    ///< Direct connection timeout.
        /// Direct SOAP timeout. An address's ports and paths are probed
        /// concurrently, so this is also the time budget for one address.
        std::chrono::milliseconds request_timeout{2000};
        /**
         * Optional progress callback invoked on the discovery worker thread.
         * Exceptions thrown by the callback are ignored.
         */
        std::function<void(const DiscoveryProgress&)> progress;
        std::shared_ptr<class DiscoveryAuthorizer> authorizer; ///< Socket-action authorization.
        bool require_authorization{false}; ///< Reject network actions when no authorizer exists.
    };

    /// Native network action requested by device discovery.
    enum class DiscoveryNetworkAction
    {
        enumerate_interfaces,
        bind_udp,
        join_multicast,
        send_multicast,
        send_unicast,
        scan_network,
        connect_tcp
    };

    /// Exact socket or network-range action proposed by discovery.
    struct DiscoveryAuthorizationRequest
    {
        DiscoveryNetworkAction action{DiscoveryNetworkAction::enumerate_interfaces};
        std::string interface_address;
        std::string destination;
        std::uint16_t port{};
        std::optional<std::string> network;
    };

    /// Authorization boundary invoked immediately before discovery network actions.
    class DiscoveryAuthorizer
    {
      public:
        virtual ~DiscoveryAuthorizer() = default;
        virtual Task<Result<void>> authorize(
            DiscoveryAuthorizationRequest request, CallOptions options
        ) = 0;
    };

    /**
     * @brief Enumerate directly connected, non-loopback IPv4 networks.
     * @param maximum_hosts Maximum address count permitted in any returned network.
     * @return CIDR strings suitable for an explicit NetworkDiscoveryOptions scan.
     */
    Result<std::vector<std::string>> local_ipv4_networks(std::size_t maximum_hosts = 1024);

    /**
     * @brief Execute bounded multicast, unicast, and optional direct discovery.
     * @param runtime Runtime used to dispatch the final completion.
     * @param endpoint_policy Policy authorizing direct HTTP candidates.
     * @param options Discovery methods and resource bounds.
     * @param call Deadline and cancellation state.
     * @return Cold task producing deduplicated candidates ordered by address.
     */
    Task<Result<std::vector<DeviceCandidate>>> discover_devices(
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        NetworkDiscoveryOptions options,
        CallOptions call
    );

    /// Discovery results plus the direct-probe coverage actually achieved.
    struct DiscoveryReport
    {
        std::vector<DeviceCandidate> devices; ///< Deduplicated candidates ordered by address.
        /// Addresses never directly probed because the deadline came first.
        std::vector<std::string> unchecked;
        /// Addresses whose probe the deadline cut short, so absence is unproven.
        std::vector<std::string> incomplete;
    };

    /**
     * @brief Execute discover_devices() and report which addresses it covered.
     *
     * An empty result only means "no device" for addresses that appear in
     * neither DiscoveryReport::unchecked nor DiscoveryReport::incomplete.
     */
    Task<Result<DiscoveryReport>> discover_network(
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        NetworkDiscoveryOptions options,
        CallOptions call
    );

    /**
     * @brief Worst-case duration of discovery with @p options.
     *
     * Covers WS-Discovery listening plus every target and scan address taking
     * its full request_timeout, so a deadline this far away lets the whole
     * range be probed. Authorizer latency is not included.
     * @return The bound, or invalid_argument for a malformed scan network.
     */
    Result<std::chrono::milliseconds> discovery_time_bound(const NetworkDiscoveryOptions& options);

    /// Which advertised service a DeviceSession::call() request addresses.
    enum class OnvifService { device, media, ptz, events, analytics, imaging };

    /// One stop of a preset tour.
    struct PresetTourSpot
    {
        PresetToken preset;                 ///< Preset to visit.
        std::chrono::seconds stay_time{};   ///< Time to stay; zero leaves it to the device.
    };

    /// Whether a preset tour is running.
    enum class PresetTourState { idle, touring, paused, unknown };

    /// A sequence of presets the device visits on its own (tt:PresetTour).
    struct PresetTour
    {
        std::string token;                    ///< Tour token.
        std::string name;                     ///< Tour name.
        PresetTourState state{PresetTourState::unknown}; ///< Reported state.
        bool auto_start{};                    ///< Whether the device starts it by itself.
        std::vector<PresetTourSpot> spots;    ///< Stops in visiting order.
    };

    /// Preset tour control action.
    enum class PresetTourOperation { start, stop, pause };

    /**
     * @brief Image orientation of a PTZ configuration (tt:PTControlDirection).
     *
     * EFlip turns the image and the pan/tilt directions upside down for ceiling
     * mounting; Reverse inverts the pan/tilt directions only.
     */
    struct PtzDirection
    {
        std::optional<std::string> eflip;   ///< `OFF`, `ON`, or `Extended`; absent if unsupported.
        std::optional<std::string> reverse; ///< `OFF`, `ON`, or `AUTO`; absent if unsupported.
    };

    /// A profile's PTZ configuration and the direction modes it accepts.
    struct PtzConfiguration
    {
        std::string token;      ///< Configuration token.
        std::string name;       ///< Configuration name.
        std::string node_token; ///< PTZ node the configuration drives.
        std::chrono::seconds default_timeout{}; ///< Default continuous-move timeout.
        PtzDirection direction;                  ///< Current orientation.
        std::vector<std::string> eflip_modes;    ///< Accepted EFlip modes.
        std::vector<std::string> reverse_modes;  ///< Accepted Reverse modes.
    };

    /// Named parameter value of an analytics module or rule (tt:SimpleItem).
    struct SimpleItem
    {
        std::string name;  ///< Parameter name, such as `Sensitivity`.
        std::string value; ///< Parameter value as text.
    };

    /**
     * @brief Structured analytics parameter (tt:ElementItem), such as a cell layout.
     *
     * The markup is kept exactly as the device supplied it and sent back unchanged
     * when the owning module or rule is modified.
     */
    struct ElementItem
    {
        std::string name; ///< Parameter name, such as `Layout`.
        std::string xml;  ///< Device-supplied inner markup.
    };

    /// An analytics module or rule: a named, typed set of parameters (tt:Config).
    struct AnalyticsConfig
    {
        std::string name; ///< Instance name, such as `MyCellMotionModule`.
        std::string type; ///< Qualified type, such as `tt:CellMotionEngine`.
        std::vector<SimpleItem> simple_items;   ///< Scalar parameters.
        std::vector<ElementItem> element_items; ///< Structured parameters.
    };

    /**
     * @brief Injectable backend for Media, PTZ, and Event service behavior.
     *
     * VirtualDeviceTransport implements this boundary for deterministic tests.
     */
    class ServiceBackend
    {
      public:
        virtual ~ServiceBackend() = default;
        /// Return all configured Media profiles.
        virtual Result<std::vector<MediaProfile>> profiles() = 0;
        /// Return a stream URI for @p profile.
        virtual Result<MediaUri> stream_uri(const ProfileToken& profile) = 0;
        /// Return a snapshot URI for @p profile.
        virtual Result<MediaUri> snapshot_uri(const ProfileToken& profile) = 0;
        /// Return native PTZ spaces for @p profile.
        virtual Result<PtzSpaces> ptz_spaces(const ProfileToken& profile) = 0;
        /// Return current PTZ status for @p profile.
        virtual Result<PtzStatus> ptz_status(const ProfileToken& profile) = 0;
        /// Apply @p vector as a relative or continuous movement.
        virtual Result<MoveResult>
        move(const ProfileToken& profile, PtzVector vector, bool relative) = 0;
        /// Apply an absolute position and optional normalized speed.
        virtual Result<MoveResult> absolute_move(
            const ProfileToken&, PtzVector, std::optional<PtzVector>
        )
        {
            return Error{
                ErrorCategory::unsupported,
                OperationId::absolute_move,
                "absolute PTZ movement is unsupported"
            };
        }
        /// Stop movement for @p profile.
        virtual Result<void> stop(const ProfileToken& profile) = 0;
        /// Return presets for @p profile.
        virtual Result<std::vector<Preset>> presets(const ProfileToken& profile) = 0;
        /// Move @p profile to @p preset.
        virtual Result<void>
        goto_preset(const ProfileToken& profile, const PresetToken& preset) = 0;
        /// Remove and return at most @p maximum queued events.
        virtual Result<std::vector<Event>> pull_events(std::size_t maximum) = 0;
        /// Renew an active Event subscription when applicable.
        virtual Result<void> renew_events() = 0;
        /// Close an active Event subscription when applicable.
        virtual Result<void> close_events() = 0;
        /// Create an independent PullPoint subscription.
        virtual Result<PullPointSubscriptionInfo>
        create_event_subscription(std::chrono::seconds requested_lifetime);
        /// Pull messages from one explicit subscription.
        virtual Result<std::vector<Event>> pull_event_subscription(
            const std::string& token,
            std::chrono::milliseconds server_timeout,
            std::size_t maximum
        );
        /// Renew one explicit subscription and return its new timing.
        virtual Result<PullPointSubscriptionInfo> renew_event_subscription(
            const std::string& token, std::chrono::seconds requested_lifetime
        );
        /// Unsubscribe one explicit subscription. Idempotent after success.
        virtual Result<void> close_event_subscription(const std::string& token);
        /// Create a subscription delivering only @p topics; empty means every topic.
        virtual Result<PullPointSubscriptionInfo> create_filtered_event_subscription(
            std::chrono::seconds requested_lifetime, const std::vector<std::string>& topics
        );
        /// Return the event topics the device can emit.
        virtual Result<std::vector<EventTopic>> event_topics();
        /// Return the analytics modules of a video analytics configuration.
        virtual Result<std::vector<AnalyticsConfig>> analytics_modules(const std::string& configuration);
        /// Replace the parameters of existing analytics modules, matched by name.
        virtual Result<void> modify_analytics_modules(
            const std::string& configuration, const std::vector<AnalyticsConfig>& modules
        );
        /// Return the analytics rules of a video analytics configuration.
        virtual Result<std::vector<AnalyticsConfig>> analytics_rules(const std::string& configuration);
        /// Replace the parameters of existing analytics rules, matched by name.
        virtual Result<void> modify_analytics_rules(
            const std::string& configuration, const std::vector<AnalyticsConfig>& rules
        );
        /// Return the device's video sources.
        virtual Result<std::vector<VideoSource>> video_sources();
        /// Return the imaging settings of @p source.
        virtual Result<ImagingSettings> imaging_settings(const std::string& source);
        /// Apply the set fields of @p settings to @p source.
        virtual Result<void> set_imaging_settings(
            const std::string& source, const ImagingSettings& settings, bool persist
        );
        /// Return the ranges and modes @p source accepts.
        virtual Result<ImagingOptions> imaging_options(const std::string& source);
        /// Return every profile with its full configuration.
        virtual Result<std::vector<ProfileConfiguration>> profile_configurations();
        /// Return what video encoder configuration @p configuration accepts.
        virtual Result<VideoEncoderOptions> video_encoder_options(const std::string& configuration);
        /// Replace a video encoder configuration.
        virtual Result<void>
        set_video_encoder_configuration(const VideoEncoderConfiguration& configuration, bool persist);
        /// Save the current position as preset @p name, or overwrite @p token when given.
        virtual Result<PresetToken>
        set_preset(const ProfileToken& profile, const std::string& name, const PresetToken& token);
        /// Delete preset @p token.
        virtual Result<void> remove_preset(const ProfileToken& profile, const PresetToken& token);
        /// Return the preset tours of @p profile.
        virtual Result<std::vector<PresetTour>> preset_tours(const ProfileToken& profile);
        /// Create an empty preset tour and return its token.
        virtual Result<std::string> create_preset_tour(const ProfileToken& profile);
        /// Replace the name, auto start, and spots of an existing tour.
        virtual Result<void> modify_preset_tour(const ProfileToken& profile, const PresetTour& tour);
        /// Start, stop, or pause tour @p token.
        virtual Result<void> operate_preset_tour(
            const ProfileToken& profile, const std::string& token, PresetTourOperation operation
        );
        /// Delete tour @p token.
        virtual Result<void> remove_preset_tour(const ProfileToken& profile, const std::string& token);
        /// Return the PTZ configuration of @p profile with its direction options.
        virtual Result<PtzConfiguration> ptz_configuration(const ProfileToken& profile);
        /// Change the set fields of @p direction in the PTZ configuration of @p profile.
        virtual Result<void>
        set_ptz_direction(const ProfileToken& profile, const PtzDirection& direction, bool persist);
        /// Send caller-composed Body markup to @p service; see DeviceSession::call().
        virtual Result<std::string>
        call(OnvifService service, const std::string& body, const std::string& response_name);
    };

    /// Domain-oriented Imaging client for brightness, contrast, and similar settings.
    class ImagingClient
    {
      public:
        /// Bind an Imaging client to a runtime and service backend.
        ImagingClient(std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend);
        /// Return the current settings of @p source (see MediaClient::get_video_sources()).
        Task<Result<ImagingSettings>> get_settings(std::string source, CallOptions options);
        /**
         * @brief Apply the set fields of @p settings to @p source.
         *
         * Numeric values must be finite. With @p persist the device keeps the
         * change across reboots.
         */
        Task<Result<void>> set_settings(
            std::string source, ImagingSettings settings, bool persist, CallOptions options
        );
        /// Return the ranges and modes @p source accepts.
        Task<Result<ImagingOptions>> get_options(std::string source, CallOptions options);

      private:
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<ServiceBackend> backend_;
    };

    /// Domain-oriented Analytics client for detection modules and rules.
    class AnalyticsClient
    {
      public:
        /// Bind an Analytics client to a runtime and service backend.
        AnalyticsClient(std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend);
        /**
         * @brief Return the modules of @p configuration (see
         * MediaProfile::video_analytics_configuration), such as motion detection.
         */
        Task<Result<std::vector<AnalyticsConfig>>>
        get_modules(std::string configuration, CallOptions options);
        /// Modify existing modules; each is matched by name and sent in full.
        Task<Result<void>> modify_modules(
            std::string configuration, std::vector<AnalyticsConfig> modules, CallOptions options
        );
        /// Return the rules of @p configuration, which raise events from module output.
        Task<Result<std::vector<AnalyticsConfig>>>
        get_rules(std::string configuration, CallOptions options);
        /// Modify existing rules; each is matched by name and sent in full.
        Task<Result<void>> modify_rules(
            std::string configuration, std::vector<AnalyticsConfig> rules, CallOptions options
        );

      private:
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<ServiceBackend> backend_;
    };

    /// Domain-oriented Media1 client.
    class MediaClient
    {
      public:
        /// Bind a Media client to a runtime and service backend.
        MediaClient(std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend);
        /// Return configured Media profiles.
        Task<Result<std::vector<MediaProfile>>> get_profiles(CallOptions options);
        /// Return a stream URI for @p profile.
        Task<Result<MediaUri>> get_stream_uri(ProfileToken profile, CallOptions options);
        /// Return a snapshot URI for @p profile.
        Task<Result<MediaUri>> get_snapshot_uri(ProfileToken profile, CallOptions options);
        /// Return the device's video sources, whose tokens address the Imaging service.
        Task<Result<std::vector<VideoSource>>> get_video_sources(CallOptions options);
        /// Return every profile with its source, encoder, PTZ, and analytics configuration.
        Task<Result<std::vector<ProfileConfiguration>>> get_profile_configurations(CallOptions options);
        /// Return what video encoder configuration @p configuration accepts.
        Task<Result<VideoEncoderOptions>>
        get_video_encoder_options(std::string configuration, CallOptions options);
        /**
         * @brief Replace a video encoder configuration, such as its resolution or bitrate.
         *
         * Start from a configuration returned by get_profile_configurations(); the
         * device replaces every field. With @p persist the change survives a reboot.
         */
        Task<Result<void>> set_video_encoder_configuration(
            VideoEncoderConfiguration configuration, bool persist, CallOptions options
        );

      private:
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<ServiceBackend> backend_;
    };

    /**
     * @brief Domain-oriented PTZ client using normalized [-1, +1] coordinates.
     */
    class PtzClient
    {
      public:
        /// Bind a PTZ client to a runtime and service backend.
        PtzClient(std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend);
        /// Return advertised native spaces for @p profile.
        Task<Result<PtzSpaces>> spaces(ProfileToken profile, CallOptions options);
        /// Return current normalized status for @p profile.
        Task<Result<PtzStatus>> status(ProfileToken profile, CallOptions options);
        /// Begin continuous movement with explicitly supplied axes.
        Task<Result<MoveResult>>
        continuous_move(ProfileToken profile, PtzVector velocity, CallOptions options);
        /// Apply a normalized relative translation.
        Task<Result<MoveResult>>
        relative_move(ProfileToken profile, PtzVector translation, CallOptions options);
        /// Move to a normalized absolute position, optionally with normalized speed.
        Task<Result<MoveResult>> absolute_move(
            ProfileToken profile,
            PtzVector position,
            std::optional<PtzVector> speed,
            CallOptions options
        );
        /// Stop movement for @p profile.
        Task<Result<void>> stop(ProfileToken profile, CallOptions options);
        /// Bypass ordinary work ordering and request a safety Stop.
        Task<Result<void>> emergency_stop(ProfileToken profile, CallOptions options);
        /// Return presets for @p profile.
        Task<Result<std::vector<Preset>>> get_presets(ProfileToken profile, CallOptions options);
        /// Move @p profile to @p preset.
        Task<Result<void>>
        goto_preset(ProfileToken profile, PresetToken preset, CallOptions options);
        /**
         * @brief Save the current position as a preset and return its token.
         *
         * An empty @p token creates a new preset named @p name; otherwise that
         * preset is overwritten with the current position.
         */
        Task<Result<PresetToken>>
        set_preset(ProfileToken profile, std::string name, PresetToken token, CallOptions options);
        /// Delete a preset.
        Task<Result<void>> remove_preset(ProfileToken profile, PresetToken preset, CallOptions options);
        /// Return the preset tours of @p profile.
        Task<Result<std::vector<PresetTour>>> get_preset_tours(ProfileToken profile, CallOptions options);
        /// Create an empty tour; fill it with modify_preset_tour() before starting it.
        Task<Result<std::string>> create_preset_tour(ProfileToken profile, CallOptions options);
        /// Replace the name, auto start, and spots of tour @p tour.token.
        Task<Result<void>> modify_preset_tour(ProfileToken profile, PresetTour tour, CallOptions options);
        /**
         * @brief Start, stop, or pause a preset tour.
         *
         * A started tour moves the camera on its own and keeps running after the
         * session closes, until it is stopped; the session's safety Stop does not
         * apply to it.
         */
        Task<Result<void>> operate_preset_tour(
            ProfileToken profile, std::string tour, PresetTourOperation operation, CallOptions options
        );
        /// Delete a preset tour.
        Task<Result<void>> remove_preset_tour(ProfileToken profile, std::string tour, CallOptions options);
        /// Return the PTZ configuration of @p profile, including its image orientation.
        Task<Result<PtzConfiguration>> get_configuration(ProfileToken profile, CallOptions options);
        /**
         * @brief Change the image orientation (EFlip, Reverse) of @p profile.
         *
         * Only set fields change; the rest of the configuration is sent back as
         * the device reported it. With @p persist the change survives a reboot.
         */
        Task<Result<void>>
        set_direction(ProfileToken profile, PtzDirection direction, bool persist, CallOptions options);

      private:
        friend class DeviceSession;
        PtzClient(
            std::shared_ptr<Runtime> runtime,
            std::shared_ptr<ServiceBackend> backend,
            std::shared_ptr<class PtzSafetyState> safety
        );
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<ServiceBackend> backend_;
        std::shared_ptr<class PtzSafetyState> safety_;
    };

    /**
     * @brief Stateful manual PTZ driver with coalescing, ramps, leases, and safe Stop.
     *
     * Input methods are non-blocking. A private worker translates the newest intent
     * into bounded-rate PtzClient calls. close() should be consumed before releasing
     * the last reference when the final Stop result matters.
     */
    class PtzController final : public std::enable_shared_from_this<PtzController>
    {
      public:
        /// Validate configuration and start a controller for one Media profile.
        static Result<std::shared_ptr<PtzController>> create(
            std::shared_ptr<PtzClient> client,
            ProfileToken profile,
            PtzControllerConfig config = {}
        );
        ~PtzController();
        /// Claim manual control; an empty owner is invalid.
        Result<void> claim_manual(std::string owner);
        /// Submit a monotonically sequenced input for the current owner.
        Result<void> set_manual_input(std::string_view owner, std::uint64_t sequence, PtzVector);
        /// Convenience input for the built-in local owner.
        Result<void> set_manual_input(PtzVector);
        /// Release the current owner's input and ramp toward zero.
        Result<void> release_manual(std::string_view owner);
        /// Release ownership after input has been released.
        Result<void> relinquish_manual(std::string_view owner);
        /// Immediately clear intent and request an urgent device Stop.
        Task<Result<void>> stop(CallOptions options);
        /// Stop the worker and issue a final urgent Stop. Idempotent.
        Task<Result<void>> close(CallOptions options);
        /// Return a lock-consistent telemetry snapshot.
        PtzControllerState state() const;

      private:
        struct Impl;
        explicit PtzController(std::shared_ptr<Impl>);
        std::shared_ptr<Impl> impl_;
    };

    /**
     * @brief Bounded stream of deduplicated discovery observations.
     *
     * inject() is the deterministic datagram boundary used by tests and adapters.
     */
    class DiscoverySession
    {
      public:
        /// Create a session with a bounded observation queue.
        DiscoverySession(std::shared_ptr<Runtime> runtime, std::size_t maximum_observations = 256);
        /// Inject one parsed observation, suppressing duplicate identity tuples.
        Task<Result<void>> inject(DiscoveryObservation observation, CallOptions options);
        /// Remove and return the next available observation.
        Task<Result<DiscoveryObservation>> next(CallOptions options);
        /// Close the session and discard queued observations.
        Task<Result<void>> close(CallOptions options);

      private:
        struct State;
        std::shared_ptr<State> state_;
        std::shared_ptr<Runtime> runtime_;
    };

    /// Structured outcome of caller-visible bounded cleanup.
    struct CloseResult
    {
        bool cleanup_complete{true};
        bool safety_complete{true};
    };
    class ManagedEventStream;

    /// Options used when creating an explicit PullPoint subscription.
    struct PullPointSubscriptionOptions
    {
        std::chrono::seconds requested_lifetime{60}; ///< Requested initial subscription lifetime.
        /// Deliver only these topics (ConcreteSet expressions, see EventTopic::topic);
        /// empty delivers every topic. A trailing `//.` also selects sub-topics.
        std::vector<std::string> topics;
    };

    /// Caller-visible lifecycle state of an explicit PullPoint subscription.
    enum class PullPointSubscriptionState { open, closing, closed };

    /// Independent PullPoint subscription with explicit pull, renew, and cleanup.
    class PullPointSubscription final :
        public std::enable_shared_from_this<PullPointSubscription>
    {
      public:
        /// Pull at most @p maximum_messages, allowing the server to wait up to @p server_timeout.
        Task<Result<std::vector<Event>>> pull(
            std::chrono::milliseconds server_timeout,
            std::size_t maximum_messages,
            CallOptions options
        );
        /// Renew the subscription and return its updated endpoint and expiry information.
        Task<Result<PullPointSubscriptionInfo>> renew(
            std::chrono::seconds requested_lifetime, CallOptions options
        );
        /// Unsubscribe from the remote PullPoint. Idempotent after successful closure.
        Task<Result<void>> unsubscribe(CallOptions options);
        /// Attempt bounded cleanup and report whether it completed within the budget.
        Task<Result<CloseResult>> close(std::chrono::milliseconds cleanup_budget);
        /// Return a lock-consistent copy of the current subscription information.
        PullPointSubscriptionInfo info() const;
        /// Return the current local lifecycle state.
        PullPointSubscriptionState state() const noexcept;

      private:
        friend class EventClient;
        friend class ManagedEventStream;
        struct Impl;
        explicit PullPointSubscription(std::shared_ptr<Impl>);
        std::shared_ptr<Impl> impl_;
    };

    /// Resource, polling, renewal, and reconnect bounds for an automatic Event stream.
    struct ManagedEventOptions
    {
        std::size_t queue_capacity{256}; ///< Maximum queued events and health items.
        std::size_t pull_limit{32}; ///< Maximum messages requested by each PullMessages call.
        std::chrono::milliseconds pull_timeout{5000}; ///< Server-side long-poll timeout.
        std::chrono::milliseconds minimum_backoff{250}; ///< Initial reconnect delay.
        std::chrono::milliseconds maximum_backoff{30000}; ///< Maximum reconnect delay.
        std::chrono::seconds requested_lifetime{60}; ///< Subscription and renewal lifetime.
        /// Topic filter applied to every subscription, including after reconnect.
        std::vector<std::string> topics;
    };

    /// Factory for explicit PullPoint subscriptions and automatic managed streams.
    class EventClient
    {
      public:
        /// Bind an Event client to a runtime and service backend.
        EventClient(std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend);
        /// Create an independent explicit PullPoint subscription.
        Task<Result<std::shared_ptr<PullPointSubscription>>> create_pullpoint(
            PullPointSubscriptionOptions options, CallOptions call
        );
        /// Create a subscription and start its bounded polling and reconnect worker.
        Task<Result<std::shared_ptr<ManagedEventStream>>> open_managed_stream(
            ManagedEventOptions options, CallOptions call
        );
        /// Return the event topics the device can emit (GetEventProperties).
        Task<Result<std::vector<EventTopic>>> get_topics(CallOptions call);

      private:
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<ServiceBackend> backend_;
    };

    /// Managed, bounded stream of events and observable continuity health signals.
    class ManagedEventStream : public std::enable_shared_from_this<ManagedEventStream>
    {
      public:
        /**
         * @brief Create a managed stream with a bounded queue.
         * @param runtime Runtime used for completion dispatch.
         * @param backend Backend used to pull events.
         * @param maximum_events Maximum queued events and health items.
         */
        ManagedEventStream(
            std::shared_ptr<Runtime> runtime,
            std::shared_ptr<ServiceBackend> backend,
            std::size_t maximum_events = 256
        );
        ~ManagedEventStream();
        /// Remove and return the next event or health item.
        Task<Result<EventOrHealth>> next(CallOptions options);
        /// Pull at most @p maximum events into the bounded managed queue.
        Task<Result<void>> poll(std::size_t maximum, CallOptions options);
        /// Record a reconnect and enqueue a possible-event-loss health item.
        Task<Result<void>> mark_reconnected(CallOptions options);
        /// Close the stream and discard queued items.
        Task<Result<void>> close(CallOptions options);

      private:
        friend class EventClient;
        ManagedEventStream(
            std::shared_ptr<Runtime> runtime,
            std::shared_ptr<ServiceBackend> backend,
            std::shared_ptr<PullPointSubscription> subscription,
            ManagedEventOptions options
        );
        struct State;
        // The worker owns only shared State, never this object, so a stream
        // destroyed on the Runtime thread can let its worker finish on its own
        // instead of blocking the thread that worker is waiting on.
        static void run_managed(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<ServiceBackend> backend_;
    };

    /**
     * @brief Calculate a WS-Security UsernameToken PasswordDigest.
     * @param nonce Raw nonce bytes.
     * @param created UTF-8 UTC Created timestamp.
     * @param password Password bytes.
     * @return Base64-encoded SHA-1 digest mandated by UsernameToken.
     */
    std::string username_token_digest(
        std::string_view nonce, std::string_view created, std::string_view password
    );

    /**
     * @brief Serialize a SOAP 1.2 request envelope.
     * @param operation Operation metadata identifier.
     * @param body Already-encoded operation body content.
     * @param credentials Optional credentials for the security header.
     * @param clock_offset Device clock minus local clock, applied to the
     *        UsernameToken Created timestamp so a device with a skewed clock
     *        still accepts it.
     * @return Owned UTF-8 SOAP envelope.
     */
    std::string soap_envelope(
        OperationId operation,
        std::string_view body,
        const std::optional<Credentials>& credentials = {},
        std::chrono::system_clock::duration clock_offset = {}
    );

    std::string soap_envelope(
        OperationId operation,
        std::string_view body,
        const CredentialLease* credentials,
        std::chrono::system_clock::duration clock_offset = {}
    );

    /**
     * @brief Validate bounded SOAP XML and locate an operation response.
     * @param xml Untrusted XML bytes.
     * @param response_name Expected response local name.
     * @param limits Structural and byte limits.
     * @return Collected response text, or a parsing/protocol Error.
     */
    Result<std::string> read_soap_operation(
        std::string_view xml, std::string_view response_name, XmlLimits limits = {}
    );

    /**
     * @brief Connected Device service session and entry point for service clients.
     *
     * DeviceSession stores the authorized Device endpoint, credentials, transport,
     * and runtime. It does not own persistent inventory.
     */
    /// Validated ownership and policy inputs used to construct a device session.
    struct DeviceSessionOptions
    {
        std::string device_url;
        std::shared_ptr<HttpTransport> transport;
        std::shared_ptr<EndpointPolicy> endpoint_policy;
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<CredentialProvider> credential_provider;
        CallOptions call;
    };

    namespace detail
    {
        class DeviceClock;
    }

    /// Device hostname (GetHostname).
    struct HostnameInformation
    {
        bool from_dhcp{}; ///< Whether the name is assigned by DHCP.
        std::string name; ///< Current hostname.
    };

    /// One network interface and its IPv4 configuration.
    struct NetworkInterface
    {
        std::string token;            ///< Interface token.
        bool enabled{};               ///< Whether the interface is enabled.
        std::string name;             ///< Interface name, such as `eth0`.
        std::string hardware_address; ///< MAC address as reported.
        int mtu{};                    ///< Maximum transmission unit.
        bool ipv4_enabled{};          ///< Whether IPv4 is enabled.
        bool ipv4_dhcp{};             ///< Whether IPv4 is configured by DHCP.
        /// IPv4 addresses with prefix length, such as `192.0.2.10/24`.
        std::vector<std::string> ipv4_addresses;
    };

    /// DNS resolver configuration (GetDNS).
    struct DnsInformation
    {
        bool from_dhcp{};                        ///< Whether servers come from DHCP.
        std::vector<std::string> search_domains; ///< Search domains.
        std::vector<std::string> servers;        ///< Manually configured servers.
        std::vector<std::string> dhcp_servers;   ///< Servers learned from DHCP.
    };

    /// A network service the device offers, such as HTTP or RTSP.
    struct NetworkProtocol
    {
        std::string name;       ///< `HTTP`, `HTTPS`, or `RTSP`.
        bool enabled{};         ///< Whether the protocol is enabled.
        std::vector<int> ports; ///< Listening ports.
    };

    /// Whether the device answers WS-Discovery probes.
    enum class DiscoveryMode { discoverable, non_discoverable };

    /// A device account (GetUsers); passwords are never reported.
    struct DeviceUser
    {
        std::string username; ///< Account name.
        std::string level;    ///< `Administrator`, `Operator`, `User`, or `Anonymous`.
    };

    /// Thread-safe owner of a connected device and all child-operation cancellation.
    class DeviceSession : public std::enable_shared_from_this<DeviceSession>
    {
      public:
        ~DeviceSession();
        /**
         * @brief Create a session for an explicitly supplied Device service URL.
         * @param device_url Caller-authorized Device service URL.
         * @param transport HTTP transport implementation.
         * @param endpoint_policy Policy applied before connecting.
         * @param runtime Runtime used for operation completion.
         * @param credentials Optional Device credentials.
         * @param options Deadline and cancellation state.
         * @return Cold task producing a connected session.
         */
        static Task<Result<std::shared_ptr<DeviceSession>>> connect(
            std::string device_url,
            std::shared_ptr<HttpTransport> transport,
            std::shared_ptr<EndpointPolicy> endpoint_policy,
            std::shared_ptr<Runtime> runtime,
            std::optional<Credentials> credentials,
            CallOptions options
        );
        static Task<Result<std::shared_ptr<DeviceSession>>> connect(DeviceSessionOptions options);
        Task<Result<CloseResult>> close(std::chrono::milliseconds cleanup_budget);
        bool is_closed() const noexcept;
        /// Execute GetDeviceInformation.
        Task<Result<DeviceInformation>> get_device_information(CallOptions options);
        /// Execute GetSystemDateAndTime.
        Task<Result<SystemDateAndTime>> get_system_date_and_time(CallOptions options);
        /// Execute GetScopes.
        Task<Result<std::vector<std::string>>> get_scopes(CallOptions options);
        /// Execute GetScopes, keeping whether each scope is fixed or configurable.
        Task<Result<std::vector<ScopeEntry>>> get_scope_entries(CallOptions options);
        /**
         * @brief Execute SetScopes, replacing every configurable scope.
         *
         * Devices refuse to overwrite fixed scopes (ter:ScopeOverwrite); use
         * add_scopes() to supplement them instead.
         */
        Task<Result<void>> set_scopes(std::vector<std::string> scopes, CallOptions options);
        /// Execute AddScopes. Scope URIs must not contain whitespace.
        Task<Result<void>> add_scopes(std::vector<std::string> scopes, CallOptions options);
        /// Execute RemoveScopes; only configurable scopes can be removed.
        Task<Result<void>> remove_scopes(std::vector<std::string> scopes, CallOptions options);
        /**
         * @brief Execute SetSystemDateAndTime.
         *
         * A successful change also discards the session's measured clock offset,
         * so later signed requests are stamped against the new device time.
         */
        Task<Result<void>> set_system_date_and_time(
            SystemDateAndTimeSettings settings, CallOptions options
        );
        /// Execute GetNTP.
        Task<Result<NtpConfiguration>> get_ntp(CallOptions options);
        /// Execute SetNTP with @p configuration's from_dhcp and servers.
        Task<Result<void>> set_ntp(NtpConfiguration configuration, CallOptions options);
        /// Execute GetHostname.
        Task<Result<HostnameInformation>> get_hostname(CallOptions options);
        /// Execute SetHostname; @p name must be a valid DNS hostname.
        Task<Result<void>> set_hostname(std::string name, CallOptions options);
        /// Execute GetNetworkInterfaces.
        Task<Result<std::vector<NetworkInterface>>> get_network_interfaces(CallOptions options);
        /// Execute GetDNS.
        Task<Result<DnsInformation>> get_dns(CallOptions options);
        /// Execute GetNetworkDefaultGateway, returning IPv4 and IPv6 gateway addresses.
        Task<Result<std::vector<std::string>>> get_network_default_gateway(CallOptions options);
        /// Execute GetNetworkProtocols.
        Task<Result<std::vector<NetworkProtocol>>> get_network_protocols(CallOptions options);
        /// Execute GetDiscoveryMode.
        Task<Result<DiscoveryMode>> get_discovery_mode(CallOptions options);
        /**
         * @brief Execute SetDiscoveryMode.
         *
         * A non-discoverable device ignores WS-Discovery probes; direct probing
         * (discover_network() with direct_probe) still finds it.
         */
        Task<Result<void>> set_discovery_mode(DiscoveryMode mode, CallOptions options);
        /// Execute GetUsers.
        Task<Result<std::vector<DeviceUser>>> get_users(CallOptions options);
        /**
         * @brief Execute SystemReboot and return the device's message.
         *
         * The device restarts shortly after answering; the session's later
         * requests fail until it is back.
         */
        Task<Result<std::string>> system_reboot(CallOptions options);
        /**
         * @brief Send any ONVIF request and return its response element's content.
         *
         * For operations without a dedicated method. @p body is the complete,
         * well-formed SOAP Body content, such as `<tds:GetZeroConfiguration/>`;
         * the prefixes tds, trt, tptz, tev, tan, timg, tt, wsnt, and tns1 are
         * declared. The request is signed, clock-corrected, and sent to the
         * endpoint @p service advertises, under the session's endpoint policy.
         * SOAP Faults are returned as errors with their codes.
         *
         * @param service Service whose advertised endpoint receives the request.
         * @param body Complete SOAP Body content.
         * @param response_name Local name of the expected response element; when
         *        empty, the body's first element name plus `Response`.
         * @param options Deadline and cancellation state.
         * @return The response element's inner markup.
         */
        Task<Result<std::string>> call(
            OnvifService service, std::string body, std::string response_name, CallOptions options
        );
        /// Execute GetServices.
        Task<Result<std::vector<Service>>> get_services(CallOptions options);
        /// Execute GetCapabilities.
        Task<Result<Capabilities>> get_capabilities(CallOptions options);
        /// Return a Media client when the transport supplies Media behavior.
        Result<std::shared_ptr<MediaClient>> media();
        /// Return a PTZ client when the transport supplies PTZ behavior.
        Result<std::shared_ptr<PtzClient>> ptz();
        /// Return the explicit Event service client.
        Result<std::shared_ptr<EventClient>> event_client();
        /// Return the Analytics client for detection modules and rules.
        Result<std::shared_ptr<AnalyticsClient>> analytics();
        /// Return the Imaging client for brightness, contrast, and similar settings.
        Result<std::shared_ptr<ImagingClient>> imaging();
        /// Return a bounded managed Event stream.
        Result<std::shared_ptr<ManagedEventStream>> events(std::size_t maximum_events = 256);

      private:
        DeviceSession(
            std::string,
            std::shared_ptr<HttpTransport>,
            std::shared_ptr<EndpointPolicy>,
            std::shared_ptr<Runtime>,
            std::shared_ptr<CredentialProvider>
        );
        Task<Result<std::string>> invoke(OperationId, std::string_view response, CallOptions);
        Task<Result<std::string>> invoke(
            OperationId, std::string body, std::string_view response, CallOptions, bool idempotent
        );
        void operation_finished() noexcept;
        void register_runtime_shutdown();
        void shutdown_from_runtime() noexcept;
        std::shared_ptr<ServiceBackend> backend();
        std::shared_ptr<class PtzSafetyState> ptz_safety();
        std::string url_;
        std::shared_ptr<HttpTransport> transport_;
        std::shared_ptr<EndpointPolicy> endpoint_policy_;
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<CredentialProvider> credential_provider_;
        enum class State { open, closing, closed };
        mutable std::mutex state_mutex_;
        State state_{State::open};
        CancellationSource session_cancellation_;
        std::vector<std::function<void(Result<CloseResult>)>> close_waiters_;
        std::size_t active_operations_{};
        std::mutex backend_mutex_;
        std::shared_ptr<ServiceBackend> backend_;
        std::shared_ptr<class PtzSafetyState> ptz_safety_;
        std::shared_ptr<detail::DeviceClock> clock_;
        bool close_safety_complete_{true};
        std::size_t runtime_shutdown_registration_{};
    };

    /**
     * @brief Deterministic in-process Device transport and service backend for tests.
     *
     * The virtual transport implements Device SOAP responses plus Media, PTZ, and
     * Event domain behavior without opening network sockets.
     *
     * @code{.cpp}
     * auto device = std::make_shared<wlonvif::VirtualDeviceTransport>(
     *     runtime, information, scopes, services, capabilities);
     * device->add_profile(profile, stream_uri, snapshot_uri, spaces);
     * device->emit(event);
     * @endcode
     */
    class VirtualDeviceTransport final : public HttpTransport, public ServiceBackend
    {
      public:
        /// Construct a virtual Device with fixed Device service data.
        VirtualDeviceTransport(
            std::shared_ptr<Runtime> runtime,
            DeviceInformation information,
            std::vector<std::string> scopes,
            std::vector<Service> services,
            Capabilities capabilities
        );
        /// Dispatch a Device SOAP request in process.
        Task<Result<HttpResponse>> send(HttpRequest, CancellationToken) override;
        /// Add or replace a Media/PTZ profile and its advertised data.
        void add_profile(MediaProfile, MediaUri stream, MediaUri snapshot, PtzSpaces);
        /// Add a PTZ preset to an existing profile.
        void add_preset(ProfileToken, Preset);
        /// Append an event to the virtual PullPoint queue.
        void emit(Event);
        /// Fail the next movement with a supplied deterministic error.
        void fail_next_move(Error error);
        /// Configure or clear a deterministic Stop failure.
        void set_stop_failure(std::optional<Error> error);
        /// Delay Stop to exercise bounded cleanup behavior.
        void set_stop_delay(std::chrono::milliseconds delay);
        /// Return the number of Stop attempts made for a profile.
        std::size_t stop_attempts(const ProfileToken& profile) const;
        /// Replace the topics reported by GetEventProperties.
        void set_event_topics(std::vector<EventTopic> topics);
        /// Add or replace a detailed profile returned by profile_configurations().
        void set_profile_configuration(ProfileConfiguration profile);
        /// Set what a video encoder configuration accepts.
        void set_video_encoder_options(std::string configuration, VideoEncoderOptions options);
        /// Set the PTZ configuration reported for @p profile.
        void set_ptz_configuration(ProfileToken profile, PtzConfiguration configuration);
        /// Return how many SystemReboot requests the virtual device has accepted.
        std::size_t reboot_count() const;
        /// Answer a custom Device-service request through the virtual SOAP handler.
        Result<std::string>
        call(OnvifService service, const std::string& body, const std::string& response_name) override;
        /// Add or replace a video source with its imaging settings and accepted ranges.
        void set_video_source(VideoSource source, ImagingSettings settings, ImagingOptions options);
        /// Add or replace the modules and rules of an analytics configuration.
        void set_analytics(
            std::string configuration,
            std::vector<AnalyticsConfig> modules,
            std::vector<AnalyticsConfig> rules
        );
        /// Return configured profiles.
        Result<std::vector<MediaProfile>> profiles() override;
        /// Return the configured stream URI.
        Result<MediaUri> stream_uri(const ProfileToken&) override;
        /// Return the configured snapshot URI.
        Result<MediaUri> snapshot_uri(const ProfileToken&) override;
        /// Return configured native PTZ spaces.
        Result<PtzSpaces> ptz_spaces(const ProfileToken&) override;
        /// Return current virtual PTZ status.
        Result<PtzStatus> ptz_status(const ProfileToken&) override;
        /// Apply movement to virtual PTZ state.
        Result<MoveResult> move(const ProfileToken&, PtzVector, bool relative) override;
        Result<MoveResult>
        absolute_move(const ProfileToken&, PtzVector, std::optional<PtzVector>) override;
        /// Stop virtual PTZ movement.
        Result<void> stop(const ProfileToken&) override;
        /// Return configured PTZ presets.
        Result<std::vector<Preset>> presets(const ProfileToken&) override;
        /// Apply a configured PTZ preset.
        Result<void> goto_preset(const ProfileToken&, const PresetToken&) override;
        /// Remove and return queued virtual events.
        Result<std::vector<Event>> pull_events(std::size_t maximum) override;
        /// Virtual subscriptions do not expire.
        Result<void> renew_events() override;
        /// Close the virtual Event subscription.
        Result<void> close_events() override;
        /// Create a subscription that delivers only events matching @p topics.
        Result<PullPointSubscriptionInfo> create_filtered_event_subscription(
            std::chrono::seconds requested_lifetime, const std::vector<std::string>& topics
        ) override;
        /// Pull queued events, discarding those outside the subscription's filter.
        Result<std::vector<Event>> pull_event_subscription(
            const std::string& token, std::chrono::milliseconds server_timeout, std::size_t maximum
        ) override;
        /// Return the configured event topics.
        Result<std::vector<EventTopic>> event_topics() override;
        /// Return the configured analytics modules.
        Result<std::vector<AnalyticsConfig>> analytics_modules(const std::string& configuration) override;
        /// Replace parameters of configured analytics modules.
        Result<void> modify_analytics_modules(
            const std::string& configuration, const std::vector<AnalyticsConfig>& modules
        ) override;
        /// Return the configured analytics rules.
        Result<std::vector<AnalyticsConfig>> analytics_rules(const std::string& configuration) override;
        /// Replace parameters of configured analytics rules.
        Result<void> modify_analytics_rules(
            const std::string& configuration, const std::vector<AnalyticsConfig>& rules
        ) override;
        /// Return the configured video sources.
        Result<std::vector<VideoSource>> video_sources() override;
        /// Return a configured source's imaging settings.
        Result<ImagingSettings> imaging_settings(const std::string& source) override;
        /// Merge the set fields into a source's settings, enforcing its options.
        Result<void> set_imaging_settings(
            const std::string& source, const ImagingSettings& settings, bool persist
        ) override;
        /// Return a configured source's imaging options.
        Result<ImagingOptions> imaging_options(const std::string& source) override;
        /// Return the configured detailed profiles.
        Result<std::vector<ProfileConfiguration>> profile_configurations() override;
        /// Return the configured encoder options.
        Result<VideoEncoderOptions> video_encoder_options(const std::string& configuration) override;
        /// Replace an encoder in every profile using it, enforcing its options.
        Result<void> set_video_encoder_configuration(
            const VideoEncoderConfiguration& configuration, bool persist
        ) override;
        /// Save the current virtual position as a preset.
        Result<PresetToken>
        set_preset(const ProfileToken& profile, const std::string& name, const PresetToken& token) override;
        /// Delete a virtual preset.
        Result<void> remove_preset(const ProfileToken& profile, const PresetToken& token) override;
        /// Return the virtual preset tours.
        Result<std::vector<PresetTour>> preset_tours(const ProfileToken& profile) override;
        /// Create an empty virtual tour.
        Result<std::string> create_preset_tour(const ProfileToken& profile) override;
        /// Replace a virtual tour's name, auto start, and spots.
        Result<void> modify_preset_tour(const ProfileToken& profile, const PresetTour& tour) override;
        /// Change a virtual tour's state.
        Result<void> operate_preset_tour(
            const ProfileToken& profile, const std::string& token, PresetTourOperation operation
        ) override;
        /// Delete a virtual tour.
        Result<void> remove_preset_tour(const ProfileToken& profile, const std::string& token) override;
        /// Return the configured PTZ configuration.
        Result<PtzConfiguration> ptz_configuration(const ProfileToken& profile) override;
        /// Change the virtual orientation, enforcing the accepted modes.
        Result<void>
        set_ptz_direction(const ProfileToken& profile, const PtzDirection& direction, bool persist) override;

      private:
        std::shared_ptr<Runtime> runtime_;
        DeviceInformation info_;
        std::vector<std::string> scopes_;
        std::vector<Service> services_;
        Capabilities capabilities_;
        struct VirtualServiceState;
        std::shared_ptr<VirtualServiceState> service_state_;
    };

} // namespace wlonvif
