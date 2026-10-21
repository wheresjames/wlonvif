#include <wlonvif/wlonvif.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <future>

namespace wlonvif
{
    namespace
    {
        double component(const std::optional<double>& value)
        {
            return value.value_or(0.0);
        }

        bool zero(const PtzVector& value, double epsilon = 1e-9)
        {
            return std::abs(component(value.pan)) <= epsilon &&
                   std::abs(component(value.tilt)) <= epsilon &&
                   std::abs(component(value.zoom)) <= epsilon;
        }

        bool changed(const PtzVector& a, const PtzVector& b, double epsilon)
        {
            return std::abs(component(a.pan) - component(b.pan)) >= epsilon ||
                   std::abs(component(a.tilt) - component(b.tilt)) >= epsilon ||
                   std::abs(component(a.zoom) - component(b.zoom)) >= epsilon;
        }

        PtzVector explicit_vector(double pan, double tilt, double zoom)
        {
            return PtzVector{pan, tilt, zoom};
        }

        /**
         * @brief Run a cold task to completion on the calling thread.
         *
         * The promise is shared-owned. A promise on this stack frame could be
         * destroyed by the returning waiter while the completing thread is still
         * inside set_value().
         */
        template <class T> T wait_task(Task<T> task)
        {
            auto promise = std::make_shared<std::promise<T>>();
            auto future = promise->get_future();
            std::move(task).start(
                [promise](T result)
                {
                    promise->set_value(std::move(result));
                }
            );
            return future.get();
        }

        CallOptions internal_options(std::chrono::milliseconds duration = std::chrono::seconds(3))
        {
            return {
                std::chrono::steady_clock::now() + duration,
                CancellationSource{}.token()
            };
        }

        Result<void> validate_config(const PtzControllerConfig& config)
        {
            const auto unit = [](double value) { return std::isfinite(value) && value >= 0 && value <= 1; };
            const auto range = [](const std::optional<NativeRange>& value)
            {
                return !value || (std::isfinite(value->minimum) && std::isfinite(value->maximum) &&
                    value->minimum >= -1 && value->maximum <= 1 && value->minimum <= value->maximum);
            };
            if (config.tick_interval.count() <= 0 || config.command_interval.count() <= 0 ||
                config.command_refresh.count() < 0 || config.status_interval.count() <= 0 ||
                config.ramp_up.count() < 0 ||
                config.ramp_down.count() < 0 || config.input_watchdog.count() <= 0 ||
                config.maximum_run.count() <= 0 || !unit(config.dead_zone) ||
                config.dead_zone >= 1 || !std::isfinite(config.response_exponent) ||
                config.response_exponent <= 0 ||
                !unit(config.maximum_pan) || !unit(config.maximum_tilt) ||
                !unit(config.maximum_zoom) || !unit(config.minimum_effective_speed) ||
                !unit(config.command_epsilon) || !unit(config.zoomed_pan_tilt_scale) ||
                !range(config.pan_limits) || !range(config.tilt_limits) || !range(config.zoom_limits))
            {
                return Error{
                    ErrorCategory::invalid_argument,
                    OperationId::continuous_move,
                    "invalid PTZ controller configuration"
                };
            }
            double previous_zoom = -2;
            for (const auto& point : config.zoom_speed_curve)
            {
                if (!std::isfinite(point.zoom) || point.zoom < -1 || point.zoom > 1 ||
                    !unit(point.scale) || point.zoom <= previous_zoom)
                    return Error{ErrorCategory::invalid_argument, OperationId::continuous_move,
                                 "invalid PTZ zoom-speed curve"};
                previous_zoom = point.zoom;
            }
            for (const auto& zone : config.keep_out_zones)
            {
                if (!std::isfinite(zone.minimum_pan) || !std::isfinite(zone.maximum_pan) ||
                    !std::isfinite(zone.minimum_tilt) || !std::isfinite(zone.maximum_tilt) ||
                    zone.minimum_pan < -1 || zone.maximum_pan > 1 ||
                    zone.minimum_tilt < -1 || zone.maximum_tilt > 1 ||
                    zone.minimum_pan > zone.maximum_pan || zone.minimum_tilt > zone.maximum_tilt)
                    return Error{ErrorCategory::invalid_argument, OperationId::continuous_move,
                                 "invalid PTZ keep-out zone"};
            }
            return {};
        }
    }

    PtzControlLaw::PtzControlLaw(PtzControllerConfig config) : config_(config)
    {
        configured_ = static_cast<bool>(validate_config(config));
    }

    Result<void> PtzControlLaw::set_input(PtzVector input)
    {
        if (!configured_)
            return Error{ErrorCategory::invalid_argument, OperationId::continuous_move,
                         "invalid PTZ control-law configuration"};
        const auto valid = [](const std::optional<double>& value)
        {
            return !value || (std::isfinite(*value) && *value >= -1 && *value <= 1);
        };
        if (!valid(input.pan) || !valid(input.tilt) || !valid(input.zoom))
        {
            return Error{
                ErrorCategory::invalid_argument,
                OperationId::continuous_move,
                "PTZ input components must be finite and between -1 and +1"
            };
        }
        input_ = explicit_vector(component(input.pan), component(input.tilt), component(input.zoom));
        return {};
    }

    void PtzControlLaw::release() noexcept
    {
        input_ = explicit_vector(0, 0, 0);
    }

    PtzVector PtzControlLaw::advance(std::chrono::milliseconds elapsed, double normalized_zoom)
    {
        return advance(elapsed, PtzVector{{}, {}, normalized_zoom});
    }

    PtzVector PtzControlLaw::advance(
        std::chrono::milliseconds elapsed, PtzVector reported_position
    )
    {
        if (!configured_) return explicit_vector(0, 0, 0);
        const auto shape = [&](double value, double maximum, bool invert)
        {
            value = std::clamp(value, -1.0, 1.0) * (invert ? -1.0 : 1.0);
            const double magnitude = std::abs(value);
            if (magnitude <= config_.dead_zone) return 0.0;
            const double normalized = (magnitude - config_.dead_zone) / (1.0 - config_.dead_zone);
            double output = std::pow(normalized, config_.response_exponent) * maximum;
            output = std::max(output, std::min(config_.minimum_effective_speed, maximum));
            return std::copysign(output, value);
        };
        double pan = shape(component(input_.pan), config_.maximum_pan, config_.invert_pan);
        double tilt = shape(component(input_.tilt), config_.maximum_tilt, config_.invert_tilt);
        double zoom = shape(component(input_.zoom), config_.maximum_zoom, config_.invert_zoom);
        const double magnitude = std::hypot(pan, tilt);
        const double diagonal_limit = std::max(config_.maximum_pan, config_.maximum_tilt);
        if (magnitude > diagonal_limit && magnitude > 0)
        {
            pan *= diagonal_limit / magnitude;
            tilt *= diagonal_limit / magnitude;
        }
        double normalized_zoom = component(reported_position.zoom);
        if (!reported_position.zoom || !std::isfinite(normalized_zoom)) normalized_zoom = -1.0;
        const double zoom_fraction = std::clamp((normalized_zoom + 1.0) * 0.5, 0.0, 1.0);
        double zoom_scale = 1.0 - zoom_fraction * (1.0 - config_.zoomed_pan_tilt_scale);
        if (!config_.zoom_speed_curve.empty())
        {
            if (normalized_zoom <= config_.zoom_speed_curve.front().zoom)
                zoom_scale = config_.zoom_speed_curve.front().scale;
            else if (normalized_zoom >= config_.zoom_speed_curve.back().zoom)
                zoom_scale = config_.zoom_speed_curve.back().scale;
            else
            {
                const auto upper = std::upper_bound(
                    config_.zoom_speed_curve.begin(), config_.zoom_speed_curve.end(), normalized_zoom,
                    [](double value, const PtzZoomSpeedPoint& point) { return value < point.zoom; });
                const auto& high = *upper;
                const auto& low = *(upper - 1);
                const double fraction = (normalized_zoom - low.zoom) / (high.zoom - low.zoom);
                zoom_scale = low.scale + fraction * (high.scale - low.scale);
            }
        }
        const auto block_at_limit = [](double velocity, const std::optional<double>& position,
                                       const std::optional<NativeRange>& limits)
        {
            if (!position || !limits) return velocity;
            if ((velocity < 0 && *position <= limits->minimum) ||
                (velocity > 0 && *position >= limits->maximum)) return 0.0;
            return velocity;
        };
        pan = block_at_limit(pan, reported_position.pan, config_.pan_limits);
        tilt = block_at_limit(tilt, reported_position.tilt, config_.tilt_limits);
        zoom = block_at_limit(zoom, reported_position.zoom, config_.zoom_limits);
        if (reported_position.pan && reported_position.tilt)
        {
            for (const auto& zone : config_.keep_out_zones)
            {
                const bool tilt_inside = *reported_position.tilt >= zone.minimum_tilt &&
                                         *reported_position.tilt <= zone.maximum_tilt;
                const bool pan_inside = *reported_position.pan >= zone.minimum_pan &&
                                        *reported_position.pan <= zone.maximum_pan;
                if (tilt_inside && ((pan > 0 && *reported_position.pan <= zone.minimum_pan) ||
                                    (pan < 0 && *reported_position.pan >= zone.maximum_pan))) pan = 0;
                if (pan_inside && ((tilt > 0 && *reported_position.tilt <= zone.minimum_tilt) ||
                                   (tilt < 0 && *reported_position.tilt >= zone.maximum_tilt))) tilt = 0;
            }
        }
        desired_ = explicit_vector(pan * zoom_scale, tilt * zoom_scale, zoom);

        const double seconds = std::max(0.0, elapsed.count() / 1000.0);
        const auto ramp = [&](double current, double target)
        {
            if (config_.ramp_mode == PtzRampMode::firmware) return target;
            const bool accelerating = std::abs(target) > std::abs(current) && current * target >= 0;
            const auto duration = accelerating ? config_.ramp_up : config_.ramp_down;
            if (duration.count() == 0) return target;
            const double step = seconds * 1000.0 / duration.count();
            return current + std::clamp(target - current, -step, step);
        };
        commanded_ = explicit_vector(
            ramp(component(commanded_.pan), component(desired_.pan)),
            ramp(component(commanded_.tilt), component(desired_.tilt)),
            ramp(component(commanded_.zoom), component(desired_.zoom))
        );
        return commanded_;
    }

    PtzVector PtzControlLaw::desired() const { return desired_; }
    PtzVector PtzControlLaw::commanded() const { return commanded_; }
    void PtzControlLaw::reset() noexcept
    {
        input_ = desired_ = commanded_ = explicit_vector(0, 0, 0);
    }

    struct PtzController::Impl
    {
        std::shared_ptr<PtzClient> client;
        ProfileToken profile;
        PtzControllerConfig config;
        PtzControlLaw law;
        mutable std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable status_wake;
        PtzControllerState state;
        std::string owner;
        std::uint64_t local_sequence{};
        std::chrono::steady_clock::time_point last_input{};
        std::chrono::steady_clock::time_point run_started{};
        bool input_active{};
        bool urgent_stop{};
        bool stop_completed{};
        bool closed{};
        PtzVector reported_position;
        std::chrono::steady_clock::time_point latest_input_time{};
        CancellationSource worker_cancel;
        std::jthread worker;
        std::jthread status_worker;

        Impl(std::shared_ptr<PtzClient> c, ProfileToken p, PtzControllerConfig cfg)
            : client(std::move(c)), profile(std::move(p)), config(cfg), law(cfg)
        {
        }

        void record_error(Error error)
        {
            std::lock_guard lock(mutex);
            state.last_error = std::move(error);
            if (!closed) state.mode = PtzControllerMode::degraded;
        }

        void send_stop()
        {
            auto result = wait_task(client->emergency_stop(profile, internal_options()));
            std::lock_guard lock(mutex);
            ++state.stops_sent;
            if (!result)
            {
                state.last_error = result.error();
                state.mode = PtzControllerMode::degraded;
            }
            else if (!closed)
            {
                state.mode = PtzControllerMode::idle;
            }
        }

        CallOptions worker_options() const
        {
            return {std::chrono::steady_clock::now() + std::chrono::seconds(3),
                    worker_cancel.token()};
        }

        void run(std::stop_token token)
        {
            auto previous = std::chrono::steady_clock::now();
            auto last_command = previous - config.command_interval;
            PtzVector transmitted = explicit_vector(0, 0, 0);
            bool device_active = false;
            while (!token.stop_requested())
            {
                const auto now = std::chrono::steady_clock::now();
                bool stop_now = false;
                {
                    std::lock_guard lock(mutex);
                    if (std::exchange(stop_completed, false))
                    {
                        device_active = false;
                        transmitted = explicit_vector(0, 0, 0);
                    }
                    if (input_active &&
                        (now - last_input >= config.input_watchdog ||
                         now - run_started >= config.maximum_run))
                    {
                        input_active = false;
                        law.release();
                        state.mode = PtzControllerMode::stopping;
                    }
                    stop_now = std::exchange(urgent_stop, false);
                    if (stop_now) law.reset();
                }
                if (stop_now)
                {
                    send_stop();
                    device_active = false;
                    transmitted = explicit_vector(0, 0, 0);
                }

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - previous);
                previous = now;
                PtzVector command;
                {
                    std::lock_guard lock(mutex);
                    command = law.advance(elapsed, reported_position);
                    state.desired = law.desired();
                    state.commanded = command;
                }
                if (now - last_command >= config.command_interval)
                {
                    const bool refresh = device_active && config.command_refresh.count() > 0 &&
                                         now - last_command >= config.command_refresh;
                    if (!zero(command) && (!device_active || refresh ||
                                           changed(command, transmitted, config.command_epsilon)))
                    {
                        const auto request_started = std::chrono::steady_clock::now();
                        auto result = wait_task(client->continuous_move(profile, command, worker_options()));
                        last_command = std::chrono::steady_clock::now();
                        if (!result)
                        {
                            if (!token.stop_requested()) record_error(result.error());
                        }
                        else
                        {
                            std::lock_guard lock(mutex);
                            ++state.commands_sent;
                            state.last_request_latency = std::chrono::duration_cast<std::chrono::microseconds>(
                                last_command - request_started);
                            if (latest_input_time.time_since_epoch().count() != 0)
                                state.last_input_to_command =
                                    std::chrono::duration_cast<std::chrono::microseconds>(
                                        last_command - latest_input_time);
                            transmitted = command;
                            device_active = true;
                        }
                    }
                    else if (zero(command) && device_active)
                    {
                        send_stop();
                        last_command = std::chrono::steady_clock::now();
                        device_active = false;
                        transmitted = explicit_vector(0, 0, 0);
                    }
                }
                std::unique_lock lock(mutex);
                wake.wait_for(lock, config.tick_interval, [&] { return token.stop_requested() || urgent_stop; });
            }
            if (device_active || !zero(law.commanded())) send_stop();
        }

        void run_status(std::stop_token token)
        {
            while (!token.stop_requested())
            {
                auto status = wait_task(client->status(profile, worker_options()));
                if (status)
                {
                    std::lock_guard lock(mutex);
                    reported_position = status.value().position_reliable
                        ? status.value().position : PtzVector{};
                    state.reported = status.value();
                }
                else if (!token.stop_requested()) record_error(status.error());
                std::unique_lock lock(mutex);
                status_wake.wait_for(
                    lock, config.status_interval, [&] { return token.stop_requested(); }
                );
            }
        }
    };

    Result<std::shared_ptr<PtzController>> PtzController::create(
        std::shared_ptr<PtzClient> client, ProfileToken profile, PtzControllerConfig config
    )
    {
        if (!client || profile.empty())
            return Error{ErrorCategory::invalid_argument, OperationId::continuous_move, "PTZ controller requires a client and profile"};
        if (auto valid = validate_config(config); !valid) return valid.error();
        auto impl = std::make_shared<Impl>(std::move(client), std::move(profile), config);
        auto controller = std::shared_ptr<PtzController>(new PtzController(impl));
        impl->worker = std::jthread([impl](std::stop_token token) { impl->run(token); });
        impl->status_worker =
            std::jthread([impl](std::stop_token token) { impl->run_status(token); });
        return controller;
    }

    PtzController::PtzController(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
    PtzController::~PtzController()
    {
        if (!impl_) return;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->closed) return;
            impl_->closed = true;
            impl_->state.mode = PtzControllerMode::closed;
            impl_->urgent_stop = true;
        }
        impl_->worker.request_stop();
        impl_->status_worker.request_stop();
        impl_->worker_cancel.request_stop();
        impl_->wake.notify_all();
        impl_->status_wake.notify_all();
        // The workers wait on completions delivered by the Runtime thread, so
        // joining them from that thread would never return. They own Impl, so
        // detaching lets them finish, including the final Stop, on their own.
        const bool detach = Runtime::on_worker_thread();
        for (auto* worker : {&impl_->worker, &impl_->status_worker})
        {
            if (!worker->joinable()) continue;
            if (detach) worker->detach();
            else worker->join();
        }
    }

    Result<void> PtzController::claim_manual(std::string owner)
    {
        if (owner.empty()) return Error{ErrorCategory::invalid_argument, OperationId::continuous_move, "manual owner must not be empty"};
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) return Error{ErrorCategory::closed, OperationId::continuous_move, "PTZ controller is closed"};
        if (!impl_->owner.empty() && impl_->owner != owner)
            return Error{ErrorCategory::authorization, OperationId::continuous_move, "manual control is owned by another input source"};
        impl_->owner = std::move(owner);
        return {};
    }

    Result<void> PtzController::set_manual_input(std::string_view owner, std::uint64_t sequence, PtzVector input)
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->closed) return Error{ErrorCategory::closed, OperationId::continuous_move, "PTZ controller is closed"};
        if (owner.empty() || owner != impl_->owner)
            return Error{ErrorCategory::authorization, OperationId::continuous_move, "manual input does not own the controller"};
        if (sequence <= impl_->state.accepted_sequence)
            return Error{ErrorCategory::invalid_argument, OperationId::continuous_move, "manual input sequence is stale"};
        if (auto accepted = impl_->law.set_input(input); !accepted) return accepted.error();
        if (impl_->input_active) ++impl_->state.coalesced_inputs;
        const auto now = std::chrono::steady_clock::now();
        if (!impl_->input_active) impl_->run_started = now;
        impl_->input_active = true;
        impl_->last_input = now;
        impl_->latest_input_time = now;
        impl_->state.peak_pending_inputs = std::max<std::size_t>(
            impl_->state.peak_pending_inputs, 1);
        impl_->state.accepted_sequence = sequence;
        impl_->state.mode = PtzControllerMode::manual;
        impl_->wake.notify_all();
        return {};
    }

    Result<void> PtzController::set_manual_input(PtzVector input)
    {
        std::uint64_t sequence = 0;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->owner.empty()) impl_->owner = "local";
            if (impl_->owner != "local")
                return Error{ErrorCategory::authorization, OperationId::continuous_move, "manual control is owned by another input source"};
            sequence = ++impl_->local_sequence;
        }
        return set_manual_input("local", sequence, input);
    }

    Result<void> PtzController::release_manual(std::string_view owner)
    {
        std::lock_guard lock(impl_->mutex);
        if (owner != impl_->owner) return Error{ErrorCategory::authorization, OperationId::ptz_stop, "manual input does not own the controller"};
        impl_->input_active = false;
        impl_->law.release();
        impl_->state.mode = PtzControllerMode::stopping;
        impl_->wake.notify_all();
        return {};
    }

    Result<void> PtzController::relinquish_manual(std::string_view owner)
    {
        std::lock_guard lock(impl_->mutex);
        if (owner != impl_->owner)
            return Error{ErrorCategory::authorization, OperationId::ptz_stop, "manual input does not own the controller"};
        if (impl_->input_active)
            return Error{ErrorCategory::invalid_argument, OperationId::ptz_stop, "release manual input before relinquishing ownership"};
        impl_->owner.clear();
        impl_->state.accepted_sequence = 0;
        return {};
    }

    Task<Result<void>> PtzController::stop(CallOptions options)
    {
        auto impl = impl_;
        return Task<Result<void>>([impl, options](auto completion) mutable
        {
            {
                std::lock_guard lock(impl->mutex);
                impl->input_active = false;
                impl->law.reset();
                impl->urgent_stop = false;
                impl->state.mode = PtzControllerMode::stopping;
            }
            std::move(impl->client->emergency_stop(impl->profile, options)).start(
                [impl, completion = std::move(completion)](Result<void> result) mutable
                {
                    {
                        std::lock_guard lock(impl->mutex);
                        ++impl->state.stops_sent;
                        impl->stop_completed = true;
                        if (!result) { impl->state.last_error = result.error(); impl->state.mode = PtzControllerMode::degraded; }
                        else if (!impl->closed) impl->state.mode = PtzControllerMode::idle;
                    }
                    impl->wake.notify_all();
                    completion(std::move(result));
                }
            );
        });
    }

    Task<Result<void>> PtzController::close(CallOptions options)
    {
        auto impl = impl_;
        return Task<Result<void>>([impl, options](auto completion) mutable
        {
            bool already_closed = false;
            {
                std::lock_guard lock(impl->mutex);
                already_closed = impl->closed;
                if (!already_closed)
                {
                    impl->closed = true;
                    impl->input_active = false;
                    impl->law.reset();
                    impl->state.mode = PtzControllerMode::closed;
                }
            }
            if (already_closed) { completion(Result<void>{}); return; }
            impl->worker.request_stop();
            impl->status_worker.request_stop();
            impl->worker_cancel.request_stop();
            impl->wake.notify_all();
            impl->status_wake.notify_all();
            std::thread(
                [impl, options, completion = std::move(completion)]() mutable
                {
                    if (impl->worker.joinable()) impl->worker.join();
                    if (impl->status_worker.joinable()) impl->status_worker.join();
                    std::move(impl->client->emergency_stop(impl->profile, options)).start(
                        [impl, completion = std::move(completion)](Result<void> result) mutable
                        {
                            {
                                std::lock_guard lock(impl->mutex);
                                ++impl->state.stops_sent;
                                if (!result) impl->state.last_error = result.error();
                            }
                            completion(std::move(result));
                        }
                    );
                }
            ).detach();
        });
    }

    PtzControllerState PtzController::state() const
    {
        std::lock_guard lock(impl_->mutex);
        return impl_->state;
    }
}
