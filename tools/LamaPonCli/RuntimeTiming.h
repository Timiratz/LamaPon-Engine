#pragma once

#include <algorithm>
#include <cstdint>

namespace LamaPon::Cli
{
    // 決定論テストではゲーム内時間だけを固定します。描画性能の統計まで
    // 固定値にすると、重いフレームでも常に60 FPSと報告してしまいます。
    struct RuntimeFrameTiming final
    {
        float wallDeltaSeconds{};
        float simulationDeltaSeconds{};
    };

    [[nodiscard]] inline RuntimeFrameTiming MakeRuntimeFrameTiming(
        const float elapsedWallSeconds,
        const bool deterministic,
        const float fixedDeltaSeconds) noexcept
    {
        const float wallDelta = std::clamp(
            elapsedWallSeconds,
            0.0f,
            0.1f);
        return {
            wallDelta,
            deterministic ? fixedDeltaSeconds : wallDelta,
        };
    }

    // frameNumberは次に更新するシミュレーションフレームです。0を含める
    // ことで、描画間引き中でも起動直後の状態を必ず一度表示します。
    [[nodiscard]] inline bool ShouldRenderRuntimeFrame(
        const std::uint64_t frameNumber,
        const std::uint32_t renderEveryNFrames,
        const bool forceRender) noexcept
    {
        const auto cadence = std::max(renderEveryNFrames, 1u);
        return forceRender || frameNumber % cadence == 0;
    }
}
