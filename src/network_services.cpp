#include "device_clock.hpp"
#include "network_services.hpp"

#include "soap_response.hpp"
#include "xml_scan.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <map>
#include <sstream>

namespace wlonvif
{
    namespace
    {
        thread_local std::optional<CallOptions> active_call_options;
    }

    std::optional<CallOptions> exchange_network_call_options(std::optional<CallOptions> options)
    {
        return std::exchange(active_call_options, std::move(options));
    }

    namespace
    {
        /**
         * @brief Run a cold task to completion on the calling thread.
         *
         * The synchronization state is shared-owned rather than left on the stack.
         * With stack state the waiter can wake, return, and destroy the condition
         * variable while the completing thread is still inside notify_one(), which
         * ThreadSanitizer flags on every call. Notifying under the lock closes the
         * same window from the other side.
         */
        template <class T> T wait_for_task(Task<T> task)
        {
            struct SharedState
            {
                std::mutex mutex;
                std::condition_variable ready;
                std::optional<T> result;
            };
            auto state = std::make_shared<SharedState>();
            std::move(task).start(
                [state](T value)
                {
                    std::lock_guard lock(state->mutex);
                    state->result.emplace(std::move(value));
                    state->ready.notify_one();
                }
            );
            std::unique_lock lock(state->mutex);
            state->ready.wait(lock, [&] { return state->result.has_value(); });
            return std::move(*state->result);
        }

        using detail_xml::attribute;
        using detail_xml::numeric_attribute;

        std::string xml_escape(std::string_view input)
        {
            return detail_xml::escape(input);
        }

        /// Return the character data of the first element with local name @p name.
        std::string first_element(std::string_view xml, std::string_view local_name)
        {
            return detail_xml::first_text(xml, local_name);
        }

        /// Return the raw markup inside the first element with local name @p name.
        std::string subtree(std::string_view xml, std::string_view local_name)
        {
            const auto element = detail_xml::find_element(xml, local_name);
            return element ? std::string(detail_xml::inner(xml, *element)) : std::string{};
        }

        std::optional<NativeRange> first_range(std::string_view xml, std::string_view name)
        {
            const auto section = subtree(xml, name);
            if (section.empty()) return std::nullopt;
            const auto minimum = detail_xml::parse_double(first_element(section, "Min"));
            const auto maximum = detail_xml::parse_double(first_element(section, "Max"));
            if (!minimum || !maximum) return std::nullopt;
            return NativeRange{*minimum, *maximum};
        }

        /**
         * @brief Parse an ONVIF xs:dateTime such as `2024-05-06T07:08:09.5Z`.
         *
         * Returns nullopt for anything it cannot fully account for, so the caller
         * can fall back rather than reporting a fabricated timestamp.
         */
        std::optional<std::chrono::system_clock::time_point>
        parse_xsd_datetime(std::string_view text)
        {
            int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
            std::size_t index = 0;
            const auto number = [&](int& output, std::size_t digits)
            {
                if (index + digits > text.size()) return false;
                const auto parsed = std::from_chars(
                    text.data() + index, text.data() + index + digits, output
                );
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + index + digits)
                {
                    return false;
                }
                index += digits;
                return true;
            };
            const auto literal = [&](char expected)
            {
                if (index >= text.size() || text[index] != expected) return false;
                ++index;
                return true;
            };
            if (!number(year, 4) || !literal('-') || !number(month, 2) || !literal('-') ||
                !number(day, 2) || !literal('T') || !number(hour, 2) || !literal(':') ||
                !number(minute, 2) || !literal(':') || !number(second, 2))
            {
                return std::nullopt;
            }
            if (index < text.size() && text[index] == '.')
            {
                ++index;
                while (index < text.size() && text[index] >= '0' && text[index] <= '9') ++index;
            }
            std::chrono::minutes offset{0};
            if (index < text.size() && (text[index] == '+' || text[index] == '-'))
            {
                const int sign = text[index] == '-' ? -1 : 1;
                ++index;
                int offset_hours = 0, offset_minutes = 0;
                if (!number(offset_hours, 2) || !literal(':') || !number(offset_minutes, 2))
                {
                    return std::nullopt;
                }
                offset = sign * (std::chrono::hours(offset_hours) +
                                 std::chrono::minutes(offset_minutes));
            }
            else if (index < text.size() && (text[index] == 'Z' || text[index] == 'z'))
            {
                ++index;
            }
            if (index != text.size()) return std::nullopt;
            const auto date = std::chrono::year(year) /
                              std::chrono::month(static_cast<unsigned>(month)) /
                              std::chrono::day(static_cast<unsigned>(day));
            if (!date.ok() || hour > 23 || minute > 59 || second > 60) return std::nullopt;
            return std::chrono::system_clock::time_point(
                std::chrono::duration_cast<std::chrono::system_clock::duration>(
                    std::chrono::sys_days(date).time_since_epoch() + std::chrono::hours(hour) +
                    std::chrono::minutes(minute) + std::chrono::seconds(second) - offset
                )
            );
        }

        /// Join every SimpleItem in @p xml as comma-separated `name=value` pairs.
        std::string simple_items(std::string_view xml)
        {
            std::string output;
            detail_xml::for_each_element(
                xml,
                "SimpleItem",
                [&](const detail_xml::Element& item)
                {
                    const auto raw = detail_xml::attributes(xml, item);
                    const auto name = attribute(raw, "Name");
                    if (name.empty()) return;
                    if (!output.empty()) output += ',';
                    output += name + '=' + attribute(raw, "Value");
                }
            );
            return output;
        }

        /// Names of every SimpleItemDescription in @p xml.
        std::vector<std::string> item_descriptions(std::string_view xml)
        {
            std::vector<std::string> output;
            detail_xml::for_each_element(
                xml,
                "SimpleItemDescription",
                [&](const detail_xml::Element& item)
                {
                    auto name = attribute(detail_xml::attributes(xml, item), "Name");
                    if (!name.empty()) output.push_back(std::move(name));
                }
            );
            return output;
        }

        /**
         * @brief Collect every topic in a GetEventProperties TopicSet.
         *
         * Topics are the elements flagged `wstop:topic="true"`. A topic's expression
         * is the path of element names below the TopicSet; the root keeps its
         * namespace prefix (`tns1:`) because filters and notifications use it.
         */
        std::vector<EventTopic> parse_topic_set(std::string_view xml)
        {
            std::vector<EventTopic> output;
            const auto set = detail_xml::find_element(xml, "TopicSet");
            if (!set) return output;
            const auto topics = detail_xml::inner(xml, *set);
            std::vector<std::string> path;
            std::optional<std::size_t> topic_depth; ///< Path depth of output.back().
            for (std::size_t position = 0; position < topics.size();)
            {
                position = topics.find('<', position);
                if (position == topics.npos) break;
                if (detail_xml::starts_with_at(topics, position, "<!") ||
                    detail_xml::starts_with_at(topics, position, "<?"))
                {
                    position = detail_xml::skip_construct(topics, position);
                    continue;
                }
                const auto tag = detail_xml::parse_tag(topics, position);
                if (!tag)
                {
                    ++position;
                    continue;
                }
                if (tag->closing)
                {
                    if (!path.empty()) path.pop_back();
                    position = tag->end;
                    continue;
                }
                if (tag->local_name == "MessageDescription")
                {
                    // The description's subtree is the payload schema, not topics.
                    const auto description =
                        detail_xml::find_element(topics, "MessageDescription", position);
                    if (!description) break;
                    if (topic_depth && *topic_depth == path.size())
                    {
                        const auto body = detail_xml::inner(topics, *description);
                        auto& topic = output.back();
                        topic.property =
                            attribute(detail_xml::attributes(topics, *description), "IsProperty") ==
                            "true";
                        if (const auto source = detail_xml::find_element(body, "Source"))
                            topic.source = item_descriptions(detail_xml::inner(body, *source));
                        if (const auto data = detail_xml::find_element(body, "Data"))
                            topic.data = item_descriptions(detail_xml::inner(body, *data));
                    }
                    position = description->end;
                    continue;
                }
                const auto qualified = topics.substr(position + 1, tag->attributes_begin - position - 1);
                path.emplace_back(path.empty() ? qualified : tag->local_name);
                if (attribute(topics.substr(tag->attributes_begin,
                                            tag->attributes_end - tag->attributes_begin),
                              "topic") == "true")
                {
                    std::string expression;
                    for (const auto& part : path) expression += (expression.empty() ? "" : "/") + part;
                    output.push_back({std::move(expression), false, {}, {}});
                    topic_depth = path.size();
                }
                if (tag->self_closing) path.pop_back();
                position = tag->end;
            }
            return output;
        }

        /// Decode every module or rule element named @p element (tt:Config).
        std::vector<AnalyticsConfig> analytics_configs(std::string_view xml, std::string_view element)
        {
            std::vector<AnalyticsConfig> output;
            detail_xml::for_each_element(
                xml,
                element,
                [&](const detail_xml::Element& config)
                {
                    AnalyticsConfig decoded;
                    const auto raw = detail_xml::attributes(xml, config);
                    decoded.name = attribute(raw, "Name");
                    decoded.type = attribute(raw, "Type");
                    const auto body = detail_xml::inner(xml, config);
                    std::vector<std::pair<std::size_t, std::size_t>> structured;
                    detail_xml::for_each_element(
                        body,
                        "ElementItem",
                        [&](const detail_xml::Element& item)
                        {
                            structured.emplace_back(item.begin, item.end);
                            decoded.element_items.push_back({
                                attribute(detail_xml::attributes(body, item), "Name"),
                                std::string(detail_xml::inner(body, item))
                            });
                        }
                    );
                    detail_xml::for_each_element(
                        body,
                        "SimpleItem",
                        [&](const detail_xml::Element& item)
                        {
                            // Items inside a structured parameter belong to it.
                            for (const auto& [begin, end] : structured)
                                if (item.begin > begin && item.begin < end) return;
                            const auto attributes = detail_xml::attributes(body, item);
                            decoded.simple_items.push_back(
                                {attribute(attributes, "Name"), attribute(attributes, "Value")}
                            );
                        }
                    );
                    output.push_back(std::move(decoded));
                }
            );
            return output;
        }

        /// Serialize @p configs as `tan:<element>` children for a Modify request.
        std::string analytics_body(
            const std::string& configuration,
            const std::vector<AnalyticsConfig>& configs,
            std::string_view element
        )
        {
            std::string body = "<tan:ConfigurationToken>" + xml_escape(configuration) +
                               "</tan:ConfigurationToken>";
            for (const auto& config : configs)
            {
                body += "<tan:" + std::string(element) + " Name=\"" + xml_escape(config.name) +
                        "\" Type=\"" + xml_escape(config.type) + "\"><tt:Parameters>";
                for (const auto& item : config.simple_items)
                {
                    body += "<tt:SimpleItem Name=\"" + xml_escape(item.name) + "\" Value=\"" +
                            xml_escape(item.value) + "\"/>";
                }
                // Structured parameters go back exactly as the device supplied them.
                for (const auto& item : config.element_items)
                {
                    body += "<tt:ElementItem Name=\"" + xml_escape(item.name) + "\">" + item.xml +
                            "</tt:ElementItem>";
                }
                body += "</tt:Parameters></tan:" + std::string(element) + '>';
            }
            return body;
        }

        /// Decode an Imaging Min/Max pair, or nullopt when absent or malformed.
        std::optional<ImagingRange> imaging_range(std::string_view xml, std::string_view name)
        {
            const auto element = detail_xml::find_element(xml, name);
            if (!element) return std::nullopt;
            const auto body = detail_xml::inner(xml, *element);
            const auto minimum = detail_xml::parse_double(detail_xml::first_text(body, "Min"));
            const auto maximum = detail_xml::parse_double(detail_xml::first_text(body, "Max"));
            if (!minimum || !maximum) return std::nullopt;
            return ImagingRange{*minimum, *maximum};
        }

        /// Text of every @p child inside the first @p parent element.
        std::vector<std::string>
        child_texts(std::string_view xml, std::string_view parent, std::string_view child)
        {
            std::vector<std::string> output;
            const auto element = detail_xml::find_element(xml, parent);
            if (!element) return output;
            const auto body = detail_xml::inner(xml, *element);
            detail_xml::for_each_element(
                body, child,
                [&](const detail_xml::Element& item) { output.push_back(detail_xml::text(body, item)); }
            );
            return output;
        }

        /// Mode of a structured imaging setting such as BacklightCompensation.
        std::optional<std::string> imaging_mode(std::string_view xml, std::string_view name)
        {
            const auto element = detail_xml::find_element(xml, name);
            if (!element) return std::nullopt;
            auto mode = detail_xml::first_text(detail_xml::inner(xml, *element), "Mode");
            return mode.empty() ? std::nullopt : std::optional<std::string>(std::move(mode));
        }

        /// Serialize the set fields of @p settings in tt:ImagingSettings20 order.
        std::string imaging_settings_xml(const ImagingSettings& settings)
        {
            std::string output;
            const auto number = [&](std::string_view name, const std::optional<double>& value)
            {
                if (value)
                    output += "<tt:" + std::string(name) + '>' + detail_xml::format_double(*value) +
                              "</tt:" + std::string(name) + '>';
            };
            const auto mode = [&](std::string_view name, const std::optional<std::string>& value)
            {
                if (value)
                    output += "<tt:" + std::string(name) + "><tt:Mode>" + xml_escape(*value) +
                              "</tt:Mode></tt:" + std::string(name) + '>';
            };
            mode("BacklightCompensation", settings.backlight_compensation);
            number("Brightness", settings.brightness);
            number("ColorSaturation", settings.color_saturation);
            number("Contrast", settings.contrast);
            if (settings.ir_cut_filter)
                output += "<tt:IrCutFilter>" + xml_escape(*settings.ir_cut_filter) + "</tt:IrCutFilter>";
            number("Sharpness", settings.sharpness);
            mode("WideDynamicRange", settings.wide_dynamic_range);
            mode("WhiteBalance", settings.white_balance);
            return output;
        }

        int integer_in(std::string_view xml, std::string_view name)
        {
            return static_cast<int>(
                detail_xml::parse_double(detail_xml::first_text(xml, name)).value_or(0)
            );
        }

        std::optional<IntegerRange> integer_range(std::string_view xml, std::string_view name)
        {
            const auto element = detail_xml::find_element(xml, name);
            if (!element) return std::nullopt;
            const auto body = detail_xml::inner(xml, *element);
            return IntegerRange{integer_in(body, "Min"), integer_in(body, "Max")};
        }

        /// Decode a tt:VideoEncoderConfiguration from its attributes and content.
        VideoEncoderConfiguration video_encoder(std::string_view attributes, std::string_view body)
        {
            VideoEncoderConfiguration output;
            output.token = attribute(attributes, "token");
            output.name = detail_xml::first_text(body, "Name");
            output.use_count = integer_in(body, "UseCount");
            output.encoding = detail_xml::first_text(body, "Encoding");
            output.resolution = {integer_in(body, "Width"), integer_in(body, "Height")};
            output.quality = detail_xml::parse_double(detail_xml::first_text(body, "Quality")).value_or(0);
            output.frame_rate_limit = integer_in(body, "FrameRateLimit");
            output.encoding_interval = integer_in(body, "EncodingInterval");
            output.bitrate_limit = integer_in(body, "BitrateLimit");
            for (const auto codec : {"H264", "MPEG4"})
            {
                const auto element = detail_xml::find_element(body, codec);
                if (!element) continue;
                const auto settings = detail_xml::inner(body, *element);
                output.gov_length = integer_in(settings, "GovLength");
                output.profile = detail_xml::first_text(
                    settings, std::string_view(codec) == "H264" ? "H264Profile" : "Mpeg4Profile"
                );
            }
            if (const auto multicast = detail_xml::find_element(body, "Multicast"))
            {
                const auto group = detail_xml::inner(body, *multicast);
                output.multicast_address = detail_xml::first_text(group, "IPv4Address");
                if (output.multicast_address.empty())
                    output.multicast_address = detail_xml::first_text(group, "IPv6Address");
                output.multicast_port = integer_in(group, "Port");
                output.multicast_ttl = integer_in(group, "TTL");
                output.multicast_auto_start = detail_xml::first_text(group, "AutoStart") == "true";
            }
            output.session_timeout = detail_xml::first_text(body, "SessionTimeout");
            return output;
        }

        /// Serialize @p configuration as the content of trt:Configuration.
        std::string video_encoder_xml(const VideoEncoderConfiguration& configuration)
        {
            const auto element = [](std::string_view name, const std::string& value)
            {
                return "<tt:" + std::string(name) + '>' + xml_escape(value) + "</tt:" +
                       std::string(name) + '>';
            };
            const auto number = [&](std::string_view name, long long value)
            {
                return element(name, std::to_string(value));
            };
            std::string output = element("Name", configuration.name) +
                                 number("UseCount", configuration.use_count) +
                                 element("Encoding", configuration.encoding) + "<tt:Resolution>" +
                                 number("Width", configuration.resolution.width) +
                                 number("Height", configuration.resolution.height) +
                                 "</tt:Resolution>" +
                                 element("Quality", detail_xml::format_double(configuration.quality)) +
                                 "<tt:RateControl>" +
                                 number("FrameRateLimit", configuration.frame_rate_limit) +
                                 number("EncodingInterval", configuration.encoding_interval) +
                                 number("BitrateLimit", configuration.bitrate_limit) +
                                 "</tt:RateControl>";
            if (configuration.encoding == "H264" || configuration.encoding == "MPEG4")
            {
                const bool h264 = configuration.encoding == "H264";
                output += std::string(h264 ? "<tt:H264>" : "<tt:MPEG4>") +
                          number("GovLength", configuration.gov_length.value_or(0)) +
                          element(h264 ? "H264Profile" : "Mpeg4Profile", configuration.profile) +
                          (h264 ? "</tt:H264>" : "</tt:MPEG4>");
            }
            const auto& address = configuration.multicast_address;
            const bool ipv6 = address.find(':') != std::string::npos;
            output += "<tt:Multicast><tt:Address>" + element("Type", ipv6 ? "IPv6" : "IPv4") +
                      element(ipv6 ? "IPv6Address" : "IPv4Address", address.empty() ? "0.0.0.0" : address) +
                      "</tt:Address>" + number("Port", configuration.multicast_port) +
                      number("TTL", configuration.multicast_ttl) +
                      element("AutoStart", configuration.multicast_auto_start ? "true" : "false") +
                      "</tt:Multicast>" +
                      element("SessionTimeout", configuration.session_timeout.empty()
                                                    ? std::string("PT60S")
                                                    : configuration.session_timeout);
            return output;
        }

        /// Decode one profile of a GetProfiles response.
        ProfileConfiguration profile_configuration(std::string_view attributes, std::string_view body)
        {
            ProfileConfiguration output;
            output.token = attribute(attributes, "token");
            output.fixed = attribute(attributes, "fixed") == "true";
            output.name = detail_xml::first_text(body, "Name");
            const auto token_of = [&](std::string_view name)
            {
                const auto element = detail_xml::find_element(body, name);
                return element ? attribute(detail_xml::attributes(body, *element), "token") : std::string{};
            };
            if (const auto element = detail_xml::find_element(body, "VideoSourceConfiguration"))
            {
                const auto content = detail_xml::inner(body, *element);
                VideoSourceConfiguration source;
                source.token = attribute(detail_xml::attributes(body, *element), "token");
                source.name = detail_xml::first_text(content, "Name");
                source.source_token = detail_xml::first_text(content, "SourceToken");
                if (const auto bounds = detail_xml::find_element(content, "Bounds"))
                {
                    const auto raw = detail_xml::attributes(content, *bounds);
                    const auto number = [&](std::string_view name)
                    { return static_cast<int>(detail_xml::parse_double(attribute(raw, name)).value_or(0)); };
                    source.x = number("x");
                    source.y = number("y");
                    source.width = number("width");
                    source.height = number("height");
                }
                output.video_source = std::move(source);
            }
            if (const auto element = detail_xml::find_element(body, "VideoEncoderConfiguration"))
                output.video_encoder =
                    video_encoder(detail_xml::attributes(body, *element), detail_xml::inner(body, *element));
            if (const auto element = detail_xml::find_element(body, "AudioSourceConfiguration"))
                output.audio_source = detail_xml::first_text(detail_xml::inner(body, *element), "SourceToken");
            if (const auto element = detail_xml::find_element(body, "AudioEncoderConfiguration"))
            {
                const auto content = detail_xml::inner(body, *element);
                output.audio_encoder = AudioEncoderConfiguration{
                    attribute(detail_xml::attributes(body, *element), "token"),
                    detail_xml::first_text(content, "Name"),
                    detail_xml::first_text(content, "Encoding"),
                    integer_in(content, "Bitrate"),
                    integer_in(content, "SampleRate")
                };
            }
            output.ptz_configuration = token_of("PTZConfiguration");
            output.video_analytics_configuration = token_of("VideoAnalyticsConfiguration");
            return output;
        }

        /// Decode a GetVideoEncoderConfigurationOptions response.
        VideoEncoderOptions video_encoder_options_from(std::string_view xml)
        {
            VideoEncoderOptions output;
            const auto options_element = detail_xml::find_element(xml, "Options");
            const auto options = options_element ? detail_xml::inner(xml, *options_element) : xml;
            output.quality = integer_range(options, "QualityRange").value_or(IntegerRange{});
            // Media1 puts bitrate ranges in a parallel Extension, so per-encoding
            // settings are read from before it and bitrates from inside it.
            const auto extension = detail_xml::find_element(options, "Extension");
            const auto base = extension ? options.substr(0, extension->begin) : options;
            const auto extended = extension ? detail_xml::inner(options, *extension) : std::string_view{};
            for (const auto codec : {"H264", "JPEG", "MPEG4"})
            {
                const auto element = detail_xml::find_element(base, codec);
                if (!element) continue;
                const auto body = detail_xml::inner(base, *element);
                VideoEncodingOptions encoding;
                encoding.encoding = codec;
                detail_xml::for_each_element(
                    body, "ResolutionsAvailable",
                    [&](const detail_xml::Element& size)
                    {
                        const auto content = detail_xml::inner(body, size);
                        encoding.resolutions.push_back({integer_in(content, "Width"), integer_in(content, "Height")});
                    }
                );
                encoding.frame_rate = integer_range(body, "FrameRateRange").value_or(IntegerRange{});
                encoding.encoding_interval =
                    integer_range(body, "EncodingIntervalRange").value_or(IntegerRange{});
                encoding.gov_length = integer_range(body, "GovLengthRange");
                for (const auto profiles : {"H264ProfilesSupported", "Mpeg4ProfileSupported"})
                    detail_xml::for_each_element(
                        body, profiles,
                        [&](const detail_xml::Element& profile)
                        { encoding.profiles.push_back(detail_xml::text(body, profile)); }
                    );
                if (const auto more = detail_xml::find_element(extended, codec))
                    encoding.bitrate = integer_range(detail_xml::inner(extended, *more), "BitrateRange");
                output.encodings.push_back(std::move(encoding));
            }
            return output;
        }

        /// Parse an xs:duration such as `PT1M5S` to whole seconds; nullopt if malformed.
        std::optional<std::chrono::seconds> parse_duration(std::string_view text)
        {
            if (text.size() < 2 || text[0] != 'P') return std::nullopt;
            long long total = 0, value = 0;
            bool time = false, digits = false;
            for (std::size_t index = 1; index < text.size(); ++index)
            {
                const char c = text[index];
                if (c >= '0' && c <= '9')
                {
                    value = value * 10 + (c - '0');
                    digits = true;
                    continue;
                }
                if (c == '.')
                {
                    // Fractions of a second are dropped; skip to the designator.
                    while (index + 1 < text.size() && text[index + 1] >= '0' && text[index + 1] <= '9') ++index;
                    continue;
                }
                if (c == 'T' && !time)
                {
                    time = true;
                    continue;
                }
                if (!digits) return std::nullopt;
                const long long unit = c == 'D' && !time ? 86400 : c == 'H' && time ? 3600
                                     : c == 'M' && time ? 60 : c == 'S' && time ? 1 : 0;
                if (unit == 0) return std::nullopt;
                total += value * unit;
                value = 0;
                digits = false;
            }
            if (digits) return std::nullopt;
            return std::chrono::seconds(total);
        }

        /// Return @p xml with the Mode text inside the first @p element replaced.
        std::optional<std::string>
        with_mode(const std::string& xml, std::string_view element, const std::string& mode)
        {
            const auto parent = detail_xml::find_element(xml, element);
            if (!parent) return std::nullopt;
            const auto body = detail_xml::inner(xml, *parent);
            const auto target = detail_xml::find_element(body, "Mode");
            if (!target) return std::nullopt;
            const auto begin = parent->content_begin + target->content_begin;
            const auto end = parent->content_begin + target->content_end;
            return xml.substr(0, begin) + xml_escape(mode) + xml.substr(end);
        }

        PresetTourState tour_state(std::string_view text)
        {
            if (text == "Idle") return PresetTourState::idle;
            if (text == "Touring") return PresetTourState::touring;
            if (text == "Paused") return PresetTourState::paused;
            return PresetTourState::unknown;
        }

        /// Which kind of endpoint an operation addresses, for host authorization.
        EndpointPurpose endpoint_purpose(OperationId operation)
        {
            switch (operation)
            {
                case OperationId::get_profiles:
                case OperationId::get_stream_uri:
                case OperationId::get_snapshot_uri: return EndpointPurpose::media;
                case OperationId::get_analytics_modules:
                case OperationId::modify_analytics_modules:
                case OperationId::get_analytics_rules:
                case OperationId::modify_analytics_rules: return EndpointPurpose::analytics;
                case OperationId::get_video_sources:
                case OperationId::get_video_encoder_options:
                case OperationId::set_video_encoder_configuration: return EndpointPurpose::media;
                case OperationId::set_preset:
                case OperationId::remove_preset:
                case OperationId::get_preset_tours:
                case OperationId::create_preset_tour:
                case OperationId::modify_preset_tour:
                case OperationId::operate_preset_tour:
                case OperationId::remove_preset_tour:
                case OperationId::get_ptz_configuration:
                case OperationId::set_ptz_configuration: return EndpointPurpose::ptz;
                case OperationId::get_imaging_settings:
                case OperationId::set_imaging_settings:
                case OperationId::get_imaging_options: return EndpointPurpose::imaging;
                case OperationId::custom: return EndpointPurpose::device;
                default: break;
            }
            return operation >= OperationId::ptz_status && operation <= OperationId::goto_preset
                       ? EndpointPurpose::ptz
                       : EndpointPurpose::events;
        }

        class NetworkServiceBackend final : public ServiceBackend
        {
          public:
            NetworkServiceBackend(
                std::string device_url,
                std::shared_ptr<HttpTransport> transport,
                std::shared_ptr<EndpointPolicy> policy,
                std::shared_ptr<Runtime> runtime,
                std::shared_ptr<CredentialProvider> credential_provider,
                CancellationToken session_cancellation,
                std::shared_ptr<detail::DeviceClock> clock
            )
                : device_url_(std::move(device_url)), transport_(std::move(transport)),
                  policy_(std::move(policy)), runtime_(std::move(runtime)),
                  credential_provider_(std::move(credential_provider)),
                  session_cancellation_(std::move(session_cancellation)),
                  clock_(clock ? std::move(clock) : std::make_shared<detail::DeviceClock>())
            {
            }

            Result<std::vector<MediaProfile>> profiles() override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), OperationId::get_profiles, {}, "GetProfilesResponse"
                );
                if (!response) return response.error();
                std::vector<MediaProfile> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "Profiles",
                    [&](const detail_xml::Element& profile)
                    {
                        const auto token =
                            attribute(detail_xml::attributes(xml, profile), "token");
                        const auto body = std::string(detail_xml::inner(xml, profile));
                        MediaProfile decoded{token, first_element(body, "Name"), {}};
                        if (const auto analytics =
                                detail_xml::find_element(body, "VideoAnalyticsConfiguration"))
                        {
                            decoded.video_analytics_configuration =
                                attribute(detail_xml::attributes(body, *analytics), "token");
                        }
                        output.push_back(std::move(decoded));
                        const auto configuration =
                            detail_xml::find_element(body, "PTZConfiguration");
                        if (configuration)
                        {
                            std::lock_guard lock(profile_mutex_);
                            ptz_configurations_[token] =
                                attribute(detail_xml::attributes(body, *configuration), "token");
                        }
                    }
                );
                return output;
            }

            Result<MediaUri> stream_uri(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                const auto body =
                    "<trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                    "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
                    "</trt:StreamSetup><trt:ProfileToken>" +
                    xml_escape(profile) + "</trt:ProfileToken>";
                auto response = invoke(
                    endpoint.value(), OperationId::get_stream_uri, body, "GetStreamUriResponse"
                );
                if (!response) return response.error();
                return media_uri(response.value());
            }

            Result<MediaUri> snapshot_uri(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::get_snapshot_uri,
                    "<trt:ProfileToken>" + xml_escape(profile) + "</trt:ProfileToken>",
                    "GetSnapshotUriResponse"
                );
                if (!response) return response.error();
                return media_uri(response.value());
            }

            Result<PtzSpaces> ptz_spaces(const ProfileToken& profile) override
            {
                {
                    std::lock_guard lock(profile_mutex_);
                    const auto cached = ptz_spaces_.find(profile);
                    if (cached != ptz_spaces_.end()) return cached->second;
                }
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                std::string configuration;
                {
                    std::lock_guard lock(profile_mutex_);
                    const auto found = ptz_configurations_.find(profile);
                    if (found != ptz_configurations_.end()) configuration = found->second;
                }
                if (configuration.empty())
                {
                    auto loaded = profiles();
                    if (!loaded) return loaded.error();
                    std::lock_guard lock(profile_mutex_);
                    const auto found = ptz_configurations_.find(profile);
                    if (found != ptz_configurations_.end()) configuration = found->second;
                }
                if (configuration.empty())
                {
                    return Error{
                        ErrorCategory::unsupported,
                        OperationId::get_ptz_configuration_options,
                        "profile has no PTZ configuration token"
                    };
                }
                auto response = invoke(
                    endpoint.value(),
                    OperationId::get_ptz_configuration_options,
                    "<tptz:ConfigurationToken>" + xml_escape(configuration) +
                        "</tptz:ConfigurationToken>",
                    "GetConfigurationOptionsResponse"
                );
                if (!response) return response.error();
                PtzSpaces output;
                const auto spaces = subtree(response.value(), "Spaces");
                const auto parse_2d = [&](std::string_view name, auto& destination)
                {
                    detail_xml::for_each_element(
                        spaces,
                        name,
                        [&](const detail_xml::Element& element)
                        {
                            const auto body = std::string(detail_xml::inner(spaces, element));
                            const auto x = first_range(body, "XRange");
                            const auto y = first_range(body, "YRange");
                            if (x && y)
                                destination.push_back({first_element(body, "URI"), *x, *y});
                        }
                    );
                };
                const auto parse_1d = [&](std::string_view name, auto& destination)
                {
                    detail_xml::for_each_element(
                        spaces,
                        name,
                        [&](const detail_xml::Element& element)
                        {
                            const auto body = std::string(detail_xml::inner(spaces, element));
                            const auto x = first_range(body, "XRange");
                            if (x) destination.push_back({first_element(body, "URI"), *x});
                        }
                    );
                };
                parse_2d("AbsolutePanTiltPositionSpace", output.absolute_pan_tilt);
                parse_1d("AbsoluteZoomPositionSpace", output.absolute_zoom);
                parse_2d("RelativePanTiltTranslationSpace", output.relative_pan_tilt);
                parse_1d("RelativeZoomTranslationSpace", output.relative_zoom);
                parse_2d("ContinuousPanTiltVelocitySpace", output.continuous_pan_tilt);
                parse_1d("ContinuousZoomVelocitySpace", output.continuous_zoom);
                const auto& pan_tilt = !output.continuous_pan_tilt.empty()
                                           ? output.continuous_pan_tilt.front()
                                           : !output.absolute_pan_tilt.empty()
                                                 ? output.absolute_pan_tilt.front()
                                                 : !output.relative_pan_tilt.empty()
                                                       ? output.relative_pan_tilt.front()
                                                       : PtzSpace2D{};
                const auto& zoom = !output.continuous_zoom.empty()
                                       ? output.continuous_zoom.front()
                                       : !output.absolute_zoom.empty()
                                             ? output.absolute_zoom.front()
                                             : !output.relative_zoom.empty()
                                                   ? output.relative_zoom.front()
                                                   : PtzSpace1D{};
                output.pan = pan_tilt.x;
                output.tilt = pan_tilt.y;
                output.zoom = zoom.x;
                {
                    std::lock_guard lock(profile_mutex_);
                    ptz_spaces_.insert_or_assign(profile, output);
                }
                return output;
            }

            Result<PtzStatus> ptz_status(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::ptz_status,
                    "<tptz:ProfileToken>" + xml_escape(profile) + "</tptz:ProfileToken>",
                    "GetStatusResponse"
                );
                if (!response) return response.error();
                PtzStatus status;
                auto ranges = ptz_spaces(profile);
                if (!ranges) return ranges.error();
                const auto position_pan_tilt = ranges.value().absolute_pan_tilt.empty()
                                                   ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                   : ranges.value().absolute_pan_tilt.front();
                const auto position_zoom = ranges.value().absolute_zoom.empty()
                                               ? PtzSpace1D{"", ranges.value().zoom}
                                               : ranges.value().absolute_zoom.front();
                bool position_seen = false;
                if (auto value = numeric_attribute(response.value(), "PanTilt", "x"))
                {
                    position_seen = true;
                    auto normalized = normalize_ptz(*value, position_pan_tilt.x);
                    if (normalized) status.position.pan = normalized.value();
                    else status.position_reliable = false;
                }
                if (auto value = numeric_attribute(response.value(), "PanTilt", "y"))
                {
                    position_seen = true;
                    auto normalized = normalize_ptz(*value, position_pan_tilt.y);
                    if (normalized) status.position.tilt = normalized.value();
                    else status.position_reliable = false;
                }
                if (auto value = numeric_attribute(response.value(), "Zoom", "x"))
                {
                    position_seen = true;
                    auto normalized = normalize_ptz(*value, position_zoom.x);
                    if (normalized) status.position.zoom = normalized.value();
                    else status.position_reliable = false;
                }
                const auto move = subtree(response.value(), "MoveStatus");
                const auto pan_tilt = first_element(move, "PanTilt");
                const auto zoom = first_element(move, "Zoom");
                if (!pan_tilt.empty()) status.pan_tilt_moving = pan_tilt == "MOVING";
                if (!zoom.empty()) status.zoom_moving = zoom == "MOVING";
                status.moving = status.pan_tilt_moving.value_or(false) ||
                                status.zoom_moving.value_or(false) ||
                                first_element(response.value(), "MoveStatus").find("MOVING") !=
                                    std::string::npos;
                if (!position_seen) status.position_reliable = false;
                return status;
            }

            Result<MoveResult>
            move(const ProfileToken& profile, PtzVector vector, bool relative) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto ranges = ptz_spaces(profile);
                if (!ranges) return ranges.error();
                const auto pan_tilt_space = relative
                                                ? (ranges.value().relative_pan_tilt.empty()
                                                       ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                       : ranges.value().relative_pan_tilt.front())
                                                : (ranges.value().continuous_pan_tilt.empty()
                                                       ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                       : ranges.value().continuous_pan_tilt.front());
                const auto zoom_space = relative
                                            ? (ranges.value().relative_zoom.empty()
                                                   ? PtzSpace1D{"", ranges.value().zoom}
                                                   : ranges.value().relative_zoom.front())
                                            : (ranges.value().continuous_zoom.empty()
                                                   ? PtzSpace1D{"", ranges.value().zoom}
                                                   : ranges.value().continuous_zoom.front());
                const auto native = [](std::optional<double> value, NativeRange range)
                {
                    if (!value) return value;
                    auto converted = denormalize_ptz(*value, range);
                    return converted ? std::optional<double>(converted.value()) : std::nullopt;
                };
                vector.pan = native(vector.pan, pan_tilt_space.x);
                vector.tilt = native(vector.tilt, pan_tilt_space.y);
                vector.zoom = native(vector.zoom, zoom_space.x);
                // Stream formatting would emit the global locale's decimal separator
                // and truncate to six significant digits; XML doubles are neither.
                const auto& number = detail_xml::format_double;
                std::string body;
                body += "<tptz:ProfileToken>" + xml_escape(profile) + "</tptz:ProfileToken>";
                body += relative ? "<tptz:Translation>" : "<tptz:Velocity>";
                if (vector.pan || vector.tilt)
                {
                    body += "<tt:PanTilt";
                    if (!pan_tilt_space.uri.empty())
                        body += " space=\"" + xml_escape(pan_tilt_space.uri) + '"';
                    if (vector.pan) body += " x=\"" + number(*vector.pan) + '"';
                    if (vector.tilt) body += " y=\"" + number(*vector.tilt) + '"';
                    body += "/>";
                }
                if (vector.zoom)
                {
                    body += "<tt:Zoom";
                    if (!zoom_space.uri.empty())
                        body += " space=\"" + xml_escape(zoom_space.uri) + '"';
                    body += " x=\"" + number(*vector.zoom) + "\"/>";
                }
                body += relative ? "</tptz:Translation>" : "</tptz:Velocity>";
                const auto operation =
                    relative ? OperationId::relative_move : OperationId::continuous_move;
                auto response = invoke(
                    endpoint.value(),
                    operation,
                    body,
                    relative ? "RelativeMoveResponse" : "ContinuousMoveResponse"
                );
                if (!response)
                {
                    return response.error();
                }
                return MoveResult{false, std::nullopt, std::nullopt};
            }

            Result<MoveResult> absolute_move(
                const ProfileToken& profile,
                PtzVector position,
                std::optional<PtzVector> speed
            ) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto ranges = ptz_spaces(profile);
                if (!ranges) return ranges.error();
                if (ranges.value().absolute_pan_tilt.empty() &&
                    ranges.value().absolute_zoom.empty())
                {
                    return Error{
                        ErrorCategory::unsupported,
                        OperationId::absolute_move,
                        "device advertises no absolute PTZ position space"
                    };
                }
                const auto pan_tilt = ranges.value().absolute_pan_tilt.empty()
                                          ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                          : ranges.value().absolute_pan_tilt.front();
                const auto zoom = ranges.value().absolute_zoom.empty()
                                      ? PtzSpace1D{"", ranges.value().zoom}
                                      : ranges.value().absolute_zoom.front();
                const auto velocity_pan_tilt = ranges.value().continuous_pan_tilt.empty()
                                                   ? PtzSpace2D{"", ranges.value().pan, ranges.value().tilt}
                                                   : ranges.value().continuous_pan_tilt.front();
                const auto velocity_zoom = ranges.value().continuous_zoom.empty()
                                               ? PtzSpace1D{"", ranges.value().zoom}
                                               : ranges.value().continuous_zoom.front();
                const auto native = [](std::optional<double> value, NativeRange range)
                {
                    if (!value) return value;
                    auto converted = denormalize_ptz(*value, range);
                    return converted ? std::optional<double>(converted.value()) : std::nullopt;
                };
                position.pan = native(position.pan, pan_tilt.x);
                position.tilt = native(position.tilt, pan_tilt.y);
                position.zoom = native(position.zoom, zoom.x);
                if (speed)
                {
                    speed->pan = native(speed->pan, velocity_pan_tilt.x);
                    speed->tilt = native(speed->tilt, velocity_pan_tilt.y);
                    speed->zoom = native(speed->zoom, velocity_zoom.x);
                }
                const auto append = [&](std::string& body, const PtzVector& vector, bool velocity)
                {
                    const auto& number = detail_xml::format_double;
                    if (vector.pan || vector.tilt)
                    {
                        body += "<tt:PanTilt";
                        const auto& uri = velocity ? velocity_pan_tilt.uri : pan_tilt.uri;
                        if (!uri.empty()) body += " space=\"" + xml_escape(uri) + '"';
                        if (vector.pan) body += " x=\"" + number(*vector.pan) + '"';
                        if (vector.tilt) body += " y=\"" + number(*vector.tilt) + '"';
                        body += "/>";
                    }
                    if (vector.zoom)
                    {
                        body += "<tt:Zoom";
                        const auto& uri = velocity ? velocity_zoom.uri : zoom.uri;
                        if (!uri.empty()) body += " space=\"" + xml_escape(uri) + '"';
                        body += " x=\"" + number(*vector.zoom) + "\"/>";
                    }
                };
                std::string body = "<tptz:ProfileToken>" + xml_escape(profile) +
                                   "</tptz:ProfileToken><tptz:Position>";
                append(body, position, false);
                body += "</tptz:Position>";
                if (speed)
                {
                    body += "<tptz:Speed>";
                    append(body, *speed, true);
                    body += "</tptz:Speed>";
                }
                auto response = invoke(
                    endpoint.value(), OperationId::absolute_move, body, "AbsoluteMoveResponse"
                );
                if (!response)
                {
                    return response.error();
                }
                return MoveResult{false, std::nullopt, std::nullopt};
            }

            Result<void> stop(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::ptz_stop,
                    "<tptz:ProfileToken>" + xml_escape(profile) +
                        "</tptz:ProfileToken><tptz:PanTilt>true</tptz:PanTilt>"
                        "<tptz:Zoom>true</tptz:Zoom>",
                    "StopResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<std::vector<Preset>> presets(const ProfileToken& profile) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::get_presets,
                    "<tptz:ProfileToken>" + xml_escape(profile) + "</tptz:ProfileToken>",
                    "GetPresetsResponse"
                );
                if (!response) return response.error();
                // Preset positions are advertised in the device's native units, but
                // PtzVector is defined as normalized [-1, +1]. Publishing the raw
                // values would make a preset position unusable as a move argument.
                std::optional<PtzSpaces> ranges;
                if (auto spaces = ptz_spaces(profile)) ranges = spaces.value();
                const auto normalized =
                    [&](std::optional<double> value, NativeRange range) -> std::optional<double>
                {
                    if (!ranges || !value) return std::nullopt;
                    auto converted = normalize_ptz(*value, range);
                    return converted ? std::optional<double>(converted.value()) : std::nullopt;
                };
                std::vector<Preset> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "Preset",
                    [&](const detail_xml::Element& element)
                    {
                        Preset preset;
                        preset.token = attribute(detail_xml::attributes(xml, element), "token");
                        const auto body = std::string(detail_xml::inner(xml, element));
                        preset.name = first_element(body, "Name");
                        // Presets are absolute positions, so they are normalized
                        // against the absolute position space, exactly as
                        // ptz_status() does, not the aggregate (velocity-first)
                        // range.
                        const auto pan_tilt = !ranges ? NativeRange{}
                            : ranges->absolute_pan_tilt.empty() ? ranges->pan
                            : ranges->absolute_pan_tilt.front().x;
                        const auto tilt = !ranges ? NativeRange{}
                            : ranges->absolute_pan_tilt.empty() ? ranges->tilt
                            : ranges->absolute_pan_tilt.front().y;
                        const auto zoom = !ranges ? NativeRange{}
                            : ranges->absolute_zoom.empty() ? ranges->zoom
                            : ranges->absolute_zoom.front().x;
                        preset.position.pan =
                            normalized(numeric_attribute(body, "PanTilt", "x"), pan_tilt);
                        preset.position.tilt =
                            normalized(numeric_attribute(body, "PanTilt", "y"), tilt);
                        preset.position.zoom =
                            normalized(numeric_attribute(body, "Zoom", "x"), zoom);
                        output.push_back(std::move(preset));
                    }
                );
                return output;
            }

            Result<void>
            goto_preset(const ProfileToken& profile, const PresetToken& preset) override
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(),
                    OperationId::goto_preset,
                    "<tptz:ProfileToken>" + xml_escape(profile) +
                        "</tptz:ProfileToken><tptz:PresetToken>" + xml_escape(preset) +
                        "</tptz:PresetToken>",
                    "GotoPresetResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<PullPointSubscriptionInfo> create_event_subscription(
                std::chrono::seconds requested_lifetime
            ) override
            {
                return create_filtered_event_subscription(requested_lifetime, {});
            }

            Result<PullPointSubscriptionInfo> create_filtered_event_subscription(
                std::chrono::seconds requested_lifetime, const std::vector<std::string>& topics
            ) override
            {
                auto endpoint = service_endpoint(Service::events);
                if (!endpoint) return endpoint.error();
                std::string filter;
                if (!topics.empty())
                {
                    std::string expression;
                    for (const auto& topic : topics)
                        expression += (expression.empty() ? "" : "|") + topic;
                    filter = "<tev:Filter><wsnt:TopicExpression Dialect=\"http://www.onvif.org/"
                             "ver10/tev/topicExpression/ConcreteSet\">" +
                             xml_escape(expression) + "</wsnt:TopicExpression></tev:Filter>";
                }
                auto subscribed = invoke(
                    endpoint.value(),
                    OperationId::event_subscribe,
                    filter + "<tev:InitialTerminationTime>PT" +
                        std::to_string(requested_lifetime.count()) +
                        "S</tev:InitialTerminationTime>",
                    "CreatePullPointSubscriptionResponse"
                );
                if (!subscribed) return subscribed.error();
                const auto reference = subtree(subscribed.value(), "SubscriptionReference");
                auto address = first_element(reference, "Address");
                if (address.empty())
                {
                    return Error{
                        ErrorCategory::protocol_violation,
                        OperationId::event_subscribe,
                        "subscription endpoint missing"
                    };
                }
                auto authorized = policy_->authorize(address, device_url_);
                if (!authorized) return authorized.error();
                auto termination = parse_xsd_datetime(
                    first_element(subscribed.value(), "TerminationTime")
                ).value_or(std::chrono::system_clock::now() + requested_lifetime);
                std::lock_guard lock(event_mutex_);
                auto token = std::to_string(next_subscription_++);
                PullPointSubscriptionInfo info{token, std::move(address), termination};
                subscriptions_.emplace(token, info);
                return info;
            }

            Result<std::vector<Event>> pull_event_subscription(
                const std::string& token,
                std::chrono::milliseconds server_timeout,
                std::size_t maximum
            ) override
            {
                // The subscription table lock is held only to copy the endpoint. A
                // PullMessages long-poll can last the full server timeout, and holding
                // the lock across it would block renew and Unsubscribe for as long.
                const auto endpoint = subscription_endpoint(token);
                if (!endpoint)
                    return Error{ErrorCategory::closed_handle, OperationId::event_pull,
                                 "subscription is closed"};
                const auto seconds = std::max<std::int64_t>(
                    1, (server_timeout.count() + 999) / 1000
                );
                auto response = invoke(
                    *endpoint,
                    OperationId::event_pull,
                    "<tev:Timeout>PT" + std::to_string(seconds) +
                        "S</tev:Timeout><tev:MessageLimit>" +
                        std::to_string(maximum) + "</tev:MessageLimit>",
                    "PullMessagesResponse"
                );
                if (!response) return response.error();
                std::vector<Event> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "NotificationMessage",
                    [&](const detail_xml::Element& element)
                    {
                        Event event;
                        const auto body = std::string(detail_xml::inner(xml, element));
                        event.topic = first_element(body, "Topic");
                        // ONVIF puts SimpleItems under both Source and Data. Taking
                        // the first item as the source and lumping the rest into data
                        // misfiles every message with more than one source item.
                        const auto message = detail_xml::find_element(body, "Message");
                        const auto payload = message
                                                 ? std::string(detail_xml::inner(body, *message))
                                                 : body;
                        const auto source = detail_xml::find_element(payload, "Source");
                        const auto data = detail_xml::find_element(payload, "Data");
                        if (source || data)
                        {
                            if (source)
                            {
                                event.source =
                                    simple_items(detail_xml::inner(payload, *source));
                            }
                            if (data)
                            {
                                event.data = simple_items(detail_xml::inner(payload, *data));
                            }
                        }
                        else
                        {
                            event.data = simple_items(payload);
                        }
                        // Message carries the device's own UtcTime; falling back to
                        // the local clock only when the device omits or mangles it.
                        std::string stamp;
                        if (message)
                        {
                            const auto inner_message =
                                detail_xml::find_element(payload, "Message");
                            stamp = attribute(
                                inner_message
                                    ? detail_xml::attributes(payload, *inner_message)
                                    : detail_xml::attributes(body, *message),
                                "UtcTime"
                            );
                        }
                        const auto parsed = parse_xsd_datetime(stamp);
                        event.utc_time =
                            parsed ? *parsed : std::chrono::system_clock::now();
                        event.sequence = next_sequence_.fetch_add(1);
                        output.push_back(std::move(event));
                    }
                );
                return output;
            }

            Result<PullPointSubscriptionInfo> renew_event_subscription(
                const std::string& token, std::chrono::seconds requested_lifetime
            ) override
            {
                const auto endpoint = subscription_endpoint(token);
                if (!endpoint)
                    return Error{ErrorCategory::closed_handle, OperationId::event_renew,
                                 "subscription is closed"};
                auto response = invoke(
                    *endpoint,
                    OperationId::event_renew,
                    "<wsnt:TerminationTime>PT" +
                        std::to_string(requested_lifetime.count()) +
                        "S</wsnt:TerminationTime>",
                    "RenewResponse"
                );
                if (!response) return response.error();
                const auto termination = parse_xsd_datetime(
                    first_element(response.value(), "TerminationTime")
                ).value_or(std::chrono::system_clock::now() + requested_lifetime);
                std::lock_guard lock(event_mutex_);
                auto subscription = subscriptions_.find(token);
                if (subscription == subscriptions_.end())
                    return Error{ErrorCategory::closed_handle, OperationId::event_renew,
                                 "subscription is closed"};
                subscription->second.termination_time = termination;
                return subscription->second;
            }

            Result<void> close_event_subscription(const std::string& token) override
            {
                const auto endpoint = subscription_endpoint(token);
                if (!endpoint) return {};
                auto response = invoke(
                    *endpoint,
                    OperationId::event_close,
                    {},
                    "UnsubscribeResponse"
                );
                // The local record is kept only while a retry could still succeed:
                // after a transport failure the device never saw the request. Once
                // the device has answered, even with a fault, the subscription is
                // gone or unusable there, and keeping the record would leak it.
                const bool device_answered = response ||
                    (response.error().category != ErrorCategory::network &&
                     response.error().category != ErrorCategory::timeout &&
                     response.error().category != ErrorCategory::cancelled &&
                     response.error().category != ErrorCategory::tls);
                if (device_answered)
                {
                    std::lock_guard lock(event_mutex_);
                    subscriptions_.erase(token);
                }
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<std::vector<Event>> pull_events(std::size_t maximum) override
            {
                std::lock_guard legacy_lock(legacy_mutex_);
                if (legacy_subscription_.empty())
                {
                    auto created = create_event_subscription(std::chrono::seconds(60));
                    if (!created) return created.error();
                    legacy_subscription_ = created.value().token;
                }
                auto pulled = pull_event_subscription(
                    legacy_subscription_, std::chrono::seconds(5), maximum
                );
                if (!pulled) legacy_subscription_.clear();
                return pulled;
            }

            Result<void> renew_events() override
            {
                std::lock_guard legacy_lock(legacy_mutex_);
                if (legacy_subscription_.empty()) return {};
                auto renewed = renew_event_subscription(
                    legacy_subscription_, std::chrono::seconds(60)
                );
                if (!renewed) legacy_subscription_.clear();
                return renewed ? Result<void>{} : Result<void>{renewed.error()};
            }

            Result<void> close_events() override
            {
                std::lock_guard legacy_lock(legacy_mutex_);
                if (legacy_subscription_.empty()) return {};
                const auto token = std::exchange(legacy_subscription_, {});
                return close_event_subscription(token);
            }

            Result<std::vector<EventTopic>> event_topics() override
            {
                auto endpoint = service_endpoint(Service::events);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), OperationId::get_event_properties, {},
                    "GetEventPropertiesResponse"
                );
                if (!response) return response.error();
                return parse_topic_set(response.value());
            }

            Result<std::vector<AnalyticsConfig>> analytics_modules(const std::string& configuration) override
            {
                return analytics_query(
                    configuration, OperationId::get_analytics_modules,
                    "GetAnalyticsModulesResponse", "AnalyticsModule"
                );
            }

            Result<void> modify_analytics_modules(
                const std::string& configuration, const std::vector<AnalyticsConfig>& modules
            ) override
            {
                return analytics_change(
                    OperationId::modify_analytics_modules,
                    analytics_body(configuration, modules, "AnalyticsModule"),
                    "ModifyAnalyticsModulesResponse"
                );
            }

            Result<std::vector<AnalyticsConfig>> analytics_rules(const std::string& configuration) override
            {
                return analytics_query(
                    configuration, OperationId::get_analytics_rules, "GetRulesResponse", "Rule"
                );
            }

            Result<void> modify_analytics_rules(
                const std::string& configuration, const std::vector<AnalyticsConfig>& rules
            ) override
            {
                return analytics_change(
                    OperationId::modify_analytics_rules,
                    analytics_body(configuration, rules, "Rule"),
                    "ModifyRulesResponse"
                );
            }

            Result<std::vector<VideoSource>> video_sources() override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), OperationId::get_video_sources, {}, "GetVideoSourcesResponse"
                );
                if (!response) return response.error();
                std::vector<VideoSource> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "VideoSources",
                    [&](const detail_xml::Element& element)
                    {
                        const auto body = std::string(detail_xml::inner(xml, element));
                        VideoSource source;
                        source.token = attribute(detail_xml::attributes(xml, element), "token");
                        source.framerate =
                            detail_xml::parse_double(first_element(body, "Framerate")).value_or(0);
                        const auto resolution = subtree(body, "Resolution");
                        source.width = static_cast<int>(
                            detail_xml::parse_double(first_element(resolution, "Width")).value_or(0));
                        source.height = static_cast<int>(
                            detail_xml::parse_double(first_element(resolution, "Height")).value_or(0));
                        output.push_back(std::move(source));
                    }
                );
                return output;
            }

            Result<ImagingSettings> imaging_settings(const std::string& source) override
            {
                auto response = imaging_call(
                    OperationId::get_imaging_settings, source, {}, "GetImagingSettingsResponse"
                );
                if (!response) return response.error();
                const std::string& xml = response.value();
                const auto number = [&](std::string_view name)
                {
                    const auto element = detail_xml::find_element(xml, name);
                    return element ? detail_xml::parse_double(detail_xml::text(xml, *element))
                                   : std::nullopt;
                };
                ImagingSettings settings;
                settings.brightness = number("Brightness");
                settings.color_saturation = number("ColorSaturation");
                settings.contrast = number("Contrast");
                settings.sharpness = number("Sharpness");
                if (detail_xml::find_element(xml, "IrCutFilter"))
                    settings.ir_cut_filter = first_element(xml, "IrCutFilter");
                settings.backlight_compensation = imaging_mode(xml, "BacklightCompensation");
                settings.wide_dynamic_range = imaging_mode(xml, "WideDynamicRange");
                settings.white_balance = imaging_mode(xml, "WhiteBalance");
                return settings;
            }

            Result<void> set_imaging_settings(
                const std::string& source, const ImagingSettings& settings, bool persist
            ) override
            {
                auto response = imaging_call(
                    OperationId::set_imaging_settings, source,
                    "<timg:ImagingSettings>" + imaging_settings_xml(settings) +
                        "</timg:ImagingSettings><timg:ForcePersistence>" +
                        (persist ? "true" : "false") + "</timg:ForcePersistence>",
                    "SetImagingSettingsResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<ImagingOptions> imaging_options(const std::string& source) override
            {
                auto response =
                    imaging_call(OperationId::get_imaging_options, source, {}, "GetOptionsResponse");
                if (!response) return response.error();
                const std::string& xml = response.value();
                ImagingOptions options;
                options.brightness = imaging_range(xml, "Brightness");
                options.color_saturation = imaging_range(xml, "ColorSaturation");
                options.contrast = imaging_range(xml, "Contrast");
                options.sharpness = imaging_range(xml, "Sharpness");
                detail_xml::for_each_element(
                    xml, "IrCutFilterModes",
                    [&](const detail_xml::Element& mode)
                    { options.ir_cut_filter_modes.push_back(detail_xml::text(xml, mode)); }
                );
                options.backlight_compensation_modes = child_texts(xml, "BacklightCompensation", "Mode");
                options.wide_dynamic_range_modes = child_texts(xml, "WideDynamicRange", "Mode");
                options.white_balance_modes = child_texts(xml, "WhiteBalance", "Mode");
                return options;
            }

            Result<std::vector<ProfileConfiguration>> profile_configurations() override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), OperationId::get_profiles, {}, "GetProfilesResponse"
                );
                if (!response) return response.error();
                std::vector<ProfileConfiguration> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "Profiles",
                    [&](const detail_xml::Element& profile)
                    {
                        output.push_back(profile_configuration(
                            detail_xml::attributes(xml, profile), detail_xml::inner(xml, profile)
                        ));
                        if (!output.back().ptz_configuration.empty())
                        {
                            std::lock_guard lock(profile_mutex_);
                            ptz_configurations_[output.back().token] = output.back().ptz_configuration;
                        }
                    }
                );
                return output;
            }

            Result<VideoEncoderOptions> video_encoder_options(const std::string& configuration) override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), OperationId::get_video_encoder_options,
                    "<trt:ConfigurationToken>" + xml_escape(configuration) + "</trt:ConfigurationToken>",
                    "GetVideoEncoderConfigurationOptionsResponse"
                );
                if (!response) return response.error();
                return video_encoder_options_from(response.value());
            }

            Result<void> set_video_encoder_configuration(
                const VideoEncoderConfiguration& configuration, bool persist
            ) override
            {
                auto endpoint = service_endpoint(Service::media);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), OperationId::set_video_encoder_configuration,
                    "<trt:Configuration token=\"" + xml_escape(configuration.token) + "\">" +
                        video_encoder_xml(configuration) +
                        "</trt:Configuration><trt:ForcePersistence>" + (persist ? "true" : "false") +
                        "</trt:ForcePersistence>",
                    "SetVideoEncoderConfigurationResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<PresetToken> set_preset(
                const ProfileToken& profile, const std::string& name, const PresetToken& token
            ) override
            {
                std::string body = profile_element(profile);
                if (!name.empty()) body += "<tptz:PresetName>" + xml_escape(name) + "</tptz:PresetName>";
                if (!token.empty()) body += "<tptz:PresetToken>" + xml_escape(token) + "</tptz:PresetToken>";
                auto response = ptz_call(OperationId::set_preset, std::move(body), "SetPresetResponse");
                if (!response) return response.error();
                auto created = first_element(response.value(), "PresetToken");
                if (created.empty())
                    return Error{ErrorCategory::protocol_violation, OperationId::set_preset,
                                 "device returned no preset token"};
                return created;
            }

            Result<void> remove_preset(const ProfileToken& profile, const PresetToken& token) override
            {
                auto response = ptz_call(
                    OperationId::remove_preset,
                    profile_element(profile) + "<tptz:PresetToken>" + xml_escape(token) + "</tptz:PresetToken>",
                    "RemovePresetResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<std::vector<PresetTour>> preset_tours(const ProfileToken& profile) override
            {
                auto response = ptz_call(
                    OperationId::get_preset_tours, profile_element(profile), "GetPresetToursResponse"
                );
                if (!response) return response.error();
                std::vector<PresetTour> output;
                const std::string& xml = response.value();
                detail_xml::for_each_element(
                    xml,
                    "PresetTour",
                    [&](const detail_xml::Element& element)
                    {
                        const auto body = std::string(detail_xml::inner(xml, element));
                        PresetTour tour;
                        tour.token = attribute(detail_xml::attributes(xml, element), "token");
                        tour.name = first_element(body, "Name");
                        tour.state = tour_state(first_element(subtree(body, "Status"), "State"));
                        tour.auto_start = first_element(body, "AutoStart") == "true";
                        detail_xml::for_each_element(
                            body, "TourSpot",
                            [&](const detail_xml::Element& spot)
                            {
                                const auto content = detail_xml::inner(body, spot);
                                tour.spots.push_back({
                                    detail_xml::first_text(content, "PresetToken"),
                                    parse_duration(detail_xml::first_text(content, "StayTime"))
                                        .value_or(std::chrono::seconds{})
                                });
                            }
                        );
                        output.push_back(std::move(tour));
                    }
                );
                return output;
            }

            Result<std::string> create_preset_tour(const ProfileToken& profile) override
            {
                auto response = ptz_call(
                    OperationId::create_preset_tour, profile_element(profile), "CreatePresetTourResponse"
                );
                if (!response) return response.error();
                auto token = first_element(response.value(), "PresetTourToken");
                if (token.empty())
                    return Error{ErrorCategory::protocol_violation, OperationId::create_preset_tour,
                                 "device returned no tour token"};
                return token;
            }

            Result<void> modify_preset_tour(const ProfileToken& profile, const PresetTour& tour) override
            {
                // Status and StartingCondition are required by the schema; devices
                // ignore the status a client sends.
                std::string body = profile_element(profile) + "<tptz:PresetTour token=\"" +
                                   xml_escape(tour.token) + "\"><tt:Name>" + xml_escape(tour.name) +
                                   "</tt:Name><tt:Status><tt:State>Idle</tt:State></tt:Status>"
                                   "<tt:AutoStart>" + (tour.auto_start ? "true" : "false") +
                                   "</tt:AutoStart><tt:StartingCondition/>";
                for (const auto& spot : tour.spots)
                {
                    body += "<tt:TourSpot><tt:PresetDetail><tt:PresetToken>" + xml_escape(spot.preset) +
                            "</tt:PresetToken></tt:PresetDetail>";
                    if (spot.stay_time.count() > 0)
                        body += "<tt:StayTime>PT" + std::to_string(spot.stay_time.count()) + "S</tt:StayTime>";
                    body += "</tt:TourSpot>";
                }
                body += "</tptz:PresetTour>";
                auto response = ptz_call(OperationId::modify_preset_tour, std::move(body), "ModifyPresetTourResponse");
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<void> operate_preset_tour(
                const ProfileToken& profile, const std::string& token, PresetTourOperation operation
            ) override
            {
                const auto action = operation == PresetTourOperation::start  ? "Start"
                                    : operation == PresetTourOperation::stop ? "Stop"
                                                                             : "Pause";
                auto response = ptz_call(
                    OperationId::operate_preset_tour,
                    profile_element(profile) + "<tptz:PresetTourToken>" + xml_escape(token) +
                        "</tptz:PresetTourToken><tptz:Operation>" + action + "</tptz:Operation>",
                    "OperatePresetTourResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<void> remove_preset_tour(const ProfileToken& profile, const std::string& token) override
            {
                auto response = ptz_call(
                    OperationId::remove_preset_tour,
                    profile_element(profile) + "<tptz:PresetTourToken>" + xml_escape(token) +
                        "</tptz:PresetTourToken>",
                    "RemovePresetTourResponse"
                );
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            Result<PtzConfiguration> ptz_configuration(const ProfileToken& profile) override
            {
                auto token = ptz_configuration_token(profile);
                if (!token) return token.error();
                auto response = configuration_xml(token.value());
                if (!response) return response.error();
                const auto& xml = response.value();
                PtzConfiguration output;
                const auto element = detail_xml::find_element(xml, "PTZConfiguration");
                const auto body = element ? detail_xml::inner(xml, *element) : std::string_view(xml);
                output.token = element ? attribute(detail_xml::attributes(xml, *element), "token")
                                       : token.value();
                output.name = detail_xml::first_text(body, "Name");
                output.node_token = detail_xml::first_text(body, "NodeToken");
                output.default_timeout = parse_duration(detail_xml::first_text(body, "DefaultPTZTimeout"))
                                             .value_or(std::chrono::seconds{});
                output.direction.eflip = imaging_mode(body, "EFlip");
                output.direction.reverse = imaging_mode(body, "Reverse");
                // Options only list the accepted modes; a device without them still
                // reports its configuration.
                auto options = ptz_call(
                    OperationId::get_ptz_configuration_options,
                    "<tptz:ConfigurationToken>" + xml_escape(token.value()) + "</tptz:ConfigurationToken>",
                    "GetConfigurationOptionsResponse"
                );
                if (options)
                {
                    output.eflip_modes = child_texts(options.value(), "EFlip", "Mode");
                    output.reverse_modes = child_texts(options.value(), "Reverse", "Mode");
                }
                return output;
            }

            Result<void> set_ptz_direction(
                const ProfileToken& profile, const PtzDirection& direction, bool persist
            ) override
            {
                constexpr auto operation = OperationId::set_ptz_configuration;
                auto token = ptz_configuration_token(profile);
                if (!token) return token.error();
                auto response = configuration_xml(token.value());
                if (!response) return response.error();
                const auto& xml = response.value();
                const auto element = detail_xml::find_element(xml, "PTZConfiguration");
                if (!element)
                    return Error{ErrorCategory::protocol_violation, operation, "PTZ configuration missing"};
                auto content = std::string(detail_xml::inner(xml, *element));
                for (const auto& [name, mode] : {std::pair{"EFlip", direction.eflip},
                                                 std::pair{"Reverse", direction.reverse}})
                {
                    if (!mode) continue;
                    auto changed = with_mode(content, name, *mode);
                    if (!changed)
                        return Error{ErrorCategory::unsupported, operation,
                                     std::string("device does not report ") + name};
                    content = std::move(*changed);
                }
                // Everything else goes back exactly as the device reported it.
                auto applied = ptz_call(
                    operation,
                    "<tptz:PTZConfiguration" + std::string(detail_xml::attributes(xml, *element)) + '>' +
                        content + "</tptz:PTZConfiguration><tptz:ForcePersistence>" +
                        (persist ? "true" : "false") + "</tptz:ForcePersistence>",
                    "SetConfigurationResponse"
                );
                return applied ? Result<void>{} : Result<void>{applied.error()};
            }

            Result<std::string>
            call(OnvifService service, const std::string& body, const std::string& response_name) override
            {
                Result<std::string> endpoint = device_url_;
                switch (service)
                {
                    case OnvifService::device: break;
                    case OnvifService::media: endpoint = service_endpoint(Service::media); break;
                    case OnvifService::ptz: endpoint = service_endpoint(Service::ptz); break;
                    case OnvifService::events: endpoint = service_endpoint(Service::events); break;
                    case OnvifService::analytics: endpoint = service_endpoint(Service::analytics); break;
                    case OnvifService::imaging: endpoint = service_endpoint(Service::imaging); break;
                }
                if (!endpoint) return endpoint.error();
                return invoke(endpoint.value(), OperationId::custom, body, response_name);
            }

          private:
            enum class Service { media, ptz, events, analytics, imaging };

            static std::string profile_element(const ProfileToken& profile)
            {
                return "<tptz:ProfileToken>" + xml_escape(profile) + "</tptz:ProfileToken>";
            }

            Result<std::string> ptz_call(OperationId operation, std::string body, std::string_view response_name)
            {
                auto endpoint = service_endpoint(Service::ptz);
                if (!endpoint) return endpoint.error();
                return invoke(endpoint.value(), operation, std::move(body), response_name);
            }

            Result<std::string> configuration_xml(const std::string& token)
            {
                return ptz_call(
                    OperationId::get_ptz_configuration,
                    "<tptz:PTZConfigurationToken>" + xml_escape(token) + "</tptz:PTZConfigurationToken>",
                    "GetConfigurationResponse"
                );
            }

            /// Return the PTZ configuration token of @p profile, loading profiles if needed.
            Result<std::string> ptz_configuration_token(const ProfileToken& profile)
            {
                for (int attempt = 0; attempt < 2; ++attempt)
                {
                    {
                        std::lock_guard lock(profile_mutex_);
                        const auto found = ptz_configurations_.find(profile);
                        if (found != ptz_configurations_.end() && !found->second.empty())
                            return found->second;
                    }
                    if (attempt == 0)
                    {
                        auto loaded = profiles();
                        if (!loaded) return loaded.error();
                    }
                }
                return Error{ErrorCategory::unsupported, OperationId::get_ptz_configuration,
                             "profile has no PTZ configuration token"};
            }

            Result<std::string> imaging_call(
                OperationId operation, const std::string& source, std::string body,
                std::string_view response_name
            )
            {
                auto endpoint = service_endpoint(Service::imaging);
                if (!endpoint) return endpoint.error();
                return invoke(
                    endpoint.value(), operation,
                    "<timg:VideoSourceToken>" + xml_escape(source) + "</timg:VideoSourceToken>" +
                        std::move(body),
                    response_name
                );
            }

            Result<std::vector<AnalyticsConfig>> analytics_query(
                const std::string& configuration,
                OperationId operation,
                std::string_view response_name,
                std::string_view element
            )
            {
                auto endpoint = service_endpoint(Service::analytics);
                if (!endpoint) return endpoint.error();
                auto response = invoke(
                    endpoint.value(), operation,
                    "<tan:ConfigurationToken>" + xml_escape(configuration) +
                        "</tan:ConfigurationToken>",
                    response_name
                );
                if (!response) return response.error();
                return analytics_configs(response.value(), element);
            }

            Result<void>
            analytics_change(OperationId operation, std::string body, std::string_view response_name)
            {
                auto endpoint = service_endpoint(Service::analytics);
                if (!endpoint) return endpoint.error();
                auto response = invoke(endpoint.value(), operation, std::move(body), response_name);
                return response ? Result<void>{} : Result<void>{response.error()};
            }

            std::optional<std::string> subscription_endpoint(const std::string& token)
            {
                std::lock_guard lock(event_mutex_);
                const auto subscription = subscriptions_.find(token);
                if (subscription == subscriptions_.end()) return std::nullopt;
                return subscription->second.endpoint;
            }

            Result<std::string> service_endpoint(Service service)
            {
                // GetCapabilities must not run while the lock is held. A safety Stop
                // would otherwise queue behind whatever request is already in flight,
                // for the full duration of that request's timeout. Racing callers may
                // each issue one GetCapabilities; the results are identical.
                bool loaded = false;
                {
                    std::lock_guard lock(capabilities_mutex_);
                    loaded = capabilities_loaded_;
                }
                if (!loaded)
                {
                    auto response = invoke(
                        device_url_,
                        OperationId::get_capabilities,
                        {},
                        "GetCapabilitiesResponse"
                    );
                    if (!response) return response.error();
                    auto media = endpoint_in(response.value(), "Media");
                    auto ptz = endpoint_in(response.value(), "PTZ");
                    auto events = endpoint_in(response.value(), "Events");
                    auto analytics = endpoint_in(response.value(), "Analytics");
                    auto imaging = endpoint_in(response.value(), "Imaging");
                    std::lock_guard lock(capabilities_mutex_);
                    if (!capabilities_loaded_)
                    {
                        media_url_ = std::move(media);
                        ptz_url_ = std::move(ptz);
                        event_url_ = std::move(events);
                        analytics_url_ = std::move(analytics);
                        imaging_url_ = std::move(imaging);
                        capabilities_loaded_ = true;
                    }
                }
                std::unique_lock lock(capabilities_mutex_);
                const auto endpoint = service == Service::media       ? media_url_
                                      : service == Service::ptz       ? ptz_url_
                                      : service == Service::analytics ? analytics_url_
                                      : service == Service::imaging   ? imaging_url_
                                                                      : event_url_;
                lock.unlock();
                if (endpoint.empty())
                {
                    return Error{
                        ErrorCategory::unsupported, {}, "device did not advertise the service"
                    };
                }
                auto authorized = policy_->authorize(endpoint, device_url_);
                return authorized ? Result<std::string>(endpoint)
                                  : Result<std::string>(authorized.error());
            }

            Result<std::string> invoke(
                const std::string& endpoint,
                OperationId operation,
                std::string body,
                std::string_view response_name
            )
            {
                auto authorized = policy_->authorize(endpoint, device_url_);
                if (!authorized) return authorized.error();
                auto call = active_call_options.value_or(CallOptions{
                    std::chrono::steady_clock::now() + std::chrono::seconds(15),
                    CancellationSource{}.token()
                });
                // Closing the session cancels its in-flight work, but a Stop exists to
                // halt motion and is exactly what session close sends. Linking the
                // session token into a Stop would cancel it before it reached the
                // wire, leaving the camera moving after close() reported done.
                if (operation != OperationId::ptz_stop)
                {
                    call.cancellation = CancellationToken::linked(
                        call.cancellation, session_cancellation_
                    );
                }
                std::shared_ptr<CredentialLease> lease;
                if (credential_provider_)
                {
                    auto acquired = wait_for_task(credential_provider_->acquire(call));
                    if (!acquired) { auto error = acquired.error(); error.operation = operation; return error; }
                    lease = std::make_shared<CredentialLease>(std::move(acquired.value()));
                }
                // Signed requests carry a Created timestamp checked against the
                // device clock, so measure it first. A Stop never waits on this.
                if (lease && operation != OperationId::ptz_stop && clock_->needs_measurement())
                {
                    auto measured = wait_for_task(transport_->send(
                        detail::clock_request(device_url_, call.deadline), call.cancellation
                    ));
                    clock_->record(measured, std::chrono::system_clock::now());
                }
                HttpRequest request{
                    endpoint,
                    soap_envelope(operation, body, lease.get(), clock_->offset()),
                    {"Content-Type: application/soap+xml; charset=utf-8"},
                    call.deadline
                };
                request.credential_lease = std::move(lease);
                request.original_url = device_url_;
                request.provenance = EndpointProvenance::device_advertised;
                request.operation = operation;
                const bool movement = operation == OperationId::continuous_move ||
                    operation == OperationId::relative_move ||
                    operation == OperationId::absolute_move ||
                    operation == OperationId::goto_preset;
                // A custom request may change anything, so it is never assumed repeatable.
                request.idempotent = !movement && operation != OperationId::custom;
                request.retry_allowed = false;
                request.purpose = endpoint_purpose(operation);
                auto response = wait_for_task(
                    transport_->send(
                        std::move(request),
                        call.cancellation
                    )
                );
                if (!response)
                {
                    auto error = response.error();
                    if (operation == OperationId::continuous_move ||
                        operation == OperationId::relative_move ||
                        operation == OperationId::absolute_move ||
                        operation == OperationId::goto_preset)
                        error.request_may_have_been_applied = true;
                    return error;
                }
                return detail::soap_response_content(response.value(), response_name, operation);
            }

            static std::string endpoint_in(std::string_view xml, std::string_view service)
            {
                const auto element = detail_xml::find_element(xml, service);
                return element ? first_element(detail_xml::inner(xml, *element), "XAddr")
                               : std::string{};
            }

            static MediaUri media_uri(const std::string& xml)
            {
                MediaUri output;
                output.uri = first_element(xml, "Uri");
                output.invalid_after_connect = first_element(xml, "InvalidAfterConnect") == "true";
                return output;
            }

            std::string device_url_;
            std::shared_ptr<HttpTransport> transport_;
            std::shared_ptr<EndpointPolicy> policy_;
            std::shared_ptr<Runtime> runtime_;
            std::shared_ptr<CredentialProvider> credential_provider_;
            CancellationToken session_cancellation_;
            std::shared_ptr<detail::DeviceClock> clock_;
            std::mutex capabilities_mutex_;
            bool capabilities_loaded_{};
            std::string media_url_;
            std::string ptz_url_;
            std::string event_url_;
            std::string analytics_url_;
            std::string imaging_url_;
            std::mutex event_mutex_;
            std::mutex legacy_mutex_;
            std::map<std::string, PullPointSubscriptionInfo> subscriptions_;
            std::uint64_t next_subscription_{1};
            std::string legacy_subscription_;
            std::atomic_uint64_t next_sequence_{};
            std::mutex profile_mutex_;
            std::map<ProfileToken, std::string> ptz_configurations_;
            std::map<ProfileToken, PtzSpaces> ptz_spaces_;
        };
    } // namespace

    std::shared_ptr<ServiceBackend> make_network_service_backend(
        std::string device_url,
        std::shared_ptr<HttpTransport> transport,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<CredentialProvider> credential_provider,
        CancellationToken session_cancellation,
        std::shared_ptr<detail::DeviceClock> clock
    )
    {
        return std::make_shared<NetworkServiceBackend>(
            std::move(device_url),
            std::move(transport),
            std::move(endpoint_policy),
            std::move(runtime),
            std::move(credential_provider),
            std::move(session_cancellation),
            std::move(clock)
        );
    }
} // namespace wlonvif
