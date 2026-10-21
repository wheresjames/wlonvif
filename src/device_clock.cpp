#include "device_clock.hpp"

#include "soap_response.hpp"
#include "xml_scan.hpp"

#include <cstdlib>

namespace wlonvif::detail
{
    namespace
    {
        /// Offsets below this are within the device's one-second reporting resolution.
        constexpr std::chrono::seconds MinimumCorrection{2};
    } // namespace

    bool DeviceClock::needs_measurement() const
    {
        std::lock_guard lock(mutex_);
        return !measured_;
    }

    std::chrono::system_clock::duration DeviceClock::offset() const
    {
        std::lock_guard lock(mutex_);
        return offset_;
    }

    void DeviceClock::record(
        const Result<HttpResponse>& response, std::chrono::system_clock::time_point received
    )
    {
        if (!response && !response.error().http) return;
        std::chrono::system_clock::duration offset{};
        if (response)
        {
            auto content = soap_response_content(
                response.value(), "GetSystemDateAndTimeResponse",
                OperationId::get_system_date_and_time
            );
            auto time = content ? decode_system_date_and_time(content.value())
                                : Result<SystemDateAndTime>(content.error());
            if (time)
            {
                const auto measured = std::chrono::round<std::chrono::seconds>(time.value().utc - received);
                if (std::chrono::abs(measured) >= MinimumCorrection) offset = measured;
            }
        }
        std::lock_guard lock(mutex_);
        measured_ = true;
        offset_ = offset;
    }

    void DeviceClock::invalidate()
    {
        std::lock_guard lock(mutex_);
        measured_ = false;
    }

    Result<SystemDateAndTime> decode_system_date_and_time(std::string_view x)
    {
        const auto invalid = [](std::string message)
        {
            return Error{
                ErrorCategory::protocol_violation,
                OperationId::get_system_date_and_time,
                std::move(message)
            };
        };
        // The response also carries LocalDateTime with identically named
        // children, so the UTC subtree has to be selected explicitly rather
        // than taking whichever Year element happens to appear first.
        const auto utc = detail_xml::find_element(x, "UTCDateTime");
        if (!utc)
        {
            return invalid("device did not report a UTC date and time");
        }
        const auto subtree = std::string(detail_xml::inner(x, *utc));
        const auto integer = [&](std::string_view name)
        {
            const auto value = detail_xml::first_text(subtree, name);
            return value.empty() ? -1 : std::atoi(value.c_str());
        };
        const auto year = integer("Year");
        const auto month = integer("Month");
        const auto day = integer("Day");
        const auto hour = integer("Hour");
        const auto minute = integer("Minute");
        const auto second = integer("Second");
        const auto date = std::chrono::year(year) /
                          std::chrono::month(static_cast<unsigned>(month)) /
                          std::chrono::day(static_cast<unsigned>(day));
        if (!date.ok() || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
            second < 0 || second > 60)
        {
            return invalid("device reported an invalid UTC date and time");
        }
        SystemDateAndTime output;
        output.utc = std::chrono::sys_days(date) + std::chrono::hours(hour) +
                     std::chrono::minutes(minute) + std::chrono::seconds(second);
        output.type = detail_xml::first_text(x, "DateTimeType") == "NTP" ? DateTimeType::ntp
                                                                         : DateTimeType::manual;
        output.daylight_savings = detail_xml::first_text(x, "DaylightSavings") == "true";
        output.time_zone = detail_xml::first_text(x, "TZ");
        return output;
    }

    HttpRequest clock_request(const std::string& device_url, std::chrono::steady_clock::time_point deadline)
    {
        HttpRequest request{
            device_url,
            soap_envelope(OperationId::get_system_date_and_time, ""),
            {"Content-Type: application/soap+xml; charset=utf-8"},
            deadline
        };
        request.original_url = device_url;
        request.operation = OperationId::get_system_date_and_time;
        request.idempotent = true;
        request.retry_allowed = false;
        return request;
    }
} // namespace wlonvif::detail
