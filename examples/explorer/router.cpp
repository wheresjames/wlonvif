/**
 * @file router.cpp
 * @brief The Explorer's web entry points: page files, request checks, and API routes.
 *
 * API paths:
 *
 *     GET  /api/state                      cameras, scan progress, server features
 *     POST /api/scan                       start a network scan
 *     GET  /api/log?since=N                request log entries after N
 *     GET  /api/recordings                 recordings on disk
 *     POST /api/recordings/NAME/stop|delete
 *     GET  /api/recordings/NAME/download   (streamed)
 *     POST /api/cameras                    add a camera by address or URL
 *     ...  /api/cameras/ID/ACTION[/SUB]    see camera_api() below
 */

#include "assets.hpp"
#include "implementation.hpp"

#include <cstdlib>

namespace wlonvif::examples::explorer
{
    namespace
    {
        constexpr std::string_view session_cookie = "wlonvif_explorer";

        std::uint64_t since_parameter(const web::Request& request)
        {
            return std::strtoull(request.query("since").c_str(), nullptr, 10);
        }

        std::string_view content_type(std::string_view path)
        {
            if (path.ends_with(".html"))
            {
                return "text/html; charset=utf-8";
            }
            if (path.ends_with(".js"))
            {
                return "text/javascript; charset=utf-8";
            }
            if (path.ends_with(".css"))
            {
                return "text/css; charset=utf-8";
            }
            return "application/octet-stream";
        }

        /// The embedded page file for a request path; `/` is the page itself.
        const assets::File* page_file(std::string_view path)
        {
            if (path == "/")
            {
                path = "/index.html";
            }
            for (const auto& file : assets::files)
            {
                if (path.substr(1) == file.path)
                {
                    return &file;
                }
            }
            return nullptr;
        }

        web::Response json_response(const Reply& reply)
        {
            web::Response response;
            response.status = reply.status;
            response.content_type = "application/json";
            response.body = json::serialize(reply.body);
            return response;
        }
    } // namespace

    std::optional<Reply> Explorer::Implementation::guard(const web::Request& request)
    {
        // The API holds camera credentials, so refuse anything a web page on
        // another origin could send: a foreign Host (DNS rebinding), a missing
        // session cookie, or a cross-origin write.
        const auto& hosts = owner.options_.allowed_hosts;
        if (!hosts.empty() && std::find(hosts.begin(), hosts.end(), request.host) == hosts.end())
        {
            return rejection(403, "unexpected Host header", "authorization");
        }
        if (request.cookie_value(std::string(session_cookie)) != owner.token_)
        {
            return rejection(401, "missing or stale session; reload the page", "authentication");
        }
        if (request.method != "GET")
        {
            const auto scheme = request.secure ? "https://" : "http://";
            if (request.origin != scheme + request.host)
            {
                return rejection(403, "cross-origin request refused", "authorization");
            }
        }
        return std::nullopt;
    }

    Reply Explorer::Implementation::api(const web::Request& request)
    {
        if (auto refused = guard(request))
        {
            return *refused;
        }
        json::object body;
        if (!request.body.empty())
        {
            boost::system::error_code error;
            auto parsed = json::parse(request.body, error);
            if (error || !parsed.is_object())
            {
                return rejection(400, "request body must be a JSON object");
            }
            body = std::move(parsed.as_object());
        }

        const auto parts = split_path(request.path()); // "api", "cameras", ...
        const auto& method = request.method;
        const auto area = parts.size() > 1 ? parts[1] : std::string();

        if (parts.size() == 2 && area == "state" && method == "GET")
        {
            return state_view();
        }
        if (parts.size() == 2 && area == "scan")
        {
            if (method == "POST")
            {
                return start_scan(body);
            }
            std::lock_guard lock(scan_mutex);
            return success(scan_status);
        }
        if (parts.size() == 2 && area == "log")
        {
            return log_view(since_parameter(request));
        }
        if (parts.size() == 2 && area == "recordings")
        {
            return recordings_view();
        }
        if (parts.size() == 4 && area == "recordings" && method == "POST")
        {
            return recording_action(parts[2], parts[3]);
        }
        if (parts.size() == 2 && area == "cameras" && method == "POST")
        {
            return add_camera_request(body);
        }
        if (parts.size() >= 4 && area == "cameras")
        {
            auto selected = camera(parts[2]);
            if (!selected)
            {
                return rejection(404, "unknown camera " + parts[2]);
            }
            return camera_api(selected, request, parts, body);
        }
        return rejection(404, "unknown API path");
    }

    Reply Explorer::Implementation::camera_api(
        const std::shared_ptr<Camera>& selected,
        const web::Request& request,
        const std::vector<std::string>& parts,
        const json::object& body
    )
    {
        // parts: "api", "cameras", ID, action, and an optional sub-action.
        const auto& method = request.method;
        const auto& action = parts[3];
        const auto sub = parts.size() == 5 ? parts[4] : std::string();
        const bool get = method == "GET";
        const bool post = method == "POST";

        // These work whether or not the camera is signed in.
        if (action == "connect" && post)
        {
            return connect(*selected, body);
        }
        if (action == "forget" && post)
        {
            return forget(selected);
        }
        if (action == "check" && get)
        {
            return check_status(*selected);
        }
        if (action == "events" && get)
        {
            return events_view(*selected, since_parameter(request));
        }
        if (action == "live" && get)
        {
            return video_status(*selected);
        }

        // Everything else talks to the camera.
        auto session = session_of(*selected);
        if (!session)
        {
            return rejection(409, "not connected; connect with credentials first", "closed-handle");
        }
        auto& camera = *selected;
        auto& device = *session;

        if (action == "overview")
        {
            return overview(camera, device);
        }
        if (action == "check" && post)
        {
            return start_check(selected, session);
        }
        if (action == "media" && sub.empty())
        {
            return media_view(camera, device);
        }
        if (action == "media" && sub == "encoder")
        {
            return set_encoder(camera, device, body);
        }
        if (action == "imaging")
        {
            return get ? imaging_view(camera, device) : set_imaging(camera, device, body);
        }
        if (action == "ptz" && sub.empty())
        {
            return ptz_view(camera, device, request.query("profile"));
        }
        if (action == "ptz" && post) // intent, stop, relative, absolute, preset, tour, direction
        {
            return ptz_action(camera, device, sub, body);
        }
        if (action == "analytics")
        {
            return get ? analytics_view(camera, device) : set_analytics(camera, device, body);
        }
        if (action == "topics")
        {
            return topics_view(camera, device);
        }
        if (action == "events" && sub == "start")
        {
            return start_events(selected, device, body);
        }
        if (action == "events" && sub == "stop")
        {
            stop_events(camera);
            return success();
        }
        if (action == "events" && sub == "clear" && post)
        {
            return clear_events(camera);
        }
        if (action == "device")
        {
            return get ? device_view(camera, device) : device_action(camera, device, body);
        }
        if (action == "live" && sub == "start")
        {
            return start_video(camera, device, text_field(body, "profile").value_or(""));
        }
        if (action == "live" && sub == "stop")
        {
            stop_video(camera);
            return success();
        }
        if (action == "record" && post)
        {
            return start_recording(camera, device, body);
        }
        if (action == "call" && post)
        {
            return raw_call(camera, device, body);
        }
        return rejection(404, "unknown API path");
    }

    // ---- Explorer --------------------------------------------------------------

    Explorer::Explorer(
        ExplorerOptions options,
        std::shared_ptr<Runtime> runtime,
        Connector connector,
        Scanner scanner,
        Resolver resolver
    )
        : options_(std::move(options)), runtime_(std::move(runtime)),
          connector_(std::move(connector)), scanner_(std::move(scanner)),
          resolver_(std::move(resolver)), token_(web::random_token()),
          implementation_(std::make_unique<Implementation>(*this))
    {
    }

    Explorer::~Explorer()
    {
        implementation_->shutdown();
    }

    web::Response Explorer::handle(const web::Request& request)
    {
        const auto path = request.path();

        if (request.method == "GET")
        {
            if (const auto* file = page_file(path))
            {
                web::Response response;
                response.content_type = std::string(content_type(file->path));
                response.body = std::string(file->body);
                if (file->path == "index.html")
                {
                    // The page hands out the API session. SameSite=Strict keeps the
                    // cookie off every cross-site request.
                    response.headers.emplace_back(
                        "Set-Cookie",
                        std::string(session_cookie) + "=" + token_ +
                            "; Path=/; HttpOnly; SameSite=Strict" +
                            (request.secure ? "; Secure" : "")
                    );
                }
                return response;
            }
        }

        if (path.starts_with("/api/"))
        {
            // Snapshots answer with an image rather than JSON.
            const auto parts = split_path(path);
            const bool snapshot = request.method == "GET" && parts.size() == 4 &&
                                  parts[1] == "cameras" && parts[3] == "snapshot";
            if (snapshot)
            {
                if (auto refused = implementation_->guard(request))
                {
                    return json_response(*refused);
                }
                auto selected = implementation_->camera(parts[2]);
                auto session = selected ? implementation_->session_of(*selected) : nullptr;
                if (!session)
                {
                    return json_response(rejection(409, "not connected", "closed-handle"));
                }
                return implementation_->snapshot(*selected, *session, request.query("profile"));
            }

            Reply reply;
            try
            {
                reply = implementation_->api(request);
            }
            catch (const std::exception& error)
            {
                reply = rejection(500, std::string("internal error: ") + error.what(), "internal");
            }
            return json_response(reply);
        }

        web::Response response;
        response.status = 404;
        response.content_type = "text/plain";
        response.body = "not found";
        return response;
    }

    bool Explorer::streams(const web::Request& request)
    {
        // Live video and recording downloads are sent in pieces as they are produced.
        const auto parts = split_path(request.path());
        if (request.method != "GET" || parts.size() != 4 || parts[0] != "api")
        {
            return false;
        }
        return (parts[1] == "cameras" && parts[3] == "live.mjpeg") ||
               (parts[1] == "recordings" && parts[3] == "download");
    }

    void Explorer::stream(const web::Request& request, web::ResponseStream& output)
    {
        if (implementation_->guard(request))
        {
            return;
        }
        const auto parts = split_path(request.path());
        if (parts[1] == "recordings")
        {
            implementation_->download(parts[2], output);
            return;
        }
        if (auto selected = implementation_->camera(parts[2]))
        {
            implementation_->stream_video(*selected, output);
        }
    }

    std::string Explorer::content_security_policy() const
    {
        // Only the explorer's own files; no inline script or style, no framing.
        return "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data: "
               "blob:; "
               "media-src 'self' blob:; connect-src 'self'; frame-ancestors 'none'; base-uri "
               "'none'; "
               "form-action 'none'";
    }

    void Explorer::stopped()
    {
        implementation_->shutdown();
    }
} // namespace wlonvif::examples::explorer
