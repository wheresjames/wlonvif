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
        std::string fault_field;
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
                if (local && std::string_view(reinterpret_cast<const char*>(local)) == "Fault")
                {
                    fault = true;
                }
                if (fault && local)
                {
                    const std::string_view name(reinterpret_cast<const char*>(local));
                    if (name == "Value")
                        fault_field = fault_detail.code.empty() ? "code" : "subcode";
                    else if (name == "Text") fault_field = "reason";
                    else if (name == "Detail") fault_field = "detail";
                }
            }
            if (xmlTextReaderNodeType(r) == XML_READER_TYPE_END_ELEMENT && collecting &&
                depth == response_depth)
            {
                collecting = false;
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
            if (fault && !fault_field.empty() &&
                (xmlTextReaderNodeType(r) == XML_READER_TYPE_TEXT ||
                 xmlTextReaderNodeType(r) == XML_READER_TYPE_CDATA))
            {
                const auto* value = reinterpret_cast<const char*>(xmlTextReaderConstValue(r));
                if (value)
                {
                    std::string* target = fault_field == "code" ? &fault_detail.code
                        : fault_field == "subcode" ? &fault_detail.subcode
                        : fault_field == "reason" ? &fault_detail.reason : &fault_detail.detail;
                    target->append(value, std::min<std::size_t>(512 - std::min<std::size_t>(target->size(), 512), std::char_traits<char>::length(value)));
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
