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
            // 停止中は最後に表示した補間位置を保持します。alphaを0へ戻すと、
            // 停止／再開時に直前の固定位置へ巻き戻るためです。
            return;
        }
        m_timing.fixedDeltaTime =
            std::isfinite(fixedDeltaTime) && fixedDeltaTime > 0.0f
                ? fixedDeltaTime : 1.0f / 60.0f;
        m_maximumCatchUpSteps = std::max(std::size_t{ 1 }, maximumCatchUpSteps);
        const double acceptedDelta = static_cast<double>(
            std::min(deltaTime, 0.1f));
        const double pendingTime = m_accumulator + acceptedDelta;
        const double capacity = static_cast<double>(m_timing.fixedDeltaTime)
            * static_cast<double>(m_maximumCatchUpSteps);
        m_accumulator = std::min(pendingTime, capacity);
        m_timing.discardedDeltaTime = static_cast<double>(deltaTime)
            - acceptedDelta + std::max(0.0, pendingTime - capacity);
        m_timing.discardedTime += m_timing.discardedDeltaTime;
    }

    bool PhysicsFrameClock::PendingStep() const noexcept
    {
        const double step = static_cast<double>(m_timing.fixedDeltaTime);
        return m_advancing && m_timing.fixedSteps < m_maximumCatchUpSteps
            && m_accumulator + step * 0.00001 >= step;
    }

    void PhysicsFrameClock::CompleteStep() noexcept
    {
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
        // 最後に完了した物理ステップの長さを使います。まだ一度も進んで
        // いないSceneには過去の姿勢がないため、遅延も0のままです。
        m_timing.interpolationDelay = m_lastStepDuration
            * (1.0 - static_cast<double>(m_timing.interpolationAlpha));
    }
}
