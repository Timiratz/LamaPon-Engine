#include "AnalysisCommands.h"

#include "LamaPon/Core/MemorySnapshot.h"
#include "LamaPon/Core/Profiler.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    LamaPon::ProfileFrame Frame(
        const std::uint64_t index,
        const double render)
    {
        LamaPon::ProfileFrame frame;
        frame.index = index;
        frame.milliseconds = render + 1.0;
        LamaPon::ProfileSample sample;
        sample.name = "Render";
        sample.milliseconds = render;
        sample.callCount = 1;
        frame.samples.push_back(sample);
        return frame;
    }

    template<typename Function>
    bool Throws(Function&& function)
    {
        try
        {
            function();
        }
        catch (const std::exception&)
        {
            return true;
        }
        return false;
    }
}

int main()
{
    try
    {
        const auto root =
            std::filesystem::current_path()
            / "test-output"
            / "cli-analysis";
        std::error_code error;
        std::filesystem::remove_all(root, error);

        const auto profileA = root / "a.json";
        const auto profileB = root / "b.json";
        const std::vector<LamaPon::ProfileFrame> framesA{
            Frame(1, 2.0), Frame(2, 4.0), Frame(3, 6.0) };
        const std::vector<LamaPon::ProfileFrame> framesB{
            Frame(1, 8.0), Frame(2, 8.0) };
        Require(
            LamaPon::WriteProfileJson(profileA, framesA)
                && LamaPon::WriteProfileJson(profileB, framesB),
            "Profile fixtures could not be written.");

        const std::wstring profileAText = profileA.wstring();
        const std::wstring profileBText = profileB.wstring();

        const std::array analyzeArguments{
            std::wstring_view{ L"analyze" },
            std::wstring_view{ profileAText },
            std::wstring_view{ L"--first" },
            std::wstring_view{ L"1" },
            std::wstring_view{ L"--top" },
            std::wstring_view{ L"5" },
        };
        const auto analyzed = LamaPon::Cli::RunAnalysisCommand(
            L"profile",
            analyzeArguments);
        Require(
            analyzed["ok"].get<bool>()
                && analyzed["command"] == "profile analyze"
                && analyzed["range"]["first"] == 1
                && analyzed["range"]["last"] == 2
                && analyzed["analysis"]["frameCount"] == 2
                && analyzed["analysis"]["markers"][0]["path"] == "Render"
                && analyzed["analysis"]["markers"][0]["milliseconds"]
                    ["median"].get<double>() == 5.0,
            "profile analyze did not honour the frame range.");

        const std::array compareArguments{
            std::wstring_view{ L"compare" },
            std::wstring_view{ profileAText },
            std::wstring_view{ profileBText },
        };
        const auto compared = LamaPon::Cli::RunAnalysisCommand(
            L"profile",
            compareArguments);
        Require(
            compared["comparison"]["frameMedianDifference"]
                    .get<double>() == 9.0 - 5.0
                && compared["comparison"]["markers"][0]["status"]
                    == "changed"
                && compared["comparison"]["markers"][0]
                    ["medianDifference"].get<double>() == 4.0,
            "profile compare did not report the slowdown.");

        LamaPon::MemorySnapshot before;
        before.label = "before";
        before.entries.push_back({
            LamaPon::MemoryCategory::Texture,
            "a.png",
            "",
            100,
            0 });
        auto after = before;
        after.label = "after";
        after.entries.push_back({
            LamaPon::MemoryCategory::Model,
            "hero.glb",
            "",
            300,
            50 });
        const auto memoryA = root / "memory-a.json";
        const auto memoryB = root / "memory-b.json";
        Require(
            LamaPon::WriteMemorySnapshotJson(memoryA, before)
                && LamaPon::WriteMemorySnapshotJson(memoryB, after),
            "Memory fixtures could not be written.");
        const std::wstring memoryAText = memoryA.wstring();
        const std::wstring memoryBText = memoryB.wstring();

        const std::array summaryArguments{
            std::wstring_view{ L"summary" },
            std::wstring_view{ memoryBText },
        };
        const auto summary = LamaPon::Cli::RunAnalysisCommand(
            L"memory",
            summaryArguments);
        Require(
            summary["summary"]["entryCount"] == 2
                && summary["summary"]["entries"][0]["name"] == "hero.glb",
            "memory summary did not order entries by size.");

        const std::array memoryCompareArguments{
            std::wstring_view{ L"compare" },
            std::wstring_view{ memoryAText },
            std::wstring_view{ memoryBText },
        };
        const auto memoryCompared = LamaPon::Cli::RunAnalysisCommand(
            L"memory",
            memoryCompareArguments);
        Require(
            memoryCompared["comparison"]["changedEntryCount"] == 1
                && memoryCompared["comparison"]["entries"][0]["change"]
                    == "added"
                && memoryCompared["comparison"]["entries"][0]
                    ["deltaBytes"] == 350,
            "memory compare did not report the added model.");

        const std::array missingFile{ std::wstring_view{ L"compare" } };
        const std::array badOption{
            std::wstring_view{ L"analyze" },
            std::wstring_view{ profileAText },
            std::wstring_view{ L"--top" },
            std::wstring_view{ L"-1" },
        };
        const std::array unknownAction{ std::wstring_view{ L"explode" } };
        const std::array brokenFile{
            std::wstring_view{ L"summary" },
            std::wstring_view{ profileAText },
        };
        Require(
            Throws([&] {
                static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                    L"profile", missingFile));
            })
                && Throws([&] {
                    static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                        L"profile", badOption));
                })
                && Throws([&] {
                    static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                        L"memory", unknownAction));
                })
                && Throws([&] {
                    static_cast<void>(LamaPon::Cli::RunAnalysisCommand(
                        L"memory", brokenFile));
                }),
            "Invalid analysis commands must be rejected.");

        std::cout << "CLI analysis command tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
