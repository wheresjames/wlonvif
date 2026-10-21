#pragma once

/**
 * @file recorded_camera_transport.hpp
 * @brief HttpTransport replaying replies recorded from a consumer PTZ camera.
 *
 * The fixtures in tests/fixtures/recorded-camera are responses recorded from real
 * firmware, so decoders are exercised against gSOAP's prefixes, attribute order, and
 * quirks rather than against markup shaped like the implementation expects. Only
 * identifying values were replaced: addresses use the RFC 5737 documentation range
 * (192.0.2.0/24), the MAC address the RFC 7042 documentation block, and the model
 * name a generic one.
 */

#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif::testing
{
    /// Read a recorded camera reply, or return empty when the file is missing.
    inline std::string recorded_fixture(const std::string& name)
    {
        std::ifstream file(std::string(WLONVIF_RECORDED_FIXTURES) + "/" + name);
        std::ostringstream content;
        content << file.rdbuf();
        return content.str();
    }

    /// Wrap a SOAP Body in the envelope the recorded camera uses.
    inline std::string recorded_envelope(const std::string& body)
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><SOAP-ENV:Envelope "
               "xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\" "
               "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
               "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
               "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
               "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" "
               "xmlns:tan=\"http://www.onvif.org/ver20/analytics/wsdl\" "
               "xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" "
               "xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\" "
               "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" "
               "xmlns:wsa5=\"http://www.w3.org/2005/08/addressing\" "
               "xmlns:ter=\"http://www.onvif.org/ver10/error\"><SOAP-ENV:Body>" +
               body + "</SOAP-ENV:Body></SOAP-ENV:Envelope>";
    }

    /**
     * @brief Answers each request with the reply registered for its operation.
     *
     * Operations are matched on the request's Body element name, so `GetOptions`
     * does not answer `GetConfigurationOptions`. Unregistered operations fail
     * as unsupported, and every request body is kept for inspection.
     */
    class RecordedCameraTransport final : public HttpTransport
    {
      public:
        /// Answer @p operation with fixture file @p file (status 200).
        void serve(std::string operation, const std::string& file)
        {
            reply(std::move(operation), 200, recorded_fixture(file));
        }
        /// Answer @p operation with @p status and literal @p body.
        void reply(std::string operation, long status, std::string body)
        {
            std::lock_guard lock(mutex_);
            replies_[std::move(operation)] = {status, std::move(body)};
        }
        Task<Result<HttpResponse>> send(HttpRequest request, CancellationToken) override
        {
            std::lock_guard lock(mutex_);
            bodies_.push_back(request.body);
            for (const auto& [operation, reply] : replies_)
            {
                const auto body = request.body.find("Body>");
                const auto name = request.body.find(':' + operation, body);
                const auto end = name == std::string::npos ? name : name + operation.size() + 1;
                if (name == std::string::npos || end >= request.body.size() ||
                    (request.body[end] != '>' && request.body[end] != '/' && request.body[end] != ' '))
                    continue;
                HttpResponse response{reply.first, reply.second, "application/soap+xml"};
                return Task<Result<HttpResponse>>([response](auto done) mutable
                                                  { done(std::move(response)); });
            }
            return Task<Result<HttpResponse>>([](auto done)
                                              { done(Error{ErrorCategory::unsupported}); });
        }
        /// Return the most recent request body naming @p operation.
        std::string last(const std::string& operation)
        {
            std::lock_guard lock(mutex_);
            for (auto body = bodies_.rbegin(); body != bodies_.rend(); ++body)
                if (body->find(':' + operation) != std::string::npos) return *body;
            return {};
        }

      private:
        std::mutex mutex_;
        std::map<std::string, std::pair<long, std::string>> replies_;
        std::vector<std::string> bodies_;
    };
} // namespace wlonvif::testing
