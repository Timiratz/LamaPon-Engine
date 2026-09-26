#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Online/NetworkTransport.h"
#include "LamaPon/Online/NetworkRoomAdvertiser.h"
#include "LamaPon/Online/NetworkRoomDirectory.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <set>

namespace LamaPon
{
    namespace
    {
        using Json = nlohmann::json;
        NetworkSession* activeSession{};

        bool Key(const std::string_view value, const bool empty = false)
        {
            if (value.empty()) return empty;
            return value.size() <= 64 && std::ranges::all_of(value, [](const char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            });
        }

        bool Text(const std::string& value, const std::size_t limit)
        {
            if (value.size() > limit || std::ranges::any_of(value, [](const char c)
            {
                const auto byte = static_cast<unsigned char>(c);
                return byte < 32 || byte == 127;
            })) return false;
            try { static_cast<void>(Json(value).dump()); return true; }
            catch (const std::exception&) { return false; }
        }

        bool ValidObject(const NetworkObjectState& object)
        {
            if (object.id == 0 || object.owner == 0 || object.data.size() > 256
                || !Key(object.sceneKey, true) || !Key(object.prefabKey, true)
                || (object.sceneKey.empty() == object.prefabKey.empty())) return false;
            const auto valid = [](const float value)
            {
                return std::isfinite(value) && std::abs(value) <= 1.0e6f;
            };
            if (!std::ranges::all_of(object.transform.position, valid)
                || !std::ranges::all_of(object.transform.scale, valid)
                || !std::ranges::all_of(object.transform.rotation, valid)) return false;
            float length{};
            for (const auto value : object.transform.rotation) length += value * value;
            return length > 0.5f && length < 1.5f;
        }

        std::uint32_t ReadId(const Json& value)
        {
            if (!value.is_number_integer() || value.get<std::int64_t>() < 0
                || value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("Integer range");
            return value.get<std::uint32_t>();
        }

        Json ObjectPacket(const NetworkObjectState& object)
        {
            return Json{ { "op", "object" }, { "id", object.id }, { "owner", object.owner },
                { "scene", object.sceneKey }, { "prefab", object.prefabKey },
                { "p", object.transform.position }, { "r", object.transform.rotation },
                { "s", object.transform.scale }, { "enabled", object.enabled },
                { "data", object.data } };
        }

        NetworkObjectState ReadObject(const Json& packet)
        {
            NetworkObjectState object;
            object.id = ReadId(packet.at("id"));
            object.owner = ReadId(packet.at("owner"));
            object.sceneKey = packet.at("scene").get<std::string>();
            object.prefabKey = packet.at("prefab").get<std::string>();
            if (packet.at("p").size() != 3 || packet.at("r").size() != 4
                || packet.at("s").size() != 3) throw std::invalid_argument("Transform size");
            object.transform.position = packet.at("p").get<std::array<float, 3>>();
            object.transform.rotation = packet.at("r").get<std::array<float, 4>>();
            object.transform.scale = packet.at("s").get<std::array<float, 3>>();
            object.enabled = packet.at("enabled").get<bool>();
            object.data = packet.at("data").get<std::string>();
            if (!ValidObject(object)) throw std::invalid_argument("Object value");
            return object;
        }
    }

    void ValidateNetworkConfiguration(const NetworkConfiguration& configuration)
    {
        if (!Key(configuration.gameId) || !Key(configuration.gameVersion)
            || !Key(configuration.sceneId) || configuration.maxPlayers < 2
            || configuration.maxPlayers > 4 || configuration.tickRate < 1
            || configuration.tickRate > 60 || !std::isfinite(configuration.timeoutSeconds)
            || configuration.timeoutSeconds < 5 || configuration.timeoutSeconds > 120
            || (configuration.backend != NetworkBackend::Lan
                && configuration.backend != NetworkBackend::EpicOnlineServices
                && configuration.backend != NetworkBackend::Direct)
            || (configuration.syncMode != NetworkSyncMode::Continuous && configuration.syncMode != NetworkSyncMode::OnChange)
            || configuration.discoveryPort == 0 || configuration.roomName.empty() || !Text(configuration.roomName, 64))
        {
            throw std::invalid_argument("P2P設定: IDは1〜64文字の英数字・._-、人数は2〜4、送信頻度は1〜60Hz、タイムアウトは5〜120秒です。");
        }
        if (configuration.backend == NetworkBackend::EpicOnlineServices
            && (!Key(configuration.eosProductId) || !Key(configuration.eosSandboxId)
                || !Key(configuration.eosDeploymentId) || !Key(configuration.eosClientId)
                || !Key(configuration.eosClientSecretEnvironment)))
        {
            throw std::invalid_argument("EOSの製品・Sandbox・Deployment・Client IDと資格情報の環境変数名が必要です。");
        }
        if (configuration.prefabs.size() > 64) throw std::invalid_argument("同期Prefabは64個まで登録できます。");
        std::vector<std::string> keys;
        for (const auto& prefab : configuration.prefabs)
        {
            // Windowsのドライブ相対パス・UNC・代替データストリームも拒否します。
            const auto& path = prefab.assetPath;
            if (!Key(prefab.key) || std::ranges::find(keys, prefab.key) != keys.end()
                || path.empty() || path.size() > 512 || !Text(path, 512)
                || path.front() == '/' || path.front() == '\\' || path.find(':') != std::string::npos)
                throw std::invalid_argument("同期Prefabには一意なキーとassetsからの相対パスを指定してください。");
            std::string normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            std::size_t start{};
            while (start <= normalized.size())
            {
                const auto end = normalized.find('/', start);
                const auto part = normalized.substr(start, end == std::string::npos ? end : end - start);
                if (part == ".." || part.empty() || part.back() == '.' || part.back() == ' ')
                    throw std::invalid_argument("同期Prefabのパスには親フォルダー参照を指定できません。");
                if (end == std::string::npos) break;
                start = end + 1;
            }
            keys.push_back(prefab.key);
        }
    }

    struct NetworkSession::Implementation final
    {
        struct Peer final
        {
            NetworkPeerId id{};
            float idle{};
            float age{};
            float tokens{ 256 };
        };
        NetworkConfiguration configuration;
        std::unique_ptr<Detail::INetworkTransport> transport;
        Detail::NetworkRoomAdvertiser advertiser;
        std::set<NetworkObjectId> dirty;
        std::string sessionState;
        std::uint32_t stateRevision{};
        NetworkState state{ NetworkState::Stopped };
        std::uint64_t generation{};
        bool host{};
        NetworkPeerId local{};
        NetworkPeerId nextPeer{ 2 };
        NetworkObjectId nextObject{ 1 };
        std::string name;
        std::string error;
        std::vector<NetworkMember> members;
        std::vector<NetworkObjectState> objects;
        std::map<Detail::TransportPeer, Peer> peers;
        std::deque<NetworkEvent> events;
        NetworkStatistics statistics;
        float age{};
        float tick{};
        float heartbeat{};
        std::uint32_t pingSequence{};
        std::uint32_t pendingPing{};
        float pingAge{};

        bool Event(NetworkEvent event)
        {
            if (events.size() >= Detail::NetworkEventLimit) return false;
            events.push_back(std::move(event));
            return true;
        }

        void Fail(std::string message)
        {
            ++generation;
            if (transport) transport->Stop();
            advertiser.Stop(); sessionState.clear(); stateRevision = 0; dirty.clear();
            peers.clear();
            members.clear();
            objects.clear();
            local = 0;
            host = false;
            state = NetworkState::Error;
            error = std::move(message);
            events.clear();
            Event({ NetworkEventKind::Error, 0, 0, {}, error });
        }

        bool Send(const Detail::TransportPeer peer, const Json& packet)
        {
            const auto data = packet.dump();
            if (data.size() > Detail::NetworkPacketMaxBytes) return false;
            if (!transport || !transport->Send(peer, data))
            {
                if (transport) transport->Disconnect(peer);
                return false;
            }
            statistics.sentBytes += data.size();
            return true;
        }

        void Broadcast(const Json& packet)
        {
            for (const auto& [peer, value] : peers)
            {
                if (value.id != 0) Send(peer, packet);
            }
        }

        void SendMembers()
        {
            Json list = Json::array();
            for (const auto& member : members)
                list.push_back({ { "id", member.id }, { "name", member.name } });
            Broadcast({ { "op", "members" }, { "members", list } });
        }

        void Reject(const Detail::TransportPeer peer)
        {
            ++statistics.rejectedMessages;
            if (host) { transport->Disconnect(peer); Lost(peer); }
            else Fail("ホストから無効な通信データを受信しました。");
        }

        bool Member(const NetworkPeerId id) const
        {
            return std::ranges::any_of(members, [id](const auto& member) { return member.id == id; });
        }

        void Lost(const Detail::TransportPeer peer)
        {
            const auto found = peers.find(peer);
            if (found == peers.end()) return;
            const auto id = found->second.id;
            peers.erase(found);
            if (!host)
            {
                Fail("ホストとの接続が終了しました。");
                return;
            }
            if (id == 0) return;
            std::erase_if(members, [id](const auto& member) { return member.id == id; });
            // 退出したプレイヤーの所有物はホストが削除し、全参加者に通知します。
            std::vector<NetworkObjectId> removed;
            for (const auto& object : objects) if (object.owner == id) removed.push_back(object.id);
            for (const auto object : removed)
            {
                std::erase_if(objects, [object](const auto& value) { return value.id == object; });
                Broadcast({ { "op", "despawn" }, { "id", object } });
            }
            Event({ NetworkEventKind::Left, id, 0, {}, {} });
            SendMembers();
        }

        void Message(const Detail::TransportPeer peer, const std::string& data)
        {
            auto found = peers.find(peer);
            if (found == peers.end()) return;
            statistics.receivedBytes += data.size();
            if (data.size() > Detail::NetworkPacketMaxBytes || found->second.tokens < 1)
            {
                Reject(peer);
                return;
            }
            --found->second.tokens;
            try
            {
                const auto packet = Json::parse(data, [](const int depth, Json::parse_event_t, Json&)
                {
                    if (depth > 8) throw std::invalid_argument("JSON depth");
                    return true;
                });
                const auto op = packet.at("op").get<std::string>();
                if (op == "hello" && host && found->second.id == 0)
                {
                    const auto incomingName = packet.at("name").get<std::string>();
                    if (ReadId(packet.at("protocol")) != 2 || packet.at("game") != configuration.gameId
                        || packet.at("version") != configuration.gameVersion
                        || packet.at("scene") != configuration.sceneId
                        || !Text(incomingName, 32) || incomingName.empty()
                        || members.size() >= configuration.maxPlayers
                        || nextPeer == std::numeric_limits<NetworkPeerId>::max())
                    {
                        Reject(peer);
                        return;
                    }
                    const auto id = nextPeer++;
                    found->second.id = id;
                    members.push_back({ id, incomingName });
                    Send(peer, { { "op", "welcome" }, { "protocol", 2 }, { "peer", id },
                        { "game", configuration.gameId }, { "version", configuration.gameVersion },
                        { "scene", configuration.sceneId } });
                    SendMembers();
                    for (const auto& object : objects) Send(peer, ObjectPacket(object));
                    Send(peer, { { "op", "state" }, { "revision", stateRevision }, { "data", sessionState } });
                    Send(peer, { { "op", "ready" } });
                    Event({ NetworkEventKind::Joined, id, 0, incomingName, {} });
                }
                else if (op == "welcome" && !host && local == 0 && state == NetworkState::Connecting)
                {
                    const auto id = ReadId(packet.at("peer"));
                    if (ReadId(packet.at("protocol")) != 2 || packet.at("game") != configuration.gameId
                        || packet.at("version") != configuration.gameVersion
                        || packet.at("scene") != configuration.sceneId || id < 2) throw std::invalid_argument("Welcome");
                    local = id;
                    found->second.id = 1;
                }
                else if (found->second.id == 0) throw std::invalid_argument("Handshake");
                else if (op == "members" && !host)
                {
                    const auto& list = packet.at("members");
                    if (!list.is_array() || list.size() < 2 || list.size() > configuration.maxPlayers)
                        throw std::invalid_argument("Members");
                    std::vector<NetworkMember> updated;
                    for (const auto& value : list)
                    {
                        const auto id = ReadId(value.at("id"));
                        const auto memberName = value.at("name").get<std::string>();
                        if (id == 0 || !Text(memberName, 32) || memberName.empty()
                            || std::ranges::any_of(updated, [id](const auto& item) { return item.id == id; }))
                            throw std::invalid_argument("Member");
                        updated.push_back({ id, memberName });
                    }
                    if (!std::ranges::any_of(updated, [](const auto& member) { return member.id == 1; })
                        || !std::ranges::any_of(updated, [this](const auto& member) { return member.id == local; }))
                        throw std::invalid_argument("Local member");
                    for (const auto& member : updated)
                        if (!Member(member.id)) Event({ NetworkEventKind::Joined, member.id, 0, member.name, {} });
                    for (const auto& member : members)
                        if (std::ranges::none_of(updated, [&member](const auto& value) { return value.id == member.id; }))
                            Event({ NetworkEventKind::Left, member.id, 0, member.name, {} });
                    members = std::move(updated);
                }
                else if (op == "object" && !host)
                {
                    auto object = ReadObject(packet);
                    if (!Member(object.owner)) throw std::invalid_argument("Owner");
                    const auto existing = std::ranges::find(objects, object.id, &NetworkObjectState::id);
                    if (existing == objects.end())
                    {
                        if (objects.size() >= Detail::NetworkObjectLimit
                            || (!object.sceneKey.empty() && std::ranges::any_of(objects, [&object](const auto& value)
                                { return value.sceneKey == object.sceneKey; }))) throw std::invalid_argument("Object limit/key");
                        objects.push_back(std::move(object));
                    }
                    else
                    {
                        if (existing->sceneKey != object.sceneKey || existing->prefabKey != object.prefabKey)
                            throw std::invalid_argument("Object identity");
                        *existing = std::move(object);
                    }
                }
                else if (op == "ready" && !host && state == NetworkState::Connecting && local != 0)
                {
                    state = NetworkState::Connected;
                    Event({ NetworkEventKind::Started, local, 0, {}, {} });
                }
                else if (op == "despawn" && !host)
                {
                    const auto id = ReadId(packet.at("id"));
                    std::erase_if(objects, [id](const auto& value) { return value.id == id; });
                }
                else if (op == "input" && host)
                {
                    const auto id = ReadId(packet.at("id"));
                    const auto object = std::ranges::find(objects, id, &NetworkObjectState::id);
                    const auto action = packet.at("name").get<std::string>();
                    const auto payload = packet.at("data").get<std::string>();
                    if (object == objects.end() || object->owner != found->second.id
                        || !Key(action) || payload.size() > 256
                        || !Event({ NetworkEventKind::Input, found->second.id, id, action, payload }))
                        throw std::invalid_argument("Input owner/queue");
                }
                else if (op == "command" && host)
                {
                    const auto action = packet.at("name").get<std::string>();
                    const auto payload = packet.at("data").get<std::string>();
                    if (!Key(action) || payload.size() > 256
                        || !Event({ NetworkEventKind::Command, found->second.id, 0, action, payload }))
                        throw std::invalid_argument("Command queue");
                }
                else if (op == "state" && !host)
                {
                    const auto revision = ReadId(packet.at("revision"));
                    const auto payload = packet.at("data").get<std::string>();
                    if (payload.size() > 256 || revision < stateRevision) throw std::invalid_argument("State revision");
                    if (revision > stateRevision || sessionState != payload)
                    {
                        if (!Event({ NetworkEventKind::SessionState, 1, 0, {}, payload })) throw std::invalid_argument("State queue");
                        sessionState = payload; stateRevision = revision;
                    }
                }
                else if (op == "event" && !host && state == NetworkState::Connected)
                {
                    const auto action = packet.at("name").get<std::string>();
                    const auto payload = packet.at("data").get<std::string>();
                    if (!Key(action) || payload.size() > 256
                        || !Event({ NetworkEventKind::GameEvent, 1, 0, action, payload }))
                        throw std::invalid_argument("Event queue");
                }
                else if (op == "ping") Send(peer, { { "op", "pong" }, { "id", ReadId(packet.at("id")) } });
                else if (op == "pong")
                {
                    const auto sequence = ReadId(packet.at("id"));
                    if (!host && pendingPing != 0 && sequence == pendingPing)
                    {
                        statistics.roundTripMilliseconds = (age - pingAge) * 1000;
                        pendingPing = 0;
                    }
                }
                else throw std::invalid_argument("Operation");
                found = peers.find(peer);
                if (found != peers.end()) found->second.idle = 0;
            }
            catch (const std::exception&)
            {
                Reject(peer);
            }
        }
    };

    NetworkSession::NetworkSession() : m_impl(std::make_unique<Implementation>()) {}
    NetworkSession::~NetworkSession()
    {
        if (activeSession == this) activeSession = nullptr;
        if (m_impl->transport) m_impl->transport->Stop();
    }

    bool NetworkSession::Configure(NetworkConfiguration configuration)
    {
        if (m_impl->state != NetworkState::Stopped && m_impl->state != NetworkState::Error)
        {
            m_impl->error = "接続を終了してからP2P設定を変更してください。";
            return false;
        }
        try { ValidateNetworkConfiguration(configuration); }
        catch (const std::exception& error) { m_impl->error = error.what(); return false; }
        m_impl->configuration = std::move(configuration);
        m_impl->error.clear();
        return true;
    }

    bool NetworkSession::Host(std::string name, std::string address)
    {
        if (m_impl->state != NetworkState::Stopped && m_impl->state != NetworkState::Error) return false;
        if (name.empty() || !Text(name, 32)) { m_impl->error = "表示名は1〜32バイトです。"; return false; }
        Stop();
        auto& impl = *m_impl;
        impl.transport = impl.configuration.backend == NetworkBackend::Lan ? Detail::CreateLanTransport()
            : (impl.configuration.backend == NetworkBackend::Direct ? Detail::CreateDirectTransport() : Detail::CreateEpicTransport());
        if (!impl.transport) { impl.Fail("このビルドにはEOS SDKがありません。LAN通信は利用できます。"); return false; }
        if (!impl.transport->Start(impl.configuration, true, address, name))
        { impl.Fail(impl.transport->Error()); return false; }
        impl.host = true;
        impl.name = std::move(name);
        impl.state = NetworkState::Starting;
        return true;
    }

    bool NetworkSession::Join(std::string address, std::string name)
    {
        if (m_impl->state != NetworkState::Stopped && m_impl->state != NetworkState::Error) return false;
        if (name.empty() || !Text(name, 32) || address.empty() || address.size() > 256)
        { m_impl->error = "接続先と1〜32バイトの表示名を指定してください。"; return false; }
        Stop();
        auto& impl = *m_impl;
        impl.transport = impl.configuration.backend == NetworkBackend::Lan ? Detail::CreateLanTransport()
            : (impl.configuration.backend == NetworkBackend::Direct ? Detail::CreateDirectTransport() : Detail::CreateEpicTransport());
        if (!impl.transport) { impl.Fail("このビルドにはEOS SDKがありません。LAN通信は利用できます。"); return false; }
        if (!impl.transport->Start(impl.configuration, false, address, name))
        { impl.Fail(impl.transport->Error()); return false; }
        impl.name = std::move(name);
        impl.state = NetworkState::Connecting;
        return true;
    }

    void NetworkSession::Stop()
    {
        auto& impl = *m_impl;
        const bool running = impl.state != NetworkState::Stopped;
        ++impl.generation;
        if (impl.transport) impl.transport->Stop();
        impl.transport.reset();
        impl.advertiser.Stop(); impl.dirty.clear(); impl.sessionState.clear(); impl.stateRevision = 0;
        impl.peers.clear(); impl.members.clear(); impl.objects.clear(); impl.events.clear();
        impl.state = NetworkState::Stopped; impl.host = false; impl.local = 0;
        impl.nextPeer = 2; impl.nextObject = 1; impl.age = 0; impl.tick = 0;
        impl.heartbeat = 0; impl.pendingPing = 0; impl.pingSequence = 0;
        impl.statistics = {}; impl.error.clear();
        if (running) impl.Event({ NetworkEventKind::Stopped, 0, 0, {}, {} });
    }

    void NetworkSession::Abort(std::string reason) { m_impl->Fail(std::move(reason)); }

    void NetworkSession::Update(const float elapsedSeconds)
    {
        auto& impl = *m_impl;
        if (!impl.transport || impl.state == NetworkState::Error || impl.state == NetworkState::Stopped
            || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0) return;
        impl.age += elapsedSeconds; impl.tick += elapsedSeconds; impl.heartbeat += elapsedSeconds;
        for (auto& [id, peer] : impl.peers)
        {
            static_cast<void>(id);
            peer.idle += elapsedSeconds; peer.age += elapsedSeconds;
            const float rate = impl.host ? 128.0f : 8192.0f;
            peer.tokens = std::min(rate, peer.tokens + elapsedSeconds * rate);
        }
        for (const auto& event : impl.transport->Poll(elapsedSeconds))
        {
            if (impl.state == NetworkState::Error) break;
            switch (event.kind)
            {
            case Detail::TransportEventKind::Ready:
                if (impl.host && impl.state == NetworkState::Starting)
                {
                    impl.state = NetworkState::Hosting; impl.local = 1;
                    impl.members.push_back({ 1, impl.name });
                    impl.Event({ NetworkEventKind::Started, 1, 0, {}, {} });
                    if (impl.configuration.advertiseLan && !impl.advertiser.Start(impl.configuration))
                        impl.error = "LANへの部屋公開を開始できません。検索ポートとファイアウォールを確認してください。";
                }
                break;
            case Detail::TransportEventKind::Connected:
                impl.peers.emplace(event.peer, Implementation::Peer{});
                if (!impl.host)
                {
                    impl.Send(event.peer, { { "op", "hello" }, { "protocol", 2 },
                        { "game", impl.configuration.gameId }, { "version", impl.configuration.gameVersion },
                        { "scene", impl.configuration.sceneId }, { "name", impl.name } });
                }
                break;
            case Detail::TransportEventKind::Message: impl.Message(event.peer, event.data); break;
            case Detail::TransportEventKind::Disconnected: impl.Lost(event.peer); break;
            case Detail::TransportEventKind::Error: impl.Fail(event.data); break;
            }
        }
        if (impl.state == NetworkState::Error) return;
        std::vector<Detail::TransportPeer> expired;
        for (const auto& [id, peer] : impl.peers)
        {
            if (peer.idle > impl.configuration.timeoutSeconds
                || (peer.id == 0 && peer.age > 5)) expired.push_back(id);
        }
        for (const auto id : expired) { impl.transport->Disconnect(id); impl.Lost(id); }
        if (impl.state == NetworkState::Error) return;
        if ((impl.state == NetworkState::Starting || impl.state == NetworkState::Connecting)
            && impl.age > impl.configuration.timeoutSeconds)
        { impl.Fail("接続がタイムアウトしました。接続先・設定・通信環境を確認してください。"); return; }
        if (impl.heartbeat >= 1)
        {
            impl.heartbeat = 0;
            const auto sequence = ++impl.pingSequence;
            for (const auto& [id, peer] : impl.peers)
            {
                if (peer.id != 0) impl.Send(id, { { "op", "ping" }, { "id", sequence } });
            }
            if (!impl.host) { impl.pendingPing = sequence; impl.pingAge = impl.age; }
        }
        if (IsHost() && impl.tick >= 1.0f / static_cast<float>(impl.configuration.tickRate))
        {
            // フレーム遅延後に過去の送信をまとめて再生しません。
            impl.tick = 0;
            for (const auto& object : impl.objects)
                if (impl.configuration.syncMode == NetworkSyncMode::Continuous || impl.dirty.contains(object.id))
                    impl.Broadcast(ObjectPacket(object));
            impl.dirty.clear();
        }
        impl.advertiser.Update(*this);
    }

    NetworkState NetworkSession::State() const noexcept { return m_impl->state; }
    std::uint64_t NetworkSession::Generation() const noexcept { return m_impl->generation; }
    bool NetworkSession::IsHost() const noexcept { return m_impl->state == NetworkState::Hosting; }
    NetworkPeerId NetworkSession::LocalPeer() const noexcept { return m_impl->local; }
    std::string NetworkSession::RoomAddress() const { return m_impl->transport ? m_impl->transport->Address() : std::string{}; }
    const std::string& NetworkSession::LastError() const noexcept { return m_impl->error; }
    const NetworkConfiguration& NetworkSession::Configuration() const noexcept { return m_impl->configuration; }
    const std::vector<NetworkMember>& NetworkSession::Members() const noexcept { return m_impl->members; }
    const std::vector<NetworkObjectState>& NetworkSession::Objects() const noexcept { return m_impl->objects; }
    const NetworkObjectState* NetworkSession::FindObject(const NetworkObjectId id) const noexcept
    {
        const auto found = std::ranges::find(m_impl->objects, id, &NetworkObjectState::id);
        return found == m_impl->objects.end() ? nullptr : &*found;
    }
    NetworkStatistics NetworkSession::Statistics() const noexcept { return m_impl->statistics; }
    bool NetworkSession::PollEvent(NetworkEvent& event)
    {
        if (m_impl->events.empty()) return false;
        event = std::move(m_impl->events.front()); m_impl->events.pop_front(); return true;
    }

    NetworkObjectId NetworkSession::Spawn(NetworkObjectState object)
    {
        auto& impl = *m_impl;
        if (!IsHost() || impl.objects.size() >= Detail::NetworkObjectLimit
            || impl.nextObject == std::numeric_limits<NetworkObjectId>::max()) return 0;
        object.id = impl.nextObject;
        if (!ValidObject(object) || !impl.Member(object.owner)
            || (!object.sceneKey.empty() && std::ranges::any_of(impl.objects, [&object](const auto& value)
                { return value.sceneKey == object.sceneKey; }))) return 0;
        try { if (ObjectPacket(object).dump().size() > Detail::NetworkPacketMaxBytes) return 0; }
        catch (const std::exception&) { return 0; }
        ++impl.nextObject;
        impl.objects.push_back(object);
        impl.Broadcast(ObjectPacket(object));
        return object.id;
    }

    bool NetworkSession::SetObject(NetworkObjectState object)
    {
        auto& impl = *m_impl;
        if (!IsHost() || !ValidObject(object) || !impl.Member(object.owner)) return false;
        const auto found = std::ranges::find(impl.objects, object.id, &NetworkObjectState::id);
        if (found == impl.objects.end() || found->sceneKey != object.sceneKey
            || found->prefabKey != object.prefabKey) return false;
        try { if (ObjectPacket(object).dump().size() > Detail::NetworkPacketMaxBytes) return false; }
        catch (const std::exception&) { return false; }
        if (*found != object) impl.dirty.insert(object.id);
        *found = std::move(object);
        return true;
    }

    bool NetworkSession::Despawn(const NetworkObjectId id)
    {
        if (!IsHost()) return false;
        const auto removed = std::erase_if(m_impl->objects, [id](const auto& value) { return value.id == id; });
        if (removed == 0) return false;
        m_impl->Broadcast({ { "op", "despawn" }, { "id", id } });
        return true;
    }

    bool NetworkSession::SendInput(const NetworkObjectId id, std::string name, std::string data)
    {
        if (!Key(name) || data.size() > 256) return false;
        const auto* object = FindObject(id);
        if (!object || object->owner != LocalPeer()) return false;
        try
        {
            const Json packet{ { "op", "input" }, { "id", id }, { "name", name }, { "data", data } };
            if (packet.dump().size() > Detail::NetworkPacketMaxBytes) return false;
            if (IsHost()) return m_impl->Event({ NetworkEventKind::Input, 1, id, std::move(name), std::move(data) });
            return State() == NetworkState::Connected && !m_impl->peers.empty()
                && m_impl->Send(m_impl->peers.begin()->first, packet);
        }
        catch (const std::exception&) { return false; }
    }

    bool NetworkSession::BroadcastEvent(std::string name, std::string data)
    {
        if (!IsHost() || !Key(name) || data.size() > 256) return false;
        try
        {
            const Json packet{ { "op", "event" }, { "name", name }, { "data", data } };
            if (packet.dump().size() > Detail::NetworkPacketMaxBytes
                || !m_impl->Event({ NetworkEventKind::GameEvent, 1, 0, name, data })) return false;
            m_impl->Broadcast(packet);
            return true;
        }
        catch (const std::exception&) { return false; }
    }

    bool NetworkSession::SendCommand(std::string name, std::string data)
    {
        if (!Key(name) || data.size() > 256) return false;
        try
        {
            const Json packet{ { "op", "command" }, { "name", name }, { "data", data } };
            if (packet.dump().size() > Detail::NetworkPacketMaxBytes) return false;
            if (IsHost()) return m_impl->Event({ NetworkEventKind::Command, 1, 0, std::move(name), std::move(data) });
            return State() == NetworkState::Connected && !m_impl->peers.empty()
                && m_impl->Send(m_impl->peers.begin()->first, packet);
        }
        catch (...) { return false; }
    }
    bool NetworkSession::SetSessionState(std::string data)
    {
        auto& impl = *m_impl;
        if (!IsHost() || data.size() > 256 || impl.stateRevision == std::numeric_limits<std::uint32_t>::max()) return false;
        if (impl.sessionState == data) return true;
        try
        {
            const Json packet{ { "op", "state" }, { "revision", impl.stateRevision + 1 }, { "data", data } };
            if (packet.dump().size() > Detail::NetworkPacketMaxBytes
                || !impl.Event({ NetworkEventKind::SessionState, 1, 0, {}, data })) return false;
            impl.sessionState = std::move(data); ++impl.stateRevision; impl.Broadcast(packet);
            return true;
        }
        catch (...) { return false; }
    }
    const std::string& NetworkSession::SessionState() const noexcept { return m_impl->sessionState; }
    bool NetworkSession::JoinDirect(std::string endpoint, std::string accessKey, std::string name)
    {
        if (m_impl->configuration.backend != NetworkBackend::Direct) return false;
        return Join("LPD1|" + endpoint + "|" + accessKey, std::move(name));
    }
    std::string NetworkSession::AccessKey() const { return m_impl->transport ? m_impl->transport->AccessKey() : std::string{}; }
    std::string NetworkSession::ConnectionCode(std::string endpoint) const
    {
        return m_impl->transport ? m_impl->transport->ConnectionCode(endpoint) : std::string{};
    }
    std::string NetworkSession::ConnectionStatus() const { return m_impl->transport ? m_impl->transport->Status() : std::string{}; }
    std::string NetworkSession::LocalAddress() const { return m_impl->transport ? m_impl->transport->LocalAddress() : std::string{}; }
    bool NetworkSession::JoinRoom(const NetworkRoom& room, std::string name)
    {
        const auto& game = m_impl->configuration;
        if (room.gameId != game.gameId || room.gameVersion != game.gameVersion || room.sceneId != game.sceneId
            || room.capacity < 2 || room.capacity > 4 || room.players >= room.capacity) return false;
        auto settings = game; settings.backend = room.backend;
        return Configure(std::move(settings)) && Join(room.connection, std::move(name));
    }

    std::string_view NetworkStateName(const NetworkState state) noexcept
    {
        switch (state)
        {
        case NetworkState::Stopped: return "未接続";
        case NetworkState::Starting: return "部屋を作成中";
        case NetworkState::Hosting: return "ホスト";
        case NetworkState::Connecting: return "接続中";
        case NetworkState::Connected: return "参加中";
        case NetworkState::Error: return "接続エラー";
        }
        return "不明";
    }
    NetworkSession* ActiveNetworkSession() noexcept { return activeSession; }
    void SetActiveNetworkSession(NetworkSession* session) noexcept { activeSession = session; }
}
