#pragma once

#include <chrono>
#include <mutex>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::detail
{
    /**
     * @brief A device's UTC clock offset, measured once per session.
     *
     * WS-Security UsernameToken digests cover a Created timestamp that devices
     * reject when it is too far from their own clock, so a camera whose clock has
     * drifted (or was never set) refuses every authenticated request. Requests are
     * therefore stamped in device time, measured with the unauthenticated
     * GetSystemDateAndTime the ONVIF Core specification provides for this purpose.
     */
    class DeviceClock
    {
      public:
        /// True until a measurement has been recorded.
        bool needs_measurement() const;
        /// Offset to add to local UTC to obtain device UTC; zero when unknown.
        std::chrono::system_clock::duration offset() const;
        /**
         * @brief Record the outcome of a GetSystemDateAndTime exchange.
         *
         * Transport failures are not recorded, so the next authenticated request
         * measures again. Any device answer, including a refusal, settles the
         * measurement so a device that requires authentication for the time is
         * not asked again on every request.
         */
        void record(const Result<HttpResponse>& response, std::chrono::system_clock::time_point received);
        /// Require a new measurement, after the device clock was deliberately changed.
        void invalidate();

      private:
        mutable std::mutex mutex_;
        bool measured_{};
        std::chrono::system_clock::duration offset_{};
    };

    /// Decode GetSystemDateAndTimeResponse content into the device's UTC time.
    Result<SystemDateAndTime> decode_system_date_and_time(std::string_view content);

    /// Build the unauthenticated GetSystemDateAndTime request used for measurement.
    HttpRequest clock_request(const std::string& device_url, std::chrono::steady_clock::time_point deadline);
} // namespace wlonvif::detail
