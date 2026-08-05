#pragma once

/**
 * @file wlonvif.hpp
 * @brief Public C++ API for ONVIF clients, discovery, events, and virtual devices.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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
        malformed_http,     ///< HTTP metadata or framing was invalid.
        malformed_xml,      ///< XML was invalid or exceeded a structural limit.
        soap_fault,         ///< The peer returned a SOAP Fault.
        unsupported,        ///< The requested feature or dialect is unsupported.
        invalid_argument,   ///< A caller-provided value is invalid.
        endpoint_rejected,  ///< EndpointPolicy rejected the destination.
        protocol_violation, ///< The peer response violated the operation contract.
        closed,             ///< The owning session, stream, or runtime is closed.
        queue_overflow,     ///< A bounded queue cannot accept more data.
        internal            ///< Unexpected library failure.
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
        event_close               ///< Event subscription closure.
    };

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

      private:
        Runtime();
        void run();
        std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<std::function<void()>> queue_;
        bool stopping_{false};
        std::thread worker_;
    };

    /// Copyable view of shared cancellation state.
    class CancellationToken
    {
      public:
        /// Return true after the associated source requests cancellation.
        bool stop_requested() const noexcept;

      private:
        friend class CancellationSource;
        explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
            : state_(std::move(state))
        {
        }
        std::shared_ptr<std::atomic_bool> state_;
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

    /// Owned HTTP request passed to HttpTransport.
    enum class HttpMethod
    {
        post, ///< Send the request body with HTTP POST.
        get   ///< Retrieve response bytes with HTTP GET.
    };

    /// Owned HTTP request passed to HttpTransport.
    struct HttpRequest
    {
        std::string url;                                ///< Authorized destination URL.
        std::string body;                               ///< Request body bytes.
        std::vector<std::string> headers;               ///< Complete request header lines.
        std::chrono::steady_clock::time_point deadline; ///< Absolute transport deadline.
        std::optional<Credentials> credentials;         ///< Optional HTTP authentication data.
        HttpMethod method{HttpMethod::post};             ///< HTTP request method.
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

    /// Normalized UTC device time.
    struct SystemDateAndTime
    {
        std::chrono::system_clock::time_point utc; ///< Device UTC time.
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
    };

    using ProfileToken = std::string; ///< Opaque Media profile token.
    using PresetToken = std::string;  ///< Opaque PTZ preset token.

    /// Named Media profile.
    struct MediaProfile
    {
        ProfileToken token; ///< Opaque profile token used by later operations.
        std::string name;   ///< Human-readable profile name.
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

    /// Native ranges for the pan, tilt, and zoom axes.
    struct PtzSpaces
    {
        NativeRange pan;  ///< Pan axis range.
        NativeRange tilt; ///< Tilt axis range.
        NativeRange zoom; ///< Zoom axis range.
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

    /// Observable managed-stream continuity condition.
    enum class StreamHealth
    {
        possible_event_loss, ///< Reconnect or resubscribe may have lost events.
        events_dropped       ///< Managed queue discarded old events at its bound.
    };
    using EventOrHealth = std::variant<Event, StreamHealth>; ///< Stream output item.

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
        std::vector<std::uint16_t> ports{80, 443, 8000, 8080, 8443, 8899}; ///< Direct ports.
        std::vector<std::string> device_paths{"/onvif/device_service"};    ///< Direct paths.
        std::size_t retransmissions{3};   ///< Copies of each Probe using one MessageID.
        std::size_t maximum_hosts{1024};  ///< Hard bound for expanded scan addresses.
        std::size_t maximum_results{256}; ///< Hard bound for returned candidates.
        std::size_t maximum_concurrency{16}; ///< Maximum simultaneous direct HTTP probes.
        std::chrono::milliseconds initial_retry_delay{50}; ///< Retry backoff base.
        std::chrono::milliseconds listen_duration{3000};   ///< ProbeMatch receive window.
        std::chrono::milliseconds connect_timeout{500};    ///< Direct connection timeout.
        std::chrono::milliseconds request_timeout{2000};   ///< Direct SOAP timeout.
        /**
         * Optional progress callback invoked on the discovery worker thread.
         * Exceptions thrown by the callback are ignored.
         */
        std::function<void(const DiscoveryProgress&)> progress;
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
        /// Stop movement for @p profile.
        Task<Result<void>> stop(ProfileToken profile, CallOptions options);
        /// Bypass ordinary work ordering and request a safety Stop.
        Task<Result<void>> emergency_stop(ProfileToken profile, CallOptions options);
        /// Return presets for @p profile.
        Task<Result<std::vector<Preset>>> get_presets(ProfileToken profile, CallOptions options);
        /// Move @p profile to @p preset.
        Task<Result<void>>
        goto_preset(ProfileToken profile, PresetToken preset, CallOptions options);

      private:
        std::shared_ptr<Runtime> runtime_;
        std::shared_ptr<ServiceBackend> backend_;
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

    /// Managed, bounded stream of events and observable continuity health signals.
    class ManagedEventStream
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
        /// Remove and return the next event or health item.
        Task<Result<EventOrHealth>> next(CallOptions options);
        /// Pull at most @p maximum events into the bounded managed queue.
        Task<Result<void>> poll(std::size_t maximum, CallOptions options);
        /// Record a reconnect and enqueue a possible-event-loss health item.
        Task<Result<void>> mark_reconnected(CallOptions options);
        /// Close the stream and discard queued items.
        Task<Result<void>> close(CallOptions options);

      private:
        struct State;
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
     * @return Owned UTF-8 SOAP envelope.
     */
    std::string soap_envelope(
        OperationId operation,
        std::string_view body,
        const std::optional<Credentials>& credentials = {}
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
    class DeviceSession : public std::enable_shared_from_this<DeviceSession>
    {
      public:
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
        /// Execute GetDeviceInformation.
        Task<Result<DeviceInformation>> get_device_information(CallOptions options);
        /// Execute GetSystemDateAndTime.
        Task<Result<SystemDateAndTime>> get_system_date_and_time(CallOptions options);
        /// Execute GetScopes.
        Task<Result<std::vector<std::string>>> get_scopes(CallOptions options);
        /// Execute GetServices.
        Task<Result<std::vector<Service>>> get_services(CallOptions options);
        /// Execute GetCapabilities.
        Task<Result<Capabilities>> get_capabilities(CallOptions options);
        /// Return a Media client when the transport supplies Media behavior.
        Result<std::shared_ptr<MediaClient>> media();
        /// Return a PTZ client when the transport supplies PTZ behavior.
        Result<std::shared_ptr<PtzClient>> ptz();
        /// Return a bounded managed Event stream.
        Result<std::shared_ptr<ManagedEventStream>> events(std::size_t maximum_events = 256);

      private:
        DeviceSession(
            std::string,
            std::shared_ptr<HttpTransport>,
            std::shared_ptr<EndpointPolicy>,
            std::shared_ptr<Runtime>,
            std::optional<Credentials>
        );
        Task<Result<std::string>> invoke(OperationId, std::string_view response, CallOptions);
        std::string url_;
        std::shared_ptr<HttpTransport> transport_;
        std::shared_ptr<EndpointPolicy> endpoint_policy_;
        std::shared_ptr<Runtime> runtime_;
        std::optional<Credentials> credentials_;
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
