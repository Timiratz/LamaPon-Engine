#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace LamaPon
{
    // 拡張が追加するドッキングパネルで、描画callbackは表示状態をImGui::Beginのopen引数へ渡す。
    struct EditorPanelDefinition final
    {
        // 全拡張で一意のパネルID
        std::string id;
        // 画面に表示するパネル名
        std::string displayName;
        // 初回のパネル表示状態
        bool defaultOpen{};
        // ウィンドウメニューに表示するか
        bool showInWindowMenu{ true };
        // 表示状態を更新できるパネル描画処理(open: 現在の表示状態の参照)。
        std::function<void(bool& open)> draw;
        // メニューの分類名・空は直下
        std::string windowMenuGroup;
    };

    // 1つの拡張のパネルとライフサイクル処理をまとめ、menuInlineがfalseなら拡張メニュー配下へ表示する。
    struct EditorExtensionDefinition final
    {
        // 登録所内で一意の拡張ID
        std::string id;
        // 画面に表示する拡張名
        std::string displayName;
        // 拡張に属するパネル定義
        std::vector<EditorPanelDefinition> panels;
        // 登録後に1度呼ぶ初期化処理
        std::function<void()> onAttach;
        // 登録順に呼ぶフレーム更新処理
        std::function<void()> onUpdate;
        // 拡張が追加するメニュー描画処理
        std::function<void()> drawMenu;
        // 登録解除または終了時の解放処理
        std::function<void()> onShutdown;
        // メニューを直下へ表示するか
        bool menuInline{};
    };

    struct RegisteredEditorPanel final
    {
        // 所属する拡張のID
        std::string extensionId;
        // 登録済みパネルのID
        std::string id;
        // 画面に表示するパネル名
        std::string displayName;
        // リセット時のパネル表示状態
        bool defaultOpen{};
        // 現在のパネル表示状態
        bool open{};
        // ウィンドウメニューに表示するか
        bool showInWindowMenu{ true };
        // 表示状態を更新できるパネル描画処理(open: 現在の表示状態の参照)。
        std::function<void(bool& open)> draw;
        // メニューの分類名・空は直下
        std::string windowMenuGroup;
    };

    struct RegisteredEditorExtension final
    {
        // 登録済み拡張のID
        std::string id;
        // 画面に表示する拡張名
        std::string displayName;
        // 登録順に呼ぶフレーム更新処理
        std::function<void()> onUpdate;
        // 拡張が追加するメニュー描画処理
        std::function<void()> drawMenu;
        // 登録解除または終了時の解放処理
        std::function<void()> onShutdown;
        // メニューを直下へ表示するか
        bool menuInline{};
    };

    // 組込み機能とプラグインのパネル・callback・表示状態をEditorLayerから独立して管理する。
    class EditorExtensionRegistry final
    {
    public:
        // 空の拡張登録所を作る。
        EditorExtensionRegistry() = default;
        // 登録順の逆順で終了処理を呼んで登録を解放する。
        ~EditorExtensionRegistry();

        // 拡張callbackと表示状態の共有を禁止する。
        EditorExtensionRegistry(const EditorExtensionRegistry&) = delete;
        // 拡張callbackと表示状態の共有を禁止する。
        EditorExtensionRegistry& operator=(
            const EditorExtensionRegistry&) = delete;

        // 定義を検証して登録しonAttach失敗時は追加を戻す(definition: 登録定義の所有先, error: 失敗理由の出力先・省略可)。
        bool Register(
            EditorExtensionDefinition definition,
            std::string* error = nullptr);
        // 表示状態を保持して拡張を解除し終了処理の例外は抑制する(extensionId: 解除する拡張のID)。
        bool Unregister(std::string_view extensionId) noexcept;
        // 登録の逆順で終了処理を呼び例外を抑制して登録と復元待ち状態を解放する。
        void Shutdown() noexcept;

        // 登録順にフレーム更新callbackを呼ぶ。
        void Update();
        // 表示中のパネルの描画callbackへ開閉状態の参照を渡す。
        void DrawPanels();
        // 復元待ち状態を消して全パネルを既定の表示状態へ戻す。
        void ResetPanelVisibility() noexcept;

        // 一覧や要素の参照は登録・解除・終了で無効になり得るため保持中は登録を変更しない。
        // 登録パネルを借用し不在ならnullを返す(panelId: 探すパネルのID)。
        [[nodiscard]] RegisteredEditorPanel* FindPanel(
            std::string_view panelId) noexcept;
        // 登録パネルを読み取り専用で借用し不在ならnullを返す(panelId: 探すパネルのID)。
        [[nodiscard]] const RegisteredEditorPanel* FindPanel(
            std::string_view panelId) const noexcept;
        // 登録済みのパネルの表示状態を更新し不在ならfalseを返す(panelId: 操作するパネルのID, open: 設定する表示状態)。
        [[nodiscard]] bool SetPanelOpen(
            std::string_view panelId,
            bool open) noexcept;
        // 未登録ならIDと表示状態を保持して後から登録された時点で復元する(panelId: 復元するパネルのID, open: 復元する表示状態)。
        void RestorePanelVisibility(
            std::string_view panelId,
            bool open);
        // 指定パネルが登録済みで表示中かを返す(panelId: 確認するパネルのID)。
        [[nodiscard]] bool IsPanelOpen(
            std::string_view panelId) const noexcept;

        // 登録順のパネル一覧を借用する。
        [[nodiscard]] std::vector<RegisteredEditorPanel>& Panels()
            noexcept
        {
            return m_panels;
        }
        // 登録順のパネル一覧を読み取り専用で借用する。
        [[nodiscard]] const std::vector<RegisteredEditorPanel>& Panels()
            const noexcept
        {
            return m_panels;
        }
        // 登録順の拡張一覧を読み取り専用で借用する。
        [[nodiscard]] const std::vector<RegisteredEditorExtension>&
            Extensions() const noexcept
        {
            return m_extensions;
        }

    private:
        // 登録順の拡張とcallbackの所有先
        std::vector<RegisteredEditorExtension> m_extensions;
        // 登録順のパネルと表示状態
        std::vector<RegisteredEditorPanel> m_panels;
        // 未登録パネルIDの復元待ち表示状態
        std::unordered_map<std::string, bool>
            m_pendingPanelVisibility;
    };
}
