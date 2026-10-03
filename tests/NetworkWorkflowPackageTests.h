#pragma once

#include "../packages/src/network-session-workflow/NetworkProfile.h"
#include "LamaPon/Online/NetworkSettingsJson.h"

// TestNetworkWorkflowPackage(graphics:描画デバイス, sourceRoot:パッケージ元, outputRoot:出力先)はNetwork Workflowの同梱動作を検証する。
inline void TestNetworkWorkflowPackage(LamaPon::GraphicsDevice& graphics,
    const std::filesystem::path& sourceRoot, const std::filesystem::path& outputRoot)
{
    using namespace LamaPon;
    using namespace LamaPonNetworkWorkflow;
    // require(result: 判定条件, message: 失敗理由)は不成立時に例外を送出する。
    const auto require = [](const bool result, const char* message)
    {
        // 条件違反を検出する
        if (!result) throw std::runtime_error(message);
    };
    // 複数プロファイルで共有する接続設定
    NetworkConfiguration shared;
    shared.gameId = "test.workflow";
    shared.gameVersion = "v2";
    shared.eosProductId = "product";
    shared.eosSandboxId = "sandbox";
    shared.eosDeploymentId = "deployment";
    shared.eosClientId = "client";
    graphics.Assets().SetAssetRoot(sourceRoot, false);
    // プロファイル読込エラー
    std::string error;
    // 読み込む各プロファイルの設定先
    NetworkConfiguration configuration;
    // 読み込んだプロファイル数
    std::size_t profiles{};
    // entry: 配布プロファイル項目を検証する
    for (const auto& entry : std::filesystem::directory_iterator(sourceRoot / "profiles"))
    {
        // 配布プロファイルのDataAsset
        const auto asset = graphics.Assets().LoadDataAsset(
            std::filesystem::relative(entry.path(), sourceRoot));
        require(ReadProfile(*asset, shared, configuration, error), "A shipped network profile is invalid.");
        require(configuration.gameId == shared.gameId && configuration.gameVersion == shared.gameVersion
            && configuration.eosProductId == shared.eosProductId, "Shared service settings were lost.");
        // 元プロファイルのJSON入力
        std::ifstream input(entry.path());
        // 元プロファイルのJSON内容
        nlohmann::json original;
        input >> original;
        // 共通設定にプロファイル値を上書きした期待値
        auto expected = Detail::NetworkSettingsToJson(shared);
        // key: 設定名, value: 設定値を元プロファイルから反映する
        for (const auto& [key, value] : original.at("values").items()) expected[key] = value;
        require(Detail::NetworkSettingsToJson(configuration)
            == Detail::NetworkSettingsToJson(Detail::NetworkSettingsFromJson(expected)),
            "A shipped network profile field did not round trip.");
        // 検証済みプロファイル数
        ++profiles;
    }
    require(profiles == 4, "Four network profiles must ship in the package.");
    // プロファイルスキーマ
    const auto schema = nlohmann::json::parse(ProfileSchema);
    // スキーマの既定値
    nlohmann::json defaults = nlohmann::json::object();
    // field: スキーマ項目の既定値を集める
    for (const auto& field : schema.at("fields")) defaults[field.at("name").get<std::string>()] = field.at("default");
    require(ReadProfile(DataAsset::FromJson(nlohmann::json{{"type",ProfileType},{"values",defaults}}.dump()),
        shared, configuration, error) && configuration.backend == NetworkBackend::Direct
        && configuration.port == 0 && !configuration.automaticPortMapping, "New network asset defaults are unsafe or invalid.");
    // values: 不正プロファイルの各設定を拒否する
    for (const auto& values : {
        R"({"backend":"Unknown"})", R"({"maxPlayers":2.5})", R"({"tickRate":true})",
        R"({"port":65536})", R"({"maxPlayers":5})", R"({"timeoutSeconds":0})",
        R"({"advertiseLan":"yes"})", R"({"prefabs":[{"key":"a","assetPath":"../outside.prefab.json"}]})",
        R"({"prefabs":[{"key":"a","assetPath":"a.prefab.json"},{"key":"a","assetPath":"b.prefab.json"}]})"})
    {
        // 読込前の設定値
        const auto before = Detail::NetworkSettingsToJson(configuration);
        require(!ReadProfile(DataAsset::FromJson(std::string("{\"type\":\"Network.ConnectionProfile\",\"values\":")
            + values + "}"), shared, configuration, error) && !error.empty()
            && Detail::NetworkSettingsToJson(configuration) == before, "Malformed profile must fail atomically.");
    }
    require(!ReadProfile(DataAsset::FromJson(R"({"type":"Other","values":{"port":0}})"),
        shared, configuration, error), "Wrong asset type was accepted.");
    require(ReadProfile(DataAsset::FromJson(R"({"type":"Network.ConnectionProfile","values":{
        "gameId":"asset.cannot.override","eos":{"productId":"override"},
        "prefabs":[{"key":"player","assetPath":"prefabs/player.prefab.json"}]}})"),
        shared, configuration, error) && configuration.gameId == shared.gameId
        && configuration.eosProductId == shared.eosProductId && configuration.prefabs.size() == 1,
        "Profile must inherit shared service fields and load prefab lists.");

    // パッケージを展開するアーカイブのベースパス
    const auto archiveRoot = outputRoot / "network-assets";
    // 生成するアーカイブファイル名
    auto archive = archiveRoot;
    archive += ".tpak";
    static_cast<void>(PackAssets(sourceRoot, archive, Crypto::ArchiveKey()));
    graphics.Assets().SetAssetRoot(archiveRoot, false);
    require(graphics.Assets().IsArchived(), "Network package archive was not mounted.");
    // 配布アーカイブから接続を作るセッション
    NetworkSession session;
    require(session.Configure(shared), "Shared configuration failed.");
    SetActiveNetworkSession(&session);
    // 終了時にアクティブセッションを解除するScope
    const struct ActiveScope final
    {
        // アクティブセッションを解除する
        ~ActiveScope() { SetActiveNetworkSession(nullptr); }
    } activeScope;
    {
        // プロファイル読込を試すScene
        Scene scene(graphics);
        // ネットワーク管理オブジェクト
        auto& manager = scene.CreateGameObject("Network Manager");
        // 配布Scriptの初期設定
        auto& script = manager.AddComponent<NativeScriptComponent>("Network.SessionController",
            R"({"profile":"profiles/DirectLocal.asset.json","playerName":"Host"})");
        scene.Update(0.01f);
        require(session.State() == NetworkState::Stopped && session.Configuration().backend == NetworkBackend::Direct
            && session.Configuration().gameId == shared.gameId && session.Configuration().port == 0,
            "Controller must apply its archived profile without connecting automatically.");
        require(nlohmann::json::parse(script.SerializedProperties()).at("profile") == "profiles/DirectLocal.asset.json"
            && scene.Events().SubscriptionCount() == 3, "Controller properties and events did not round trip.");
        // 二重登録を試すネットワーク管理オブジェクト
        auto& duplicate = scene.CreateGameObject("Duplicate Manager").AddComponent<NativeScriptComponent>(
            "Network.SessionController", R"({"profile":"profiles/LanLocal.asset.json"})");
        scene.Update(0.01f);
        require(session.Configuration().backend == NetworkBackend::Direct && scene.Events().SubscriptionCount() == 3,
            "Duplicate controller overwrote the first profile or subscribed events.");
        // 二重登録されたScriptコンポーネント
        // duplicateは対象Scriptの有効状態を切り替える
        duplicate.SetEnabled(false);
        scene.Update(0.01f);
        scene.Events().Publish("Network.Host");
        session.Update(0);
        require(session.State() == NetworkState::Hosting, "Host event did not start encrypted loopback hosting.");
        // 接続世代を比較する基準値
        const auto generation = session.Generation();
        scene.Events().Publish("Network.Host");
        require(session.Generation() == generation, "Repeated host event restarted a live room.");
        // 既存APIでルームへ参加するクライアント
        NetworkSession client;
        require(client.Configure(session.Configuration()) && client.Join(session.ConnectionCode(), "Client"),
            "An existing API client could not join the controller room.");
        // クライアント接続の期限
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        // クライアントが接続するまでセッションを更新する
        while (client.State() != NetworkState::Connected)
        {
            session.Update(0.005f); client.Update(0.005f);
            // 接続の期限を確認する
            require(std::chrono::steady_clock::now() < deadline, "Encrypted controller room join timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(client.Members().size() == 2, "Client membership is incomplete.");
        script.SetEnabled(false);
        scene.Update(0.01f);
        require(session.State() == NetworkState::Stopped && scene.Events().SubscriptionCount() == 0,
            "Disabling the manager must disconnect and unsubscribe.");
        scene.Events().Publish("Network.Host");
        require(session.State() == NetworkState::Stopped, "Disabled manager accepted a host event.");
        script.SetEnabled(true);
        scene.Update(0.01f);
        scene.Events().Publish("Network.Host"); session.Update(0);
        require(session.State() == NetworkState::Hosting && scene.Events().SubscriptionCount() == 3,
            "Re-enabled manager did not reacquire the session exactly once.");
        scene.Events().Publish("Network.Leave");
        require(session.State() == NetworkState::Stopped, "Leave event did not stop the room.");
        scene.DestroyGameObject(manager);
        require(scene.Events().SubscriptionCount() == 0, "Destroyed controller retained event callbacks.");

        // 参加側Scriptへ実行時接続情報を渡す。
        // リモートホストを作るセッション
        NetworkSession remoteHost;
        require(remoteHost.Configure(session.Configuration()) && remoteHost.Host("RemoteHost"), "Remote host start failed.");
        remoteHost.Update(0);
        // 接続先を受け取るゲームオブジェクト
        auto& joiner = scene.CreateGameObject("Joining Manager");
        // 接続イベントを受け取るScript
        auto& joinScript = joiner.AddComponent<NativeScriptComponent>("Network.SessionController",
            R"({"profile":"profiles/DirectLocal.asset.json"})");
        scene.Update(0.01f);
        // 接続イベントに渡す引数
        EventArgs args;
        args.text = remoteHost.ConnectionCode();
        scene.Events().Publish("Network.Join", args);
        // リモート接続の期限
        const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        // joinイベントの接続完了まで双方を更新する
        while (session.State() != NetworkState::Connected)
        {
            remoteHost.Update(0.005f); session.Update(0.005f);
            // 接続の期限を確認する
            require(std::chrono::steady_clock::now() < joinDeadline, "Controller join event timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(joinScript.SerializedProperties().find(args.text) == std::string::npos,
            "Secret connection information must never enter Scene properties.");
        scene.DestroyGameObject(joiner);
        require(session.State() == NetworkState::Stopped, "Scene object destruction did not close its connection.");

        // 不正なプロファイルを指定するオブジェクト
        auto& invalid = scene.CreateGameObject("Invalid Profile");
        invalid.AddComponent<NativeScriptComponent>("Network.SessionController", R"({"profile":"missing.asset.json"})");
        scene.Update(0.01f); scene.Events().Publish("Network.Host");
        require(session.State() == NetworkState::Stopped && scene.Events().SubscriptionCount() == 0,
            "Invalid profiles must not subscribe or connect.");
        scene.DestroyGameObject(invalid);
        // 既存接続を所有しない管理対象
        auto& owned = scene.CreateGameObject("Already connected");
        require(session.Host("ExistingHost"), "Existing connection start failed.");
        session.Update(0);
        owned.AddComponent<NativeScriptComponent>("Network.SessionController", R"({"profile":"profiles/LanLocal.asset.json"})");
        scene.Update(0.01f); scene.DestroyGameObject(owned);
        require(session.State() == NetworkState::Hosting, "Controller stopped a connection it never acquired.");
        session.Stop();

        // 同期中のためセッションを所有しないオブジェクト
        auto& synchronized = scene.CreateGameObject("Synchronized Manager");
        synchronized.AddComponent<NetworkIdentityComponent>();
        synchronized.AddComponent<NativeScriptComponent>("Network.SessionController",
            R"({"profile":"profiles/DirectLocal.asset.json"})");
        scene.Update(0.01f);
        require(scene.Events().SubscriptionCount() == 0, "A synchronized object must not own the network session.");
    }
    {
        // アーカイブ内DemoSceneとUIイベントの連携を確認する
        // シーンJSONのバイト列
        const auto bytes = graphics.Assets().ReadFileBytes("scenes/NetworkDemo.scene.json");
        // 読み込んだDemoScene
        auto demo = nlohmann::json::parse(bytes.begin(), bytes.end());
        // object: 各GameObjectのScript設定を検証用プロファイルへ変更する
        for (auto& object : demo.at("objects"))
            // component: 各GameObjectのコンポーネントを調べる
            for (auto& component : object.at("components"))
                // NativeScriptだけを切り替える
                if (component.at("type") == "NativeScript")
                    component["properties"]["profile"] = "profiles/DirectLocal.asset.json";
        // DemoSceneを読み込むScene
        Scene scene(graphics);
        scene.LoadFromJson(demo.dump());
        scene.Update(0.01f);
        // 読み込んだHostボタン
        const auto* hostButton = scene.FindGameObjectByName("Host Room");
        // 読み込んだLeaveボタン
        const auto* leaveButton = scene.FindGameObjectByName("Leave Room");
        require(hostButton && leaveButton, "Demo buttons were not loaded.");
        scene.Events().Publish(hostButton->GetComponent<UIButtonComponent>()->ClickEventName());
        session.Update(0);
        require(session.State() == NetworkState::Hosting, "Demo host button event is not wired to its controller.");
        scene.Events().Publish(leaveButton->GetComponent<UIButtonComponent>()->ClickEventName());
        require(session.State() == NetworkState::Stopped, "Demo leave button event is not wired to its controller.");
    }
    {
        // Scene破棄後にセッション参照と購読が残らないことを確認する
        // Sceneより先に破棄するセッション
        auto shorterSession = std::make_unique<NetworkSession>();
        SetActiveNetworkSession(shorterSession.get());
        // 破棄順序を検証するScene
        Scene scene(graphics);
        // セッション破棄前の管理オブジェクト
        auto& manager = scene.CreateGameObject("Shutdown Order");
        manager.AddComponent<NativeScriptComponent>("Network.SessionController",
            R"({"profile":"profiles/DirectLocal.asset.json"})");
        scene.Update(0.01f);
        require(scene.Events().SubscriptionCount() == 3, "Shutdown order test did not activate its controller.");
        shorterSession.reset();
        scene.DestroyGameObject(manager);
        require(ActiveNetworkSession() == nullptr && scene.Events().SubscriptionCount() == 0,
            "Shutdown retained a session pointer or event subscription.");
    }
    graphics.Assets().SetAssetRoot(sourceRoot, false);
    std::filesystem::remove(archive);
}
