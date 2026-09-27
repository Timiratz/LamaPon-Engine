#pragma once

#include "../packages/src/network-session-workflow/NetworkProfile.h"

inline void TestNetworkWorkflowPackage(LamaPon::GraphicsDevice& graphics,
    const std::filesystem::path& sourceRoot, const std::filesystem::path& outputRoot)
{
    using namespace LamaPon;
    using namespace LamaPonNetworkWorkflow;
    const auto require = [](const bool result, const char* message)
    {
        if (!result) throw std::runtime_error(message);
    };
    NetworkConfiguration shared;
    shared.gameId = "test.workflow";
    shared.gameVersion = "v2";
    shared.eosProductId = "product";
    shared.eosSandboxId = "sandbox";
    shared.eosDeploymentId = "deployment";
    shared.eosClientId = "client";
    graphics.Assets().SetAssetRoot(sourceRoot, false);
    std::string error;
    NetworkConfiguration configuration;
    std::size_t profiles{};
    for (const auto& entry : std::filesystem::directory_iterator(sourceRoot / "profiles"))
    {
        const auto asset = graphics.Assets().LoadDataAsset(
            std::filesystem::relative(entry.path(), sourceRoot));
        require(ReadProfile(*asset, shared, configuration, error), "A shipped network profile is invalid.");
        require(configuration.gameId == shared.gameId && configuration.gameVersion == shared.gameVersion
            && configuration.eosProductId == shared.eosProductId, "Shared service settings were lost.");
        std::ifstream input(entry.path());
        nlohmann::json original;
        input >> original;
        auto expected = Detail::NetworkSettingsToJson(shared);
        for (const auto& [key, value] : original.at("values").items()) expected[key] = value;
        require(Detail::NetworkSettingsToJson(configuration)
            == Detail::NetworkSettingsToJson(Detail::NetworkSettingsFromJson(expected)),
            "A shipped network profile field did not round trip.");
        ++profiles;
    }
    require(profiles == 4, "Four network profiles must ship in the package.");
    const auto schema = nlohmann::json::parse(ProfileSchema);
    nlohmann::json defaults = nlohmann::json::object();
    for (const auto& field : schema.at("fields")) defaults[field.at("name").get<std::string>()] = field.at("default");
    require(ReadProfile(DataAsset::FromJson(nlohmann::json{{"type",ProfileType},{"values",defaults}}.dump()),
        shared, configuration, error) && configuration.backend == NetworkBackend::Direct
        && configuration.port == 0 && !configuration.automaticPortMapping, "New network asset defaults are unsafe or invalid.");
    for (const auto& values : {
        R"({"backend":"Unknown"})", R"({"maxPlayers":2.5})", R"({"tickRate":true})",
        R"({"port":65536})", R"({"maxPlayers":5})", R"({"timeoutSeconds":0})",
        R"({"advertiseLan":"yes"})", R"({"prefabs":[{"key":"a","assetPath":"../outside.prefab.json"}]})",
        R"({"prefabs":[{"key":"a","assetPath":"a.prefab.json"},{"key":"a","assetPath":"b.prefab.json"}]})"})
    {
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

    const auto archiveRoot = outputRoot / "network-assets";
    auto archive = archiveRoot;
    archive += ".tpak";
    static_cast<void>(PackAssets(sourceRoot, archive, Crypto::ArchiveKey()));
    graphics.Assets().SetAssetRoot(archiveRoot, false);
    require(graphics.Assets().IsArchived(), "Network package archive was not mounted.");
    NetworkSession session;
    require(session.Configure(shared), "Shared configuration failed.");
    SetActiveNetworkSession(&session);
    const struct ActiveScope final
    {
        ~ActiveScope() { SetActiveNetworkSession(nullptr); }
    } activeScope;
    {
        Scene scene(graphics);
        auto& manager = scene.CreateGameObject("Network Manager");
        auto& script = manager.AddComponent<NativeScriptComponent>("Network.SessionController",
            R"({"profile":"profiles/DirectLocal.asset.json","playerName":"Host"})");
        scene.Update(0.01f);
        require(session.State() == NetworkState::Stopped && session.Configuration().backend == NetworkBackend::Direct
            && session.Configuration().gameId == shared.gameId && session.Configuration().port == 0,
            "Controller must apply its archived profile without connecting automatically.");
        require(nlohmann::json::parse(script.SerializedProperties()).at("profile") == "profiles/DirectLocal.asset.json"
            && scene.Events().SubscriptionCount() == 3, "Controller properties and events did not round trip.");
        auto& duplicate = scene.CreateGameObject("Duplicate Manager").AddComponent<NativeScriptComponent>(
            "Network.SessionController", R"({"profile":"profiles/LanLocal.asset.json"})");
        scene.Update(0.01f);
        require(session.Configuration().backend == NetworkBackend::Direct && scene.Events().SubscriptionCount() == 3,
            "Duplicate controller overwrote the first profile or subscribed events.");
        duplicate.SetEnabled(false);
        scene.Update(0.01f);
        scene.Events().Publish("Network.Host");
        session.Update(0);
        require(session.State() == NetworkState::Hosting, "Host event did not start encrypted loopback hosting.");
        const auto generation = session.Generation();
        scene.Events().Publish("Network.Host");
        require(session.Generation() == generation, "Repeated host event restarted a live room.");
        NetworkSession client;
        require(client.Configure(session.Configuration()) && client.Join(session.ConnectionCode(), "Client"),
            "An existing API client could not join the controller room.");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (client.State() != NetworkState::Connected)
        {
            session.Update(0.005f); client.Update(0.005f);
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

        // ホストは既存API、参加側は配布Scriptで開始し、接続情報を実行時イベントで渡します。
        NetworkSession remoteHost;
        require(remoteHost.Configure(session.Configuration()) && remoteHost.Host("RemoteHost"), "Remote host start failed.");
        remoteHost.Update(0);
        auto& joiner = scene.CreateGameObject("Joining Manager");
        auto& joinScript = joiner.AddComponent<NativeScriptComponent>("Network.SessionController",
            R"({"profile":"profiles/DirectLocal.asset.json"})");
        scene.Update(0.01f);
        EventArgs args;
        args.text = remoteHost.ConnectionCode();
        scene.Events().Publish("Network.Join", args);
        const auto joinDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (session.State() != NetworkState::Connected)
        {
            remoteHost.Update(0.005f); session.Update(0.005f);
            require(std::chrono::steady_clock::now() < joinDeadline, "Controller join event timed out.");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(joinScript.SerializedProperties().find(args.text) == std::string::npos,
            "Secret connection information must never enter Scene properties.");
        scene.DestroyGameObject(joiner);
        require(session.State() == NetworkState::Stopped, "Scene object destruction did not close its connection.");

        auto& invalid = scene.CreateGameObject("Invalid Profile");
        invalid.AddComponent<NativeScriptComponent>("Network.SessionController", R"({"profile":"missing.asset.json"})");
        scene.Update(0.01f); scene.Events().Publish("Network.Host");
        require(session.State() == NetworkState::Stopped && scene.Events().SubscriptionCount() == 0,
            "Invalid profiles must not subscribe or connect.");
        scene.DestroyGameObject(invalid);
        auto& owned = scene.CreateGameObject("Already connected");
        require(session.Host("ExistingHost"), "Existing connection start failed.");
        session.Update(0);
        owned.AddComponent<NativeScriptComponent>("Network.SessionController", R"({"profile":"profiles/LanLocal.asset.json"})");
        scene.Update(0.01f); scene.DestroyGameObject(owned);
        require(session.State() == NetworkState::Hosting, "Controller stopped a connection it never acquired.");
        session.Stop();

        auto& synchronized = scene.CreateGameObject("Synchronized Manager");
        synchronized.AddComponent<NetworkIdentityComponent>();
        synchronized.AddComponent<NativeScriptComponent>("Network.SessionController",
            R"({"profile":"profiles/DirectLocal.asset.json"})");
        scene.Update(0.01f);
        require(scene.Events().SubscriptionCount() == 0, "A synchronized object must not own the network session.");
    }
    {
        // パッケージ単体をアーカイブ化しているため、インストール先の接頭辞だけ
        // 取り除き、同梱SceneをそのままデシリアライズしてUIイベントとの連携を確認します。
        const auto bytes = graphics.Assets().ReadFileBytes("scenes/NetworkDemo.scene.json");
        auto demo = nlohmann::json::parse(bytes.begin(), bytes.end());
        for (auto& object : demo.at("objects"))
            for (auto& component : object.at("components"))
                if (component.at("type") == "NativeScript")
                    component["properties"]["profile"] = "profiles/DirectLocal.asset.json";
        Scene scene(graphics);
        scene.LoadFromJson(demo.dump());
        scene.Update(0.01f);
        const auto* hostButton = scene.FindGameObjectByName("Host Room");
        const auto* leaveButton = scene.FindGameObjectByName("Leave Room");
        require(hostButton && leaveButton, "Demo buttons were not loaded.");
        scene.Events().Publish(hostButton->GetComponent<UIButtonComponent>()->ClickEventName());
        session.Update(0);
        require(session.State() == NetworkState::Hosting, "Demo host button event is not wired to its controller.");
        scene.Events().Publish(leaveButton->GetComponent<UIButtonComponent>()->ClickEventName());
        require(session.State() == NetworkState::Stopped, "Demo leave button event is not wired to its controller.");
    }
    {
        // Applicationと同じくセッションをSceneより先に破棄しても、Scriptが
        // 保存したポインターを参照せず購読と所有権を片付けることを確認します。
        auto shorterSession = std::make_unique<NetworkSession>();
        SetActiveNetworkSession(shorterSession.get());
        Scene scene(graphics);
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
