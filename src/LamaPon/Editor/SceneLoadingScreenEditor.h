#pragma once

#include "LamaPon/Scene/SceneTransition.h"

namespace LamaPon
{
    // 継続入力は操作終了時、チェックや選択は変更時にUndoへ確定する編集結果。
    struct SceneLoadingScreenEditResult final
    {
        // 設定値が変わったか
        bool changed{};
        // Undo履歴へ確定する変更があるか
        bool committed{};
    };

    // 呼出し側でPushIDを使い部品のIDを分けてから描画する。
    // 読込画面を編集して変更とUndo確定を返す(settings: 編集する読込画面の設定)。
    [[nodiscard]] SceneLoadingScreenEditResult DrawSceneLoadingScreenEditor(
        SceneLoadingScreenSettings& settings);
}
