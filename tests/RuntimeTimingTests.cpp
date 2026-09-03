#include "RuntimeTiming.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    int g_failures{};

    void Require(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right) noexcept
    {
        return std::abs(left - right) <= 1.0e-6f;
    }
}

int main()
{
    using LamaPon::Cli::MakeRuntimeFrameTiming;
    using LamaPon::Cli::ShouldRenderRuntimeFrame;

    const auto deterministic = MakeRuntimeFrameTiming(
        0.041f,
        true,
        1.0f / 60.0f);
    Require(
        NearlyEqual(deterministic.wallDeltaSeconds, 0.041f),
        "Deterministic simulation must retain real wall time for FPS.");
    Require(
        NearlyEqual(
            deterministic.simulationDeltaSeconds,
            1.0f / 60.0f),
        "Deterministic simulation must use the fixed timestep.");

    const auto realtime = MakeRuntimeFrameTiming(0.02f, false, 0.01f);
    Require(
        NearlyEqual(realtime.wallDeltaSeconds, 0.02f)
            && NearlyEqual(realtime.simulationDeltaSeconds, 0.02f),
        "Realtime simulation must use wall time.");
    Require(
        NearlyEqual(
            MakeRuntimeFrameTiming(1.0f, false, 0.01f)
                .wallDeltaSeconds,
            0.1f),
        "A stalled realtime frame must remain bounded.");

    Require(
        ShouldRenderRuntimeFrame(0, 4, false)
            && ShouldRenderRuntimeFrame(4, 4, false)
            && !ShouldRenderRuntimeFrame(3, 4, false),
        "Render cadence must include the first and each Nth frame.");
    Require(
        ShouldRenderRuntimeFrame(3, 4, true),
        "A screenshot or observation must force a render.");
    Require(
        ShouldRenderRuntimeFrame(9, 0, false),
        "A zero cadence must safely behave as every frame.");

    if (g_failures != 0)
    {
        return EXIT_FAILURE;
    }
    std::cout << "Runtime timing tests passed.\n";
    return EXIT_SUCCESS;
}
