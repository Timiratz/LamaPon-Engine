#include <WinSock2.h>
#include <WS2tcpip.h>
#include "LamaPon/Online/NetworkSession.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    // 条件不成立なら説明付きでテストを失敗させます。
    // Require(value: 判定条件, message: 失敗理由)
    void Require(const bool value, const char* message)
    {
        // 失敗条件
        if (!value) throw std::runtime_error(message);
    }
    // 条件が成立するまで両端の通信を進めます。
    // Until(predicate: 完了条件, host: ホスト, client: 接続先, third: 第三接続先)
    template<class Predicate>
    void Until(Predicate predicate, LamaPon::NetworkSession& host,
        LamaPon::NetworkSession& client, LamaPon::NetworkSession* third = nullptr)
    {
        // 接続待ちの期限
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        // 条件成立または期限切れまで更新
        while (!predicate())
        {
            host.Update(0.005f); client.Update(0.005f);
            // 第三接続先がある場合に更新
            if (third) third->Update(0.005f);
            // 待機期限を超えた場合
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("Timed out: " + host.LastError() + " / " + client.LastError());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // 接続、所有権、複製、切断、再起動を検証します。
    void ConnectionAndReplication()
    {
        using namespace LamaPon;
        // 共通の接続設定
        NetworkConfiguration config;
        config.port = 0;
        // host: 接続受付、client・third・fourth: 通常接続、excess: 人数上限検査
        NetworkSession host, client, third, fourth, excess;
        Require(host.Configure(config), "Host configuration");
        Require(host.Host("Host"), "Host start");
        host.Update(0);
        Require(host.IsHost() && host.LocalPeer() == 1, "Host state");
        // 最初に複製するオブジェクト
        NetworkObjectState fixed;
        fixed.sceneKey = "world.box";
        fixed.transform.position[0] = 5;
        // ホスト上の固定オブジェクト識別子
        const auto fixedId = host.Spawn(fixed);
        Require(fixedId != 0, "Host spawn");
        Require(client.Configure(config) && client.Join(host.RoomAddress(), "Second"), "Client join");
        Until([&] { return client.State() == NetworkState::Connected; }, host, client);
        Require(client.Members().size() == 2 && client.FindObject(fixedId)
            && client.FindObject(fixedId)->transform.position[0] == 5, "Late join baseline");
        Require(client.Spawn(fixed) == 0 && !client.Despawn(fixedId), "Client cannot mutate world");
        // クライアント所有のオブジェクト
        NetworkObjectState player;
        player.prefabKey = "player";
        player.owner = client.LocalPeer();
        // クライアント所有オブジェクト識別子
        const auto playerId = host.Spawn(player);
        Until([&] { return client.FindObject(playerId) != nullptr; }, host, client);
        Require(!client.SendInput(fixedId, "move", "1"), "Ownership rejection");
        Require(client.SendInput(playerId, "move", "1"), "Owned input");
        // 入力イベントの受信状態
        bool input{};
        Until([&]
        {
            // ホストに届いたイベント
            NetworkEvent event;
            // 入力イベントを読み切る
            while (host.PollEvent(event))
                // 入力イベントだけを検証
                if (event.kind == NetworkEventKind::Input)
                {
                    Require(event.peer == client.LocalPeer() && event.object == playerId
                        && event.name == "move" && event.data == "1", "Input attribution");
                    // 入力受信済み状態
                    input = true;
                }
            return input;
        }, host, client);
        // 変更対象の複製状態
        auto changed = *host.FindObject(fixedId);
        changed.transform.position[0] = 42;
        Require(host.SetObject(changed), "State change");
        Until([&] { return client.FindObject(fixedId)->transform.position[0] == 42; }, host, client);
        Require(host.BroadcastEvent("score", "12"), "Broadcast");
        // ブロードキャスト受信状態
        bool broadcast{};
        Until([&]
        {
            // クライアントに届いたイベント
            NetworkEvent event;
            // ゲームイベントを読み切る
            while (client.PollEvent(event))
                // 対象イベントの受信状態
                if (event.kind == NetworkEventKind::GameEvent) broadcast = event.name == "score" && event.data == "12";
            return broadcast;
        }, host, client);
        Require(third.Configure(config) && third.Join(host.RoomAddress(), "Third"), "Third join");
        Until([&] { return third.State() == NetworkState::Connected; }, host, client, &third);
        Require(third.FindObject(fixedId)->transform.position[0] == 42, "Current baseline");
        Require(fourth.Configure(config) && fourth.Join(host.RoomAddress(), "Fourth"), "Fourth join");
        Until([&] { return fourth.State() == NetworkState::Connected; }, host, fourth);
        Require(excess.Configure(config) && excess.Join(host.RoomAddress(), "Fifth"), "Excess connection");
        Until([&] { return excess.State() == NetworkState::Error; }, host, excess);
        Require(host.Members().size() == 4, "Player limit");
        client.Stop();
        Until([&] { return host.Members().size() == 3 && third.FindObject(playerId) == nullptr; }, host, third);
        Require(host.FindObject(playerId) == nullptr, "Owned objects removed on departure");
        Require(host.Despawn(fixedId), "Despawn");
        Until([&] { return third.FindObject(fixedId) == nullptr; }, host, third);
        host.Stop();
        Until([&] { return third.State() == NetworkState::Error; }, host, third);
        Require(third.Objects().empty() && third.LocalPeer() == 0, "Host exit cleanup");
        Require(host.Host("Restart"), "Host restart");
        host.Update(0);
        Require(host.Members().size() == 1 && host.Objects().empty(), "Clean restart");
    }
    // ゲーム設定の相違を接続拒否で検証します。
    void IncompatibleGames()
    {
        using namespace LamaPon;
        // 各設定項目の不一致を順に検証
        for (int difference = 0; difference < 3; ++difference)
        {
            // host: 接続受付、client: 不一致設定で接続
            NetworkSession host, client;
            // 不一致を適用する接続設定
            NetworkConfiguration config;
            config.port = 0;
            Require(host.Configure(config) && host.Host(), "Mismatch host");
            host.Update(0);
            // ゲーム識別子の不一致
            if (difference == 0) config.gameId = "another.game";
            // ゲーム版の不一致
            if (difference == 1) config.gameVersion = "2";
            // シーン識別子の不一致
            if (difference == 2) config.sceneId = "other";
            Require(client.Configure(config) && client.Join(host.RoomAddress()), "Mismatch client");
            Until([&] { return client.State() == NetworkState::Error; }, host, client);
            Require(host.Members().size() == 1, "Incompatible peer admitted");
        }
    }
    // 設定、オブジェクト数、イベント数の上限を検証します。
    void Bounds()
    {
        using namespace LamaPon;
        // 上限検証用セッション
        NetworkSession session;
        // 無効値を設定する構成
        NetworkConfiguration invalid;
        invalid.maxPlayers = 5;
        Require(!session.Configure(invalid), "Configuration limits");
        invalid.maxPlayers = 4; invalid.port = 0;
        Require(session.Configure(invalid) && session.Host(), "Bounds host");
        session.Update(0);
        // 複製上限を検証するオブジェクト
        NetworkObjectState object;
        object.sceneKey = "unique";
        Require(session.Spawn(object) != 0 && session.Spawn(object) == 0, "Duplicate scene key");
        object.sceneKey.clear(); object.prefabKey = "player";
        object.data.assign(257, 'a');
        Require(session.Spawn(object) == 0, "Payload bound");
        object.data.clear(); object.owner = 99;
        Require(session.Spawn(object) == 0, "Unknown owner");
        object.owner = 1;
        // オブジェクト上限まで生成
        for (int index = 1; index < 128; ++index) Require(session.Spawn(object) != 0, "Object capacity");
        Require(session.Spawn(object) == 0, "Object limit");
        // 最大状態を受け取る後発クライアント
        NetworkSession late;
        Require(late.Configure(invalid) && late.Join(session.RoomAddress()), "Maximum baseline join");
        Until([&] { return late.State() == NetworkState::Connected; }, session, late);
        Require(late.Objects().size() == 128, "Complete maximum-size baseline before connected");
        late.Stop();
        // 古いイベントを破棄する受け皿
        NetworkEvent event;
        // 既存イベントを読み切る
        while (session.PollEvent(event)) {}
        // イベント上限まで送信
        for (int index = 0; index < 512; ++index) Require(session.BroadcastEvent("event", "data"), "Event capacity");
        Require(!session.BroadcastEvent("event", "overflow"), "Event queue bound");
        Require(!session.Configure(invalid), "Cannot reconfigure active session");
        session.Stop();
        SetActiveNetworkSession(&session);
        Require(ActiveNetworkSession() == &session, "Active API");
        SetActiveNetworkSession(nullptr);
    }

    struct RawPeer final
    {
        // 生TCP接続ソケット
        // 未加工通信に使うソケット
        SOCKET socket{ INVALID_SOCKET };
        // 指定アドレスへ未加工接続します。
        // RawPeer(address: 接続先アドレス)
        explicit RawPeer(const std::string& address)
        {
            socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            Require(socket != INVALID_SOCKET, "Raw socket");
            // サーバー接続先
            // 接続先IPv4エンドポイント
            sockaddr_in endpoint{};
            endpoint.sin_family = AF_INET;
            endpoint.sin_port = htons(static_cast<unsigned short>(std::stoi(address.substr(address.find(':') + 1))));
            InetPtonA(AF_INET, "127.0.0.1", &endpoint.sin_addr);
            Require(connect(socket, reinterpret_cast<sockaddr*>(&endpoint), sizeof(endpoint)) == 0, "Raw connect");
        }
        // ソケットを閉じます。
        ~RawPeer()
        {
            // 有効なソケットを閉じる
            if (socket != INVALID_SOCKET) closesocket(socket);
        }
        // バイト列を全て送信します。
        // Wire(bytes: 送信データ)
        void Wire(const std::string& bytes) const
        {
            // 次に送る位置
            std::size_t offset{};
            // 未送信部分がなくなるまで送信
            while (offset < bytes.size())
            {
                // 今回送れたバイト数
                const int sent = send(socket, bytes.data() + offset, static_cast<int>(bytes.size() - offset), 0);
                Require(sent > 0, "Raw send"); offset += static_cast<std::size_t>(sent);
            }
        }
        // プロトコルの長さ付きフレームを作ります。
        // Frame(message: フレーム本文)
        static std::string Frame(const std::string& message)
        {
            // 4バイト長ヘッダー付きデータ
            std::string bytes(4, '\0');
            // 本文のバイト長
            const auto length = static_cast<std::uint32_t>(message.size());
            // 長さをネットワークバイト順へ格納
            for (std::uint32_t index = 0; index < 4; ++index)
                bytes[index] = static_cast<char>((length >> (24 - index * 8)) & 255);
            return bytes + message;
        }
    };

    // 不正フレーム、部分受信、過剰入力の隔離を検証します。
    void MalformedTraffic()
    {
        using namespace LamaPon;
        // 未加工接続用の認証メッセージ
        constexpr auto hello = R"({"op":"hello","protocol":2,"game":"lamapon.game","version":"1","scene":"main","name":"Raw"})";
        // 拒否されるべき攻撃データ
        const std::vector<std::string> attacks{
            "{broken", R"({"op":"unknown"})", R"({"op":"input","id":1,"name":"move","data":"1"})",
            R"({"op":"input","id":-1,"name":"move","data":"1"})",
            R"({"op":"input","id":1.0,"name":"move","data":"1"})",
            R"({"op":"input","id":4294967296,"name":"move","data":"1"})",
            R"({"op":"object","id":1})", R"({"op":"ping","id":null})", R"({"op":"pong","id":-1})",
            R"({"op":"ping","id":1,"extra":[[[[[[[[[[0]]]]]]]]]]})"
        };
        // 攻撃データごとに独立したホストで検証
        for (const auto& attack : attacks)
        {
            // 攻撃検証用ホスト設定
            NetworkConfiguration config; config.port = 0;
            // host: 攻撃対象、healthy: 正常接続の維持
            NetworkSession host, healthy;
            Require(host.Configure(config) && host.Host(), "Malformed host"); host.Update(0);
            // 攻撃検証中も保持するワールド状態
            NetworkObjectState fixed; fixed.sceneKey = "world.box";
            // 固定オブジェクト識別子
            const auto id = host.Spawn(fixed);
            Require(healthy.Configure(config) && healthy.Join(host.RoomAddress()), "Healthy peer");
            Until([&] { return healthy.State() == NetworkState::Connected; }, host, healthy);
            // 未加工TCP接続
            RawPeer raw(host.RoomAddress());
            // 同一受信batchで認証と攻撃が届いても、切断後のデータを採用しません。
            raw.Wire(RawPeer::Frame(hello) + RawPeer::Frame(attack));
            Until([&] { return host.Statistics().rejectedMessages != 0; }, host, healthy);
            Require(host.IsHost() && host.Members().size() == 2 && host.FindObject(id)
                && host.FindObject(id)->transform.position[0] == 0, "Malformed peer isolated");
            Require(host.BroadcastEvent("still.alive", "ok"), "Host survives malformed traffic");
        }
        // 不正なフレーム長を検証
        for (const std::uint32_t length : { 0u, 1101u, 0xffffffffu })
        {
            // 不正長検証用ホスト設定
            NetworkConfiguration config; config.port = 0;
            // host: 不正フレーム受信、unused: 待機引数
            NetworkSession host, unused;
            Require(host.Configure(config) && host.Host(), "Invalid framing host"); host.Update(0);
            // 不正フレーム送信ソケット
            RawPeer raw(host.RoomAddress());
            // 長さだけを持つヘッダー
            std::string bytes(4, '\0');
            // 長さをネットワークバイト順へ格納
            for (std::uint32_t index = 0; index < 4; ++index)
                bytes[index] = static_cast<char>((length >> (24 - index * 8)) & 255);
            raw.Wire(bytes);
            // 切断処理が反映されるまで更新
            for (int index = 0; index < 20; ++index)
            { host.Update(0.01f); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            Require(host.IsHost() && host.Members().size() == 1, "Invalid frame not admitted");
            // 切断確認用受信バッファ
            char incoming{};
            // 非ブロッキング受信設定
            u_long nonblocking = 1; ioctlsocket(raw.socket, FIONBIO, &nonblocking);
            // 切断状態を表す受信結果
            const int received = recv(raw.socket, &incoming, 1, 0);
            Require(received == 0 || (received < 0 && WSAGetLastError() == WSAECONNRESET), "Invalid frame socket closed");
        }
        {
            // 部分フレーム検証用ホスト設定
            NetworkConfiguration config; config.port = 0;
            // host: 部分フレーム受信、unused: 待機引数
            NetworkSession host, unused;
            Require(host.Configure(config) && host.Host(), "Fragment host"); host.Update(0);
            // 未加工接続
            RawPeer raw(host.RoomAddress());
            // 認証メッセージのフレーム
            const auto bytes = RawPeer::Frame(hello);
            raw.Wire(bytes.substr(0, 2)); host.Update(0);
            raw.Wire(bytes.substr(2, 5)); host.Update(0);
            Require(host.Members().size() == 1, "Partial frame buffered");
            raw.Wire(bytes.substr(7));
            Until([&] { return host.Members().size() == 2; }, host, unused);
            // 過剰入力フレーム列
            std::string flood;
            // レート制限を超える入力を組み立て
            for (int index = 0; index < 300; ++index) flood += RawPeer::Frame(R"({"op":"ping","id":1})");
            raw.Wire(flood);
            Until([&] { return host.Members().size() == 1; }, host, unused);
            Require(host.Statistics().rejectedMessages != 0, "Input rate limit");
        }
    }

    // 必須の環境変数を取得し、一時バッファを消去します。
    // VerificationSetting(name: 環境変数名)
    std::string VerificationSetting(const char* name)
    {
        // CRTが確保する環境変数バッファ
        char* value{};
        // 環境変数バッファの長さ
        std::size_t size{};
        // 未設定または空の設定を拒否
        if (_dupenv_s(&value, &size, name) != 0 || !value || size <= 1)
        {
            // 確保済みバッファを解放
            if (value) std::free(value);
            throw std::runtime_error(std::string("Missing verification environment: ") + name);
        }
        // 呼び出し元へ返す設定値
        std::string setting(value);
        SecureZeroMemory(value, size);
        std::free(value);
        return setting;
    }

    // 実際のEOS資格情報でホスト作成と再起動を検証します。
    void EpicHostSmoke()
    {
        using namespace LamaPon;
        Require(HasEpicNetworkBackend(), "Configure an EOS SDK build before this manual test.");
        // EOSホスト検証設定
        NetworkConfiguration config;
        config.backend = NetworkBackend::EpicOnlineServices;
        config.gameId = "LamaPon.EOS.Verification";
        config.gameVersion = "1";
        config.timeoutSeconds = 60;
        config.eosProductId = VerificationSetting("LAMAPON_EOS_PRODUCT_ID");
        config.eosSandboxId = VerificationSetting("LAMAPON_EOS_SANDBOX_ID");
        config.eosDeploymentId = VerificationSetting("LAMAPON_EOS_DEPLOYMENT_ID");
        config.eosClientId = VerificationSetting("LAMAPON_EOS_CLIENT_ID");
        // EOSホストセッション
        NetworkSession host;
        Require(host.Configure(config), "EOS host verification configuration");
        // 再起動前の部屋識別子
        std::string previousRoom;
        // 部屋の新規発行を二度検証
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            // ホスト開始失敗時
            if (!host.Host("Verification"))
                throw std::runtime_error("EOS host start: " + host.LastError());
            // ホスト準備完了の期限
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
            // 前回の更新時刻
            auto previousTick = std::chrono::steady_clock::now();
            // ホスト準備完了まで更新
            while (!host.IsHost())
            {
                // 現在時刻
                const auto now = std::chrono::steady_clock::now();
                host.Update(std::chrono::duration<float>(now - previousTick).count());
                previousTick = now;
                // EOSログイン失敗時
                if (host.State() == NetworkState::Error)
                    throw std::runtime_error("EOS host login: " + host.LastError());
                // 準備待ちの期限切れ
                if (now > deadline) throw std::runtime_error("EOS host login timed out after 45 seconds.");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // 作成された部屋識別子
            const auto room = host.RoomAddress();
            Require(room.size() == 59 && room.substr(32, 3) == ":LP"
                && host.Members().size() == 1 && host.LocalPeer() == 1,
                "EOS host ready state and room format");
            // 再起動で別の部屋が発行されたことを確認
            if (!previousRoom.empty()) Require(room != previousRoom, "EOS restart uses a new room");
            previousRoom = room;
            // 部屋ID・資格情報をログに載せず、実際のReady到達だけを記録します。
            std::cout << "EOS online login and host ready passed (" << attempt + 1 << "/2).\n";
            host.Stop();
            Require(host.State() == NetworkState::Stopped && host.RoomAddress().empty()
                && host.Members().empty(), "EOS host stop releases session");
        }
        std::cout << "EOS online host creation, stop and restart passed. Peer connection is not tested.\n";
    }

    // EOS SDKの初期化、検証失敗、終了を反復確認します。
    void EpicSdkLifecycle()
    {
        using namespace LamaPon;
        // SDK非搭載ビルドでは対象外
        if (!HasEpicNetworkBackend()) return;
        // 実SDKの初期化を製品資格情報や通信なしで検証します。
        // 試験用の環境変数名
        constexpr auto variable = "LAMAPON_EOS_TEST_DUMMY_SECRET";
        struct EnvironmentGuard
        {
            // 復元する環境変数名
            const char* name;
            // 実行前の環境変数値
            std::string previous;
            // 実行前に設定されていたか
            bool existed{};
            // 実行前の値を退避します。
            // EnvironmentGuard(key: 環境変数名)
            explicit EnvironmentGuard(const char* key) : name(key)
            {
                // CRTが確保する環境変数バッファ
                char* value{};
                // 環境変数バッファの長さ
                std::size_t size{};
                // 設定済みの値を退避
                if (_dupenv_s(&value, &size, name) == 0 && value)
                {
                    previous = value;
                    existed = true;
                    SecureZeroMemory(value, size);
                    std::free(value);
                }
            }
    // 環境変数を戻し保存値を消去します。
    ~EnvironmentGuard()
            {
                _putenv_s(name, existed ? previous.c_str() : "");
                // 保存値があれば機密データを消去
                if (!previous.empty()) SecureZeroMemory(previous.data(), previous.size());
            }
        // restore: 実行前の環境変数状態を復元
        } restore(variable);
        // EOS SDKライフサイクル検証設定
        NetworkConfiguration config;
        config.backend = NetworkBackend::EpicOnlineServices;
        config.eosProductId = config.eosSandboxId = config.eosDeploymentId = config.eosClientId = "test";
        config.eosClientSecretEnvironment = variable;
        // SDK検証対象セッション
        NetworkSession session;
        Require(session.Configure(config), "EOS SDK configuration");
        Require(_putenv_s(variable, "") == 0, "Clear test environment");
        Require(!session.Host() && session.State() == NetworkState::Error
            && session.LastError().find("環境変数") != std::string::npos, "EOS missing credential");
        Require(_putenv_s(variable, "offline-test-placeholder") == 0, "Set dummy credential");
        // 不正なIDを使ったSDK初期化を反復
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            // SDK初期化の開始結果
            const auto started = session.Join("not-a-product-user:LP0123456789abcdef01234567");
            // 初期化が進みすぎた場合や期待エラー以外
            if (started || session.State() != NetworkState::Error
                || session.LastError().find("ユーザー形式") == std::string::npos)
                throw std::runtime_error("EOS SDK lifecycle attempt " + std::to_string(attempt)
                    + ": " + session.LastError());
            session.Stop();
            Require(session.State() == NetworkState::Stopped, "EOS restart after SDK validation failure");
        }
        std::cout << "EOS SDK real DLL initialization and shutdown (16 restarts) passed.\n";
    }

    // タイムアウト、入力検証、未搭載バックエンドを確認します。
    void TimeoutAndBackendAvailability()
    {
        using namespace LamaPon;
        // 接続タイムアウト検証設定
        NetworkConfiguration config; config.port = 0; config.timeoutSeconds = 5;
        // host: タイムアウト検証、client: 接続相手
        NetworkSession host, client;
        Require(host.Configure(config) && host.Host(), "Timeout host"); host.Update(0);
        Require(client.Configure(config) && client.Join(host.RoomAddress()), "Timeout join");
        Until([&] { return client.State() == NetworkState::Connected; }, host, client);
        // タイムアウトで削除する所有オブジェクト
        NetworkObjectState player; player.prefabKey = "player"; player.owner = client.LocalPeer();
        // タイムアウト対象識別子
        const auto id = host.Spawn(player);
        Until([&] { return client.FindObject(id) != nullptr; }, host, client);
        host.Update(0); host.Update(5.1f);
        Require(host.Members().size() == 1 && host.FindObject(id) == nullptr, "Silent peer expires with owned objects");
        Until([&] { return client.State() == NetworkState::Error; }, host, client);
        host.Stop();
        Require(!host.Host("bad\nname"), "Control characters rejected");
        Require(!host.Join("example.com"), "DNS not accepted as LAN address");
        // EOSバックエンド非搭載時のエラーを確認
        if (!HasEpicNetworkBackend())
        {
            config.backend = NetworkBackend::EpicOnlineServices;
            config.eosProductId = config.eosSandboxId = config.eosDeploymentId = config.eosClientId = "test";
            Require(host.Configure(config) && !host.Host() && host.State() == NetworkState::Error
                && !host.LastError().empty(), "Missing SDK reports actionable error");
        }
    }
}

// ネットワークセッション検証を実行します。
// main(argc: 引数数, argv: 引数一覧)
int main(const int argc, char* argv[])
{
    // 例外をテスト失敗として報告
    try
    {
        // 通常実行は外部接続せず、この指定時だけEOSホストを作成
        if (argc == 2 && std::string_view(argv[1]) == "--eos-host-smoke")
        {
            EpicHostSmoke();
            // 明示検証の成功
            return 0;
        }
        // 未知のコマンドライン引数を拒否
        if (argc != 1) throw std::runtime_error("Usage: LamaPonNetworkSessionTests [--eos-host-smoke]");
        ConnectionAndReplication(); IncompatibleGames(); Bounds(); MalformedTraffic(); TimeoutAndBackendAvailability(); EpicSdkLifecycle();
        std::cout << "P2P connection, baseline, ownership, replication, events, limits, departure and restart passed.\n";
        // 全ての検証が成功
        return 0;
    }
    // テスト例外を標準エラーへ出力
    // catch(error: 例外内容)
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
