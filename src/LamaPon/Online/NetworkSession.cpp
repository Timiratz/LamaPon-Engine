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
        // アクティブな通信の借用先
        NetworkSession* activeSession{};

        // 最大64文字の英数字・ピリオド・下線・ハイフンを検証する(value: 識別子, empty: 空の識別子を許可するか)。
        bool Key(const std::string_view value, const bool empty = false)
        {
            if (value.empty()) return empty;
            // 識別子の1文字を検証する(c: 入力文字)。
            return value.size() <= 64 && std::ranges::all_of(value, [](const char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            });
        }

        // 制御文字がない上限内のUTF-8文字列かを検証する(value: 検査する文字列, limit: 上限バイト数)。
        bool Text(const std::string& value, const std::size_t limit)
        {
            // 制御文字かを検証する(c: 入力文字)。
            if (value.size() > limit || std::ranges::any_of(value, [](const char c)
            {
                // 入力文字の符号なし表現
                const auto byte = static_cast<unsigned char>(c);
                return byte < 32 || byte == 127;
            })) return false;
            try { static_cast<void>(Json(value).dump()); return true; }
            catch (const std::exception&) { return false; }
        }

        // 識別情報・データ長・有限な変換と回転長を検証する(object: 同期対象の状態)。
        // 回転の二乗長は0.5〜1.5の開区間を許容し、ここでは正規化しません。
        bool ValidObject(const NetworkObjectState& object)
        {
            if (object.id == 0 || object.owner == 0 || object.data.size() > 256
                || !Key(object.sceneKey, true) || !Key(object.prefabKey, true)
                || (object.sceneKey.empty() == object.prefabKey.empty())) return false;
            // 有限で範囲内かを判定(value: 変換の1要素)。
            const auto valid = [](const float value)
            {
                return std::isfinite(value) && std::abs(value) <= 1.0e6f;
            };
            if (!std::ranges::all_of(object.transform.position, valid)
                || !std::ranges::all_of(object.transform.scale, valid)
                || !std::ranges::all_of(object.transform.rotation, valid)) return false;
            // 回転クォータニオンの二乗長
            float length{};
            // 回転クォータニオンの1成分
            for (const auto value : object.transform.rotation) length += value * value;
            return length > 0.5f && length < 1.5f;
        }

        // JSONからuint32範囲の整数を読み、不正なら例外にする(value: 整数のJSON値)。
        std::uint32_t ReadId(const Json& value)
        {
            if (!value.is_number_integer() || value.get<std::int64_t>() < 0
                || value.get<std::uint64_t>() > std::numeric_limits<std::uint32_t>::max())
                throw std::invalid_argument("Integer range");
            return value.get<std::uint32_t>();
        }

        // 同期状態を送信JSONへ変換する(object: 同期対象の状態)。
        Json ObjectPacket(const NetworkObjectState& object)
        {
            return Json{ { "op", "object" }, { "id", object.id }, { "owner", object.owner },
                { "scene", object.sceneKey }, { "prefab", object.prefabKey },
                { "p", object.transform.position }, { "r", object.transform.rotation },
                { "s", object.transform.scale }, { "enabled", object.enabled },
                { "data", object.data } };
        }

        // JSONの要素数と同期状態を検証して読む(packet: 受信したobject操作)。
        NetworkObjectState ReadObject(const Json& packet)
        {
            // 同期オブジェクトの状態またはID
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
        // 確認済みのPrefab識別子
        std::vector<std::string> keys;
        // 同期Prefabの登録情報
        for (const auto& prefab : configuration.prefabs)
        {
            // Windowsのドライブ相対パス・UNC・代替データストリームも拒否します。
            // assetsからの相対パス
            const auto& path = prefab.assetPath;
            if (!Key(prefab.key) || std::ranges::find(keys, prefab.key) != keys.end()
                || path.empty() || path.size() > 512 || !Text(path, 512)
                || path.front() == '/' || path.front() == '\\' || path.find(':') != std::string::npos)
                throw std::invalid_argument("同期Prefabには一意なキーとassetsからの相対パスを指定してください。");
            // 区切りを統一した相対パス
            std::string normalized = path;
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            // パス要素の開始位置
            std::size_t start{};
            while (start <= normalized.size())
            {
                // パス要素の終端位置
                const auto end = normalized.find('/', start);
                // 検証するパスの1要素
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
            // 参加確定したID・未確定は0
            NetworkPeerId id{};
            // 最後の有効な受信からの秒数
            float idle{};
            // 接続またはセッションの経過秒数
            float age{};
            // 受信可能なメッセージの残量
            float tokens{ 256 };
        };
        // 検証済みの通信設定
        NetworkConfiguration configuration;
        // 通信バックエンドの所有先
        std::unique_ptr<Detail::INetworkTransport> transport;
        // LAN部屋広告の状態
        Detail::NetworkRoomAdvertiser advertiser;
        // 次のtickで送る変更済みID
        std::set<NetworkObjectId> dirty;
        // 途中参加にも配信する部屋状態
        std::string sessionState;
        // 部屋状態の変更連番
        std::uint32_t stateRevision{};
        // セッションの接続状態
        NetworkState state{ NetworkState::Stopped };
        // 停止または失敗ごとの世代番号
        std::uint64_t generation{};
        // ホスト側として動作するか
        bool host{};
        // 自身の参加者ID・未確定は0
        NetworkPeerId local{};
        // 次の参加者ID・2から開始
        NetworkPeerId nextPeer{ 2 };
        // 次の通信対象ID・1から開始
        NetworkObjectId nextObject{ 1 };
        // 自身の参加者表示名
        std::string name;
        // 直近の通信エラー
        std::string error;
        // 現在の参加者一覧
        std::vector<NetworkMember> members;
        // 登録された同期対象の一覧
        std::vector<NetworkObjectState> objects;
        // 接続ID別の参加者情報
        std::map<Detail::TransportPeer, Peer> peers;
        // ゲームが消費するイベント
        std::deque<NetworkEvent> events;
        // 送受信と拒否の通信集計
        NetworkStatistics statistics;
        // 接続またはセッションの経過秒数
        float age{};
        // 前回の同期送信からの秒数
        float tick{};
        // 前回のping送信からの秒数
        float heartbeat{};
        // 次のpingに使う連番
        std::uint32_t pingSequence{};
        // 往復時間を計測中のping番号
        std::uint32_t pendingPing{};
        // 計測中pingを送った経過秒数
        float pingAge{};

        // 上限内ならゲームイベントをキューに積む(event: 登録するイベント)。
        bool Event(NetworkEvent event)
        {
            if (events.size() >= Detail::NetworkEventLimit) return false;
            events.push_back(std::move(event));
            return true;
        }

        // 通信と同期状態を消し、接続世代を進めてエラーを残す(message: 失敗理由)。
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

        // JSONを送信キューへ積み、登録失敗時は接続を切る(peer: 宛先の接続ID, packet: 送信するJSON)。
        bool Send(const Detail::TransportPeer peer, const Json& packet)
        {
            // JSON化した通信内容
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

        // 参加確定済みの各接続へJSONの送信を要求する(packet: 配信するJSON)。
        void Broadcast(const Json& packet)
        {
            // 接続IDと参加者の状態
            for (const auto& [peer, value] : peers)
            {
                if (value.id != 0) Send(peer, packet);
            }
        }

        // 最新の参加者一覧を全参加者へ送る。
        void SendMembers()
        {
            // 送信または受信する参加者一覧
            Json list = Json::array();
            // 登録または更新する参加者
            for (const auto& member : members)
                list.push_back({ { "id", member.id }, { "name", member.name } });
            Broadcast({ { "op", "members" }, { "members", list } });
        }

        // 拒否件数を数え、ホストは相手を切断し参加側は接続を終了する(peer: 不正な通信の接続ID)。
        void Reject(const Detail::TransportPeer peer)
        {
            ++statistics.rejectedMessages;
            if (host) { transport->Disconnect(peer); Lost(peer); }
            else Fail("ホストから無効な通信データを受信しました。");
        }

        // 参加者一覧にIDがあるかを調べる(id: 参加者ID)。
        bool Member(const NetworkPeerId id) const
        {
            // 参加者IDを照合する(member: 登録された参加者)。
            return std::ranges::any_of(members, [id](const auto& member) { return member.id == id; });
        }

        // 退出した所有物を除き、ホスト切断時は接続を終了する(peer: 切れた接続ID)。
        void Lost(const Detail::TransportPeer peer)
        {
            // 接続または同期対象の検索結果
            const auto found = peers.find(peer);
            if (found == peers.end()) return;
            // 参加者または通信対象のID
            const auto id = found->second.id;
            peers.erase(found);
            if (!host)
            {
                Fail("ホストとの接続が終了しました。");
                return;
            }
            if (id == 0) return;
            // 退出したIDを除く(member: 登録された参加者)。
            std::erase_if(members, [id](const auto& member) { return member.id == id; });
            // 退出したプレイヤーの所有物はホストが削除し、全参加者に通知します。
            // 削除対象のID一覧または件数
            std::vector<NetworkObjectId> removed;
            // 同期オブジェクトの状態またはID
            for (const auto& object : objects) if (object.owner == id) removed.push_back(object.id);
            // 同期オブジェクトの状態またはID
            for (const auto object : removed)
            {
                // 所有物を削除する(value: 登録された同期対象)。
                std::erase_if(objects, [object](const auto& value) { return value.id == object; });
                Broadcast({ { "op", "despawn" }, { "id", object } });
            }
            Event({ NetworkEventKind::Left, id, 0, {}, {} });
            SendMembers();
        }

        // 件数・形式・互換性・所有者を検証して通信内容を反映する(peer: 送信元接続ID, data: 受信JSONのバイト列)。
        void Message(const Detail::TransportPeer peer, const std::string& data)
        {
            // 接続または同期対象の検索結果
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
                // 深さ8を超えた入力を拒否する(depth: JSONの入れ子の深さ)。
                // 送信または受信のJSON
                const auto packet = Json::parse(data, [](const int depth, Json::parse_event_t, Json&)
                {
                    if (depth > 8) throw std::invalid_argument("JSON depth");
                    return true;
                });
                // 受信した操作の識別子
                const auto op = packet.at("op").get<std::string>();
                if (op == "hello" && host && found->second.id == 0)
                {
                    // 接続を要求した参加者名
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
                    // 参加者または通信対象のID
                    const auto id = nextPeer++;
                    found->second.id = id;
                    members.push_back({ id, incomingName });
                    Send(peer, { { "op", "welcome" }, { "protocol", 2 }, { "peer", id },
                        { "game", configuration.gameId }, { "version", configuration.gameVersion },
                        { "scene", configuration.sceneId } });
                    SendMembers();
                    // 同期オブジェクトの状態またはID
                    for (const auto& object : objects) Send(peer, ObjectPacket(object));
                    Send(peer, { { "op", "state" }, { "revision", stateRevision }, { "data", sessionState } });
                    Send(peer, { { "op", "ready" } });
                    Event({ NetworkEventKind::Joined, id, 0, incomingName, {} });
                }
                else if (op == "welcome" && !host && local == 0 && state == NetworkState::Connecting)
                {
                    // 参加者または通信対象のID
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
                    // 送信または受信する参加者一覧
                    const auto& list = packet.at("members");
                    if (!list.is_array() || list.size() < 2 || list.size() > configuration.maxPlayers)
                        throw std::invalid_argument("Members");
                    // 検証後に反映する参加者一覧
                    std::vector<NetworkMember> updated;
                    // 受信した参加者のJSON値
                    for (const auto& value : list)
                    {
                        // 参加者または通信対象のID
                        const auto id = ReadId(value.at("id"));
                        // 受信した参加者の表示名
                        const auto memberName = value.at("name").get<std::string>();
                        // IDの重複を調べる(item: 確認済みの参加者)。
                        if (id == 0 || !Text(memberName, 32) || memberName.empty()
                            || std::ranges::any_of(updated, [id](const auto& item) { return item.id == id; }))
                            throw std::invalid_argument("Member");
                        updated.push_back({ id, memberName });
                    }
                    // ホストと自身の登録を検証する(member: 更新する参加者)。
                    if (!std::ranges::any_of(updated, [](const auto& member) { return member.id == 1; })
                        || !std::ranges::any_of(updated, [this](const auto& member) { return member.id == local; }))
                        throw std::invalid_argument("Local member");
                    // 登録または更新する参加者
                    for (const auto& member : updated)
                        if (!Member(member.id)) Event({ NetworkEventKind::Joined, member.id, 0, member.name, {} });
                    // 登録または更新する参加者
                    for (const auto& member : members)
                        // 退出した参加者かを調べる(value: 更新する参加者)。
                        if (std::ranges::none_of(updated, [&member](const auto& value) { return value.id == member.id; }))
                            Event({ NetworkEventKind::Left, member.id, 0, member.name, {} });
                    members = std::move(updated);
                }
                else if (op == "object" && !host)
                {
                    // 同期オブジェクトの状態またはID
                    auto object = ReadObject(packet);
                    if (!Member(object.owner)) throw std::invalid_argument("Owner");
                    // 同じIDの登録済み同期対象
                    const auto existing = std::ranges::find(objects, object.id, &NetworkObjectState::id);
                    if (existing == objects.end())
                    {
                        // 固定対象の識別子を照合する(value: 登録された同期対象)。
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
                    // 参加者または通信対象のID
                    const auto id = ReadId(packet.at("id"));
                    // 指定IDの対象を消す(value: 登録された同期対象)。
                    std::erase_if(objects, [id](const auto& value) { return value.id == id; });
                }
                else if (op == "input" && host)
                {
                    // 参加者または通信対象のID
                    const auto id = ReadId(packet.at("id"));
                    // 同期オブジェクトの状態またはID
                    const auto object = std::ranges::find(objects, id, &NetworkObjectState::id);
                    // 入力・イベント・コマンド名
                    const auto action = packet.at("name").get<std::string>();
                    // 最大256バイトの受信内容
                    const auto payload = packet.at("data").get<std::string>();
                    if (object == objects.end() || object->owner != found->second.id
                        || !Key(action) || payload.size() > 256
                        || !Event({ NetworkEventKind::Input, found->second.id, id, action, payload }))
                        throw std::invalid_argument("Input owner/queue");
                }
                else if (op == "command" && host)
                {
                    // 入力・イベント・コマンド名
                    const auto action = packet.at("name").get<std::string>();
                    // 最大256バイトの受信内容
                    const auto payload = packet.at("data").get<std::string>();
                    if (!Key(action) || payload.size() > 256
                        || !Event({ NetworkEventKind::Command, found->second.id, 0, action, payload }))
                        throw std::invalid_argument("Command queue");
                }
                else if (op == "state" && !host)
                {
                    // 受信した部屋状態の変更連番
                    const auto revision = ReadId(packet.at("revision"));
                    // 最大256バイトの受信内容
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
                    // 入力・イベント・コマンド名
                    const auto action = packet.at("name").get<std::string>();
                    // 最大256バイトの受信内容
                    const auto payload = packet.at("data").get<std::string>();
                    if (!Key(action) || payload.size() > 256
                        || !Event({ NetworkEventKind::GameEvent, 1, 0, action, payload }))
                        throw std::invalid_argument("Event queue");
                }
                else if (op == "ping") Send(peer, { { "op", "pong" }, { "id", ReadId(packet.at("id")) } });
                else if (op == "pong")
                {
                    // ping応答または送信の番号
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
        // 設定検証の失敗(error: 検証エラー)。
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
        // 通信セッション状態の参照
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
        // 通信セッション状態の参照
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
        // 通信セッション状態の参照
        auto& impl = *m_impl;
        // 停止前に接続処理中だったか
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
        // 通信セッション状態の参照
        auto& impl = *m_impl;
        if (!impl.transport || impl.state == NetworkState::Error || impl.state == NetworkState::Stopped
            || !std::isfinite(elapsedSeconds) || elapsedSeconds < 0) return;
        impl.age += elapsedSeconds; impl.tick += elapsedSeconds; impl.heartbeat += elapsedSeconds;
        // 接続IDと参加者の状態
        for (auto& [id, peer] : impl.peers)
        {
            static_cast<void>(id);
            peer.idle += elapsedSeconds; peer.age += elapsedSeconds;
            // 1秒あたりに許可する受信件数
            const float rate = impl.host ? 128.0f : 8192.0f;
            peer.tokens = std::min(rate, peer.tokens + elapsedSeconds * rate);
        }
        // バックエンドの通信イベント
        for (const auto& event : impl.transport->Poll(elapsedSeconds))
        {
            if (impl.state == NetworkState::Error) break;
            // 通信イベントをセッション状態へ反映します。
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
        // 無通信または認証期限超過のID
        std::vector<Detail::TransportPeer> expired;
        // 接続IDと参加者の状態
        for (const auto& [id, peer] : impl.peers)
        {
            if (peer.idle > impl.configuration.timeoutSeconds
                || (peer.id == 0 && peer.age > 5)) expired.push_back(id);
        }
        // 参加者または通信対象のID
        for (const auto id : expired) { impl.transport->Disconnect(id); impl.Lost(id); }
        if (impl.state == NetworkState::Error) return;
        if ((impl.state == NetworkState::Starting || impl.state == NetworkState::Connecting)
            && impl.age > impl.configuration.timeoutSeconds)
        { impl.Fail("接続がタイムアウトしました。接続先・設定・通信環境を確認してください。"); return; }
        if (impl.heartbeat >= 1)
        {
            impl.heartbeat = 0;
            // ping応答または送信の番号
            const auto sequence = ++impl.pingSequence;
            // 接続IDと参加者の状態
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
            // 同期オブジェクトの状態またはID
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
        // 接続または同期対象の検索結果
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
        // 通信セッション状態の参照
        auto& impl = *m_impl;
        if (!IsHost() || impl.objects.size() >= Detail::NetworkObjectLimit
            || impl.nextObject == std::numeric_limits<NetworkObjectId>::max()) return 0;
        object.id = impl.nextObject;
        // 固定対象の識別子を照合する(value: 登録された同期対象)。
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
        // 通信セッション状態の参照
        auto& impl = *m_impl;
        if (!IsHost() || !ValidObject(object) || !impl.Member(object.owner)) return false;
        // 接続または同期対象の検索結果
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
        // 指定IDの対象を消す(value: 登録された同期対象)。
        // 削除対象のID一覧または件数
        const auto removed = std::erase_if(m_impl->objects, [id](const auto& value) { return value.id == id; });
        if (removed == 0) return false;
        m_impl->Broadcast({ { "op", "despawn" }, { "id", id } });
        return true;
    }

    // 所有者の入力をホストへ送る(id: 通信オブジェクトID, name: 入力名, data: 最大256バイトの内容)。
    bool NetworkSession::SendInput(const NetworkObjectId id, std::string name, std::string data)
    {
        if (!Key(name) || data.size() > 256) return false;
        // 同期オブジェクトの状態またはID
        const auto* object = FindObject(id);
        if (!object || object->owner != LocalPeer()) return false;
        try
        {
            // 送信または受信のJSON
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
            // 送信または受信のJSON
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
            // 送信または受信のJSON
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
        // 通信セッション状態の参照
        auto& impl = *m_impl;
        if (!IsHost() || data.size() > 256 || impl.stateRevision == std::numeric_limits<std::uint32_t>::max()) return false;
        if (impl.sessionState == data) return true;
        try
        {
            // 送信または受信のJSON
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
        // 部屋照合に使う現在の設定
        const auto& game = m_impl->configuration;
        if (room.gameId != game.gameId || room.gameVersion != game.gameVersion || room.sceneId != game.sceneId
            || room.capacity < 2 || room.capacity > 4 || room.players >= room.capacity) return false;
        // 接続する部屋の通信設定
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
