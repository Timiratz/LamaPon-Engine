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
        constexpr std::size_t DiscoveryLimit = 1100;
        struct DiscoverySocket final
        {
            SOCKET value{ INVALID_SOCKET };
            bool initialized{};
            bool Open(const std::uint16_t port, const bool shared)
            {
                Close(); WSADATA data{};
                if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
                initialized = true; value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (value == INVALID_SOCKET) { Close(); return false; }
                const BOOL enabled = TRUE;
                if (shared) setsockopt(value, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
                setsockopt(value, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled), sizeof(enabled));
                u_long nonblocking = 1;
                sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
                if (ioctlsocket(value, FIONBIO, &nonblocking) != 0 || bind(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                { Close(); return false; }
                return true;
            }
            void Close() noexcept
            {
                if (value != INVALID_SOCKET) closesocket(value); value = INVALID_SOCKET;
                if (initialized) WSACleanup(); initialized = false;
            }
            ~DiscoverySocket() { Close(); }
        };
        bool LocalSource(const sockaddr_in& source)
        {
            const auto ip = ntohl(source.sin_addr.s_addr);
            return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 16) == 0xc0a8
                || (ip & 0xfff00000) == 0xac100000 || (ip >> 16) == 0xa9fe;
        }
        bool RoomText(const std::string& text)
        {
            return !text.empty() && text.size() <= 64 && std::ranges::none_of(text, [](const char c)
                { return static_cast<unsigned char>(c) < 32 || c == 127; });
        }
        Json Parse(const std::string& packet)
        {
            return Json::parse(packet, [](const int depth, Json::parse_event_t, Json&)
                { if (depth > 4) throw std::invalid_argument("Discovery depth"); return true; });
        }
    }
    struct NetworkRoomBrowser::Implementation final
    {
        DiscoverySocket socket;
        NetworkConfiguration configuration;
        NetworkDiscoveryScope scope{ NetworkDiscoveryScope::Lan };
        std::string query, error;
        std::vector<NetworkRoom> rooms;
        std::map<std::string, Clock::time_point> seen;
        float untilRefresh{};
        void Query()
        {
            auto random = Detail::RandomNetworkKey(); query = Detail::Hex(std::span(random).first(16));
            const std::string packet = Json{ { "op", "LamaPon.Search.1" }, { "query", query },
                { "game", configuration.gameId }, { "version", configuration.gameVersion }, { "scene", configuration.sceneId } }.dump();
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
        auto& impl = *m_impl; impl.untilRefresh -= seconds;
        if (impl.untilRefresh <= 0) Refresh();
        for (int count = 0; count < 64; ++count)
        {
            std::array<char, DiscoveryLimit + 1> buffer{}; sockaddr_in source{}; int length = sizeof(source);
            const int received = recvfrom(impl.socket.value, buffer.data(), static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &length);
            if (received < 0) { if (WSAGetLastError() == WSAEMSGSIZE) continue; break; }
            if (received == 0 || received > DiscoveryLimit || !LocalSource(source)) continue;
            if (impl.scope == NetworkDiscoveryScope::SameComputer && (ntohl(source.sin_addr.s_addr) >> 24) != 127) continue;
            try
            {
                const auto packet = Parse(std::string(buffer.data(), received));
                if (packet.at("op") != "LamaPon.Room.1" || packet.at("query") != impl.query
                    || packet.at("game") != impl.configuration.gameId || packet.at("version") != impl.configuration.gameVersion
                    || packet.at("scene") != impl.configuration.sceneId) continue;
                NetworkRoom room; room.name = packet.at("name").get<std::string>();
                room.gameId = impl.configuration.gameId; room.gameVersion = impl.configuration.gameVersion; room.sceneId = impl.configuration.sceneId;
                const auto backend = packet.at("backend").get<std::string>();
                if (backend != "Lan" && backend != "Direct") continue;
                room.backend = backend == "Lan" ? NetworkBackend::Lan : NetworkBackend::Direct;
                if (room.backend != impl.configuration.backend || !RoomText(room.name)) continue;
                const auto players = packet.at("players").get<std::int64_t>(), capacity = packet.at("capacity").get<std::int64_t>();
                const auto port = packet.at("port").get<std::int64_t>();
                if (players < 1 || capacity < 2 || capacity > 4 || players > capacity || port < 1 || port > 65535) continue;
                room.players = static_cast<std::uint32_t>(players); room.capacity = static_cast<std::uint32_t>(capacity);
                Detail::NetworkEndpoint endpoint; endpoint.storage.ss_family = AF_INET;
                *reinterpret_cast<sockaddr_in*>(&endpoint.storage) = source;
                reinterpret_cast<sockaddr_in*>(&endpoint.storage)->sin_port = htons(static_cast<u_short>(port));
                room.connection = endpoint.Text();
                if (room.backend == NetworkBackend::Direct)
                {
                    const auto key = packet.at("key").get<std::string>(); Detail::NetworkKey bytes{};
                    if (!Detail::Unhex(key, bytes)) continue; SecureZeroMemory(bytes.data(), bytes.size());
                    room.connection = "LPD1|" + room.connection + "|" + key;
                }
                const auto existing = std::ranges::find(impl.rooms, room.connection, &NetworkRoom::connection);
                if (existing == impl.rooms.end()) { if (impl.rooms.size() >= 64) continue; impl.rooms.push_back(room); }
                else *existing = room;
                impl.seen[room.connection] = Clock::now();
            }
            catch (...) { /* 未知・不正な広告は検索結果に入れません。 */ }
        }
        const auto now = Clock::now();
        std::erase_if(impl.rooms, [&impl, now](const NetworkRoom& room)
        {
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
            DiscoverySocket socket;
            NetworkConfiguration configuration;
            Clock::time_point window{ Clock::now() };
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
            auto& impl = *m_impl;
            if (!session.IsHost() || impl.socket.value == INVALID_SOCKET) return;
            if (Clock::now() - impl.window >= std::chrono::seconds(1)) { impl.window = Clock::now(); impl.replies = 0; }
            for (int count = 0; count < 16; ++count)
            {
                std::array<char, DiscoveryLimit + 1> buffer{}; sockaddr_in source{}; int length = sizeof(source);
                const int received = recvfrom(impl.socket.value, buffer.data(), static_cast<int>(buffer.size()), 0,
                    reinterpret_cast<sockaddr*>(&source), &length);
                if (received < 0) { if (WSAGetLastError() == WSAEMSGSIZE) continue; break; }
                if (received == 0 || received > DiscoveryLimit || impl.replies >= 64 || !LocalSource(source)) continue;
                try
                {
                    const auto request = Parse(std::string(buffer.data(), received));
                    if (request.at("op") != "LamaPon.Search.1" || request.at("game") != impl.configuration.gameId
                        || request.at("version") != impl.configuration.gameVersion || request.at("scene") != impl.configuration.sceneId) continue;
                    const auto query = request.at("query").get<std::string>(); std::array<unsigned char, 16> queryBytes{};
                    if (!Unhex(query, queryBytes)) continue;
                    auto address = session.LocalAddress();
                    if (address.starts_with("LPD1|")) address = address.substr(5, address.find('|', 5) - 5);
                    NetworkEndpoint listen;
                    if (!NetworkEndpoint::Parse(address, 0, listen) || listen.Family() != AF_INET) continue;
                    // loopbackに限定されたホストはLANへ公開しません。
                    if ((listen.Host() == "127.0.0.1") && (ntohl(source.sin_addr.s_addr) >> 24) != 127) continue;
                    Json reply{ { "op", "LamaPon.Room.1" }, { "query", query }, { "game", impl.configuration.gameId },
                        { "version", impl.configuration.gameVersion }, { "scene", impl.configuration.sceneId },
                        { "name", impl.configuration.roomName }, { "port", listen.Port() },
                        { "players", session.Members().size() }, { "capacity", impl.configuration.maxPlayers },
                        { "backend", impl.configuration.backend == NetworkBackend::Direct ? "Direct" : "Lan" } };
                    if (impl.configuration.backend == NetworkBackend::Direct) reply["key"] = session.AccessKey();
                    const auto packet = reply.dump(); if (packet.size() > DiscoveryLimit) continue;
                    static_cast<void>(sendto(impl.socket.value, packet.data(), static_cast<int>(packet.size()), 0,
                        reinterpret_cast<sockaddr*>(&source), sizeof(source))); ++impl.replies;
                }
                catch (...) { /* 不正な検索要求に応答しません。 */ }
            }
        }
    }
}
