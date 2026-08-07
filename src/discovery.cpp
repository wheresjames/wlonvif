#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cerrno>
#include <cstring>
#include <curl/curl.h>
#include "curl_init.hpp"
#include "dispatch.hpp"
#include "xml_scan.hpp"
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <wlonvif/wlonvif.hpp>

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#endif

namespace wlonvif
{
    namespace
    {
        constexpr std::string_view MulticastAddress = "239.255.255.250";
        constexpr std::uint16_t DiscoveryPort = 3702;
        constexpr std::size_t MaximumDatagram = 64 * 1024;

        class SocketRuntime
        {
          public:
            SocketRuntime()
            {
#ifdef _WIN32
                WSADATA data{};
                valid_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
            }
            ~SocketRuntime()
            {
#ifdef _WIN32
                if (valid_)
                {
                    WSACleanup();
                }
#endif
            }
            bool valid() const noexcept
            {
                return valid_;
            }

          private:
            bool valid_{true};
        };

        void close_socket(SocketHandle socket)
        {
#ifdef _WIN32
            closesocket(socket);
#else
            close(socket);
#endif
        }

        void make_nonblocking(SocketHandle socket)
        {
#ifdef _WIN32
            u_long enabled = 1;
            (void)ioctlsocket(socket, FIONBIO, &enabled);
#else
            const int flags = fcntl(socket, F_GETFL, 0);
            if (flags >= 0)
            {
                (void)fcntl(socket, F_SETFL, flags | O_NONBLOCK);
            }
#endif
        }

        // Discovery datagrams are unauthenticated attacker-reachable input, so they
        // are scanned in a single forward pass rather than with std::regex, whose
        // recursive backtracking overflows the stack on adversarial documents.
        std::string first_element(std::string_view xml, std::string_view name)
        {
            return detail_xml::first_text(xml, name);
        }

        /// Return the lowercase host of @p url, or empty when it has none.
        std::string url_host(std::string_view url)
        {
            const auto scheme = url.find("://");
            if (scheme == url.npos)
            {
                return {};
            }
            auto authority = url.substr(scheme + 3);
            authority = authority.substr(0, authority.find_first_of("/?#"));
            const auto at = authority.rfind('@');
            if (at != authority.npos)
            {
                authority.remove_prefix(at + 1);
            }
            std::string host;
            if (!authority.empty() && authority.front() == '[')
            {
                const auto closing = authority.find(']');
                host = closing == authority.npos
                           ? std::string{}
                           : std::string(authority.substr(1, closing - 1));
            }
            else
            {
                host = std::string(authority.substr(0, authority.find(':')));
            }
            std::transform(
                host.begin(),
                host.end(),
                host.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); }
            );
            return host;
        }

        std::vector<std::string> words(const std::string& input)
        {
            std::istringstream stream(input);
            std::vector<std::string> output;
            for (std::string value; stream >> value;)
            {
                output.push_back(std::move(value));
            }
            return output;
        }

        std::string random_message_id()
        {
            std::random_device random;
            std::uniform_int_distribution<unsigned> byte(0, 255);
            std::array<unsigned, 16> bytes{};
            for (auto& value : bytes)
            {
                value = byte(random);
            }
            bytes[6] = (bytes[6] & 0x0fU) | 0x40U;
            bytes[8] = (bytes[8] & 0x3fU) | 0x80U;
            std::ostringstream output;
            output << std::hex;
            for (std::size_t index = 0; index < bytes.size(); ++index)
            {
                if (index == 4 || index == 6 || index == 8 || index == 10)
                {
                    output << '-';
                }
                output.width(2);
                output.fill('0');
                output << bytes[index];
            }
            return "urn:uuid:" + output.str();
        }

        std::string probe_xml(const std::string& message_id, DiscoveryProbeType type)
        {
            std::string types;
            if (type == DiscoveryProbeType::device)
            {
                types = "<d:Types>tds:Device</d:Types>";
            }
            else if (type == DiscoveryProbeType::network_video_transmitter)
            {
                types = "<d:Types>dn:NetworkVideoTransmitter</d:Types>";
            }
            return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                   "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
                   "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
                   "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
                   "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
                   "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
                   "<e:Header><w:MessageID>" +
                   message_id +
                   "</w:MessageID><w:To e:mustUnderstand=\"true\">urn:schemas-xmlsoap-org:ws:"
                   "2005:04:discovery</w:To><w:Action e:mustUnderstand=\"true\">http://schemas."
                   "xmlsoap.org/ws/2005/04/discovery/Probe</w:Action></e:Header><e:Body><d:Probe>" +
                   types + "</d:Probe></e:Body></e:Envelope>";
        }

        std::vector<std::string> interface_addresses()
        {
            std::vector<std::string> output;
#ifndef _WIN32
            ifaddrs* interfaces = nullptr;
            if (getifaddrs(&interfaces) != 0)
            {
                return output;
            }
            for (auto* current = interfaces; current; current = current->ifa_next)
            {
                if (!current->ifa_addr || current->ifa_addr->sa_family != AF_INET ||
                    !(current->ifa_flags & IFF_UP) || (current->ifa_flags & IFF_LOOPBACK) ||
                    !(current->ifa_flags & IFF_MULTICAST))
                {
                    continue;
                }
                std::array<char, INET_ADDRSTRLEN> text{};
                const auto* address = reinterpret_cast<sockaddr_in*>(current->ifa_addr);
                if (inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size()))
                {
                    output.emplace_back(text.data());
                }
            }
            freeifaddrs(interfaces);
#endif
            if (output.empty())
            {
                output.emplace_back("0.0.0.0");
            }
            std::sort(output.begin(), output.end());
            output.erase(std::unique(output.begin(), output.end()), output.end());
            return output;
        }

        void merge_candidate(
            std::map<std::string, DeviceCandidate>& candidates,
            DeviceCandidate candidate,
            std::size_t maximum
        )
        {
            if (candidate.address.empty())
            {
                return;
            }
            if (!candidates.contains(candidate.address) && candidates.size() >= maximum)
            {
                return;
            }
            auto [position, inserted] = candidates.try_emplace(candidate.address, candidate);
            if (!inserted &&
                static_cast<int>(candidate.evidence) > static_cast<int>(position->second.evidence))
            {
                auto discovery = position->second.discovery;
                position->second = std::move(candidate);
                if (!position->second.discovery)
                {
                    position->second.discovery = std::move(discovery);
                }
            }
            else if (!inserted && candidate.discovery)
            {
                position->second.discovery = std::move(candidate.discovery);
            }
        }

        void report_progress(
            const NetworkDiscoveryOptions& options,
            DiscoveryProgressStage stage,
            std::size_t completed,
            std::size_t total,
            std::string current
        ) noexcept
        {
            if (!options.progress)
            {
                return;
            }
            try
            {
                options.progress({stage, completed, total, std::move(current)});
            }
            catch (...)
            {
            }
        }

        void receive_matches(
            SocketHandle socket,
            std::string interface_address,
            std::chrono::steady_clock::time_point end,
            const CancellationToken& cancellation,
            std::map<std::string, DeviceCandidate>& candidates,
            std::size_t maximum_results
        )
        {
            std::array<char, MaximumDatagram> buffer{};
            while (!cancellation.stop_requested() && std::chrono::steady_clock::now() < end)
            {
                const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    end - std::chrono::steady_clock::now()
                );
                if (remaining.count() <= 0)
                {
                    break;
                }
                // poll() rather than select(): fd_set silently overflows for any
                // descriptor at or above FD_SETSIZE, which a busy process reaches.
                pollfd readable{socket, POLLIN, 0};
                const int selected =
#ifdef _WIN32
                    WSAPoll(&readable, 1, static_cast<int>(remaining.count()));
#else
                    poll(&readable, 1, static_cast<int>(remaining.count()));
#endif
                if (selected <= 0)
                {
                    break;
                }
                sockaddr_in source{};
#ifdef _WIN32
                int source_size = sizeof(source);
#else
                socklen_t source_size = sizeof(source);
#endif
                const int received = recvfrom(
                    socket,
                    buffer.data(),
                    static_cast<int>(buffer.size()),
                    0,
                    reinterpret_cast<sockaddr*>(&source),
                    &source_size
                );
                if (received <= 0)
                {
                    continue;
                }
                std::array<char, INET_ADDRSTRLEN> address_text{};
                if (!inet_ntop(AF_INET, &source.sin_addr, address_text.data(), address_text.size()))
                {
                    continue;
                }
                const std::string_view xml(buffer.data(), static_cast<std::size_t>(received));
                DiscoveryObservation observation;
                observation.kind = DiscoveryKind::probe_match;
                observation.endpoint_reference = first_element(xml, "Address");
                observation.source = address_text.data();
                observation.interface_id = interface_address;
                observation.scopes = words(first_element(xml, "Scopes"));
                observation.xaddrs = words(first_element(xml, "XAddrs"));
                if (observation.endpoint_reference.empty() && observation.xaddrs.empty())
                {
                    continue;
                }
                DeviceCandidate candidate;
                candidate.address = address_text.data();
                candidate.evidence = DeviceEvidence::discovery;
                candidate.discovery = observation;
                candidate.diagnostic = "WS-Discovery ProbeMatch";
                // The observation keeps every advertised XAddr verbatim because it
                // is documented as untrusted. device_service is different: callers
                // connect to it, so only an address the responding host actually
                // claims for itself may be promoted. Otherwise a spoofed ProbeMatch
                // redirects the caller to any host it names.
                for (const auto& advertised : observation.xaddrs)
                {
                    if (url_host(advertised) == candidate.address)
                    {
                        candidate.device_service = advertised;
                        break;
                    }
                }
                merge_candidate(candidates, std::move(candidate), maximum_results);
            }
        }

        void ws_discover(
            const NetworkDiscoveryOptions& options,
            std::chrono::steady_clock::time_point deadline,
            const CancellationToken& cancellation,
            std::map<std::string, DeviceCandidate>& candidates
        )
        {
            SocketRuntime socket_runtime;
            if (!socket_runtime.valid())
            {
                return;
            }
            auto interfaces =
                options.interfaces.empty() ? interface_addresses() : options.interfaces;
            for (std::size_t interface_index = 0; interface_index < interfaces.size();
                 ++interface_index)
            {
                const auto& interface_address = interfaces[interface_index];
                if (cancellation.stop_requested() || std::chrono::steady_clock::now() >= deadline)
                {
                    return;
                }
                report_progress(
                    options,
                    DiscoveryProgressStage::ws_discovery,
                    interface_index,
                    interfaces.size(),
                    interface_address
                );
                SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (socket == InvalidSocket)
                {
                    continue;
                }
                sockaddr_in local{};
                local.sin_family = AF_INET;
                local.sin_port = htons(0);
                if (inet_pton(AF_INET, interface_address.c_str(), &local.sin_addr) != 1 ||
                    bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
                {
                    close_socket(socket);
                    continue;
                }
                in_addr outgoing = local.sin_addr;
                (void)setsockopt(
                    socket,
                    IPPROTO_IP,
                    IP_MULTICAST_IF,
                    reinterpret_cast<const char*>(&outgoing),
                    sizeof(outgoing)
                );
                sockaddr_in multicast{};
                multicast.sin_family = AF_INET;
                multicast.sin_port = htons(DiscoveryPort);
                (void)inet_pton(AF_INET, MulticastAddress.data(), &multicast.sin_addr);

                std::vector<sockaddr_in> destinations;
                if (options.multicast)
                {
                    destinations.push_back(multicast);
                }
                if (destinations.empty())
                {
                    close_socket(socket);
                    continue;
                }
                std::vector<std::pair<DiscoveryProbeType, std::string>> probes;
                for (auto type : options.probe_types)
                {
                    probes.emplace_back(type, random_message_id());
                }
                for (std::size_t retry = 0;
                     retry < options.retransmissions && !cancellation.stop_requested() &&
                     std::chrono::steady_clock::now() < deadline;
                     ++retry)
                {
                    for (const auto& [type, message_id] : probes)
                    {
                        const auto probe = probe_xml(message_id, type);
                        for (const auto& destination : destinations)
                        {
                            (void)sendto(
                                socket,
                                probe.data(),
                                static_cast<int>(probe.size()),
                                0,
                                reinterpret_cast<const sockaddr*>(&destination),
                                sizeof(destination)
                            );
                        }
                    }
                    if (retry + 1 < options.retransmissions)
                    {
                        const auto delay = options.initial_retry_delay *
                                           static_cast<int>(1U << std::min<std::size_t>(retry, 20));
                        std::this_thread::sleep_for(
                            std::min(
                                delay,
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    deadline - std::chrono::steady_clock::now()
                                )
                            )
                        );
                    }
                }
                const auto listen_end =
                    std::min(deadline, std::chrono::steady_clock::now() + options.listen_duration);
                receive_matches(
                    socket, interface_address, listen_end, cancellation, candidates, options.maximum_results
                );
                close_socket(socket);
                report_progress(
                    options,
                    DiscoveryProgressStage::ws_discovery,
                    interface_index + 1,
                    interfaces.size(),
                    interface_address
                );
            }
        }

        void ws_unicast_discover(
            const NetworkDiscoveryOptions& options,
            std::chrono::steady_clock::time_point deadline,
            const CancellationToken& cancellation,
            std::map<std::string, DeviceCandidate>& candidates
        )
        {
            if (options.targets.empty() || cancellation.stop_requested() ||
                std::chrono::steady_clock::now() >= deadline)
            {
                return;
            }
            SocketRuntime socket_runtime;
            SocketHandle socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (!socket_runtime.valid() || socket == InvalidSocket)
            {
                return;
            }
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(0);
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            if (bind(socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
            {
                close_socket(socket);
                return;
            }
            make_nonblocking(socket);

            std::vector<sockaddr_in> destinations;
            destinations.reserve(options.targets.size());
            for (const auto& target : options.targets)
            {
                sockaddr_in destination{};
                destination.sin_family = AF_INET;
                destination.sin_port = htons(DiscoveryPort);
                if (inet_pton(AF_INET, target.c_str(), &destination.sin_addr) == 1)
                {
                    destinations.push_back(destination);
                }
            }
            std::vector<std::pair<DiscoveryProbeType, std::string>> probes;
            for (auto type : options.probe_types)
            {
                probes.emplace_back(type, random_message_id());
            }
            for (std::size_t retry = 0;
                 retry < options.retransmissions && !cancellation.stop_requested() &&
                 std::chrono::steady_clock::now() < deadline;
                 ++retry)
            {
                for (const auto& [type, message_id] : probes)
                {
                    const auto probe = probe_xml(message_id, type);
                    for (const auto& destination : destinations)
                    {
                        (void)sendto(
                            socket,
                            probe.data(),
                            static_cast<int>(probe.size()),
                            0,
                            reinterpret_cast<const sockaddr*>(&destination),
                            sizeof(destination)
                        );
                    }
                }
                if (retry + 1 < options.retransmissions)
                {
                    std::this_thread::sleep_for(options.initial_retry_delay);
                }
            }
            const auto listen_end =
                std::min(deadline, std::chrono::steady_clock::now() + options.listen_duration);
            receive_matches(
                socket,
                "unicast",
                listen_end,
                cancellation,
                candidates,
                options.maximum_results
            );
            close_socket(socket);
        }

        Result<std::vector<std::string>>
        expand_networks(const std::vector<std::string>& networks, std::size_t maximum_hosts)
        {
            std::set<std::string> output;
            for (const auto& network : networks)
            {
                const auto slash = network.find('/');
                if (slash == std::string::npos)
                {
                    return Error{
                        ErrorCategory::invalid_argument,
                        OperationId::discovery_probe,
                        "scan network is not CIDR: " + network
                    };
                }
                in_addr address{};
                if (inet_pton(AF_INET, network.substr(0, slash).c_str(), &address) != 1)
                {
                    return Error{
                        ErrorCategory::invalid_argument,
                        OperationId::discovery_probe,
                        "invalid scan network: " + network
                    };
                }
                int prefix = -1;
                const auto prefix_text = std::string_view(network).substr(slash + 1);
                const auto parsed =
                    std::from_chars(prefix_text.data(), prefix_text.data() + prefix_text.size(), prefix);
                if (parsed.ec != std::errc{} || parsed.ptr != prefix_text.data() + prefix_text.size() ||
                    prefix < 0 || prefix > 32)
                {
                    return Error{
                        ErrorCategory::invalid_argument,
                        OperationId::discovery_probe,
                        "invalid CIDR prefix: " + network
                    };
                }
                const std::uint64_t count = std::uint64_t{1} << (32 - prefix);
                if (count > maximum_hosts || output.size() + count > maximum_hosts)
                {
                    return Error{
                        ErrorCategory::invalid_argument,
                        OperationId::discovery_probe,
                        "scan exceeds maximum_hosts"
                    };
                }
                const std::uint32_t host_address = ntohl(address.s_addr);
                const std::uint32_t mask = prefix == 0 ? 0 : 0xffffffffU << (32 - prefix);
                const std::uint32_t base = host_address & mask;
                for (std::uint64_t offset = 0; offset < count; ++offset)
                {
                    if (count > 2 && (offset == 0 || offset + 1 == count))
                    {
                        continue;
                    }
                    in_addr candidate{htonl(base + static_cast<std::uint32_t>(offset))};
                    std::array<char, INET_ADDRSTRLEN> text{};
                    if (inet_ntop(AF_INET, &candidate, text.data(), text.size()))
                    {
                        output.emplace(text.data());
                    }
                }
            }
            std::vector<std::string> addresses(output.begin(), output.end());
            std::sort(
                addresses.begin(),
                addresses.end(),
                [](const std::string& left, const std::string& right)
                {
                    in_addr left_address{};
                    in_addr right_address{};
                    (void)inet_pton(AF_INET, left.c_str(), &left_address);
                    (void)inet_pton(AF_INET, right.c_str(), &right_address);
                    return ntohl(left_address.s_addr) < ntohl(right_address.s_addr);
                }
            );
            return addresses;
        }

        std::size_t collect_body(char* data, std::size_t size, std::size_t count, void* user)
        {
            auto& body = *static_cast<std::string*>(user);
            const auto bytes = size * count;
            if (body.size() + bytes > 1024 * 1024)
            {
                return 0;
            }
            body.append(data, bytes);
            return bytes;
        }

        std::optional<DeviceCandidate> probe_address(
            const std::string& address,
            const std::optional<std::string>& advertised_url,
            const NetworkDiscoveryOptions& options,
            const EndpointPolicy& policy,
            std::chrono::steady_clock::time_point deadline,
            const CancellationToken& cancellation
        )
        {
            const auto body = soap_envelope(OperationId::get_system_date_and_time, "");
            struct Candidate
            {
                std::string url;
                bool untrusted; ///< True when the device, not the caller, supplied it.
            };
            std::vector<Candidate> urls;
            if (advertised_url)
            {
                urls.push_back({*advertised_url, true});
            }
            for (auto port : options.ports)
            {
                for (const auto& path : options.device_paths)
                {
                    const bool secure = port == 443 || port == 8443;
                    urls.push_back(
                        {(secure ? "https://" : "http://") + address + ":" +
                             std::to_string(port) + path,
                         false}
                    );
                }
            }
            // The address under inspection is the origin the policy is evaluated
            // against. Authorizing a URL against its own origin, as this previously
            // did, makes every policy a tautology: a spoofed ProbeMatch can then
            // name any host or port and have this send a request to it.
            const std::string reference_origin = "http://" + address;
            for (const auto& [url, untrusted] : urls)
            {
                if (cancellation.stop_requested() || std::chrono::steady_clock::now() >= deadline)
                {
                    return std::nullopt;
                }
                // URLs built from the caller's own address, ports, and paths carry
                // no device input and are authorized by construction.
                if (untrusted && !policy.authorize(url, reference_origin))
                {
                    continue;
                }
                CURL* curl = curl_easy_init();
                if (!curl)
                {
                    continue;
                }
                curl_slist* headers = nullptr;
                headers = curl_slist_append(
                    headers, "Content-Type: application/soap+xml; charset=utf-8"
                );
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
                const auto remaining = std::max(
                    std::chrono::milliseconds(1),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now()
                    )
                );
                curl_easy_setopt(
                    curl,
                    CURLOPT_CONNECTTIMEOUT_MS,
                    static_cast<long>(std::min(options.connect_timeout, remaining).count())
                );
                curl_easy_setopt(
                    curl,
                    CURLOPT_TIMEOUT_MS,
                    static_cast<long>(std::min(options.request_timeout, remaining).count())
                );
                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
                std::string response_body;
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_body);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
                const CURLcode result = curl_easy_perform(curl);
                long status = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                const bool onvif_response =
                    response_body.find("GetSystemDateAndTimeResponse") != std::string::npos;
                const bool soap_fault = response_body.find(":Fault") != std::string::npos ||
                                        response_body.find("<Fault") != std::string::npos;
                if (result == CURLE_OK &&
                    (status == 401 || status == 403 || onvif_response || soap_fault))
                {
                    DeviceCandidate candidate;
                    candidate.address = address;
                    candidate.device_service = url;
                    if (status == 401 || status == 403)
                    {
                        candidate.evidence = DeviceEvidence::authentication_challenge;
                        candidate.diagnostic = "Device service authentication challenge";
                    }
                    else if (soap_fault)
                    {
                        candidate.evidence = DeviceEvidence::soap_fault;
                        candidate.diagnostic = "SOAP fault from Device service candidate";
                    }
                    else
                    {
                        candidate.evidence = DeviceEvidence::onvif_response;
                        candidate.diagnostic = "ONVIF Device operation response";
                    }
                    return candidate;
                }
            }
            return std::nullopt;
        }

        void direct_probe(
            const std::vector<std::string>& addresses,
            const NetworkDiscoveryOptions& options,
            const EndpointPolicy& policy,
            std::chrono::steady_clock::time_point deadline,
            const CancellationToken& cancellation,
            std::map<std::string, DeviceCandidate>& candidates
        )
        {
            struct Target
            {
                std::string address;
                std::optional<std::string> advertised_url;
            };
            std::vector<Target> targets;
            targets.reserve(addresses.size());
            for (const auto& address : addresses)
            {
                const auto discovered = candidates.find(address);
                targets.push_back(
                    {address,
                     discovered == candidates.end() ? std::nullopt
                                                    : discovered->second.device_service}
                );
            }

            std::atomic_size_t next{};
            std::atomic_size_t completed{};
            std::mutex output_mutex;
            const auto worker_count = std::min(options.maximum_concurrency, targets.size());
            std::vector<std::jthread> workers;
            workers.reserve(worker_count);
            for (std::size_t worker = 0; worker < worker_count; ++worker)
            {
                workers.emplace_back(
                    [&]
                    {
                        for (;;)
                        {
                            const auto index = next.fetch_add(1);
                            if (index >= targets.size() || cancellation.stop_requested() ||
                                std::chrono::steady_clock::now() >= deadline)
                            {
                                return;
                            }
                            const auto& target = targets[index];
                            {
                                std::lock_guard lock(output_mutex);
                                report_progress(
                                    options,
                                    DiscoveryProgressStage::direct_probe,
                                    completed.load(),
                                    targets.size(),
                                    target.address
                                );
                            }
                            auto candidate = probe_address(
                                target.address,
                                target.advertised_url,
                                options,
                                policy,
                                deadline,
                                cancellation
                            );
                            const auto count = completed.fetch_add(1) + 1;
                            std::lock_guard lock(output_mutex);
                            if (candidate)
                            {
                                merge_candidate(
                                    candidates, std::move(*candidate), options.maximum_results
                                );
                            }
                            report_progress(
                                options,
                                DiscoveryProgressStage::direct_probe,
                                count,
                                targets.size(),
                                target.address
                            );
                        }
                    }
                );
            }
        }
    } // namespace

    Result<std::vector<std::string>> local_ipv4_networks(std::size_t maximum_hosts)
    {
        std::vector<std::string> output;
#ifndef _WIN32
        ifaddrs* interfaces = nullptr;
        if (getifaddrs(&interfaces) != 0)
        {
            return Error{
                ErrorCategory::network,
                OperationId::discovery_probe,
                "unable to enumerate network interfaces"
            };
        }
        for (auto* current = interfaces; current; current = current->ifa_next)
        {
            if (!current->ifa_addr || !current->ifa_netmask ||
                current->ifa_addr->sa_family != AF_INET || !(current->ifa_flags & IFF_UP) ||
                (current->ifa_flags & IFF_LOOPBACK))
            {
                continue;
            }
            const auto address = reinterpret_cast<sockaddr_in*>(current->ifa_addr)->sin_addr;
            const auto mask = reinterpret_cast<sockaddr_in*>(current->ifa_netmask)->sin_addr;
            const std::uint32_t host_mask = ntohl(mask.s_addr);
            const int prefix = std::popcount(host_mask);
            const std::uint64_t hosts = std::uint64_t{1} << (32 - prefix);
            if (hosts > maximum_hosts)
            {
                continue;
            }
            in_addr network{address.s_addr & mask.s_addr};
            std::array<char, INET_ADDRSTRLEN> text{};
            if (inet_ntop(AF_INET, &network, text.data(), text.size()))
            {
                output.push_back(std::string(text.data()) + "/" + std::to_string(prefix));
            }
        }
        freeifaddrs(interfaces);
#endif
        std::sort(output.begin(), output.end());
        output.erase(std::unique(output.begin(), output.end()), output.end());
        return output;
    }

    Task<Result<std::vector<DeviceCandidate>>> discover_devices(
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        NetworkDiscoveryOptions options,
        CallOptions call
    )
    {
        return Task<Result<std::vector<DeviceCandidate>>>(
            [runtime = std::move(runtime),
             endpoint_policy = std::move(endpoint_policy),
             options = std::move(options),
             call](auto completion) mutable
            {
                std::thread(
                    [runtime = std::move(runtime),
                     endpoint_policy = std::move(endpoint_policy),
                     options = std::move(options),
                     call,
                     completion = std::move(completion)]() mutable
                    {
                        Result<std::vector<DeviceCandidate>> result = Error{
                            ErrorCategory::internal,
                            OperationId::discovery_probe,
                            "discovery did not complete"
                        };
                        try
                        {
                            if (options.maximum_concurrency == 0)
                            {
                                throw std::invalid_argument("maximum_concurrency must be positive");
                            }
                            std::vector<std::string> scan_addresses;
                            if (!options.scan_networks.empty())
                            {
                                auto expanded =
                                    expand_networks(options.scan_networks, options.maximum_hosts);
                                if (!expanded)
                                {
                                    result = expanded.error();
                                    detail::dispatch(
                                        runtime,
                                        [completion = std::move(completion),
                                         result = std::move(result)]() mutable
                                        {
                                            completion(std::move(result));
                                        }
                                    );
                                    return;
                                }
                                scan_addresses = std::move(expanded.value());
                            }

                            std::map<std::string, DeviceCandidate> candidates;
                            auto discovery_options = options;
                            discovery_options.targets.insert(
                                discovery_options.targets.end(),
                                scan_addresses.begin(),
                                scan_addresses.end()
                            );
                            ws_unicast_discover(
                                discovery_options, call.deadline, call.cancellation, candidates
                            );
                            ws_discover(
                                discovery_options, call.deadline, call.cancellation, candidates
                            );

                            std::vector<std::string> addresses = options.targets;
                            addresses.insert(
                                addresses.end(), scan_addresses.begin(), scan_addresses.end()
                            );
                            if (options.direct_probe)
                            {
                                for (const auto& [address, _] : candidates)
                                {
                                    addresses.push_back(address);
                                }
                            }
                            std::sort(
                                addresses.begin(),
                                addresses.end(),
                                [](const std::string& left, const std::string& right)
                                {
                                    in_addr left_address{};
                                    in_addr right_address{};
                                    const bool left_ipv4 =
                                        inet_pton(AF_INET, left.c_str(), &left_address) == 1;
                                    const bool right_ipv4 =
                                        inet_pton(AF_INET, right.c_str(), &right_address) == 1;
                                    if (left_ipv4 != right_ipv4)
                                    {
                                        return left_ipv4;
                                    }
                                    if (left_ipv4)
                                    {
                                        return ntohl(left_address.s_addr) <
                                               ntohl(right_address.s_addr);
                                    }
                                    return left < right;
                                }
                            );
                            addresses.erase(
                                std::unique(addresses.begin(), addresses.end()), addresses.end()
                            );
                            if (options.direct_probe && endpoint_policy)
                            {
                                detail::initialize_curl();
                                direct_probe(
                                    addresses,
                                    options,
                                    *endpoint_policy,
                                    call.deadline,
                                    call.cancellation,
                                    candidates
                                );
                            }
                            if (call.cancellation.stop_requested())
                            {
                                result = Error{
                                    ErrorCategory::cancelled,
                                    OperationId::discovery_probe,
                                    "discovery cancelled"
                                };
                            }
                            else
                            {
                                std::vector<DeviceCandidate> output;
                                output.reserve(candidates.size());
                                for (auto& [_, candidate] : candidates)
                                {
                                    output.push_back(std::move(candidate));
                                }
                                result = std::move(output);
                            }
                        }
                        catch (const std::exception& error)
                        {
                            result = Error{
                                ErrorCategory::invalid_argument,
                                OperationId::discovery_probe,
                                error.what()
                            };
                        }
                        detail::dispatch(
                            runtime,
                            [completion = std::move(completion),
                             result = std::move(result)]() mutable
                            {
                                completion(std::move(result));
                            }
                        );
                    }
                ).detach();
            }
        );
    }
} // namespace wlonvif
