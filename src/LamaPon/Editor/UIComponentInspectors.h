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
        // 値の変更とUndoの確定を分け、ドラッグ中の履歴増殖を避けます。
        std::optional<std::string> value;
        bool commit{};
    };

    // UIコンポーネントの編集に必要な表示値と操作だけを借用します。
    // 参照・コールバックはDraw中にだけ使い、次のフレームへ保存しません。
    // 選択、Scene、GraphicsDevice、Undo履歴そのものは所有しません。
    struct UIInspectorContext final
    {
        const std::filesystem::path& selectedAsset;
        std::uint32_t viewportWidth{};
        std::uint32_t viewportHeight{};
        std::function<void()> recordHistory;
        std::function<void(const std::string&, bool)> setStatus;
        std::function<RenderTexturePickerResult(const char*, const std::string&)>
            pickRenderTexture;
    };

    // 呼び出し側がImGuiのウィンドウ・ComponentのID・編集可否を設定し、
    // 各コールバックを提供します。対応したComponentならtrueを返します。
    // 未対応なら描画・変更を行わず、次のInspectorへ委譲できます。
    [[nodiscard]] bool DrawUIComponentInspector(
        Component& component, const UIInspectorContext& context);
}
