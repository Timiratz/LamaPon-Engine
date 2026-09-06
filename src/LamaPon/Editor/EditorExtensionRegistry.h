#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    // 拡張機能が追加するドッキング可能パネルです。描画コールバックには
    // 開閉状態が渡されるため、ImGui::Beginのopen引数へそのまま使えます。
    struct EditorPanelDefinition final
    {
        std::string id;
        std::string displayName;
        bool defaultOpen{};
        bool showInWindowMenu{ true };
        std::function<void(bool& open)> draw;
    };

    // 一つの機能に属するパネルとライフサイクル処理をまとめます。
    // menuInline=falseのメニューは「拡張機能」配下のサブメニューになります。
    struct EditorExtensionDefinition final
    {
        std::string id;
        std::string displayName;
        std::vector<EditorPanelDefinition> panels;
        std::function<void()> onAttach;
        std::function<void()> onUpdate;
        std::function<void()> drawMenu;
        std::function<void()> onShutdown;
        bool menuInline{};
    };

    struct RegisteredEditorPanel final
    {
        std::string extensionId;
        std::string id;
        std::string displayName;
        bool defaultOpen{};
        bool open{};
        bool showInWindowMenu{ true };
        std::function<void(bool& open)> draw;
    };

    struct RegisteredEditorExtension final
    {
        std::string id;
        std::string displayName;
        std::function<void()> onUpdate;
        std::function<void()> drawMenu;
        std::function<void()> onShutdown;
        bool menuInline{};
    };

    // EditorLayerから独立した登録所です。今後の組み込み機能やプラグインは、
    // EditorLayerのDrawや設定JSONを直接編集せず、このAPIへ登録できます。
    class EditorExtensionRegistry final
    {
    public:
        EditorExtensionRegistry() = default;
        ~EditorExtensionRegistry();

        EditorExtensionRegistry(const EditorExtensionRegistry&) = delete;
        EditorExtensionRegistry& operator=(
            const EditorExtensionRegistry&) = delete;

        bool Register(
            EditorExtensionDefinition definition,
            std::string* error = nullptr);
        bool Unregister(std::string_view extensionId) noexcept;
        void Shutdown() noexcept;

        void Update();
        void DrawPanels();
        void ResetPanelVisibility() noexcept;

        [[nodiscard]] RegisteredEditorPanel* FindPanel(
            std::string_view panelId) noexcept;
        [[nodiscard]] const RegisteredEditorPanel* FindPanel(
            std::string_view panelId) const noexcept;
        [[nodiscard]] bool SetPanelOpen(
            std::string_view panelId,
            bool open) noexcept;
        // 設定の読み込み時点で未登録のパネルも記憶し、後からそのIDの
        // 拡張機能が登録されたときに復元します。
        void RestorePanelVisibility(
            std::string_view panelId,
            bool open);
        [[nodiscard]] bool IsPanelOpen(
            std::string_view panelId) const noexcept;

        [[nodiscard]] std::vector<RegisteredEditorPanel>& Panels()
            noexcept
        {
            return m_panels;
        }
        [[nodiscard]] const std::vector<RegisteredEditorPanel>& Panels()
            const noexcept
        {
            return m_panels;
        }
        [[nodiscard]] const std::vector<RegisteredEditorExtension>&
            Extensions() const noexcept
        {
            return m_extensions;
        }

    private:
        std::vector<RegisteredEditorExtension> m_extensions;
        std::vector<RegisteredEditorPanel> m_panels;
        std::unordered_map<std::string, bool>
            m_pendingPanelVisibility;
    };
}
