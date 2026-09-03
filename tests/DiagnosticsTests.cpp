#include "LamaPon/Core/CrashReporter.h"
#include "LamaPon/Core/Profiler.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
    void Require(const bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

int main()
{
    try
    {
        auto& profiler = LamaPon::Profiler::Instance();
        profiler.Clear();
        profiler.SetEnabled(true);
        profiler.SetFrameCapacity(2);
        profiler.BeginFrame();
        profiler.Record(
            "Simulation",
            std::chrono::milliseconds(2));
        profiler.Record(
            "Simulation",
            std::chrono::milliseconds(3));
        profiler.EndFrame();

        const auto frames = profiler.Snapshot();
        Require(
            frames.size() == 1
                && frames.front().samples.size() == 1,
            "Profiler did not retain the frame.");
        Require(
            frames.front().samples.front().name
                    == "Simulation"
                && frames.front().samples.front().callCount
                    == 2
                && frames.front().samples.front().
                    milliseconds >= 5.0,
            "Profiler did not aggregate samples.");

        const auto outputRoot =
            std::filesystem::current_path()
            / "test-output"
            / "diagnostics";
        std::error_code error;
        std::filesystem::remove_all(outputRoot, error);
        const auto profilePath =
            outputRoot / "profile.json";
        Require(
            profiler.WriteJson(profilePath),
            "Profiler JSON could not be written.");
        std::ifstream profile(profilePath, std::ios::binary);
        const std::string profileText{
            std::istreambuf_iterator<char>(profile),
            std::istreambuf_iterator<char>()
        };
        Require(
            profileText.find("LamaPonProfile")
                    != std::string::npos
                && profileText.find("Simulation")
                    != std::string::npos,
            "Profiler JSON is incomplete.");

        const auto crashDirectory =
            outputRoot / "crashes";
        LamaPon::CrashReporter::Install(
            crashDirectory,
            "DiagnosticsTests");
        Require(
            LamaPon::CrashReporter::IsInstalled(),
            "Crash reporter was not installed.");
        Require(
            LamaPon::CrashReporter::WriteDiagnostic(
                "diagnostic smoke test"),
            "Crash diagnostic could not be written.");
        LamaPon::CrashReporter::Uninstall();

        bool foundDiagnostic{};
        for (const auto& entry :
            std::filesystem::directory_iterator(
                crashDirectory))
        {
            if (entry.is_regular_file()
                && entry.path().extension() == ".txt")
            {
                foundDiagnostic = true;
                break;
            }
        }
        Require(
            foundDiagnostic,
            "Crash diagnostic file was not created.");

        std::cout << "Diagnostics tests passed.\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
