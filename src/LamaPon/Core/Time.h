#pragma once

#include <cstdint>

namespace LamaPon::Time
{
    // 現在フレームのスケール済みdeltaTime（秒）。
    // ゲームプレイのUpdateコールバックが受け取る値と同じです。
    [[nodiscard]] float DeltaTime() noexcept;
    // タイムスケール適用前のdeltaTime（秒）。
    [[nodiscard]] float UnscaledDeltaTime() noexcept;
    // アプリケーション起動からのスケール済み経過秒数。
    [[nodiscard]] double TimeSinceStartup() noexcept;
    // アプリケーション起動からのスケールなし経過秒数。
    [[nodiscard]] double UnscaledTimeSinceStartup() noexcept;
    // アプリケーション起動からのフレーム数。
    [[nodiscard]] std::uint64_t FrameCount() noexcept;

    // ゲームプレイ速度の倍率。0で一時停止、1で等速。
    // [0, 100]にクランプされます。
    [[nodiscard]] float TimeScale() noexcept;
    void SetTimeScale(float scale) noexcept;
    [[nodiscard]] bool IsPaused() noexcept;

    [[nodiscard]] constexpr float FixedDeltaTime() noexcept
    {
        return 1.0f / 60.0f;
    }

    namespace Detail
    {
        // アプリケーションが毎フレーム、生の（スケールなし・
        // クランプ済み）deltaTimeを渡して呼び出します。
        void AdvanceFrame(float unscaledDeltaTime) noexcept;
        // 時計をリセットし、タイムスケールを1へ戻します。
        // Play開始／停止とアプリケーション起動時に呼ばれます。
        void Reset() noexcept;
    }
}
