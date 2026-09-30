#pragma once

#include "LamaPon/LamaPon.h"
#include "SceneTransitionAssets.h"

#include <filesystem>
#include <string>

namespace LamaPonSceneShowcase
{
    // 覆いを描くGameObjectとScriptの名前です。GameObjectはシーンを
    // 切り替えても残り（DontDestroyOnLoad）、次の遷移でも使い回します。
    inline constexpr auto OverlayObjectName = "SceneTransition.Overlay";
    inline constexpr auto OverlayScriptType = "SceneTransition.Overlay";
    // UIより手前に描きます（エンジンの読み込み画面はさらに手前です）。
    inline constexpr int OverlaySortOrder = 30000;

    // 覆いを用意して、lookの見た目で次の遷移を描かせます。遷移を
    // 始めた直後に呼びます。覆いは遷移が終わると自動で隠れます。
    inline void ShowOverlay(LamaPon::Scene& scene, const Look& look)
    {
        const auto properties =
            nlohmann::json{ { "look", LookToJson(look) } }.dump();
        auto* overlay = scene.FindGameObjectByName(OverlayObjectName);
        if (overlay == nullptr)
        {
            overlay = &scene.CreateGameObject(OverlayObjectName);
        }
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
        if (overlay->GetComponent<LamaPon::SpriteRendererComponent>()
            == nullptr)
        {
            auto& sprite =
                overlay->AddComponent<LamaPon::SpriteRendererComponent>();
            sprite.SetSortOrder(OverlaySortOrder);
            // 最初のUpdateで覆い具合を反映するまでは描きません。
            sprite.SetEnabled(false);
        }
        if (auto* script =
                overlay->GetComponent<LamaPon::NativeScriptComponent>())
        {
            // Scriptを作り直し、新しい見た目で描き始めます。
            script->SetPropertiesJson(properties);
        }
        else
        {
            overlay->AddComponent<LamaPon::NativeScriptComponent>(
                OverlayScriptType,
                properties);
        }
        if (!overlay->IsPersistent() && overlay->Parent() == nullptr)
        {
            scene.DontDestroyOnLoad(*overlay, OverlayObjectName);
        }
    }

    // プリセットを読み、遷移を始めて覆いを表示します。destinationが
    // 空ならシーンを切り替えずに覆って開きます（覆い終えたときに
    // SceneTransition.Coveredイベントが届きます）。開始できなければ
    // errorへ理由を入れてfalseを返します。
    [[nodiscard]] inline bool PlayPreset(
        LamaPon::Scene& scene,
        const std::filesystem::path& presetPath,
        const std::filesystem::path& destination,
        std::string& error)
    {
        LamaPon::SceneTransitionSettings timing;
        Look look;
        const auto asset = scene.LoadDataAsset(presetPath);
        if (!ReadPreset(*asset, timing, look))
        {
            error = "遷移プリセットを読み込めません: "
                + LamaPon::PathToUtf8(presetPath);
            return false;
        }
        auto& scenes = scene.Scenes();
        const bool accepted = destination.empty()
            ? scenes.PlayTransition(timing)
            : scenes.RequestLoadAsync(destination, timing);
        if (!accepted)
        {
            error = "シーン遷移を開始できません: " + scenes.LastError();
            return false;
        }
        if (look.effect != Effect::None)
        {
            ShowOverlay(scene, look);
        }
        return true;
    }
}
