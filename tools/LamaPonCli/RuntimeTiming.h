#pragma once

#include <algorithm>
#include <cstdint>

namespace LamaPon::Cli
{
    // 決定論テストではゲーム内時間だけを固定します。
    // 描画性能の統計まで固定値にすると、重いフレームでも常に60 FPSと報告してしまいます。
    struct RuntimeFrameTiming final
    {
        // 描画・性能統計に使う実経過秒です。
        float wallDeltaSeconds{};
        // simulationへ渡す時間step秒です。
        float simulationDeltaSeconds{};
    };

    // wall deltaとsimulation deltaを分けて作ります(elapsedWallSeconds: 実時間, deterministic: 固定有効, fixedDeltaSeconds: 固定step)
    [[nodiscard]] inline RuntimeFrameTiming MakeRuntimeFrameTiming(
        const float elapsedWallSeconds,
        const bool deterministic,
        const float fixedDeltaSeconds) noexcept
    {
        // 実時間deltaを0から100msへ制限します。
        const float wallDelta = std::clamp(
            elapsedWallSeconds,
            0.0f,
            0.1f);
        return {
            wallDelta,
            deterministic ? fixedDeltaSeconds : wallDelta,
        };
    }

    // 描画対象frameか返します(frameNumber: frame番号, renderEveryNFrames: 間引き幅, forceRender: 強制描画)
    // frame 0は起動直後の状態を表示するため描画対象です。
    [[nodiscard]] inline bool ShouldRenderRuntimeFrame(
        const std::uint64_t frameNumber,
        const std::uint32_t renderEveryNFrames,
        const bool forceRender) noexcept
    {
        // 0指定を1へ補正した描画間隔です。
        const auto cadence = std::max(renderEveryNFrames, 1u);
        return forceRender || frameNumber % cadence == 0;
    }
}
