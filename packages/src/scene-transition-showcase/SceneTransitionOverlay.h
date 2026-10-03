#pragma once

#include "LamaPon/LamaPon.h"
#include "SceneTransitionAssets.h"

#include <filesystem>
#include <string>

namespace LamaPonSceneShowcase
{
    // Sceneを跨いで再利用するOverlay GameObject名
    inline constexpr auto OverlayObjectName = "SceneTransition.Overlay";
    // Overlay Spriteへ付けるScript型名
    inline constexpr auto OverlayScriptType = "SceneTransition.Overlay";
    // UIを覆うSpriteの描画順
    inline constexpr int OverlaySortOrder = 30000;

    // 遷移Overlayを準備します(scene: 所有Scene, look: 外観設定)。
    // GameObjectはScene間で保持し、遷移終了時に自動で隠します。
    inline void ShowOverlay(LamaPon::Scene& scene, const Look& look)
    {
        // Overlay Scriptへ渡す外観JSON
        const auto properties =
            nlohmann::json{ { "look", LookToJson(look) } }.dump();
        // 既存のOverlay GameObject
        auto* overlay = scene.FindGameObjectByName(OverlayObjectName);
        // 初回はOverlay GameObjectを作成します。
        if (overlay == nullptr)
        {
            overlay = &scene.CreateGameObject(OverlayObjectName);
        }
        // 全画面Rect Transformが未登録なら追加します。
        if (overlay->GetComponent<LamaPon::UIRectTransformComponent>()
            == nullptr)
        {
            // アンカーを四隅へ広げ、画面全体を覆う1枚のSpriteにします。
            overlay->AddComponent<LamaPon::UIRectTransformComponent>(
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 1.0f, 1.0f },
                DirectX::XMFLOAT2{ 0.5f, 0.5f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f },
                DirectX::XMFLOAT2{ 0.0f, 0.0f });
        }
        // Overlay Spriteが未登録なら追加します。
        if (overlay->GetComponent<LamaPon::SpriteRendererComponent>()
            == nullptr)
        {
            // 新しいOverlayに追加したSprite
            auto& sprite =
                overlay->AddComponent<LamaPon::SpriteRendererComponent>();
            sprite.SetSortOrder(OverlaySortOrder);
            // 最初のUpdateで覆い具合を反映するまでは描きません。
            sprite.SetEnabled(false);
        }
        // 既存Overlay Scriptは新しい外観で初期化し直します。
        if (auto* script =
                overlay->GetComponent<LamaPon::NativeScriptComponent>())
        {
            // Scriptを作り直し、新しい見た目で描き始めます。
            script->SetPropertiesJson(properties);
        }
        // 既存Scriptが無ければ新たに追加します。
        else
        {
            overlay->AddComponent<LamaPon::NativeScriptComponent>(
                OverlayScriptType,
                properties);
        }
        // 親なしOverlayだけScene破棄から保護します。
        if (!overlay->IsPersistent() && overlay->Parent() == nullptr)
        {
            scene.DontDestroyOnLoad(*overlay, OverlayObjectName);
        }
    }

    // 遷移Presetを開始します(scene: 現在Scene, presetPath: Preset, destination: 遷移先, error: 失敗理由)。
    // destinationが空ならSceneを変えず、覆い完了時にCoveredイベントを通知します。
    [[nodiscard]] inline bool PlayPreset(
        LamaPon::Scene& scene,
        const std::filesystem::path& presetPath,
        const std::filesystem::path& destination,
        std::string& error)
    {
        // Presetから読み込む時間設定
        LamaPon::SceneTransitionSettings timing;
        // Overlayに使うShader・色設定
        Look look;
        // 指定パスから読み込んだData Asset
        const auto asset = scene.LoadDataAsset(presetPath);
        // Preset内容を時間設定と外観へ復元します。
        if (!ReadPreset(*asset, timing, look))
        {
            error = "遷移プリセットを読み込めません: "
                + LamaPon::PathToUtf8(presetPath);
            return false;
        }
        // 現在のScene遷移管理
        auto& scenes = scene.Scenes();
        // Scene指定の有無で直接演出か非同期読込を選びます。
        const bool accepted = destination.empty()
            ? scenes.PlayTransition(timing)
            : scenes.RequestLoadAsync(destination, timing);
        // 受理されなければ原因を返します。
        if (!accepted)
        {
            error = "シーン遷移を開始できません: " + scenes.LastError();
            return false;
        }
        // 演出がある場合は覆いSpriteを準備します。
        if (look.effect != Effect::None)
        {
            ShowOverlay(scene, look);
        }
        return true;
    }
}
