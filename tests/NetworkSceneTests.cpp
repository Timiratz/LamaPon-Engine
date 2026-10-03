#include "LamaPon/LamaPon.h"
#include "../samples/Networking/P2PCooperativeController.h"
#include <objbase.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    struct ComSession final
    {
        // COM初期化の結果
        HRESULT result{ CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
        // 成功時だけCOMを終了する
        ~ComSession()
        {
            // 初期化成功時だけCOMを終了する
            if (SUCCEEDED(result)) CoUninitialize();
        }
    };
    // Require(condition: 条件, message: 失敗理由)は不成立時に例外を送出する。
    void Require(const bool condition, const char* message)
    {
        // 条件違反を検出する
        if (!condition) throw std::runtime_error(message);
    }

    struct StopScript final : LamaPon::Script
    {
        // ネットワーク停止処理を呼び出す
        void Stop() { StopNetwork(); }
    };

    class StopNetworkProbe final : public LamaPon::Component
    {
    public:
        // StopNetworkProbe(completed: 停止コールバック完了状態)は状態を記録する。
        explicit StopNetworkProbe(bool& completed) : m_completed(completed) {}
        // TypeName()はテスト用コンポーネント名を返す。
        std::string_view TypeName() const noexcept override { return "StopNetworkProbe"; }
    protected:
        // OnUpdate(deltaTime: 経過時間)は停止処理中の所有状態を検証する。
        void OnUpdate(float) override
        {
            // 停止を実行するテスト用スクリプト
            StopScript script;
            script.Stop();
            Require(Owner().GetScene().FindGameObject(Owner().Id()) == &Owner(), "Stop keeps executing callback alive");
            m_completed = true;
        }
    private:
        // 停止処理完了状態の参照
        bool& m_completed;
    };

    // SceneIntegration(output: テスト出力先)はネットワークScene連携を検証する。
    void SceneIntegration(const std::filesystem::path& output)
    {
        using namespace LamaPon;
        std::filesystem::create_directories(output);
        // ホスト用とクライアント用の描画デバイス
        GraphicsDevice hostGraphics, clientGraphics;
        hostGraphics.Assets().SetAssetRoot(output);
        clientGraphics.Assets().SetAssetRoot(output);
        // ホスト用とクライアント用のScene
        Scene hostScene(hostGraphics), clientScene(clientGraphics);
        // Prefabを構築する元Scene
        Scene prefabSource(hostGraphics);
        // ネットワークPrefabの親オブジェクト
        auto& prefab = prefabSource.CreateGameObject("Player");
        prefab.AddComponent<NetworkIdentityComponent>();
        prefab.AddComponent<RigidbodyComponent>();
        // ネットワークPrefabの子オブジェクト
        auto& child = prefabSource.CreateGameObject("Child");
        child.SetParent(&prefab);
        child.AddComponent<RotatorComponent>();
        prefabSource.SavePrefab(prefab, output / "player.prefab.json");

        // ホスト側の同期対象
        auto& source = hostScene.CreateGameObject("Box");
        // 同期対象の識別コンポーネント
        auto& sourceIdentity = source.AddComponent<NetworkIdentityComponent>("world.box");
        sourceIdentity.SetInterpolationSeconds(0);
        source.GetTransform().position.x = 5;
        source.AddComponent<RotatorComponent>();
        // Sceneから保存した同期データ
        const auto saved = hostScene.SerializeToJson();
        clientScene.LoadFromJson(saved);
        // クライアント側の同期対象
        auto* target = clientScene.FindGameObjectByName("Box");
        Require(target && target->GetComponent<NetworkIdentityComponent>()->SceneKey() == "world.box", "Identity serialization");
        Require(target->GetComponent<NetworkIdentityComponent>()->NetworkId() == 0, "No runtime ID in scene");
        // 複製後の同期対象
        auto& copy = hostScene.DuplicateGameObject(source);
        Require(copy.GetComponent<NetworkIdentityComponent>()->SceneKey() != sourceIdentity.SceneKey(), "Unique duplicate key");
        hostScene.DestroyGameObject(copy);

        // 接続に共用するネットワーク設定
        NetworkConfiguration configuration;
        configuration.port = 0;
        configuration.prefabs.push_back({ "player", "player.prefab.json" });
        // ホストとクライアントのネットワークセッション
        NetworkSession host, client;
        Require(host.Configure(configuration) && client.Configure(configuration), "Bridge configuration");
        // 各セッションをSceneへ接続するブリッジ
        NetworkSceneBridge hostBridge(hostScene, host), clientBridge(clientScene, client);
        Require(host.Host(), "Bridge host");
        host.Update(0);
        hostBridge.BeforeSimulation(0);
        // ホスト上の同期対象ID
        const auto boxId = sourceIdentity.NetworkId();
        Require(boxId != 0, "Static identity registration");
        Require(client.Join(host.RoomAddress()), "Bridge client");
        // step()は接続・Scene同期を1ステップ進める。
        const auto step = [&]
        {
            host.Update(0.01f); client.Update(0.01f);
            hostBridge.BeforeSimulation(0.01f); clientBridge.BeforeSimulation(0.01f);
            hostBridge.AfterSimulation(0.01f); clientBridge.AfterSimulation(0.01f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        // until(predicate: 待機条件)は条件成立まで同期を進める。
        const auto until = [&](auto predicate)
        {
            // 待機の期限
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            // predicateが成立するまで待つ
            while (!predicate())
            {
                step();
                // タイムアウトを検出する
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("Scene synchronization timed out: " + host.LastError() + client.LastError());
            }
        };
        // 接続と初期Scene同期の完了を待つ
        until([&] { return client.State() == NetworkState::Connected && clientBridge.Find(boxId); });
        Require(!target->GetComponent<RotatorComponent>()->IsEnabled(), "Remote simulation disabled");
        source.GetTransform().position.x = 42;
        until([&] { return target->GetTransform().position.x == 42; });

        // Prefabの初期Transform
        NetworkTransform spawn;
        spawn.position = { 3, 4, 5 };
        // クライアントへ生成するPrefab
        auto* player = hostBridge.Spawn("player", spawn, client.LocalPeer());
        Require(player != nullptr, "Registered prefab spawn");
        // 生成したPrefabのネットワークID
        const auto playerId = player->GetComponent<NetworkIdentityComponent>()->NetworkId();
        until([&] { return clientBridge.Find(playerId) != nullptr; });
        // クライアント側のPrefab
        auto* remotePlayer = clientBridge.Find(playerId);
        Require(remotePlayer->GetTransform().position.x == 3
            && !remotePlayer->GetComponent<RigidbodyComponent>()->IsEnabled(), "Prefab transform and host physics");
        Require(!remotePlayer->Children().front()->GetComponent<RotatorComponent>()->IsEnabled(), "Child simulation disabled");
        Require(hostBridge.Spawn("unregistered") == nullptr, "Unknown prefab key rejected");
        Require(hostBridge.Despawn(playerId), "Prefab despawn");
        until([&] { return clientBridge.Find(playerId) == nullptr && hostBridge.Find(playerId) == nullptr; });
        Require(hostBridge.Despawn(boxId), "Static despawn");
        until([&] { return !target->IsEnabled(); });
        // 削除済みオブジェクトの再生成がないか確認する
        for (int index = 0; index < 50; ++index) step();
        Require(host.Objects().empty(), "Retired static object not respawned");
        clientBridge.Reset(); hostBridge.Reset();

        Require(host.Host(), "Script stop host"); host.Update(0); hostBridge.BeforeSimulation(0);
        // Scriptから停止するネットワークオブジェクト
        auto* stopping = hostBridge.Spawn("player");
        Require(stopping != nullptr, "Script stop prefab");
        // 停止対象のゲームオブジェクトID
        const auto stoppingId = stopping->Id();
        // 停止コールバックの完了状態
        bool stopCompleted{};
        stopping->AddComponent<StopNetworkProbe>(stopCompleted);
        SetActiveNetworkSession(&host); SetActiveNetworkSceneBridge(&hostBridge);
        hostScene.Update(0);
        Require(stopCompleted && host.State() == NetworkState::Stopped && hostScene.FindGameObject(stoppingId), "Script stop deferred cleanup");
        hostBridge.AfterSimulation(0);
        Require(hostScene.FindGameObject(stoppingId) == nullptr, "Prefab removed after callback returns");
        SetActiveNetworkSceneBridge(nullptr); SetActiveNetworkSession(nullptr);
        Require(target->IsEnabled() && target->GetTransform().position.x == 5
            && target->GetComponent<RotatorComponent>()->IsEnabled(), "Stop restores scene and simulation");
        Require(sourceIdentity.NetworkId() == 0, "Stop clears binding");

        Require(host.Host(), "Immediate restart host"); host.Update(0); hostBridge.BeforeSimulation(0);
        // 停止後に再生成するPrefab
        auto* oldPlayer = hostBridge.Spawn("player");
        Require(oldPlayer != nullptr, "Old generation dynamic object");
        // 旧セッションのゲームオブジェクトID
        const auto oldPlayerObject = oldPlayer->Id();
        // 再起動前のセッション世代
        const auto oldGeneration = host.Generation();
        host.Stop(); Require(host.Host(), "Restart before bridge update"); host.Update(0);
        hostBridge.BeforeSimulation(0);
        Require(host.Generation() != oldGeneration && hostScene.FindGameObject(oldPlayerObject) == nullptr
            && sourceIdentity.NetworkId() != 0 && host.Objects().size() == 1, "Session generation restores stale bindings");
        hostBridge.Reset();

        // 重複Sceneキーを持つ異常オブジェクト
        auto& duplicate = hostScene.CreateGameObject("Duplicate");
        duplicate.AddComponent<NetworkIdentityComponent>("world.box");
        Require(host.Host(), "Invalid scene host"); host.Update(0); hostBridge.BeforeSimulation(0);
        Require(host.State() == NetworkState::Error && !host.LastError().empty(), "Scene synchronization error visible");
        hostScene.DestroyGameObject(duplicate); hostBridge.Reset();

        Require(host.Host(), "Scene restart"); host.Update(0); hostBridge.BeforeSimulation(0);
        hostScene.LoadFromJson(saved);
        hostBridge.BeforeSimulation(0);
        Require(host.State() == NetworkState::Stopped, "Scene replacement terminates old session");

        hostScene.Clear(); clientScene.Clear();
        hostBridge.BeforeSimulation(0); clientBridge.BeforeSimulation(0);
        configuration.backend = NetworkBackend::Direct;
        configuration.syncMode = NetworkSyncMode::OnChange;
        Require(host.Configure(configuration) && client.Configure(configuration), "Secure cooperative configuration");
        Require(host.Host(), "Cooperative sample host"); host.Update(0);
        Require(client.Join(host.RoomAddress()), "Cooperative sample join");
        // ホストとクライアントのサンプル操作
        Samples::P2PCooperativeController hostController, clientController;
        // cooperativeStep(horizontal: クライアント入力)は同期を1ステップ進める。
        const auto cooperativeStep = [&](const float horizontal)
        {
            host.Update(0.01f); client.Update(0.01f);
            hostBridge.BeforeSimulation(0.01f); clientBridge.BeforeSimulation(0.01f);
            hostController.Update(host, hostBridge, 0.01f, 0, 0);
            clientController.Update(client, clientBridge, 0.01f, horizontal, 0);
            hostBridge.AfterSimulation(0.01f); clientBridge.AfterSimulation(0.01f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        // サンプル操作を180フレーム進める
        for (int index = 0; index < 180; ++index) cooperativeStep(1);
        Require(client.State() == NetworkState::Connected && client.Objects().size() == 2, "Sample creates one player per member");
        // 各クライアントオブジェクトの所有権と位置を検証する
        for (const auto& object : client.Objects())
        {
            // クライアント所有の移動範囲を確認する
            if (object.owner == client.LocalPeer())
                Require(object.transform.position[0] > 3 && object.transform.position[0] < 8, "Sample applies client input on host at bounded speed");
            // ホスト所有オブジェクトが停止していることを確認する
            if (object.owner == 1) Require(object.transform.position[0] == 0, "Sample keeps idle host player stationary");
        }
        clientBridge.Reset(); hostBridge.Reset();

        // 保存するネットワーク設定
        ProjectSettings settings;
        settings.network = configuration;
        // 設定ファイルの出力先
        const auto path = output / "project-settings.json";
        SaveProjectSettings(path, settings, ProjectSettingsFileType::Project);
        // 保存済み設定の読み戻し結果
        const auto loaded = LoadProjectSettings(path);
        Require(loaded.network.prefabs.size() == 1 && loaded.network.port == 0
            && loaded.network.prefabs[0].key == "player" && loaded.network.backend == NetworkBackend::Direct
            && loaded.network.syncMode == NetworkSyncMode::OnChange, "Network settings round trip");
    }
}

// main(argc: 引数数, argv: 引数一覧)はScene連携テストを実行する。
int main(const int argc, char** argv)
{
    // テスト用COMセッション
    ComSession com;
    // テスト失敗を終了コードに変換する
    try
    {
        // 出力先引数が1つであることを確認する
        if (argc != 2) throw std::runtime_error("Expected approved test output directory.");
        // 絶対化して正規化した出力先
        const auto output = std::filesystem::absolute(argv[1]).lexically_normal();
        // テスト生成物の出力先が許可範囲内か確認する
        if (output.filename() != "network-test-data"
            || output.parent_path() != std::filesystem::current_path().lexically_normal())
            throw std::runtime_error("Test fixtures must stay in the current build's network-test-data directory.");
        struct FixtureCleanup final
        {
            // テスト生成物の削除対象
            std::filesystem::path path;
            // テスト生成物を終了時に削除する
            ~FixtureCleanup()
            {
                // 削除時のエラー記録先
                std::error_code error;
                // テスト生成物を削除する
                std::filesystem::remove_all(path, error);
            }
        } cleanup{ output };
        SceneIntegration(output);
        std::cout << "P2P scene serialization, ownership, prefab lifecycle, interpolation, simulation suppression, stop and scene replacement passed.\n";
        // テスト成功を返す
        return 0;
    }
    // 例外内容を出力して失敗終了する
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
