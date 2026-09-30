#pragma once

#include "LamaPon/Scene/SceneTransition.h"

namespace LamaPon
{
    // 編集部品の結果です。changedは値が変わったフレーム、committedは
    // Undo履歴へ確定してよいフレーム（ドラッグの終了、チェックの
    // 切り替え）です。
    struct SceneLoadingScreenEditResult final
    {
        bool changed{};
        bool committed{};
    };

    // 標準の読み込み画面の文言・色・背景画像などを編集するImGuiの
    // 部品です（Project Settingsの「ゲーム」で使います）。呼び出し側が
    // PushIDでIDを分けてから呼びます。
    [[nodiscard]] SceneLoadingScreenEditResult DrawSceneLoadingScreenEditor(
        SceneLoadingScreenSettings& settings);
}
