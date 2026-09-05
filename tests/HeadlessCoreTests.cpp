#include "LamaPon/Core/JobSystem.h"
#include "LamaPon/Core/Profiler.h"
#include "LamaPon/Core/Time.h"
#include "LamaPon/Core/VersionCompare.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(LAMAPON_HEADLESS)
#error "HeadlessCoreTests must be built with the headless configuration."
#endif

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
        const double left,
        const double right,
        const double tolerance = 0.000001) noexcept
    {
        return std::abs(left - right) <= tolerance;
    }
}

int main()
{
    using namespace LamaPon;

    const auto version = ParseVersionNumbers("v2026.9.6");
    Require(
        version == std::vector<std::uint32_t>{ 2026, 9, 6 },
        "version parsing must be platform independent");
    Require(
        IsNewerVersion("2026.9.6", "2026.9.6.1"),
        "version comparison must handle additional components");
    Require(
        !IsNewerVersion("invalid", "2026.9.6"),
        "invalid versions must be rejected");

    Time::Detail::Reset();
    Time::SetTimeScale(0.5f);
    Time::Detail::AdvanceFrame(0.02f);
    Require(
        NearlyEqual(Time::UnscaledDeltaTime(), 0.02),
        "unscaled time must retain the frame delta");
    Require(
        NearlyEqual(Time::DeltaTime(), 0.01),
        "scaled time must apply the time scale");
    Require(Time::FrameCount() == 1, "frame count must advance");
    Time::SetTimeScale(0.0f);
    Require(Time::IsPaused(), "zero time scale must pause simulation");
    Time::Detail::Reset();

    constexpr std::size_t ItemCount = 1024;
    std::vector<std::atomic_uint> visits(ItemCount);
    JobSystem::Instance().ParallelFor(
        ItemCount,
        17,
        [&visits](const std::size_t begin, const std::size_t end)
        {
            for (std::size_t index = begin; index < end; ++index)
            {
                visits[index].fetch_add(1, std::memory_order_relaxed);
            }
        });
    for (std::size_t index{}; index < visits.size(); ++index)
    {
        Require(
            visits[index].load(std::memory_order_relaxed) == 1,
            "parallel jobs must process each item exactly once");
    }

    bool exceptionReturned{};
    try
    {
        JobSystem::Instance().ParallelFor(
            8,
            1,
            [](const std::size_t begin, const std::size_t)
            {
                if (begin == 0)
                {
                    throw std::runtime_error("expected headless job failure");
                }
            });
    }
    catch (const std::runtime_error&)
    {
        exceptionReturned = true;
    }
    Require(
        exceptionReturned,
        "worker exceptions must return to the calling thread");

    auto& profiler = Profiler::Instance();
    profiler.Clear();
    profiler.SetEnabled(true);
    profiler.BeginFrame();
    profiler.Record("HeadlessUpdate", std::chrono::milliseconds(2));
    profiler.EndFrame();
    const auto frames = profiler.Snapshot();
    Require(
        frames.size() == 1
            && frames.front().samples.size() == 1
            && frames.front().samples.front().name == "HeadlessUpdate",
        "the CPU profiler must work without a graphics device");

    if (g_failures != 0)
    {
        std::cerr << g_failures << " headless assertion(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "Headless core tests passed.\n";
    return EXIT_SUCCESS;
}
