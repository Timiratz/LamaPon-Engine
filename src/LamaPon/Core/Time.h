#pragma once

#include <cstdint>

// 時計の読み書きはメインスレッドで行う。
namespace LamaPon::Time
{
    // Updateへ渡す、タイムスケール適用後の経過秒数を返します。
    [[nodiscard]] float DeltaTime() noexcept;
    // タイムスケール適用前の経過秒数を返します。
    [[nodiscard]] float UnscaledDeltaTime() noexcept;
    // 起動からのタイムスケール適用後の経過秒数を返します。
    [[nodiscard]] double TimeSinceStartup() noexcept;
    // 起動からのタイムスケール適用前の経過秒数を返します。
    [[nodiscard]] double UnscaledTimeSinceStartup() noexcept;
    // 起動からのフレーム数を返します。
    [[nodiscard]] std::uint64_t FrameCount() noexcept;

    // 再生速度の倍率を返し、0は停止、1は等速を表します。
    [[nodiscard]] float TimeScale() noexcept;
    // 再生速度を0〜100の範囲に設定します(scale: 速度倍率)。
    void SetTimeScale(float scale) noexcept;
    // 再生速度が0ならtrueを返します。
    [[nodiscard]] bool IsPaused() noexcept;

    // 互換用の固定更新間隔として1/60秒を返します。
    // 実際の刻み幅はFixedUpdate引数、描画時刻はLateUpdateでScene::PhysicsTiming()を参照します。
    [[nodiscard]] constexpr float FixedDeltaTime() noexcept
    {
        return 1.0f / 60.0f;
    }

    namespace Detail
    {
        // 毎フレーム時計を進めます(unscaledDeltaTime: スケール適用前の経過秒数)。
        // アプリケーションがクランプ済みの経過秒数を渡します。
        void AdvanceFrame(float unscaledDeltaTime) noexcept;
        // 起動・再生切替時に時計と予約通知を初期化し、倍率を1に戻す。
        void Reset() noexcept;
    }
}
