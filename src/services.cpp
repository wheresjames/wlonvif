#include <cctype>
#include <cmath>
#include <deque>
#include <unordered_set>
#include <wlonvif/wlonvif.hpp>
#include "network_services.hpp"
#include "ptz_safety.hpp"

namespace wlonvif
{
    namespace
    {
        template <class T> T wait_task(Task<T> task)
        {
            struct WaitState
            {
                std::mutex mutex;
                std::condition_variable ready;
                std::optional<T> result;
            };
            auto state = std::make_shared<WaitState>();
            std::move(task).start([state](T result)
            {
                {
                    std::lock_guard lock(state->mutex);
                    state->result.emplace(std::move(result));
                }
                state->ready.notify_one();
            });
            std::unique_lock lock(state->mutex);
            state->ready.wait(lock, [&] { return state->result.has_value(); });
            return std::move(*state->result);
        }
        template <class T, class F>
        Task<Result<T>> async_call(
            std::shared_ptr<Runtime> runtime,
            CallOptions options,
            OperationId operation,
            F function,
            bool precheck = true
        )
        {
            return Task<Result<T>>(
                [runtime = std::move(runtime),
                 options,
                 operation,
                 function = std::move(function),
                 precheck](auto completion) mutable
                {
                    std::thread(
                        [runtime = std::move(runtime),
                         options,
                         operation,
                         precheck,
                         function = std::move(function),
                         completion = std::move(completion)]() mutable
                        {
                            Result<T> result = Error{
                                ErrorCategory::internal, operation, "operation did not complete"
                            };
                            if (precheck && options.cancellation.stop_requested())
                            {
                                result = Error{
                                    ErrorCategory::cancelled, operation, "operation cancelled"
                                };
                            }
                            else if (precheck && std::chrono::steady_clock::now() >= options.deadline)
                            {
                                result = Error{
                                    ErrorCategory::timeout, operation, "deadline expired"
                                };
                            }
                            else
                            {
                                ScopedNetworkCallOptions scope(options);
                                try
                                {
                                    result = function();
                                }
                                catch (const std::exception& error)
                                {
                                    result = Error{
                                        ErrorCategory::internal, operation, error.what()
                                    };
                                }
                            }
                            auto dispatch =
                                [completion = std::move(completion),
                                 result = std::move(result)]() mutable
                            {
                                completion(std::move(result));
                            };
                            if (!runtime->post(dispatch))
                            {
                                dispatch();
                            }
                        }
                    ).detach();
                }
            );
        }
        template <class T> Result<T> invalid(OperationId operation, std::string message)
        {
            return Error{ErrorCategory::invalid_argument, operation, std::move(message)};
        }

        /**
         * @brief Call options for a best-effort safety Stop.
         *
         * A Stop exists to halt motion, so it must not inherit the very cancellation
         * or expiry that prompted it: the transport refuses a request carrying a
         * cancelled token, which would silently drop the Stop at the moment it
         * matters most. The caller's deadline is kept when it is still ahead.
         */
        CallOptions safety_options(const CallOptions& requested)
        {
            const auto now = std::chrono::steady_clock::now();
            return CallOptions{
                requested.deadline > now ? requested.deadline : now + std::chrono::seconds(5),
                CancellationSource{}.token()
            };
        }

        bool valid_ptz_vector(const PtzVector& vector)
        {
            const auto valid = [](const std::optional<double>& value)
            {
                return !value || (std::isfinite(*value) && *value >= -1 && *value <= 1);
            };
            return valid(vector.pan) && valid(vector.tilt) && valid(vector.zoom);
        }
    } // namespace

    Result<PullPointSubscriptionInfo>
    ServiceBackend::create_event_subscription(std::chrono::seconds requested_lifetime)
    {
        auto pulled = pull_events(0);
        if (!pulled) return pulled.error();
        return PullPointSubscriptionInfo{
            "legacy", {}, std::chrono::system_clock::now() + requested_lifetime
        };
    }
    Result<std::vector<Event>> ServiceBackend::pull_event_subscription(
        const std::string&, std::chrono::milliseconds, std::size_t maximum
    )
    {
        return pull_events(maximum);
    }
    Result<PullPointSubscriptionInfo> ServiceBackend::renew_event_subscription(
        const std::string& token, std::chrono::seconds requested_lifetime
    )
    {
        auto renewed = renew_events();
        if (!renewed) return renewed.error();
        return PullPointSubscriptionInfo{
            token, {}, std::chrono::system_clock::now() + requested_lifetime
        };
    }
    Result<void> ServiceBackend::close_event_subscription(const std::string&)
    {
        return close_events();
    }
    Result<PullPointSubscriptionInfo> ServiceBackend::create_filtered_event_subscription(
        std::chrono::seconds requested_lifetime, const std::vector<std::string>& topics
    )
    {
        // Silently ignoring a filter would deliver events the caller excluded.
        if (!topics.empty())
            return Error{ErrorCategory::unsupported, OperationId::event_subscribe,
                         "topic filters are unsupported"};
        return create_event_subscription(requested_lifetime);
    }
    Result<std::vector<EventTopic>> ServiceBackend::event_topics()
    {
        return Error{ErrorCategory::unsupported, OperationId::get_event_properties,
                     "event topic discovery is unsupported"};
    }
    Result<std::vector<AnalyticsConfig>> ServiceBackend::analytics_modules(const std::string&)
    {
        return Error{ErrorCategory::unsupported, OperationId::get_analytics_modules,
                     "analytics is unsupported"};
    }
    Result<void> ServiceBackend::modify_analytics_modules(
        const std::string&, const std::vector<AnalyticsConfig>&
    )
    {
        return Error{ErrorCategory::unsupported, OperationId::modify_analytics_modules,
                     "analytics is unsupported"};
    }
    Result<std::vector<AnalyticsConfig>> ServiceBackend::analytics_rules(const std::string&)
    {
        return Error{ErrorCategory::unsupported, OperationId::get_analytics_rules,
                     "analytics is unsupported"};
    }
    Result<void> ServiceBackend::modify_analytics_rules(
        const std::string&, const std::vector<AnalyticsConfig>&
    )
    {
        return Error{ErrorCategory::unsupported, OperationId::modify_analytics_rules,
                     "analytics is unsupported"};
    }

    Result<std::vector<VideoSource>> ServiceBackend::video_sources()
    {
        return Error{ErrorCategory::unsupported, OperationId::get_video_sources,
                     "video source query is unsupported"};
    }
    Result<ImagingSettings> ServiceBackend::imaging_settings(const std::string&)
    {
        return Error{ErrorCategory::unsupported, OperationId::get_imaging_settings,
                     "imaging is unsupported"};
    }
    Result<void> ServiceBackend::set_imaging_settings(const std::string&, const ImagingSettings&, bool)
    {
        return Error{ErrorCategory::unsupported, OperationId::set_imaging_settings,
                     "imaging is unsupported"};
    }
    Result<ImagingOptions> ServiceBackend::imaging_options(const std::string&)
    {
        return Error{ErrorCategory::unsupported, OperationId::get_imaging_options,
                     "imaging is unsupported"};
    }

    Result<std::vector<ProfileConfiguration>> ServiceBackend::profile_configurations()
    {
        return Error{ErrorCategory::unsupported, OperationId::get_profiles,
                     "profile configuration query is unsupported"};
    }
    Result<VideoEncoderOptions> ServiceBackend::video_encoder_options(const std::string&)
    {
        return Error{ErrorCategory::unsupported, OperationId::get_video_encoder_options,
                     "video encoder options are unsupported"};
    }
    Result<void> ServiceBackend::set_video_encoder_configuration(const VideoEncoderConfiguration&, bool)
    {
        return Error{ErrorCategory::unsupported, OperationId::set_video_encoder_configuration,
                     "video encoder configuration is unsupported"};
    }

    namespace
    {
        Error unsupported_ptz(OperationId operation)
        {
            return Error{ErrorCategory::unsupported, operation, "operation is unsupported"};
        }
    } // namespace
    Result<PresetToken> ServiceBackend::set_preset(const ProfileToken&, const std::string&, const PresetToken&)
    {
        return unsupported_ptz(OperationId::set_preset);
    }
    Result<void> ServiceBackend::remove_preset(const ProfileToken&, const PresetToken&)
    {
        return unsupported_ptz(OperationId::remove_preset);
    }
    Result<std::vector<PresetTour>> ServiceBackend::preset_tours(const ProfileToken&)
    {
        return unsupported_ptz(OperationId::get_preset_tours);
    }
    Result<std::string> ServiceBackend::create_preset_tour(const ProfileToken&)
    {
        return unsupported_ptz(OperationId::create_preset_tour);
    }
    Result<void> ServiceBackend::modify_preset_tour(const ProfileToken&, const PresetTour&)
    {
        return unsupported_ptz(OperationId::modify_preset_tour);
    }
    Result<void> ServiceBackend::operate_preset_tour(const ProfileToken&, const std::string&, PresetTourOperation)
    {
        return unsupported_ptz(OperationId::operate_preset_tour);
    }
    Result<void> ServiceBackend::remove_preset_tour(const ProfileToken&, const std::string&)
    {
        return unsupported_ptz(OperationId::remove_preset_tour);
    }
    Result<PtzConfiguration> ServiceBackend::ptz_configuration(const ProfileToken&)
    {
        return unsupported_ptz(OperationId::get_ptz_configuration);
    }
    Result<void> ServiceBackend::set_ptz_direction(const ProfileToken&, const PtzDirection&, bool)
    {
        return unsupported_ptz(OperationId::set_ptz_configuration);
    }

    Result<std::string> ServiceBackend::call(OnvifService, const std::string&, const std::string&)
    {
        return Error{ErrorCategory::unsupported, OperationId::custom, "custom requests are unsupported"};
    }

    Task<Result<std::string>> DeviceSession::call(
        OnvifService service, std::string body, std::string response_name, CallOptions options
    )
    {
        if (is_closed())
            return Task<Result<std::string>>([](auto done)
                                             { done(Error{ErrorCategory::closed_handle, OperationId::custom,
                                                          "session is closed"}); });
        auto b = backend();
        return async_call<std::string>(
            runtime_, options, OperationId::custom,
            [b, service, body = std::move(body), response_name = std::move(response_name)]() mutable
            {
                constexpr auto operation = OperationId::custom;
                // The first element's local name identifies the operation.
                const auto open = body.find('<');
                const auto name_begin = body.find_first_not_of(" \t\r\n", open == std::string::npos ? 0 : open + 1);
                if (open == std::string::npos || name_begin == std::string::npos || body[name_begin] == '/' ||
                    body[name_begin] == '!' || body[name_begin] == '?')
                    return invalid<std::string>(operation, "body must start with an operation element");
                const auto name_end = body.find_first_of(" \t\r\n/>", name_begin);
                auto qualified = body.substr(name_begin, name_end - name_begin);
                const auto colon = qualified.find(':');
                const auto local = colon == std::string::npos ? qualified : qualified.substr(colon + 1);
                if (local.empty()) return invalid<std::string>(operation, "body must start with an operation element");
                if (response_name.empty()) response_name = local + "Response";
                for (const unsigned char c : response_name)
                    if (!std::isalnum(c) && c != '_' && c != '-' && c != '.')
                        return invalid<std::string>(operation, "invalid response element name");
                return b->call(service, body, response_name);
            }
        );
    }

    ImagingClient::ImagingClient(
        std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend
    ) : runtime_(std::move(runtime)), backend_(std::move(backend)) {}

    Task<Result<ImagingSettings>> ImagingClient::get_settings(std::string source, CallOptions options)
    {
        auto backend = backend_;
        return async_call<ImagingSettings>(
            runtime_, options, OperationId::get_imaging_settings,
            [backend, source = std::move(source)] { return backend->imaging_settings(source); }
        );
    }
    Task<Result<void>> ImagingClient::set_settings(
        std::string source, ImagingSettings settings, bool persist, CallOptions options
    )
    {
        auto backend = backend_;
        return async_call<void>(
            runtime_, options, OperationId::set_imaging_settings,
            [backend, source = std::move(source), settings = std::move(settings), persist]
            {
                constexpr auto operation = OperationId::set_imaging_settings;
                if (source.empty()) return invalid<void>(operation, "video source token is required");
                for (const auto& value : {settings.brightness, settings.color_saturation,
                                          settings.contrast, settings.sharpness})
                    if (value && !std::isfinite(*value))
                        return invalid<void>(operation, "imaging values must be finite");
                const bool any = settings.brightness || settings.color_saturation ||
                                 settings.contrast || settings.sharpness || settings.ir_cut_filter ||
                                 settings.backlight_compensation || settings.wide_dynamic_range ||
                                 settings.white_balance;
                if (!any) return invalid<void>(operation, "no imaging setting given");
                return backend->set_imaging_settings(source, settings, persist);
            }
        );
    }
    Task<Result<ImagingOptions>> ImagingClient::get_options(std::string source, CallOptions options)
    {
        auto backend = backend_;
        return async_call<ImagingOptions>(
            runtime_, options, OperationId::get_imaging_options,
            [backend, source = std::move(source)] { return backend->imaging_options(source); }
        );
    }

    AnalyticsClient::AnalyticsClient(
        std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend
    ) : runtime_(std::move(runtime)), backend_(std::move(backend)) {}

    namespace
    {
        /// Reject analytics changes that the device would misapply or drop.
        Result<void> validate_analytics(
            const std::string& configuration, const std::vector<AnalyticsConfig>& configs,
            OperationId operation
        )
        {
            if (configuration.empty())
                return invalid<void>(operation, "analytics configuration token is required");
            if (configs.empty()) return invalid<void>(operation, "nothing to modify");
            for (const auto& config : configs)
                if (config.name.empty() || config.type.empty())
                    return invalid<void>(operation, "modules and rules need a name and type");
            return {};
        }
    } // namespace

    Task<Result<std::vector<AnalyticsConfig>>>
    AnalyticsClient::get_modules(std::string configuration, CallOptions options)
    {
        auto backend = backend_;
        return async_call<std::vector<AnalyticsConfig>>(
            runtime_, options, OperationId::get_analytics_modules,
            [backend, configuration = std::move(configuration)]
            { return backend->analytics_modules(configuration); }
        );
    }
    Task<Result<void>> AnalyticsClient::modify_modules(
        std::string configuration, std::vector<AnalyticsConfig> modules, CallOptions options
    )
    {
        auto backend = backend_;
        return async_call<void>(
            runtime_, options, OperationId::modify_analytics_modules,
            [backend, configuration = std::move(configuration), modules = std::move(modules)]
            {
                auto valid =
                    validate_analytics(configuration, modules, OperationId::modify_analytics_modules);
                return valid ? backend->modify_analytics_modules(configuration, modules) : valid;
            }
        );
    }
    Task<Result<std::vector<AnalyticsConfig>>>
    AnalyticsClient::get_rules(std::string configuration, CallOptions options)
    {
        auto backend = backend_;
        return async_call<std::vector<AnalyticsConfig>>(
            runtime_, options, OperationId::get_analytics_rules,
            [backend, configuration = std::move(configuration)]
            { return backend->analytics_rules(configuration); }
        );
    }
    Task<Result<void>> AnalyticsClient::modify_rules(
        std::string configuration, std::vector<AnalyticsConfig> rules, CallOptions options
    )
    {
        auto backend = backend_;
        return async_call<void>(
            runtime_, options, OperationId::modify_analytics_rules,
            [backend, configuration = std::move(configuration), rules = std::move(rules)]
            {
                auto valid =
                    validate_analytics(configuration, rules, OperationId::modify_analytics_rules);
                return valid ? backend->modify_analytics_rules(configuration, rules) : valid;
            }
        );
    }

    struct PullPointSubscription::Impl
    {
        mutable std::mutex mutex;
        std::mutex operation_mutex;
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<ServiceBackend> backend;
        PullPointSubscriptionInfo information;
        PullPointSubscriptionState state{PullPointSubscriptionState::open};
        CancellationSource cancellation;
        /// Cancels in-flight pulls when Unsubscribe starts; replaced if it fails.
        CancellationSource pull_cancellation;
    };

    PullPointSubscription::PullPointSubscription(std::shared_ptr<Impl> impl)
        : impl_(std::move(impl)) {}

    PullPointSubscriptionInfo PullPointSubscription::info() const
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->information;
    }

    PullPointSubscriptionState PullPointSubscription::state() const noexcept
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->state;
    }

    Task<Result<std::vector<Event>>> PullPointSubscription::pull(
        std::chrono::milliseconds server_timeout,
        std::size_t maximum_messages,
        CallOptions options
    )
    {
        auto impl = impl_;
        const auto pull_token = [&]
        {
            std::lock_guard lock(impl->mutex);
            return impl->pull_cancellation.token();
        }();
        options.cancellation = CancellationToken::linked(
            CancellationToken::linked(options.cancellation, impl->cancellation.token()),
            pull_token
        );
        return async_call<std::vector<Event>>(
            impl->runtime, options, OperationId::event_pull,
            [impl, server_timeout, maximum_messages]
            {
                std::lock_guard operation_lock(impl->operation_mutex);
                if (maximum_messages == 0)
                    return Result<std::vector<Event>>(Error{
                        ErrorCategory::invalid_argument,
                        OperationId::event_pull,
                        "maximum_messages must be positive"
                    });
                std::string token;
                {
                    std::lock_guard lock(impl->mutex);
                    if (impl->state != PullPointSubscriptionState::open)
                        return Result<std::vector<Event>>(Error{
                            ErrorCategory::closed_handle, OperationId::event_pull,
                            "subscription is closed"
                        });
                    token = impl->information.token;
                }
                return impl->backend->pull_event_subscription(
                    token, server_timeout, maximum_messages
                );
            }
        );
    }

    Task<Result<PullPointSubscriptionInfo>> PullPointSubscription::renew(
        std::chrono::seconds requested_lifetime, CallOptions options
    )
    {
        auto impl = impl_;
        options.cancellation = CancellationToken::linked(
            options.cancellation, impl->cancellation.token()
        );
        return async_call<PullPointSubscriptionInfo>(
            impl->runtime, options, OperationId::event_renew,
            [impl, requested_lifetime]
            {
                std::lock_guard operation_lock(impl->operation_mutex);
                if (requested_lifetime <= std::chrono::seconds::zero())
                    return Result<PullPointSubscriptionInfo>(Error{
                        ErrorCategory::invalid_argument, OperationId::event_renew,
                        "requested lifetime must be positive"
                    });
                std::string token;
                {
                    std::lock_guard lock(impl->mutex);
                    if (impl->state != PullPointSubscriptionState::open)
                        return Result<PullPointSubscriptionInfo>(Error{
                            ErrorCategory::closed_handle, OperationId::event_renew,
                            "subscription is closed"
                        });
                    token = impl->information.token;
                }
                auto renewed = impl->backend->renew_event_subscription(
                    token, requested_lifetime
                );
                if (renewed)
                {
                    std::lock_guard lock(impl->mutex);
                    impl->information = renewed.value();
                }
                return renewed;
            }
        );
    }

    Task<Result<void>> PullPointSubscription::unsubscribe(CallOptions options)
    {
        auto impl = impl_;
        return async_call<void>(
            impl->runtime, options, OperationId::event_close,
            [impl]
            {
                {
                    std::lock_guard lock(impl->mutex);
                    if (impl->state == PullPointSubscriptionState::closed) return Result<void>{};
                    if (impl->state == PullPointSubscriptionState::closing)
                        return Result<void>(Error{
                            ErrorCategory::closed_handle, OperationId::event_close,
                            "subscription close is already in progress"
                        });
                    impl->state = PullPointSubscriptionState::closing;
                    // A pull in flight holds operation_mutex for up to its full
                    // server timeout. Cancelling it first lets Unsubscribe proceed
                    // within the caller's budget rather than after the long-poll.
                    impl->pull_cancellation.request_stop();
                }
                std::lock_guard operation_lock(impl->operation_mutex);
                std::string token;
                {
                    std::lock_guard lock(impl->mutex);
                    token = impl->information.token;
                }
                auto closed = impl->backend->close_event_subscription(token);
                {
                    std::lock_guard lock(impl->mutex);
                    impl->state = closed ? PullPointSubscriptionState::closed
                                         : PullPointSubscriptionState::open;
                    if (closed) impl->cancellation.request_stop();
                    else impl->pull_cancellation = CancellationSource{};
                }
                return closed;
            }, false
        );
    }

    Task<Result<CloseResult>> PullPointSubscription::close(
        std::chrono::milliseconds cleanup_budget
    )
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::max(cleanup_budget, std::chrono::milliseconds::zero());
        auto source = CancellationSource{};
        auto task = unsubscribe({deadline, source.token()});
        return Task<Result<CloseResult>>(
            [task = std::move(task)](auto done) mutable
            {
                std::move(task).start([done = std::move(done)](Result<void> result) mutable
                {
                    if (result) done(CloseResult{});
                    else if (result.error().category == ErrorCategory::timeout)
                        done(CloseResult{false, false});
                    else done(result.error());
                });
            }
        );
    }

    EventClient::EventClient(
        std::shared_ptr<Runtime> runtime, std::shared_ptr<ServiceBackend> backend
    ) : runtime_(std::move(runtime)), backend_(std::move(backend)) {}

    Task<Result<std::vector<EventTopic>>> EventClient::get_topics(CallOptions call)
    {
        auto backend = backend_;
        return async_call<std::vector<EventTopic>>(
            runtime_, call, OperationId::get_event_properties,
            [backend] { return backend->event_topics(); }
        );
    }

    Task<Result<std::shared_ptr<PullPointSubscription>>> EventClient::create_pullpoint(
        PullPointSubscriptionOptions options, CallOptions call
    )
    {
        auto runtime = runtime_;
        auto backend = backend_;
        return async_call<std::shared_ptr<PullPointSubscription>>(
            runtime, call, OperationId::event_subscribe,
            [runtime, backend, options]
            {
                if (options.requested_lifetime <= std::chrono::seconds::zero())
                    return Result<std::shared_ptr<PullPointSubscription>>(Error{
                        ErrorCategory::invalid_argument, OperationId::event_subscribe,
                        "requested lifetime must be positive"
                    });
                auto created = backend->create_filtered_event_subscription(
                    options.requested_lifetime, options.topics
                );
                if (!created)
                    return Result<std::shared_ptr<PullPointSubscription>>(created.error());
                auto impl = std::make_shared<PullPointSubscription::Impl>();
                impl->runtime = runtime;
                impl->backend = backend;
                impl->information = std::move(created.value());
                return Result<std::shared_ptr<PullPointSubscription>>(
                    std::shared_ptr<PullPointSubscription>(new PullPointSubscription(impl))
                );
            }
        );
    }

    Task<Result<std::shared_ptr<ManagedEventStream>>> EventClient::open_managed_stream(
        ManagedEventOptions options, CallOptions call
    )
    {
        auto runtime = runtime_;
        auto backend = backend_;
        auto created = create_pullpoint({options.requested_lifetime, options.topics}, call);
        return Task<Result<std::shared_ptr<ManagedEventStream>>>(
            [runtime, backend, options, created = std::move(created)](auto done) mutable
            {
                std::move(created).start(
                    [runtime, backend, options, done = std::move(done)](
                        Result<std::shared_ptr<PullPointSubscription>> subscription
                    ) mutable
                    {
                        if (!subscription) { done(subscription.error()); return; }
                        try
                        {
                            done(std::shared_ptr<ManagedEventStream>(new ManagedEventStream(
                                runtime, backend, std::move(subscription.value()), options
                            )));
                        }
                        catch (const std::exception& error)
                        {
                            done(Error{ErrorCategory::internal, OperationId::event_subscribe,
                                       error.what()});
                        }
                    }
                );
            }
        );
    }

    Result<double> normalize_ptz(double value, NativeRange range)
    {
        if (!std::isfinite(value) || !std::isfinite(range.minimum) ||
            !std::isfinite(range.maximum) || range.minimum >= range.maximum ||
            value < range.minimum || value > range.maximum)
        {
            return invalid<double>(OperationId::ptz_status, "invalid native PTZ value or range");
        }
        return 2.0 * (value - range.minimum) / (range.maximum - range.minimum) - 1.0;
    }
    Result<double> denormalize_ptz(double value, NativeRange range)
    {
        if (!std::isfinite(value) || value < -1.0 || value > 1.0 || !std::isfinite(range.minimum) ||
            !std::isfinite(range.maximum) || range.minimum >= range.maximum)
        {
            return invalid<double>(
                OperationId::ptz_status, "invalid normalized PTZ value or range"
            );
        }
        return range.minimum + (value + 1.0) * (range.maximum - range.minimum) / 2.0;
    }

    MediaClient::MediaClient(std::shared_ptr<Runtime> r, std::shared_ptr<ServiceBackend> b)
        : runtime_(std::move(r)), backend_(std::move(b))
    {
    }
    Task<Result<std::vector<MediaProfile>>> MediaClient::get_profiles(CallOptions o)
    {
        auto b = backend_;
        return async_call<std::vector<MediaProfile>>(
            runtime_,
            o,
            OperationId::get_profiles,
            [b]
            {
                return b->profiles();
            }
        );
    }
    Task<Result<MediaUri>> MediaClient::get_stream_uri(ProfileToken t, CallOptions o)
    {
        auto b = backend_;
        return async_call<MediaUri>(
            runtime_,
            o,
            OperationId::get_stream_uri,
            [b, t = std::move(t)]
            {
                return b->stream_uri(t);
            }
        );
    }
    Task<Result<std::vector<VideoSource>>> MediaClient::get_video_sources(CallOptions o)
    {
        auto b = backend_;
        return async_call<std::vector<VideoSource>>(
            runtime_, o, OperationId::get_video_sources, [b] { return b->video_sources(); }
        );
    }
    Task<Result<std::vector<ProfileConfiguration>>> MediaClient::get_profile_configurations(CallOptions o)
    {
        auto b = backend_;
        return async_call<std::vector<ProfileConfiguration>>(
            runtime_, o, OperationId::get_profiles, [b] { return b->profile_configurations(); }
        );
    }
    Task<Result<VideoEncoderOptions>>
    MediaClient::get_video_encoder_options(std::string configuration, CallOptions o)
    {
        auto b = backend_;
        return async_call<VideoEncoderOptions>(
            runtime_, o, OperationId::get_video_encoder_options,
            [b, configuration = std::move(configuration)] { return b->video_encoder_options(configuration); }
        );
    }
    Task<Result<void>> MediaClient::set_video_encoder_configuration(
        VideoEncoderConfiguration configuration, bool persist, CallOptions o
    )
    {
        auto b = backend_;
        return async_call<void>(
            runtime_, o, OperationId::set_video_encoder_configuration,
            [b, configuration = std::move(configuration), persist]
            {
                constexpr auto operation = OperationId::set_video_encoder_configuration;
                if (configuration.token.empty() || configuration.encoding.empty())
                    return invalid<void>(operation, "configuration token and encoding are required");
                if (configuration.resolution.width <= 0 || configuration.resolution.height <= 0 ||
                    configuration.frame_rate_limit <= 0 || configuration.encoding_interval <= 0 ||
                    configuration.bitrate_limit < 0 || !std::isfinite(configuration.quality) ||
                    (configuration.gov_length && *configuration.gov_length <= 0))
                    return invalid<void>(operation, "encoder values must be positive");
                return b->set_video_encoder_configuration(configuration, persist);
            }
        );
    }
    Task<Result<MediaUri>> MediaClient::get_snapshot_uri(ProfileToken t, CallOptions o)
    {
        auto b = backend_;
        return async_call<MediaUri>(
            runtime_,
            o,
            OperationId::get_snapshot_uri,
            [b, t = std::move(t)]
            {
                return b->snapshot_uri(t);
            }
        );
    }

    PtzClient::PtzClient(std::shared_ptr<Runtime> r, std::shared_ptr<ServiceBackend> b)
        : runtime_(std::move(r)), backend_(std::move(b)),
          safety_(std::make_shared<PtzSafetyState>(backend_, CancellationSource{}.token()))
    {
    }
    PtzClient::PtzClient(
        std::shared_ptr<Runtime> r,
        std::shared_ptr<ServiceBackend> b,
        std::shared_ptr<PtzSafetyState> safety
    ) : runtime_(std::move(r)), backend_(std::move(b)), safety_(std::move(safety))
    {
    }
    Task<Result<PtzSpaces>> PtzClient::spaces(ProfileToken t, CallOptions o)
    {
        auto b = backend_;
        return async_call<PtzSpaces>(
            runtime_,
            o,
            OperationId::get_ptz_configuration_options,
            [b, t = std::move(t)]
            {
                return b->ptz_spaces(t);
            }
        );
    }
    Task<Result<PtzStatus>> PtzClient::status(ProfileToken t, CallOptions o)
    {
        auto b = backend_;
        auto safety = safety_;
        return async_call<PtzStatus>(
            runtime_,
            o,
            OperationId::ptz_status,
            [b, safety, t = std::move(t)]
            {
                return safety->status(t, [b, &t] { return b->ptz_status(t); });
            }
        );
    }
    static Task<Result<MoveResult>> move_call(
        std::shared_ptr<Runtime> r,
        std::shared_ptr<ServiceBackend> b,
        std::shared_ptr<PtzSafetyState> safety,
        ProfileToken t,
        PtzVector v,
        bool relative,
        CallOptions o
    )
    {
        return async_call<MoveResult>(
            std::move(r),
            o,
            relative ? OperationId::relative_move : OperationId::continuous_move,
            [b, safety, t = std::move(t), v, relative, o]() mutable
            {
                if (!valid_ptz_vector(v))
                {
                    return Result<MoveResult>(Error{
                        ErrorCategory::invalid_argument,
                        relative ? OperationId::relative_move : OperationId::continuous_move,
                        "PTZ components must be finite and between -1 and +1"
                    });
                }
                const auto operation = relative ? OperationId::relative_move
                                                : OperationId::continuous_move;
                return safety->move(t, operation, relative, o,
                    [b, &t, v, relative] { return b->move(t, v, relative); });
            },
            false
        );
    }
    Task<Result<MoveResult>> PtzClient::continuous_move(ProfileToken t, PtzVector v, CallOptions o)
    {
        return move_call(runtime_, backend_, safety_, std::move(t), v, false, o);
    }
    Task<Result<MoveResult>> PtzClient::relative_move(ProfileToken t, PtzVector v, CallOptions o)
    {
        return move_call(runtime_, backend_, safety_, std::move(t), v, true, o);
    }
    Task<Result<MoveResult>> PtzClient::absolute_move(
        ProfileToken t, PtzVector position, std::optional<PtzVector> speed, CallOptions o
    )
    {
        auto b = backend_;
        auto safety = safety_;
        return async_call<MoveResult>(
            runtime_,
            o,
            OperationId::absolute_move,
            [b, safety, t = std::move(t), position, speed, o]
            {
                if (!valid_ptz_vector(position) || (speed && !valid_ptz_vector(*speed)))
                    return Result<MoveResult>(Error{
                        ErrorCategory::invalid_argument,
                        OperationId::absolute_move,
                        "PTZ components must be finite and between -1 and +1"
                    });
                return safety->move(t, OperationId::absolute_move, true, o,
                    [b, &t, position, speed] { return b->absolute_move(t, position, speed); });
            },
            false
        );
    }
    Task<Result<void>> PtzClient::stop(ProfileToken t, CallOptions o)
    {
        auto safety = safety_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::ptz_stop,
            [safety, t = std::move(t), o]
            {
                return safety->stop(t, o);
            }
        );
    }
    Task<Result<void>> PtzClient::emergency_stop(ProfileToken t, CallOptions o)
    {
        auto safety = safety_;
        // A safety Stop has to reach the device even when the caller's token is
        // already cancelled or its deadline already passed. Skipping the precheck
        // is only half of that: the request itself must also travel under an
        // uncancelled token, or the transport refuses it before it reaches the wire.
        return async_call<void>(
            runtime_,
            o,
            OperationId::ptz_stop,
            [safety, t = std::move(t), o]
            {
                return safety->stop(t, safety_options(o));
            },
            false
        );
    }
    Task<Result<std::vector<Preset>>> PtzClient::get_presets(ProfileToken t, CallOptions o)
    {
        auto b = backend_;
        return async_call<std::vector<Preset>>(
            runtime_,
            o,
            OperationId::get_presets,
            [b, t = std::move(t)]
            {
                return b->presets(t);
            }
        );
    }
    Task<Result<void>> PtzClient::goto_preset(ProfileToken t, PresetToken p, CallOptions o)
    {
        auto b = backend_;
        auto safety = safety_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::goto_preset,
            [b, safety, t = std::move(t), p = std::move(p), o]
            {
                return safety->preset(t, o,
                    [b, &t, &p] { return b->goto_preset(t, p); });
            },
            false
        );
    }

    Task<Result<PresetToken>>
    PtzClient::set_preset(ProfileToken profile, std::string name, PresetToken token, CallOptions o)
    {
        auto b = backend_;
        return async_call<PresetToken>(
            runtime_, o, OperationId::set_preset,
            [b, profile = std::move(profile), name = std::move(name), token = std::move(token)]
            {
                if (profile.empty() || (name.empty() && token.empty()))
                    return invalid<PresetToken>(OperationId::set_preset,
                                                "a profile and a preset name or token are required");
                return b->set_preset(profile, name, token);
            }
        );
    }
    Task<Result<void>> PtzClient::remove_preset(ProfileToken profile, PresetToken preset, CallOptions o)
    {
        auto b = backend_;
        return async_call<void>(
            runtime_, o, OperationId::remove_preset,
            [b, profile = std::move(profile), preset = std::move(preset)]
            {
                if (preset.empty()) return invalid<void>(OperationId::remove_preset, "preset token is required");
                return b->remove_preset(profile, preset);
            }
        );
    }
    Task<Result<std::vector<PresetTour>>> PtzClient::get_preset_tours(ProfileToken profile, CallOptions o)
    {
        auto b = backend_;
        return async_call<std::vector<PresetTour>>(
            runtime_, o, OperationId::get_preset_tours,
            [b, profile = std::move(profile)] { return b->preset_tours(profile); }
        );
    }
    Task<Result<std::string>> PtzClient::create_preset_tour(ProfileToken profile, CallOptions o)
    {
        auto b = backend_;
        return async_call<std::string>(
            runtime_, o, OperationId::create_preset_tour,
            [b, profile = std::move(profile)] { return b->create_preset_tour(profile); }
        );
    }
    Task<Result<void>> PtzClient::modify_preset_tour(ProfileToken profile, PresetTour tour, CallOptions o)
    {
        auto b = backend_;
        return async_call<void>(
            runtime_, o, OperationId::modify_preset_tour,
            [b, profile = std::move(profile), tour = std::move(tour)]
            {
                constexpr auto operation = OperationId::modify_preset_tour;
                if (tour.token.empty()) return invalid<void>(operation, "tour token is required");
                for (const auto& spot : tour.spots)
                    if (spot.preset.empty() || spot.stay_time.count() < 0)
                        return invalid<void>(operation, "every tour spot needs a preset");
                return b->modify_preset_tour(profile, tour);
            }
        );
    }
    Task<Result<void>> PtzClient::operate_preset_tour(
        ProfileToken profile, std::string tour, PresetTourOperation operation, CallOptions o
    )
    {
        auto b = backend_;
        return async_call<void>(
            runtime_, o, OperationId::operate_preset_tour,
            [b, profile = std::move(profile), tour = std::move(tour), operation]
            {
                if (tour.empty()) return invalid<void>(OperationId::operate_preset_tour, "tour token is required");
                return b->operate_preset_tour(profile, tour, operation);
            }
        );
    }
    Task<Result<void>> PtzClient::remove_preset_tour(ProfileToken profile, std::string tour, CallOptions o)
    {
        auto b = backend_;
        return async_call<void>(
            runtime_, o, OperationId::remove_preset_tour,
            [b, profile = std::move(profile), tour = std::move(tour)]
            {
                if (tour.empty()) return invalid<void>(OperationId::remove_preset_tour, "tour token is required");
                return b->remove_preset_tour(profile, tour);
            }
        );
    }
    Task<Result<PtzConfiguration>> PtzClient::get_configuration(ProfileToken profile, CallOptions o)
    {
        auto b = backend_;
        return async_call<PtzConfiguration>(
            runtime_, o, OperationId::get_ptz_configuration,
            [b, profile = std::move(profile)] { return b->ptz_configuration(profile); }
        );
    }
    Task<Result<void>>
    PtzClient::set_direction(ProfileToken profile, PtzDirection direction, bool persist, CallOptions o)
    {
        auto b = backend_;
        return async_call<void>(
            runtime_, o, OperationId::set_ptz_configuration,
            [b, profile = std::move(profile), direction = std::move(direction), persist]
            {
                if (!direction.eflip && !direction.reverse)
                    return invalid<void>(OperationId::set_ptz_configuration, "no direction setting given");
                return b->set_ptz_direction(profile, direction, persist);
            }
        );
    }

    struct DiscoverySession::State
    {
        std::mutex mutex;
        std::deque<DiscoveryObservation> queue;
        std::unordered_set<std::string> seen;
        std::deque<std::string> seen_order;
        std::size_t maximum;
        std::size_t maximum_seen;
        bool closed{false};

        /// Requires mutex. Record @p key, returning false when it was already seen.
        bool remember(std::string key)
        {
            if (!seen.insert(key).second)
            {
                return false;
            }
            // The queue is bounded but the deduplication keys are attacker-supplied
            // datagram fields, so they need their own bound to stay memory-safe.
            seen_order.push_back(std::move(key));
            while (seen_order.size() > maximum_seen)
            {
                seen.erase(seen_order.front());
                seen_order.pop_front();
            }
            return true;
        }
    };
    DiscoverySession::DiscoverySession(std::shared_ptr<Runtime> r, std::size_t m)
        : state_(std::make_shared<State>()), runtime_(std::move(r))
    {
        state_->maximum = m;
        state_->maximum_seen = std::max<std::size_t>(m * 4, 256);
    }
    Task<Result<void>> DiscoverySession::inject(DiscoveryObservation v, CallOptions o)
    {
        auto s = state_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::discovery_probe,
            [s, v = std::move(v)]() mutable
            {
                std::lock_guard l(s->mutex);
                if (s->closed)
                {
                    return Result<void>(Error{ErrorCategory::closed, OperationId::discovery_probe});
                }
                auto key = v.endpoint_reference + '|' + v.source + '|' + v.interface_id + '|' +
                           std::to_string(v.message_number);
                if (s->seen.contains(key))
                {
                    return Result<void>();
                }
                // Overflow is checked before the key is remembered. Remembering a
                // rejected observation would make every retransmission of it look
                // like a duplicate, losing the device even once space frees up.
                if (s->queue.size() >= s->maximum)
                {
                    return Result<void>(Error{
                        ErrorCategory::queue_overflow,
                        OperationId::discovery_probe,
                        "observation queue full"
                    });
                }
                s->remember(std::move(key));
                s->queue.push_back(std::move(v));
                return Result<void>();
            }
        );
    }
    Task<Result<DiscoveryObservation>> DiscoverySession::next(CallOptions o)
    {
        auto s = state_;
        return async_call<DiscoveryObservation>(
            runtime_,
            o,
            OperationId::discovery_probe,
            [s]
            {
                std::lock_guard l(s->mutex);
                if (s->queue.empty())
                {
                    return Result<DiscoveryObservation>(Error{
                        s->closed ? ErrorCategory::closed : ErrorCategory::timeout,
                        OperationId::discovery_probe,
                        "no observation available"
                    });
                }
                auto v = std::move(s->queue.front());
                s->queue.pop_front();
                return Result<DiscoveryObservation>(std::move(v));
            }
        );
    }
    Task<Result<void>> DiscoverySession::close(CallOptions o)
    {
        auto s = state_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::discovery_probe,
            [s]
            {
                std::lock_guard l(s->mutex);
                s->closed = true;
                s->queue.clear();
                return Result<void>();
            }
        );
    }

    struct ManagedEventStream::State
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::deque<EventOrHealth> queue;
        std::size_t maximum;
        bool closed{false};
        bool automatic{false};
        std::optional<Error> terminal_error;

        // Automatic-mode worker context. The worker reads these through its own
        // shared reference, so it never touches the ManagedEventStream object.
        std::shared_ptr<Runtime> runtime;
        std::shared_ptr<ServiceBackend> backend;
        std::shared_ptr<PullPointSubscription> subscription;
        ManagedEventOptions options;
        CancellationSource stop;
        std::mutex worker_mutex;
        std::thread worker;

        static bool is_loss_marker(const EventOrHealth& item)
        {
            const auto* health = std::get_if<StreamHealth>(&item);
            return health && *health == StreamHealth::events_dropped;
        }

        /**
         * @brief Requires mutex. Append @p item, honouring the queue bound.
         *
         * Whenever entries are discarded a single events_dropped marker is folded
         * into the head of the queue. Keeping the marker in band means the consumer
         * is told about the loss even while the queue stays permanently full, and
         * counting it against the bound means the queue never exceeds its maximum.
         */
        void push(EventOrHealth item)
        {
            if (maximum == 0)
            {
                return;
            }
            bool discarded = false;
            while (queue.size() + 1 > maximum)
            {
                queue.pop_front();
                discarded = true;
            }
            queue.push_back(std::move(item));
            if (!discarded || (!queue.empty() && is_loss_marker(queue.front())))
            {
                return;
            }
            while (queue.size() + 1 > maximum)
            {
                queue.pop_front();
            }
            queue.push_front(StreamHealth::events_dropped);
        }

        /**
         * @brief Stop the worker and wait for it unless that could deadlock.
         *
         * The worker waits on completions delivered by the Runtime thread. Joining
         * it from that thread would wait forever, so there the worker is detached
         * instead; it holds its own reference to this State and exits promptly
         * once stop is requested.
         */
        void stop_worker() noexcept
        {
            stop.request_stop();
            changed.notify_all();
            std::lock_guard lock(worker_mutex);
            if (!worker.joinable() || worker.get_id() == std::this_thread::get_id()) return;
            if (Runtime::on_worker_thread()) worker.detach();
            else worker.join();
        }
    };
    ManagedEventStream::ManagedEventStream(
        std::shared_ptr<Runtime> r, std::shared_ptr<ServiceBackend> b, std::size_t m
    )
        : state_(std::make_shared<State>()), runtime_(std::move(r)), backend_(std::move(b))
    {
        state_->maximum = m;
    }
    ManagedEventStream::ManagedEventStream(
        std::shared_ptr<Runtime> r,
        std::shared_ptr<ServiceBackend> b,
        std::shared_ptr<PullPointSubscription> subscription,
        ManagedEventOptions options
    ) : state_(std::make_shared<State>()), runtime_(std::move(r)), backend_(std::move(b))
    {
        if (options.queue_capacity == 0 || options.pull_limit == 0 ||
            options.pull_timeout <= std::chrono::milliseconds::zero() ||
            options.minimum_backoff <= std::chrono::milliseconds::zero() ||
            options.maximum_backoff < options.minimum_backoff ||
            options.requested_lifetime <= std::chrono::seconds::zero())
            throw std::invalid_argument("invalid managed Event options");
        state_->maximum = options.queue_capacity;
        state_->automatic = true;
        state_->runtime = runtime_;
        state_->backend = backend_;
        state_->subscription = std::move(subscription);
        state_->options = options;
        std::lock_guard lock(state_->worker_mutex);
        state_->worker = std::thread([state = state_] { run_managed(state); });
    }

    ManagedEventStream::~ManagedEventStream()
    {
        if (state_->automatic) state_->stop_worker();
    }

    void ManagedEventStream::run_managed(std::shared_ptr<State> state)
    {
        const auto& options = state->options;
        const auto stop = state->stop.token();
        auto backoff = options.minimum_backoff;
        bool reconnecting = false;
        const auto interrupted_wait = [&](std::chrono::milliseconds duration)
        {
            const auto end = std::chrono::steady_clock::now() + duration;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < end)
                std::this_thread::sleep_for(std::min(
                    std::chrono::milliseconds(20),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        end - std::chrono::steady_clock::now()
                    )
                ));
        };
        while (!stop.stop_requested())
        {
            auto current = state->subscription;
            const auto information = current->info();
            const auto lifetime = options.requested_lifetime;
            const auto margin = std::max(
                std::chrono::seconds(5), lifetime / 5
            );
            if (std::chrono::system_clock::now() + margin >= information.termination_time)
            {
                auto renewed = wait_task(current->renew(
                    lifetime,
                    {std::chrono::steady_clock::now() + std::chrono::seconds(10), stop}
                ));
                if (!renewed && renewed.error().category != ErrorCategory::cancelled)
                    reconnecting = true;
            }
            if (!reconnecting)
            {
                auto pulled = wait_task(current->pull(
                    options.pull_timeout,
                    options.pull_limit,
                    {std::chrono::steady_clock::now() + options.pull_timeout +
                         std::chrono::seconds(2), stop}
                ));
                if (pulled)
                {
                    const bool empty = pulled.value().empty();
                    {
                        std::lock_guard lock(state->mutex);
                        for (auto& event : pulled.value()) state->push(std::move(event));
                    }
                    state->changed.notify_all();
                    backoff = options.minimum_backoff;
                    if (empty) interrupted_wait(std::chrono::milliseconds(20));
                    continue;
                }
                if (pulled.error().category == ErrorCategory::cancelled) break;
                if (!pulled.error().retryable &&
                    pulled.error().category != ErrorCategory::network &&
                    pulled.error().category != ErrorCategory::timeout)
                {
                    {
                        std::lock_guard lock(state->mutex);
                        state->terminal_error = pulled.error();
                    }
                    state->changed.notify_all();
                    break;
                }
                reconnecting = true;
            }
            interrupted_wait(backoff);
            if (stop.stop_requested()) break;
            EventClient events(state->runtime, state->backend);
            auto replacement = wait_task(events.create_pullpoint(
                {options.requested_lifetime, options.topics},
                {std::chrono::steady_clock::now() + std::chrono::seconds(10), stop}
            ));
            if (replacement)
            {
                // Release the abandoned subscription. Without this every reconnect
                // leaves one behind on the device and in the backend's table.
                (void)wait_task(current->unsubscribe(
                    {std::chrono::steady_clock::now() + std::chrono::seconds(2),
                     CancellationSource{}.token()}
                ));
                state->subscription = std::move(replacement.value());
                {
                    std::lock_guard lock(state->mutex);
                    state->push(StreamHealth::possible_event_loss);
                }
                state->changed.notify_all();
                reconnecting = false;
                backoff = options.minimum_backoff;
            }
            else
                backoff = std::min(options.maximum_backoff, backoff * 2);
        }
    }
    Task<Result<void>> ManagedEventStream::poll(std::size_t maximum, CallOptions o)
    {
        auto s = state_;
        auto b = backend_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::event_pull,
            [s, b, maximum]
            {
                auto renewed = b->renew_events();
                if (!renewed)
                {
                    return renewed;
                }
                auto events = b->pull_events(maximum);
                if (!events)
                {
                    return Result<void>(events.error());
                }
                std::lock_guard l(s->mutex);
                if (s->closed)
                {
                    return Result<void>(Error{ErrorCategory::closed, OperationId::event_pull});
                }
                for (auto& e : events.value())
                {
                    s->push(std::move(e));
                }
                return Result<void>();
            }
        );
    }
    Task<Result<EventOrHealth>> ManagedEventStream::next(CallOptions o)
    {
        auto s = state_;
        return async_call<EventOrHealth>(
            runtime_,
            o,
            OperationId::event_pull,
            [s, o]
            {
                std::unique_lock l(s->mutex);
                if (s->automatic)
                {
                    // Cancellation cannot notify the condition variable, so the wait
                    // is sliced; waiting straight to the deadline would ignore a
                    // cancelled caller for the rest of it.
                    while (s->queue.empty() && !s->closed && !s->terminal_error &&
                           !o.cancellation.stop_requested() &&
                           std::chrono::steady_clock::now() < o.deadline)
                        s->changed.wait_until(l, std::min(
                            o.deadline,
                            std::chrono::steady_clock::now() + std::chrono::milliseconds(20)
                        ));
                }
                if (s->terminal_error) return Result<EventOrHealth>(*s->terminal_error);
                if (s->queue.empty())
                {
                    return Result<EventOrHealth>(Error{
                        s->closed ? ErrorCategory::closed : ErrorCategory::timeout,
                        OperationId::event_pull,
                        "no event available"
                    });
                }
                auto v = std::move(s->queue.front());
                s->queue.pop_front();
                return Result<EventOrHealth>(std::move(v));
            }
        );
    }
    Task<Result<void>> ManagedEventStream::mark_reconnected(CallOptions o)
    {
        auto s = state_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::event_renew,
            [s]
            {
                std::lock_guard l(s->mutex);
                if (s->closed)
                {
                    return Result<void>(Error{ErrorCategory::closed, OperationId::event_renew});
                }
                // Enqueueing through push() means an event evicted to make room for
                // this marker is still reported as a drop, rather than vanishing.
                s->push(StreamHealth::possible_event_loss);
                return Result<void>();
            }
        );
    }
    Task<Result<void>> ManagedEventStream::close(CallOptions o)
    {
        if (state_->automatic)
        {
            auto self = shared_from_this();
            return Task<Result<void>>([self = std::move(self), o](auto done) mutable
            {
                self->state_->stop.request_stop();
                self->state_->changed.notify_all();
                // Waiting for the worker and the Unsubscribe happens on a helper
                // thread. Doing it here would deadlock whenever close() is started
                // from a completion, because the worker's own completions are
                // delivered by that same Runtime thread.
                std::thread([self, o, done = std::move(done)]() mutable
                {
                    self->state_->stop_worker();
                    std::shared_ptr<PullPointSubscription> subscription;
                    {
                        std::lock_guard lock(self->state_->worker_mutex);
                        subscription = self->state_->subscription;
                    }
                    auto closed = wait_task(subscription->unsubscribe(o));
                    {
                        std::lock_guard lock(self->state_->mutex);
                        self->state_->closed = true;
                        self->state_->queue.clear();
                    }
                    self->state_->changed.notify_all();
                    auto runtime = self->runtime_;
                    auto deliver = [done = std::move(done), closed = std::move(closed)]() mutable
                    {
                        done(std::move(closed));
                    };
                    if (!runtime || !runtime->post(deliver)) deliver();
                }).detach();
            });
        }
        auto s = state_;
        auto b = backend_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::event_close,
            [s, b]
            {
                auto closed = b->close_events();
                std::lock_guard l(s->mutex);
                s->closed = true;
                s->queue.clear();
                s->changed.notify_all();
                return closed;
            }
        );
    }
} // namespace wlonvif
