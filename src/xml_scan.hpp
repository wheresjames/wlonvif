#pragma once

/**
 * @file xml_scan.hpp
 * @brief Internal, allocation-light XML scanning helpers shared by the library.
 *
 * SOAP responses are validated by read_soap_operation() before any of this runs.
 * These helpers then locate elements and attributes inside the validated bytes.
 *
 * Two properties matter here:
 *
 * - Element and attribute lookup ignores the namespace prefix. Devices emit
 *   `tds:`, `ns2:`, `SOAP-ENV:`, or no prefix at all for the same element, so
 *   matching a literal prefix silently loses data on conforming devices.
 * - Every scan is a single forward pass. std::regex backtracks recursively and
 *   overflows the stack on the multi-megabyte untrusted documents these limits
 *   permit, so no regular expressions are used on device input.
 */

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace wlonvif::detail_xml
{
    inline bool is_name_char(char value) noexcept
    {
        const auto byte = static_cast<unsigned char>(value);
        return std::isalnum(byte) != 0 || value == '_' || value == '-' || value == '.';
    }

    inline bool is_space(char value) noexcept
    {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    }

    inline bool starts_with_at(
        std::string_view text, std::size_t position, std::string_view literal
    ) noexcept
    {
        return position <= text.size() && text.size() - position >= literal.size() &&
               text.compare(position, literal.size(), literal) == 0;
    }

    /// Append the UTF-8 encoding of @p code_point to @p output.
    inline void append_utf8(std::string& output, unsigned long code_point)
    {
        if (code_point < 0x80u)
        {
            output += static_cast<char>(code_point);
        }
        else if (code_point < 0x800u)
        {
            output += static_cast<char>(0xc0u | (code_point >> 6));
            output += static_cast<char>(0x80u | (code_point & 0x3fu));
        }
        else if (code_point < 0x10000u)
        {
            output += static_cast<char>(0xe0u | (code_point >> 12));
            output += static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu));
            output += static_cast<char>(0x80u | (code_point & 0x3fu));
        }
        else if (code_point <= 0x10ffffu)
        {
            output += static_cast<char>(0xf0u | (code_point >> 18));
            output += static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu));
            output += static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu));
            output += static_cast<char>(0x80u | (code_point & 0x3fu));
        }
    }

    /// Resolve the predefined entities and numeric character references in @p input.
    inline std::string unescape(std::string_view input)
    {
        std::string output;
        output.reserve(input.size());
        for (std::size_t index = 0; index < input.size();)
        {
            if (input[index] != '&')
            {
                output += input[index++];
                continue;
            }
            const auto terminator = input.find(';', index + 1);
            if (terminator == input.npos || terminator - index > 12)
            {
                output += input[index++];
                continue;
            }
            const auto entity = input.substr(index + 1, terminator - index - 1);
            if (entity == "amp") { output += '&'; }
            else if (entity == "lt") { output += '<'; }
            else if (entity == "gt") { output += '>'; }
            else if (entity == "quot") { output += '"'; }
            else if (entity == "apos") { output += '\''; }
            else if (entity.size() > 1 && entity.front() == '#')
            {
                const bool hex = entity[1] == 'x' || entity[1] == 'X';
                const auto digits = entity.substr(hex ? 2 : 1);
                unsigned long code_point = 0;
                const auto parsed = std::from_chars(
                    digits.data(), digits.data() + digits.size(), code_point, hex ? 16 : 10
                );
                if (digits.empty() || parsed.ec != std::errc{} ||
                    parsed.ptr != digits.data() + digits.size())
                {
                    output += input[index++];
                    continue;
                }
                append_utf8(output, code_point);
            }
            else
            {
                output += input[index++];
                continue;
            }
            index = terminator + 1;
        }
        return output;
    }

    /// Escape @p input for use as XML character data or an attribute value.
    inline std::string escape(std::string_view input)
    {
        std::string output;
        output.reserve(input.size());
        for (const char value : input)
        {
            switch (value)
            {
                case '&': output += "&amp;"; break;
                case '<': output += "&lt;"; break;
                case '>': output += "&gt;"; break;
                case '"': output += "&quot;"; break;
                case '\'': output += "&apos;"; break;
                default: output += value;
            }
        }
        return output;
    }

    /// Parse a locale-independent XML `double`, or nullopt when @p text is not one.
    inline std::optional<double> parse_double(std::string_view text)
    {
        std::size_t begin = 0;
        std::size_t end = text.size();
        while (begin < end && is_space(text[begin])) { ++begin; }
        while (end > begin && is_space(text[end - 1])) { --end; }
        if (begin == end) { return std::nullopt; }
        double value = 0;
        const auto parsed = std::from_chars(text.data() + begin, text.data() + end, value);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + end) { return std::nullopt; }
        return value;
    }

    /// Serialize @p value as a locale-independent, round-trippable XML `double`.
    inline std::string format_double(double value)
    {
        std::array<char, 64> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
        if (result.ec != std::errc{}) { return "0"; }
        return std::string(buffer.data(), result.ptr);
    }

    /// Skip a comment, CDATA section, processing instruction, or declaration.
    inline std::size_t skip_construct(std::string_view xml, std::size_t position)
    {
        if (starts_with_at(xml, position, "<!--"))
        {
            const auto end = xml.find("-->", position + 4);
            return end == xml.npos ? xml.npos : end + 3;
        }
        if (starts_with_at(xml, position, "<![CDATA["))
        {
            const auto end = xml.find("]]>", position + 9);
            return end == xml.npos ? xml.npos : end + 3;
        }
        const auto end = xml.find('>', position);
        return end == xml.npos ? xml.npos : end + 1;
    }

    /// One start, end, or empty-element tag.
    struct Tag
    {
        bool closing{};                ///< True for `</name>`.
        bool self_closing{};           ///< True for `<name/>`.
        std::string_view local_name;   ///< Name with any namespace prefix removed.
        std::size_t attributes_begin{}; ///< First byte of the raw attribute text.
        std::size_t attributes_end{};   ///< One past the last byte of the attribute text.
        std::size_t end{};              ///< One past the tag's closing `>`.
    };

    /// Parse the tag starting at @p position, which must address a `<`.
    inline std::optional<Tag> parse_tag(std::string_view xml, std::size_t position)
    {
        std::size_t index = position + 1;
        Tag tag;
        if (index < xml.size() && xml[index] == '/')
        {
            tag.closing = true;
            ++index;
        }
        std::size_t local_begin = index;
        while (index < xml.size() && is_name_char(xml[index])) { ++index; }
        if (index < xml.size() && xml[index] == ':')
        {
            local_begin = ++index;
            while (index < xml.size() && is_name_char(xml[index])) { ++index; }
        }
        if (index == local_begin) { return std::nullopt; }
        tag.local_name = xml.substr(local_begin, index - local_begin);
        tag.attributes_begin = index;
        char quote = '\0';
        while (index < xml.size())
        {
            const char value = xml[index];
            if (quote != '\0')
            {
                if (value == quote) { quote = '\0'; }
            }
            else if (value == '"' || value == '\'') { quote = value; }
            else if (value == '>') { break; }
            ++index;
        }
        if (index >= xml.size()) { return std::nullopt; }
        tag.attributes_end = index;
        if (tag.attributes_end > tag.attributes_begin && xml[tag.attributes_end - 1] == '/')
        {
            tag.self_closing = true;
            --tag.attributes_end;
        }
        tag.end = index + 1;
        return tag;
    }

    /// One located element and the byte ranges of its attributes and content.
    struct Element
    {
        std::size_t begin{};            ///< Index of the start tag's `<`.
        std::size_t attributes_begin{}; ///< First byte of the raw attribute text.
        std::size_t attributes_end{};   ///< One past the last byte of the attribute text.
        std::size_t content_begin{};    ///< First byte of the element's content.
        std::size_t content_end{};      ///< One past the last byte of the content.
        std::size_t end{};              ///< One past the element's end tag.
    };

    /**
     * @brief Locate the first element named @p local_name at or after @p from.
     *
     * The namespace prefix is ignored. Nested elements sharing the same local
     * name are counted so the matching end tag is found rather than the first one.
     */
    inline std::optional<Element>
    find_element(std::string_view xml, std::string_view local_name, std::size_t from = 0)
    {
        if (from > xml.size()) { return std::nullopt; }
        for (std::size_t position = from; position < xml.size();)
        {
            position = xml.find('<', position);
            if (position == xml.npos) { return std::nullopt; }
            if (starts_with_at(xml, position, "<!") || starts_with_at(xml, position, "<?"))
            {
                position = skip_construct(xml, position);
                if (position == xml.npos) { return std::nullopt; }
                continue;
            }
            const auto tag = parse_tag(xml, position);
            if (!tag)
            {
                ++position;
                continue;
            }
            if (tag->closing || tag->local_name != local_name)
            {
                position = tag->end;
                continue;
            }
            Element element;
            element.begin = position;
            element.attributes_begin = tag->attributes_begin;
            element.attributes_end = tag->attributes_end;
            element.content_begin = tag->end;
            if (tag->self_closing)
            {
                element.content_end = tag->end;
                element.end = tag->end;
                return element;
            }
            std::size_t depth = 1;
            for (std::size_t cursor = tag->end; cursor < xml.size();)
            {
                cursor = xml.find('<', cursor);
                if (cursor == xml.npos) { return std::nullopt; }
                if (starts_with_at(xml, cursor, "<!") || starts_with_at(xml, cursor, "<?"))
                {
                    cursor = skip_construct(xml, cursor);
                    if (cursor == xml.npos) { return std::nullopt; }
                    continue;
                }
                const auto nested = parse_tag(xml, cursor);
                if (!nested)
                {
                    ++cursor;
                    continue;
                }
                if (nested->local_name == local_name)
                {
                    if (nested->closing)
                    {
                        if (--depth == 0)
                        {
                            element.content_end = cursor;
                            element.end = nested->end;
                            return element;
                        }
                    }
                    else if (!nested->self_closing)
                    {
                        ++depth;
                    }
                }
                cursor = nested->end;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    /// Return the element's raw inner markup.
    inline std::string_view inner(std::string_view xml, const Element& element) noexcept
    {
        return xml.substr(element.content_begin, element.content_end - element.content_begin);
    }

    /// Return the element's raw attribute text.
    inline std::string_view attributes(std::string_view xml, const Element& element) noexcept
    {
        return xml.substr(
            element.attributes_begin, element.attributes_end - element.attributes_begin
        );
    }

    /// Return the element's character data with markup removed and entities resolved.
    inline std::string text(std::string_view xml, const Element& element)
    {
        std::string output;
        for (std::size_t position = element.content_begin; position < element.content_end;)
        {
            const auto next = xml.find('<', position);
            const auto stop = std::min(next == xml.npos ? element.content_end : next, element.content_end);
            output += unescape(xml.substr(position, stop - position));
            if (stop >= element.content_end) { break; }
            if (starts_with_at(xml, stop, "<![CDATA["))
            {
                const auto end = xml.find("]]>", stop + 9);
                if (end == xml.npos || end > element.content_end) { break; }
                output += xml.substr(stop + 9, end - (stop + 9));
                position = end + 3;
                continue;
            }
            if (starts_with_at(xml, stop, "<!") || starts_with_at(xml, stop, "<?"))
            {
                const auto skipped = skip_construct(xml, stop);
                if (skipped == xml.npos) { break; }
                position = skipped;
                continue;
            }
            const auto tag = parse_tag(xml, stop);
            position = tag ? tag->end : stop + 1;
        }
        return output;
    }

    /// Return the character data of the first element named @p local_name.
    inline std::string
    first_text(std::string_view xml, std::string_view local_name, std::size_t from = 0)
    {
        const auto element = find_element(xml, local_name, from);
        return element ? text(xml, *element) : std::string{};
    }

    /**
     * @brief Return the value of attribute @p name in raw attribute text.
     *
     * Names are matched whole and prefix-insensitively, so a request for `token`
     * does not match `ProfileToken`.
     */
    inline std::string attribute(std::string_view text_of_attributes, std::string_view name)
    {
        std::size_t index = 0;
        while (index < text_of_attributes.size())
        {
            while (index < text_of_attributes.size() && is_space(text_of_attributes[index]))
            {
                ++index;
            }
            std::size_t local_begin = index;
            while (index < text_of_attributes.size() && is_name_char(text_of_attributes[index]))
            {
                ++index;
            }
            if (index < text_of_attributes.size() && text_of_attributes[index] == ':')
            {
                local_begin = ++index;
                while (index < text_of_attributes.size() &&
                       is_name_char(text_of_attributes[index]))
                {
                    ++index;
                }
            }
            if (index == local_begin)
            {
                ++index;
                continue;
            }
            const auto local = text_of_attributes.substr(local_begin, index - local_begin);
            while (index < text_of_attributes.size() && is_space(text_of_attributes[index]))
            {
                ++index;
            }
            if (index >= text_of_attributes.size() || text_of_attributes[index] != '=')
            {
                continue;
            }
            ++index;
            while (index < text_of_attributes.size() && is_space(text_of_attributes[index]))
            {
                ++index;
            }
            if (index >= text_of_attributes.size()) { break; }
            const char quote = text_of_attributes[index];
            if (quote != '"' && quote != '\'') { continue; }
            const std::size_t value_begin = ++index;
            while (index < text_of_attributes.size() && text_of_attributes[index] != quote)
            {
                ++index;
            }
            if (index >= text_of_attributes.size()) { break; }
            const auto value = text_of_attributes.substr(value_begin, index - value_begin);
            ++index;
            if (local == name) { return unescape(value); }
        }
        return {};
    }

    /// Return the named attribute of the first element named @p local_name.
    inline std::optional<double> numeric_attribute(
        std::string_view xml, std::string_view local_name, std::string_view name
    )
    {
        const auto element = find_element(xml, local_name);
        if (!element) { return std::nullopt; }
        const auto value = attribute(attributes(xml, *element), name);
        return value.empty() ? std::nullopt : parse_double(value);
    }

    /// Invoke @p visitor with each element named @p local_name at the scanned level.
    template <class Visitor>
    void for_each_element(std::string_view xml, std::string_view local_name, Visitor visitor)
    {
        std::size_t from = 0;
        while (const auto element = find_element(xml, local_name, from))
        {
            visitor(*element);
            from = element->end > from ? element->end : from + 1;
        }
    }
} // namespace wlonvif::detail_xml
