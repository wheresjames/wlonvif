#pragma once

/**
 * @file implementation.hpp
 * @brief The Explorer's private state, shared by the API handler files.
 *
 * Explorer::Implementation owns the camera list, the network scan, the request
 * log, and recordings. Its handlers are spread over one source file per area:
 *
 * | File                    | Area                                           |
 * |-------------------------|------------------------------------------------|
 * | cameras.cpp             | camera list, sign-in, network scan, overview   |
 * | compatibility_check.cpp | the read-only compatibility check              |
 * | media_api.cpp           | media profiles, encoders, imaging, analytics   |
 * | ptz_api.cpp             | PTZ views, movement, presets, tours            |
 * | events_api.cpp          | event topics and the event monitor             |
 * | device_api.cpp          | clock, network, scopes, reboot, raw requests   |
 * | video_api.cpp           | live MJPEG video, snapshots, recordings        |
 * | router.cpp              | request checks and the URL-to-handler mapping  |
 *
 * Handlers run on web server worker threads, several at once. Shared state is
 * guarded by the mutex declared next to it, and no mutex is held while waiting
 * for a camera, since a camera can take seconds to answer.
 */

#include "api_support.hpp"
#include "explorer.hpp"
#include "recorder.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <example_support.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <video_source.hpp>

namespace wlonvif::examples::explorer
{
    using SteadyClock = std::chrono::steady_clock;

    /// One camera in the sidebar: what a scan found, plus its session once signed in.
    struct Explorer::Camera
    {
        // Set when the camera is added; read without a lock afterwards.
        std::string id; ///< `camera-N`, used in API paths.
        std::string address;
        std::string device_url; ///< Device service URL; resolved on first sign-in if unknown.
        std::string evidence;   ///< How a scan recognized it, from evidence_name().
        std::vector<std::string> scopes;
        bool manual{}; ///< Added by the user rather than found by a scan.

        std::mutex mutex; ///< Guards the sign-in state below.
        std::optional<Credentials> credentials;
        std::shared_ptr<DeviceSession> session; ///< Null until signed in.
        std::optional<DeviceInformation> identity;
        std::optional<Capabilities> capabilities; ///< Services advertised at sign-in.
        std::optional<json::object> connection_error;

        std::mutex check_mutex; ///< Guards the compatibility check.
        std::thread check_thread;
        bool check_running{};
        json::array check_results;
        std::string check_started;

        std::mutex video_mutex; ///< Guards the live video.
        std::shared_ptr<web::FfmpegVideo> video;
        std::string video_profile;
        int viewers{}; ///< Open MJPEG responses; the last one to leave stops FFmpeg.

        std::mutex event_mutex; ///< Guards the event monitor.
        std::shared_ptr<ManagedEventStream> events;
        std::thread event_thread;
        std::shared_ptr<std::atomic_bool> event_stop;
        std::deque<json::object> event_log; ///< Newest last, at most 500 entries.
        std::uint64_t event_sequence{};     ///< Numbers entries so the page can ask for newer ones.
        std::optional<json::object> event_error;
        std::vector<std::string> event_topics;

        std::mutex ptz_mutex; ///< Guards both PTZ movement modes.
        /// Smooth mode: a ramping controller per profile.
        std::map<std::string, std::shared_ptr<PtzController>> controllers;
        /// Direct mode: one ContinuousMove per press, kept alive by the page's repeats.
        struct DirectMove
        {
            std::shared_ptr<PtzClient> client;
            PtzVector velocity;
            bool moving{};
            SteadyClock::time_point renewed; ///< Last repeat from the page.
        };
        std::map<std::string, DirectMove> direct; ///< Keyed by profile token.
    };

    using Camera = Explorer::Camera;

    struct Explorer::Implementation
    {
        explicit Implementation(Explorer& explorer);

        Explorer& owner; ///< Options and integrations (connector, scanner, resolver).

        std::mutex mutex; ///< Guards cameras and next_camera.
        std::map<std::string, std::shared_ptr<Camera>> cameras;
        std::uint64_t next_camera{1};

        std::mutex scan_mutex; ///< Guards the scan state.
        std::thread scan_thread;
        bool scanning{};
        json::object scan_status{{"running", false}};
        /// Cancels a scan and every camera request in flight at shutdown.
        CancellationSource cancellation;

        std::mutex log_mutex; ///< Guards the request log.
        std::deque<json::object> log;
        std::uint64_t log_sequence{};

        std::mutex recording_mutex;                                   ///< Guards recordings.
        std::map<std::string, std::shared_ptr<Recording>> recordings; ///< By file name.

        std::thread watchdog; ///< Stops direct PTZ moves the page abandoned.
        std::atomic_bool watchdog_stop{};

        /// How long a direct PTZ move continues without a repeat from the page.
        static constexpr std::chrono::milliseconds direct_lease{750};

        // ---- Calling cameras (cameras.cpp) --------------------------------------

        /// Options for one camera request: the configured timeout, cancelled at shutdown.
        CallOptions call() const;

        /// Add one entry to the request log the page shows.
        void record(
            const std::string& camera,
            std::string action,
            SteadyClock::duration elapsed,
            const std::optional<Error>& error
        );

        /// Run a library task to completion, timing and logging it.
        template <class T>
        Result<T> run(const std::string& camera, std::string action, Task<Result<T>> task)
        {
            const auto started = SteadyClock::now();
            auto result = examples::wait_for_task(std::move(task));
            record(camera, std::move(action), SteadyClock::now() - started, error_of(result));
            return result;
        }

        /// Run a synchronous step that returns a Result, timing and logging it.
        template <class F> auto timed(const std::string& camera, std::string action, F function)
        {
            const auto started = SteadyClock::now();
            auto result = function();
            record(camera, std::move(action), SteadyClock::now() - started, error_of(result));
            return result;
        }

        template <class T> static std::optional<Error> error_of(const Result<T>& result)
        {
            return result ? std::nullopt : std::optional<Error>(result.error());
        }

        // ---- Camera list, sign-in, scan (cameras.cpp) ---------------------------

        std::shared_ptr<Camera> camera(const std::string& id);
        std::vector<std::shared_ptr<Camera>> all_cameras();
        /// Add a camera, or return the existing one with the same address or URL.
        std::shared_ptr<Camera>
        add_camera(const std::string& address, const std::string& device_url, bool manual);
        /// The sidebar entry for a camera, without credentials.
        json::object summary(Camera& camera);
        std::shared_ptr<DeviceSession> session_of(Camera& camera);
        std::optional<Credentials> credentials_of(Camera& camera);
        Reply connect(Camera& camera, const json::object& body);
        /// Close the session and everything that uses it: events, video, PTZ.
        void disconnect(Camera& camera);
        Reply forget(const std::shared_ptr<Camera>& camera);
        Reply add_camera_request(const json::object& body);
        Reply state_view();
        Reply log_view(std::uint64_t since);
        Reply start_scan(const json::object& body);
        Reply overview(Camera& camera, DeviceSession& session);
        /// Stop every thread and close every session; runs once at exit.
        void shutdown();

        // ---- Compatibility check (compatibility_check.cpp) ----------------------

        Reply start_check(
            const std::shared_ptr<Camera>& camera, const std::shared_ptr<DeviceSession>& session
        );
        Reply check_status(Camera& camera);

        // ---- Media, imaging, analytics (media_api.cpp) --------------------------

        Reply media_view(Camera& camera, DeviceSession& session);
        Reply set_encoder(Camera& camera, DeviceSession& session, const json::object& body);
        Reply imaging_view(Camera& camera, DeviceSession& session);
        Reply set_imaging(Camera& camera, DeviceSession& session, const json::object& body);
        Reply analytics_view(Camera& camera, DeviceSession& session);
        Reply set_analytics(Camera& camera, DeviceSession& session, const json::object& body);

        // ---- PTZ (ptz_api.cpp) --------------------------------------------------

        Reply ptz_view(Camera& camera, DeviceSession& session, const std::string& profile);
        Reply ptz_action(
            Camera& camera,
            DeviceSession& session,
            const std::string& action,
            const json::object& body
        );
        Reply direct_intent(
            Camera& camera, DeviceSession& session, const std::string& profile, PtzVector value
        );
        Result<std::shared_ptr<PtzController>>
        controller(Camera& camera, DeviceSession& session, const std::string& profile);
        void close_controllers(Camera& camera);
        /// The watchdog thread's loop.
        void watch_direct_moves();

        // ---- Events (events_api.cpp) --------------------------------------------

        Reply topics_view(Camera& camera, DeviceSession& session);
        Reply start_events(
            const std::shared_ptr<Camera>& camera, DeviceSession& session, const json::object& body
        );
        void stop_events(Camera& camera);
        Reply clear_events(Camera& camera);
        Reply events_view(Camera& camera, std::uint64_t since);

        // ---- Device settings and raw requests (device_api.cpp) ------------------

        Reply device_view(Camera& camera, DeviceSession& session);
        Reply device_action(Camera& camera, DeviceSession& session, const json::object& body);
        Reply raw_call(Camera& camera, DeviceSession& session, const json::object& body);

        // ---- Video, snapshots, recordings (video_api.cpp) -----------------------

        /// The profile's stream URI with the camera credentials added, for FFmpeg.
        Result<std::string>
        stream_input(Camera& camera, DeviceSession& session, const std::string& profile);
        Reply start_video(Camera& camera, DeviceSession& session, const std::string& profile);
        void stop_video(Camera& camera);
        Reply video_status(Camera& camera);
        /// Serve the live video as MJPEG until the browser leaves.
        void stream_video(Camera& camera, web::ResponseStream& output);
        web::Response snapshot(Camera& camera, DeviceSession& session, const std::string& profile);
        Reply start_recording(Camera& camera, DeviceSession& session, const json::object& body);
        Reply recordings_view();
        Reply recording_action(const std::string& name, const std::string& action);
        void download(const std::string& name, web::ResponseStream& output);

        // ---- Routing (router.cpp) -----------------------------------------------

        /// Refuse requests from other origins or hosts, or without the session cookie.
        std::optional<Reply> guard(const web::Request& request);
        /// Map an `/api/...` request to its handler.
        Reply api(const web::Request& request);
        Reply camera_api(
            const std::shared_ptr<Camera>& camera,
            const web::Request& request,
            const std::vector<std::string>& parts,
            const json::object& body
        );
    };
} // namespace wlonvif::examples::explorer
