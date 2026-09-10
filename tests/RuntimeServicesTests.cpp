#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Input/InputSystem.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    void CheckUnavailable(const LamaPon::RuntimeServices& services)
    {
        bool audioRejected{}, inputRejected{};
        try { static_cast<void>(services.Audio()); }
        catch (const std::logic_error&) { audioRejected = true; }
        try { static_cast<void>(services.Input()); }
        catch (const std::logic_error&) { inputRejected = true; }
        Require(audioRejected && inputRejected, "Inactive services must reject access");
    }
}

int main()
{
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int result{};
    try
    {
        LamaPon::RuntimeServices services;
        CheckUnavailable(services);
        Require(services.TryAssets() == nullptr, "Services must start empty");
        auto& assets = services.EnsureAssets(nullptr, nullptr, false);
        Require(&assets == &services.EnsureAssets(nullptr, nullptr, false),
            "File-only asset access must reuse its cache");
        Require(!assets.ReadFileBytes("assets/shaders/LamaPonLit.hlsl").empty(),
            "File-only service must read engine assets without a GraphicsDevice");
        const auto assetRoot =
            std::filesystem::absolute("assets").lexically_normal();
        assets.SetAssetRoot(assetRoot);
        assets.SetProgressiveUploadThreshold(4096);
        assets.SetTextCacheBudgetBytes(8192);
        // 初期化前のファイル読み込みから通常サービスへ移行し、描画だけの
        // 再初期化ではAudioを維持できることも確認します。
        services.Initialize(nullptr, nullptr, nullptr, true);
        static_cast<void>(services.Audio());
        static_cast<void>(services.Input());
        Require(services.TryAssets() != nullptr, "Initialization must create assets");
        Require(
            services.TryAssets()->AssetRoot() == assetRoot
                && services.TryAssets()->ProgressiveUploadThreshold() == 4096
                && services.TryAssets()->TextCacheBudgetBytes() == 8192,
            "Runtime initialization did not preserve asset configuration");
        services.Input().SetActions(
            {
                {
                    "PreservedAction",
                    {
                        {
                            LamaPon::InputControl::KeyboardSpace,
                            1.0f
                        }
                    }
                }
            });
        constexpr auto missingModel = "missing-background-model.obj";
        Require(
            services.TryAssets()->PrepareModelAsync(missingModel),
            "Background model preparation did not start");
        services.QuiesceGraphicsWork();
        services.QuiesceGraphicsWork();
        std::string quiesceError;
        Require(
            services.TryAssets()->PollModelPreparation(
                    missingModel,
                    &quiesceError)
                    == LamaPon::ModelPreparationState::Failed
                && !services.TryAssets()->PrepareModelAsync(missingModel)
                && !quiesceError.empty(),
            "Graphics work quiescence did not close model work admission");
        auto* const audio = &services.Audio();
        services.PrepareForGraphicsReinitialization();
        Require(
            services.TryAssets() == nullptr,
            "Graphics reinitialization must release assets");
        bool inputRejected{};
        try { static_cast<void>(services.Input()); }
        catch (const std::logic_error&) { inputRejected = true; }
        Require(
            inputRejected && &services.Audio() == audio,
            "Graphics reinitialization must preserve only audio");
        services.Initialize(nullptr, nullptr, nullptr, true);
        Require(
            &services.Audio() == audio,
            "Runtime service initialization recreated preserved audio");
        Require(
            services.TryAssets()->AssetRoot() == assetRoot
                && services.TryAssets()->ProgressiveUploadThreshold() == 4096
                && services.TryAssets()->TextCacheBudgetBytes() == 8192,
            "Graphics reinitialization lost asset configuration");
        Require(
            services.Input().Actions().size() == 1
                && services.Input().Actions().front().name
                    == "PreservedAction",
            "Graphics reinitialization lost input actions");
        auto* input = &services.Input();
        bool repeatedRejected{};
        try { services.Initialize(nullptr, nullptr, nullptr, true); }
        catch (const std::logic_error&) { repeatedRejected = true; }
        Require(repeatedRejected && input == &services.Input(),
            "Repeated initialization must retain the active input owner");
        services.Shutdown();
        services.Shutdown();
        Require(services.TryAssets() == nullptr, "Shutdown must release assets");
        CheckUnavailable(services);
        auto& resetAssets = services.EnsureAssets(
            nullptr,
            nullptr,
            false);
        Require(
            resetAssets.AssetRoot().empty()
                && resetAssets.ProgressiveUploadThreshold()
                    == LamaPon::AssetManager::DefaultProgressiveUploadThreshold
                && resetAssets.TextCacheBudgetBytes()
                    == 32u * 1024u * 1024u,
            "Full shutdown retained reinitialization asset settings");
        Require(!resetAssets.ReadFileBytes("assets/shaders/LamaPonLit.hlsl").empty(),
            "File-only access must recover after shutdown");
        services.Initialize(nullptr, nullptr, nullptr, false);
        bool preservedActionFound{};
        for (const auto& action : services.Input().Actions())
        {
            preservedActionFound = preservedActionFound
                || action.name == "PreservedAction";
        }
        Require(
            !preservedActionFound,
            "Full shutdown retained reinitialization input actions");
        services.Shutdown();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
