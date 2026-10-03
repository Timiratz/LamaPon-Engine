#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace LamaPon
{
    class FrameDebugger;
    using GameObjectId = std::uint64_t;

    // FrameDebuggerを借用し、描画イベントの選択と絞込を所有して途中の描画結果をScene View・Game Viewへ表示する。
    class FrameDebuggerPanel final
    {
    public:
        // GameObject IDを受け取り選択するcallback
        using SelectObject = std::function<void(GameObjectId)>;
        // イベントの順序を固定するため再生を一時停止するcallback。
        using PauseGame = std::function<void()>;

        // 描画記録を借用して選択・停止callbackを所有する(debugger: panelより長く存続する記録処理, select: GameObjectを選ぶ処理, pause: 再生を一時停止する処理)。
        FrameDebuggerPanel(
            FrameDebugger& debugger,
            SelectObject select,
            PauseGame pause);

        // 描画記録とpanel状態の共有を禁止する。
        FrameDebuggerPanel(const FrameDebuggerPanel&) = delete;
        // 描画記録とpanel状態の共有を禁止する。
        FrameDebuggerPanel& operator=(const FrameDebuggerPanel&) = delete;

        // 描画イベントの一覧と選択内容・途中結果の操作を表示する(title: ウィンドウの表示名とID, open: panelの表示状態の参照)。
        void Draw(const char* title, bool& open);
        // 毎フレーム呼びpanelが閉じていれば記録と描画上限を解除する(panelOpen: 現在のpanelの表示状態)。
        void SynchronizeEnabled(bool panelOpen);

        // 指定イベントまで描く上限を設定し選択時は再生を一時停止する(index: 0始まりの番号・未指定で全描画)。
        void SelectEvent(std::optional<std::uint32_t> index);
        // 選択中の0始まり描画上限を返し全描画ならnulloptを返す。
        [[nodiscard]] std::optional<std::uint32_t>
            SelectedEvent() const noexcept
        {
            return m_selected;
        }

    private:
        // 検索条件に合う描画イベントをGPU区間ごとに表示する。
        void DrawEventList();
        // 選択した描画の形状・材質・状態と対象選択操作を表示する。
        void DrawEventDetails();

        // panelより長く存続する記録処理
        FrameDebugger& m_debugger;
        // 対象GameObjectを選ぶ処理
        SelectObject m_select;
        // 再生中のゲームを止める処理
        PauseGame m_pause;
        // 最後に描く0始まりイベント番号
        std::optional<std::uint32_t> m_selected;
        // 表示するイベントの検索文字列
        std::string m_filter;
        // ユーザーが記録を有効にしたか
        bool m_active{};
        // 選択イベントまでscrollするか
        bool m_scrollToSelection{};
    };
}
