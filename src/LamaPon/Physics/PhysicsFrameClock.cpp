#include "LamaPon/Physics/PhysicsFrameClock.h"

#include <cmath>

namespace LamaPon::Detail
{
    void PhysicsFrameClock::BeginFrame(const float deltaTime,
        const float fixedDeltaTime,
        const std::size_t maximumCatchUpSteps) noexcept
    {
        m_timing.fixedSteps = 0;
        m_timing.discardedDeltaTime = 0.0;
        m_advancing = std::isfinite(deltaTime) && deltaTime > 0.0f;
        if (!m_advancing)
        {
            // 停止・再開時に姿勢が巻き戻らないよう、最後の補間位置を保持します。
            return;
        }
        m_timing.fixedDeltaTime =
            std::isfinite(fixedDeltaTime) && fixedDeltaTime > 0.0f
                ? fixedDeltaTime : 1.0f / 60.0f;
        m_maximumCatchUpSteps = std::max(std::size_t{ 1 }, maximumCatchUpSteps);
        // 0.1秒以下に制限した進行時間
        const double acceptedDelta = static_cast<double>(
            std::min(deltaTime, 0.1f));
        // 以前の未実行分と今回の合計秒数
        const double pendingTime = m_accumulator + acceptedDelta;
        // 固定更新の上限で消化できる秒数
        const double capacity = static_cast<double>(m_timing.fixedDeltaTime)
            * static_cast<double>(m_maximumCatchUpSteps);
        m_accumulator = std::min(pendingTime, capacity);
        m_timing.discardedDeltaTime = static_cast<double>(deltaTime)
            - acceptedDelta + std::max(0.0, pendingTime - capacity);
        m_timing.discardedTime += m_timing.discardedDeltaTime;
    }

    bool PhysicsFrameClock::PendingStep() const noexcept
    {
        // 現在の固定刻みの秒数
        const double step = static_cast<double>(m_timing.fixedDeltaTime);
        return m_advancing && m_timing.fixedSteps < m_maximumCatchUpSteps
            && m_accumulator + step * 0.00001 >= step;
    }

    void PhysicsFrameClock::CompleteStep() noexcept
    {
        // 現在の固定刻みの秒数
        const double step = static_cast<double>(m_timing.fixedDeltaTime);
        m_accumulator = std::max(0.0, m_accumulator - step);
        m_lastStepDuration = step;
        m_timing.simulatedTime += step;
        ++m_timing.fixedSteps;
    }

    void PhysicsFrameClock::FinishFrame() noexcept
    {
        if (!m_advancing)
        {
            return;
        }
        m_timing.interpolationAlpha = std::clamp(
            static_cast<float>(m_accumulator / m_timing.fixedDeltaTime),
            0.0f, 1.0f);
        // 完了済みステップがない場合は過去の姿勢がないため、補間遅延を0にします。
        m_timing.interpolationDelay = m_lastStepDuration
            * (1.0 - static_cast<double>(m_timing.interpolationAlpha));
    }
}
