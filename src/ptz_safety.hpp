#pragma once

#include <map>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    class PtzSafetyState
    {
      public:
        PtzSafetyState(std::shared_ptr<ServiceBackend> backend, CancellationToken cancellation);

        Result<MoveResult> move(
            const ProfileToken& profile,
            OperationId operation,
            bool finite,
            CallOptions options,
            const std::function<Result<MoveResult>()>& invoke
        );
        Result<void> preset(
            const ProfileToken& profile,
            CallOptions options,
            const std::function<Result<void>()>& invoke
        );
        Result<void> stop(const ProfileToken& profile, CallOptions options);
        Result<PtzStatus> status(
            const ProfileToken& profile, const std::function<Result<PtzStatus>()>& invoke
        );
        bool close_all(std::chrono::steady_clock::time_point deadline) noexcept;
        std::size_t movement_count() const noexcept;

      private:
        struct Movement
        {
            OperationId operation;
            bool finite;
        };
        SafetyActionDetail stop_locked(const ProfileToken&, CallOptions);
        static void attach(Error&, const SafetyActionDetail&);
        std::shared_ptr<ServiceBackend> backend_;
        CancellationToken cancellation_;
        mutable std::mutex mutex_;
        std::map<ProfileToken, Movement> movements_;
        bool closing_{};
    };
}
