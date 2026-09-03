#include "LamaPon/Core/Time.h"
#include "LamaPon/Reactive/Reactive.h"

#include <algorithm>

namespace
{
    // 時間の更新はメインスレッドからのみ行われます。
    float g_timeScale{ 1.0f };
    float g_deltaTime{};
    float g_unscaledDeltaTime{};
    double g_timeSinceStartup{};
    double g_unscaledTimeSinceStartup{};
    std::uint64_t g_frameCount{};
}

namespace LamaPon::Time
{
    float DeltaTime() noexcept
    {
        return g_deltaTime;
    }

    float UnscaledDeltaTime() noexcept
    {
        return g_unscaledDeltaTime;
    }

    double TimeSinceStartup() noexcept
    {
        return g_timeSinceStartup;
    }

    double UnscaledTimeSinceStartup() noexcept
    {
        return g_unscaledTimeSinceStartup;
    }

    std::uint64_t FrameCount() noexcept
    {
        return g_frameCount;
    }

    float TimeScale() noexcept
    {
        return g_timeScale;
    }

    void SetTimeScale(const float scale) noexcept
    {
        g_timeScale = std::clamp(scale, 0.0f, 100.0f);
    }

    bool IsPaused() noexcept
    {
        return g_timeScale <= 0.0f;
    }

    namespace Detail
    {
        void AdvanceFrame(
            const float unscaledDeltaTime) noexcept
        {
            const float safeDelta =
                std::max(unscaledDeltaTime, 0.0f);
            g_unscaledDeltaTime = safeDelta;
            g_deltaTime = safeDelta * g_timeScale;
            g_unscaledTimeSinceStartup +=
                static_cast<double>(safeDelta);
            g_timeSinceStartup +=
                static_cast<double>(g_deltaTime);
            ++g_frameCount;
        }

        void Reset() noexcept
        {
            Reactive::Detail::Reset();
            g_timeScale = 1.0f;
            g_deltaTime = 0.0f;
            g_unscaledDeltaTime = 0.0f;
            g_timeSinceStartup = 0.0;
            g_unscaledTimeSinceStartup = 0.0;
            g_frameCount = 0;
        }
    }
}
