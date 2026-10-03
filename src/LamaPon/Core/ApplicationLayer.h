#pragma once

#include <Windows.h>
#include <cstdint>
#include <utility>

namespace LamaPon
{
    struct InputSnapshot;

    class ApplicationLayer
    {
    public:
        // 派生レイヤーの資源を破棄します。
        virtual ~ApplicationLayer() = default;

        // レイヤーの複製を禁止します。
        ApplicationLayer(const ApplicationLayer&) = delete;
        // レイヤーのコピー代入を禁止します。
        ApplicationLayer& operator=(const ApplicationLayer&) = delete;

        // 窓メッセージを処理し、消費したかを返します(window: 対象の窓, message: メッセージID, wParam: メッセージの第1情報, lParam: メッセージの第2情報)。
        [[nodiscard]] virtual bool HandleMessage(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam) const = 0;

        // フレーム開始時の入力とGUI状態を更新します。
        virtual void BeginFrame() = 0;
        // このフレームのGUIを構築します。
        virtual void Draw() = 0;
        // シーンビューとゲームビューを描画します。
        virtual void RenderSceneViews() = 0;
        // 構築済みのGUIを描画先へ送ります。
        virtual void Render() = 0;

        // ゲームを再生しているかを返します。
        [[nodiscard]] virtual bool IsPlaying() const noexcept = 0;
        // ゲームの更新を一時停止しているかを返します。
        // trueの間はシミュレーション更新を止め、描画を継続する。
        [[nodiscard]] virtual bool IsPaused() const noexcept
        {
            return false;
        }
        // 一時停止中の1フレーム更新要求を消費し、更新を通すか返します。
        [[nodiscard]] virtual bool ConsumeSimulationStep() noexcept
        {
            return false;
        }
        // ハードウェア入力に優先する、このフレームの入力を取得します。
        [[nodiscard]] virtual bool ConsumeInputSnapshot(
            InputSnapshot&) noexcept
        {
            return false;
        }
        // GUIがキーボード入力を優先して受け取るかを返します。
        [[nodiscard]] virtual bool WantsKeyboard() const noexcept = 0;

        // 再生中のゲームビューの論理解像度を変更し、適用したか返します。
        [[nodiscard]] virtual bool SetGameViewSize(
            std::uint32_t, std::uint32_t) { return false; }
        // ゲームビューの論理解像度を返し、未対応なら0を返します。
        [[nodiscard]] virtual std::pair<std::uint32_t, std::uint32_t>
            GameViewSize() const noexcept { return { 0, 0 }; }

        // 未保存変更などを確認し、ウィンドウを閉じてよければtrueを返す。
        [[nodiscard]] virtual bool ConfirmClose() { return true; }

    protected:
        // 派生クラスからレイヤーを初期化します。
        ApplicationLayer() = default;
    };
}
