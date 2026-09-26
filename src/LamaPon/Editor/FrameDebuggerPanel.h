#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace LamaPon
{
    class FrameDebugger;
    using GameObjectId = std::uint64_t;

    // フレームを構成する描画イベントを一覧にし、選んだイベントまでで
    // 描画を止めた途中の絵をScene View / Game Viewへ出すパネルです
    // （UnityのFrame Debuggerに相当）。FrameDebuggerは借用し、パネルは
    // 選択中のイベントと表示の絞り込みを所有します。
    class FrameDebuggerPanel final
    {
    public:
        using SelectObject = std::function<void(GameObjectId)>;
        // 再生中なら一時停止します。フレームを止めないと、イベントの
        // 並びが毎フレーム変わって選択が安定しないためです。
        using PauseGame = std::function<void()>;

        FrameDebuggerPanel(
            FrameDebugger& debugger,
            SelectObject select,
            PauseGame pause);

        FrameDebuggerPanel(const FrameDebuggerPanel&) = delete;
        FrameDebuggerPanel& operator=(const FrameDebuggerPanel&) = delete;

        void Draw(const char* title, bool& open);
        // パネルを閉じたらデバッガーを止め、全て描く状態へ戻します。
        // EditorLayerが毎フレーム、パネルの開閉状態を渡して呼びます。
        void SynchronizeEnabled(bool panelOpen);

        // 指定したイベントまで描きます。nulloptで全て描きます。
        void SelectEvent(std::optional<std::uint32_t> index);
        [[nodiscard]] std::optional<std::uint32_t>
            SelectedEvent() const noexcept
        {
            return m_selected;
        }

    private:
        void DrawEventList();
        void DrawEventDetails();

        FrameDebugger& m_debugger;
        SelectObject m_select;
        PauseGame m_pause;
        std::optional<std::uint32_t> m_selected;
        std::string m_filter;
        bool m_active{};
        bool m_scrollToSelection{};
    };
}
