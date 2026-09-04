#pragma once

#include <algorithm>
#include <cstddef>

namespace LamaPon
{
    // Windows Runtimeの物理時計。Scene::PhysicsTiming()から読み取ります。
    // 当該フレームの値はLateUpdateで確定し、Updateでは前フレームの値です。
    // simulatedTimeはClear以降に実行した固定更新の合計で、実時間ではありません。
    struct PhysicsFrameTiming final
    {
        float fixedDeltaTime{ 1.0f / 60.0f };
        float interpolationAlpha{};
        std::size_t fixedSteps{};
        double simulatedTime{};
        double interpolationDelay{};
        // Sceneへ渡された時間のうち、上限によって実行しなかった秒数。
        // 上流で既に捨てられた時間や、GPU・配信処理の負荷は含みません。
        double discardedDeltaTime{};
        double discardedTime{};

        // FixedUpdateで積算した非負のゲーム内時刻を、Rigidbodyの描画時刻へ
        // 合わせます。全固定更新後のLateUpdateで、進行中のタイマーに使います。
        // 記録データのサンプリングは呼び出し側が行い、先読みはしません。
        [[nodiscard]] double InterpolateTime(
            const double fixedTime) const noexcept
        {
            return std::max(0.0, fixedTime - interpolationDelay);
        }

        [[nodiscard]] double PresentationTime() const noexcept
        {
            return InterpolateTime(simulatedTime);
        }
    };

    namespace Detail
    {
        // 蓄積時間・追いつき上限・描画時計を一緒に所有します。
        // SceneはPendingStepの間だけ物理を実行し、成功後にCompleteStepします。
        // 設定をフレーム開始時に固定し、コールバック中の変更で刻みを混ぜません。
        class PhysicsFrameClock final
        {
        public:
            void BeginFrame(float deltaTime, float fixedDeltaTime,
                std::size_t maximumCatchUpSteps) noexcept;
            [[nodiscard]] bool PendingStep() const noexcept;
            void CompleteStep() noexcept;
            void FinishFrame() noexcept;

            [[nodiscard]] const PhysicsFrameTiming& Timing() const noexcept
            {
                return m_timing;
            }

        private:
            PhysicsFrameTiming m_timing;
            double m_accumulator{};
            double m_lastStepDuration{};
            std::size_t m_maximumCatchUpSteps{};
            bool m_advancing{};
        };
    }
}
