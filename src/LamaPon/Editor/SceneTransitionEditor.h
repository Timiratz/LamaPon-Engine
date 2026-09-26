#pragma once

#include "LamaPon/Scene/SceneTransition.h"

namespace LamaPon
{
    // 編集部品の結果です。changedは値が変わったフレーム、committedは
    // Undo履歴へ確定してよいフレーム（ドラッグの終了、チェックや
    // 選択肢の切り替え）です。
    struct SceneTransitionEditResult final
    {
        bool changed{};
        bool committed{};
        bool previewRequested{};
    };

    // SceneTransitionSettingsを編集するImGuiの部品です。UI Buttonの
    // InspectorとProject Settingsで共有します。呼び出し側がPushIDで
    // IDを分けてから呼びます。showPreviewButtonがtrueなら、Gameビューで
    // 再生するプレビューボタンを表示します。演出に関係する項目だけを
    // 表示し、Noneでは遷移の説明だけを表示します。
    [[nodiscard]] SceneTransitionEditResult DrawSceneTransitionEditor(
        SceneTransitionSettings& settings,
        bool showPreviewButton);

    // 標準の読み込み画面の文言・色・背景画像などを編集します。
    [[nodiscard]] SceneTransitionEditResult DrawSceneLoadingScreenEditor(
        SceneLoadingScreenSettings& settings);

    // Inspectorや説明文で使う日本語の表示名です。
    [[nodiscard]] const char* SceneTransitionEffectLabel(
        SceneTransitionEffect effect) noexcept;
    [[nodiscard]] const char* SceneTransitionDirectionLabel(
        SceneTransitionDirection direction) noexcept;
    [[nodiscard]] const char* SceneTransitionEasingLabel(
        SceneTransitionEasing easing) noexcept;
    [[nodiscard]] const char* SceneTransitionShaderPatternLabel(
        SceneTransitionShaderPattern pattern) noexcept;
}
