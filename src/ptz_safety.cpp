#include "ptz_safety.hpp"
#include "network_services.hpp"

namespace wlonvif
{
    namespace
    {
        CallOptions bounded_safety_options(
            CallOptions requested,
            std::chrono::steady_clock::time_point upper =
                std::chrono::steady_clock::now() + std::chrono::seconds(5)
        )
        {
            const auto now = std::chrono::steady_clock::now();
            auto deadline = requested.deadline > now ? requested.deadline
                                                     : now + std::chrono::seconds(5);
            deadline = std::min(deadline, upper);
            return {deadline, CancellationSource{}.token()};
        }

        std::string failure_code(const Error& error)
        {
            std::string code;
            if (error.soap_fault)
                code = error.soap_fault->subcode.empty() ? error.soap_fault->code
                                                         : error.soap_fault->subcode;
            else if (error.http)
                code = std::to_string(error.http->status);
            if (code.size() > 128) code.resize(128);
            return code;
        }
    }

    PtzSafetyState::PtzSafetyState(
        std::shared_ptr<ServiceBackend> backend, CancellationToken cancellation
    ) : backend_(std::move(backend)), cancellation_(std::move(cancellation)) {}

    void PtzSafetyState::attach(Error& error, const SafetyActionDetail& detail)
    {
        error.safety_action = detail;
    }

    PtzSafetyState::Admission PtzSafetyState::admit(
        const ProfileToken& profile, OperationId operation, const std::string& what,
        CallOptions options
    )
    {
        std::lock_guard lock(mutex_);
        const bool cancelled = closing_ || cancellation_.stop_requested() ||
                               options.cancellation.stop_requested();
        if (cancelled || std::chrono::steady_clock::now() >= options.deadline)
        {
            return {Error{cancelled ? ErrorCategory::cancelled : ErrorCategory::timeout,
                          operation, what + (cancelled ? " cancelled" : " timed out"),
                          false, true},
                    0};
        }
        ++in_flight_;
        return {std::nullopt, stops_started_[profile]};
    }

    bool PtzSafetyState::record(
        const ProfileToken& profile, OperationId operation, bool finite, std::uint64_t stops_seen
    )
    {
        std::lock_guard lock(mutex_);
        movements_[profile] = Movement{operation, finite, ++clock_};
        // A Stop issued while this movement was in flight reached the device first,
        // so the movement now runs unopposed. The same holds once close has begun.
        return closing_ || stops_started_[profile] != stops_seen;
    }

    void PtzSafetyState::finish_in_flight() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            if (in_flight_ != 0) --in_flight_;
        }
        idle_.notify_all();
    }

    SafetyActionDetail PtzSafetyState::send_stop(
        const ProfileToken& profile, CallOptions options
    )
    {
        std::uint64_t started = 0;
        {
            std::lock_guard lock(mutex_);
            ++stops_started_[profile];
            started = ++clock_;
        }
        SafetyActionDetail detail;
        detail.attempted = true;
        Result<void> stopped = Error{ErrorCategory::internal, OperationId::ptz_stop};
        try
        {
            ScopedNetworkCallOptions scope(bounded_safety_options(options));
            stopped = backend_->stop(profile);
        }
        catch (const std::exception& error)
        {
            stopped = Error{ErrorCategory::internal, OperationId::ptz_stop, error.what()};
        }
        detail.succeeded = static_cast<bool>(stopped);
        if (stopped)
        {
            // Only a movement recorded before this Stop was sent is known halted.
            std::lock_guard lock(mutex_);
            const auto found = movements_.find(profile);
            if (found != movements_.end() && found->second.stamp < started)
                movements_.erase(found);
        }
        else
        {
            detail.failure_category = stopped.error().category;
            detail.failure_code = failure_code(stopped.error());
        }
        return detail;
    }

    Result<MoveResult> PtzSafetyState::guarded_move(
        const ProfileToken& profile,
        OperationId operation,
        bool finite,
        const std::string& what,
        CallOptions options,
        const std::function<Result<MoveResult>()>& invoke
    )
    {
        auto admission = admit(profile, operation, what, options);
        if (admission.rejection)
        {
            auto error = std::move(*admission.rejection);
            attach(error, send_stop(profile, options));
            return error;
        }
        Result<MoveResult> result = Error{ErrorCategory::internal, operation};
        try
        {
            result = invoke();
        }
        catch (const std::exception& error)
        {
            result = Error{ErrorCategory::internal, operation, error.what(), false, true};
        }
        if (result)
        {
            const bool raced = record(profile, operation, finite, admission.stops_seen);
            if (raced || result.value().request_may_have_been_applied)
            {
                auto detail = send_stop(profile, options);
                result.value().safety_action = detail;
                if (!detail.succeeded)
                {
                    Error stop_error{
                        detail.failure_category.value_or(ErrorCategory::internal),
                        OperationId::ptz_stop,
                        "best-effort PTZ Stop failed"
                    };
                    stop_error.safety_action = detail;
                    result.value().stop_error = std::move(stop_error);
                }
            }
            finish_in_flight();
            return result;
        }
        auto error = result.error();
        bool safety_required = error.request_may_have_been_applied ||
                               error.category == ErrorCategory::cancelled ||
                               error.category == ErrorCategory::timeout;
        if (error.request_may_have_been_applied &&
            record(profile, operation, finite, admission.stops_seen))
            safety_required = true;
        if (safety_required) attach(error, send_stop(profile, options));
        finish_in_flight();
        return error;
    }

    Result<MoveResult> PtzSafetyState::move(
        const ProfileToken& profile,
        OperationId operation,
        bool finite,
        CallOptions options,
        const std::function<Result<MoveResult>()>& invoke
    )
    {
        return guarded_move(profile, operation, finite, "movement", options, invoke);
    }

    Result<void> PtzSafetyState::preset(
        const ProfileToken& profile,
        CallOptions options,
        const std::function<Result<void>()>& invoke
    )
    {
        auto moved = guarded_move(
            profile, OperationId::goto_preset, true, "preset movement", options,
            [&invoke]() -> Result<MoveResult>
            {
                auto result = invoke();
                if (!result) return result.error();
                return MoveResult{};
            }
        );
        if (!moved) return moved.error();
        return {};
    }

    Result<void> PtzSafetyState::stop(const ProfileToken& profile, CallOptions options)
    {
        auto detail = send_stop(profile, options);
        if (detail.succeeded) return {};
        Error error{detail.failure_category.value_or(ErrorCategory::internal),
                    OperationId::ptz_stop, "PTZ Stop failed"};
        error.safety_action = std::move(detail);
        return error;
    }

    Result<PtzStatus> PtzSafetyState::status(
        const ProfileToken& profile, const std::function<Result<PtzStatus>()>& invoke
    )
    {
        std::uint64_t observed = 0;
        {
            std::lock_guard lock(mutex_);
            observed = clock_;
        }
        auto result = invoke();
        if (result && !result.value().moving)
        {
            // A movement recorded while the status request was in flight may not be
            // reflected in the reply, so only older movements are cleared.
            std::lock_guard lock(mutex_);
            const auto found = movements_.find(profile);
            if (found != movements_.end() && found->second.stamp <= observed)
                movements_.erase(found);
        }
        return result;
    }

    bool PtzSafetyState::close_all(std::chrono::steady_clock::time_point deadline) noexcept
    {
        try
        {
            std::vector<ProfileToken> profiles;
            bool complete = true;
            {
                std::unique_lock lock(mutex_);
                closing_ = true;
                // Movements already on the wire are allowed to land so they can be
                // recorded and stopped; each also stops itself on seeing closing_.
                if (!idle_.wait_until(lock, deadline, [&] { return in_flight_ == 0; }))
                    complete = false;
                for (const auto& [profile, movement] : movements_)
                {
                    (void)movement;
                    profiles.push_back(profile);
                }
            }
            for (const auto& profile : profiles)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    complete = false;
                    break;
                }
                const auto detail = send_stop(profile, {deadline, CancellationSource{}.token()});
                complete = detail.succeeded && complete;
            }
            std::lock_guard lock(mutex_);
            return complete && movements_.empty();
        }
        catch (...)
        {
            return false;
        }
    }

    std::size_t PtzSafetyState::movement_count() const noexcept
    {
        std::lock_guard lock(mutex_);
        return movements_.size();
    }
}
