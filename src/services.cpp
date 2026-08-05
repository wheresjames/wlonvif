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
            OperationId::ptz_status,
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
                if (o.cancellation.stop_requested() ||
                    std::chrono::steady_clock::now() >= o.deadline)
                {
                    auto stopped = b->stop(t);
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
        return stop(std::move(t), o);
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
        std::size_t maximum;
        bool closed{false};
    };
    DiscoverySession::DiscoverySession(std::shared_ptr<Runtime> r, std::size_t m)
        : state_(std::make_shared<State>()), runtime_(std::move(r))
    {
        state_->maximum = m;
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
                if (!s->seen.insert(key).second)
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
        std::size_t dropped{};
        bool closed{false};
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
                    if (s->queue.size() >= s->maximum)
                    {
                        s->queue.pop_front();
                        ++s->dropped;
                    }
                    s->queue.emplace_back(std::move(e));
                }
                if (s->dropped && s->queue.size() < s->maximum)
                {
                    s->queue.emplace_back(StreamHealth::events_dropped);
                    s->dropped = 0;
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
                if (s->queue.size() >= s->maximum)
                {
                    s->queue.pop_front();
                }
                s->queue.emplace_back(StreamHealth::possible_event_loss);
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
