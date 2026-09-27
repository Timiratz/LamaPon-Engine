#include "LamaPon/LamaPon.h"
#include "LamaPon/Assets/AssetPacker.h"
#include "../packages/src/scene-transition-showcase/SceneTransitionAssets.h"
#include "../packages/src/scene-transition-showcase/SceneTransitionSchema.h"

#include <objbase.h>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "NetworkWorkflowPackageTests.h"

int main(const int argumentCount, const char* const* argumentValues)
{
    try
    {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        {
            throw std::runtime_error("Could not initialize COM for asset loading.");
        }
        const struct ComScope final
        {
            ~ComScope() { CoUninitialize(); }
        } comScope;

        if (argumentCount != 4)
        {
            throw std::invalid_argument(
                "A project Game Module path and two package source paths are required.");
        }
        LamaPon::GameModuleHost host;
        if (!host.Load(std::filesystem::path(argumentValues[1])))
        {
            throw std::runtime_error(host.LastError());
        }
        if (host.ModuleName() != "LamaPon Project Game Module"
            || host.RegisteredComponents().size() != 4
            || host.FindComponent("Test.ExternalScript") == nullptr
            || host.FindComponent("Game.BeginnerScript") == nullptr
            || host.FindComponent("SceneTransition.Controller") == nullptr
            || host.FindDataAssetType("SceneTransition.Preset") == nullptr
            || host.FindComponent("Network.SessionController") == nullptr
            || host.FindDataAssetType("Network.ConnectionProfile") == nullptr)
        {
            throw std::runtime_error(
                "The external project script was not registered.");
        }

        const auto* beginnerDescriptor =
            host.FindComponent("Game.BeginnerScript");
        if (beginnerDescriptor->propertiesSchemaJson == nullptr)
        {
            throw std::runtime_error(
                "The beginner Script schema was not registered.");
        }

        // デモSceneのUIアセットも有効化するため、非表示の描画先を用意します。
        const struct HiddenWindow final
        {
            HWND handle = CreateWindowExW(0, L"STATIC", L"Transition Package Test",
                WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            ~HiddenWindow() { if (handle != nullptr) DestroyWindow(handle); }
        } window;
        if (window.handle == nullptr)
        {
            throw std::runtime_error("Could not create the hidden render window.");
        }
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(window.handle, 640, 360);
        LamaPon::Scene scene(graphics);
        auto& object = scene.CreateGameObject("Beginner Script Test");
        object.AddComponent<LamaPon::NativeScriptComponent>(
            "Game.BeginnerScript");
        scene.Update(0.25f);
        if (object.GetTransform().position.x != 2.0f
            || object.GetTransform().position.y != 0.25f)
        {
            throw std::runtime_error(
                "The beginner Script lifecycle was not invoked.");
        }

        const std::filesystem::path packageRoot(argumentValues[2]);
        graphics.Assets().SetAssetRoot(packageRoot, false);
        std::size_t presetCount{};
        for (const auto& entry : std::filesystem::directory_iterator(
            packageRoot / "presets"))
        {
            const auto asset = scene.LoadDataAsset(
                std::filesystem::relative(entry.path(), packageRoot));
            LamaPon::SceneTransitionSettings settings;
            if (!LamaPonSceneShowcase::ReadPreset(*asset, settings)
                || settings.effect == LamaPon::SceneTransitionEffect::None)
            {
                throw std::runtime_error("A shipped preset could not be read.");
            }
            std::ifstream input(entry.path());
            nlohmann::json source;
            input >> source;
            if (LamaPon::SceneTransitionToJson(settings) !=
                LamaPon::SceneTransitionToJson(
                    LamaPon::SceneTransitionFromJson(source.at("values"))))
            {
                throw std::runtime_error("A preset field changed while reading its data asset.");
            }
            if (!settings.ruleTexture.empty()
                && !std::filesystem::exists(packageRoot / "rules/gradient.png"))
            {
                throw std::runtime_error("The rule image is missing.");
            }
            ++presetCount;
        }
        if (presetCount != 16)
        {
            throw std::runtime_error("The package must contain all 16 presets.");
        }
        LamaPon::SceneTransitionSettings sanitized;
        const auto malformed = LamaPon::DataAsset::FromJson(R"({
            "type":"SceneTransition.Preset","values":{
                "effect":"futureEffect","coverDuration":-9,
                "color":"bad","divisions":999}})");
        if (!LamaPonSceneShowcase::ReadPreset(malformed, sanitized)
            || sanitized.effect != LamaPon::SceneTransitionEffect::None
            || sanitized.coverDuration < 0 || sanitized.divisions != 64
            || LamaPonSceneShowcase::ReadPreset(
                LamaPon::DataAsset::FromJson(R"({"type":"Other","values":{"effect":"fade"}})"),
                sanitized))
        {
            throw std::runtime_error("Preset type checks and sanitization failed.");
        }
        const auto schema = nlohmann::json::parse(LamaPonSceneShowcase::PresetSchema);
        nlohmann::json defaults = nlohmann::json::object();
        for (const auto& field : schema.at("fields"))
        {
            defaults[field.at("name").get<std::string>()] = field.at("default");
        }
        if (!LamaPonSceneShowcase::ReadPreset(LamaPon::DataAsset::FromJson(
            nlohmann::json{{"type","SceneTransition.Preset"},{"values",defaults}}.dump()),
            sanitized) || sanitized.effect != LamaPon::SceneTransitionEffect::Fade)
        {
            throw std::runtime_error("New preset assets must default to Fade.");
        }

        // 展開済みassetsのない書き出し環境でも同じScriptが動くことを確認します。
        const auto archiveRoot = std::filesystem::absolute(argumentValues[1])
            .parent_path() / "transition-assets";
        auto archivePath = archiveRoot;
        archivePath += ".tpak";
        const auto packed = LamaPon::PackAssets(packageRoot, archivePath,
            LamaPon::Crypto::ArchiveKey());
        graphics.Assets().SetAssetRoot(archiveRoot, false);
        if (!graphics.Assets().IsArchived() || packed.fileCount < 16)
        {
            throw std::runtime_error("The package asset archive was not mounted.");
        }

        auto& controller = scene.CreateGameObject("Transition Controller");
        auto& script = controller.AddComponent<LamaPon::NativeScriptComponent>(
            "SceneTransition.Controller", R"({
                "transition":"presets/DotsForward.asset.json",
                "eventName":"Test.Play","playOnStart":false})");
        scene.Update(0.01f);
        if (scene.Scenes().IsTransitioning()
            || nlohmann::json::parse(script.SerializedProperties()).at("transition")
                != "presets/DotsForward.asset.json")
        {
            throw std::runtime_error("Controller settings must round trip without autoplay.");
        }
        scene.Events().Publish("Test.Play");
        if (!scene.Scenes().IsTransitioning()
            || scene.Scenes().ActiveTransition().effect
                != LamaPon::SceneTransitionEffect::Dots)
        {
            throw std::runtime_error("The event did not play the selected data asset.");
        }
        scene.Scenes().AdvanceTransition(0.15f);
        const auto coverage = scene.Scenes().TransitionCoverage();
        scene.Events().Publish("Test.Play");
        if (std::abs(scene.Scenes().TransitionCoverage() - coverage) > 0.0001f)
        {
            throw std::runtime_error("A second event restarted the active transition.");
        }
        scene.Scenes().ResetTransition();
        script.SetEnabled(false);
        scene.Update(0.01f);
        scene.Events().Publish("Test.Play");
        if (scene.Scenes().IsTransitioning())
        {
            throw std::runtime_error("Disabled controllers must ignore events.");
        }
        script.SetEnabled(true);
        scene.Update(0.01f);
        scene.Events().Publish("Test.Play");
        if (!scene.Scenes().IsTransitioning())
        {
            throw std::runtime_error("Re-enabled controllers must accept events.");
        }
        scene.Scenes().ResetTransition();
        scene.DestroyGameObject(controller);
        if (scene.Events().SubscriptionCount() != 0)
        {
            throw std::runtime_error("Controller destruction must unsubscribe events.");
        }

        const std::filesystem::path destination("scenes/Showcase.scene.json");
        const auto resolvedDestination = graphics.Assets().ResolvePath(destination);
        auto& loader = scene.CreateGameObject("Transition Scene Loader");
        loader.AddComponent<LamaPon::NativeScriptComponent>(
            "SceneTransition.Controller", nlohmann::json{
                {"transition","presets/Fade.asset.json"},
                {"destination",LamaPon::PathToUtf8(destination)},
                {"playOnStart",true}}.dump());
        scene.Update(0.01f);
        if (!scene.Scenes().IsTransitioning() || !scene.Scenes().HasPendingLoad()
            || scene.Scenes().CurrentScenePath() == resolvedDestination)
        {
            throw std::runtime_error("Autoplay must queue a covered scene change.");
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (scene.Scenes().HasPendingLoad() || scene.Scenes().IsTransitioning())
        {
            scene.Scenes().AdvanceTransition(0.1f);
            scene.Update(0.01f);
            if (std::chrono::steady_clock::now() > deadline)
            {
                throw std::runtime_error("The controller scene change did not finish.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (scene.Scenes().CurrentScenePath() != resolvedDestination
            || !scene.Scenes().LastError().empty()
            || scene.FindGameObjectByName("Play Transition") == nullptr
            || scene.Events().SubscriptionCount() != 1)
        {
            throw std::runtime_error("The controller did not finish loading and unsubscribe.");
        }
        graphics.Assets().SetAssetRoot(packageRoot, false);
        std::filesystem::remove(archivePath);
        TestNetworkWorkflowPackage(graphics, std::filesystem::path(argumentValues[3]),
            std::filesystem::absolute(argumentValues[1]).parent_path());
        std::cout << "External project Game Module test passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
