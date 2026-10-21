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

        Result<void> authorize_discovery(
            const NetworkDiscoveryOptions& options,
            DiscoveryAuthorizationRequest request,
            std::chrono::steady_clock::time_point deadline,
            const CancellationToken& cancellation
        )
        {
            if (!options.authorizer)
            {
                return options.require_authorization
                    ? Result<void>(Error{
                          ErrorCategory::host_permission_denied,
                          OperationId::discovery_probe,
                          "discovery network authorization is unavailable"
                      })
                    : Result<void>{};
            }
            struct State
            {
                std::mutex mutex;
                std::condition_variable ready;
                std::optional<Result<void>> result;
            };
            auto state = std::make_shared<State>();
            auto task = options.authorizer->authorize(
                std::move(request), {deadline, cancellation}
            );
            std::move(task).start([state](Result<void> result)
            {
                {
                    std::lock_guard lock(state->mutex);
                    state->result.emplace(std::move(result));
                }
                state->ready.notify_one();
            });
            std::unique_lock lock(state->mutex);
            // Cancellation cannot notify this condition variable, so the wait is
            // sliced; waiting straight to the deadline would ignore cancellation.
            while (!state->result && !cancellation.stop_requested() &&
                   std::chrono::steady_clock::now() < deadline)
                state->ready.wait_until(
                    lock,
                    std::min(deadline, std::chrono::steady_clock::now() +
                                           std::chrono::milliseconds(20))
                );
            if (cancellation.stop_requested()) return Error{ErrorCategory::cancelled};
            if (!state->result) return Error{ErrorCategory::timeout};
            auto result = std::move(*state->result);
            if (!result && result.error().category != ErrorCategory::cancelled &&
                result.error().category != ErrorCategory::timeout)
                result.error().category = ErrorCategory::host_permission_denied;
            return result;
        }

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

        Result<void> ws_discover(
            const NetworkDiscoveryOptions& options,
            std::chrono::steady_clock::time_point deadline,
            const CancellationToken& cancellation,
            std::map<std::string, DeviceCandidate>& candidates
        )
        {
            SocketRuntime socket_runtime;
            if (!socket_runtime.valid())
            {
                return {};
            }
            if (options.interfaces.empty())
            {
                auto authorized = authorize_discovery(
                    options,
                    {DiscoveryNetworkAction::enumerate_interfaces, {}, {}, 0, {}},
                    deadline,
                    cancellation
                );
                if (!authorized) return authorized;
            }
            auto interfaces =
                options.interfaces.empty() ? interface_addresses() : options.interfaces;
            // Each interface waits a full listen window, so interfaces are listened
            // to concurrently; serially, hosts with several interfaces (container
            // bridges, VPNs) spent the whole deadline here before direct probing.
            // Authorization stays on this thread so the authorizer is never
            // invoked concurrently.
            std::mutex progress_mutex;
            std::size_t interfaces_completed = 0;
            std::vector<std::map<std::string, DeviceCandidate>> found(interfaces.size());
            std::vector<std::jthread> listeners;
            listeners.reserve(interfaces.size());
            for (std::size_t interface_index = 0; interface_index < interfaces.size();
                 ++interface_index)
            {
                const auto& interface_address = interfaces[interface_index];
                if (cancellation.stop_requested() || std::chrono::steady_clock::now() >= deadline)
                {
                    break;
                }
                {
                    std::lock_guard lock(progress_mutex);
                    report_progress(
                        options,
                        DiscoveryProgressStage::ws_discovery,
                        interfaces_completed,
                        interfaces.size(),
                        interface_address
                    );
                }
                auto bind_authorized = authorize_discovery(
                    options,
                    {DiscoveryNetworkAction::bind_udp, interface_address, {}, 0, {}},
                    deadline,
                    cancellation
                );
                if (!bind_authorized) return bind_authorized;
                if (options.multicast)
                {
                    auto send_authorized = authorize_discovery(
                        options,
                        {DiscoveryNetworkAction::send_multicast,
                         interface_address,
                         std::string(MulticastAddress),
                         DiscoveryPort,
                         {}},
                        deadline,
                        cancellation
                    );
                    if (!send_authorized) return send_authorized;
                }
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
                listeners.emplace_back(
                    [&, interface_index, socket, destinations = std::move(destinations)]
                    {
                    const auto& interface_address = interfaces[interface_index];
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
                        socket,
                        interface_address,
                        listen_end,
                        cancellation,
                        found[interface_index],
                        options.maximum_results
                    );
                    close_socket(socket);
                    std::lock_guard lock(progress_mutex);
                    report_progress(
                        options,
                        DiscoveryProgressStage::ws_discovery,
                        ++interfaces_completed,
                        interfaces.size(),
                        interface_address
                    );
                    }
                );
            }
            for (auto& listener : listeners)
            {
                listener.join();
            }
            for (auto& interface_candidates : found)
            {
                for (auto& [_, candidate] : interface_candidates)
                {
                    merge_candidate(candidates, std::move(candidate), options.maximum_results);
                }
            }
            return {};
        }

        Result<void> authorize_unicast_discovery(
            const NetworkDiscoveryOptions& options,
            std::chrono::steady_clock::time_point deadline,
            const CancellationToken& cancellation
        )
        {
            if (options.targets.empty())
            {
                return {};
            }
            auto bind_authorized = authorize_discovery(
                options,
                {DiscoveryNetworkAction::bind_udp, "0.0.0.0", {}, 0, {}},
                deadline,
                cancellation
            );
            if (!bind_authorized) return bind_authorized;
            for (const auto& target : options.targets)
            {
                auto send_authorized = authorize_discovery(
                    options,
                    {DiscoveryNetworkAction::send_unicast, {}, target, DiscoveryPort, {}},
                    deadline,
                    cancellation
                );
                if (!send_authorized) return send_authorized;
            }
            return {};
        }

        /// Send unicast Probes to every target; authorize_unicast_discovery() first.
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
            return;
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

        /// Result of directly probing one address.
        struct ProbeOutcome
        {
            std::optional<DeviceCandidate> candidate;
            /// False when the deadline or cancellation cut the probe short, so
            /// finding nothing does not show that nothing is there.
            bool complete{true};
        };

        /// Classify one finished probe request; nullopt when it found nothing.
        std::optional<DeviceCandidate> classify_probe(
            const std::string& address,
            const std::string& url,
            CURLcode result,
            long status,
            const std::string& response_body
        )
        {
            const bool onvif_response =
                response_body.find("GetSystemDateAndTimeResponse") != std::string::npos;
            const bool soap_fault = response_body.find(":Fault") != std::string::npos ||
                                    response_body.find("<Fault") != std::string::npos;
            if (result != CURLE_OK ||
                !(status == 401 || status == 403 || onvif_response || soap_fault))
            {
                return std::nullopt;
            }
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

        ProbeOutcome probe_address(
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
            if (cancellation.stop_requested() || std::chrono::steady_clock::now() >= deadline)
            {
                return {std::nullopt, false};
            }
            // Every URL is requested at once. Serially, an address with nothing
            // behind it cost one full connect timeout per port, which made a
            // range scan take several times longer than any single probe.
            struct Request
            {
                std::string url;
                CURL* curl{};
                curl_slist* headers{};
                std::string response_body;
                bool connect_clipped{}; ///< Deadline shortened the connect timeout.
                bool total_clipped{};   ///< Deadline shortened the request timeout.
                CURLcode result{CURLE_OK};
                bool finished{};
            };
            std::vector<Request> requests;
            requests.reserve(urls.size());
            // The address under inspection is the origin the policy is evaluated
            // against. Authorizing a URL against its own origin, as this previously
            // did, makes every policy a tautology: a spoofed ProbeMatch can then
            // name any host or port and have this send a request to it.
            const std::string reference_origin = "http://" + address;
            const auto remaining = std::max(
                std::chrono::milliseconds(1),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()
                )
            );
            for (const auto& [url, untrusted] : urls)
            {
                // URLs built from the caller's own address, ports, and paths carry
                // no device input and are authorized by construction.
                if (untrusted && !policy.authorize(url, reference_origin))
                {
                    continue;
                }
                Request request;
                request.url = url;
                request.connect_clipped = remaining < options.connect_timeout;
                request.total_clipped = remaining < options.request_timeout;
                requests.push_back(std::move(request));
            }
            CURLM* multi = curl_multi_init();
            if (!multi)
            {
                return {std::nullopt, false};
            }
            for (auto& request : requests)
            {
                request.curl = curl_easy_init();
                if (!request.curl)
                {
                    request.finished = true;
                    request.result = CURLE_FAILED_INIT;
                    continue;
                }
                CURL* curl = request.curl;
                request.headers = curl_slist_append(
                    nullptr, "Content-Type: application/soap+xml; charset=utf-8"
                );
                curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
                // Probes run on many worker threads; signal-based DNS timeouts in
                // a synchronous-resolver libcurl build are unsafe there.
                curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request.headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
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
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_body);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &request.response_body);
                curl_easy_setopt(curl, CURLOPT_PRIVATE, &request);
                curl_multi_add_handle(multi, curl);
            }
            for (int running = 1; running > 0;)
            {
                if (curl_multi_perform(multi, &running) != CURLM_OK) break;
                for (int queued = 0; CURLMsg* message = curl_multi_info_read(multi, &queued);)
                {
                    if (message->msg != CURLMSG_DONE) continue;
                    Request* request = nullptr;
                    curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &request);
                    if (request)
                    {
                        request->finished = true;
                        request->result = message->data.result;
                    }
                }
                if (running == 0) break;
                // Polling in slices keeps cancellation responsive.
                if (cancellation.stop_requested()) break;
                (void)curl_multi_poll(multi, nullptr, 0, 50, nullptr);
            }
            ProbeOutcome outcome;
            // URLs are examined in priority order, so the advertised XAddr wins
            // over a guessed port exactly as it did when they were tried in turn.
            for (auto& request : requests)
            {
                if (!request.curl) continue;
                long status = 0;
                curl_off_t connected = 0;
                curl_easy_getinfo(request.curl, CURLINFO_RESPONSE_CODE, &status);
                curl_easy_getinfo(request.curl, CURLINFO_CONNECT_TIME_T, &connected);
                if (!outcome.candidate && request.finished)
                {
                    outcome.candidate = classify_probe(
                        address, request.url, request.result, status, request.response_body
                    );
                }
                // A timeout only proves absence when the full configured timeout
                // ran; one the deadline shortened leaves the question open.
                const bool timed_out = request.result == CURLE_OPERATION_TIMEDOUT;
                if (!request.finished ||
                    (timed_out && (connected == 0 ? request.connect_clipped : request.total_clipped)))
                {
                    outcome.complete = false;
                }
                curl_multi_remove_handle(multi, request.curl);
                curl_easy_cleanup(request.curl);
                curl_slist_free_all(request.headers);
            }
            curl_multi_cleanup(multi);
            // Requests an abort left unfinished already cleared complete; a found
            // device answers the question regardless of the other ports.
            if (outcome.candidate) outcome.complete = true;
            return outcome;
        }

        /// Direct-probe coverage, recorded per address in probe order.
        struct ProbeCoverage
        {
            std::vector<std::string> unchecked;
            std::vector<std::string> incomplete;
        };

        ProbeCoverage direct_probe(
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
            enum class Coverage { pending, complete, incomplete };
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
            std::vector<Coverage> coverage(targets.size(), Coverage::pending);

            std::atomic_size_t next{};
            std::atomic_size_t completed{};
            std::mutex output_mutex;
            const auto worker_count = std::min(options.maximum_concurrency, targets.size());
            {
                std::vector<std::jthread> workers;
                workers.reserve(worker_count);
                for (std::size_t worker = 0; worker < worker_count; ++worker)
                {
                    workers.emplace_back(
                        [&]
                        {
                            for (;;)
                            {
                                if (cancellation.stop_requested() ||
                                    std::chrono::steady_clock::now() >= deadline)
                                {
                                    return;
                                }
                                const auto index = next.fetch_add(1);
                                if (index >= targets.size())
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
                                auto outcome = probe_address(
                                    target.address,
                                    target.advertised_url,
                                    options,
                                    policy,
                                    deadline,
                                    cancellation
                                );
                                const auto count = completed.fetch_add(1) + 1;
                                std::lock_guard lock(output_mutex);
                                coverage[index] =
                                    outcome.complete ? Coverage::complete : Coverage::incomplete;
                                if (outcome.candidate)
                                {
                                    merge_candidate(
                                        candidates, std::move(*outcome.candidate), options.maximum_results
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
            ProbeCoverage output;
            for (std::size_t index = 0; index < targets.size(); ++index)
            {
                if (coverage[index] == Coverage::pending)
                    output.unchecked.push_back(targets[index].address);
                else if (coverage[index] == Coverage::incomplete)
                    output.incomplete.push_back(targets[index].address);
            }
            return output;
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

    Task<Result<DiscoveryReport>> discover_network(
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        NetworkDiscoveryOptions options,
        CallOptions call
    )
    {
        return Task<Result<DiscoveryReport>>(
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
                        Result<DiscoveryReport> result = Error{
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
                                for (const auto& network : options.scan_networks)
                                {
                                    auto authorized = authorize_discovery(
                                        options,
                                        {DiscoveryNetworkAction::scan_network,
                                         {}, {}, 0, network},
                                        call.deadline,
                                        call.cancellation
                                    );
                                    if (!authorized)
                                    {
                                        result = authorized.error();
                                        detail::dispatch(
                                            runtime,
                                            [completion = std::move(completion),
                                             result = std::move(result)]() mutable
                                            { completion(std::move(result)); }
                                        );
                                        return;
                                    }
                                }
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
                            auto unicast_authorized = authorize_unicast_discovery(
                                discovery_options, call.deadline, call.cancellation
                            );
                            if (!unicast_authorized) throw unicast_authorized.error();
                            // Unicast and multicast each wait a listen window, so they
                            // overlap rather than consuming the deadline back to back.
                            std::map<std::string, DeviceCandidate> unicast_candidates;
                            std::jthread unicast(
                                [&]
                                {
                                    ws_unicast_discover(
                                        discovery_options,
                                        call.deadline,
                                        call.cancellation,
                                        unicast_candidates
                                    );
                                }
                            );
                            auto multicast = ws_discover(
                                discovery_options, call.deadline, call.cancellation, candidates
                            );
                            unicast.join();
                            if (!multicast) throw multicast.error();
                            for (auto& [_, candidate] : unicast_candidates)
                            {
                                merge_candidate(
                                    candidates, std::move(candidate), options.maximum_results
                                );
                            }

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
                            ProbeCoverage coverage;
                            if (options.direct_probe && endpoint_policy)
                            {
                                for (const auto& address : addresses)
                                {
                                    for (const auto port : options.ports)
                                    {
                                        auto authorized = authorize_discovery(
                                            options,
                                            {DiscoveryNetworkAction::connect_tcp,
                                             {}, address, port, std::nullopt},
                                            call.deadline,
                                            call.cancellation
                                        );
                                        if (!authorized) throw authorized.error();
                                    }
                                }
                                detail::initialize_curl();
                                coverage = direct_probe(
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
                                DiscoveryReport report;
                                report.devices.reserve(candidates.size());
                                for (auto& [_, candidate] : candidates)
                                {
                                    report.devices.push_back(std::move(candidate));
                                }
                                report.unchecked = std::move(coverage.unchecked);
                                report.incomplete = std::move(coverage.incomplete);
                                result = std::move(report);
                            }
                        }
                        catch (const Error& error)
                        {
                            result = error;
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

    Task<Result<std::vector<DeviceCandidate>>> discover_devices(
        std::shared_ptr<Runtime> runtime,
        std::shared_ptr<EndpointPolicy> endpoint_policy,
        NetworkDiscoveryOptions options,
        CallOptions call
    )
    {
        return Task<Result<std::vector<DeviceCandidate>>>(
            [report = discover_network(
                 std::move(runtime), std::move(endpoint_policy), std::move(options), call
             )](auto completion) mutable
            {
                std::move(report).start(
                    [completion = std::move(completion)](Result<DiscoveryReport> result) mutable
                    {
                        if (!result)
                        {
                            completion(result.error());
                            return;
                        }
                        completion(std::move(result.value().devices));
                    }
                );
            }
        );
    }

    Result<std::chrono::milliseconds> discovery_time_bound(const NetworkDiscoveryOptions& options)
    {
        using std::chrono::milliseconds;
        milliseconds bound{1000}; // Thread start-up, socket setup, and completion dispatch.
        const bool unicast = !options.targets.empty() || !options.scan_networks.empty();
        if (options.multicast || unicast)
        {
            // Unicast and every multicast interface listen concurrently; the
            // multicast backoff between retransmissions is the longer of the two.
            milliseconds backoff{};
            for (std::size_t retry = 0; retry + 1 < options.retransmissions; ++retry)
            {
                backoff += options.initial_retry_delay *
                           static_cast<int>(1U << std::min<std::size_t>(retry, 20));
            }
            bound += backoff + options.listen_duration;
        }
        if (options.direct_probe && options.maximum_concurrency > 0)
        {
            std::size_t addresses = options.targets.size();
            if (!options.scan_networks.empty())
            {
                auto expanded = expand_networks(options.scan_networks, options.maximum_hosts);
                if (!expanded) return expanded.error();
                addresses += expanded.value().size();
            }
            // WS-Discovery may add responders outside the targets and ranges;
            // allow one further round of probes for them.
            const auto rounds =
                (addresses + options.maximum_concurrency - 1) / options.maximum_concurrency + 1;
            bound += options.request_timeout * static_cast<long>(rounds);
        }
        return bound;
    }
} // namespace wlonvif
