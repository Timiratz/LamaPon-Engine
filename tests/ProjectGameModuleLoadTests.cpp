#include <WinSock2.h>

#include "LamaPon/LamaPon.h"
#include "LamaPon/Assets/AssetPacker.h"
#include "../packages/src/scene-transition-showcase/SceneTransitionOverlay.h"
#include "../packages/src/scene-transition-showcase/SceneTransitionSchema.h"

#include <objbase.h>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "NetworkWorkflowPackageTests.h"
#include "OllamaPackageTests.h"

// main(argumentCount: 引数数, argumentValues: 引数一覧)は外部Game Moduleと同梱パッケージを検証する。
int main(const int argumentCount, const char* const* argumentValues)
{
    // テスト失敗を終了コードに変換する
    try
    {
        // COMの初期化に失敗した場合は検証を中断する
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
        {
            throw std::runtime_error("Could not initialize COM for asset loading.");
        }
        // COM終了を保証するScopeインスタンス
        const struct ComScope final
        {
            // 初期化したCOMを終了する
            ~ComScope() { CoUninitialize(); }
        } comScope;

        // プロジェクトと3パッケージの引数がそろっていることを確認する
        if (argumentCount != 5)
        {
            throw std::invalid_argument(
                "A project Game Module path and three package source paths are required.");
        }
        // 外部プロジェクトのGame Module
        LamaPon::GameModuleHost host;
        // 指定モジュールを読み込めることを確認する
        if (!host.Load(std::filesystem::path(argumentValues[1])))
        {
            throw std::runtime_error(host.LastError());
        }
        // 必須コンポーネントとDataAsset型の登録を確認する
        if (host.ModuleName() != "LamaPon Project Game Module"
            || host.RegisteredComponents().size() != 6
            || host.FindComponent("Test.ExternalScript") == nullptr
            || host.FindComponent("Game.BeginnerScript") == nullptr
            || host.FindComponent("SceneTransition.Controller") == nullptr
            || host.FindComponent("SceneTransition.Overlay") == nullptr
            || host.FindDataAssetType("SceneTransition.Preset") == nullptr
            || host.FindComponent("Network.SessionController") == nullptr
            || host.FindDataAssetType("Network.ConnectionProfile") == nullptr
            || host.FindComponent("Ollama.Chat") == nullptr
            || host.FindDataAssetType("Ollama.ModelProfile") == nullptr)
        {
            throw std::runtime_error(
                "The external project script was not registered.");
        }

        // Beginner Scriptの登録情報
        const auto* beginnerDescriptor =
            host.FindComponent("Game.BeginnerScript");
        // Scriptのプロパティスキーマが登録済みか確認する
        if (beginnerDescriptor->propertiesSchemaJson == nullptr)
        {
            throw std::runtime_error(
                "The beginner Script schema was not registered.");
        }

        // window: UIアセット読み込みに使う非表示ウィンドウ
        const struct HiddenWindow final
        {
            // Scene表示に使う非表示ウィンドウ
            HWND handle = CreateWindowExW(0, L"STATIC", L"Transition Package Test",
                WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            // 作成に成功したウィンドウを破棄する
            ~HiddenWindow()
            {
                // 有効なウィンドウだけを破棄する
                if (handle != nullptr) DestroyWindow(handle);
            }
        } window;
        // 非表示ウィンドウが作成できたことを確認する
        if (window.handle == nullptr)
        {
            throw std::runtime_error("Could not create the hidden render window.");
        }
        // パッケージ表示に使う描画デバイス
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(window.handle, 640, 360);
        // パッケージScriptを実行するScene
        LamaPon::Scene scene(graphics);
        // Beginner Scriptを検証するGameObject
        auto& object = scene.CreateGameObject("Beginner Script Test");
        object.AddComponent<LamaPon::NativeScriptComponent>(
            "Game.BeginnerScript");
        scene.Update(0.25f);
        // Beginner Scriptの更新結果を検証する
        if (object.GetTransform().position.x != 2.0f
            || object.GetTransform().position.y != 0.25f)
        {
            throw std::runtime_error(
                "The beginner Script lifecycle was not invoked.");
        }

        // Scene Transitionパッケージのルート
        const std::filesystem::path packageRoot(argumentValues[2]);
        graphics.Assets().SetAssetRoot(packageRoot, false);
        // 読み込んだプリセット数
        std::size_t presetCount{};
        // entry: 配布プリセット項目を読み込んで検証する
        for (const auto& entry : std::filesystem::directory_iterator(
            packageRoot / "presets"))
        {
            // 読み込んだDataAsset
            const auto asset = scene.LoadDataAsset(
                std::filesystem::relative(entry.path(), packageRoot));
            // プリセットの遷移設定と見た目設定
            LamaPon::SceneTransitionSettings timing;
            // プリセットの見た目設定
            LamaPonSceneShowcase::Look look;
            // プリセット値が読み込み中に変わっていないか確認する
            if (!LamaPonSceneShowcase::ReadPreset(*asset, timing, look)
                || look.effect == LamaPonSceneShowcase::Effect::None
                || LamaPon::IsInstantSceneTransition(timing))
            {
                throw std::runtime_error("A shipped preset could not be read.");
            }
            // 配布ファイルのJSON入力
            std::ifstream input(entry.path());
            // 配布ファイルのJSON値
            nlohmann::json source;
            input >> source;
            // JSONのプリセット値一覧
            const auto& values = source.at("values");
            // すべてのプリセット値が往復変換で維持されたか確認する
            if (LamaPon::SceneTransitionToJson(timing) !=
                    LamaPon::SceneTransitionToJson(LamaPon::SceneTransitionFromJson(
                        values, LamaPonSceneShowcase::DefaultTiming()))
                || LamaPonSceneShowcase::LookToJson(look) !=
                    LamaPonSceneShowcase::LookToJson(
                        LamaPonSceneShowcase::LookFromJson(values)))
            {
                throw std::runtime_error("A preset field changed while reading its data asset.");
            }
            // installedRoot: インストール済み接頭辞
            constexpr std::string_view installedRoot =
                "packages/scene-transition-showcase/";
            // ルール画像のパスがパッケージ内にあることを確認する
            if (!look.ruleTexture.empty()
                && (!look.ruleTexture.starts_with(installedRoot)
                    || !std::filesystem::exists(packageRoot
                        / look.ruleTexture.substr(installedRoot.size()))))
            {
                throw std::runtime_error("The rule image is missing.");
            }
            // 配布プリセットが個別シェーダーを指定していないことを確認する
            if (!look.shader.empty())
            {
                throw std::runtime_error("Shipped presets must use the package shader.");
            }
            // 読み込み済みプリセット数を記録する
            ++presetCount;
        }
        // 16種類のプリセットがそろっているか確認する
        if (presetCount != 16)
        {
            throw std::runtime_error("The package must contain all 16 presets.");
        }
        // 不正値の補正結果を受け取る遷移設定
        LamaPon::SceneTransitionSettings sanitized;
        // 不正値の補正結果を受け取る見た目設定
        LamaPonSceneShowcase::Look sanitizedLook;
        // 不正な値と未知のEffectを含むDataAsset
        const auto malformed = LamaPon::DataAsset::FromJson(R"({
            "type":"SceneTransition.Preset","values":{
                "effect":"futureEffect","coverDuration":-9,
                "color":"bad","divisions":999}})");
        // 不正値と未知Effectの補正を確認する
        if (!LamaPonSceneShowcase::ReadPreset(malformed, sanitized, sanitizedLook)
            || sanitizedLook.effect != LamaPonSceneShowcase::Effect::Fade
            || sanitized.coverDuration != 0.0f || sanitizedLook.divisions != 64
            || sanitizedLook.color.w != 1.0f
            || LamaPonSceneShowcase::ReadPreset(
                LamaPon::DataAsset::FromJson(R"({"type":"Other","values":{"effect":"fade"}})"),
                sanitized, sanitizedLook))
        {
            throw std::runtime_error("Preset type checks and sanitization failed.");
        }
        // Effectなしのプリセットが即時遷移になることを確認する
        // なしEffectのテスト用プリセットを読み込む
        if (!LamaPonSceneShowcase::ReadPreset(LamaPon::DataAsset::FromJson(R"({
                "type":"SceneTransition.Preset","values":{
                    "effect":"none","coverDuration":2}})"), sanitized, sanitizedLook)
            || !LamaPon::IsInstantSceneTransition(sanitized))
        {
            throw std::runtime_error("A preset without an effect must switch instantly.");
        }
        // Scene Transitionスキーマ
        const auto schema = nlohmann::json::parse(LamaPonSceneShowcase::PresetSchema);
        // スキーマに定義された既定値
        nlohmann::json defaults = nlohmann::json::object();
        // field: プリセットの各スキーマ既定値を集める
        for (const auto& field : schema.at("fields"))
        {
            defaults[field.at("name").get<std::string>()] = field.at("default");
        }
        // 新規プリセットの既定値を検証する
        if (!LamaPonSceneShowcase::ReadPreset(LamaPon::DataAsset::FromJson(
            nlohmann::json{{"type","SceneTransition.Preset"},{"values",defaults}}.dump()),
            sanitized, sanitizedLook)
            || sanitizedLook.effect != LamaPonSceneShowcase::Effect::Fade
            || sanitized.coverDuration != 0.4f)
        {
            throw std::runtime_error("New preset assets must default to Fade.");
        }

        // 展開済みファイルのないアーカイブ出力からScriptを検証する
        // Game Moduleと同じ場所に作るパッケージルート
        const auto archiveRoot = std::filesystem::absolute(argumentValues[1])
            .parent_path() / "transition-assets";
        // PackAssetsへ渡すアーカイブファイル名
        auto archivePath = archiveRoot;
        archivePath += ".tpak";
        // DataAssetパッケージを作成した結果
        const auto packed = LamaPon::PackAssets(packageRoot, archivePath,
            LamaPon::Crypto::ArchiveKey());
        graphics.Assets().SetAssetRoot(archiveRoot, false);
        // アーカイブがマウントされ、全アセットが含まれることを確認する
        if (!graphics.Assets().IsArchived() || packed.fileCount < 16)
        {
            throw std::runtime_error("The package asset archive was not mounted.");
        }

        // Scene Transition制御用GameObject
        auto& controller = scene.CreateGameObject("Transition Controller");
        // パッケージの遷移Controller Script
        auto& script = controller.AddComponent<LamaPon::NativeScriptComponent>(
            "SceneTransition.Controller", R"({
                "transition":"presets/DotsForward.asset.json",
                "eventName":"Test.Play","playOnStart":false})");
        scene.Update(0.01f);
        // 初期設定のまま自動再生せず保存値が維持されていることを確認する
        if (scene.Scenes().IsTransitioning()
            || nlohmann::json::parse(script.SerializedProperties()).at("transition")
                != "presets/DotsForward.asset.json")
        {
            throw std::runtime_error("Controller settings must round trip without autoplay.");
        }
        scene.Events().Publish("Test.Play");
        // Sceneから取得した遷移Overlay
        auto* overlay = scene.FindGameObjectByName(
            LamaPonSceneShowcase::OverlayObjectName);
        // OverlayのSpriteRenderer
        auto* overlaySprite = overlay != nullptr
            ? overlay->GetComponent<LamaPon::SpriteRendererComponent>()
            : nullptr;
        // イベントでプリセット遷移とOverlayが有効になることを確認する
        if (!scene.Scenes().IsTransitioning()
            || scene.Scenes().ActiveTransition().coverDuration != 0.4f
            || overlay == nullptr || !overlay->IsPersistent()
            || overlaySprite == nullptr
            || overlaySprite->SortOrder() != LamaPonSceneShowcase::OverlaySortOrder)
        {
            throw std::runtime_error("The event did not play the selected data asset.");
        }
        // Overlayが被覆率と演出番号をShaderへ渡すことを確認する
        scene.Scenes().AdvanceTransition(0.15f);
        scene.Update(0.0f);
        // 現在の画面被覆率
        const auto coverage = scene.Scenes().TransitionCoverage();
        // Overlay Shaderに渡されたカスタム値
        const auto parameters = overlaySprite->CustomParameter(0);
        // Overlay描画とShader設定が遷移状態へ追従することを確認する
        if (!overlaySprite->IsEnabled()
            || std::abs(parameters.x - coverage) > 0.0001f
            || parameters.w != static_cast<float>(LamaPonSceneShowcase::Effect::Dots)
            || LamaPon::PathToUtf8(overlaySprite->ShaderPath())
                != LamaPonSceneShowcase::DefaultShaderPath)
        {
            throw std::runtime_error("The package overlay did not follow the transition.");
        }
        scene.Events().Publish("Test.Play");
        // 再度のイベントで実行中の遷移が再開されないことを確認する
        if (std::abs(scene.Scenes().TransitionCoverage() - coverage) > 0.0001f)
        {
            throw std::runtime_error("A second event restarted the active transition.");
        }
        scene.Scenes().ResetTransition();
        scene.Update(0.0f);
        // 遷移終了後にOverlayが隠れることを確認する
        if (overlaySprite->IsEnabled())
        {
            throw std::runtime_error("The overlay must hide after its transition.");
        }
        script.SetEnabled(false);
        scene.Update(0.01f);
        scene.Events().Publish("Test.Play");
        // 無効化したControllerがイベントを無視することを確認する
        if (scene.Scenes().IsTransitioning())
        {
            throw std::runtime_error("Disabled controllers must ignore events.");
        }
        script.SetEnabled(true);
        scene.Update(0.01f);
        scene.Events().Publish("Test.Play");
        // 再有効化したControllerがイベントを受けることを確認する
        if (!scene.Scenes().IsTransitioning())
        {
            throw std::runtime_error("Re-enabled controllers must accept events.");
        }
        scene.Scenes().ResetTransition();
        scene.DestroyGameObject(controller);
        // Controller破棄時にイベント購読が解除されることを確認する
        if (scene.Events().SubscriptionCount() != 0)
        {
            throw std::runtime_error("Controller destruction must unsubscribe events.");
        }

        // 遷移後に読み込むSceneパス
        const std::filesystem::path destination("scenes/Showcase.scene.json");
        // AssetManagerが解決したSceneパス
        const auto resolvedDestination = graphics.Assets().ResolvePath(destination);
        // 遷移先を指定するScene Loader
        auto& loader = scene.CreateGameObject("Transition Scene Loader");
        loader.AddComponent<LamaPon::NativeScriptComponent>(
            "SceneTransition.Controller", nlohmann::json{
                {"transition","presets/Fade.asset.json"},
                {"destination",LamaPon::PathToUtf8(destination)},
                {"playOnStart",true}}.dump());
        scene.Update(0.01f);
        // Scene変更を覆い付きで待機状態に入れる
        if (!scene.Scenes().IsTransitioning() || !scene.Scenes().HasPendingLoad()
            || scene.Scenes().CurrentScenePath() == resolvedDestination)
        {
            throw std::runtime_error("Autoplay must queue a covered scene change.");
        }
        // Scene変更完了の期限
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        // Scene読込または遷移が完了するまで進める
        while (scene.Scenes().HasPendingLoad() || scene.Scenes().IsTransitioning())
        {
            scene.Scenes().AdvanceTransition(0.1f);
            scene.Update(0.01f);
            // Scene変更が期限内に完了することを確認する
            if (std::chrono::steady_clock::now() > deadline)
            {
                throw std::runtime_error("The controller scene change did not finish.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Scene変更後のパス・エラー・UI・購読を確認する
        if (scene.Scenes().CurrentScenePath() != resolvedDestination
            || !scene.Scenes().LastError().empty()
            || scene.FindGameObjectByName("Play Transition") == nullptr
            || scene.Events().SubscriptionCount() != 1)
        {
            throw std::runtime_error("The controller did not finish loading and unsubscribe.");
        }
        // SceneをまたいだOverlayが遷移後に隠れていることを確認する
        overlay = scene.FindGameObjectByName(LamaPonSceneShowcase::OverlayObjectName);
        overlaySprite = overlay != nullptr
            ? overlay->GetComponent<LamaPon::SpriteRendererComponent>()
            : nullptr;
        // Scene読込後のOverlay描画状態を確認する
        if (overlaySprite == nullptr || overlaySprite->IsEnabled()
            || overlaySprite->CustomParameter(0).w
                != static_cast<float>(LamaPonSceneShowcase::Effect::Fade))
        {
            throw std::runtime_error("The overlay did not persist across the scene load.");
        }
        graphics.Assets().SetAssetRoot(packageRoot, false);
        std::filesystem::remove(archivePath);
        TestNetworkWorkflowPackage(graphics, std::filesystem::path(argumentValues[3]),
            std::filesystem::absolute(argumentValues[1]).parent_path());
        TestOllamaPackage(graphics, std::filesystem::path(argumentValues[4]),
            std::filesystem::absolute(argumentValues[1]).parent_path());
        std::cout << "External project Game Module test passed.\n";
        // テスト成功を返す
        return 0;
    }
    // 例外内容を出力して失敗終了する
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        // テスト失敗を返す
        return 1;
    }
}
