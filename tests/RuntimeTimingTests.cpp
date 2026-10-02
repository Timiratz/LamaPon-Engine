#include "RuntimeTiming.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    // 条件違反の累積件数
    int g_failures{};

    // 条件違反を表示して累積件数を増やす(condition: 成立すべき条件, message: 違反時の説明)。
    void Require(const bool condition, const std::string& message)
    {
        // 成立しない条件を失敗件数へ反映します。
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
            ++g_failures;
        }
    }

    // 絶対差1e-6以下の時刻値を同値として扱う(left: 比較する左値, right: 比較する右値)。
    [[nodiscard]] bool NearlyEqual(
        const float left,
        const float right) noexcept
    {
        return std::abs(left - right) <= 1.0e-6f;
    }
}

// 実時間と固定刻みの区別・遅延上限・描画間引きと強制描画を検査する。
int main()
{
    using LamaPon::Cli::MakeRuntimeFrameTiming;
    using LamaPon::Cli::ShouldRenderRuntimeFrame;

    // 実時間を残して固定刻みで進む時刻
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

    // 実時間の刻みで進む時刻
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

    // 失敗条件があれば失敗コードを返します。
    if (g_failures != 0)
    {
        return EXIT_FAILURE;
    }
    std::cout << "Runtime timing tests passed.\n";
    return EXIT_SUCCESS;
}
