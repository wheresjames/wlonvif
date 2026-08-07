#include "protocol.hpp"

#include <boost/json.hpp>
#include <algorithm>
#include <cmath>
#include <set>

namespace wlonvif::examples::ptz_web
{
    namespace
    {
        std::optional<std::string> text(const boost::json::object& object, std::string_view key)
        {
            const auto* value = object.if_contains(key);
            return value && value->is_string() && !value->as_string().empty() &&
                   value->as_string().size() <= 256
                ? std::optional<std::string>{value->as_string().c_str()} : std::nullopt;
        }
        std::optional<double> number(const boost::json::object& object, std::string_view key)
        {
            const auto* value = object.if_contains(key);
            if (!value || !value->is_number()) return std::nullopt;
            const double result = value->to_number<double>();
            return std::isfinite(result) ? std::optional<double>{result} : std::nullopt;
        }
        std::optional<bool> boolean(const boost::json::object& object, std::string_view key,
                                    bool fallback = false)
        {
            const auto* value = object.if_contains(key);
            if (!value) return fallback;
            return value->is_bool() ? std::optional{value->as_bool()} : std::nullopt;
        }
        std::optional<std::uint64_t> unsigned_integer(
            const boost::json::object& object, std::string_view key
        )
        {
            const auto* value = object.if_contains(key);
            if (!value) return std::nullopt;
            if (value->is_uint64()) return value->as_uint64();
            if (value->is_int64() && value->as_int64() >= 0)
                return static_cast<std::uint64_t>(value->as_int64());
            return std::nullopt;
        }
        bool only_fields(const boost::json::object& object,
                         std::initializer_list<std::string_view> allowed)
        {
            for (const auto& item : object)
                if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
                    return false;
            return true;
        }
        Error invalid(std::string message, ErrorCategory category = ErrorCategory::invalid_argument)
        {
            return Error{category, OperationId::continuous_move, std::move(message)};
        }
        bool unit(const std::optional<double>& value)
        {
            return value && std::isfinite(*value) && *value >= -1 && *value <= 1;
        }
    }

    ControlProtocol::ControlProtocol(ProtocolConfig config) : config_(std::move(config)) {}
    bool ControlProtocol::origin_allowed(std::string_view origin) const
    {
        return !origin.empty() && std::find(config_.allowed_origins.begin(),
            config_.allowed_origins.end(), origin) != config_.allowed_origins.end();
    }
    bool ControlProtocol::session_valid(std::string_view session_token,
                                        std::chrono::steady_clock::time_point now) const
    {
        const auto found = sessions_.find(std::string(session_token));
        return found != sessions_.end() && now <= found->second.expires;
    }
    Result<std::string> ControlProtocol::create_session(
        std::string_view origin, std::chrono::steady_clock::time_point now,
        std::string session_token)
    {
        if (!origin_allowed(origin)) return invalid("origin is not allowed", ErrorCategory::authorization);
        if (session_token.size() < 32 || session_token.size() > 128)
            return invalid("invalid generated session token", ErrorCategory::internal);
        (void)expire(now);
        if (sessions_.size() >= config_.maximum_sessions)
            return invalid("session limit reached", ErrorCategory::queue_overflow);
        Session session;
        session.owner = "web-" + std::to_string(++next_owner_);
        session.expires = now + config_.lease;
        session.rate_window = now;
        sessions_.insert_or_assign(session_token, session);
        return session.owner;
    }
    Result<WebCommand> ControlProtocol::handle(
        std::string_view session_token, std::string_view origin, std::string_view message,
        std::chrono::steady_clock::time_point now)
    {
        if (message.size() > config_.maximum_message_bytes)
            return invalid("control message exceeds size limit", ErrorCategory::queue_overflow);
        if (!origin_allowed(origin)) return invalid("origin is not allowed", ErrorCategory::authorization);
        auto found = sessions_.find(std::string(session_token));
        if (found == sessions_.end()) return invalid("unknown session", ErrorCategory::authentication);
        auto& session = found->second;
        if (now > session.expires)
        {
            sessions_.erase(found);
            return invalid("session lease expired", ErrorCategory::authentication);
        }
        if (now - session.rate_window >= std::chrono::seconds(1))
        {
            session.rate_window = now;
            session.rate_count = 0;
        }
        if (++session.rate_count > config_.maximum_messages_per_second)
            return invalid("control message rate exceeded", ErrorCategory::queue_overflow);
        boost::system::error_code json_error;
        auto document = boost::json::parse(message, json_error);
        if (json_error || !document.is_object()) return invalid("malformed control JSON");
        const auto& object = document.as_object();
        const auto version = unsigned_integer(object, "version");
        const auto sequence = unsigned_integer(object, "sequence");
        const auto type = text(object, "type");
        if (!version || *version != 1 || !sequence || *sequence < 1 || !type)
            return invalid("invalid control message envelope");
        const auto integer_sequence = *sequence;
        if (integer_sequence <= session.sequence) return invalid("control sequence is stale");
        WebCommand command;
        command.owner = session.owner;
        command.sequence = integer_sequence;
        if (*type == "intent")
        {
            const auto pan = number(object, "pan");
            const auto tilt = number(object, "tilt");
            const auto zoom = number(object, "zoom");
            if (!only_fields(object, {"version", "type", "sequence", "pan", "tilt", "zoom"}) ||
                !unit(pan) || !unit(tilt) || !unit(zoom)) return invalid("invalid intent axes");
            command.type = CommandType::intent;
            command.intent = PtzVector{*pan, *tilt, *zoom};
        }
        else if (*type == "stop")
        {
            if (!only_fields(object, {"version", "type", "sequence"}))
                return invalid("invalid stop message");
            command.type = CommandType::stop;
        }
        else if (*type == "click" || *type == "box")
        {
            const auto x = number(object, "x"); const auto y = number(object, "y");
            const auto width = number(object, "width"); const auto height = number(object, "height");
            if (!x || !y || !width || !height) return invalid("invalid viewport command");
            command.first = ViewportPoint{*x, *y, *width, *height};
            command.type = *type == "click" ? CommandType::click : CommandType::box;
            if (command.type == CommandType::box)
            {
                const auto x2 = number(object, "x2"); const auto y2 = number(object, "y2");
                if (!x2 || !y2) return invalid("invalid box command");
                command.second = ViewportPoint{*x2, *y2, *width, *height};
            }
            if (auto age = unsigned_integer(object, "calibration_age_ms"))
            {
                command.calibration_age = std::chrono::milliseconds(
                    static_cast<std::chrono::milliseconds::rep>(*age));
            }
            const auto invert_pan = boolean(object, "invert_pan");
            const auto invert_tilt = boolean(object, "invert_tilt");
            if (!invert_pan || !invert_tilt) return invalid("invalid viewport inversion setting");
            command.invert_pan = *invert_pan;
            command.invert_tilt = *invert_tilt;
            const auto allowed = command.type == CommandType::click
                ? only_fields(object, {"version", "type", "sequence", "x", "y", "width",
                                       "height", "calibration_age_ms", "invert_pan", "invert_tilt"})
                : only_fields(object, {"version", "type", "sequence", "x", "y", "x2", "y2",
                                       "width", "height", "calibration_age_ms", "invert_pan",
                                       "invert_tilt"});
            if (!allowed) return invalid("unexpected viewport message field");
        }
        else if (*type == "preset")
        {
            auto preset = text(object, "preset");
            if (!preset || !only_fields(object, {"version", "type", "sequence", "preset"}))
                return invalid("preset token is required");
            command.type = CommandType::preset;
            command.preset = std::move(*preset);
        }
        else if (*type == "select_profile")
        {
            auto profile = text(object, "profile");
            if (!profile || !only_fields(object, {"version", "type", "sequence", "profile"}))
                return invalid("profile token is required");
            command.type = CommandType::select_profile;
            command.profile = std::move(*profile);
        }
        else if (*type == "tour")
        {
            const auto* presets = object.if_contains("presets");
            const auto dwell = unsigned_integer(object, "dwell_ms");
            const auto repeats = unsigned_integer(object, "repeats");
            if (!presets || !presets->is_array() || presets->as_array().empty() ||
                presets->as_array().size() > 16 || !dwell || *dwell < 100 || *dwell > 60000 ||
                !repeats || *repeats < 1 || *repeats > 10 ||
                !only_fields(object, {"version", "type", "sequence", "presets", "dwell_ms", "repeats"}))
                return invalid("invalid tour command");
            for (const auto& item : presets->as_array())
            {
                if (!item.is_string() || item.as_string().empty() || item.as_string().size() > 256)
                    return invalid("invalid tour preset token");
                command.tour_presets.emplace_back(item.as_string().c_str());
            }
            command.type = CommandType::tour;
            command.dwell = std::chrono::milliseconds(*dwell);
            command.repeats = static_cast<std::size_t>(*repeats);
        }
        else return invalid("unknown control message type");
        session.sequence = integer_sequence;
        session.expires = now + config_.lease;
        return command;
    }
    std::optional<std::string> ControlProtocol::disconnect(std::string_view session_token)
    {
        auto found = sessions_.find(std::string(session_token));
        if (found == sessions_.end()) return std::nullopt;
        auto owner = found->second.owner;
        sessions_.erase(found);
        return owner;
    }
    std::vector<std::string> ControlProtocol::expire(std::chrono::steady_clock::time_point now)
    {
        std::vector<std::string> owners;
        for (auto it = sessions_.begin(); it != sessions_.end();)
        {
            if (now > it->second.expires)
            {
                owners.push_back(it->second.owner);
                it = sessions_.erase(it);
            }
            else ++it;
        }
        return owners;
    }
    void ControlProtocol::revoke_all() { sessions_.clear(); }
}
