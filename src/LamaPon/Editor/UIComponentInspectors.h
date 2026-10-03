#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace LamaPon
{
    class Component;

    struct RenderTexturePickerResult final
    {

        // 更新する値・未設定なら変更なし
        std::optional<std::string> value;
        // Undo履歴を確定するか
        bool commit{};
    };

    // 参照とコールバックは描画中だけ借用し、次のフレームへ保存しません。
    struct UIInspectorContext final
    {
        // 選択中の資産への借用参照
        const std::filesystem::path& selectedAsset;
        // 表示領域の幅・px
        std::uint32_t viewportWidth{};
        // 表示領域の高さ・px
        std::uint32_t viewportHeight{};
        // Undo履歴を確定します。
        std::function<void()> recordHistory;
        // 状態を通知します（string: 通知内容、bool: エラーか）。
        std::function<void(const std::string&, bool)> setStatus;
        // 画像を選択します（char*: 欄のID、string: 現在値）。
        std::function<RenderTexturePickerResult(const char*, const std::string&)>
            pickRenderTexture;
    };

    // 呼び出し側はウィンドウ・ComponentのID・編集可否を設定し、使用するコールバックを必ず提供します。
    // 対応するUI型を編集してtrueを返し、未対応ならfalseを返します(component: 編集対象, context: 描画中だけ借用する操作)。
    [[nodiscard]] bool DrawUIComponentInspector(
        Component& component, const UIInspectorContext& context);
}
