#pragma once

/**
 * @file soap_response.hpp
 * @brief Internal helper interpreting an HTTP response that carries a SOAP reply.
 *
 * DeviceSession and the service backend previously each had their own copy of
 * this logic, and the copies drifted: one rejected non-2xx statuses and scoped
 * decoders to the response element, the other accepted any status and handed
 * decoders the whole envelope, where a SOAP Header could shadow real fields.
 */

#include "xml_scan.hpp"

#include <string>
#include <string_view>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::detail
{
    /**
     * @brief Validate @p response and return the inner markup of @p response_name.
     *
     * HTTP 401/403/429 map to their dedicated categories. Any other non-2xx status
     * is reported as the SOAP Fault it carries, or as a protocol violation when it
     * carries none. The returned markup is taken from inside the SOAP Body, so
     * nothing in the Header or after the response element can reach a decoder.
     */
    inline Result<std::string> soap_response_content(
        const HttpResponse& response, std::string_view response_name, OperationId operation
    )
    {
        const auto status = response.status;
        const auto http_error = [&](ErrorCategory category, std::string message, bool retryable)
        {
            Error error{category, operation, std::move(message), retryable};
            error.http = HttpErrorDetail{status};
            return error;
        };
        if (status == 401)
            return http_error(ErrorCategory::authentication, "authentication failed", false);
        if (status == 403)
            return http_error(ErrorCategory::authorization, "request was forbidden", false);
        if (status == 429)
            return http_error(ErrorCategory::rate_limited, "HTTP rate limit exceeded", true);
        auto parsed = read_soap_operation(response.body, response_name);
        if (status < 200 || status >= 300)
        {
            if (!parsed)
            {
                const auto category = parsed.error().category;
                if (category == ErrorCategory::soap_fault ||
                    category == ErrorCategory::authentication ||
                    category == ErrorCategory::device_busy ||
                    category == ErrorCategory::rate_limited)
                {
                    auto error = parsed.error();
                    error.operation = operation;
                    return error;
                }
            }
            return http_error(
                ErrorCategory::protocol_violation,
                "unexpected HTTP status " + std::to_string(status),
                false
            );
        }
        if (!parsed)
        {
            auto error = parsed.error();
            error.operation = operation;
            return error;
        }
        const std::string_view body(response.body);
        const auto envelope_body = detail_xml::find_element(body, "Body");
        const auto scope = envelope_body ? detail_xml::inner(body, *envelope_body) : body;
        const auto element = detail_xml::find_element(scope, response_name);
        if (!element)
        {
            return Error{
                ErrorCategory::protocol_violation,
                operation,
                "Expected SOAP response element missing"
            };
        }
        return std::string(detail_xml::inner(scope, *element));
    }
} // namespace wlonvif::detail
