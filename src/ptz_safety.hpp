#pragma once

#include <condition_variable>
#include <map>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    /**
     * @brief Per-session ledger of PTZ movements that may still need a Stop.
     *
     * The mutex guards only the ledger; no network request is made while it is
     * held. A Stop therefore never queues behind an in-flight move, and a move
     * that lands after a Stop (or after close began) is detected and stopped.
     */
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
            std::uint64_t stamp; ///< Ledger clock value when the movement was recorded.
        };
        /// Outcome of the pre-flight check shared by every movement.
        struct Admission
        {
            std::optional<Error> rejection; ///< Set when the movement must not be sent.
            std::uint64_t stops_seen{};     ///< Stops started for the profile so far.
        };
        Admission admit(const ProfileToken&, OperationId, const std::string& what, CallOptions);
        Result<MoveResult> guarded_move(
            const ProfileToken&, OperationId, bool finite, const std::string& what,
            CallOptions, const std::function<Result<MoveResult>()>& invoke
        );
        /// Record a movement that may be under way; true when it must be stopped now.
        bool record(const ProfileToken&, OperationId, bool finite, std::uint64_t stops_seen);
        void finish_in_flight() noexcept;
        /// Send a Stop. Must be called without holding the mutex.
        SafetyActionDetail send_stop(const ProfileToken&, CallOptions);
        static void attach(Error&, const SafetyActionDetail&);
        std::shared_ptr<ServiceBackend> backend_;
        CancellationToken cancellation_;
        mutable std::mutex mutex_;
        std::condition_variable idle_;
        std::map<ProfileToken, Movement> movements_;
        std::map<ProfileToken, std::uint64_t> stops_started_;
        std::uint64_t clock_{};
        std::size_t in_flight_{};
        bool closing_{};
    };
}
