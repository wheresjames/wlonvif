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
        std::string result;
        int read_status = 0;
        while ((read_status = xmlTextReaderRead(r)) == 1)
        {
            if (++nodes > limits.nodes ||
                static_cast<std::size_t>(xmlTextReaderDepth(r)) > limits.depth)
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
                if (local && response_name == reinterpret_cast<const char*>(local))
                {
                    found = true;
                }
                if (local && std::string_view(reinterpret_cast<const char*>(local)) == "Fault")
                {
                    fault = true;
                }
            }
            if (xmlTextReaderNodeType(r) == XML_READER_TYPE_DOCUMENT_TYPE)
            {
                xmlFreeTextReader(r);
                return Error{ErrorCategory::malformed_xml, {}, "DTD is prohibited"};
            }
            if (found && (xmlTextReaderNodeType(r) == XML_READER_TYPE_TEXT ||
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
                    return Error{
                        ErrorCategory::authentication,
                        {},
                        "device authentication failed"
                    };
                }
                return Error{ErrorCategory::soap_fault, {}, "device returned a SOAP Fault"};
            }
            return Error{
                ErrorCategory::protocol_violation, {}, "Expected SOAP response element missing"
            };
        }
        return result;
    }
} // namespace wlonvif
