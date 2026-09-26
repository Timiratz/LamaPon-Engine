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
        HRESULT result{ CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
        ~ComSession() { if (SUCCEEDED(result)) CoUninitialize(); }
    };
    void Require(const bool condition, const char* message)
    { if (!condition) throw std::runtime_error(message); }

    struct StopScript final : LamaPon::Script
    {
        void Stop() { StopNetwork(); }
    };

    class StopNetworkProbe final : public LamaPon::Component
    {
    public:
        explicit StopNetworkProbe(bool& completed) : m_completed(completed) {}
        std::string_view TypeName() const noexcept override { return "StopNetworkProbe"; }
    protected:
        void OnUpdate(float) override
        {
            StopScript script;
            script.Stop();
            Require(Owner().GetScene().FindGameObject(Owner().Id()) == &Owner(), "Stop keeps executing callback alive");
            m_completed = true;
        }
    private:
        bool& m_completed;
    };

    void SceneIntegration(const std::filesystem::path& output)
    {
        using namespace LamaPon;
        std::filesystem::create_directories(output);
        GraphicsDevice hostGraphics, clientGraphics;
        hostGraphics.Assets().SetAssetRoot(output);
        clientGraphics.Assets().SetAssetRoot(output);
        Scene hostScene(hostGraphics), clientScene(clientGraphics);
        Scene prefabSource(hostGraphics);
        auto& prefab = prefabSource.CreateGameObject("Player");
        prefab.AddComponent<NetworkIdentityComponent>();
        prefab.AddComponent<RigidbodyComponent>();
        auto& child = prefabSource.CreateGameObject("Child");
        child.SetParent(&prefab);
        child.AddComponent<RotatorComponent>();
        prefabSource.SavePrefab(prefab, output / "player.prefab.json");

        auto& source = hostScene.CreateGameObject("Box");
        auto& sourceIdentity = source.AddComponent<NetworkIdentityComponent>("world.box");
        sourceIdentity.SetInterpolationSeconds(0);
        source.GetTransform().position.x = 5;
        source.AddComponent<RotatorComponent>();
        const auto saved = hostScene.SerializeToJson();
        clientScene.LoadFromJson(saved);
        auto* target = clientScene.FindGameObjectByName("Box");
        Require(target && target->GetComponent<NetworkIdentityComponent>()->SceneKey() == "world.box", "Identity serialization");
        Require(target->GetComponent<NetworkIdentityComponent>()->NetworkId() == 0, "No runtime ID in scene");
        auto& copy = hostScene.DuplicateGameObject(source);
        Require(copy.GetComponent<NetworkIdentityComponent>()->SceneKey() != sourceIdentity.SceneKey(), "Unique duplicate key");
        hostScene.DestroyGameObject(copy);

        NetworkConfiguration configuration;
        configuration.port = 0;
        configuration.prefabs.push_back({ "player", "player.prefab.json" });
        NetworkSession host, client;
        Require(host.Configure(configuration) && client.Configure(configuration), "Bridge configuration");
        NetworkSceneBridge hostBridge(hostScene, host), clientBridge(clientScene, client);
        Require(host.Host(), "Bridge host");
        host.Update(0);
        hostBridge.BeforeSimulation(0);
        const auto boxId = sourceIdentity.NetworkId();
        Require(boxId != 0, "Static identity registration");
        Require(client.Join(host.RoomAddress()), "Bridge client");
        const auto step = [&]
        {
            host.Update(0.01f); client.Update(0.01f);
            hostBridge.BeforeSimulation(0.01f); clientBridge.BeforeSimulation(0.01f);
            hostBridge.AfterSimulation(0.01f); clientBridge.AfterSimulation(0.01f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        const auto until = [&](auto predicate)
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!predicate())
            {
                step();
                if (std::chrono::steady_clock::now() > deadline)
                    throw std::runtime_error("Scene synchronization timed out: " + host.LastError() + client.LastError());
            }
        };
        until([&] { return client.State() == NetworkState::Connected && clientBridge.Find(boxId); });
        Require(!target->GetComponent<RotatorComponent>()->IsEnabled(), "Remote simulation disabled");
        source.GetTransform().position.x = 42;
        until([&] { return target->GetTransform().position.x == 42; });

        NetworkTransform spawn;
        spawn.position = { 3, 4, 5 };
        auto* player = hostBridge.Spawn("player", spawn, client.LocalPeer());
        Require(player != nullptr, "Registered prefab spawn");
        const auto playerId = player->GetComponent<NetworkIdentityComponent>()->NetworkId();
        until([&] { return clientBridge.Find(playerId) != nullptr; });
        auto* remotePlayer = clientBridge.Find(playerId);
        Require(remotePlayer->GetTransform().position.x == 3
            && !remotePlayer->GetComponent<RigidbodyComponent>()->IsEnabled(), "Prefab transform and host physics");
        Require(!remotePlayer->Children().front()->GetComponent<RotatorComponent>()->IsEnabled(), "Child simulation disabled");
        Require(hostBridge.Spawn("unregistered") == nullptr, "Unknown prefab key rejected");
        Require(hostBridge.Despawn(playerId), "Prefab despawn");
        until([&] { return clientBridge.Find(playerId) == nullptr && hostBridge.Find(playerId) == nullptr; });
        Require(hostBridge.Despawn(boxId), "Static despawn");
        until([&] { return !target->IsEnabled(); });
        for (int index = 0; index < 50; ++index) step();
        Require(host.Objects().empty(), "Retired static object not respawned");
        clientBridge.Reset(); hostBridge.Reset();

        Require(host.Host(), "Script stop host"); host.Update(0); hostBridge.BeforeSimulation(0);
        auto* stopping = hostBridge.Spawn("player");
        Require(stopping != nullptr, "Script stop prefab");
        const auto stoppingId = stopping->Id();
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
        auto* oldPlayer = hostBridge.Spawn("player");
        Require(oldPlayer != nullptr, "Old generation dynamic object");
        const auto oldPlayerObject = oldPlayer->Id();
        const auto oldGeneration = host.Generation();
        host.Stop(); Require(host.Host(), "Restart before bridge update"); host.Update(0);
        hostBridge.BeforeSimulation(0);
        Require(host.Generation() != oldGeneration && hostScene.FindGameObject(oldPlayerObject) == nullptr
            && sourceIdentity.NetworkId() != 0 && host.Objects().size() == 1, "Session generation restores stale bindings");
        hostBridge.Reset();

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
        Require(host.Host(), "Cooperative sample host"); host.Update(0);
        Require(client.Join(host.RoomAddress()), "Cooperative sample join");
        Samples::P2PCooperativeController hostController, clientController;
        const auto cooperativeStep = [&](const float horizontal)
        {
            host.Update(0.01f); client.Update(0.01f);
            hostBridge.BeforeSimulation(0.01f); clientBridge.BeforeSimulation(0.01f);
            hostController.Update(host, hostBridge, 0.01f, 0, 0);
            clientController.Update(client, clientBridge, 0.01f, horizontal, 0);
            hostBridge.AfterSimulation(0.01f); clientBridge.AfterSimulation(0.01f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        for (int index = 0; index < 180; ++index) cooperativeStep(1);
        Require(client.State() == NetworkState::Connected && client.Objects().size() == 2, "Sample creates one player per member");
        for (const auto& object : client.Objects())
        {
            if (object.owner == client.LocalPeer())
                Require(object.transform.position[0] > 3 && object.transform.position[0] < 8, "Sample applies client input on host at bounded speed");
            if (object.owner == 1) Require(object.transform.position[0] == 0, "Sample keeps idle host player stationary");
        }
        clientBridge.Reset(); hostBridge.Reset();

        ProjectSettings settings;
        settings.network = configuration;
        const auto path = output / "project-settings.json";
        SaveProjectSettings(path, settings, ProjectSettingsFileType::Project);
        const auto loaded = LoadProjectSettings(path);
        Require(loaded.network.prefabs.size() == 1 && loaded.network.port == 0
            && loaded.network.prefabs[0].key == "player", "Network settings round trip");
    }
}

int main(const int argc, char** argv)
{
    ComSession com;
    try
    {
        if (argc != 2) throw std::runtime_error("Expected approved test output directory.");
        const auto output = std::filesystem::absolute(argv[1]).lexically_normal();
        if (output.filename() != "network-test-data"
            || output.parent_path() != std::filesystem::current_path().lexically_normal())
            throw std::runtime_error("Test fixtures must stay in the current build's network-test-data directory.");
        struct FixtureCleanup final
        {
            std::filesystem::path path;
            ~FixtureCleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{ output };
        SceneIntegration(output);
        std::cout << "P2P scene serialization, ownership, prefab lifecycle, interpolation, simulation suppression, stop and scene replacement passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
