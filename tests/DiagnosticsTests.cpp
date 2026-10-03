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
    // 条件不成立ならmessageで例外にします(condition: 判定, message: 失敗文)
    void Require(const bool condition, const char* message)
    {
        // 条件が偽ならdiagnostics testを失敗させます。
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }
}

// profiler JSONとcrash診断fileの生成を検証します。
int main()
{
    // 検証例外をprocess failureへ変換します。
    try
    {
        // profiler singletonを取得します。
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

        // frame ringへ保持されたprofile snapshotです。
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

        // test-output内のdiagnostics directoryです。
        const auto outputRoot =
            std::filesystem::current_path()
            / "test-output"
            / "diagnostics";
        // directory削除失敗を受け取るerror codeです。
        std::error_code error;
        std::filesystem::remove_all(outputRoot, error);
        // profile JSONの出力file pathです。
        const auto profilePath =
            outputRoot / "profile.json";
        Require(
            profiler.WriteJson(profilePath),
            "Profiler JSON could not be written.");
        // 出力したprofile JSONを読み込みます。
        std::ifstream profile(profilePath, std::ios::binary);
        // 読み込んだprofile JSON textです。
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

        // crash diagnosticの出力directoryです。
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

        // 診断text fileが見つかったかを記録します。
        bool foundDiagnostic{};
        // crash report directory内のfileを調べます。
        for (const auto& entry :
            std::filesystem::directory_iterator(
                crashDirectory))
        {
            // 通常の.txt fileを診断出力として数えます。
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
        // 検証例外を失敗診断へ変換します(exception: failure)
        catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
