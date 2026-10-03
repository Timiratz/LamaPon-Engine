#include "LamaPon/Online/NetworkEndpoint.h"
#include "LamaPon/Online/NetworkSession.h"
#include "LamaPon/Online/NetworkCrypto.h"
#include "LamaPon/Online/NetworkPortMapping.h"
#include "LamaPon/Online/NetworkRoomBrowser.h"
#include "LamaPon/Online/NetworkSettingsJson.h"
#include "../samples/Networking/P2PTurnBasedController.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    using namespace LamaPon;
    using namespace LamaPon::Detail;
    // Require(value: 判定条件, text: 失敗理由)は不成立時に例外を送出する。
    void Require(const bool value, const char* text)
    {
        // 条件違反を検出する
        if (!value) throw std::runtime_error(text);
    }
    // Rejected(action: 実行対象, text: 失敗理由)は例外発生を必須とする。
    template<class Action> void Rejected(Action action, const char* text)
    {
        // 例外が発生したか
        bool rejected{};
        // 実行対象の例外を捕捉する
        try { action(); }
        // 例外発生を記録する
        catch (const std::exception&) { rejected = true; }
        Require(rejected, text);
    }
    // Until(host: ホスト, client: クライアント, condition: 待機条件, other: 任意の追加セッション)は条件成立まで通信を進める。
    template<class Condition> void Until(NetworkSession& host, NetworkSession& client, Condition condition,
        NetworkSession* other = nullptr)
    {
        // 待機の期限
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        // conditionが成立するまで双方を更新する
        while (!condition())
        {
            host.Update(0.005f); client.Update(0.005f);
            // 追加セッションも同時に更新する
            if (other) other->Update(0.005f);
            // タイムアウトを検出する
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Direct test timeout: " + host.LastError() + " / " + client.LastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // CryptoVectors()は暗号規格ベクトルと改ざん・再送拒否を検証する。
    void CryptoVectors()
    {
        // RFC 4231で指定されたHMAC鍵
        const std::array<unsigned char, 20> hmacKey{ 11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11 };
        // RFC 4231の入力メッセージ
        constexpr std::string_view message = "Hi There";
        Require(Hex(NetworkHmac(hmacKey, std::span(reinterpret_cast<const unsigned char*>(message.data()), message.size())))
            == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "RFC 4231 HMAC vector");
        // RFC 5869の秘密入力
        std::array<unsigned char, 22> secret{}; secret.fill(11);
        // HKDFのsalt入力
        std::array<unsigned char, 13> salt{};
        // i: salt要素番号を表す
        for (unsigned char i = 0; i < salt.size(); ++i) salt[i] = i;
        // HKDFの追加情報
        std::string info;
        // i: 追加情報の各バイト値を表す
        for (int i = 0xf0; i <= 0xf9; ++i) info += static_cast<char>(i);
        Require(Hex(NetworkHkdf(salt, secret, info)) == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf", "RFC 5869 HKDF vector");
        // AES鍵・nonce・認証タグの入力
        NetworkKey key{}; std::array<unsigned char, 12> nonce{}; std::array<unsigned char, 16> tag{};
        // AES-GCMの暗号文
        const auto cipher = NetworkAesGcm(false, key, nonce, {}, std::string(16, '\0'), tag);
        Require(Hex(std::span(reinterpret_cast<const unsigned char*>(cipher.data()), cipher.size()))
            == "cea7403d4d606b6e074ec5d3baf39d18" && Hex(tag) == "d0d1c8a799996bf0265b98b5d48ab919", "AES-256-GCM vector");
        Require(NetworkAesGcm(true, key, nonce, {}, cipher, tag) == std::string(16, '\0'), "AES vector decrypt");
        // 暗号化側と復号側の状態
        NetworkCipher send, receive; key = RandomNetworkKey(); send.Initialize(key); receive.Initialize(key);
        // 暗号化したテストメッセージ
        const auto packet = send.Seal("private game message");
        Require(packet.find("private game message") == std::string::npos, "Encrypted body");
        // 認証タグを改ざんしたパケット
        auto changed = packet; changed.back() ^= 1;
        Rejected([&] { static_cast<void>(receive.Open(changed)); }, "Tampered tag rejected");
        Require(receive.Open(packet) == "private game message", "Valid record after invalid authentication");
        Rejected([&] { static_cast<void>(receive.Open(packet)); }, "Replay rejected");
        Require(receive.Open(send.Seal("second")) == "second", "Monotonic record sequence");
        // ECDHの送信側と受信側
        NetworkKeyExchange first, second;
        // 双方の公開鍵
        const auto firstPublic = first.PublicKey(), secondPublic = second.PublicKey();
        Require(first.Agree(secondPublic) == second.Agree(firstPublic), "Ephemeral ECDH agreement");
        // 不正な曲線点を含む公開鍵
        auto invalid = secondPublic; invalid[0] ^= 1;
        Rejected([&] { static_cast<void>(first.Agree(invalid)); }, "Incorrect ECDH curve rejected");
    }
    // EndpointAndSettings()はIPv6端点と互換設定の検証を行う。
    void EndpointAndSettings()
    {
        // 各形式の解析に使う端点
        NetworkEndpoint endpoint;
        Require(NetworkEndpoint::Parse("[::1]:27840", 0, endpoint) && endpoint.Family() == AF_INET6
            && endpoint.Text() == "[::1]:27840" && endpoint.Unicast(), "Bracketed IPv6 endpoint");
        Require(NetworkEndpoint::Parse("::", 0, endpoint) && endpoint.Wildcard(), "IPv6 wildcard ephemeral port");
        Require(NetworkEndpoint::Parse("[fe80::1%7]:1234", 0, endpoint) && endpoint.Host() == "fe80::1%7", "Numeric IPv6 scope");
        Require(!NetworkEndpoint::Parse("[::1]:65536", 0, endpoint)
            && !NetworkEndpoint::Parse("hostname.invalid", 0, endpoint)
            && !NetworkEndpoint::Parse("127.0.0.1:-1", 0, endpoint), "Invalid endpoint rejected");
        Require(IsPublicNetworkIpv4("8.8.8.8") && IsPublicNetworkIpv4("203.1.2.3") && IsPublicNetworkIpv4("192.1.2.3"), "Public ranges");
        // address: 公開扱いされない予約IPv4アドレスを確認する
        for (const auto* address : { "127.0.0.1", "192.168.1.2", "10.0.0.1", "172.16.1.2", "100.64.1.2", "198.18.0.1", "203.0.113.1", "0.0.0.0", "224.0.0.1" })
            Require(!IsPublicNetworkIpv4(address), "Non-public mapped address rejected");
        // Direct接続の設定
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.syncMode = NetworkSyncMode::OnChange;
        config.advertiseLan = true; config.automaticPortMapping = true; config.roomName = "ターン制の部屋"; config.discoveryPort = 27849;
        // 設定JSONと読み戻し結果
        const auto json = NetworkSettingsToJson(config); const auto loaded = NetworkSettingsFromJson(json);
        Require(loaded.backend == config.backend && loaded.syncMode == config.syncMode && loaded.advertiseLan
            && loaded.automaticPortMapping && loaded.roomName == config.roomName && loaded.discoveryPort == 27849, "New settings round trip");
        Require(json.dump().find("LPD1") == std::string::npos && !json.contains("accessKey"), "No ephemeral credentials in settings");
        // 旧形式から復元する既定設定
        const auto old = NetworkSettingsFromJson(nlohmann::json::object());
        Require(old.backend == NetworkBackend::Lan && !old.automaticPortMapping && !old.advertiseLan, "Old project remains opt-in");
        // 不正な同期方式を含む設定
        auto invalid = json; invalid["syncMode"] = "Unknown";
        Rejected([&] { static_cast<void>(NetworkSettingsFromJson(invalid)); }, "Invalid sync profile rejected");
    }
    class FakeGateway final : public INetworkGateway
    {
    public:
        // 現在のモックポート転送
        std::optional<NetworkPortEntry> entry;
        // 追加・削除要求の回数
        int adds{}, removes{};
        // 永続転送と検査失敗の切替
        bool permanent{}, fail{};
        // Inspect(port: 対象ポート)は現在の転送設定を返す。
        std::optional<NetworkPortEntry> Inspect(std::uint16_t) override
        {
            // モックのタイムアウトを再現する
            if (fail) throw std::runtime_error("Mock timeout");
            return entry;
        }
        // Add(port: 対象ポート, value: 追加設定)はモック転送を追加する。
        bool Add(std::uint16_t, const NetworkPortEntry& value) override
        {
            ++adds; entry = value;
            // 永続転送を要求された状態にする
            if (permanent) entry->leaseSeconds = 0;
            return true;
        }
        // Remove(port: 対象ポート)はモック転送を削除する。
        void Remove(std::uint16_t) override { ++removes; entry.reset(); }
        // ExternalAddress()はテスト用の公開アドレスを返す。
        std::string ExternalAddress() override { return "8.8.8.8"; }
    };
    // MappingOwnership()は自分の転送だけを更新・削除することを検証する。
    void MappingOwnership()
    {
        // 転送所有権テスト用ゲートウェイ
        FakeGateway gateway;
        {
            // 取得・更新を検証する転送リース
            NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own");
            Require(lease.Acquire() && gateway.entry->leaseSeconds == 120 && lease.Renew(), "Acquire and renew bounded lease");
        }
        Require(gateway.removes == 1 && !gateway.entry, "Release own mapping");
        gateway.entry = NetworkPortEntry{ "192.168.1.30", 27840, "other application", 120 };
        // 既存転送の追加回数
        const int before = gateway.adds;
        {
            // 既存転送を保護するリース
            NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own"); Require(!lease.Acquire(), "Existing mapping protected"); }
        Require(gateway.adds == before && gateway.removes == 1 && gateway.entry, "Never overwrite or delete existing mapping");
        gateway.entry.reset();
        {
            // 所有者変更後の更新を検証するリース
            NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own"); Require(lease.Acquire(), "Ownership test acquire");
            gateway.entry->description = "new owner";
            Require(!lease.Renew(), "Renew does not overwrite replacement");
        }
        Require(gateway.removes == 1 && gateway.entry->description == "new owner", "Replacement mapping retained");
        gateway.entry.reset(); gateway.permanent = true;
        {
            // 永続転送を拒否するリース
            NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own"); Require(!lease.Acquire(), "Permanent-only mapping rejected"); }
        Require(gateway.removes == 2 && !gateway.entry, "Rollback unexpected permanent mapping");
        gateway.fail = true;
        // 検査状態不明を失敗として扱うリース
        NetworkPortLease lease(gateway, "192.168.1.20", 27840, "LamaPon-own");
        Rejected([&] { lease.Acquire(); }, "Unknown inspection is not treated as empty");
    }
    // DirectConnection(address: 接続先アドレス)は暗号化接続と参加制限を検証する。
    void DirectConnection(const char* address)
    {
        // Direct接続に使うネットワーク設定
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.port = 0; config.syncMode = NetworkSyncMode::OnChange;
        // ホスト・通常参加者・遅延参加者・4人目のセッション
        NetworkSession host, client, late, fourth;
        Require(host.Configure(config) && client.Configure(config) && late.Configure(config) && fourth.Configure(config) && host.Host("Host", address), "Direct host start");
        host.Update(0); Require(host.IsHost() && host.AccessKey().size() == 64 && host.ConnectionCode().starts_with("LPD1|"), "Direct ready code");
        Require(host.SetSessionState("initial turn state"), "Host global state");
        Require(client.JoinDirect(host.LocalAddress(), host.AccessKey(), "Client"), "Separate endpoint and key");
        Until(host, client, [&] { return client.State() == NetworkState::Connected; });
        Require(client.SessionState() == "initial turn state" && client.Members().size() == 2, "Global state before ready");
        Require(!client.SetSessionState("cheat") && !client.BroadcastEvent("cheat", ""), "Host-only state and events");
        Require(client.SendCommand("choose", "card-3"), "Authenticated command");
        // コマンド通知の受信状態
        bool received{};
        Until(host, client, [&]
        {
            // 次に受信するネットワークイベント
            NetworkEvent event;
            // キュー内イベントをすべて調べる
            while (host.PollEvent(event))
            {
                // Commandイベントを送信者と内容で確認する
                if (event.kind == NetworkEventKind::Command)
                    received = event.peer == client.LocalPeer() && event.name == "choose" && event.data == "card-3";
            }
            return received;
        });
        // 複製同期の対象オブジェクト
        NetworkObjectState object; object.sceneKey = "board";
        // 生成したネットワークID
        const auto id = host.Spawn(object); Require(id != 0, "Direct object create");
        Until(host, client, [&] { return client.FindObject(id) != nullptr; });
        // 変更前の送信バイト数
        const auto bytes = host.Statistics().sentBytes;
        // i: 変更なしのスナップショットを繰り返す番号
        for (int i = 0; i < 25; ++i) { Require(host.SetObject(*host.FindObject(id)), "Unchanged object"); host.Update(0.01f); client.Update(0.01f); }
        Require(host.Statistics().sentBytes == bytes, "On-change avoids unchanged snapshots");
        object = *host.FindObject(id); object.data = "changed"; Require(host.SetObject(object), "Dirty object");
        Until(host, client, [&] { return client.FindObject(id)->data == "changed"; });
        // index: 初期同期数上限までの追加番号
        for (int index = 1; index < 128; ++index)
        {
            // 追加するスナップショット
            NetworkObjectState extra; extra.sceneKey = "bulk." + std::to_string(index);
            Require(host.Spawn(extra) != 0, "Secure full baseline spawn");
        }
        Until(host, client, [&] { return client.Objects().size() == 128; });
        Require(host.SetSessionState("late join turn"), "Latest global state");
        Require(late.Join(host.ConnectionCode(), "Late"), "Code is optional connection method");
        Until(host, client, [&] { return late.State() == NetworkState::Connected; }, &late);
        Require(late.SessionState() == "late join turn" && late.FindObject(id)->data == "changed", "Secure late join baseline");
        Require(late.Objects().size() == 128, "Encrypted 128-object baseline");
        Require(fourth.Join(host.RoomAddress(), "Fourth"), "Fourth encrypted player");
        Until(host, client, [&] { return fourth.State() == NetworkState::Connected; }, &fourth);
        // 定員超過時の参加試行
        NetworkSession full; Require(full.Configure(config) && full.Join(host.RoomAddress()), "Full encrypted room attempt");
        Until(host, client, [&] { return full.State() == NetworkState::Error; }, &full);
        Require(host.Members().size() == 4, "Full room does not expand membership");
        // 不正鍵を試すセッション
        NetworkSession wrong; Require(wrong.Configure(config), "Bad-key configuration");
        // 先頭バイトを変更した不正鍵
        auto badKey = host.AccessKey(); badKey[0] = badKey[0] == '0' ? '1' : '0';
        Require(wrong.JoinDirect(host.LocalAddress(), badKey), "Bad key begins transport");
        Until(host, client, [&] { return wrong.State() == NetworkState::Error; }, &wrong);
        Require(host.IsHost() && host.Members().size() == 4 && wrong.LastError().find(badKey) == std::string::npos, "Bad key rejected without host failure or secret log");
        // 再起動前に失効する鍵
        const auto oldKey = host.AccessKey();
        fourth.Stop(); late.Stop(); client.Stop(); host.Stop();
        Require(host.AccessKey().empty() && host.SessionState().empty() && host.RoomAddress().empty(), "Stop clears runtime state");
        Require(host.Host("Again", address), "Direct restart"); host.Update(0);
        Require(host.AccessKey() != oldKey, "Fresh runtime key");
        // 停止済み鍵で参加するセッション
        NetworkSession expired; Require(expired.Configure(config) && expired.JoinDirect(host.LocalAddress(), oldKey), "Expired key attempt");
        Until(host, client, [&] { return expired.State() == NetworkState::Error; }, &expired);
        Require(host.Members().size() == 1, "Stopped room credentials are invalid after restart");
        // localhost以外では行わない暗号化必須の旧方式試験
        if (std::string_view(address) == "127.0.0.1")
        {
            // 暗号化を要求しない互換設定
            NetworkConfiguration legacy; legacy.port = 0;
            // 平文接続を試すセッション
            NetworkSession plaintext; Require(plaintext.Configure(legacy) && plaintext.Join(host.LocalAddress()), "Plaintext downgrade attempt");
            Until(host, client, [&] { return plaintext.State() == NetworkState::Error; }, &plaintext);
            Require(host.IsHost() && host.Members().size() == 1, "Direct never accepts plaintext fallback");
        }
    }
    // TurnBasedGame()はターン制状態の複製と不正手番拒否を検証する。
    void TurnBasedGame()
    {
        // ターン制ゲームのDirect設定
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.port = 0; config.syncMode = NetworkSyncMode::OnChange;
        // ホスト・参加者・観戦者の各セッション
        NetworkSession host, client, spectator;
        Require(host.Configure(config) && client.Configure(config) && spectator.Configure(config), "Turn config");
        Require(host.Host(), "Turn host"); host.Update(0); Require(client.Join(host.RoomAddress()), "Turn client");
        Until(host, client, [&] { return client.State() == NetworkState::Connected; });
        // ホスト・参加者・観戦者のゲーム状態
        Samples::P2PTurnBasedController first, second, watching;
        // step()は各セッションとゲーム制御を1フレーム進める。
        const auto step = [&]
        {
            host.Update(0.01f); client.Update(0.01f); spectator.Update(0.01f);
            first.Update(host); second.Update(client); watching.Update(spectator);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        // i: 接続状態を安定させる更新番号
        for (int i = 0; i < 20; ++i) step();
        Require(!second.RequestMove(client, 0) && first.RequestMove(host, 0), "Only active turn may request");
        // i: 最初の手番を同期させる更新番号
        for (int i = 0; i < 20; ++i) step();
        Require(second.Board()[0] == 'X' && second.Turn() == client.LocalPeer(), "Turn state replicated");
        Require(client.SendCommand("board.move", "0:4"), "Stale turn command");
        // i: 古い盤面リビジョンを処理する更新番号
        for (int i = 0; i < 20; ++i) step();
        Require(first.Board()[4] == '.', "Stale revision rejected by game");
        Require(second.RequestMove(client, 4), "Second turn");
        // i: 2回目の手番を同期させる更新番号
        for (int i = 0; i < 20; ++i) step();
        Require(spectator.Join(host.RoomAddress()), "Spectator late join");
        // i: 観戦者へ途中盤面を同期する更新番号
        for (int i = 0; i < 30; ++i) step();
        Require(watching.Board()[0] == 'X' && watching.Board()[4] == 'O' && !watching.RequestMove(spectator, 8), "Late spectator sees board and cannot move");
        Require(first.RequestMove(host, 1), "Third turn");
        // i: 3手目を全員へ反映する更新番号
        for (int i = 0; i < 20; ++i) step();
        Require(second.RequestMove(client, 5), "Fourth turn");
        // i: 4手目を全員へ反映する更新番号
        for (int i = 0; i < 20; ++i) step();
        Require(first.RequestMove(host, 2), "Winning turn");
        // i: 勝利手を全員へ反映する更新番号
        for (int i = 0; i < 20; ++i) step();
        Require(first.Winner() == 1 && second.Winner() == 1 && watching.Winner() == 1 && host.Objects().empty(), "Complete turn game with no scene objects");
    }
    // RoomDiscovery()はLAN検索・一覧更新・部屋参加を検証する。
    void RoomDiscovery()
    {
        // LAN公開とDirect接続を試す設定
        NetworkConfiguration config; config.backend = NetworkBackend::Direct; config.port = 0;
        config.advertiseLan = true; config.roomName = "Public LAN room"; config.discoveryPort = 27849;
        // 検索対象のホストと参加クライアント
        NetworkSession host, client;
        // LANルーム検索ブラウザー
        NetworkRoomBrowser browser;
        Require(host.Configure(config) && client.Configure(config) && host.Host("Host", "0.0.0.0"), "Discoverable host"); host.Update(0);
        Require(browser.Start(config, NetworkDiscoveryScope::SameComputer), "Same-PC discovery start");
        // 検索の期限
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        // ルームが見つかるまでホストとブラウザーを更新する
        while (browser.Rooms().empty())
        {
            browser.Update(0.005f); host.Update(0.005f);
            // 検索タイムアウトを検出する
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Discovery timeout: " + browser.LastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // 検索結果の先頭ルーム
        const auto room = browser.Rooms().front();
        Require(room.name == config.roomName && room.players == 1 && room.capacity == 4 && room.connection.starts_with("LPD1|127."), "Room metadata and source-derived connection");
        // 別ゲームのルーム情報
        auto foreign = room; foreign.gameId = "different.game";
        Require(!client.JoinRoom(foreign), "Directory game mismatch rejected");
        Require(client.JoinRoom(room), "Join by room selection without copying invitation");
        Until(host, client, [&] { return client.State() == NetworkState::Connected; });
        browser.Refresh();
        // i: 容量更新を確認する検索フレーム番号
        for (int i = 0; i < 30; ++i) { browser.Update(0.005f); host.Update(0.005f); client.Update(0.005f); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        Require(browser.Rooms().size() == 1 && browser.Rooms().front().players == 2, "Discovery deduplicates and refreshes capacity");
        browser.Stop(); Require(browser.Rooms().empty() && !browser.IsSearching(), "Browser stop");
    }
}
// Direct接続・暗号化・ターン制状態とLAN検索を検証する
int main()
{
    // テスト失敗を終了コードに変換する
    try
    {
        CryptoVectors(); EndpointAndSettings(); MappingOwnership(); DirectConnection("127.0.0.1"); DirectConnection("::1");
        TurnBasedGame(); RoomDiscovery();
        std::cout << "Direct authenticated IPv4/IPv6, crypto vectors, tamper/replay, turn-based state, LAN discovery and lease ownership passed.\n";
        // テスト成功を返す
        return 0;
    }
    // 例外内容を出力して失敗終了する
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
