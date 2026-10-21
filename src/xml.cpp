#include <algorithm>
#include <cctype>
#include <libxml/xmlreader.h>
#include <wlonvif/wlonvif.hpp>

namespace wlonvif
{
    Result<std::string>
    read_soap_operation(std::string_view xml, std::string_view response_name, XmlLimits limits)
    {
        if (xml.size() > limits.bytes)
        {
            return Error{ErrorCategory::malformed_xml, {}, "XML byte limit exceeded"};
        }
        auto* r = xmlReaderForMemory(
            xml.data(),
            static_cast<int>(xml.size()),
            nullptr,
            nullptr,
            XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING
        );
        if (!r)
        {
            return Error{ErrorCategory::malformed_xml, {}, "XML reader creation failed"};
        }
        std::size_t nodes = 0, text = 0;
        bool found = false;
        bool fault = false;
        SoapFaultDetail fault_detail;
        // Fault fields are scoped to the element that opened them. Without the
        // depth, every later text node in the document is appended to whichever
        // field opened last.
        std::string* fault_field = nullptr;
        int fault_field_depth = -1;
        int fault_depth = -1;
        bool fault_finished = false;
        int open_subcodes = 0;
        // Text is collected only between the response element's start and end tags.
        // Without tracking the depth the response opened at, every later text node
        // in the document is appended too, so a sibling element after the response
        // can inject content that reads as though the response produced it.
        bool collecting = false;
        int response_depth = -1;
        std::string result;
        int read_status = 0;
        while ((read_status = xmlTextReaderRead(r)) == 1)
        {
            const int depth = xmlTextReaderDepth(r);
            if (++nodes > limits.nodes || static_cast<std::size_t>(depth) > limits.depth)
            {
                xmlFreeTextReader(r);
                return Error{ErrorCategory::malformed_xml, {}, "XML structural limit exceeded"};
            }
            if (xmlTextReaderNodeType(r) == XML_READER_TYPE_ELEMENT)
            {
                if (static_cast<std::size_t>(xmlTextReaderAttributeCount(r)) >
                    limits.attributes_per_element)
                {
                    xmlFreeTextReader(r);
                    return Error{ErrorCategory::malformed_xml, {}, "XML attribute limit exceeded"};
                }
                auto* local = xmlTextReaderConstLocalName(r);
                if (local && !found && response_name == reinterpret_cast<const char*>(local))
                {
                    found = true;
                    response_depth = depth;
                    collecting = xmlTextReaderIsEmptyElement(r) != 1;
                }
                if (local && !fault &&
                    std::string_view(reinterpret_cast<const char*>(local)) == "Fault")
                {
                    fault = true;
                    fault_depth = depth;
                }
                else if (fault && !fault_finished && local && !fault_field)
                {
                    // SOAP 1.2: Code/Value, Code/Subcode/Value, Reason/Text, Detail.
                    // SOAP 1.1: faultcode, faultstring, detail.
                    const std::string_view name(reinterpret_cast<const char*>(local));
                    std::string* target = nullptr;
                    if (name == "Subcode") ++open_subcodes;
                    else if (name == "Value")
                        target = open_subcodes > 0 ? &fault_detail.subcode : &fault_detail.code;
                    else if (name == "faultcode") target = &fault_detail.code;
                    else if (name == "Text" || name == "faultstring")
                        target = &fault_detail.reason;
                    else if (name == "Detail" || name == "detail") target = &fault_detail.detail;
                    if (target && xmlTextReaderIsEmptyElement(r) != 1)
                    {
                        // A nested Subcode is more specific than its parent, so its
                        // Value replaces rather than concatenates.
                        if (target == &fault_detail.subcode || target == &fault_detail.code)
                            target->clear();
                        fault_field = target;
                        fault_field_depth = depth;
                    }
                }
            }
            if (xmlTextReaderNodeType(r) == XML_READER_TYPE_END_ELEMENT && collecting &&
                depth == response_depth)
            {
                collecting = false;
            }
            if (xmlTextReaderNodeType(r) == XML_READER_TYPE_END_ELEMENT && fault)
            {
                if (fault_field && depth == fault_field_depth) fault_field = nullptr;
                const auto* local = xmlTextReaderConstLocalName(r);
                const std::string_view name =
                    local ? reinterpret_cast<const char*>(local) : "";
                if (!fault_field && name == "Subcode" && open_subcodes > 0) --open_subcodes;
                if (depth == fault_depth && name == "Fault") fault_finished = true;
            }
            if (xmlTextReaderNodeType(r) == XML_READER_TYPE_DOCUMENT_TYPE)
            {
                xmlFreeTextReader(r);
                return Error{ErrorCategory::malformed_xml, {}, "DTD is prohibited"};
            }
            if (collecting && (xmlTextReaderNodeType(r) == XML_READER_TYPE_TEXT ||
                               xmlTextReaderNodeType(r) == XML_READER_TYPE_CDATA))
            {
                auto* value = xmlTextReaderConstValue(r);
                if (value)
                {
                    auto v = reinterpret_cast<const char*>(value);
                    text += std::char_traits<char>::length(v);
                    if (text > limits.text_bytes)
                    {
                        xmlFreeTextReader(r);
                        return Error{ErrorCategory::malformed_xml, {}, "XML text limit exceeded"};
                    }
                    result += v;
                    result += '\n';
                }
            }
            if (fault_field &&
                (xmlTextReaderNodeType(r) == XML_READER_TYPE_TEXT ||
                 xmlTextReaderNodeType(r) == XML_READER_TYPE_CDATA))
            {
                const auto* value = reinterpret_cast<const char*>(xmlTextReaderConstValue(r));
                if (value)
                {
                    const auto room = 512 - std::min<std::size_t>(fault_field->size(), 512);
                    fault_field->append(
                        value, std::min(room, std::char_traits<char>::length(value))
                    );
                }
            }
        }
        xmlFreeTextReader(r);
        if (read_status < 0)
        {
            return Error{ErrorCategory::malformed_xml, {}, "Malformed XML document"};
        }
        if (!found)
        {
            if (fault)
            {
                std::string normalized(xml);
                std::transform(
                    normalized.begin(),
                    normalized.end(),
                    normalized.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
                );
                if (normalized.find("notauthorized") != std::string::npos ||
                    normalized.find("failedauthentication") != std::string::npos ||
                    normalized.find("authentication failed") != std::string::npos)
                {
                    Error error{
                        ErrorCategory::authentication,
                        {},
                        "device authentication failed"
                    };
                    error.soap_fault = std::move(fault_detail);
                    return error;
                }
                std::string fault_signal = fault_detail.subcode.empty()
                    ? fault_detail.code : fault_detail.subcode;
                std::transform(
                    fault_signal.begin(), fault_signal.end(), fault_signal.begin(),
                    [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
                );
                if (fault_signal == "ter:devicebusy" || fault_signal == "devicebusy")
                {
                    Error error{ErrorCategory::device_busy, {}, "device reported busy", true};
                    error.soap_fault = std::move(fault_detail);
                    return error;
                }
                if (fault_signal == "ter:ratelimit" || fault_signal == "ter:toomanyrequests" ||
                    fault_signal == "ratelimit" || fault_signal == "toomanyrequests")
                {
                    Error error{ErrorCategory::rate_limited, {}, "device rate limit exceeded", true};
                    error.soap_fault = std::move(fault_detail);
                    return error;
                }
                Error error{ErrorCategory::soap_fault, {}, "device returned a SOAP Fault"};
                error.soap_fault = std::move(fault_detail);
                return error;
            }
            return Error{
                ErrorCategory::protocol_violation, {}, "Expected SOAP response element missing"
            };
        }
        return result;
    }
} // namespace wlonvif
