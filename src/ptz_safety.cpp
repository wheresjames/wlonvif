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

    SafetyActionDetail PtzSafetyState::stop_locked(
        const ProfileToken& profile, CallOptions options
    )
    {
        SafetyActionDetail detail;
        detail.attempted = true;
        set_network_call_options(bounded_safety_options(options));
        auto stopped = backend_->stop(profile);
        clear_network_call_options();
        detail.succeeded = static_cast<bool>(stopped);
        if (stopped)
        {
            movements_.erase(profile);
        }
        else
        {
            detail.failure_category = stopped.error().category;
            detail.failure_code = failure_code(stopped.error());
        }
        return detail;
    }

    Result<MoveResult> PtzSafetyState::move(
        const ProfileToken& profile,
        OperationId operation,
        bool finite,
        CallOptions options,
        const std::function<Result<MoveResult>()>& invoke
    )
    {
        std::lock_guard lock(mutex_);
        if (closing_ || cancellation_.stop_requested() || options.cancellation.stop_requested() ||
            std::chrono::steady_clock::now() >= options.deadline)
        {
            const bool cancelled = closing_ || cancellation_.stop_requested() ||
                                   options.cancellation.stop_requested();
            Error error{cancelled ? ErrorCategory::cancelled : ErrorCategory::timeout,
                        operation, cancelled ? "movement cancelled" : "movement timed out",
                        false, true};
            attach(error, stop_locked(profile, options));
            return error;
        }
        auto result = invoke();
        if (result)
        {
            movements_[profile] = Movement{operation, finite};
            if (result.value().request_may_have_been_applied)
            {
                auto detail = stop_locked(profile, options);
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
            return result;
        }
        auto error = result.error();
        const bool safety_required = error.request_may_have_been_applied ||
                                     error.category == ErrorCategory::cancelled ||
                                     error.category == ErrorCategory::timeout;
        if (error.request_may_have_been_applied)
            movements_[profile] = Movement{operation, finite};
        if (safety_required) attach(error, stop_locked(profile, options));
        return error;
    }

    Result<void> PtzSafetyState::preset(
        const ProfileToken& profile,
        CallOptions options,
        const std::function<Result<void>()>& invoke
    )
    {
        std::lock_guard lock(mutex_);
        if (closing_ || cancellation_.stop_requested() || options.cancellation.stop_requested() ||
            std::chrono::steady_clock::now() >= options.deadline)
        {
            const bool cancelled = closing_ || cancellation_.stop_requested() ||
                                   options.cancellation.stop_requested();
            Error error{cancelled ? ErrorCategory::cancelled : ErrorCategory::timeout,
                        OperationId::goto_preset,
                        cancelled ? "preset movement cancelled" : "preset movement timed out",
                        false, true};
            attach(error, stop_locked(profile, options));
            return error;
        }
        auto result = invoke();
        if (result)
        {
            movements_[profile] = Movement{OperationId::goto_preset, true};
            return {};
        }
        auto error = result.error();
        if (error.request_may_have_been_applied || error.category == ErrorCategory::cancelled ||
            error.category == ErrorCategory::timeout)
        {
            if (error.request_may_have_been_applied)
                movements_[profile] = Movement{OperationId::goto_preset, true};
            attach(error, stop_locked(profile, options));
        }
        return error;
    }

    Result<void> PtzSafetyState::stop(const ProfileToken& profile, CallOptions options)
    {
        std::lock_guard lock(mutex_);
        auto detail = stop_locked(profile, options);
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
        std::lock_guard lock(mutex_);
        auto result = invoke();
        if (result && !result.value().moving) movements_.erase(profile);
        return result;
    }

    bool PtzSafetyState::close_all(std::chrono::steady_clock::time_point deadline) noexcept
    {
        try
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            bool complete = true;
            std::vector<ProfileToken> profiles;
            for (const auto& [profile, movement] : movements_)
            {
                (void)movement;
                profiles.push_back(profile);
            }
            for (const auto& profile : profiles)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    complete = false;
                    break;
                }
                const auto detail = stop_locked(
                    profile, {deadline, CancellationSource{}.token()}
                );
                complete = detail.succeeded && complete;
            }
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
