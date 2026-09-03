#pragma once

#include <Windows.h>

namespace LamaPon
{
    struct InputSnapshot;

    class ApplicationLayer
    {
    public:
        virtual ~ApplicationLayer() = default;

        ApplicationLayer(const ApplicationLayer&) = delete;
        ApplicationLayer& operator=(const ApplicationLayer&) = delete;

        [[nodiscard]] virtual bool HandleMessage(
            HWND window,
            UINT message,
            WPARAM wParam,
            LPARAM lParam) const = 0;

        virtual void BeginFrame() = 0;
        virtual void Draw() = 0;
        virtual void RenderSceneViews() = 0;
        virtual void Render() = 0;

        [[nodiscard]] virtual bool IsPlaying() const noexcept = 0;
        // 再生中に一時停止しているか。trueの間はゲームプレイの更新を
        // 止めます（描画は続けます）。
        //
        // timeScaleを0にする方式は採っていません。それだとUpdate自体は
        // 呼ばれ続けるので、deltaTimeを使っていない処理（フレーム単位の
        // カウンタや入力のポーリング）が進んでしまいます。
        [[nodiscard]] virtual bool IsPaused() const noexcept
        {
            return false;
        }
        // 一時停止中に「次のフレーム」が押されていたら、1回だけ更新を
        // 通すためにtrueを返し、要求を消費します。constでないのは
        // 消費する（状態が変わる）ためです。
        [[nodiscard]] virtual bool ConsumeSimulationStep() noexcept
        {
            return false;
        }
        // リモート操作や自動テストが、このフレームのゲーム入力を
        // ハードウェア入力より優先して差し替える入口です。
        [[nodiscard]] virtual bool ConsumeInputSnapshot(
            InputSnapshot&) noexcept
        {
            return false;
        }
        [[nodiscard]] virtual bool WantsKeyboard() const noexcept = 0;

        // ウィンドウを閉じてよいかを返します（falseで中止）。
        // エディターは未保存の変更がある場合にここで警告を出します。
        [[nodiscard]] virtual bool ConfirmClose() { return true; }

    protected:
        ApplicationLayer() = default;
    };
}
