#include <cmath>
#include <deque>
#include <unordered_set>
#include <wlonvif/wlonvif.hpp>
#include "network_services.hpp"

namespace wlonvif
{
    namespace
    {
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
                                set_network_call_options(options);
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
                                clear_network_call_options();
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
        : runtime_(std::move(r)), backend_(std::move(b))
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
        return async_call<PtzStatus>(
            runtime_,
            o,
            OperationId::ptz_status,
            [b, t = std::move(t)]
            {
                return b->ptz_status(t);
            }
        );
    }
    static Task<Result<MoveResult>> move_call(
        std::shared_ptr<Runtime> r,
        std::shared_ptr<ServiceBackend> b,
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
            [b, t = std::move(t), v, relative, o]() mutable
            {
                if (!valid_ptz_vector(v))
                {
                    return Result<MoveResult>(Error{
                        ErrorCategory::invalid_argument,
                        relative ? OperationId::relative_move : OperationId::continuous_move,
                        "PTZ components must be finite and between -1 and +1"
                    });
                }
                if (o.cancellation.stop_requested() ||
                    std::chrono::steady_clock::now() >= o.deadline)
                {
                    set_network_call_options(safety_options(o));
                    auto stopped = b->stop(t);
                    set_network_call_options(o);
                    Error e{
                        o.cancellation.stop_requested() ? ErrorCategory::cancelled
                                                        : ErrorCategory::timeout,
                        relative ? OperationId::relative_move : OperationId::continuous_move,
                        "movement cancelled",
                        false,
                        true
                    };
                    if (!stopped)
                    {
                        e.message += "; Stop failed: " + stopped.error().message;
                    }
                    return Result<MoveResult>(std::move(e));
                }
                return b->move(t, v, relative);
            },
            false
        );
    }
    Task<Result<MoveResult>> PtzClient::continuous_move(ProfileToken t, PtzVector v, CallOptions o)
    {
        return move_call(runtime_, backend_, std::move(t), v, false, o);
    }
    Task<Result<MoveResult>> PtzClient::relative_move(ProfileToken t, PtzVector v, CallOptions o)
    {
        return move_call(runtime_, backend_, std::move(t), v, true, o);
    }
    Task<Result<MoveResult>> PtzClient::absolute_move(
        ProfileToken t, PtzVector position, std::optional<PtzVector> speed, CallOptions o
    )
    {
        auto b = backend_;
        return async_call<MoveResult>(
            runtime_,
            o,
            OperationId::absolute_move,
            [b, t = std::move(t), position, speed]
            {
                if (!valid_ptz_vector(position) || (speed && !valid_ptz_vector(*speed)))
                    return Result<MoveResult>(Error{
                        ErrorCategory::invalid_argument,
                        OperationId::absolute_move,
                        "PTZ components must be finite and between -1 and +1"
                    });
                return b->absolute_move(t, position, speed);
            }
        );
    }
    Task<Result<void>> PtzClient::stop(ProfileToken t, CallOptions o)
    {
        auto b = backend_;
        return async_call<void>(
            runtime_,
            o,
            OperationId::ptz_stop,
            [b, t = std::move(t)]
            {
                return b->stop(t);
            }
        );
    }
    Task<Result<void>> PtzClient::emergency_stop(ProfileToken t, CallOptions o)
    {
        auto b = backend_;
        // A safety Stop has to reach the device even when the caller's token is
        // already cancelled or its deadline already passed. Skipping the precheck
        // is only half of that: the request itself must also travel under an
        // uncancelled token, or the transport refuses it before it reaches the wire.
        return async_call<void>(
            runtime_,
            o,
            OperationId::ptz_stop,
            [b, t = std::move(t), o]
            {
                set_network_call_options(safety_options(o));
                auto stopped = b->stop(t);
                set_network_call_options(o);
                return stopped;
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
        return async_call<void>(
            runtime_,
            o,
            OperationId::goto_preset,
            [b, t = std::move(t), p = std::move(p)]
            {
                return b->goto_preset(t, p);
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
                if (!s->remember(std::move(key)))
                {
                    return Result<void>();
                }
                if (s->queue.size() >= s->maximum)
                {
                    return Result<void>(Error{
                        ErrorCategory::queue_overflow,
                        OperationId::discovery_probe,
                        "observation queue full"
                    });
                }
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
        std::deque<EventOrHealth> queue;
        std::size_t maximum;
        bool closed{false};

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
    };
    ManagedEventStream::ManagedEventStream(
        std::shared_ptr<Runtime> r, std::shared_ptr<ServiceBackend> b, std::size_t m
    )
        : state_(std::make_shared<State>()), runtime_(std::move(r)), backend_(std::move(b))
    {
        state_->maximum = m;
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
            [s]
            {
                std::lock_guard l(s->mutex);
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
                return closed;
            }
        );
    }
} // namespace wlonvif
