/** @file main.cpp @brief Safely inspect and control a real ONVIF PTZ device. */

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <example_support.hpp>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace
{
    bool parse_number(std::string_view text, double& output)
    {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), output);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }

    void usage(const char* executable)
    {
        std::cout
            << "Usage: " << executable
            << " (--url DEVICE_URL | --ip ADDRESS) --profile TOKEN COMMAND [options]\n\n"
            << "Commands: status, presets, stop, move, goto-preset, tour, keyboard\n"
            << "  move [--pan N] [--tilt N] [--zoom N] [--continuous]\n"
            << "       [--duration-ms N] --confirm-movement\n"
            << "  goto-preset --preset TOKEN --confirm-movement\n\n"
            << "  tour --preset TOKEN --preset TOKEN [...] [--duration-ms N]"
               " --confirm-movement\n\n"
            << "  keyboard [--speed 0.01..0.25] [--initial-hold-ms 100..2000]\n"
            << "           [--release-ms 50..1000] [--latched] --confirm-movement\n"
            << "    WASD/cursor keys: pan and tilt; +/-: zoom; Space: Stop; Q: quit\n\n"
            << "Credentials: --username NAME --password PASSWORD; environment and prompt"
               " fallbacks apply.\n"
            << "Movement defaults to relative, clamps axes to [-0.25,+0.25], and always Stop.\n";
    }

    class StopGuard
    {
      public:
        StopGuard(std::shared_ptr<wlonvif::PtzClient> client, std::string profile)
            : client_(std::move(client)), profile_(std::move(profile))
        {
        }
        void activate() { active_ = true; }
        wlonvif::Result<void> stop()
        {
            if (!active_) return {};
            active_ = false;
            return wlonvif::examples::wait_for_task(
                client_->emergency_stop(profile_, wlonvif::examples::safety_call_options(3))
            );
        }
        ~StopGuard()
        {
            if (active_)
            {
                (void)wlonvif::examples::wait_for_task(
                    client_->emergency_stop(profile_, wlonvif::examples::safety_call_options(3))
                );
            }
        }

      private:
        std::shared_ptr<wlonvif::PtzClient> client_;
        std::string profile_;
        bool active_{};
    };

    enum class Key
    {
        none,
        up,
        down,
        left,
        right,
        zoom_in,
        zoom_out,
        stop,
        quit
    };

    class KeyboardTerminal
    {
      public:
        KeyboardTerminal()
        {
#ifdef _WIN32
            valid_ = _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
            valid_ = isatty(fileno(stdin)) != 0 && isatty(fileno(stdout)) != 0 &&
                     tcgetattr(fileno(stdin), &original_) == 0;
            if (valid_)
            {
                auto raw = original_;
                raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
                raw.c_cc[VMIN] = 0;
                raw.c_cc[VTIME] = 1;
                valid_ = tcsetattr(fileno(stdin), TCSAFLUSH, &raw) == 0;
            }
#endif
            if (valid_)
            {
                std::cout << "\033[?25l" << std::flush;
            }
        }

        ~KeyboardTerminal()
        {
#ifndef _WIN32
            if (valid_)
            {
                (void)tcsetattr(fileno(stdin), TCSAFLUSH, &original_);
            }
#endif
            if (valid_)
            {
                std::cout << "\033[?25h\n" << std::flush;
            }
        }

        bool valid() const { return valid_; }

        Key read_key()
        {
#ifdef _WIN32
            if (!_kbhit())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return Key::none;
            }
            int value = _getch();
            if (value == 0 || value == 224)
            {
                value = _getch();
                if (value == 72) return Key::up;
                if (value == 80) return Key::down;
                if (value == 75) return Key::left;
                if (value == 77) return Key::right;
                return Key::none;
            }
#else
            unsigned char value = 0;
            if (read(fileno(stdin), &value, 1) != 1)
            {
                return Key::none;
            }
            if (value == 27)
            {
                unsigned char sequence[2]{};
                if (read(fileno(stdin), &sequence[0], 1) == 1 &&
                    read(fileno(stdin), &sequence[1], 1) == 1 && sequence[0] == '[')
                {
                    if (sequence[1] == 'A') return Key::up;
                    if (sequence[1] == 'B') return Key::down;
                    if (sequence[1] == 'C') return Key::right;
                    if (sequence[1] == 'D') return Key::left;
                }
                return Key::none;
            }
#endif
            if (value == 'w' || value == 'W') return Key::up;
            if (value == 's' || value == 'S') return Key::down;
            if (value == 'a' || value == 'A') return Key::left;
            if (value == 'd' || value == 'D') return Key::right;
            if (value == '+' || value == '=') return Key::zoom_in;
            if (value == '-' || value == '_') return Key::zoom_out;
            if (value == ' ') return Key::stop;
            if (value == 'q' || value == 'Q') return Key::quit;
            return Key::none;
        }

      private:
        bool valid_{};
#ifndef _WIN32
        termios original_{};
#endif
    };

    std::string_view key_name(Key key)
    {
        switch (key)
        {
            case Key::up: return "UP / W";
            case Key::down: return "DOWN / S";
            case Key::left: return "LEFT / A";
            case Key::right: return "RIGHT / D";
            case Key::zoom_in: return "ZOOM IN / +";
            case Key::zoom_out: return "ZOOM OUT / -";
            case Key::stop: return "STOP / SPACE";
            case Key::quit: return "QUIT / Q";
            case Key::none: return "none";
        }
        return "none";
    }

    void draw_keyboard(const wlonvif::PtzStatus& status, Key key, std::string_view state)
    {
        constexpr int width = 61;
        constexpr int height = 13;
        const auto pan = std::clamp(status.position.pan.value_or(0), -1.0, 1.0);
        const auto tilt = std::clamp(status.position.tilt.value_or(0), -1.0, 1.0);
        const int camera_x = static_cast<int>(std::lround((pan + 1.0) * 0.5 * (width - 1)));
        const int camera_y = static_cast<int>(std::lround((1.0 - tilt) * 0.5 * (height - 1)));

        const auto line = [](std::string text)
        {
            if (text.size() < static_cast<std::size_t>(width))
                text += std::string(static_cast<std::size_t>(width) - text.size(), ' ');
            std::cout << '|' << text.substr(0, width) << "|\n";
        };
        std::cout << "\033[2J\033[H" << '+' << std::string(width, '=') << "+\n";
        line("                 wlonvif PTZ keyboard control");
        std::cout << '+' << std::string(width, '=') << "+\n";
        line(" W / Up: tilt up                 S / Down: tilt down");
        line(" A / Left: pan left              D / Right: pan right");
        line(" +: zoom in    -: zoom out    Space: Stop    Q: quit");
        std::cout << '+' << std::string(width, '=') << "+\n";
        std::cout << '+' << std::string(width, '-') << "+\n";
        for (int y = 0; y < height; ++y)
        {
            std::cout << '|';
            for (int x = 0; x < width; ++x)
            {
                if (x == camera_x && y == camera_y) std::cout << '@';
                else if (x == width / 2 && y == height / 2) std::cout << '+';
                else if (x == width / 2) std::cout << '|';
                else if (y == height / 2) std::cout << '-';
                else std::cout << ' ';
            }
            std::cout << "|\n";
        }
        std::cout << '+' << std::string(width, '-') << "+\n"
                  << " Pan " << pan << "  Tilt " << tilt
                  << "  Zoom " << status.position.zoom.value_or(0) << '\n'
                  << " Key: [" << key_name(key) << "]  State: " << state
                  << "  Device moving: " << (status.moving ? "yes" : "no") << "\n"
                  << " Repeats are coalesced; release infers Stop. Q exits safely.\n"
                  << std::flush;
    }

    wlonvif::Result<void> keyboard_control(
        const std::shared_ptr<wlonvif::PtzClient>& client,
        const std::string& profile,
        StopGuard& guard,
        int timeout_seconds,
        double speed,
        bool latched,
        std::size_t initial_hold_ms,
        std::size_t release_ms
    )
    {
        KeyboardTerminal terminal;
        if (!terminal.valid())
        {
            return wlonvif::Error{
                wlonvif::ErrorCategory::invalid_argument,
                {},
                "keyboard mode requires an interactive terminal"
            };
        }
        auto current = wlonvif::examples::wait_for_task(
            client->status(profile, wlonvif::examples::call_options(timeout_seconds))
        );
        if (!current) return current.error();

        struct InputState
        {
            std::mutex mutex;
            Key key{Key::none};
            std::uint64_t sequence{};
            std::chrono::steady_clock::time_point observed{};
        } input;
        std::jthread reader(
            [&](std::stop_token stop)
            {
                while (!stop.stop_requested())
                {
                    const auto key = terminal.read_key();
                    if (key == Key::none) continue;
                    std::lock_guard lock(input.mutex);
                    input.key = key;
                    input.observed = std::chrono::steady_clock::now();
                    ++input.sequence;
                }
            }
        );

        const auto control = wlonvif::examples::call_options(3600);
        std::uint64_t processed_sequence = 0;
        Key active_key = Key::none;
        bool active = false;
        bool repeating = false;
        bool quit = false;
        auto release_at = std::chrono::steady_clock::time_point::max();
        draw_keyboard(current.value(), Key::none, latched ? "ready (latched)" : "ready");

        const auto stop_and_refresh = [&](Key key) -> wlonvif::Result<void>
        {
            if (active)
            {
                auto stopped = guard.stop();
                if (!stopped) return stopped.error();
            }
            else
            {
                guard.activate();
                auto stopped = guard.stop();
                if (!stopped) return stopped.error();
            }
            active = false;
            active_key = Key::none;
            repeating = false;
            current = wlonvif::examples::wait_for_task(
                client->status(profile, wlonvif::examples::call_options(timeout_seconds))
            );
            if (!current) return current.error();
            draw_keyboard(current.value(), key, "stopped");
            return {};
        };

        while (!control.cancellation.stop_requested() && !quit)
        {
            Key key = Key::none;
            std::uint64_t sequence = 0;
            std::chrono::steady_clock::time_point observed;
            {
                std::lock_guard lock(input.mutex);
                key = input.key;
                sequence = input.sequence;
                observed = input.observed;
            }
            if (sequence != processed_sequence)
            {
                processed_sequence = sequence;
                if (key == Key::quit)
                {
                    quit = true;
                    continue;
                }
                if (key == Key::stop)
                {
                    auto stopped = stop_and_refresh(key);
                    if (!stopped) return stopped.error();
                    continue;
                }

                const bool same_direction = active && key == active_key;
                if (same_direction)
                {
                    repeating = true;
                    if (!latched)
                        release_at = observed + std::chrono::milliseconds(release_ms);
                    continue;
                }
                if (!latched &&
                    std::chrono::steady_clock::now() >=
                        observed + std::chrono::milliseconds(initial_hold_ms))
                {
                    continue;
                }

                wlonvif::PtzVector velocity;
                if (key == Key::up) velocity.tilt = speed;
                else if (key == Key::down) velocity.tilt = -speed;
                else if (key == Key::left) velocity.pan = -speed;
                else if (key == Key::right) velocity.pan = speed;
                else if (key == Key::zoom_in) velocity.zoom = speed;
                else if (key == Key::zoom_out) velocity.zoom = -speed;
                else continue;
                current.value().moving = true;
                draw_keyboard(current.value(), key, latched ? "active (latched)" : "active");
                guard.activate();
                auto moved = wlonvif::examples::wait_for_task(client->continuous_move(
                    profile,
                    velocity,
                    wlonvif::examples::call_options(timeout_seconds)
                ));
                if (!moved)
                {
                    (void)guard.stop();
                    return moved.error();
                }
                active = true;
                active_key = key;
                repeating = false;
                if (!latched)
                    release_at = observed + std::chrono::milliseconds(initial_hold_ms);
            }

            if (active && !latched && std::chrono::steady_clock::now() >= release_at)
            {
                auto stopped = stop_and_refresh(repeating ? active_key : Key::stop);
                if (!stopped) return stopped.error();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        reader.request_stop();
        if (!active) return {};
        return stop_and_refresh(Key::quit);
    }
}

int main(int argc, char** argv)
{
    auto parsed = wlonvif::examples::parse_common_arguments(argc, argv);
    if (!parsed)
    {
        std::cerr << parsed.error().message << '\n';
        return 2;
    }
    auto common = std::move(parsed.value().common);
    std::string url, address, profile, command, preset;
    std::vector<std::string> tour_presets;
    wlonvif::PtzVector vector;
    bool continuous = false, confirmed = false;
    bool keyboard_latched = false;
    double keyboard_speed = 0.1;
    std::size_t duration_ms = 750;
    std::size_t initial_hold_ms = 550;
    std::size_t release_ms = 180;
    for (std::size_t index = 0; index < parsed.value().remaining.size(); ++index)
    {
        const auto& argument = parsed.value().remaining[index];
        if (argument == "--help" || argument == "-h")
        {
            usage(argv[0]);
            return 0;
        }
        if ((argument == "--url" || argument == "--ip" || argument == "--profile" || argument == "--preset" ||
             argument == "--pan" || argument == "--tilt" || argument == "--zoom" ||
             argument == "--speed" || argument == "--duration-ms" ||
             argument == "--initial-hold-ms" || argument == "--release-ms") &&
            index + 1 >= parsed.value().remaining.size())
        {
            std::cerr << "Missing value for " << argument << '\n';
            return 2;
        }
        if (argument == "--url") url = parsed.value().remaining[++index];
        else if (argument == "--ip") address = parsed.value().remaining[++index];
        else if (argument == "--profile") profile = parsed.value().remaining[++index];
        else if (argument == "--preset")
        {
            preset = parsed.value().remaining[++index];
            tour_presets.push_back(preset);
        }
        else if (argument == "--duration-ms")
        {
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++index], duration_ms) ||
                duration_ms > 5000)
            {
                std::cerr << "Duration must be between 1 and 5000 ms\n";
                return 2;
            }
        }
        else if (argument == "--speed")
        {
            if (!parse_number(parsed.value().remaining[++index], keyboard_speed) ||
                keyboard_speed < 0.01 || keyboard_speed > 0.25)
            {
                std::cerr << "Keyboard speed must be between 0.01 and 0.25\n";
                return 2;
            }
        }
        else if (argument == "--initial-hold-ms")
        {
            if (!wlonvif::examples::parse_size(
                    parsed.value().remaining[++index], initial_hold_ms
                ) ||
                initial_hold_ms < 100 || initial_hold_ms > 2000)
            {
                std::cerr << "Initial hold must be between 100 and 2000 ms\n";
                return 2;
            }
        }
        else if (argument == "--release-ms")
        {
            if (!wlonvif::examples::parse_size(parsed.value().remaining[++index], release_ms) ||
                release_ms < 50 || release_ms > 1000)
            {
                std::cerr << "Release delay must be between 50 and 1000 ms\n";
                return 2;
            }
        }
        else if (argument == "--pan" || argument == "--tilt" || argument == "--zoom")
        {
            double value = 0;
            if (!parse_number(parsed.value().remaining[++index], value) || value < -0.25 ||
                value > 0.25)
            {
                std::cerr << "PTZ components must be between -0.25 and +0.25\n";
                return 2;
            }
            if (argument == "--pan") vector.pan = value;
            else if (argument == "--tilt") vector.tilt = value;
            else vector.zoom = value;
        }
        else if (argument == "--continuous") continuous = true;
        else if (argument == "--latched") keyboard_latched = true;
        else if (argument == "--confirm-movement") confirmed = true;
        else if (command.empty()) command = argument;
        else
        {
            std::cerr << "Unknown argument: " << argument << '\n';
            return 2;
        }
    }
    if (url.empty() == address.empty() || profile.empty() || command.empty())
    {
        usage(argv[0]);
        return 2;
    }
    const bool movement = command == "move" || command == "goto-preset" || command == "tour" ||
                          command == "keyboard";
    if (movement && !confirmed)
    {
        std::cerr << "Movement requires --confirm-movement\n";
        return 2;
    }
    if (command == "goto-preset" && preset.empty())
    {
        std::cerr << "goto-preset requires --preset\n";
        return 2;
    }
    if (command == "tour" && tour_presets.size() < 2)
    {
        std::cerr << "tour requires at least two --preset values\n";
        return 2;
    }
    auto credentials = wlonvif::examples::load_credentials(common);
    if (!credentials)
    {
        std::cerr << credentials.error().message << '\n';
        return 2;
    }
    auto runtime_result = wlonvif::Runtime::create();
    if (!runtime_result) return 1;
    auto runtime = runtime_result.value();
    auto endpoint = wlonvif::examples::resolve_device_endpoint(
        url, address, runtime, common.timeout_seconds
    );
    if (!endpoint)
    {
        std::cerr << "Endpoint discovery failed: " << endpoint.error().message << '\n';
        (void)runtime->shutdown();
        return 1;
    }
    url = std::move(endpoint.value().url);
    auto session = wlonvif::examples::connect_device(
        url, runtime, credentials.value(), common.timeout_seconds
    );
    if (!session)
    {
        std::cerr << session.error().message << '\n';
        return 1;
    }
    auto ptz = session.value()->ptz();
    if (!ptz)
    {
        std::cerr << ptz.error().message << '\n';
        return 1;
    }
    StopGuard guard(ptz.value(), profile);
    if (command == "status")
    {
        auto result = wlonvif::examples::wait_for_task(
            ptz.value()->status(profile, wlonvif::examples::call_options(common.timeout_seconds))
        );
        if (!result) { std::cerr << result.error().message << '\n'; return 1; }
        const auto& value = result.value();
        std::cout << "Pan: " << value.position.pan.value_or(0) << "\nTilt: "
                  << value.position.tilt.value_or(0) << "\nZoom: "
                  << value.position.zoom.value_or(0) << "\nMoving: "
                  << (value.moving ? "yes" : "no") << '\n';
    }
    else if (command == "presets")
    {
        auto result = wlonvif::examples::wait_for_task(ptz.value()->get_presets(
            profile, wlonvif::examples::call_options(common.timeout_seconds)
        ));
        if (!result) { std::cerr << result.error().message << '\n'; return 1; }
        for (const auto& value : result.value()) std::cout << value.token << "  " << value.name << '\n';
    }
    else if (command == "stop")
    {
        guard.activate();
        auto result = guard.stop();
        if (!result) { std::cerr << "Stop failed: " << result.error().message << '\n'; return 1; }
    }
    else if (command == "goto-preset")
    {
        guard.activate();
        auto result = wlonvif::examples::wait_for_task(ptz.value()->goto_preset(
            profile, preset, wlonvif::examples::call_options(common.timeout_seconds)
        ));
        auto stopped = guard.stop();
        if (!result || !stopped)
        {
            std::cerr << (!result ? result.error().message : stopped.error().message) << '\n';
            return 1;
        }
    }
    else if (command == "move")
    {
        if (!vector.pan && !vector.tilt && !vector.zoom)
        {
            std::cerr << "move requires at least one axis\n";
            return 2;
        }
        guard.activate();
        const auto move_options = wlonvif::examples::call_options(common.timeout_seconds);
        auto result = continuous
                          ? wlonvif::examples::wait_for_task(ptz.value()->continuous_move(
                                profile,
                                vector,
                                move_options
                            ))
                          : wlonvif::examples::wait_for_task(ptz.value()->relative_move(
                                profile,
                                vector,
                                move_options
                            ));
        if (continuous && result)
        {
            const auto stop_at = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(duration_ms);
            while (!move_options.cancellation.stop_requested() &&
                   std::chrono::steady_clock::now() < stop_at)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        auto stopped = guard.stop();
        if (!result || !stopped)
        {
            std::cerr << (!result ? result.error().message : stopped.error().message) << '\n';
            return 1;
        }
    }
    else if (command == "tour")
    {
        const auto tour_options = wlonvif::examples::call_options(common.timeout_seconds);
        for (const auto& token : tour_presets)
        {
            if (tour_options.cancellation.stop_requested()) break;
            guard.activate();
            auto moved = wlonvif::examples::wait_for_task(
                ptz.value()->goto_preset(profile, token, tour_options)
            );
            if (!moved)
            {
                (void)guard.stop();
                std::cerr << moved.error().message << '\n';
                return 1;
            }
            const auto stop_at = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(duration_ms);
            while (!tour_options.cancellation.stop_requested() &&
                   std::chrono::steady_clock::now() < stop_at)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto stopped = guard.stop();
            if (!stopped)
            {
                std::cerr << "Stop failed: " << stopped.error().message << '\n';
                return 1;
            }
        }
    }
    else if (command == "keyboard")
    {
        const auto result = keyboard_control(
            ptz.value(),
            profile,
            guard,
            common.timeout_seconds,
            keyboard_speed,
            keyboard_latched,
            initial_hold_ms,
            release_ms
        );
        if (!result)
        {
            std::cerr << result.error().message << '\n';
            return 1;
        }
    }
    else
    {
        std::cerr << "Unknown command: " << command << '\n';
        return 2;
    }
    (void)runtime->shutdown();
    return 0;
}
