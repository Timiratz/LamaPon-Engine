#include "LamaPon/Core/RuntimeServices.h"
#include "LamaPon/Assets/AssetManager.h"
#include "LamaPon/Audio/AudioSystem.h"
#include "LamaPon/Input/InputSystem.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace
{
    // 条件不成立ならテストを失敗させます。
    // Require(condition: 成立条件, message: 失敗理由)
    void Require(const bool condition, const char* message)
    {
        // 失敗理由を例外で通知
        if (!condition) throw std::runtime_error(message);
    }

    // 非アクティブなAudio/Inputへのアクセス拒否を確認します。
    // CheckUnavailable(services: 確認対象サービス)
    void CheckUnavailable(const LamaPon::RuntimeServices& services)
    {
        // audioRejected: 音声拒否, inputRejected: 入力拒否
        bool audioRejected{}, inputRejected{};
        // 音声未初期化時はlogic_errorを返す
        try { static_cast<void>(services.Audio()); }
        // 音声アクセス拒否を記録
        catch (const std::logic_error&) { audioRejected = true; }
        // 入力未初期化時はlogic_errorを返す
        try { static_cast<void>(services.Input()); }
        // 入力アクセス拒否を記録
        catch (const std::logic_error&) { inputRejected = true; }
        Require(audioRejected && inputRejected, "Inactive services must reject access");
    }
}

// RuntimeServicesの初期化・再初期化・終了契約を検証します。
int main()
{
    // COM初期化結果
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // 例外を終了状態へ変換する結果
    int result{};
    // 各サービスの状態遷移を検査
    try
    {
        // 音声・入力・資産サービス
        LamaPon::RuntimeServices services;
        CheckUnavailable(services);
        Require(services.TryAssets() == nullptr, "Services must start empty");
        // 初期化済みAssetManagerへの参照
        auto& assets = services.EnsureAssets(nullptr, nullptr, false);
        Require(&assets == &services.EnsureAssets(nullptr, nullptr, false),
            "File-only asset access must reuse its cache");
        Require(!assets.ReadFileBytes("assets/shaders/LamaPonLit.hlsl").empty(),
            "File-only service must read engine assets without a GraphicsDevice");
        // ファイル読込の基準ディレクトリ
        const auto assetRoot =
            std::filesystem::absolute("assets").lexically_normal();
        assets.SetAssetRoot(assetRoot);
        assets.SetProgressiveUploadThreshold(4096);
        assets.SetTextCacheBudgetBytes(8192);
        // ファイル読込から通常サービスの初期化へ移行
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
        // 非同期読込の拒否確認に使う欠損モデル名
        constexpr auto missingModel = "missing-background-model.obj";
        Require(
            services.TryAssets()->PrepareModelAsync(missingModel),
            "Background model preparation did not start");
        services.QuiesceGraphicsWork();
        services.QuiesceGraphicsWork();
        // 読込停止時の失敗理由
        std::string quiesceError;
        Require(
            services.TryAssets()->PollModelPreparation(
                    missingModel,
                    &quiesceError)
                    == LamaPon::ModelPreparationState::Failed
                && !services.TryAssets()->PrepareModelAsync(missingModel)
                && !quiesceError.empty(),
            "Graphics work quiescence did not close model work admission");
        // 再初期化をまたいで保持するAudio実体
        auto* const audio = &services.Audio();
        services.PrepareForGraphicsReinitialization();
        Require(
            services.TryAssets() == nullptr,
            "Graphics reinitialization must release assets");
        // 再初期化中にInputアクセスが拒否されたか
        bool inputRejected{};
        // Inputが無効な期間はアクセス拒否
        try { static_cast<void>(services.Input()); }
        // Inputアクセス拒否を記録
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
        // 再初期化前のInput実体
        auto* input = &services.Input();
        // 重複初期化が拒否されたか
        bool repeatedRejected{};
        // 初期化済みサービスの重複初期化を拒否
        try { services.Initialize(nullptr, nullptr, nullptr, true); }
        // 重複初期化エラーを記録
        catch (const std::logic_error&) { repeatedRejected = true; }
        Require(repeatedRejected && input == &services.Input(),
            "Repeated initialization must retain the active input owner");
        services.Shutdown();
        services.Shutdown();
        Require(services.TryAssets() == nullptr, "Shutdown must release assets");
        CheckUnavailable(services);
        // Shutdown後に作り直した資産サービス
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
        // Full shutdown後に旧Input操作が残るか
        bool preservedActionFound{};
        // action: 再初期化後の入力操作
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
    // テスト例外を標準エラーと失敗終了コードへ変換
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    // COM初期化に成功した場合だけ対応する終了処理を行う
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
