#include <WinSock2.h>
#include <WS2tcpip.h>

#include "LamaPon/Online/NetworkTransport.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <map>

namespace LamaPon::Detail
{
    namespace
    {
        constexpr std::size_t QueueLimit = 256 * 1024;
        constexpr std::size_t IoBudget = 32 * 1024;

        bool Nonblocking(const SOCKET socket)
        {
            u_long enabled = 1;
            return ioctlsocket(socket, FIONBIO, &enabled) == 0;
        }

        bool ParseEndpoint(const std::string& text, const std::uint16_t defaultPort,
            sockaddr_in& address)
        {
            std::string ip = text;
            std::uint32_t port = defaultPort;
            if (const auto split = ip.find(':'); split != std::string::npos)
            {
                const auto number = ip.substr(split + 1);
                const auto result = std::from_chars(number.data(),
                    number.data() + number.size(), port);
                if (result.ec != std::errc{} || result.ptr != number.data() + number.size()
                    || port == 0 || port > 65535)
                {
                    return false;
                }
                ip.resize(split);
            }
            if (ip == "localhost") ip = "127.0.0.1";
            address.sin_family = AF_INET;
            address.sin_port = htons(static_cast<u_short>(port));
            return InetPtonA(AF_INET, ip.c_str(), &address.sin_addr) == 1;
        }

        struct Connection final
        {
            SOCKET socket{ INVALID_SOCKET };
            bool connecting{};
            std::vector<char> incoming;
            std::deque<std::string> outgoing;
            std::size_t offset{};
            std::size_t queuedBytes{};
        };

        class LanTransport final : public INetworkTransport
        {
        public:
            ~LanTransport() override { Stop(); }

            bool Start(const NetworkConfiguration& configuration, const bool host,
                const std::string& address, const std::string&) override
            {
                Stop();
                m_error.clear();
                sockaddr_in endpoint{};
                if (!ParseEndpoint(address, configuration.port, endpoint)
                    || (!host && endpoint.sin_port == 0))
                {
                    m_error = "数値IPv4とポートを指定してください（例: 192.168.1.10:27840）。";
                    return false;
                }
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                {
                    m_error = "Windowsのネットワークを初期化できません。";
                    return false;
                }
                m_initialized = true;
                const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (socket == INVALID_SOCKET || !Nonblocking(socket))
                {
                    if (socket != INVALID_SOCKET) closesocket(socket);
                    m_error = "通信ソケットを作成できません。";
                    Stop();
                    return false;
                }
                if (host)
                {
                    // WindowsでSO_REUSEADDRを使うと、他のプロセスが同じ
                    // ポートを奪えるため、ホストの待受ポートは排他的にします。
                    const BOOL exclusive = TRUE;
                    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
                    if (bind(socket, reinterpret_cast<const sockaddr*>(&endpoint),
                            sizeof(endpoint)) != 0 || listen(socket, 8) != 0)
                    {
                        closesocket(socket);
                        m_error = "待受できません。ポートの使用状況とファイアウォールを確認してください。";
                        Stop();
                        return false;
                    }
                    m_listener = socket;
                    int length = sizeof(endpoint);
                    getsockname(socket, reinterpret_cast<sockaddr*>(&endpoint), &length);
                    std::array<char, INET_ADDRSTRLEN> ip{};
                    InetNtopA(AF_INET, &endpoint.sin_addr, ip.data(), ip.size());
                    m_address = std::string(ip.data()) + ":"
                        + std::to_string(ntohs(endpoint.sin_port));
                    m_pending.push_back({ TransportEventKind::Ready, 0, {} });
                }
                else
                {
                    Connection connection;
                    connection.socket = socket;
                    const BOOL noDelay = TRUE;
                    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                    if (connect(socket, reinterpret_cast<const sockaddr*>(&endpoint),
                            sizeof(endpoint)) != 0)
                    {
                        if (WSAGetLastError() != WSAEWOULDBLOCK)
                        {
                            closesocket(socket);
                            m_error = "ホストへの接続を開始できません。";
                            Stop();
                            return false;
                        }
                        connection.connecting = true;
                    }
                    else
                    {
                        m_pending.push_back({ TransportEventKind::Connected, 1, {} });
                    }
                    m_connections.emplace(1, std::move(connection));
                    m_address = address;
                }
                return true;
            }

            void Stop() noexcept override
            {
                for (const auto& [id, connection] : m_connections)
                {
                    static_cast<void>(id);
                    closesocket(connection.socket);
                }
                m_connections.clear();
                if (m_listener != INVALID_SOCKET) closesocket(m_listener);
                m_listener = INVALID_SOCKET;
                if (m_initialized) WSACleanup();
                m_initialized = false;
                m_pending.clear();
                m_address.clear();
                m_nextPeer = 1;
            }

            std::vector<TransportEvent> Poll(float) override
            {
                auto events = std::move(m_pending);
                m_pending.clear();
                if (m_listener != INVALID_SOCKET)
                {
                    // 未認証の接続も上限に含めます。acceptも1フレーム8回までです。
                    for (int count = 0; count < 8; ++count)
                    {
                        const SOCKET accepted = accept(m_listener, nullptr, nullptr);
                        if (accepted == INVALID_SOCKET) break;
                        if (m_connections.size() >= 8 || !Nonblocking(accepted))
                        {
                            closesocket(accepted);
                            continue;
                        }
                        const BOOL noDelay = TRUE;
                        setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                            reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                        Connection connection;
                        connection.socket = accepted;
                        const auto id = m_nextPeer++;
                        m_connections.emplace(id, std::move(connection));
                        events.push_back({ TransportEventKind::Connected, id, {} });
                    }
                }
                std::vector<TransportPeer> disconnected;
                for (auto& [id, connection] : m_connections)
                {
                    if (connection.connecting)
                    {
                        fd_set writable, failed;
                        FD_ZERO(&writable);
                        FD_ZERO(&failed);
                        FD_SET(connection.socket, &writable);
                        FD_SET(connection.socket, &failed);
                        timeval timeout{};
                        if (select(0, nullptr, &writable, &failed, &timeout) <= 0) continue;
                        int error{};
                        int length = sizeof(error);
                        if (getsockopt(connection.socket, SOL_SOCKET, SO_ERROR,
                                reinterpret_cast<char*>(&error), &length) != 0 || error != 0)
                        {
                            disconnected.push_back(id);
                            continue;
                        }
                        connection.connecting = false;
                        events.push_back({ TransportEventKind::Connected, id, {} });
                    }
                    bool alive = Receive(connection, id, events);
                    if (alive) alive = Flush(connection);
                    if (!alive) disconnected.push_back(id);
                }
                for (const auto id : disconnected)
                {
                    Close(id);
                    events.push_back({ TransportEventKind::Disconnected, id, {} });
                }
                return events;
            }

            bool Send(const TransportPeer peer, const std::string& packet) override
            {
                const auto found = m_connections.find(peer);
                if (found == m_connections.end() || packet.empty()
                    || packet.size() > NetworkPacketMaxBytes) return false;
                auto& connection = found->second;
                if (connection.queuedBytes + packet.size() + 4 > QueueLimit) return false;
                std::string framed(4, '\0');
                const auto length = static_cast<std::uint32_t>(packet.size());
                for (std::uint32_t byte = 0; byte < 4; ++byte)
                {
                    framed[byte] = static_cast<char>((length >> (24 - byte * 8)) & 255);
                }
                framed += packet;
                connection.queuedBytes += framed.size();
                connection.outgoing.push_back(std::move(framed));
                return true;
            }

            void Disconnect(const TransportPeer peer) override
            {
                if (m_connections.contains(peer))
                {
                    Close(peer);
                    m_pending.push_back({ TransportEventKind::Disconnected, peer, {} });
                }
            }
            std::string Address() const override { return m_address; }
            std::string Error() const override { return m_error; }

        private:
            static bool Receive(Connection& connection, const TransportPeer id,
                std::vector<TransportEvent>& events)
            {
                std::array<char, 4096> buffer{};
                std::size_t bytes{};
                std::size_t messages{};
                while (messages < 64)
                {
                    // 前フレームの64件上限で残した完全なpacketも先に処理します。
                    while (connection.incoming.size() >= 4 && messages < 64)
                    {
                        std::uint32_t length{};
                        for (std::size_t byte = 0; byte < 4; ++byte)
                        {
                            length = (length << 8)
                                | static_cast<unsigned char>(connection.incoming[byte]);
                        }
                        if (length == 0 || length > NetworkPacketMaxBytes) return false;
                        if (connection.incoming.size() < length + 4) break;
                        events.push_back({ TransportEventKind::Message, id,
                            std::string(connection.incoming.begin() + 4,
                                connection.incoming.begin() + 4 + length) });
                        connection.incoming.erase(connection.incoming.begin(),
                            connection.incoming.begin() + 4 + length);
                        ++messages;
                    }
                    if (bytes >= IoBudget || messages >= 64) return true;
                    const int received = recv(connection.socket, buffer.data(),
                        static_cast<int>(std::min(buffer.size(), IoBudget - bytes)), 0);
                    if (received == 0) return false;
                    if (received < 0) return WSAGetLastError() == WSAEWOULDBLOCK;
                    bytes += static_cast<std::size_t>(received);
                    connection.incoming.insert(connection.incoming.end(),
                        buffer.begin(), buffer.begin() + received);
                }
                return true;
            }

            static bool Flush(Connection& connection)
            {
                std::size_t bytes{};
                while (!connection.outgoing.empty() && bytes < IoBudget)
                {
                    const auto& packet = connection.outgoing.front();
                    const int sent = send(connection.socket, packet.data() + connection.offset,
                        static_cast<int>(std::min(packet.size() - connection.offset,
                            IoBudget - bytes)), 0);
                    if (sent < 0) return WSAGetLastError() == WSAEWOULDBLOCK;
                    if (sent == 0) return false;
                    const auto count = static_cast<std::size_t>(sent);
                    connection.offset += count;
                    connection.queuedBytes -= count;
                    bytes += count;
                    if (connection.offset == packet.size())
                    {
                        connection.outgoing.pop_front();
                        connection.offset = 0;
                    }
                }
                return true;
            }

            void Close(const TransportPeer id)
            {
                const auto found = m_connections.find(id);
                if (found == m_connections.end()) return;
                closesocket(found->second.socket);
                m_connections.erase(found);
            }
            bool m_initialized{};
            SOCKET m_listener{ INVALID_SOCKET };
            TransportPeer m_nextPeer{ 1 };
            std::map<TransportPeer, Connection> m_connections;
            std::vector<TransportEvent> m_pending;
            std::string m_address;
            std::string m_error;
        };
    }

    std::unique_ptr<INetworkTransport> CreateLanTransport()
    {
        return std::make_unique<LanTransport>();
    }
}
