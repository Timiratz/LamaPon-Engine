#include "LamaPon/Online/NetworkEndpoint.h"
#include "LamaPon/Online/NetworkRoomBrowser.h"
#include "LamaPon/Online/NetworkRoomAdvertiser.h"
#include "LamaPon/Online/NetworkCrypto.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>

namespace LamaPon
{
    namespace
    {
        using Json = nlohmann::json;
        using Clock = std::chrono::steady_clock;
        // UDP検索の上限バイト数
        constexpr std::size_t DiscoveryLimit = 1100;
        struct DiscoverySocket final
        {
            // 検索用UDPソケット
            SOCKET value{ INVALID_SOCKET };
            // Winsockの初期化済み状態
            bool initialized{};
            // 共有指定の非同期UDPソケットを開く(port: 待受ポート・0なら自動, shared: 待受ポートを共有するか)。
            bool Open(const std::uint16_t port, const bool shared)
            {
                Close();
                // Winsockの初期化情報
                WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
                initialized = true; value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (value == INVALID_SOCKET) { Close(); return false; }
                // ソケットオプションの有効値
                const BOOL enabled = TRUE;
                if (shared) setsockopt(value, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
                setsockopt(value, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
                // 非同期受信の有効値
                u_long nonblocking = 1;
                // UDPの待受アドレス
                sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
                if (ioctlsocket(value, FIONBIO, &nonblocking) != 0 || bind(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                { Close(); return false; }
                return true;
            }
            // ソケットとWinsockを解放する。
            void Close() noexcept
            {
                if (value != INVALID_SOCKET) closesocket(value); value = INVALID_SOCKET;
                if (initialized) WSACleanup(); initialized = false;
            }
            // 検索ソケットを閉じる。
            ~DiscoverySocket() { Close(); }
        };
        // プライベート・loopback・リンクローカルIPv4かを判定する(source: UDPの送信元)。
        bool LocalSource(const sockaddr_in& source)
        {
            // 送信元IPv4のホスト順表現
            const auto ip = ntohl(source.sin_addr.s_addr);
            return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 16) == 0xc0a8
                || (ip & 0xfff00000) == 0xac100000 || (ip >> 16) == 0xa9fe;
        }
        // 空でない64バイト以内の部屋名かを判定する(text: 部屋名)。
        bool RoomText(const std::string& text)
        {
            // 制御文字かを調べる(c: 部屋名の1文字)。
            return !text.empty() && text.size() <= 64 && std::ranges::none_of(text, [](const char c)
                { return static_cast<unsigned char>(c) < 32 || c == 127; });
        }
        // 深さ4以下の検索JSONを読み、不正な入力は例外にする(packet: 受信パケット)。
        Json Parse(const std::string& packet)
        {
            // 深さ4を超えたら拒否する(depth: JSONの入れ子の深さ)。
            return Json::parse(packet, [](const int depth, Json::parse_event_t, Json&)
                { if (depth > 4) throw std::invalid_argument("Discovery depth"); return true; });
        }
    }
    struct NetworkRoomBrowser::Implementation final
    {
        // 検索用UDPソケットの所有先
        DiscoverySocket socket;
        // 検索の照合条件と広告設定
        NetworkConfiguration configuration;
        // LANまたは同一PCの検索範囲
        NetworkDiscoveryScope scope{ NetworkDiscoveryScope::Lan };
        // 要求照合子と検索エラー
        std::string query, error;
        // 接続情報で識別する検索結果
        std::vector<NetworkRoom> rooms;
        // 各広告を最後に受信した時刻
        std::map<std::string, Clock::time_point> seen;
        // 次の検索要求までの残り秒数
        float untilRefresh{};
        // 検索照合子を更新して検索要求をブロードキャストする。
        void Query()
        {
            // 要求照合子用の乱数
            auto random = Detail::RandomNetworkKey(); query = Detail::Hex(std::span(random).first(16));
            // ブロードキャストする検索要求
            const std::string packet = Json{ { "op", "LamaPon.Search.1" }, { "query", query },
                { "game", configuration.gameId }, { "version", configuration.gameVersion }, { "scene", configuration.sceneId } }.dump();
            // 検索要求の送信先
            sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port = htons(configuration.discoveryPort);
            endpoint.sin_addr.s_addr = htonl(scope == NetworkDiscoveryScope::SameComputer ? 0x7fffffff : 0xffffffff);
            if (sendto(socket.value, packet.data(), static_cast<int>(packet.size()), 0,
                reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == SOCKET_ERROR) error = "LAN検索を送信できません。回線とファイアウォールを確認してください。";
            untilRefresh = 1;
        }
    };
    NetworkRoomBrowser::NetworkRoomBrowser() : m_impl(std::make_unique<Implementation>()) {}
    NetworkRoomBrowser::~NetworkRoomBrowser() = default;
    bool NetworkRoomBrowser::Start(NetworkConfiguration configuration, const NetworkDiscoveryScope scope)
    {
        Stop();
        try { ValidateNetworkConfiguration(configuration); }
        // 設定検証の例外(e: 検証エラー)。
        catch (const std::exception& e) { m_impl->error = e.what(); return false; }
        if (configuration.backend == NetworkBackend::EpicOnlineServices) { m_impl->error = "EOSの部屋はLAN検索に対応していません。"; return false; }
        m_impl->configuration = std::move(configuration); m_impl->scope = scope;
        if (!m_impl->socket.Open(0, false)) { m_impl->error = "LAN検索ソケットを作成できません。"; return false; }
        Refresh(); return true;
    }
    void NetworkRoomBrowser::Refresh()
    {
        if (!IsSearching()) return;
        try { m_impl->Query(); }
        catch (...) { m_impl->error = "LAN検索の要求を作成できません。"; }
    }
    void NetworkRoomBrowser::Update(const float seconds)
    {
        if (!IsSearching() || !std::isfinite(seconds) || seconds < 0) return;
        // 検索状態の参照
        auto& impl = *m_impl; impl.untilRefresh -= seconds;
        if (impl.untilRefresh <= 0) Refresh();
        // 1回の受信処理番号
        for (int count = 0; count < 64; ++count)
        {
            // 上限超過も判定する受信領域
            std::array<char, DiscoveryLimit + 1> buffer{};
            // UDPパケットの送信元
            sockaddr_in source{};
            // 送信元アドレス領域のサイズ
            int length = sizeof(source);
            // 受信したバイト数
            const int received = recvfrom(impl.socket.value, buffer.data(), static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &length);
            if (received < 0) { if (WSAGetLastError() == WSAEMSGSIZE) continue; break; }
            if (received == 0 || received > DiscoveryLimit || !LocalSource(source)) continue;
            if (impl.scope == NetworkDiscoveryScope::SameComputer && (ntohl(source.sin_addr.s_addr) >> 24) != 127) continue;
            try
            {
                // 受信した部屋広告のJSON
                const auto packet = Parse(std::string(buffer.data(), received));
                if (packet.at("op") != "LamaPon.Room.1" || packet.at("query") != impl.query
                    || packet.at("game") != impl.configuration.gameId || packet.at("version") != impl.configuration.gameVersion
                    || packet.at("scene") != impl.configuration.sceneId) continue;
                // 受信した部屋の掲載情報
                NetworkRoom room; room.name = packet.at("name").get<std::string>();
                room.gameId = impl.configuration.gameId; room.gameVersion = impl.configuration.gameVersion; room.sceneId = impl.configuration.sceneId;
                // 広告内のバックエンド名
                const auto backend = packet.at("backend").get<std::string>();
                if (backend != "Lan" && backend != "Direct") continue;
                room.backend = backend == "Lan" ? NetworkBackend::Lan : NetworkBackend::Direct;
                if (room.backend != impl.configuration.backend || !RoomText(room.name)) continue;
                // 参加者数と定員
                const auto players = packet.at("players").get<std::int64_t>(), capacity = packet.at("capacity").get<std::int64_t>();
                // 広告された待受ポート
                const auto port = packet.at("port").get<std::int64_t>();
                if (players < 1 || capacity < 2 || capacity > 4 || players > capacity || port < 1 || port > 65535) continue;
                room.players = static_cast<std::uint32_t>(players); room.capacity = static_cast<std::uint32_t>(capacity);
                // 送信元IPと広告の待受ポート
                Detail::NetworkEndpoint endpoint; endpoint.storage.ss_family = AF_INET;
                *reinterpret_cast<sockaddr_in*>(&endpoint.storage) = source;
                reinterpret_cast<sockaddr_in*>(&endpoint.storage)->sin_port = htons(static_cast<u_short>(port));
                room.connection = endpoint.Text();
                if (room.backend == NetworkBackend::Direct)
                {
                    // Direct接続の秘密鍵文字列
                    const auto key = packet.at("key").get<std::string>();
                    // 秘密鍵の形式確認用バイト列
                    Detail::NetworkKey bytes{};
                    if (!Detail::Unhex(key, bytes)) continue; SecureZeroMemory(bytes.data(), bytes.size());
                    room.connection = "LPD1|" + room.connection + "|" + key;
                }
                // 同じ接続情報の掲載済み部屋
                const auto existing = std::ranges::find(impl.rooms, room.connection, &NetworkRoom::connection);
                if (existing == impl.rooms.end()) { if (impl.rooms.size() >= 64) continue; impl.rooms.push_back(room); }
                else *existing = room;
                impl.seen[room.connection] = Clock::now();
            }
            // 未知・不正な広告は検索結果に入れません。
            catch (...) { }
        }
        // 広告の期限判定時刻
        const auto now = Clock::now();
        // 4秒を超えた広告を除く(room: 掲載済みの部屋)。
        std::erase_if(impl.rooms, [&impl, now](const NetworkRoom& room)
        {
            // 部屋の最終受信時刻の位置
            const auto found = impl.seen.find(room.connection);
            if (found != impl.seen.end() && now - found->second <= std::chrono::seconds(4)) return false;
            impl.seen.erase(room.connection); return true;
        });
    }
    void NetworkRoomBrowser::Stop()
    {
        m_impl->socket.Close(); m_impl->rooms.clear(); m_impl->seen.clear(); m_impl->error.clear(); m_impl->query.clear();
    }
    bool NetworkRoomBrowser::IsSearching() const noexcept { return m_impl->socket.value != INVALID_SOCKET; }
    const std::vector<NetworkRoom>& NetworkRoomBrowser::Rooms() const noexcept { return m_impl->rooms; }
    const std::string& NetworkRoomBrowser::LastError() const noexcept { return m_impl->error; }

    namespace Detail
    {
        struct NetworkRoomAdvertiser::Implementation final
        {
            // 検索用UDPソケットの所有先
            DiscoverySocket socket;
            // 検索の照合条件と広告設定
            NetworkConfiguration configuration;
            // 応答件数を数える期間の開始
            Clock::time_point window{ Clock::now() };
            // 現在の1秒間の応答試行数
            unsigned int replies{};
        };
        NetworkRoomAdvertiser::NetworkRoomAdvertiser() : m_impl(std::make_unique<Implementation>()) {}
        NetworkRoomAdvertiser::~NetworkRoomAdvertiser() = default;
        bool NetworkRoomAdvertiser::Start(const NetworkConfiguration& configuration)
        {
            Stop(); m_impl->configuration = configuration;
            return configuration.advertiseLan && configuration.backend != NetworkBackend::EpicOnlineServices
                && m_impl->socket.Open(configuration.discoveryPort, true);
        }
        void NetworkRoomAdvertiser::Stop() noexcept { m_impl->socket.Close(); }
        void NetworkRoomAdvertiser::Update(const NetworkSession& session)
        {
            // 広告状態の参照
            auto& impl = *m_impl;
            if (!session.IsHost() || impl.socket.value == INVALID_SOCKET) return;
            if (Clock::now() - impl.window >= std::chrono::seconds(1)) { impl.window = Clock::now(); impl.replies = 0; }
            // 1回の受信処理番号
            for (int count = 0; count < 16; ++count)
            {
                // 上限超過も判定する受信領域
                std::array<char, DiscoveryLimit + 1> buffer{};
                // UDPパケットの送信元
                sockaddr_in source{};
                // 送信元アドレス領域のサイズ
                int length = sizeof(source);
                // 受信したバイト数
                const int received = recvfrom(impl.socket.value, buffer.data(), static_cast<int>(buffer.size()), 0,
                    reinterpret_cast<sockaddr*>(&source), &length);
                if (received < 0) { if (WSAGetLastError() == WSAEMSGSIZE) continue; break; }
                if (received == 0 || received > DiscoveryLimit || impl.replies >= 64 || !LocalSource(source)) continue;
                try
                {
                    // 受信した部屋検索要求
                    const auto request = Parse(std::string(buffer.data(), received));
                    if (request.at("op") != "LamaPon.Search.1" || request.at("game") != impl.configuration.gameId
                        || request.at("version") != impl.configuration.gameVersion || request.at("scene") != impl.configuration.sceneId) continue;
                    // 応答に返す要求照合子
                    const auto query = request.at("query").get<std::string>();
                    // 要求照合子の形式確認領域
                    std::array<unsigned char, 16> queryBytes{};
                    if (!Unhex(query, queryBytes)) continue;
                    // 公開する待受アドレス
                    auto address = session.LocalAddress();
                    if (address.starts_with("LPD1|")) address = address.substr(5, address.find('|', 5) - 5);
                    // セッションの待受アドレス
                    NetworkEndpoint listen;
                    if (!NetworkEndpoint::Parse(address, 0, listen) || listen.Family() != AF_INET) continue;
                    // 127.0.0.1で待つホストは127/8の要求元だけに応答します。
                    if ((listen.Host() == "127.0.0.1") && (ntohl(source.sin_addr.s_addr) >> 24) != 127) continue;
                    // 検索要求への部屋情報応答
                    Json reply{ { "op", "LamaPon.Room.1" }, { "query", query }, { "game", impl.configuration.gameId },
                        { "version", impl.configuration.gameVersion }, { "scene", impl.configuration.sceneId },
                        { "name", impl.configuration.roomName }, { "port", listen.Port() },
                        { "players", session.Members().size() }, { "capacity", impl.configuration.maxPlayers },
                        { "backend", impl.configuration.backend == NetworkBackend::Direct ? "Direct" : "Lan" } };
                    if (impl.configuration.backend == NetworkBackend::Direct) reply["key"] = session.AccessKey();
                    // 送信する部屋広告のJSON
                    const auto packet = reply.dump(); if (packet.size() > DiscoveryLimit) continue;
                    static_cast<void>(sendto(impl.socket.value, packet.data(), static_cast<int>(packet.size()), 0,
                        reinterpret_cast<sockaddr*>(&source), sizeof(source))); ++impl.replies;
                }
                // 不正な検索要求に応答しません。
                catch (...) { }
            }
        }
    }
}
