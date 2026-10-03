#include <WinSock2.h>
#include <WS2tcpip.h>

#include "LamaPon/Online/NetworkTransport.h"
#include "LamaPon/Online/NetworkEndpoint.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <deque>
#include <map>

namespace LamaPon::Detail
{
    namespace
    {
        // 接続ごとの送信キュー上限B
        constexpr std::size_t QueueLimit = 256 * 1024;
        // 接続ごとの一回の送受信上限B
        constexpr std::size_t IoBudget = 32 * 1024;

        // 非同期ソケットへ切り替える(socket: 対象ソケット)。
        bool Nonblocking(const SOCKET socket)
        {
            // 非同期ソケットの有効値
            u_long enabled = 1;
            return ioctlsocket(socket, FIONBIO, &enabled) == 0;
        }

        struct Connection final
        {
            // TCPソケットのハンドル
            SOCKET socket{ INVALID_SOCKET };
            // 接続完了待ちの状態
            bool connecting{};
            // 未処理の受信バイト列
            std::vector<char> incoming;
            // 長さヘッダー付きの送信キュー
            std::deque<std::string> outgoing;
            // 先頭パケットの送信済み位置
            std::size_t offset{};
            // 未送信の総バイト数
            std::size_t queuedBytes{};
        };

        class LanTransport final : public INetworkTransport
        {
        public:
            // TCP接続とWinsockを解放する。
            ~LanTransport() override { Stop(); }

            // 非同期TCPの待受または接続を始める(configuration: 通信設定, host: 待受側か, address: 数値接続先)。
            bool Start(const NetworkConfiguration& configuration, const bool host,
                const std::string& address, const std::string&) override
            {
                Stop();
                m_error.clear();
                // 解析したTCP接続先
                NetworkEndpoint endpoint{};
                if (!NetworkEndpoint::Parse(address, configuration.port, endpoint)
                    || (!host && (endpoint.Port() == 0 || !endpoint.Unicast())))
                {
                    m_error = "数値IPv4:port または [IPv6]:port を指定してください。";
                    return false;
                }
                // Winsockの初期化情報
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
                {
                    m_error = "Windowsのネットワークを初期化できません。";
                    return false;
                }
                m_initialized = true;
                // TCPソケットのハンドル
                const SOCKET socket = ::socket(endpoint.Family(), SOCK_STREAM, IPPROTO_TCP);
                if (socket == INVALID_SOCKET || !Nonblocking(socket))
                {
                    if (socket != INVALID_SOCKET) closesocket(socket);
                    m_error = "通信ソケットを作成できません。";
                    Stop();
                    return false;
                }
                // IPv6の待受はIPv6専用とし、招待先もIPv4と区別します。
                if (endpoint.Family() == AF_INET6)
                {
                    // IPv6専用指定の有効値
                    const DWORD onlyV6 = 1;
                    setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY,
                        reinterpret_cast<const char*>(&onlyV6), sizeof(onlyV6));
                }
                m_packetLimit = NetworkPacketMaxBytes
                    + (configuration.backend == NetworkBackend::Direct ? 128 : 0);
                if (host)
                {
                    // 同じポートを別プロセスが奪わないよう、ホストの待受を排他的にします。
                    // 待受ポートの排他指定
                    const BOOL exclusive = TRUE;
                    setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                        reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
                    if (bind(socket, endpoint.Address(), endpoint.size) != 0 || listen(socket, 8) != 0)
                    {
                        closesocket(socket);
                        m_error = "待受できません。ポートの使用状況とファイアウォールを確認してください。";
                        Stop();
                        return false;
                    }
                    m_listener = socket;
                    // アドレスサイズまたは内容長
                    int length = endpoint.size;
                    getsockname(socket, endpoint.Address(), &length);
                    m_address = endpoint.Text();
                    m_pending.push_back({ TransportEventKind::Ready, 0, {} });
                }
                else
                {
                    // TCP接続の状態参照
                    Connection connection;
                    connection.socket = socket;
                    // Nagle無効化の有効値
                    const BOOL noDelay = TRUE;
                    setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                    if (connect(socket, endpoint.Address(), endpoint.size) != 0)
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

            // 全ソケットとイベントを破棄する。
            void Stop() noexcept override
            {
                // 接続IDとTCP接続
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

            // 受け付けと接続ごとの送受信を上限内で進め、イベントを取り出す。
            std::vector<TransportEvent> Poll(float) override
            {
                // 今回取り出す通信イベント
                auto events = std::move(m_pending);
                m_pending.clear();
                if (m_listener != INVALID_SOCKET)
                {
                    // 未認証を含め接続を8件に制限し、acceptは1回のPollで最大8回に制限します。
                    // 処理回数または送信バイト数
                    for (int count = 0; count < 8; ++count)
                    {
                        // 新たに受け付けたソケット
                        const SOCKET accepted = accept(m_listener, nullptr, nullptr);
                        if (accepted == INVALID_SOCKET) break;
                        if (m_connections.size() >= 8 || !Nonblocking(accepted))
                        {
                            closesocket(accepted);
                            continue;
                        }
                        // Nagle無効化の有効値
                        const BOOL noDelay = TRUE;
                        setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                            reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
                        // TCP接続の状態参照
                        Connection connection;
                        connection.socket = accepted;
                        // 接続ID
                        const auto id = m_nextPeer++;
                        m_connections.emplace(id, std::move(connection));
                        events.push_back({ TransportEventKind::Connected, id, {} });
                    }
                }
                // 切断する接続IDの一覧
                std::vector<TransportPeer> disconnected;
                // 接続IDとTCP接続
                for (auto& [id, connection] : m_connections)
                {
                    if (connection.connecting)
                    {
                        // 接続完了と接続失敗の集合
                        fd_set writable, failed;
                        FD_ZERO(&writable);
                        FD_ZERO(&failed);
                        FD_SET(connection.socket, &writable);
                        FD_SET(connection.socket, &failed);
                        // 待機しない接続完了確認
                        timeval timeout{};
                        if (select(0, nullptr, &writable, &failed, &timeout) <= 0) continue;
                        // ソケットの接続エラー
                        int error{};
                        // アドレスサイズまたは内容長
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
                    // 接続を継続できるか
                    bool alive = Receive(connection, id, events);
                    if (alive) alive = Flush(connection);
                    if (!alive) disconnected.push_back(id);
                }
                // 接続ID
                for (const auto id : disconnected)
                {
                    Close(id);
                    events.push_back({ TransportEventKind::Disconnected, id, {} });
                }
                return events;
            }

            // 長さヘッダー付きパケットを送信キューへ積む(peer: 宛先接続ID, packet: 送信内容)。
            bool Send(const TransportPeer peer, const std::string& packet) override
            {
                // 対象のTCP接続の位置
                const auto found = m_connections.find(peer);
                if (found == m_connections.end() || packet.empty()
                    || packet.size() > m_packetLimit) return false;
                // TCP接続の状態参照
                auto& connection = found->second;
                if (connection.queuedBytes + packet.size() + 4 > QueueLimit) return false;
                // 長さヘッダー付き送信パケット
                std::string framed(4, '\0');
                // アドレスサイズまたは内容長
                const auto length = static_cast<std::uint32_t>(packet.size());
                // 32ビット長のバイト位置
                for (std::uint32_t byte = 0; byte < 4; ++byte)
                {
                    framed[byte] = static_cast<char>((length >> (24 - byte * 8)) & 255);
                }
                framed += packet;
                connection.queuedBytes += framed.size();
                connection.outgoing.push_back(std::move(framed));
                return true;
            }

            // 切断して次のPollに切断イベントを残す(peer: 切断する接続ID)。
            void Disconnect(const TransportPeer peer) override
            {
                if (m_connections.contains(peer))
                {
                    Close(peer);
                    m_pending.push_back({ TransportEventKind::Disconnected, peer, {} });
                }
            }
            // 公開待受先または接続先を返す。
            std::string Address() const override { return m_address; }
            // 直近のTCP通信エラーを返す。
            std::string Error() const override { return m_error; }

        private:
            // 接続ごとに最大64件・32KiBまで受信処理する(connection: TCP接続の状態, id: 接続ID, events: 受信イベントの出力先)。
            bool Receive(Connection& connection, const TransportPeer id,
                std::vector<TransportEvent>& events)
            {
                // 今回の受信領域
                std::array<char, 4096> buffer{};
                // 今回処理したバイト数
                std::size_t bytes{};
                // 今回処理したパケット数
                std::size_t messages{};
                while (messages < 64)
                {
                    // 前フレームの64件上限で残した完全なpacketも先に処理します。
                    while (connection.incoming.size() >= 4 && messages < 64)
                    {
                        // アドレスサイズまたは内容長
                        std::uint32_t length{};
                        // 32ビット長のバイト位置
                        for (std::size_t byte = 0; byte < 4; ++byte)
                        {
                            length = (length << 8)
                                | static_cast<unsigned char>(connection.incoming[byte]);
                        }
                        if (length == 0 || length > m_packetLimit) return false;
                        if (connection.incoming.size() < length + 4) break;
                        events.push_back({ TransportEventKind::Message, id,
                            std::string(connection.incoming.begin() + 4,
                                connection.incoming.begin() + 4 + length) });
                        connection.incoming.erase(connection.incoming.begin(),
                            connection.incoming.begin() + 4 + length);
                        ++messages;
                    }
                    if (bytes >= IoBudget || messages >= 64) return true;
                    // 受信バイト数またはエラー
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

            // 接続ごとに最大32KiBまで送信キューを処理する(connection: TCP接続の状態)。
            static bool Flush(Connection& connection)
            {
                // 今回処理したバイト数
                std::size_t bytes{};
                while (!connection.outgoing.empty() && bytes < IoBudget)
                {
                    // 先頭の未送信パケット
                    const auto& packet = connection.outgoing.front();
                    // 送信バイト数またはエラー
                    const int sent = send(connection.socket, packet.data() + connection.offset,
                        static_cast<int>(std::min(packet.size() - connection.offset,
                            IoBudget - bytes)), 0);
                    if (sent < 0) return WSAGetLastError() == WSAEWOULDBLOCK;
                    if (sent == 0) return false;
                    // 処理回数または送信バイト数
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

            // 指定接続のソケットを閉じて登録を消す(id: 接続ID)。
            void Close(const TransportPeer id)
            {
                // 対象のTCP接続の位置
                const auto found = m_connections.find(id);
                if (found == m_connections.end()) return;
                closesocket(found->second.socket);
                m_connections.erase(found);
            }
            // 暗号化分を含む内容長の上限
            std::size_t m_packetLimit{ NetworkPacketMaxBytes };
            // Winsockの初期化済み状態
            bool m_initialized{};
            // ホストの待受ソケット
            SOCKET m_listener{ INVALID_SOCKET };
            // 次に割り当てる接続ID
            TransportPeer m_nextPeer{ 1 };
            // 接続ID別のTCP接続状態
            std::map<TransportPeer, Connection> m_connections;
            // 次に返す通信イベント
            std::vector<TransportEvent> m_pending;
            // ホスト待受先または接続先
            std::string m_address;
            // 直近の通信エラー
            std::string m_error;
        };
    }

    // TCPによるLAN通信実装を所有権付きで作る。
    std::unique_ptr<INetworkTransport> CreateLanTransport()
    {
        return std::make_unique<LanTransport>();
    }
}
