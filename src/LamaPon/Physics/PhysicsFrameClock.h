#pragma once

#include <algorithm>
#include <cstddef>

namespace LamaPon
{
    // Scene::PhysicsTimingの当該フレーム値はLateUpdateで確定し、Updateでは前フレーム値です。
    // simulatedTimeは時計の初期化以降に実行した固定更新の合計秒数で、実時間ではありません。
    // 捨てた時間にはSceneへ渡る前の時間制限やGPU・配信処理の負荷を含めません。
    struct PhysicsFrameTiming final
    {
        // 現在の固定刻みの秒数
        float fixedDeltaTime{ 1.0f / 60.0f };
        // 描画補間の0〜1の進捗
        float interpolationAlpha{};
        // 今フレームの固定更新回数
        std::size_t fixedSteps{};
        // 固定更新の積算秒数
        double simulatedTime{};
        // 描画補間の遅延秒数
        double interpolationDelay{};
        // 今フレームで捨てた秒数
        double discardedDeltaTime{};
        // 捨てた時間の積算秒数
        double discardedTime{};

        // 固定更新の時刻を描画時刻へ合わせます(fixedTime: 固定更新で積算した秒数)。
        // 全固定更新後のLateUpdateで使い、記録データのサンプリングは呼び出し側が行います。
        [[nodiscard]] double InterpolateTime(
            const double fixedTime) const noexcept
        {
            return std::max(0.0, fixedTime - interpolationDelay);
        }

        // 積算したシミュレーション時刻から描画時刻を求めます。
        [[nodiscard]] double PresentationTime() const noexcept
        {
            return InterpolateTime(simulatedTime);
        }
    };

    namespace Detail
    {
        // BeginFrameで設定を固定し、PendingStepの間に物理を実行して成功後だけCompleteStepします。
        class PhysicsFrameClock final
        {
        public:
            // 時間と固定更新の上限を設定します(deltaTime: フレーム秒数, fixedDeltaTime: 固定刻みの秒数, maximumCatchUpSteps: 実行回数上限)。
            // 進行する時間は0.1秒以下に制限し、非正値・非有限値では最後の描画補間を維持します。
            void BeginFrame(float deltaTime, float fixedDeltaTime,
                std::size_t maximumCatchUpSteps) noexcept;
            // このフレームに実行可能な固定更新が残っているか返します。
            [[nodiscard]] bool PendingStep() const noexcept;
            // 成功した固定更新一回分を蓄積時間と時計へ反映します。
            void CompleteStep() noexcept;
            // 固定更新後の描画補間率と遅延秒数を確定します。
            void FinishFrame() noexcept;

            // 時計の現在状態を非所有参照で返します。
            [[nodiscard]] const PhysicsFrameTiming& Timing() const noexcept
            {
                return m_timing;
            }

        private:
            // 公開する物理時計の状態
            PhysicsFrameTiming m_timing;
            // 未実行の蓄積秒数
            double m_accumulator{};
            // 最後に完了した固定刻みの秒数
            double m_lastStepDuration{};
            // 今フレームの固定更新上限
            std::size_t m_maximumCatchUpSteps{};
            // 今フレームの進行有無
            bool m_advancing{};
        };
    }
}
