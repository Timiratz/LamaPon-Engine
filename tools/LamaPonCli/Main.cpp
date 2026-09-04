// LamaPonCli: エディターを開かずにプロジェクトを操作する
// コマンドラインツールです。
//
// なぜ要るか: 生成AIには目も手も無いので、GUIでしかできない操作は
// AIにとって「存在しない機能」になります。このツールは人がエディターで
// やることをテキスト（JSON）とexitコードで返し、AIが自分の変更を
// 自分で確かめられるようにします。
//
// 出力の約束（機械可読の契約）:
//   - stdoutへはJSONオブジェクトを**1つだけ**書きます。
//     進行状況などの人向けの文はすべてstderrへ出します。
//   - exitコードは 0=成功 / 1=失敗。失敗時もJSONは出ます
//     （ok:false と error に理由が入る）。
//   - キーは足すことはあっても消さない方針です。AIの手順が
//     キー名に依存するためです。
//
// 今あるサブコマンド:
//   render  シーンをヘッドレスで1枚撮り、PNGと数値サマリーを返す
//   new     プロジェクトを新規作成する（Hubと同じ雛形）
//   build   C++ Game Moduleをビルドする（エディターの自動ビルドと同じ）
//   export  配布用のゲームを書き出す（エディターと同じ梱包）
//   job     build/render/exportをバックグラウンドで実行する
//   runtime runtime start/status/send/stopで実行中ゲームを操作する
//   inspect Sceneの構造を機械可読なJSONで返す
//   validate Sceneの構造とアセット参照を検証する

#include "LamaPon/LamaPon.h"
#include "LamaPon/Assets/AssetImporter.h"
#include "LamaPon/Editor/GameExporter.h"
#include "LamaPon/Editor/GameModuleBuilder.h"
#include "LamaPon/Scripting/GameModule.h"
#include "LamaPon/Graphics/PngWriter.h"
#include "LamaPon/Hub/LearningJourney.h"
#include "LamaPon/Hub/ProjectHub.h"
#include "RuntimeTiming.h"
#include "BuildDiagnostics.h"
#include "SceneCommands.h"
#include "ComponentSchemas.h"
#include "JsonFiles.h"
#include "ProjectPaths.h"

#include <Windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace LamaPon::Cli;

namespace
{
    // 人向けの進行表示。stdoutはJSON専用なのでこちらはstderrです。
    void Progress(const std::string& message)
    {
        std::cerr << message << std::endl;
    }

    // ---- AI向けバックグラウンドジョブ ----
    //
    // build/render/exportは既存の同期CLIとしても残します。job startは
    // 同じLamaPonCliを専用ワーカープロセスとして起動し、プロセス間の
    // 受け渡しをJSONファイルで行います。これにより、呼び出し側は
    // CLIの終了を待たずにjobIdを受け取り、別プロセスや別のAIターンから
    // status/cancelを呼べます。

    constexpr int JobFileVersion = 1;

    [[nodiscard]] std::string MakeJobId()
    {
        static std::uint64_t sequence{};
        ++sequence;
        return "job-"
            + std::to_string(GetCurrentProcessId())
            + "-"
            + std::to_string(GetTickCount64())
            + "-"
            + std::to_string(sequence);
    }

    [[nodiscard]] bool IsSafeJobId(
        const std::wstring_view value)
    {
        if (value.empty() || value == L"." || value == L"..")
        {
            return false;
        }
        for (const wchar_t character : value)
        {
            if (!(character >= L'a' && character <= L'z')
                && !(character >= L'A' && character <= L'Z')
                && !(character >= L'0' && character <= L'9')
                && character != L'-'
                && character != L'_')
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::filesystem::path JobRoot(
        const std::filesystem::path& projectRoot)
    {
        return projectRoot / L".lamapon" / L"jobs";
    }

    [[nodiscard]] std::filesystem::path JobDirectory(
        const std::filesystem::path& projectRoot,
        const std::wstring_view jobId)
    {
        if (!IsSafeJobId(jobId))
        {
            throw std::invalid_argument(
                "The job id contains invalid characters.");
        }
        return JobRoot(projectRoot) / jobId;
    }

    [[nodiscard]] std::filesystem::path RuntimeRoot(
        const std::filesystem::path& projectRoot)
    {
        return projectRoot / L".lamapon" / L"runtime";
    }

    [[nodiscard]] std::filesystem::path RuntimeDirectory(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId)
    {
        if (!IsSafeJobId(sessionId))
        {
            throw std::invalid_argument(
                "The runtime session id contains invalid characters.");
        }
        return RuntimeRoot(projectRoot)
            / LamaPon::PathFromUtf8(
                LamaPon::WideToUtf8(sessionId));
    }

    [[nodiscard]] std::string MakeRuntimeId()
    {
        static std::uint64_t sequence{};
        ++sequence;
        return "runtime-"
            + std::to_string(GetCurrentProcessId())
            + "-"
            + std::to_string(GetTickCount64())
            + "-"
            + std::to_string(sequence);
    }

    [[nodiscard]] std::string ReadTail(
        const std::filesystem::path& path,
        const std::size_t maximumBytes = 12000)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
        {
            return {};
        }
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end <= 0)
        {
            return {};
        }
        const auto size = static_cast<std::uintmax_t>(end);
        const auto offset = size > maximumBytes
            ? size - maximumBytes
            : 0;
        input.seekg(static_cast<std::streamoff>(offset));
        std::string result(
            static_cast<std::size_t>(size - offset),
            '\0');
        input.read(
            result.data(),
            static_cast<std::streamsize>(result.size()));
        result.resize(
            static_cast<std::size_t>(input.gcount()));
        return result;
    }

    [[nodiscard]] std::wstring QuoteWindowsArgument(
        const std::wstring_view value)
    {
        std::wstring result{ L"\"" };
        std::size_t backslashes{};
        for (const wchar_t character : value)
        {
            if (character == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result += L'\"';
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result += character;
        }
        result.append(backslashes * 2, L'\\');
        result += L'\"';
        return result;
    }

    [[nodiscard]] std::wstring BuildCommandLine(
        const std::vector<std::wstring>& arguments)
    {
        std::wstring result;
        for (const auto& argument : arguments)
        {
            if (!result.empty())
            {
                result += L' ';
            }
            result += QuoteWindowsArgument(argument);
        }
        return result;
    }

    [[nodiscard]] std::filesystem::path JobExecutable()
    {
        const auto executable =
            LamaPon::ExecutableDirectory() / L"LamaPonCli.exe";
        if (!std::filesystem::is_regular_file(executable))
        {
            throw std::runtime_error(
                "LamaPonCli.exe was not found next to the current executable: "
                + LamaPon::PathToUtf8(executable));
        }
        return executable;
    }

    [[nodiscard]] bool IsJobProcessAlive(
        const DWORD processId)
    {
        if (processId == 0)
        {
            return false;
        }
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId);
        if (process == nullptr)
        {
            return false;
        }
        DWORD exitCode = STILL_ACTIVE;
        const bool queried =
            GetExitCodeProcess(process, &exitCode) != FALSE;
        CloseHandle(process);
        return queried && exitCode == STILL_ACTIVE;
    }

    [[nodiscard]] std::filesystem::path FindProjectArgument(
        const std::vector<std::wstring>& arguments)
    {
        for (std::size_t index = 0;
            index + 1 < arguments.size();
            ++index)
        {
            if (arguments[index] == L"--project")
            {
                return arguments[index + 1];
            }
        }
        return {};
    }

    [[nodiscard]] nlohmann::json JobState(
        const std::filesystem::path& jobDirectory)
    {
        return ReadJsonFile(jobDirectory / L"state.json");
    }

    void WriteJobState(
        const std::filesystem::path& jobDirectory,
        const nlohmann::json& state)
    {
        WriteJsonFile(jobDirectory / L"state.json", state);
    }

    [[nodiscard]] nlohmann::json JobReport(
        const char* command,
        nlohmann::json job)
    {
        nlohmann::json report{
            { "ok", true },
            { "command", command },
            { "job", std::move(job) },
        };
        return report;
    }

    [[nodiscard]] int RunJobStart(
        const std::wstring& operation,
        const std::vector<std::wstring>& operationArguments)
    {
        if (operation != L"build"
            && operation != L"render"
            && operation != L"export"
            && operation != L"inspect"
            && operation != L"validate"
            && operation != L"patch"
            && operation != L"test")
        {
            throw std::invalid_argument(
                "job start supports build, render, export, inspect, validate, patch, and test.");
        }
        const auto projectArgument =
            FindProjectArgument(operationArguments);
        if (projectArgument.empty())
        {
            throw std::invalid_argument(
                "job start requires --project for the operation.");
        }
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(projectArgument));
        const auto jobRoot = JobRoot(projectRoot);
        std::error_code directoryError;
        std::filesystem::create_directories(
            jobRoot,
            directoryError);
        if (directoryError)
        {
            throw std::runtime_error(
                "Could not create job root: "
                + directoryError.message());
        }

        const auto jobId = MakeJobId();
        const auto directory = jobRoot / LamaPon::PathFromUtf8(jobId);
        std::filesystem::create_directories(directory);

        nlohmann::json arguments = nlohmann::json::array();
        for (const auto& argument : operationArguments)
        {
            arguments.push_back(
                LamaPon::PathToUtf8(
                    std::filesystem::path(argument)));
        }
        WriteJsonFile(
            directory / L"request.json",
            {
                { "version", JobFileVersion },
                { "operation",
                    LamaPon::PathToUtf8(
                        std::filesystem::path(operation)) },
                { "arguments", std::move(arguments) },
                { "project",
                    LamaPon::PathToUtf8(projectRoot) },
            });

        nlohmann::json state{
            { "version", JobFileVersion },
            { "jobId", jobId },
            { "operation",
                LamaPon::PathToUtf8(
                    std::filesystem::path(operation)) },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "status", "queued" },
            { "progress", 0.0 },
            { "message", "Queued" },
            { "pid", 0 },
            { "jobDirectory", LamaPon::PathToUtf8(directory) },
            { "resultPath",
                LamaPon::PathToUtf8(directory / L"result.json") },
            { "progressLogPath",
                LamaPon::PathToUtf8(directory / L"progress.log") },
        };
        WriteJobState(directory, state);

        const auto executable = JobExecutable();
        std::vector<std::wstring> workerArguments{
            executable.wstring(),
            L"job",
            L"worker",
            L"--job",
            directory.wstring(),
        };
        std::wstring commandLine =
            BuildCommandLine(workerArguments);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                executable.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED,
                nullptr,
                projectRoot.c_str(),
                &startup,
                &process))
        {
            throw std::runtime_error(
                "Could not start the job worker (Win32 error "
                + std::to_string(GetLastError())
                + ").");
        }

        state["pid"] = process.dwProcessId;
        WriteJobState(directory, state);
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            state["status"] = "failed";
            state["message"] = "Could not resume the job worker.";
            state["error"] =
                "Could not resume the job worker.";
            WriteJobState(directory, state);
            throw std::runtime_error(
                "Could not resume the job worker.");
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        state["poll"] =
            "LamaPonCli.exe job status --project \""
            + LamaPon::PathToUtf8(projectRoot)
            + "\" --id "
            + jobId;
        std::cout
            << JobReport("job start", std::move(state)).dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] int RunJobWorker(
        const std::filesystem::path& directory)
    {
        auto state = JobState(directory);
        if (state.value("status", std::string{}) == "cancelled")
        {
            return 0;
        }
        state["status"] = "running";
        state["progress"] = 0.0;
        state["message"] = "Running";
        state["pid"] = GetCurrentProcessId();
        WriteJobState(directory, state);

        const auto request =
            ReadJsonFile(directory / L"request.json");
        const auto projectRoot =
            LamaPon::PathFromUtf8(
                request.at("project").get<std::string>());
        std::vector<std::wstring> arguments;
        for (const auto& value : request.at("arguments"))
        {
            arguments.push_back(
                LamaPon::Utf8ToWide(value.get<std::string>()));
        }

        const auto executable = JobExecutable();
        const auto resultTemporary =
            directory / L"result.json.tmp";
        const auto resultPath =
            directory / L"result.json";
        const auto progressPath =
            directory / L"progress.log";
        std::error_code removeError;
        std::filesystem::remove(resultPath, removeError);
        std::filesystem::remove(resultTemporary, removeError);

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength =
            sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;
        HANDLE output = CreateFileW(
            resultTemporary.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        HANDLE errorOutput = CreateFileW(
            progressPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        HANDLE input = CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (output == INVALID_HANDLE_VALUE
            || errorOutput == INVALID_HANDLE_VALUE
            || input == INVALID_HANDLE_VALUE)
        {
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (errorOutput != INVALID_HANDLE_VALUE) CloseHandle(errorOutput);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            throw std::runtime_error(
                "Could not create job output files.");
        }

        HANDLE childJob = CreateJobObjectW(nullptr, nullptr);
        if (childJob == nullptr)
        {
            CloseHandle(output);
            CloseHandle(errorOutput);
            CloseHandle(input);
            throw std::runtime_error(
                "Could not create the job process group.");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                childJob,
                JobObjectExtendedLimitInformation,
                &limits,
                sizeof(limits)))
        {
            CloseHandle(childJob);
            CloseHandle(output);
            CloseHandle(errorOutput);
            CloseHandle(input);
            throw std::runtime_error(
                "Could not configure the job process group.");
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = output;
        startup.hStdError = errorOutput;
        PROCESS_INFORMATION process{};
        std::wstring commandLine =
            BuildCommandLine(
                [&]
                {
                    std::vector<std::wstring> full{
                        executable.wstring() };
                    full.insert(
                        full.end(),
                        arguments.begin(),
                        arguments.end());
                    return full;
                }());
        const bool started = CreateProcessW(
            executable.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            projectRoot.c_str(),
            &startup,
            &process) != FALSE;
        CloseHandle(output);
        CloseHandle(errorOutput);
        CloseHandle(input);
        if (!started)
        {
            CloseHandle(childJob);
            throw std::runtime_error(
                "Could not start the job command (Win32 error "
                + std::to_string(GetLastError())
                + ").");
        }
        if (!AssignProcessToJobObject(childJob, process.hProcess))
        {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(childJob);
            throw std::runtime_error(
                "Could not attach the job command to its process group.");
        }
        CloseHandle(process.hThread);
        const DWORD waitResult =
            WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        CloseHandle(childJob);
        if (waitResult != WAIT_OBJECT_0)
        {
            throw std::runtime_error(
                "The job command wait failed.");
        }

        if (!MoveFileExW(
                resultTemporary.c_str(),
                resultPath.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            throw std::runtime_error(
                "The job command did not produce a result file.");
        }

        nlohmann::json result;
        try
        {
            result = ReadJsonFile(resultPath);
        }
        catch (const std::exception& exception)
        {
            result = {
                { "ok", false },
                { "error", exception.what() },
            };
        }
        state = JobState(directory);
        if (state.value("status", std::string{}) == "cancelled")
        {
            return 0;
        }
        state["status"] =
            exitCode == 0 && result.value("ok", false)
                ? "succeeded"
                : "failed";
        state["progress"] = 1.0;
        state["message"] =
            state["status"] == "succeeded"
                ? "Completed"
                : "Failed";
        state["exitCode"] = exitCode;
        state["result"] = std::move(result);
        WriteJobState(directory, state);
        return exitCode == 0 ? 0 : 1;
    }

    [[nodiscard]] int RunJobWorkerSafe(
        const std::filesystem::path& directory)
    {
        try
        {
            return RunJobWorker(directory);
        }
        catch (const std::exception& exception)
        {
            try
            {
                auto state = JobState(directory);
                if (state.value("status", std::string{})
                    != "cancelled")
                {
                    state["status"] = "failed";
                    state["progress"] = 1.0;
                    state["message"] = "Failed";
                    state["error"] = exception.what();
                    WriteJobState(directory, state);
                }
            }
            catch (const std::exception& stateException)
            {
                Progress(
                    "Could not record job failure: "
                    + std::string(stateException.what()));
            }
            return 1;
        }
    }

    [[nodiscard]] int RunJobStatus(
        const std::filesystem::path& projectRoot,
        const std::wstring_view jobId)
    {
        const auto directory = JobDirectory(projectRoot, jobId);
        auto state = JobState(directory);
        const auto status =
            state.value("status", std::string{});
        if ((status == "queued" || status == "running")
            && !IsJobProcessAlive(
                state.value("pid", 0u)))
        {
            state["status"] = "failed";
            state["progress"] = 1.0;
            state["message"] =
                "The job worker exited without a result.";
            state["error"] =
                "The job worker exited without a result.";
            WriteJobState(directory, state);
        }
        const auto logTail =
            ReadTail(directory / L"progress.log");
        if (!logTail.empty())
        {
            state["logTail"] = logTail;
        }
        std::cout
            << JobReport("job status", std::move(state)).dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] int RunJobCancel(
        const std::filesystem::path& projectRoot,
        const std::wstring_view jobId)
    {
        const auto directory = JobDirectory(projectRoot, jobId);
        auto state = JobState(directory);
        const auto status =
            state.value("status", std::string{});
        if (status == "queued" || status == "running")
        {
            const DWORD processId =
                state.value("pid", 0u);
            HANDLE process = OpenProcess(
                PROCESS_TERMINATE,
                FALSE,
                processId);
            if (process != nullptr)
            {
                TerminateProcess(process, 2);
                CloseHandle(process);
            }
            state["status"] = "cancelled";
            state["progress"] = 0.0;
            state["message"] = "Cancelled";
            state["cancelled"] = true;
            WriteJobState(directory, state);
        }
        std::cout
            << JobReport("job cancel", std::move(state)).dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] int RunJobList(
        const std::filesystem::path& projectRoot)
    {
        nlohmann::json jobs = nlohmann::json::array();
        const auto root = JobRoot(projectRoot);
        std::error_code iteratorError;
        if (std::filesystem::is_directory(root, iteratorError))
        {
            for (const auto& entry :
                std::filesystem::directory_iterator(root, iteratorError))
            {
                if (iteratorError || !entry.is_directory())
                {
                    continue;
                }
                const auto statePath =
                    entry.path() / L"state.json";
                if (std::filesystem::is_regular_file(statePath))
                {
                    try
                    {
                        jobs.push_back(ReadJsonFile(statePath));
                    }
                    catch (const std::exception&)
                    {
                        // 作成中のジョブは次のlistで読めるため、
                        // ここでは全体を失敗させません。
                    }
                }
            }
        }
        std::cout
            << JobReport(
                "job list",
                nlohmann::json{
                    { "project", LamaPon::PathToUtf8(projectRoot) },
                    { "jobs", std::move(jobs) },
                }).dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    // スワップチェーンを作るためだけの非表示ウィンドウです
    // （描画回帰テストと同じ方式）。画面には何も出ません。
    [[nodiscard]] HWND CreateHiddenWindow(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"LamaPonCliHidden";
        if (RegisterClassExW(&windowClass) == 0)
        {
            throw std::runtime_error(
                "RegisterClassExW failed.");
        }
        const HWND window = CreateWindowExW(
            0,
            windowClass.lpszClassName,
            L"LamaPonCli",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            static_cast<int>(width),
            static_cast<int>(height),
            nullptr,
            nullptr,
            windowClass.hInstance,
            nullptr);
        if (window == nullptr)
        {
            throw std::runtime_error(
                "CreateWindowExW failed.");
        }
        return window;
    }


    // 画像の数値サマリー。AIが「画像を見ずに」壊れ方の当たりを
    // 付けるための値です。
    struct ImageSummary final
    {
        // チャンネルごとの平均（0〜255）。
        double meanRed{};
        double meanGreen{};
        double meanBlue{};
        // 異なるRGB値の数。1なら「一色しか描かれていない」＝
        // ほぼ確実に何も描画されていません。
        std::size_t uniqueColors{};
        // 壊れたShaderの代役（マゼンタ）とみなした画素数。
        // 判定は描画回帰テストと同じ式です（トーンマップ後でも
        // 拾えるよう、純色ではなく傾向で見ます）。
        std::size_t magentaPixels{};
    };

    [[nodiscard]] ImageSummary Summarize(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::vector<std::uint8_t>& pixels)
    {
        ImageSummary summary{};
        std::unordered_set<std::uint32_t> colors;
        const std::size_t count =
            static_cast<std::size_t>(width) * height;
        double totalRed{};
        double totalGreen{};
        double totalBlue{};
        for (std::size_t index = 0; index < count; ++index)
        {
            const std::uint8_t red =
                pixels[index * 4];
            const std::uint8_t green =
                pixels[index * 4 + 1];
            const std::uint8_t blue =
                pixels[index * 4 + 2];
            totalRed += red;
            totalGreen += green;
            totalBlue += blue;
            colors.insert(
                (static_cast<std::uint32_t>(red) << 16)
                | (static_cast<std::uint32_t>(green) << 8)
                | blue);
            if (red > 90
                && blue > 90
                && green + 60 < red
                && green + 60 < blue)
            {
                ++summary.magentaPixels;
            }
        }
        if (count > 0)
        {
            summary.meanRed = totalRed / count;
            summary.meanGreen = totalGreen / count;
            summary.meanBlue = totalBlue / count;
        }
        summary.uniqueColors = colors.size();
        return summary;
    }

    // Loggerの記録をJSONへ写します。infoまで全部入れると
    // 洪水になるので、既定は警告とエラーだけです。
    [[nodiscard]] nlohmann::json CollectLogs(
        std::size_t& errorCount,
        std::size_t& warningCount)
    {
        auto logs = nlohmann::json::array();
        for (const auto& entry :
            LamaPon::Logger::Instance().Snapshot())
        {
            if (entry.level == LamaPon::LogLevel::Info)
            {
                continue;
            }
            if (entry.level == LamaPon::LogLevel::Error)
            {
                ++errorCount;
            }
            else
            {
                ++warningCount;
            }
            logs.push_back({
                { "level",
                    std::string{
                        LamaPon::LogLevelName(
                            entry.level) } },
                { "message", entry.message },
            });
        }
        return logs;
    }

    // シーン内の「壊れているもの」を構造化して集めます。
    //
    // logsの文章と違い、ここは**機械が読む**前提です。magentaPixelsは
    // 「どこかのShaderが壊れた」までしか言えませんが、こちらは
    // どのオブジェクトの・どのShaderが・何のエラーかまで特定します。
    // AIはこの配列が空になるまで直せばよい、という使い方になります。
    //
    // 注意: Shaderのコンパイルは最初の描画時に走るので、この収集は
    // **描画の後**に呼ぶこと。描かれなかったもの（無効化されている、
    // カメラ外でカリングされた等）のエラーは載りません。
    [[nodiscard]] nlohmann::json CollectProblems(
        const LamaPon::Scene& scene)
    {
        auto problems = nlohmann::json::array();
        const auto addShaderError =
            [&problems](
                const std::string& objectName,
                const char* componentName,
                const std::filesystem::path& shaderPath,
                const std::string& error)
            {
                if (error.empty())
                {
                    return;
                }
                problems.push_back({
                    { "object", objectName },
                    { "component", componentName },
                    { "kind", "shader-compile-error" },
                    { "shader",
                        LamaPon::PathToUtf8(shaderPath) },
                    { "detail", error },
                });
            };
        for (const auto& gameObject : scene.GameObjects())
        {
            const auto& name = gameObject->Name();
            if (const auto* renderer =
                    gameObject->GetComponent<
                        LamaPon::MeshRendererComponent>())
            {
                addShaderError(
                    name,
                    "MeshRenderer",
                    renderer->ShaderPath(),
                    renderer->ShaderError());
            }
            if (const auto* renderer =
                    gameObject->GetComponent<
                        LamaPon::ModelRendererComponent>())
            {
                addShaderError(
                    name,
                    "ModelRenderer",
                    renderer->ShaderPath(),
                    renderer->ShaderError());
            }
            if (const auto* renderer =
                    gameObject->GetComponent<
                        LamaPon::SpriteRendererComponent>())
            {
                addShaderError(
                    name,
                    "SpriteRenderer",
                    renderer->ShaderPath(),
                    renderer->ShaderError());
            }
            if (const auto* particles =
                    gameObject->GetComponent<
                        LamaPon::ParticleSystemComponent>())
            {
                addShaderError(
                    name,
                    "ParticleSystem",
                    particles->ShaderPath(),
                    particles->ShaderError());
            }
            if (const auto* script =
                    gameObject->GetComponent<
                        LamaPon::NativeScriptComponent>();
                script && !script->LastError().empty())
            {
                problems.push_back({
                    { "object", name },
                    { "component", "NativeScript" },
                    { "kind", "script-error" },
                    { "script", script->DisplayName() },
                    { "detail", script->LastError() },
                });
            }
            if (const auto* collider =
                    gameObject->GetComponent<
                        LamaPon::MeshCollider3DComponent>();
                collider
                    && !collider->LastError().empty())
            {
                problems.push_back({
                    { "object", name },
                    { "component", "MeshCollider3D" },
                    { "kind", "collider-error" },
                    { "model",
                        LamaPon::PathToUtf8(
                            collider->ModelPath()) },
                    { "detail", collider->LastError() },
                });
            }
        }
        return problems;
    }

    struct RenderOptions final
    {
        std::filesystem::path projectRoot;
        // 空ならプロジェクトの起動シーンを使います。
        std::filesystem::path scene;
        std::filesystem::path outputPng{ "render.png" };
        // 0ならプロジェクト設定の解像度を使います。
        std::uint32_t width{};
        std::uint32_t height{};
        // 描くフレーム数。TAAやSSRのような「前のフレームを材料に
        // する」効果は1枚目では効かないので、既定で数枚temporal
        // ウォームアップします。撮るのは最後の1枚です。
        std::uint32_t frames{ 4 };
        // ゲーム時間を進める秒数（1/60刻みでScene::Updateを回す）。
        // 既定の0では**シーンは置いたまま**です。物理で落ち着かせて
        // から撮りたいときなどに使います。
        double simulateSeconds{};
        // 撮る前に押しておく入力（`--input Jump@0.5:0.2`）。
        // 「押した結果」を撮れないと、ジャンプや攻撃のような操作を
        // CLIから確認できないため、Action名と押す時刻を受け取ります。
        struct InputEvent final
        {
            std::string action;
            double at{};
            double duration{ 0.1 };
            // 正なら正方向のBinding、負なら負方向のBindingを押します。
            // `MoveHorizontal`のような軸のActionは、AとDが同じ名前に
            // まとまっているため、これが無いと**片方向しか試せません**
            // （右へは動かせるが左へは動かせない）。
            double value{ 1.0 };
        };
        std::vector<InputEvent> inputEvents;
        bool warp{};
        bool d3dDebug{};
    };

    // ---- AI向け常駐ランタイムセッション ----
    //
    // EditorのUIを経由せず、LamaPonCli自身がゲームループを持ちます。
    // セッションの入出力はJSONファイルだけなので、別のAIターンや
    // 外部オーケストレーターからも同じ手順で操作できます。

    constexpr int RuntimeFileVersion = 1;

    struct RuntimeStartOptions final
    {
        std::filesystem::path projectRoot;
        std::filesystem::path scene;
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t targetFrameRate{ 60 };
        float fixedDeltaTime{};
        bool warp{};
        bool d3dDebug{};
        bool deterministic{};
        std::uint32_t renderEveryNFrames{ 1 };
        bool paceFrames{ true };
        std::filesystem::path recordPath;
        nlohmann::json replayCommands = nlohmann::json::array();
    };

    struct RuntimeSessionHandle final
    {
        std::filesystem::path projectRoot;
        std::filesystem::path directory;
        std::string sessionId;
        nlohmann::json state;
    };

    [[nodiscard]] nlohmann::json RuntimeValueJson(
        const LamaPon::RuntimeGameState::Value& value)
    {
        nlohmann::json result;
        std::visit(
            [&result](const auto& item)
            {
                result = item;
            },
            value);
        return result;
    }

    [[nodiscard]] nlohmann::json BuildRuntimeSnapshot(
        const LamaPon::Scene& scene,
        const LamaPon::GraphicsDevice& graphics,
        const bool paused)
    {
        const auto& frame = graphics.FrameStats();
        const auto& physics = scene.PhysicsStats();
        const auto& visibility = scene.VisibilityStats();
        nlohmann::json snapshot{
            { "playing", true },
            { "paused", paused },
            { "scene",
                LamaPon::PathToUtf8(
                    scene.Scenes().CurrentScenePath()) },
            { "objectCount", scene.GameObjects().size() },
            // 2Dは1ワールド単位＝1画素なので、位置のアサーションは
            // 画面サイズを知らないと書けません。これが無いと
            // 「720p前提で書いたテストが960pで落ちる」ような、
            // ゲームではなくテスト側の誤りが起きます。
            { "screen", {
                { "width", graphics.UIWidth() },
                { "height", graphics.UIHeight() },
            } },
            { "time", {
                { "deltaTime", LamaPon::Time::DeltaTime() },
                { "unscaledDeltaTime",
                    LamaPon::Time::UnscaledDeltaTime() },
                { "timeSinceStartup",
                    LamaPon::Time::TimeSinceStartup() },
                { "unscaledTimeSinceStartup",
                    LamaPon::Time::UnscaledTimeSinceStartup() },
                { "frameCount", LamaPon::Time::FrameCount() },
                { "timeScale", LamaPon::Time::TimeScale() },
                { "pausedByTimeScale", LamaPon::Time::IsPaused() },
            } },
            { "frame", {
                { "fps", frame.framesPerSecond },
                { "frameTimeMilliseconds",
                    frame.frameTimeMilliseconds },
                { "cpuTimeMilliseconds",
                    frame.cpuTimeMilliseconds },
                { "totalFrames", frame.totalFrames },
                { "shaderFallbackDraws",
                    frame.shaderFallbackDraws },
            } },
            { "physics", {
                { "colliderCount2D", physics.colliderCount2D },
                { "colliderCount3D", physics.colliderCount3D },
                { "candidatePairCount2D",
                    physics.candidatePairCount2D },
                { "candidatePairCount3D",
                    physics.candidatePairCount3D },
                { "narrowPhaseTestCount2D",
                    physics.narrowPhaseTestCount2D },
                { "narrowPhaseTestCount3D",
                    physics.narrowPhaseTestCount3D },
                { "activeContactCount",
                    physics.activeContactCount },
                { "fixedStepsLastFrame",
                    scene.PhysicsFixedStepsLastFrame() },
            } },
            { "visibility", {
                { "rendererCount", visibility.rendererCount },
                { "visibleRendererCount",
                    visibility.visibleRendererCount },
                { "frustumCulledCount",
                    visibility.frustumCulledCount },
                { "occlusionCulledCount",
                    visibility.occlusionCulledCount },
                { "lodCulledCount", visibility.lodCulledCount },
            } },
        };

        auto gameState = nlohmann::json::object();
        auto values = scene.Scenes().State().Snapshot();
        std::ranges::sort(
            values,
            [](const auto& left, const auto& right)
            {
                return left.first < right.first;
            });
        for (const auto& [key, value] : values)
        {
            gameState[key] = RuntimeValueJson(value);
        }
        snapshot["gameState"] = std::move(gameState);

        auto objects = nlohmann::json::array();
        for (const auto& object : scene.GameObjects())
        {
            if (object == nullptr)
            {
                continue;
            }
            const auto& transform = object->GetTransform();
            const auto euler = transform.EulerAngles();
            auto components = nlohmann::json::array();
            for (const auto& component : object->Components())
            {
                if (component == nullptr)
                {
                    continue;
                }
                components.push_back({
                    { "type", std::string(component->TypeName()) },
                    { "enabled", component->IsEnabled() },
                    { "activeAndEnabled",
                        component->IsActiveAndEnabled() },
                });
            }
            objects.push_back({
                { "id", object->Id() },
                { "name", object->Name() },
                { "tag", object->Tag() },
                { "enabled", object->IsEnabled() },
                { "activeInHierarchy",
                    object->IsActiveInHierarchy() },
                { "parentId",
                    object->Parent() == nullptr
                        ? 0
                        : object->Parent()->Id() },
                { "sourceScene", object->SourceScene() },
                { "position", {
                    transform.position.x,
                    transform.position.y,
                    transform.position.z,
                } },
                { "rotationEulerRadians", {
                    euler.x,
                    euler.y,
                    euler.z,
                } },
                { "rotationQuaternion", {
                    transform.rotationQuaternion.x,
                    transform.rotationQuaternion.y,
                    transform.rotationQuaternion.z,
                    transform.rotationQuaternion.w,
                } },
                { "scale", {
                    transform.scale.x,
                    transform.scale.y,
                    transform.scale.z,
                } },
                { "components", std::move(components) },
            });
        }
        snapshot["objects"] = std::move(objects);

        auto actions = nlohmann::json::array();
        for (const auto& action : graphics.Input().Actions())
        {
            auto bindings = nlohmann::json::array();
            for (const auto& binding : action.bindings)
            {
                bindings.push_back({
                    { "control",
                        std::string(
                            LamaPon::InputControlName(
                                binding.control)) },
                    { "scale", binding.scale },
                });
            }
            actions.push_back({
                { "name", action.name },
                { "bindings", std::move(bindings) },
                { "value", graphics.Input().Value(action.name) },
                { "down", graphics.Input().IsDown(action.name) },
                { "pressed",
                    graphics.Input().WasPressed(action.name) },
                { "released",
                    graphics.Input().WasReleased(action.name) },
            });
        }
        snapshot["input"] = std::move(actions);

        const auto profileFrames =
            LamaPon::Profiler::Instance().Snapshot();
        auto profile = nlohmann::json{
            { "enabled",
                LamaPon::Profiler::Instance().IsEnabled() },
            { "frameIndex", 0 },
            { "milliseconds", 0.0 },
            { "samples", nlohmann::json::array() },
        };
        if (!profileFrames.empty())
        {
            const auto& latest = profileFrames.back();
            profile["frameIndex"] = latest.index;
            profile["milliseconds"] = latest.milliseconds;
            for (const auto& sample : latest.samples)
            {
                profile["samples"].push_back({
                    { "name", sample.name },
                    { "milliseconds", sample.milliseconds },
                    { "calls", sample.callCount },
                });
            }
        }
        snapshot["profiler"] = std::move(profile);

        auto logs = nlohmann::json::array();
        const auto logEntries = LamaPon::Logger::Instance().Snapshot();
        const auto firstLog = logEntries.size() > 64
            ? logEntries.end() - 64
            : logEntries.begin();
        for (auto iterator = firstLog;
            iterator != logEntries.end();
            ++iterator)
        {
            if (iterator->level == LamaPon::LogLevel::Info)
            {
                continue;
            }
            logs.push_back({
                { "sequence", iterator->sequence },
                { "level",
                    std::string(
                        LamaPon::LogLevelName(iterator->level)) },
                { "message", iterator->message },
                { "gameObjectId", iterator->gameObjectId },
            });
        }
        snapshot["logs"] = std::move(logs);
        return snapshot;
    }

    [[nodiscard]] nlohmann::json RuntimeState(
        const std::filesystem::path& directory)
    {
        const auto path = directory / L"state.json";
        std::string lastError;
        // MoveFileEx is atomic on a local NTFS volume, but WebDAV providers
        // can briefly expose neither the old nor the new name while replacing
        // a file. Runtime state is polled concurrently, so that transient
        // provider window must not fail a command or an entire test.
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            try
            {
                return ReadJsonFile(path);
            }
            catch (const std::exception& exception)
            {
                lastError = exception.what();
                if (attempt + 1 < 50)
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                }
            }
        }
        throw std::runtime_error(
            "Could not read stable runtime state after retrying: "
            + lastError);
    }

    void WriteRuntimeState(
        const std::filesystem::path& directory,
        const nlohmann::json& state)
    {
        WriteJsonFile(directory / L"state.json", state);
    }

    [[nodiscard]] std::string RuntimeCommandName(
        const nlohmann::json& command)
    {
        if (command.contains("op") && command.at("op").is_string())
        {
            return command.at("op").get<std::string>();
        }
        return command.value("type", std::string{});
    }

    [[nodiscard]] std::string TrimRuntimeToken(
        std::string value)
    {
        const auto first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return {};
        }
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
        if (value.size() >= 2
            && ((value.front() == '"' && value.back() == '"')
                || (value.front() == '\'' && value.back() == '\'')))
        {
            return value.substr(1, value.size() - 2);
        }
        return value;
    }

    [[nodiscard]] nlohmann::json RuntimeCommandValue(
        const std::string& token)
    {
        const auto value = TrimRuntimeToken(token);
        if (value == "true")
        {
            return true;
        }
        if (value == "false")
        {
            return false;
        }
        if (value == "null")
        {
            return nullptr;
        }
        try
        {
            std::size_t parsedLength{};
            const auto integer = std::stoll(value, &parsedLength);
            if (parsedLength == value.size())
            {
                return integer;
            }
        }
        catch (const std::exception&)
        {
        }
        try
        {
            std::size_t parsedLength{};
            const auto number = std::stod(value, &parsedLength);
            if (parsedLength == value.size())
            {
                return number;
            }
        }
        catch (const std::exception&)
        {
        }
        return value;
    }

    [[nodiscard]] nlohmann::json ParseRuntimeCommandText(
        const std::wstring_view text)
    {
        const auto source = LamaPon::WideToUtf8(text);
        const auto trimmed = TrimRuntimeToken(source);
        if (trimmed == "pause"
            || trimmed == "resume"
            || trimmed == "step"
            || trimmed == "observe"
            || trimmed == "stop")
        {
            return { { "op", trimmed } };
        }

        // PowerShell can remove the inner quotes when a JSON object is passed
        // to a native process. Accept the resulting compact form as a small,
        // predictable command DSL so agents can use --command directly.
        if (trimmed.size() >= 2
            && trimmed.front() == '{'
            && trimmed.back() == '}'
            && trimmed.find('"') == std::string::npos)
        {
            nlohmann::json result = nlohmann::json::object();
            const auto body = trimmed.substr(
                1,
                trimmed.size() - 2);
            std::size_t start{};
            while (start <= body.size())
            {
                const auto separator = body.find(',', start);
                const auto part = body.substr(
                    start,
                    separator == std::string::npos
                        ? std::string::npos
                        : separator - start);
                const auto colon = part.find(':');
                if (colon == std::string::npos)
                {
                    throw std::invalid_argument(
                        "runtime --command contains a field without ':'.");
                }
                const auto key = TrimRuntimeToken(
                    part.substr(0, colon));
                if (key.empty())
                {
                    throw std::invalid_argument(
                        "runtime --command contains an empty field name.");
                }
                result[key] = RuntimeCommandValue(
                    part.substr(colon + 1));
                if (separator == std::string::npos)
                {
                    break;
                }
                start = separator + 1;
            }
            return result;
        }

        try
        {
            return nlohmann::json::parse(source);
        }
        catch (const std::exception&)
        {
            throw std::invalid_argument(
                "runtime --command must be valid JSON (or a compact "
                "operation such as {op:pause}).");
        }
    }

    [[nodiscard]] std::uint32_t RuntimeInputFrames(
        const nlohmann::json& command)
    {
        if (!command.contains("frames"))
        {
            return 1;
        }
        const auto& value = command.at("frames");
        if (!value.is_number_integer()
            || value.get<std::int64_t>() < 1)
        {
            throw std::invalid_argument(
                "input frames must be a positive integer.");
        }
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            value.get<std::int64_t>(),
            1,
            600));
    }

    void ApplyRuntimeInput(
        const nlohmann::json& command,
        const LamaPon::InputSystem& inputSystem,
        LamaPon::InputSnapshot& snapshot,
        std::uint32_t& frames,
        std::string& error)
    {
        if (!command.contains("value")
            || !command.at("value").is_number())
        {
            error = "input requires a numeric value.";
            return;
        }
        const float value = command.at("value").get<float>();
        if (!std::isfinite(value))
        {
            error = "input value must be finite.";
            return;
        }

        LamaPon::InputControl control{};
        float controlValue = value;
        bool resolved = false;
        if (command.contains("control")
            && command.at("control").is_string())
        {
            try
            {
                control = LamaPon::InputControlFromName(
                    command.at("control").get<std::string>());
                resolved = true;
            }
            catch (const std::exception&)
            {
                error = "unknown input control: "
                    + command.at("control").get<std::string>();
            }
        }
        else if (command.contains("action")
            && command.at("action").is_string())
        {
            const auto actionName =
                command.at("action").get<std::string>();
            const auto action = std::find_if(
                inputSystem.Actions().begin(),
                inputSystem.Actions().end(),
                [&actionName](const auto& candidate)
                {
                    return candidate.name == actionName;
                });
            if (action == inputSystem.Actions().end())
            {
                error = "unknown input action: " + actionName;
            }
            else
            {
                const auto binding = std::find_if(
                    action->bindings.begin(),
                    action->bindings.end(),
                    [](const auto& candidate)
                    {
                        return std::abs(candidate.scale)
                            > 1.0e-6f;
                    });
                if (binding == action->bindings.end())
                {
                    error =
                        "input action has no usable binding: "
                        + actionName;
                }
                else
                {
                    control = binding->control;
                    controlValue = std::clamp(
                        value / binding->scale,
                        -1.0f,
                        1.0f);
                    resolved = true;
                }
            }
        }
        else
        {
            error = "input requires control or action.";
        }
        if (!resolved)
        {
            return;
        }

        snapshot.Set(
            control,
            std::clamp(controlValue, -1.0f, 1.0f));
        frames = std::max(
            frames,
            RuntimeInputFrames(command));
    }

    [[nodiscard]] std::filesystem::path RuntimeScreenshotPath(
        const std::filesystem::path& directory,
        const nlohmann::json& command,
        const std::uint64_t sequence)
    {
        auto path = LamaPon::PathFromUtf8(
            command.value(
                "path",
                std::string{
                    "screenshot-"
                    + std::to_string(sequence)
                    + ".png" }));
        if (path.empty() || path.is_absolute())
        {
            throw std::invalid_argument(
                "runtime screenshot path must be relative.");
        }
        path = path.lexically_normal();
        if (path.native().starts_with(L".."))
        {
            throw std::invalid_argument(
                "runtime screenshot path cannot leave the session folder.");
        }
        return directory / path;
    }

    [[nodiscard]] LamaPon::GameObject* FindRuntimeObject(
        LamaPon::Scene& scene,
        const nlohmann::json& command,
        const bool allowAll = false)
    {
        const auto& selector = command.contains("selector")
            ? command.at("selector")
            : command;
        if (selector.contains("id")
            && selector.at("id").is_number_integer())
        {
            const auto id = selector.at("id").get<std::int64_t>();
            if (id < 0)
            {
                throw std::invalid_argument(
                    "runtime object id must not be negative.");
            }
            return scene.FindGameObject(
                static_cast<LamaPon::GameObjectId>(id));
        }
        if (selector.contains("name")
            && selector.at("name").is_string())
        {
            return scene.FindGameObjectByName(
                selector.at("name").get<std::string>());
        }
        if (selector.contains("tag")
            && selector.at("tag").is_string())
        {
            return scene.FindGameObjectByTag(
                selector.at("tag").get<std::string>());
        }
        if (allowAll)
        {
            return nullptr;
        }
        throw std::invalid_argument(
            "runtime object commands require selector.id, selector.name, or selector.tag.");
    }

    void ReadRuntimeVector(
        const nlohmann::json& value,
        float* destination,
        const std::size_t count,
        const char* name)
    {
        if (!value.is_array() || value.size() != count)
        {
            throw std::invalid_argument(
                std::string{ name }
                + " must be an array of "
                + std::to_string(count)
                + " numbers.");
        }
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!value.at(index).is_number())
            {
                throw std::invalid_argument(
                    std::string{ name } + " must contain only numbers.");
            }
            const auto number = value.at(index).get<float>();
            if (!std::isfinite(number))
            {
                throw std::invalid_argument(
                    std::string{ name } + " must contain finite numbers.");
            }
            destination[index] = number;
        }
    }

    [[nodiscard]] nlohmann::json RuntimeObjectSnapshot(
        const nlohmann::json& snapshot,
        LamaPon::GameObject* object)
    {
        if (object == nullptr)
        {
            return nullptr;
        }
        for (const auto& candidate : snapshot.at("objects"))
        {
            if (candidate.value<std::uint64_t>("id", 0)
                == object->Id())
            {
                return candidate;
            }
        }
        return nullptr;
    }

    void ApplyRuntimeObjectCommand(
        const std::string& operation,
        const nlohmann::json& command,
        LamaPon::Scene& scene,
        LamaPon::GraphicsDevice& graphics,
        LamaPon::GameModuleHost& gameModule,
        nlohmann::json& state)
    {
        if (operation == "query")
        {
            const auto snapshot = BuildRuntimeSnapshot(
                scene,
                graphics,
                false);
            auto* object = FindRuntimeObject(
                scene,
                command,
                true);
            state["lastQuery"] = object == nullptr
                ? snapshot.at("objects")
                : RuntimeObjectSnapshot(snapshot, object);
            return;
        }

        if (operation == "set-transform"
            || operation == "set-object")
        {
            auto* object = FindRuntimeObject(scene, command);
            if (object == nullptr)
            {
                throw std::invalid_argument(
                    "runtime object selector did not match an object.");
            }
            auto& transform = object->GetTransform();
            if (command.contains("position"))
            {
                ReadRuntimeVector(
                    command.at("position"),
                    &transform.position.x,
                    3,
                    "position");
            }
            if (command.contains("scale"))
            {
                ReadRuntimeVector(
                    command.at("scale"),
                    &transform.scale.x,
                    3,
                    "scale");
            }
            if (command.contains("rotationEulerRadians"))
            {
                DirectX::XMFLOAT3 euler{};
                ReadRuntimeVector(
                    command.at("rotationEulerRadians"),
                    &euler.x,
                    3,
                    "rotationEulerRadians");
                transform.SetEulerAngles(euler);
            }
            if (command.contains("rotationQuaternion"))
            {
                ReadRuntimeVector(
                    command.at("rotationQuaternion"),
                    &transform.rotationQuaternion.x,
                    4,
                    "rotationQuaternion");
                transform.SetRotationVector(
                    transform.RotationVector());
            }
            if (command.contains("name"))
            {
                object->SetName(command.at("name").get<std::string>());
            }
            if (command.contains("tag"))
            {
                object->SetTag(command.at("tag").get<std::string>());
            }
            if (command.contains("enabled"))
            {
                object->SetEnabled(command.at("enabled").get<bool>());
            }
            state["lastObject"] = object->Id();
            return;
        }

        if (operation == "set-state")
        {
            if (!command.contains("key")
                || !command.at("key").is_string()
                || !command.contains("value"))
            {
                throw std::invalid_argument(
                    "set-state requires string key and value.");
            }
            const auto key = command.at("key").get<std::string>();
            const auto& value = command.at("value");
            auto& runtimeState = scene.Scenes().State();
            if (value.is_boolean())
            {
                runtimeState.SetBoolean(key, value.get<bool>());
            }
            else if (value.is_number_integer())
            {
                runtimeState.SetInteger(
                    key,
                    value.get<std::int64_t>());
            }
            else if (value.is_number())
            {
                runtimeState.SetNumber(key, value.get<double>());
            }
            else if (value.is_string())
            {
                runtimeState.SetString(
                    key,
                    value.get<std::string>());
            }
            else if (value.is_null())
            {
                runtimeState.Remove(key);
            }
            else
            {
                throw std::invalid_argument(
                    "set-state value must be a scalar.");
            }
            return;
        }

        if (operation == "reload")
        {
            const auto requested = command.value(
                "scene",
                LamaPon::PathToUtf8(
                    scene.Scenes().CurrentScenePath()));
            if (!scene.Scenes().RequestLoad(
                    LamaPon::PathFromUtf8(requested))
                || !scene.Scenes().ProcessPending())
            {
                throw std::runtime_error(
                    scene.Scenes().LastError().empty()
                        ? "runtime scene reload failed."
                        : scene.Scenes().LastError());
            }
            return;
        }

        if (operation == "reload-module")
        {
            if (!gameModule.Reload())
            {
                throw std::runtime_error(
                    gameModule.LastError().empty()
                        ? "runtime Game Module reload failed."
                        : gameModule.LastError());
            }
            state["gameModuleReloaded"] = true;
        }
    }

    [[nodiscard]] nlohmann::json RuntimeFailureState(
        const std::filesystem::path& directory,
        const std::exception& exception)
    {
        nlohmann::json state{
            { "version", RuntimeFileVersion },
            { "status", "failed" },
            { "progress", 1.0 },
            { "message", "Failed" },
            { "error", exception.what() },
            { "sessionDirectory",
                LamaPon::PathToUtf8(directory) },
        };
        return state;
    }

    [[nodiscard]] int RunRuntimeWorker(
        const std::filesystem::path& directory)
    {
        const auto request =
            ReadJsonFile(directory / L"request.json");
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    LamaPon::PathFromUtf8(
                        request.at("project")
                            .get<std::string>())));
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        const auto settings =
            LamaPon::LoadProjectSettings(settingsPath);
        const auto scenePath = NormalizeScenePath(
            projectRoot,
            LamaPon::PathFromUtf8(
                request.value("scene", std::string{}).empty()
                    ? LamaPon::PathToUtf8(settings.startupScene)
                    : request.at("scene").get<std::string>()));
        const auto width = std::max<std::uint32_t>(
            request.value("width", settings.windowWidth),
            1u);
        const auto height = std::max<std::uint32_t>(
            request.value("height", settings.windowHeight),
            1u);
        const auto targetFrameRate = std::clamp<std::uint32_t>(
            request.value("targetFrameRate", 60u),
            1u,
            240u);
        const bool deterministic = request.value(
            "deterministic",
            false);
        const float fixedDeltaTime = std::clamp(
            request.value(
                "fixedDeltaTime",
                1.0f / static_cast<float>(targetFrameRate)),
            0.0001f,
            0.1f);
        const auto renderEveryNFrames = std::clamp<std::uint32_t>(
            request.value("renderEveryNFrames", 1u),
            1u,
            100'000u);
        const bool paceFrames = request.value("paceFrames", true);
        if (!paceFrames && !deterministic)
        {
            throw std::invalid_argument(
                "Unpaced runtime execution requires deterministic mode.");
        }
        const auto recordPath = request.value(
            "recordPath",
            std::string{}).empty()
            ? std::filesystem::path{}
            : LamaPon::PathFromUtf8(
                request.at("recordPath").get<std::string>());
        const auto replayCommands = request.value(
            "replayCommands",
            nlohmann::json::array());
        if (!replayCommands.is_array())
        {
            throw std::invalid_argument(
                "runtime replayCommands must be an array.");
        }

        auto state = RuntimeState(directory);
        state["status"] = "running";
        state["message"] = "Running";
        state["pid"] = GetCurrentProcessId();
        state["scene"] = LamaPon::PathToUtf8(scenePath);
        state["deterministic"] = deterministic;
        state["fixedDeltaTime"] = fixedDeltaTime;
        state["renderEveryNFrames"] = renderEveryNFrames;
        state["paceFrames"] = paceFrames;
        state["replayCommandCount"] = replayCommands.size();
        WriteRuntimeState(directory, state);

        LamaPon::GraphicsDevice::SetPreferWarpAdapter(
            request.value("warp", false));
        LamaPon::GraphicsDevice::SetEnableDebugLayer(
            request.value("d3dDebug", false));
        const HWND window = CreateHiddenWindow(width, height);
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(window, width, height);
        graphics.Assets().SetAssetRoot(projectRoot / L"assets");
        auto graphicsSettings = settings.graphics;
        graphicsSettings.vSyncEnabled = false;
        graphics.SetGraphicsSettings(graphicsSettings);
        graphics.SetAsyncShaderCompilationEnabled(false);
        graphics.Input().SetActions(settings.inputActions);
        LamaPon::SetActivePhysicsSettings(settings.physics);
        static_cast<void>(
            LamaPon::Logger::Instance().SetFilePath(
                directory / L"runtime.log"));

        LamaPon::GameModuleHost gameModule;
        const auto gameModulePath = projectRoot
            / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        bool gameModuleLoaded = false;
        std::string gameModuleError;
        if (std::filesystem::is_regular_file(gameModulePath))
        {
            gameModuleLoaded = gameModule.Load(gameModulePath);
            if (!gameModuleLoaded)
            {
                gameModuleError = gameModule.LastError();
                LamaPon::Logger::Instance().Warning(
                    "Game Moduleを読み込めませんでした: "
                    + gameModuleError);
            }
        }
        else
        {
            gameModuleError =
                "Game Moduleがありません（先にbuildしてください）。";
        }

        LamaPon::Scene scene(graphics);
        scene.SetRegisteredTags(settings.tags);
        if (!scene.Scenes().RequestLoad(scenePath)
            || !scene.Scenes().ProcessPending())
        {
            throw std::runtime_error(
                scene.Scenes().LastError().empty()
                    ? "The runtime scene did not finish loading."
                    : scene.Scenes().LastError());
        }

        state["gameModule"] = {
            { "loaded", gameModuleLoaded },
            { "path", LamaPon::PathToUtf8(gameModulePath) },
            { "reason", gameModuleError },
        };
        state["runtime"] = BuildRuntimeSnapshot(
            scene,
            graphics,
            false);
        WriteRuntimeState(directory, state);

        const auto controlPath = directory / L"control.json";
        std::uint64_t lastCommandSequence{};
        bool paused{};
        bool stepRequested{};
        bool stopRequested{};
        std::uint32_t inputFrames{};
        LamaPon::InputSnapshot inputSnapshot;
        std::filesystem::path screenshotPath;
        std::uint64_t frameNumber{};
        std::size_t replayIndex{};
        nlohmann::json replayDocument{
            { "version", RuntimeFileVersion },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "scene", LamaPon::PathToUtf8(scenePath) },
            { "targetFrameRate", targetFrameRate },
            { "fixedDeltaTime", fixedDeltaTime },
            { "deterministic", deterministic },
            { "renderEveryNFrames", renderEveryNFrames },
            { "paceFrames", paceFrames },
            { "commands", nlohmann::json::array() },
        };
        const auto writeReplay =
            [&recordPath, &replayDocument]
            {
                if (!recordPath.empty())
                {
                    WriteJsonFile(recordPath, replayDocument);
                }
            };
        writeReplay();
        auto previousUpdate = std::chrono::steady_clock::now();
        auto previousPresentation = previousUpdate;
        auto lastStateWrite = previousUpdate;

        const auto writeState =
            [&state, &directory, &lastStateWrite]
            {
                WriteRuntimeState(directory, state);
                lastStateWrite = std::chrono::steady_clock::now();
            };

        while (!stopRequested)
        {
            bool commandChanged = false;
            bool forceRender = false;
            if (replayIndex < replayCommands.size()
                && replayCommands.at(replayIndex).is_object()
                && replayCommands.at(replayIndex).value<std::uint64_t>(
                    "frame",
                    0) <= frameNumber)
            {
                auto replayCommand = replayCommands.at(replayIndex++)
                    .value(
                        "command",
                        nlohmann::json::object());
                replayCommand["seq"] = lastCommandSequence + 1;
                WriteJsonFile(controlPath, replayCommand);
            }
            if (std::filesystem::is_regular_file(controlPath))
            {
                try
                {
                    const auto command = ReadJsonFile(controlPath);
                    const auto sequence =
                        command.value<std::uint64_t>("seq", 0);
                    if (sequence != 0
                        && sequence > lastCommandSequence)
                    {
                        lastCommandSequence = sequence;
                        commandChanged = true;
                        state["lastCommandSeq"] = sequence;
                        state["lastCommand"] =
                            RuntimeCommandName(command);
                        state["lastCommandOk"] = true;
                        state.erase("lastCommandError");
                        const auto operation =
                            RuntimeCommandName(command);
                        if (operation == "pause")
                        {
                            paused = true;
                        }
                        else if (operation == "resume")
                        {
                            paused = false;
                        }
                        else if (operation == "step")
                        {
                            if (!paused)
                            {
                                throw std::invalid_argument(
                                    "step requires a paused runtime.");
                            }
                            stepRequested = true;
                        }
                        else if (operation == "input")
                        {
                            std::string inputError;
                            ApplyRuntimeInput(
                                command,
                                graphics.Input(),
                                inputSnapshot,
                                inputFrames,
                                inputError);
                            if (!inputError.empty())
                            {
                                state["lastCommandOk"] = false;
                                state["lastCommandError"] = inputError;
                            }
                        }
                        else if (operation == "timescale")
                        {
                            if (!command.contains("value")
                                || !command.at("value").is_number())
                            {
                                throw std::invalid_argument(
                                    "timescale requires a numeric value.");
                            }
                            const float value =
                                command.at("value").get<float>();
                            if (!std::isfinite(value))
                            {
                                throw std::invalid_argument(
                                    "timescale must be finite.");
                            }
                            LamaPon::Time::SetTimeScale(value);
                        }
                        else if (operation == "screenshot")
                        {
                            screenshotPath = RuntimeScreenshotPath(
                                directory,
                                command,
                                sequence);
                            forceRender = true;
                        }
                        else if (operation == "observe")
                        {
                            // 描画を間引く高速テストでも、observeは現在の
                            // 表示状態まで確定してから応答します。
                            forceRender = true;
                        }
                        else if (operation == "stop")
                        {
                            stopRequested = true;
                        }
                        else if (operation == "query"
                            || operation == "set-transform"
                            || operation == "set-object"
                            || operation == "set-state"
                            || operation == "reload"
                            || operation == "reload-module")
                        {
                            ApplyRuntimeObjectCommand(
                                operation,
                                command,
                                scene,
                                graphics,
                                gameModule,
                                state);
                        }
                        else
                        {
                            throw std::invalid_argument(
                                "unknown runtime operation: "
                                + operation);
                        }
                        if (!recordPath.empty())
                        {
                            replayDocument["commands"].push_back({
                                { "frame", frameNumber },
                                { "command", command },
                            });
                            writeReplay();
                        }
                    }
                }
                catch (const std::exception& exception)
                {
                    state["lastCommandOk"] = false;
                    state["lastCommandError"] = exception.what();
                    commandChanged = true;
                }
            }

            if (stopRequested)
            {
                state["status"] = "stopped";
                state["message"] = "Stopped";
                state["replayIndex"] = replayIndex;
                state["replayComplete"] =
                    replayIndex >= replayCommands.size();
                writeState();
                break;
            }

            const auto current = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<float>(
                current - previousUpdate).count();
            previousUpdate = current;
            const auto timing = LamaPon::Cli::MakeRuntimeFrameTiming(
                elapsed,
                deterministic,
                fixedDeltaTime);
            LamaPon::Time::Detail::AdvanceFrame(
                timing.simulationDeltaSeconds);
            LamaPon::Profiler::Instance().BeginFrame();

            if (inputFrames > 0)
            {
                graphics.Input().UpdateFromSnapshot(inputSnapshot);
                --inputFrames;
                if (inputFrames == 0)
                {
                    // UpdateFromSnapshot has already consumed this final
                    // frame. Clear the retained controls as well so a later
                    // input command cannot resurrect a key/button from an
                    // earlier command when it adds its own control.
                    inputSnapshot.values.clear();
                }
            }
            else
            {
                graphics.Input().UpdateFromSnapshot({});
            }

            const bool simulate = !paused || stepRequested;
            stepRequested = false;
            if (simulate)
            {
                scene.Update(LamaPon::Time::DeltaTime());
            }
            // DLL更新の監視はゲーム内時間ではなく実時間です。高速な
            // 決定論テストで固定時間を足すと、毎秒何百回も監視します。
            gameModule.PollHotReload(timing.wallDeltaSeconds);

            const bool renderFrame =
                LamaPon::Cli::ShouldRenderRuntimeFrame(
                    frameNumber,
                    renderEveryNFrames,
                    forceRender);

            std::vector<std::uint8_t> pixels;
            std::uint32_t capturedWidth{};
            std::uint32_t capturedHeight{};
            if (renderFrame)
            {
                const float clearColor[4]{
                    0.025f, 0.035f, 0.055f, 1.0f };
                graphics.BeginFrame(clearColor);
                try
                {
                    graphics.BeginSceneComposition(clearColor);
                    scene.RenderMainCamera(
                        graphics.AspectRatio(),
                        false,
                        graphics.SceneCompositionTarget());
                    graphics.EndSceneComposition(
                        scene.PostProcessFrameData());
                    scene.Render2D();
                }
                catch (const std::exception& exception)
                {
                    LamaPon::Logger::Instance().Error(
                        std::string{ "Runtime render failed: " }
                        + exception.what());
                }

                if (!screenshotPath.empty())
                {
                    pixels = graphics.CaptureBackBuffer(
                        capturedWidth,
                        capturedHeight);
                }
                graphics.EndFrame();
                const auto presentation =
                    std::chrono::steady_clock::now();
                const auto presentationDelta =
                    std::chrono::duration<float>(
                        presentation - previousPresentation).count();
                previousPresentation = presentation;
                graphics.RecordFrameStatistics(
                    presentationDelta,
                    std::chrono::duration<float, std::milli>(
                        presentation - current).count());
            }
            LamaPon::Profiler::Instance().EndFrame();
            ++frameNumber;

            if (!screenshotPath.empty())
            {
                std::error_code screenshotDirectoryError;
                std::filesystem::create_directories(
                    screenshotPath.parent_path(),
                    screenshotDirectoryError);
                if (screenshotDirectoryError)
                {
                    throw std::runtime_error(
                        "Could not create runtime screenshot folder: "
                        + screenshotDirectoryError.message());
                }
                LamaPon::SavePng(
                    screenshotPath,
                    capturedWidth,
                    capturedHeight,
                    pixels);
                state["screenshot"] =
                    LamaPon::PathToUtf8(screenshotPath);
                screenshotPath.clear();
                commandChanged = true;
            }

            state["status"] = paused ? "paused" : "running";
            state["frame"] = frameNumber;
            state["replayIndex"] = replayIndex;
            state["replayComplete"] =
                replayIndex >= replayCommands.size();
            const bool stateWriteDue = frameNumber == 1u
                || commandChanged
                || std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()
                        - lastStateWrite).count() >= 250;
            if (stateWriteDue)
            {
                // 大きなSceneのJSON化は描画より高価な場合があります。
                // 外へ状態を書かないフレームでは組み立ても省きます。
                state["runtime"] = BuildRuntimeSnapshot(
                    scene,
                    graphics,
                    paused);
                writeState();
            }

            if (paceFrames)
            {
                const auto frameDuration =
                    std::chrono::duration<double>(
                        1.0 / static_cast<double>(targetFrameRate));
                const auto frameDeadline = current
                    + std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                            frameDuration);
                if (std::chrono::steady_clock::now() < frameDeadline)
                {
                    std::this_thread::sleep_until(frameDeadline);
                }
            }
            else if (paused)
            {
                // pause中はゲーム状態が進まないため、命令待ちでCPUを
                // 占有しない程度だけ譲ります。
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            }
        }

        DestroyWindow(window);
        return 0;
    }

    [[nodiscard]] int RunRuntimeWorkerSafe(
        const std::filesystem::path& directory)
    {
        try
        {
            return RunRuntimeWorker(directory);
        }
        catch (const std::exception& exception)
        {
            auto state = RuntimeFailureState(directory, exception);
            try
            {
                WriteRuntimeState(directory, state);
            }
            catch (const std::exception& stateException)
            {
                Progress(
                    "Could not record runtime failure: "
                    + std::string(stateException.what()));
            }
            return 1;
        }
    }

    [[nodiscard]] nlohmann::json RuntimeReport(
        const char* command,
        nlohmann::json session)
    {
        return {
            { "ok", true },
            { "command", command },
            { "session", std::move(session) },
        };
    }

    [[nodiscard]] RuntimeSessionHandle StartRuntimeSession(
        const RuntimeStartOptions& options)
    {
        if (!options.paceFrames && !options.deterministic)
        {
            throw std::invalid_argument(
                "--no-pace (or paceFrames:false) requires"
                " deterministic mode.");
        }
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(options.projectRoot));
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        const auto settings =
            LamaPon::LoadProjectSettings(settingsPath);
        const auto root = RuntimeRoot(projectRoot);
        std::error_code directoryError;
        std::filesystem::create_directories(root, directoryError);
        if (directoryError)
        {
            throw std::runtime_error(
                "Could not create runtime root: "
                + directoryError.message());
        }

        const auto sessionId = MakeRuntimeId();
        const auto directory =
            root / LamaPon::PathFromUtf8(sessionId);
        std::filesystem::create_directories(directory);
        const auto scene = options.scene.empty()
            ? settings.startupScene
            : options.scene;
        WriteJsonFile(
            directory / L"request.json",
            {
                { "version", RuntimeFileVersion },
                { "sessionId", sessionId },
                { "project", LamaPon::PathToUtf8(projectRoot) },
                { "scene", LamaPon::PathToUtf8(scene) },
                { "width",
                    options.width != 0
                        ? options.width
                        : settings.windowWidth },
                { "height",
                    options.height != 0
                        ? options.height
                        : settings.windowHeight },
                { "targetFrameRate", options.targetFrameRate },
                { "fixedDeltaTime",
                    options.fixedDeltaTime != 0.0f
                        ? options.fixedDeltaTime
                        : 1.0f / static_cast<float>(
                            options.targetFrameRate) },
                { "warp", options.warp },
                { "d3dDebug", options.d3dDebug },
                { "deterministic", options.deterministic },
                { "renderEveryNFrames",
                    options.renderEveryNFrames },
                { "paceFrames", options.paceFrames },
                { "recordPath",
                    options.recordPath.empty()
                        ? std::string{}
                        : LamaPon::PathToUtf8(options.recordPath) },
                { "replayCommands", options.replayCommands },
            });

        nlohmann::json state{
            { "version", RuntimeFileVersion },
            { "sessionId", sessionId },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "scene", LamaPon::PathToUtf8(scene) },
            { "status", "queued" },
            { "progress", 0.0 },
            { "message", "Queued" },
            { "pid", 0 },
            { "sessionDirectory",
                LamaPon::PathToUtf8(directory) },
            { "requestPath",
                LamaPon::PathToUtf8(directory / L"request.json") },
            { "controlPath",
                LamaPon::PathToUtf8(directory / L"control.json") },
            { "statePath",
                LamaPon::PathToUtf8(directory / L"state.json") },
            { "nextCommandSeq", 1 },
        };
        WriteRuntimeState(directory, state);

        const auto executable = JobExecutable();
        std::vector<std::wstring> workerArguments{
            executable.wstring(),
            L"runtime",
            L"worker",
            L"--session",
            directory.wstring(),
        };
        auto commandLine = BuildCommandLine(workerArguments);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                executable.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                projectRoot.c_str(),
                &startup,
                &process))
        {
            throw std::runtime_error(
                "Could not start the runtime worker (Win32 error "
                + std::to_string(GetLastError())
                + ").");
        }
        state["status"] = "running";
        state["message"] = "Starting";
        state["pid"] = process.dwProcessId;
        WriteRuntimeState(directory, state);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        state["poll"] =
            "LamaPonCli.exe runtime status --project \""
            + LamaPon::PathToUtf8(projectRoot)
            + "\" --id "
            + sessionId;
        return {
            projectRoot,
            directory,
            sessionId,
            std::move(state),
        };
    }

    [[nodiscard]] int RunRuntimeStart(
        const RuntimeStartOptions& options)
    {
        auto session = StartRuntimeSession(options);
        session.state["poll"] =
            "LamaPonCli.exe runtime status --project \""
            + LamaPon::PathToUtf8(session.projectRoot)
            + "\" --id "
            + session.sessionId;
        std::cout
            << RuntimeReport(
                "runtime start",
                std::move(session.state)).dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] nlohmann::json ReadRuntimeStatus(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId)
    {
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        auto state = RuntimeState(directory);
        const auto status = state.value("status", std::string{});
        if ((status == "queued"
                || status == "running"
                || status == "paused")
            && !IsJobProcessAlive(state.value("pid", 0u)))
        {
            state["status"] = "failed";
            state["progress"] = 1.0;
            state["message"] =
                "The runtime worker exited without stopping cleanly.";
            state["error"] =
                "The runtime worker exited without stopping cleanly.";
            WriteRuntimeState(directory, state);
        }
        return state;
    }

    [[nodiscard]] int RunRuntimeStatus(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId)
    {
        std::cout
            << RuntimeReport(
                "runtime status",
                ReadRuntimeStatus(projectRoot, sessionId)).dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] nlohmann::json SendRuntimeCommand(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId,
        nlohmann::json command)
    {
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        auto state = RuntimeState(directory);
        const auto status = state.value("status", std::string{});
        if (status != "queued"
            && status != "running"
            && status != "paused")
        {
            throw std::runtime_error(
                "The runtime session is not running: " + status);
        }
        if (!IsJobProcessAlive(state.value("pid", 0u)))
        {
            throw std::runtime_error(
                "The runtime worker is no longer running.");
        }
        if (!command.is_object())
        {
            throw std::invalid_argument(
                "The runtime command must be a JSON object.");
        }

        const auto acknowledged =
            state.value<std::uint64_t>("lastCommandSeq", 0);
        std::uint64_t existingSequence{};
        try
        {
            const auto existing =
                ReadJsonFile(directory / L"control.json");
            existingSequence =
                existing.value<std::uint64_t>("seq", 0);
        }
        catch (const std::exception&)
        {
        }
        if (existingSequence > acknowledged)
        {
            throw std::runtime_error(
                "The previous runtime command is still pending"
                " (seq " + std::to_string(existingSequence) + ").");
        }
        const auto sequence =
            std::max(existingSequence, acknowledged) + 1;
        command["seq"] = sequence;
        WriteJsonFile(directory / L"control.json", command);
        // Workerが同時に更新しているstate.jsonを、送信側が古い内容で
        // 丸ごと上書きしてはいけません。採番はcontrolと応答済みseq
        // だけで行い、完了状態はWorkerだけが書きます。

        nlohmann::json response{
            { "ok", true },
            { "command", "runtime send" },
            { "sessionId",
                LamaPon::PathToUtf8(
                    LamaPon::PathFromUtf8(
                        LamaPon::WideToUtf8(sessionId))) },
            { "seq", sequence },
            { "operation", RuntimeCommandName(command) },
            { "statePath",
                LamaPon::PathToUtf8(directory / L"state.json") },
        };
        return response;
    }

    [[nodiscard]] int RunRuntimeSend(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId,
        nlohmann::json command)
    {
        std::cout
            << SendRuntimeCommand(
                projectRoot,
                sessionId,
                std::move(command)).dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    void TerminateRuntimeSession(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId,
        const std::string& reason)
    {
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        auto state = RuntimeState(directory);
        const auto processId = state.value("pid", 0u);
        if (processId != 0)
        {
            HANDLE process = OpenProcess(
                PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                processId);
            if (process != nullptr)
            {
                TerminateProcess(process, 1);
                CloseHandle(process);
            }
        }
        state["status"] = "failed";
        state["progress"] = 1.0;
        state["message"] = reason;
        state["error"] = reason;
        state["recovered"] = true;
        WriteRuntimeState(directory, state);
    }

    [[nodiscard]] const nlohmann::json* RuntimeJsonPath(
        const nlohmann::json& document,
        const std::string& path)
    {
        const nlohmann::json* current = &document;
        std::size_t start{};
        while (start < path.size())
        {
            const auto dot = path.find('.', start);
            const auto token = path.substr(
                start,
                dot == std::string::npos
                    ? std::string::npos
                    : dot - start);
            if (token.empty())
            {
                return nullptr;
            }
            const auto bracket = token.find('[');
            const auto field = token.substr(
                0,
                bracket == std::string::npos
                    ? std::string::npos
                    : bracket);
            if (!field.empty())
            {
                if (!current->is_object())
                {
                    return nullptr;
                }
                if (!current->contains(field))
                {
                    // gameStateの"u3.card"のようにドットを含むキーは分割探索では
                    // 見つからない。残りのパス全体を1個のキーとして引き直す。
                    const auto literal = path.substr(start);
                    if (current->contains(literal))
                    {
                        return &current->at(literal);
                    }
                    return nullptr;
                }
                current = &current->at(field);
            }
            std::size_t selectorStart = bracket;
            while (selectorStart != std::string::npos)
            {
                const auto selectorEnd = token.find(
                    ']',
                    selectorStart + 1);
                if (selectorEnd == std::string::npos)
                {
                    return nullptr;
                }
                const auto selector = token.substr(
                    selectorStart + 1,
                    selectorEnd - selectorStart - 1);
                if (current->is_array())
                {
                    try
                    {
                        const auto index = std::stoull(selector);
                        if (index >= current->size())
                        {
                            return nullptr;
                        }
                        current = &current->at(index);
                    }
                    catch (const std::exception&)
                    {
                        const auto equal = selector.find('=');
                        if (equal == std::string::npos)
                        {
                            return nullptr;
                        }
                        const auto key = selector.substr(0, equal);
                        const auto expected = TrimRuntimeToken(
                            selector.substr(equal + 1));
                        const nlohmann::json* match = nullptr;
                        for (const auto& item : *current)
                        {
                            if (item.is_object()
                                && item.contains(key)
                                && item.at(key).is_string()
                                && item.at(key).get<std::string>()
                                    == expected)
                            {
                                match = &item;
                                break;
                            }
                        }
                        if (match == nullptr)
                        {
                            return nullptr;
                        }
                        current = match;
                    }
                }
                else
                {
                    return nullptr;
                }
                selectorStart = token.find('[', selectorEnd + 1);
            }
            if (dot == std::string::npos)
            {
                break;
            }
            start = dot + 1;
        }
        return current;
    }

    [[nodiscard]] bool RuntimeJsonNumberCompare(
        const nlohmann::json& actual,
        const nlohmann::json& expected,
        const std::string& operation)
    {
        if (!actual.is_number() || !expected.is_number())
        {
            return false;
        }
        const auto left = actual.get<double>();
        const auto right = expected.get<double>();
        if (operation == "gt") return left > right;
        if (operation == "gte") return left >= right;
        if (operation == "lt") return left < right;
        return left <= right;
    }

    [[nodiscard]] nlohmann::json RuntimeAssertionResult(
        const nlohmann::json& document,
        const nlohmann::json& assertion)
    {
        const auto path = assertion.value("path", std::string{});
        const auto* actual = path.empty()
            ? nullptr
            : RuntimeJsonPath(document, path);
        const bool exists = actual != nullptr;
        bool passed = assertion.value("exists", true) == exists;
        std::string operatorName;
        for (const auto* candidate : {
                "equals", "notEquals", "gt", "gte", "lt", "lte",
                "contains" })
        {
            if (assertion.contains(candidate))
            {
                operatorName = candidate;
                break;
            }
        }
        if (!operatorName.empty())
        {
            const auto& expected = assertion.at(operatorName);
            if (!exists)
            {
                passed = false;
            }
            else if (operatorName == "equals")
            {
                passed = *actual == expected;
            }
            else if (operatorName == "notEquals")
            {
                passed = *actual != expected;
            }
            else if (operatorName == "contains")
            {
                passed = actual->is_string()
                    && expected.is_string()
                    && actual->get<std::string>().find(
                        expected.get<std::string>())
                        != std::string::npos;
            }
            else
            {
                passed = RuntimeJsonNumberCompare(
                    *actual,
                    expected,
                    operatorName);
            }
        }
        return {
            { "path", path },
            { "passed", passed },
            { "actual", actual == nullptr ? nullptr : *actual },
            { "expected", assertion.contains(operatorName)
                ? assertion.at(operatorName)
                : nlohmann::json{} },
        };
    }

    [[nodiscard]] nlohmann::json RuntimeCommandFromStep(
        const nlohmann::json& step)
    {
        if (!step.contains("command"))
        {
            return nlohmann::json::object();
        }
        const auto& command = step.at("command");
        if (command.is_object())
        {
            return command;
        }
        if (command.is_string())
        {
            return ParseRuntimeCommandText(
                LamaPon::Utf8ToWide(command.get<std::string>()));
        }
        throw std::invalid_argument(
            "runtime test step command must be an object or string.");
    }

    void RuntimeTestWait(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId,
        const std::chrono::steady_clock::time_point deadline,
        const std::uint64_t minimumFrame = 0,
        const std::uint64_t minimumCommandSequence = 0)
    {
        while (true)
        {
            const auto state = ReadRuntimeStatus(
                projectRoot,
                sessionId);
            const auto status = state.value(
                "status",
                std::string{});
            if (status == "failed")
            {
                throw std::runtime_error(
                    state.value(
                        "error",
                        std::string{
                            "runtime worker failed." }));
            }
            const bool commandCompleted =
                state.value<std::uint64_t>("lastCommandSeq", 0)
                    >= minimumCommandSequence;
            if (status == "stopped")
            {
                if (commandCompleted)
                {
                    return;
                }
                throw std::runtime_error(
                    "runtime stopped before acknowledging a command.");
            }
            if (state.value<std::uint64_t>("frame", 0)
                    >= minimumFrame
                && commandCompleted)
            {
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                throw std::runtime_error(
                    "runtime test timed out while waiting for a frame"
                    " or command acknowledgement.");
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));
        }
    }

    [[nodiscard]] std::filesystem::path ResolveRuntimeInputFile(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& requested)
    {
        if (requested.is_absolute()
            || std::filesystem::exists(requested))
        {
            return requested;
        }
        return projectRoot / requested;
    }

    [[nodiscard]] int RunRuntimeTest(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& specPath)
    {
        const auto spec = ReadJsonFile(
            ResolveRuntimeInputFile(projectRoot, specPath));
        RuntimeStartOptions options;
        options.projectRoot = projectRoot;
        options.scene = LamaPon::PathFromUtf8(
            spec.value("scene", std::string{}));
        options.width = spec.value("width", 0u);
        options.height = spec.value("height", 0u);
        options.targetFrameRate = std::clamp<std::uint32_t>(
            spec.value("fps", 60u),
            1u,
            240u);
        options.fixedDeltaTime = spec.value(
            "fixedDeltaTime",
            1.0f / static_cast<float>(options.targetFrameRate));
        options.warp = spec.value("warp", true);
        options.d3dDebug = spec.value("d3dDebug", false);
        options.deterministic = spec.value("deterministic", true);
        options.renderEveryNFrames = std::clamp<std::uint32_t>(
            spec.value("renderEveryNFrames", 1u),
            1u,
            100'000u);
        options.paceFrames = spec.value("paceFrames", true);
        if (spec.contains("record"))
        {
            options.recordPath = std::filesystem::absolute(
                projectRoot
                / LamaPon::PathFromUtf8(
                    spec.at("record").get<std::string>()));
        }
        const auto timeout = std::chrono::milliseconds(
            std::max(
                spec.value("timeoutMs", 10000),
                100));
        const auto deadline =
            std::chrono::steady_clock::now() + timeout;
        RuntimeSessionHandle session;
        nlohmann::json assertions = nlohmann::json::array();
        bool passed = false;
        std::string error;
        try
        {
            session = StartRuntimeSession(options);
            const auto sessionId =
                LamaPon::Utf8ToWide(session.sessionId);
            RuntimeTestWait(
                projectRoot,
                sessionId,
                deadline,
                1);
            for (const auto& step : spec.value(
                "steps",
                nlohmann::json::array()))
            {
                const auto command = RuntimeCommandFromStep(step);
                if (!command.empty())
                {
                    const auto response = SendRuntimeCommand(
                        projectRoot,
                        sessionId,
                        command);
                    const auto sequence =
                        response.at("seq").get<std::uint64_t>();
                    // screenshotはWorkerがPNG保存を終えてからseqを
                    // stateへ書くため、この待機でwaitFrames:0でも安全です。
                    RuntimeTestWait(
                        projectRoot,
                        sessionId,
                        deadline,
                        0,
                        sequence);
                }
                const auto waitFrames = step.value<std::uint64_t>(
                    "waitFrames",
                    1);
                const auto current = ReadRuntimeStatus(
                    projectRoot,
                    sessionId);
                RuntimeTestWait(
                    projectRoot,
                    sessionId,
                    deadline,
                    current.value<std::uint64_t>("frame", 0)
                        + waitFrames);
                if (step.contains("assert"))
                {
                    for (const auto& assertion : step.at("assert"))
                    {
                        const auto state = ReadRuntimeStatus(
                            projectRoot,
                            sessionId);
                        assertions.push_back(
                            RuntimeAssertionResult(state, assertion));
                    }
                }
                if (step.contains("waitMs"))
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(
                            step.at("waitMs").get<std::uint32_t>()));
                }
            }
            const auto finalState = ReadRuntimeStatus(
                projectRoot,
                sessionId);
            for (const auto& assertion : spec.value(
                "assert",
                nlohmann::json::array()))
            {
                assertions.push_back(
                    RuntimeAssertionResult(finalState, assertion));
            }
            passed = std::ranges::all_of(
                assertions,
                [](const auto& result)
                {
                    return result.value("passed", false);
                });
            const auto stopResponse = SendRuntimeCommand(
                projectRoot,
                sessionId,
                { { "op", "stop" } });
            const auto cleanupDeadline = std::max(
                deadline,
                std::chrono::steady_clock::now()
                    + std::chrono::seconds(2));
            RuntimeTestWait(
                projectRoot,
                sessionId,
                cleanupDeadline,
                0,
                stopResponse.at("seq").get<std::uint64_t>());
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
            if (!session.sessionId.empty())
            {
                try
                {
                    TerminateRuntimeSession(
                        projectRoot,
                        LamaPon::Utf8ToWide(session.sessionId),
                        error);
                }
                catch (const std::exception&)
                {
                }
            }
        }
        nlohmann::json report{
            { "ok", passed && error.empty() },
            { "command", "runtime test" },
            { "sessionId", session.sessionId },
            { "assertions", std::move(assertions) },
        };
        if (!error.empty())
        {
            report["error"] = error;
        }
        std::cout << report.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace)
            << std::endl;
        return report.at("ok").get<bool>() ? 0 : 1;
    }

    [[nodiscard]] int RunRuntimeReplay(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& replayPath)
    {
        const auto replay = ReadJsonFile(
            ResolveRuntimeInputFile(projectRoot, replayPath));
        RuntimeStartOptions options;
        options.projectRoot = projectRoot;
        options.scene = LamaPon::PathFromUtf8(
            replay.value("scene", std::string{}));
        options.targetFrameRate = std::clamp<std::uint32_t>(
            replay.value("targetFrameRate", 60u),
            1u,
            240u);
        options.fixedDeltaTime = replay.value(
            "fixedDeltaTime",
            1.0f / static_cast<float>(options.targetFrameRate));
        options.deterministic = true;
        options.warp = true;
        options.renderEveryNFrames = std::clamp<std::uint32_t>(
            replay.value("renderEveryNFrames", 1u),
            1u,
            100'000u);
        options.paceFrames = replay.value("paceFrames", true);
        options.replayCommands = replay.value(
            "commands",
            nlohmann::json::array());
        const auto session = StartRuntimeSession(options);
        const auto sessionId = LamaPon::Utf8ToWide(session.sessionId);
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(30'000);
        while (true)
        {
            const auto state = ReadRuntimeStatus(
                projectRoot,
                sessionId);
            if (state.value("status", std::string{}) == "failed"
                || state.value("status", std::string{}) == "stopped"
                || state.value("replayComplete", false))
            {
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                TerminateRuntimeSession(
                    projectRoot,
                    sessionId,
                    "runtime replay timed out.");
                throw std::runtime_error(
                    "runtime replay timed out.");
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(25));
        }
        const auto finalState = ReadRuntimeStatus(
            projectRoot,
            sessionId);
        const auto finalStatus = finalState.value(
            "status",
            std::string{});
        if (finalStatus == "queued"
            || finalStatus == "running"
            || finalStatus == "paused")
        {
            static_cast<void>(SendRuntimeCommand(
                projectRoot,
                sessionId,
                { { "op", "stop" } }));
        }
        std::cout << RuntimeReport(
            "runtime replay",
            finalState).dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return finalStatus == "failed"
            ? 1
            : 0;
    }

    [[nodiscard]] int RunRuntimeRecover(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId)
    {
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        const auto state = RuntimeState(directory);
        if (IsJobProcessAlive(state.value("pid", 0u)))
        {
            throw std::runtime_error(
                "The runtime session is still running.");
        }
        const auto request = ReadJsonFile(directory / L"request.json");
        RuntimeStartOptions options;
        options.projectRoot = projectRoot;
        options.scene = LamaPon::PathFromUtf8(
            request.value("scene", std::string{}));
        options.width = request.value("width", 0u);
        options.height = request.value("height", 0u);
        options.targetFrameRate = request.value(
            "targetFrameRate",
            60u);
        options.fixedDeltaTime = request.value(
            "fixedDeltaTime",
            1.0f / static_cast<float>(options.targetFrameRate));
        options.warp = request.value("warp", true);
        options.d3dDebug = request.value("d3dDebug", false);
        options.deterministic = request.value(
            "deterministic",
            false);
        options.renderEveryNFrames = std::clamp<std::uint32_t>(
            request.value("renderEveryNFrames", 1u),
            1u,
            100'000u);
        options.paceFrames = request.value("paceFrames", true);
        const auto replacement = StartRuntimeSession(options);
        nlohmann::json response{
            { "ok", true },
            { "command", "runtime recover" },
            { "previousSessionId",
                LamaPon::WideToUtf8(sessionId) },
            { "session", std::move(replacement.state) },
        };
        std::cout << response.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] nlohmann::json AssetRecordJson(
        const std::filesystem::path& assetRoot,
        const LamaPon::AssetRecord& record)
    {
        nlohmann::json dependencies = nlohmann::json::array();
        for (const auto& dependency : record.dependencies)
        {
            dependencies.push_back(dependency);
        }
        nlohmann::json dependents = nlohmann::json::array();
        for (const auto& dependent : record.dependents)
        {
            dependents.push_back(dependent);
        }
        const auto absolutePath =
            std::filesystem::weakly_canonical(
                assetRoot / record.path);
        const auto metaPath =
            std::filesystem::weakly_canonical(
                assetRoot / record.metaPath);
        nlohmann::json result{
            { "guid", record.guid },
            { "path", LamaPon::PathToUtf8(record.path) },
            { "absolutePath", LamaPon::PathToUtf8(absolutePath) },
            { "metaPath", LamaPon::PathToUtf8(metaPath) },
            { "importer", record.importer },
            { "exists", std::filesystem::is_regular_file(absolutePath) },
            { "dependencies", std::move(dependencies) },
            { "dependents", std::move(dependents) },
        };
        if (std::filesystem::is_regular_file(metaPath))
        {
            try
            {
                result["meta"] = ReadJsonFile(metaPath);
            }
            catch (const std::exception& exception)
            {
                result["metaError"] = exception.what();
            }
        }
        else
        {
            result["meta"] = nullptr;
        }
        return result;
    }

    [[nodiscard]] int RunAssetCommand(
        const std::filesystem::path& requestedProject,
        const std::wstring& action,
        const std::filesystem::path& requestedPath,
        const std::string& requestedGuid,
        const std::string& importerFilter)
    {
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        LamaPon::AssetDatabase database;
        database.SetAssetRoot(assetRoot);
        const auto refresh = database.Refresh(false);

        if (action == L"list")
        {
            nlohmann::json assets = nlohmann::json::array();
            std::unordered_map<std::string, std::size_t> importerCounts;
            for (const auto& record : database.Assets())
            {
                if (!importerFilter.empty()
                    && record.importer != importerFilter)
                {
                    continue;
                }
                ++importerCounts[record.importer];
                assets.push_back(AssetRecordJson(assetRoot, record));
            }
            nlohmann::json counts = nlohmann::json::object();
            for (const auto& [importer, count] : importerCounts)
            {
                counts[importer] = count;
            }
            const nlohmann::json report{
                { "ok", true },
                { "command", "asset list" },
                { "project", LamaPon::PathToUtf8(projectRoot) },
                { "assetRoot", LamaPon::PathToUtf8(assetRoot) },
                { "assetCount", assets.size() },
                { "dependencyCount", refresh.dependencyCount },
                { "createdMetaCount", refresh.createdMetaCount },
                { "importerCounts", std::move(counts) },
                { "assets", std::move(assets) },
            };
            std::cout
                << report.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
                << std::endl;
            return 0;
        }

        if (action == L"inspect")
        {
            if (requestedPath.empty() && requestedGuid.empty())
            {
                throw std::invalid_argument(
                    "asset inspect requires --path or --guid.");
            }
            const LamaPon::AssetRecord* record{};
            if (!requestedGuid.empty())
            {
                record = database.FindByGuid(requestedGuid);
            }
            else
            {
                const auto relative = NormalizeAssetPath(
                    projectRoot,
                    requestedPath);
                record = database.FindByPath(relative);
            }
            if (record == nullptr)
            {
                throw std::runtime_error(
                    "The requested asset was not found.");
            }
            const nlohmann::json report{
                { "ok", true },
                { "command", "asset inspect" },
                { "project", LamaPon::PathToUtf8(projectRoot) },
                { "asset", AssetRecordJson(assetRoot, *record) },
            };
            std::cout
                << report.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
                << std::endl;
            return 0;
        }

        throw std::invalid_argument(
            "Unknown asset action: "
            + LamaPon::PathToUtf8(std::filesystem::path(action)));
    }

    [[nodiscard]] int RunAssetImport(
        const std::filesystem::path& requestedProject,
        const std::vector<std::filesystem::path>& sources,
        const std::filesystem::path& requestedTarget)
    {
        if (sources.empty())
        {
            throw std::invalid_argument(
                "asset import requires at least one --source.");
        }
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        const auto target = NormalizeAssetPath(
            projectRoot,
            requestedTarget.empty()
                ? std::filesystem::path{ L"." }
                : requestedTarget);
        const auto result = LamaPon::AssetImporter::Import(
            sources,
            assetRoot,
            target);
        nlohmann::json files = nlohmann::json::array();
        for (const auto& file : result.files)
        {
            files.push_back({
                { "source", LamaPon::PathToUtf8(file.source) },
                { "path", LamaPon::PathToUtf8(file.path) },
                { "renamed", file.renamed },
            });
        }
        nlohmann::json failures = nlohmann::json::array();
        for (const auto& failure : result.failures)
        {
            failures.push_back({
                { "source", LamaPon::PathToUtf8(failure.source) },
                { "message", failure.message },
            });
        }
        const nlohmann::json report{
            { "ok", failures.empty() },
            { "command", "asset import" },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "target", LamaPon::PathToUtf8(target) },
            { "files", std::move(files) },
            { "failures", std::move(failures) },
            { "importedDirectoryCount",
                result.importedDirectoryCount },
            { "renamedSourceCount",
                result.renamedSourceCount },
            { "skippedMetadataCount",
                result.skippedMetadataCount },
            { "skippedLinkCount",
                result.skippedLinkCount },
        };
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return failures.empty() ? 0 : 1;
    }

    [[nodiscard]] bool IsCppIdentifier(
        const std::string& value)
    {
        if (value.empty()
            || (value.front() >= '0' && value.front() <= '9'))
        {
            return false;
        }
        return std::ranges::all_of(
            value,
            [](const unsigned char character)
            {
                return (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '_';
            });
    }

    [[nodiscard]] int RunScriptCreate(
        const std::filesystem::path& requestedProject,
        const std::filesystem::path& requestedPath,
        const std::string& className,
        const bool force)
    {
        if (!IsCppIdentifier(className))
        {
            throw std::invalid_argument(
                "script create requires a valid C++ class name.");
        }
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        const auto relative = NormalizeAssetPath(
            projectRoot,
            requestedPath);
        if (relative.extension() != L".cpp")
        {
            throw std::invalid_argument(
                "script create path must use the .cpp extension.");
        }
        const auto destination = assetRoot / relative;
        const bool existed =
            std::filesystem::is_regular_file(destination);
        if (existed && !force)
        {
            throw std::runtime_error(
                "The script already exists; use --force to overwrite: "
                + LamaPon::PathToUtf8(relative));
        }
        const std::string source =
            "#include \"LamaPon/LamaPon.h\"\n"
            "\n"
            "class " + className
            + " final : public LamaPon::Script\n"
              "{\n"
              "public:\n"
              "    void Start() override\n"
              "    {\n"
              "        // 初期化処理\n"
              "    }\n"
              "\n"
              "    void Update(const float deltaTime) override\n"
              "    {\n"
              "        static_cast<void>(deltaTime);\n"
              "        // 毎フレームの処理\n"
              "    }\n"
              "};\n"
              "\n"
              "LAMAPON_SCRIPT(" + className + ");\n";
        WriteTextAtomic(destination, source);
        const nlohmann::json report{
            { "ok", true },
            { "command", "script create" },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "path", LamaPon::PathToUtf8(relative) },
            { "absolutePath", LamaPon::PathToUtf8(destination) },
            { "class", className },
            { "overwritten", existed },
            { "bytes", source.size() },
        };
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] std::string ReadTextFile(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{} };
    }

    // ソースから`LAMAPON_SCRIPT`系の登録名を拾います。
    //
    // シーンJSONのNativeScriptへ書く名前は、クラス名そのものではなく
    // `Game.<クラス名>`です（`LAMAPON_SCRIPT`が`"Game." #type`で
    // 登録するため）。この対応をCLIから知る手段が無く、`Game.`を
    // 忘れても`patch`も`build`も成功してしまい、`render`の`problems`に
    // 「型が登録されていません」と出るまで気付けませんでした。
    //
    // DLLを読まずソースを読むだけなので、`build`の前でも答えられます。
    [[nodiscard]] nlohmann::json ScriptTypesInSource(
        const std::string& contents)
    {
        nlohmann::json types = nlohmann::json::array();
        struct Macro final
        {
            const char* name;
            bool hasExplicitId;
        };
        static constexpr Macro Macros[]{
            { "LAMAPON_SCRIPT_WITH_SCHEMA", true },
            { "LAMAPON_SCRIPT_NAMED", true },
            { "LAMAPON_SCRIPT", false },
        };
        std::vector<std::size_t> consumed;
        for (const auto& macro : Macros)
        {
            const std::string token{ macro.name };
            std::size_t search = 0;
            while ((search = contents.find(token, search))
                != std::string::npos)
            {
                const std::size_t at = search;
                search += token.size();
                // 長いマクロ名の一部を短い名前として二重に拾わない
                // ようにします（..._NAMEDはLAMAPON_SCRIPTを含む）。
                if (std::ranges::find(consumed, at)
                    != consumed.end())
                {
                    continue;
                }
                const auto open = contents.find_first_not_of(
                    " \t",
                    search);
                if (open == std::string::npos
                    || contents[open] != '(')
                {
                    continue;
                }
                const auto close = contents.find(')', open);
                if (close == std::string::npos)
                {
                    continue;
                }
                consumed.push_back(at);
                auto arguments = contents.substr(
                    open + 1,
                    close - open - 1);
                std::vector<std::string> parts;
                std::size_t partStart = 0;
                while (partStart <= arguments.size())
                {
                    const auto comma =
                        arguments.find(',', partStart);
                    auto part = arguments.substr(
                        partStart,
                        comma == std::string::npos
                            ? std::string::npos
                            : comma - partStart);
                    partStart = comma == std::string::npos
                        ? arguments.size() + 1
                        : comma + 1;
                    const auto first =
                        part.find_first_not_of(" \t\r\n\"");
                    if (first == std::string::npos)
                    {
                        continue;
                    }
                    const auto last =
                        part.find_last_not_of(" \t\r\n\"");
                    parts.push_back(
                        part.substr(first, last - first + 1));
                }
                if (parts.empty())
                {
                    continue;
                }
                const std::string className = parts.front();
                const std::string id =
                    macro.hasExplicitId && parts.size() >= 2
                        ? parts[1]
                        : "Game." + className;
                types.push_back({
                    { "class", className },
                    // シーンJSONの`"script"`へそのまま書ける値。
                    { "id", id },
                });
            }
        }
        return types;
    }

    [[nodiscard]] std::size_t LineCount(
        const std::string& contents)
    {
        if (contents.empty())
        {
            return 0;
        }
        const auto newlineCount = static_cast<std::size_t>(
            std::count(contents.begin(), contents.end(), '\n'));
        return contents.back() == '\n'
            ? newlineCount
            : newlineCount + 1;
    }

    [[nodiscard]] int RunScriptCommand(
        const std::filesystem::path& requestedProject,
        const std::wstring& action,
        const std::filesystem::path& requestedPath)
    {
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        LamaPon::AssetDatabase database;
        database.SetAssetRoot(assetRoot);
        static_cast<void>(database.Refresh(false));

        if (action == L"list")
        {
            nlohmann::json scripts = nlohmann::json::array();
            for (const auto& record : database.Assets())
            {
                if (record.importer != "CppScript")
                {
                    continue;
                }
                const auto absolute = assetRoot / record.path;
                std::error_code sizeError;
                const auto size = std::filesystem::file_size(
                    absolute,
                    sizeError);
                scripts.push_back({
                    { "guid", record.guid },
                    { "path", LamaPon::PathToUtf8(record.path) },
                    { "bytes", sizeError ? 0 : size },
                    // シーンJSONへ書く登録名。`patch`の
                    // add-componentへそのまま渡せます。
                    { "scriptTypes",
                        ScriptTypesInSource(
                            ReadTextFile(absolute)) },
                    { "dependencies", record.dependencies },
                    { "dependents", record.dependents },
                });
            }
            const nlohmann::json report{
                { "ok", true },
                { "command", "script list" },
                { "project", LamaPon::PathToUtf8(projectRoot) },
                { "scriptCount", scripts.size() },
                { "scripts", std::move(scripts) },
            };
            std::cout
                << report.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
                << std::endl;
            return 0;
        }

        if (action == L"inspect")
        {
            if (requestedPath.empty())
            {
                throw std::invalid_argument(
                    "script inspect requires --path.");
            }
            const auto relative = NormalizeAssetPath(
                projectRoot,
                requestedPath);
            const auto absolute = assetRoot / relative;
            if (!std::filesystem::is_regular_file(absolute))
            {
                throw std::runtime_error(
                    "The requested script was not found: "
                    + LamaPon::PathToUtf8(relative));
            }
            const std::string contents = ReadTextFile(absolute);
            const auto* record = database.FindByPath(relative);
            const nlohmann::json report{
                { "ok", true },
                { "command", "script inspect" },
                { "project", LamaPon::PathToUtf8(projectRoot) },
                { "path", LamaPon::PathToUtf8(relative) },
                { "guid", record == nullptr
                    ? std::string{}
                    : record->guid },
                { "bytes", contents.size() },
                { "lines", LineCount(contents) },
                { "scriptTypes", ScriptTypesInSource(contents) },
                { "source", contents },
            };
            std::cout
                << report.dump(
                    2,
                    ' ',
                    false,
                    nlohmann::json::error_handler_t::replace)
                << std::endl;
            return 0;
        }

        throw std::invalid_argument(
            "Unknown script action: "
            + LamaPon::PathToUtf8(std::filesystem::path(action)));
    }

    [[nodiscard]] int RunProjectInspect(
        const std::filesystem::path& requestedProject)
    {
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        const auto settings = ReadJsonFile(settingsPath);
        const auto parsedSettings = LamaPon::LoadProjectSettings(
            settingsPath);
        LamaPon::AssetDatabase database;
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        database.SetAssetRoot(assetRoot);
        const auto refresh = database.Refresh(false);
        std::unordered_map<std::string, std::size_t> importerCounts;
        for (const auto& record : database.Assets())
        {
            ++importerCounts[record.importer];
        }
        nlohmann::json counts = nlohmann::json::object();
        for (const auto& [importer, count] : importerCounts)
        {
            counts[importer] = count;
        }
        const nlohmann::json report{
            { "ok", true },
            { "command", "project inspect" },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "settingsPath", LamaPon::PathToUtf8(settingsPath) },
            { "settings", settings },
            { "startupScene",
                LamaPon::PathToUtf8(parsedSettings.startupScene) },
            { "assetCount", refresh.assetCount },
            { "dependencyCount", refresh.dependencyCount },
            { "importerCounts", std::move(counts) },
        };
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] int RunProjectList()
    {
        nlohmann::json projects = nlohmann::json::array();
        for (const auto& project :
            LamaPon::Hub::LoadRecentProjects())
        {
            projects.push_back({
                { "name", project.name },
                { "path", LamaPon::PathToUtf8(project.path) },
            });
        }
        const nlohmann::json report{
            { "ok", true },
            { "command", "project list" },
            { "projectCount", projects.size() },
            { "projects", std::move(projects) },
            { "hubSettings",
                LamaPon::PathToUtf8(
                    LamaPon::Hub::SettingsPath()) },
        };
        std::cout << report.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] int RunProjectRegistration(
        const std::filesystem::path& requestedProject,
        const bool add)
    {
        const auto projectRoot = add
            ? CanonicalProjectRoot(requestedProject)
            : std::filesystem::absolute(requestedProject)
                .lexically_normal();
        if (add)
        {
            LamaPon::Hub::AddRecentProject(projectRoot);
        }
        else
        {
            // 移動・削除済みのパスもHubから掃除できるよう、removeは
            // Projectとして存在することを要求しません。
            LamaPon::Hub::RemoveRecentProject(projectRoot);
        }
        const nlohmann::json report{
            { "ok", true },
            { "command", add ? "project add" : "project remove" },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "registered", add },
        };
        std::cout << report.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    struct DirectoryInventory final
    {
        std::uint64_t entries{};
        std::uintmax_t bytes{};

        [[nodiscard]] bool operator==(
            const DirectoryInventory&) const noexcept = default;
    };

    [[nodiscard]] DirectoryInventory InventoryDirectory(
        const std::filesystem::path& root)
    {
        DirectoryInventory result;
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(root))
        {
            ++result.entries;
            if (entry.is_regular_file())
            {
                result.bytes += entry.file_size();
            }
        }
        return result;
    }

    [[nodiscard]] std::wstring ProjectPathKey(
        const std::filesystem::path& path)
    {
        auto key = std::filesystem::weakly_canonical(
            std::filesystem::absolute(path)).native();
        std::transform(
            key.begin(),
            key.end(),
            key.begin(),
            [](const wchar_t character)
            {
                return static_cast<wchar_t>(std::towlower(character));
            });
        return key;
    }

    [[nodiscard]] bool IsSameOrNestedProjectPath(
        const std::filesystem::path& candidate,
        const std::filesystem::path& parent)
    {
        const auto candidateKey = ProjectPathKey(candidate);
        auto parentKey = ProjectPathKey(parent);
        if (candidateKey == parentKey)
        {
            return true;
        }
        if (!parentKey.empty()
            && parentKey.back()
                != std::filesystem::path::preferred_separator)
        {
            parentKey.push_back(
                std::filesystem::path::preferred_separator);
        }
        return candidateKey.starts_with(parentKey);
    }

    [[nodiscard]] int RunProjectMove(
        const std::filesystem::path& requestedSource,
        const std::filesystem::path& requestedDestination,
        const bool allowInsideEngineSource)
    {
        const auto source = CanonicalProjectRoot(requestedSource);
        const auto destination = std::filesystem::absolute(
            requestedDestination).lexically_normal();
        if (IsSameOrNestedProjectPath(destination, source)
            || IsSameOrNestedProjectPath(source, destination))
        {
            throw std::invalid_argument(
                "The move destination must not be the source, its child,"
                " or its parent.");
        }
        if (!allowInsideEngineSource
            && LamaPon::Hub::IsInsideEngineSourceTree(destination))
        {
            throw std::runtime_error(
                "A game project cannot be moved inside the LamaPon"
                " source tree. Choose a folder beside the repository.");
        }

        const bool restoreEmptyDestination =
            std::filesystem::exists(destination)
            && std::filesystem::is_directory(destination)
            && std::filesystem::is_empty(destination);
        if (std::filesystem::exists(destination)
            && !restoreEmptyDestination)
        {
            throw std::runtime_error(
                "The move destination already exists and is not empty: "
                + LamaPon::PathToUtf8(destination));
        }
        std::filesystem::create_directories(destination.parent_path());

        std::filesystem::path staging;
        for (std::uint32_t attempt{}; attempt < 100u; ++attempt)
        {
            staging = destination.parent_path()
                / (destination.filename().wstring()
                    + L".lamapon-move-staging-"
                    + std::to_wstring(GetCurrentProcessId())
                    + L"-"
                    + std::to_wstring(GetTickCount64() + attempt));
            if (!std::filesystem::exists(staging))
            {
                break;
            }
            staging.clear();
        }
        if (staging.empty())
        {
            throw std::runtime_error(
                "Could not reserve a temporary move folder.");
        }

        bool stagingExists = false;
        try
        {
            Progress(
                "copy: " + LamaPon::PathToUtf8(source)
                + " -> " + LamaPon::PathToUtf8(destination));
            std::filesystem::copy(
                source,
                staging,
                std::filesystem::copy_options::recursive
                    | std::filesystem::copy_options::copy_symlinks);
            stagingExists = true;
            const auto sourceInventory = InventoryDirectory(source);
            const auto stagingInventory = InventoryDirectory(staging);
            if (!LamaPon::Hub::IsProject(staging)
                || sourceInventory != stagingInventory)
            {
                throw std::runtime_error(
                    "The copied project did not pass the integrity check.");
            }
            if (restoreEmptyDestination)
            {
                std::filesystem::remove(destination);
            }
            std::filesystem::rename(staging, destination);
            stagingExists = false;
        }
        catch (...)
        {
            if (stagingExists)
            {
                std::error_code cleanupError;
                std::filesystem::remove_all(staging, cleanupError);
            }
            if (restoreEmptyDestination
                && !std::filesystem::exists(destination))
            {
                std::error_code restoreError;
                std::filesystem::create_directories(
                    destination,
                    restoreError);
            }
            throw;
        }

        const auto sourceInventory = InventoryDirectory(source);
        const auto destinationInventory = InventoryDirectory(destination);
        const bool verified = LamaPon::Hub::IsProject(destination)
            && sourceInventory == destinationInventory;
        bool hubUpdated = false;
        std::string warning;
        if (verified)
        {
            try
            {
                LamaPon::Hub::AddRecentProject(destination);
                LamaPon::Hub::RemoveRecentProject(source);
                hubUpdated = true;
            }
            catch (const std::exception& exception)
            {
                warning = std::string{
                    "The project was copied, but Hub registration failed: " }
                    + exception.what();
            }
        }

        bool sourceRemoved = false;
        if (verified)
        {
            std::error_code removeError;
            std::filesystem::remove_all(source, removeError);
            sourceRemoved = !removeError
                && !std::filesystem::exists(source);
            if (!sourceRemoved)
            {
                if (!warning.empty())
                {
                    warning += ' ';
                }
                warning +=
                    "The verified destination is intact, but the source"
                    " could not be completely removed.";
            }
        }
        else
        {
            warning =
                "The destination is intact, but the final integrity check"
                " changed; the source was kept.";
        }

        const bool ok = verified && sourceRemoved;
        nlohmann::json report{
            { "ok", ok },
            { "command", "project move" },
            { "source", LamaPon::PathToUtf8(source) },
            { "destination", LamaPon::PathToUtf8(destination) },
            { "verified", verified },
            { "sourceRemoved", sourceRemoved },
            { "hubUpdated", hubUpdated },
            { "entries", destinationInventory.entries },
            { "bytes", destinationInventory.bytes },
        };
        if (!warning.empty())
        {
            report["warning"] = warning;
        }
        std::cout << report.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace)
            << std::endl;
        return ok ? 0 : 1;
    }

    [[nodiscard]] int RunRender(const RenderOptions& options)
    {
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    options.projectRoot));
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        const LamaPon::ProjectSettings settings =
            LamaPon::LoadProjectSettings(settingsPath);

        const std::uint32_t width =
            options.width != 0
                ? options.width
                : settings.windowWidth;
        const std::uint32_t height =
            options.height != 0
                ? options.height
                : settings.windowHeight;
        const auto scenePath = NormalizeScenePath(
            projectRoot,
            options.scene.empty()
                ? settings.startupScene
                : options.scene);

        Progress(
            "project: " + LamaPon::PathToUtf8(projectRoot));
        Progress(
            "scene:   " + LamaPon::PathToUtf8(scenePath));

        // ここからは描画回帰テストと同じ立ち上げ方です。
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(
            options.warp);
        LamaPon::GraphicsDevice::SetEnableDebugLayer(
            options.d3dDebug);
        const HWND window =
            CreateHiddenWindow(width, height);
        LamaPon::GraphicsDevice graphics;
        graphics.Initialize(window, width, height);
        graphics.Assets().SetAssetRoot(
            projectRoot / L"assets");

        // 絵はプロジェクトの品質設定のまま撮ります（ゲームと同じ
        // 見た目を返すのが目的のため）。垂直同期だけは待つ意味が
        // 無いので切ります。
        auto graphicsSettings = settings.graphics;
        graphicsSettings.vSyncEnabled = false;
        graphics.SetGraphicsSettings(graphicsSettings);
        // 1枚だけ撮るので非同期コンパイルは切ります。入れたままだと
        // 「まだ焼けていないので標準Lit」の絵を撮ってしまいます。
        graphics.SetAsyncShaderCompilationEnabled(false);
        LamaPon::SetActivePhysicsSettings(settings.physics);

        // C++ Script（Game Module）を読み込みます。**シーンを読む前**に
        // 用意しないと、NativeScriptComponentが型を見つけられません。
        //
        // これが無いと「Scriptで画面を組み立てるゲーム」は1行も動かず、
        // 真っ黒な絵が撮れてしまいます（しかも原因が出ません）。
        // AIがrenderで自分の成果を確かめられるようにするため、
        // ここはApplicationと同じようにロードします。
        LamaPon::GameModuleHost gameModule;
        const auto gameModulePath = projectRoot
            / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        bool gameModuleLoaded = false;
        std::string gameModuleError;
        if (std::filesystem::exists(gameModulePath))
        {
            gameModuleLoaded =
                gameModule.Load(gameModulePath);
            if (!gameModuleLoaded)
            {
                gameModuleError = gameModule.LastError();
                LamaPon::Logger::Instance().Warning(
                    "Game Moduleを読み込めませんでした。C++ Script"
                    "は動きません: " + gameModuleError);
            }
        }
        else
        {
            gameModuleError =
                "Game Moduleがありません（先にbuildしてください）: "
                + LamaPon::PathToUtf8(gameModulePath);
            // Scriptを使っていないプロジェクトでは正常なので、
            // 情報として残すだけにします。
            LamaPon::Logger::Instance().Info(gameModuleError);
        }

        LamaPon::Scene scene(graphics);
        scene.SetRegisteredTags(settings.tags);
        if (!scene.Scenes().RequestLoad(scenePath))
        {
            throw std::runtime_error(
                scene.Scenes().LastError());
        }
        // RequestLoadは要求を積むだけです（実際の読み込みは毎フレームの
        // ポンプが行う）。CLIはゲームループを回さないので、ここで
        // 自分でポンプします。忘れると**空のシーンを撮ってしまい**、
        // しかもエラーは出ません。
        if (!scene.Scenes().ProcessPending())
        {
            throw std::runtime_error(
                scene.Scenes().LastError().empty()
                    ? "The scene did not finish loading."
                    : scene.Scenes().LastError());
        }
        if (scene.MainCamera() == nullptr)
        {
            // 止めはしません。カメラ無しのスクリーンショット
            // （＝クリア色一色）も「そういう状態だ」という情報です。
            LamaPon::Logger::Instance().Warning(
                "シーンにメインカメラがありません。"
                "画面はクリア色のままになります。");
        }

        // C++ Scriptがあるかどうか。`Start()`は最初の更新で呼ばれるため、
        // Scriptで画面を組み立てるゲームは1回も更新しないと空のままです。
        bool sceneHasScripts = false;
        for (const auto& gameObject : scene.GameObjects())
        {
            if (gameObject != nullptr
                && gameObject->GetComponent<
                    LamaPon::NativeScriptComponent>()
                    != nullptr)
            {
                sceneHasScripts = true;
                break;
            }
        }

        // `--input`のAction名を、実際に押すControlへ解決します。
        // Actionは複数のBindingを持てるので、要求された向きと同じ
        // 符号の倍率が付いたものだけを押します（既定は正方向。
        // 押したつもりが逆に動くのを避けるためです）。
        //
        // `MoveHorizontal`のようにAとDが1つのActionへまとまっている
        // 場合、正方向だけでは**右にしか動かせません**。`=-1`を書くと
        // 負方向のBinding（A）を押せます。
        struct ResolvedInput final
        {
            std::vector<LamaPon::InputControl> controls;
            double from{};
            double to{};
            std::string action;
            double value{ 1.0 };
        };
        std::vector<ResolvedInput> resolvedInputs;
        double inputEnd = 0.0;
        for (const auto& event : options.inputEvents)
        {
            const auto found = std::find_if(
                settings.inputActions.begin(),
                settings.inputActions.end(),
                [&event](const auto& action)
                {
                    return action.name == event.action;
                });
            if (found == settings.inputActions.end())
            {
                throw std::runtime_error(
                    "--input names an action that is not in "
                    "the project settings: " + event.action);
            }
            ResolvedInput resolved;
            resolved.action = event.action;
            resolved.from = event.at;
            resolved.to = event.at
                + std::max(event.duration, 1.0 / 60.0);
            resolved.value = event.value < 0.0 ? -1.0 : 1.0;
            const bool wantsNegative = resolved.value < 0.0;
            for (const auto& binding : found->bindings)
            {
                if (wantsNegative
                    ? binding.scale < 0.0f
                    : binding.scale > 0.0f)
                {
                    resolved.controls.push_back(
                        binding.control);
                }
            }
            if (resolved.controls.empty())
            {
                throw std::runtime_error(
                    std::string{ "--input action has no " }
                    + (wantsNegative ? "negative" : "positive")
                    + " binding: " + event.action);
            }
            inputEnd = std::max(inputEnd, resolved.to);
            resolvedInputs.push_back(std::move(resolved));
        }

        // ゲーム時間を進める（任意）。物理・アニメーションが動きます。
        const float step = 1.0f / 60.0f;
        auto steps = static_cast<std::uint32_t>(
            std::max(options.simulateSeconds, 0.0) * 60.0);
        // `--input`だけ渡されたときは、その入力が効くところまで自動で
        // 進めます（`--simulate`を書き忘れて「何も起きない」結果に
        // なるのを防ぐため）。押し終わりから0.25秒だけ余韻を見ます。
        //
        // `--simulate`が明示されている場合は延ばしません。「押した
        // 直後の1コマを撮る」ような使い方を潰さないためです。
        if (!resolvedInputs.empty()
            && options.simulateSeconds <= 0.0)
        {
            const auto needed = static_cast<std::uint32_t>(
                (inputEnd + 0.25) * 60.0);
            steps = std::max(steps, needed);
        }
        // Scriptがあるなら最低1フレームは回します。そうしないと
        // `Start()`が呼ばれず、「Scriptで作った画面」が写りません。
        // Scriptが無いプロジェクトの挙動は今までと変わりません。
        if (sceneHasScripts && steps == 0u)
        {
            steps = 1u;
        }
        if (steps > 0u)
        {
            Progress(
                "simulate: "
                + std::to_string(steps) + " steps");
            for (std::uint32_t index = 0;
                index < steps;
                ++index)
            {
                // 押している時間帯なら、そのフレームのスナップショット
                // へControlを立てます。Actionの`WasPressed`は前フレーム
                // との差で決まるので、押した瞬間・離した瞬間も
                // ゲーム側から見えます。
                if (!resolvedInputs.empty())
                {
                    const double now =
                        static_cast<double>(index) / 60.0;
                    LamaPon::InputSnapshot snapshot;
                    for (const auto& resolved : resolvedInputs)
                    {
                        if (now < resolved.from
                            || now >= resolved.to)
                        {
                            continue;
                        }
                        for (const auto control :
                            resolved.controls)
                        {
                            snapshot.Set(control, 1.0f);
                        }
                    }
                    graphics.Input().UpdateFromSnapshot(
                        snapshot);
                }
                scene.Update(step);
            }
        }

        // Application.cppのゲーム実行と同じ経路で描きます。
        // ここが独自の呼び方だと「CLIだけ通っていない経路」が
        // できて、撮った絵が本物と食い違います。
        const float clearColor[4]{
            0.025f, 0.035f, 0.055f, 1.0f };
        std::vector<std::uint8_t> pixels;
        std::uint32_t capturedWidth{};
        std::uint32_t capturedHeight{};
        auto previousPresentation =
            std::chrono::steady_clock::now();
        for (std::uint32_t frame = 0;
            frame < std::max(options.frames, 1u);
            ++frame)
        {
            const auto frameStart =
                std::chrono::steady_clock::now();
            // 撮る1フレームだけを測ります。ウォームアップ分まで
            // 数えると「4フレーム描いたので4倍」になり、壊れた
            // オブジェクトの数と読み違えるためです。
            if (frame + 1 >= std::max(options.frames, 1u))
            {
                graphics.ResetShaderFallbackDraws();
            }
            graphics.BeginFrame(clearColor);
            graphics.BeginSceneComposition(clearColor);
            scene.RenderMainCamera(
                graphics.AspectRatio(),
                false,
                graphics.SceneCompositionTarget());
            graphics.EndSceneComposition(
                scene.PostProcessFrameData());
            scene.Render2D();
            if (frame + 1 >= std::max(options.frames, 1u))
            {
                pixels = graphics.CaptureBackBuffer(
                    capturedWidth,
                    capturedHeight);
            }
            graphics.EndFrame();
            const auto presentation =
                std::chrono::steady_clock::now();
            graphics.RecordFrameStatistics(
                std::chrono::duration<float>(
                    presentation - previousPresentation).count(),
                std::chrono::duration<float, std::milli>(
                    presentation - frameStart).count());
            previousPresentation = presentation;

            if (frame + 1 < std::max(options.frames, 1u))
            {
                // 通常のゲームループでは、提示したフレームの統計を
                // 次のUpdateでScriptが観測できます。CLIも同じ順序に
                // しないと、提示完了後に切り替えるTextの二重バッファが
                // 永久に未確定になります。0秒なのでゲーム内時間や
                // 物理は進めません。
                LamaPon::Time::Detail::AdvanceFrame(0.0f);
                scene.Update(0.0f);
            }
        }

        const auto outputPng =
            std::filesystem::absolute(options.outputPng);
        {
            std::error_code error;
            std::filesystem::create_directories(
                outputPng.parent_path(),
                error);
        }
        LamaPon::SavePng(
            outputPng,
            capturedWidth,
            capturedHeight,
            pixels);
        Progress(
            "png: " + LamaPon::PathToUtf8(outputPng));

        const auto summary = Summarize(
            capturedWidth,
            capturedHeight,
            pixels);
        std::size_t errorCount{};
        std::size_t warningCount{};
        auto logs = CollectLogs(errorCount, warningCount);
        // 描画の後で集めること（Shaderは初回描画時にコンパイル
        // されるため。CollectProblemsのコメントを参照）。
        auto problems = CollectProblems(scene);

        const nlohmann::json report{
            { "ok", true },
            { "command", "render" },
            { "project",
                LamaPon::PathToUtf8(projectRoot) },
            { "scene", LamaPon::PathToUtf8(scenePath) },
            { "frames", std::max(options.frames, 1u) },
            { "simulatedSeconds",
                static_cast<double>(steps) / 60.0 },
            // C++ Scriptが動く状態で撮れたかどうか。falseなら
            // 「Scriptが作る絵」は写っていません（原因はreason）。
            { "gameModule", {
                { "loaded", gameModuleLoaded },
                { "sceneHasScripts", sceneHasScripts },
                { "path",
                    LamaPon::PathToUtf8(gameModulePath) },
                { "reason", gameModuleError } } },
            { "inputEvents", [&resolvedInputs]
                {
                    nlohmann::json events = nlohmann::json::array();
                    for (const auto& resolved : resolvedInputs)
                    {
                        events.push_back({
                            { "action", resolved.action },
                            { "from", resolved.from },
                            { "to", resolved.to },
                            { "value", resolved.value } });
                    }
                    return events;
                }() },
            { "image", {
                { "path",
                    LamaPon::PathToUtf8(outputPng) },
                { "width", capturedWidth },
                { "height", capturedHeight },
                { "meanColor", {
                    summary.meanRed,
                    summary.meanGreen,
                    summary.meanBlue } },
                { "uniqueColors", summary.uniqueColors },
                { "magentaPixels",
                    summary.magentaPixels },
            } },
            // 壊れたシェーダーの代役が使われた回数。**0なら一度も
            // 使われていない**と言い切れます（エンジン自身が数えた
            // 事実で、image.magentaPixelsのような色からの推測では
            // ありません）。撮った1フレーム分だけを数えます。
            { "shaderFallbackDraws",
                graphics.FrameStats()
                    .shaderFallbackDraws },
            // 壊れているものの一覧（オブジェクト名・種類・対象・
            // 詳細）。AIはここが空になるまで直せばよい。
            { "problems", std::move(problems) },
            { "errorCount", errorCount },
            { "warningCount", warningCount },
            { "logs", std::move(logs) },
        };
        // 不正なUTF-8が混ざってもJSONを壊さず置換します
        // （stdoutの約束を守るため）。
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    // プロジェクトの新規作成。LamaPon Hubの「新規作成」と同じ
    // 雛形（起動シーン＋組み込みShader一式）を作ります。
    struct NewOptions final
    {
        std::filesystem::path directory;
        // 空ならフォルダー名を使います。
        std::string name;
        LamaPon::Hub::ProjectTemplate projectTemplate{
            LamaPon::Hub::ProjectTemplate::LearningThreeDimensional
        };
        bool allowInsideEngineSource{};
    };

    [[nodiscard]] std::string ProjectTemplateName(
        const LamaPon::Hub::ProjectTemplate projectTemplate)
    {
        switch (projectTemplate)
        {
        case LamaPon::Hub::ProjectTemplate::LearningTwoDimensional:
            return "learning-2d";
        case LamaPon::Hub::ProjectTemplate::LearningThreeDimensional:
            return "learning-3d";
        case LamaPon::Hub::ProjectTemplate::TwoDimensional:
            return "2d";
        case LamaPon::Hub::ProjectTemplate::ThreeDimensional:
        default:
            return "3d";
        }
    }

    [[nodiscard]] int RunNew(const NewOptions& options)
    {
        const auto projectRoot =
            std::filesystem::absolute(options.directory)
                .lexically_normal();
        const std::string name =
            options.name.empty()
                ? LamaPon::PathToUtf8(
                    projectRoot.filename())
                : options.name;
        LamaPon::Hub::CreateProject(
            projectRoot,
            name,
            options.projectTemplate,
            options.allowInsideEngineSource);
        // Hubの「最近使ったプロジェクト」にも載せます。CLIで作った
        // プロジェクトを、人がHubから開けるようにするためです。
        LamaPon::Hub::AddRecentProject(projectRoot);
        Progress(
            "created: "
            + LamaPon::PathToUtf8(projectRoot));

        std::size_t errorCount{};
        std::size_t warningCount{};
        auto logs = CollectLogs(errorCount, warningCount);
        const nlohmann::json report{
            { "ok", true },
            { "command", "new" },
            { "project",
                LamaPon::PathToUtf8(projectRoot) },
            { "name", name },
            { "template", ProjectTemplateName(
                options.projectTemplate) },
            { "learningJourney",
                LamaPon::Hub::HasLearningJourney(projectRoot) },
            // 作った直後のrender用。ここを読めば次のコマンドが組める。
            { "startupScene", "scenes/Main.scene.json" },
            { "errorCount", errorCount },
            { "warningCount", warningCount },
            { "logs", std::move(logs) },
        };
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    [[nodiscard]] nlohmann::json LearningStepJson(
        const LamaPon::Hub::LearningStep& step,
        const bool completed)
    {
        return {
            { "id", step.id },
            { "phase", step.phase },
            { "phaseDisplay",
                LamaPon::Hub::LearningPhaseDisplayName(step.phase) },
            { "title", step.title },
            { "purpose", step.purpose },
            { "action", step.action },
            { "success", step.success },
            { "role", step.role },
            { "roleDisplay",
                LamaPon::Hub::LearningRoleDisplayName(step.role) },
            { "estimatedMinutes", step.estimatedMinutes },
            { "files", step.files },
            { "completed", completed }
        };
    }

    [[nodiscard]] nlohmann::json LearningStatusJson(
        const std::filesystem::path& projectRoot,
        const std::string& command)
    {
        const auto journey =
            LamaPon::Hub::LoadLearningJourney(projectRoot);
        const auto progress =
            LamaPon::Hub::LoadLearningProgress(projectRoot);
        const auto status =
            LamaPon::Hub::GetLearningStatus(projectRoot);
        const std::unordered_set<std::string> completed{
            progress.completedStepIds.begin(),
            progress.completedStepIds.end()
        };

        nlohmann::json steps = nlohmann::json::array();
        std::uint32_t remainingMinutes{};
        for (const auto& step : journey.steps)
        {
            const bool isCompleted = completed.contains(step.id);
            steps.push_back(LearningStepJson(step, isCompleted));
            if (!isCompleted)
            {
                remainingMinutes += step.estimatedMinutes;
            }
        }
        nlohmann::json nextStep = nullptr;
        if (status.nextStep.has_value())
        {
            nextStep = LearningStepJson(*status.nextStep, false);
        }
        const std::size_t percent = status.totalSteps == 0
            ? 0
            : status.completedSteps * 100 / status.totalSteps;
        return {
            { "ok", true },
            { "command", command },
            { "project", LamaPon::PathToUtf8(projectRoot) },
            { "journey", {
                { "title", journey.title },
                { "concept", journey.conceptText }
            } },
            { "progress", {
                { "completed", status.completedSteps },
                { "total", status.totalSteps },
                { "percent", percent },
                { "remainingMinutes", remainingMinutes },
                { "selectedRole", status.selectedRole },
                { "selectedRoleDisplay",
                    LamaPon::Hub::LearningRoleDisplayName(
                        status.selectedRole) }
            } },
            { "nextStep", std::move(nextStep) },
            { "steps", std::move(steps) },
            { "guide", LamaPon::PathToUtf8(
                projectRoot / L"LEARNING.md") },
            { "progressFile", LamaPon::PathToUtf8(
                LamaPon::Hub::LearningProgressPath(projectRoot)) }
        };
    }

    [[nodiscard]] int RunLearn(
        const std::wstring_view action,
        const std::filesystem::path& requestedProject,
        const std::string& requestedStep,
        const std::string& requestedRole)
    {
        const auto projectRoot =
            CanonicalProjectRoot(requestedProject);

        if (action == L"doctor")
        {
            auto diagnosis =
                LamaPon::Hub::DiagnoseLearningJourney(projectRoot);
            const auto module =
                LamaPon::InspectGameModuleBuildState(projectRoot);
            if (module.hasSources)
            {
                diagnosis.checks.push_back({
                    "game-module",
                    "C++ Game Module",
                    module.outputExists && !module.buildRequired,
                    false,
                    !module.outputExists
                        ? "まだ初回ビルドされていません。learnは続行できますが、C++の反応にはbuildが必要です。"
                        : module.buildRequired
                            ? "C++ソースがDLLより新しいため再ビルドが必要です。"
                            : "C++ Scriptは最新のDLLへビルド済みです。"
                });
            }
            nlohmann::json checks = nlohmann::json::array();
            for (const auto& check : diagnosis.checks)
            {
                checks.push_back({
                    { "id", check.id },
                    { "label", check.label },
                    { "ok", check.ok },
                    { "required", check.required },
                    { "detail", check.detail }
                });
            }
            const nlohmann::json report{
                { "ok", diagnosis.ready },
                { "command", "learn doctor" },
                { "project", LamaPon::PathToUtf8(projectRoot) },
                { "ready", diagnosis.ready },
                { "checks", std::move(checks) },
                { "nextCommand", diagnosis.ready
                    ? module.buildRequired
                        ? "LamaPonCli build --project <dir>"
                        : "LamaPonCli learn status --project <dir>"
                    : "LamaPonCli learn init --project <dir>" }
            };
            std::cout << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
                << std::endl;
            return diagnosis.ready ? 0 : 1;
        }

        bool initialized = false;
        if (action == L"init")
        {
            if (!LamaPon::Hub::HasLearningJourney(projectRoot))
            {
                LamaPon::Hub::InitializeLearningJourney(projectRoot);
                initialized = true;
            }
        }
        else if (!LamaPon::Hub::HasLearningJourney(projectRoot))
        {
            throw std::runtime_error(
                "This project has no learning journey. Run:"
                " LamaPonCli learn init --project <dir>");
        }

        std::string completedStep;
        if (action == L"complete")
        {
            completedStep = requestedStep;
            if (completedStep.empty())
            {
                const auto status =
                    LamaPon::Hub::GetLearningStatus(projectRoot);
                if (status.nextStep.has_value())
                {
                    completedStep = status.nextStep->id;
                }
            }
            if (!completedStep.empty())
            {
                LamaPon::Hub::CompleteLearningStep(
                    projectRoot,
                    completedStep);
            }
        }
        else if (action == L"role")
        {
            if (requestedRole.empty())
            {
                throw std::invalid_argument(
                    "learn role requires --role.");
            }
            LamaPon::Hub::SetLearningRole(
                projectRoot,
                requestedRole);
        }
        else if (action == L"reset")
        {
            LamaPon::Hub::ResetLearningProgress(projectRoot);
        }
        else if (action != L"status" && action != L"init")
        {
            throw std::invalid_argument(
                "learn requires status, init, complete, role, reset, or doctor.");
        }

        auto report = LearningStatusJson(
            projectRoot,
            "learn " + LamaPon::PathToUtf8(
                std::filesystem::path(action)));
        if (action == L"init")
        {
            report["initialized"] = initialized;
        }
        if (action == L"complete")
        {
            report["completedStep"] = completedStep.empty()
                ? nlohmann::json(nullptr)
                : nlohmann::json(completedStep);
        }
        if (action == L"reset")
        {
            report["reset"] = true;
        }
        std::cout << report.dump(
            2,
            ' ',
            false,
            nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    // C++ Game Moduleのビルド。エディターの「保存→自動ビルド」と
    // 同じコマンド（GameModuleBuilder）を、こちらは同期で実行します。
    struct BuildOptions final
    {
        std::filesystem::path projectRoot;
        // Release / Debug。既定はこのCLI自身と同じ構成です
        // （Runtimeのインポートライブラリと合わせるため）。
        std::string configuration{
            LAMAPON_BUILD_CONFIGURATION };
    };

    // バイト列が正しいUTF-8かどうか（ASCIIのみでもtrue）。
    [[nodiscard]] bool LooksLikeValidUtf8(
        const std::string_view bytes) noexcept
    {
        std::size_t index = 0;
        while (index < bytes.size())
        {
            const auto lead =
                static_cast<unsigned char>(bytes[index]);
            std::size_t continuation = 0;
            if (lead < 0x80)
            {
                continuation = 0;
            }
            else if ((lead & 0xE0) == 0xC0)
            {
                continuation = 1;
            }
            else if ((lead & 0xF0) == 0xE0)
            {
                continuation = 2;
            }
            else if ((lead & 0xF8) == 0xF0)
            {
                continuation = 3;
            }
            else
            {
                return false;
            }
            if (index + continuation >= bytes.size()
                && continuation > 0)
            {
                return false;
            }
            for (std::size_t offset = 1;
                offset <= continuation;
                ++offset)
            {
                if ((static_cast<unsigned char>(
                        bytes[index + offset])
                    & 0xC0) != 0x80)
                {
                    return false;
                }
            }
            index += continuation + 1;
        }
        return true;
    }

    // ANSIコードページ（日本語環境ならCP932）の1行をUTF-8へ。
    [[nodiscard]] std::string AcpToUtf8(
        const std::string_view bytes)
    {
        if (bytes.empty())
        {
            return {};
        }
        const int wideLength = MultiByteToWideChar(
            CP_ACP,
            0,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0);
        if (wideLength <= 0)
        {
            return {};
        }
        std::wstring wide(
            static_cast<std::size_t>(wideLength),
            L'\0');
        MultiByteToWideChar(
            CP_ACP,
            0,
            bytes.data(),
            static_cast<int>(bytes.size()),
            wide.data(),
            wideLength);
        return LamaPon::WideToUtf8(wide);
    }

    // ビルドログをUTF-8で読みます。
    //
    // ログは**混在エンコーディング**です（VMで実測）: NMAKEやcl.exeの
    // 直接の出力はANSIコードページ（CP932）ですが、CMakeの依存関係
    // スキャナー経由で再出力されるコンパイルエラーはUTF-8で届きます。
    // 全体を一括でCP932変換すると、UTF-8の行が「讒区枚繧ｨ繝ｩ繝ｼ」の
    // ような化け方をするため、**行ごとに**判定して変換します。
    [[nodiscard]] std::string ReadBuildLog(
        const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        const std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        std::string result;
        result.reserve(bytes.size());
        std::size_t begin = 0;
        while (begin <= bytes.size())
        {
            std::size_t end = bytes.find('\n', begin);
            const bool last = end == std::string::npos;
            if (last)
            {
                end = bytes.size();
            }
            std::string_view line{
                bytes.data() + begin,
                end - begin };
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (LooksLikeValidUtf8(line))
            {
                result.append(line);
            }
            else
            {
                result.append(AcpToUtf8(line));
            }
            if (last)
            {
                break;
            }
            result.push_back('\n');
            begin = end + 1;
        }
        return result;
    }

    // ログから「error」を含む行を抜き出します（cl.exeの
    // 「error C2065」、リンクの「error LNK2019」、CMakeの
    // 「CMake Error」を拾う）。AIはこの配列だけ読めばよく、
    // ログ全文を漁らずに済みます。
    [[nodiscard]] nlohmann::json ExtractErrorLines(
        const std::string& log)
    {
        auto lines = nlohmann::json::array();
        std::size_t begin = 0;
        while (begin < log.size() && lines.size() < 50)
        {
            std::size_t end = log.find('\n', begin);
            if (end == std::string::npos)
            {
                end = log.size();
            }
            std::string line =
                log.substr(begin, end - begin);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (LamaPon::Cli::IsBuildErrorLine(line))
            {
                lines.push_back(line);
            }
            begin = end + 1;
        }
        return lines;
    }

    [[nodiscard]] int RunBuild(const BuildOptions& options)
    {
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    options.projectRoot));
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }

        // エンジンルートの解決はエディター（Sandbox/Main.cpp）と
        // 同じです: 配布パッケージならexeの隣、開発中ならソースツリー。
        auto engineRoot = std::filesystem::path{
            LAMAPON_DEFAULT_PROJECT_ROOT
        };
        const auto installedEngineRoot =
            LamaPon::ExecutableDirectory();
        if (std::filesystem::is_regular_file(
                installedEngineRoot
                    / L"tools"
                    / L"ProjectGameModule"
                    / L"CMakeLists.txt"))
        {
            engineRoot = installedEngineRoot;
        }

        const auto buildCommand =
            LamaPon::MakeGameModuleBuildCommand(
                projectRoot,
                engineRoot,
                LamaPon::ExecutableDirectory(),
                options.configuration);
        Progress(
            "build: "
            + LamaPon::PathToUtf8(
                buildCommand.outputModule));
        // WebDAV上のプロジェクトはソースのmtimeが古いまま見えることが
        // あり、NMakeが変更を見失う（旧DLLのまま「成功」）。内容ハッシュで
        // 検出して時刻を進める。進めた数は結果JSONで報告する。
        const int touchedStaleSources =
            LamaPon::RefreshStaleGameModuleSources(
                projectRoot,
                buildCommand.buildDirectory);
        // ビルド前のDLLの時刻。「今回のビルドで実際に更新されたか」を
        // 後で判定するため（失敗しても前回のDLLは残るので、存在だけ
        // では紛らわしい）。
        std::error_code timeError;
        const auto moduleTimeBefore =
            std::filesystem::last_write_time(
                buildCommand.outputModule,
                timeError);

        // cmd.exeを同期実行します（エディターは非同期ですが、CLIは
        // 結果をJSONで返すので終わるまで待ちます）。
        std::wstring comSpec(MAX_PATH, L'\0');
        const DWORD comSpecLength =
            GetEnvironmentVariableW(
                L"ComSpec",
                comSpec.data(),
                MAX_PATH);
        comSpec.resize(
            comSpecLength > 0 && comSpecLength < MAX_PATH
                ? comSpecLength
                : 0);
        if (comSpec.empty())
        {
            comSpec = L"C:\\Windows\\System32\\cmd.exe";
        }
        // CreateProcessWはコマンドラインを書き換えることがあるので
        // 可変のバッファで渡します。
        std::wstring commandLine =
            L"\"" + comSpec + L"\" "
            + buildCommand.parameters;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                comSpec.c_str(),
                commandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                projectRoot.c_str(),
                &startup,
                &process))
        {
            throw std::runtime_error(
                "Could not start the build process.");
        }
        CloseHandle(process.hThread);
        // 初回はCMakeの構成から走るので長めに待ちます。
        const DWORD waitResult = WaitForSingleObject(
            process.hProcess,
            15u * 60u * 1000u);
        if (waitResult != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hProcess);
            throw std::runtime_error(
                "The build did not finish within 15"
                " minutes. See the log: "
                + LamaPon::PathToUtf8(
                    buildCommand.logPath));
        }
        DWORD exitCode = 1;
        static_cast<void>(GetExitCodeProcess(
            process.hProcess,
            &exitCode));
        CloseHandle(process.hProcess);

        const std::string log =
            ReadBuildLog(buildCommand.logPath);
        auto buildErrors = ExtractErrorLines(log);
        // Z:(WebDAV)はコピー完了直後のstatが一時的に失敗することがある
        // （実在するのに moduleExists:false → ビルド失敗と誤報した前例
        // 2026-08-13。HANDOFF-ENGINE §4-12のrename問題と同族）。
        // ビルドが成功を主張しているときだけ、少し待って再確認する。
        std::error_code existsError;
        bool moduleExists =
            std::filesystem::is_regular_file(
                buildCommand.outputModule,
                existsError);
        for (int attempt = 0;
            !moduleExists && exitCode == 0 && attempt < 10;
            ++attempt)
        {
            Sleep(200);
            existsError.clear();
            moduleExists =
                std::filesystem::is_regular_file(
                    buildCommand.outputModule,
                    existsError);
        }
        // NMakeが「変更なし」と判断してexit 0でも、Engine更新後の古い
        // DLLが残ることがあります。Runtimeが実際に起動時拒否する状態を
        // build成功として返さないよう、同じ時刻条件をここでも確認します。
        bool runtimeCompatible = moduleExists;
        std::error_code runtimeTimeError;
        const auto runtimePath =
            LamaPon::ExecutableDirectory()
            / L"LamaPonRuntime.dll";
        const auto runtimeTime = std::filesystem::last_write_time(
            runtimePath,
            runtimeTimeError);
        std::error_code compatibilityModuleTimeError;
        const auto compatibilityModuleTime =
            std::filesystem::last_write_time(
                buildCommand.outputModule,
                compatibilityModuleTimeError);
        if (runtimeTimeError || compatibilityModuleTimeError
            || compatibilityModuleTime < runtimeTime)
        {
            runtimeCompatible = false;
            buildErrors.push_back(
                "Game Module is still older than LamaPonRuntime.dll;"
                " the build did not relink the module.");
        }
        // 時刻だけでは足りません。NMakeはヘッダ依存の追跡が
        // 不完全で、GameModuleApiVersion を上げてもそれを埋め込んだ
        // objを再コンパイルせず、他のobjだけでリンクします。
        // するとDLLの時刻は新しくなるのに中身は古い版数のままに
        // なり、ここで runtimeCompatible:true を返してしまいます
        // （2026-08-18にCarGameで発生。エディターの再生が背景だけに
        // なったのに、buildは成功と言い続けた）。
        // 実際にDLLへ聞いて、嫌でも嘲かないようにします。
        if (runtimeCompatible)
        {
            const auto builtApiVersion =
                LamaPon::ReadGameModuleApiVersion(
                    buildCommand.outputModule);
            if (!builtApiVersion.has_value())
            {
                runtimeCompatible = false;
                buildErrors.push_back(
                    "Game Module could not be inspected after the"
                    " build; it may be corrupt or missing"
                    " LamaPonGetGameModule.");
            }
            else if (*builtApiVersion
                != LamaPon::GameModuleApiVersion)
            {
                runtimeCompatible = false;
                buildErrors.push_back(
                    "Game Module was built for API version "
                    + std::to_string(*builtApiVersion)
                    + " but this engine requires "
                    + std::to_string(
                        LamaPon::GameModuleApiVersion)
                    + ". The build directory is stale; delete \""
                    + LamaPon::PathToUtf8(
                        buildCommand.buildDirectory)
                    + "\" and build again.");
            }
        }
        // 終了コード0でもDLLが無い／古ければ成功とは言いません
        // （「exit=0を信じて古いバイナリを検証し続けた」事故の再発防止）。
        const bool ok = exitCode == 0
            && moduleExists
            && runtimeCompatible;
        // 今回のビルドでDLLが実際に書き換わったか。失敗時に前回の
        // 古いDLLを「ある」と読んで載せてしまう誤解を防ぎます。
        // 変更が無くup-to-dateだった成功ビルドでもfalseになります
        // （その場合は前回の成果物のままで正しい）。
        std::error_code afterError;
        const auto moduleTimeAfter =
            std::filesystem::last_write_time(
                buildCommand.outputModule,
                afterError);
        const bool moduleUpdated =
            moduleExists
            && !afterError
            && (timeError
                || moduleTimeAfter != moduleTimeBefore);

        nlohmann::json report{
            { "ok", ok },
            { "command", "build" },
            { "project",
                LamaPon::PathToUtf8(projectRoot) },
            { "configuration", options.configuration },
            { "module",
                LamaPon::PathToUtf8(
                    buildCommand.outputModule) },
            { "moduleExists", moduleExists },
            { "moduleUpdated", moduleUpdated },
            { "touchedStaleSources", touchedStaleSources },
            { "runtimeCompatible", runtimeCompatible },
            { "buildDirectory",
                LamaPon::PathToUtf8(
                    buildCommand.buildDirectory) },
            { "usesLocalBuildCache",
                buildCommand.usesLocalBuildCache },
            { "exitCode", exitCode },
            { "logPath",
                LamaPon::PathToUtf8(
                    buildCommand.logPath) },
            { "buildErrors", std::move(buildErrors) },
        };
        if (!ok)
        {
            // 失敗時はログの末尾も付けます。errorという語を含まない
            // 失敗（ツール自体が見つからない等）への保険です。
            constexpr std::size_t tailLimit = 4000;
            report["logTail"] =
                log.size() > tailLimit
                    ? log.substr(log.size() - tailLimit)
                    : log;
        }
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return ok ? 0 : 1;
    }

    // 配布用のゲーム書き出し。エディターの「エクスポート」と同じ
    // 梱包処理（ExportGamePackage）を呼びます。
    struct CliExportOptions final
    {
        std::filesystem::path projectRoot;
        // 空なら <プロジェクト>/export へ書き出します。
        std::filesystem::path outputDirectory;
        bool zip{};
    };

    [[nodiscard]] int RunExport(
        const CliExportOptions& options)
    {
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    options.projectRoot));
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        const LamaPon::ProjectSettings settings =
            LamaPon::LoadProjectSettings(settingsPath);

        // エディターと同じ場所からGame Moduleを拾います。
        // 無くても書き出しは成立します（C++ Script未使用の
        // プロジェクト）が、「入っていると思っていたのに入って
        // いない」が最悪なので、有無をJSONへ明示します。
        const auto gameModulePath = projectRoot
            / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        const bool gameModuleIncluded =
            std::filesystem::is_regular_file(
                gameModulePath);
        if (!gameModuleIncluded)
        {
            LamaPon::Logger::Instance().Warning(
                "Game Moduleが見つからないため、C++ Script"
                "無しで書き出します: "
                + LamaPon::PathToUtf8(gameModulePath));
        }

        LamaPon::GameExportOptions exportOptions{
            LamaPon::ExecutableDirectory(),
            projectRoot / L"assets",
            options.outputDirectory.empty()
                ? projectRoot / L"export"
                : std::filesystem::absolute(
                    options.outputDirectory),
            settings,
            gameModulePath
        };
        exportOptions.createZipArchive = options.zip;
        Progress(
            "export: "
            + LamaPon::PathToUtf8(
                exportOptions.outputDirectory));
        const LamaPon::GameExportResult result =
            LamaPon::ExportGamePackage(exportOptions);

        std::size_t errorCount{};
        std::size_t warningCount{};
        auto logs = CollectLogs(errorCount, warningCount);
        const nlohmann::json report{
            { "ok", true },
            { "command", "export" },
            { "project",
                LamaPon::PathToUtf8(projectRoot) },
            { "output", {
                { "directory",
                    LamaPon::PathToUtf8(
                        result.outputDirectory) },
                { "executable",
                    LamaPon::PathToUtf8(
                        result.executablePath) },
                { "zip",
                    LamaPon::PathToUtf8(
                        result.zipPath) },
                { "totalBytes", result.totalBytes },
                { "fileCount", result.fileCount },
            } },
            { "gameModuleIncluded", gameModuleIncluded },
            { "errorCount", errorCount },
            { "warningCount", warningCount },
            { "logs", std::move(logs) },
        };
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 0;
    }

    void PrintUsage()
    {
        std::cerr <<
            "LamaPonCli - LamaPon projects without the"
            " editor GUI\n"
            "\n"
            "usage:\n"
            "  LamaPonCli render --project <dir> [options]\n"
            "  LamaPonCli new --dir <dir> [options]\n"
            "  LamaPonCli build --project <dir> [options]\n"
            "  LamaPonCli export --project <dir> [options]\n"
            "  LamaPonCli learn status --project <dir>\n"
            "  LamaPonCli learn init --project <dir>\n"
            "  LamaPonCli learn complete --project <dir> [--step <id>]\n"
            "  LamaPonCli learn role --project <dir> --role <role>\n"
            "  LamaPonCli learn reset --project <dir>\n"
            "  LamaPonCli learn doctor --project <dir>\n"
            "  LamaPonCli project inspect --project <dir>\n"
            "  LamaPonCli project list\n"
            "  LamaPonCli project add --project <dir>\n"
            "  LamaPonCli project remove --project <dir>\n"
            "  LamaPonCli project move --project <dir> --to <dir>\n"
            "  LamaPonCli asset list --project <dir> [options]\n"
            "  LamaPonCli asset inspect --project <dir> [options]\n"
            "  LamaPonCli asset import --project <dir> --source <file> [options]\n"
            "  LamaPonCli script list --project <dir>\n"
            "  LamaPonCli script inspect --project <dir> --path <file>\n"
            "  LamaPonCli script create --project <dir> --path <file> --class <name>\n"
            "  LamaPonCli component list [options]\n"
            "  LamaPonCli component schema --type <type>\n"
            "  LamaPonCli prefab inspect --project <dir> --path <file>\n"
            "  LamaPonCli prefab validate --project <dir> --path <file>\n"
            "  LamaPonCli prefab patch --project <dir> --path <file> --operations <file>\n"
            "  LamaPonCli inspect --project <dir> [options]\n"
            "  LamaPonCli validate --project <dir> [options]\n"
            "  LamaPonCli patch --project <dir> --operations <file> [options]\n"
            "  LamaPonCli test --project <dir> --spec <file> [options]\n"
            "  LamaPonCli job start <operation> [options]\n"
            "  LamaPonCli job status --project <dir> --id <jobId>\n"
            "  LamaPonCli job cancel --project <dir> --id <jobId>\n"
            "  LamaPonCli job list --project <dir>\n"
            "  LamaPonCli runtime start --project <dir> [options]\n"
            "  LamaPonCli runtime status --project <dir> --id <sessionId>\n"
            "  LamaPonCli runtime send --project <dir> --id <sessionId>\n"
            "  LamaPonCli runtime stop --project <dir> --id <sessionId>\n"
            "  LamaPonCli runtime test --project <dir> --spec <file>\n"
            "  LamaPonCli runtime replay --project <dir> --file <file>\n"
            "  LamaPonCli runtime recover --project <dir> --id <sessionId>\n"
            "\n"
            "render options:\n"
            "  --project <dir>   LamaPon project root"
            " (required)\n"
            "  --scene <path>    scene to shoot (default:"
            " the project's startup scene)\n"
            "  --out <file.png>  output image (default:"
            " render.png)\n"
            "  --width <n>       override the resolution\n"
            "  --height <n>\n"
            "  --frames <n>      frames to render before"
            " the capture (default: 4)\n"
            "  --simulate <sec>  advance game time before"
            " the capture (default: 0)\n"
            "  --input <events>  press input actions while"
            " simulating,\n"
            "                    as Action@seconds[:hold]"
            " separated by commas\n"
            "                    (for example:"
            " --input \"Jump@0.5:0.2,Fire@1.0\")\n"
            "  --warp            render on the CPU (WARP)\n"
            "  --d3ddebug        enable the D3D11 debug"
            " layer\n"
            "\n"
            "new options:\n"
            "  --dir <dir>       folder to create the"
            " project in (required, must be empty)\n"
            "  --name <name>     game name (default: the"
            " folder name)\n"
            "  --template <t>    3d, 2d, learning-3d, or learning-2d"
            " (default: learning-3d)\n"
            "  --allow-inside-engine  explicit engine sample override\n"
            "\n"
            "build options:\n"
            "  --project <dir>   LamaPon project root"
            " (required)\n"
            "  --config <c>      Release or Debug"
            " (default: same as this tool)\n"
            "\n"
            "export options:\n"
            "  --project <dir>   LamaPon project root"
            " (required)\n"
            "  --out <dir>       output folder (default:"
            " <project>/export)\n"
            "  --zip             also create a"
            " distribution ZIP\n"
            "\n"
            "learn options:\n"
            "  status              show progress and the next action\n"
            "  init                add learning files to an existing project\n"
            "  complete            complete --step, or the next step if omitted\n"
            "  role --role <r>     undecided, engineer, planner, or designer\n"
            "  reset               remove only local learning progress\n"
            "  doctor              validate the curriculum and referenced files\n"
            "\n"
            "project/asset/script options:\n"
            "  --project <dir>   LamaPon project root"
            " (required)\n"
            "  asset list --importer <name>  filter by importer\n"
            "  asset inspect --path <file>   inspect by asset path\n"
            "  asset inspect --guid <guid>   inspect by GUID\n"
            "  asset import --source <file> [--source <file>...]\n"
            "  asset import --target <dir>  destination under assets/\n"
            "  script inspect --path <file>  inspect source code\n"
            "  script create --class <name> create a LamaPon::Script\n"
            "  script create --force        overwrite an existing file\n"
            "  component list --category <c> filter UI, Physics, or Animation\n"
            "  component schema --type <t>  inspect fields and defaults\n"
            "  prefab inspect/validate      read or validate a Prefab\n"
            "  prefab patch --dry-run       preview Prefab changes\n"
            "\n"
            "inspect/validate options:\n"
            "  --project <dir>   LamaPon project root"
            " (required)\n"
            "  --scene <path>    scene to inspect (default:"
            " the project's startup scene)\n"
            "\n"
            "patch options:\n"
            "  --project <dir>   LamaPon project root"
            " (required)\n"
            "  --scene <path>    scene to change (default:"
            " the project's startup scene)\n"
            "  --operations <f>  JSON patch operations file"
            " (required)\n"
            "  --out <file>      write to another file inside"
            " the project\n"
            "  --dry-run         report the result without"
            " writing a scene\n"
            "\n"
            "test options:\n"
            "  --project <dir>   LamaPon project root"
            " (required)\n"
            "  --scene <path>    scene to test (default:"
            " the project's startup scene)\n"
            "  --spec <file>     JSON assertions file"
            " (required)\n"
            "  --report <file>   also write the JSON report"
            " inside the project\n"
            "\n"
            "job options:\n"
            "  start <operation>  operation is build, render, export,"
            " inspect, validate, patch, or test;"
            " accepts that command's options\n"
            "  status/cancel      require --project and --id\n"
            "  list               requires --project\n"
            "  Job files are stored under <project>/.lamapon/jobs/<id>\n"
            "\n"
            "runtime options:\n"
            "  start               launch a standalone game session\n"
            "  status              read the latest runtime snapshot\n"
            "  send                send --command JSON or --command-file\n"
            "  stop                request a clean session shutdown\n"
            "  test                run a JSON playtest scenario\n"
            "  replay              replay a deterministic command recording\n"
            "  recover             restart a crashed or stopped session\n"
            "  --scene <path>      scene to load (start only)\n"
            "  --width/--height <n> window size (start only)\n"
            "  --fps <n>           target frame rate (default: 60)\n"
            "  --warp              use the CPU WARP renderer\n"
            "  --d3ddebug          enable the D3D11 debug layer\n"
            "  --deterministic     use a fixed simulation timestep\n"
            "  --fixed-delta <s>   fixed timestep in seconds\n"
            "  --render-every <n>  draw every N simulation frames\n"
            "  --no-pace           skip waits in deterministic mode\n"
            "  --record <file>     write a command replay recording\n"
            "  --spec <file>       playtest scenario JSON\n"
            "  --file <file>       replay JSON\n"
            "  --command <json>    JSON operation for send\n"
            "  --command-file <f>  read the operation from a JSON file\n"
            "Runtime files are stored under <project>/.lamapon/runtime/<id>\n"
            "\n"
            "stdout is a single JSON object."
            " Exit code 0 means success.\n";
    }
}

int wmain(const int argumentCount, wchar_t** arguments)
{
    // 日本語のログをコンソールでも化けさせないため。JSON自体は
    // 常にUTF-8です（ファイルやパイプで読む分には影響しません）。
    SetConsoleOutputCP(CP_UTF8);
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<void>(comResult);

    const std::wstring_view command =
        argumentCount >= 2 ? arguments[1] : L"";
    try
    {
        // オプション巡回の共通部品。値付きオプションの
        // 「次の引数」を安全に取ります。
        int index = 2;
        const auto next =
            [&index, argumentCount, arguments]()
                -> std::wstring
            {
                if (index + 1 >= argumentCount)
                {
                    throw std::invalid_argument(
                        LamaPon::PathToUtf8(
                            std::filesystem::path{
                                arguments[index] })
                        + " requires a value.");
                }
                return arguments[++index];
            };
        const auto unknownOption =
            [](const std::wstring_view argument)
                -> std::invalid_argument
            {
                return std::invalid_argument(
                    "Unknown option: "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path{
                        argument }));
            };

        if (command == L"learn")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "learn requires status, init, complete, role, reset, or doctor.");
            }
            const std::wstring_view action = arguments[2];
            std::filesystem::path projectRoot;
            std::string step;
            std::string role;
            index = 3;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument = arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--step")
                {
                    step = LamaPon::WideToUtf8(next());
                }
                else if (argument == L"--role")
                {
                    role = LamaPon::WideToUtf8(next());
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty())
            {
                throw std::invalid_argument(
                    "learn requires --project.");
            }
            if (!step.empty() && action != L"complete")
            {
                throw std::invalid_argument(
                    "--step is only valid for learn complete.");
            }
            if (!role.empty() && action != L"role")
            {
                throw std::invalid_argument(
                    "--role is only valid for learn role.");
            }
            return RunLearn(action, projectRoot, step, role);
        }

        if (command == L"runtime")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "runtime requires start, status, send, stop, or worker.");
            }
            const std::wstring action = arguments[2];
            RuntimeStartOptions startOptions;
            std::filesystem::path projectRoot;
            std::wstring sessionId;
            std::filesystem::path sessionDirectory;
            std::filesystem::path commandFile;
            std::filesystem::path specFile;
            std::filesystem::path replayFile;
            std::wstring commandText;

            index = 3;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument = arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                    startOptions.projectRoot = projectRoot;
                }
                else if (argument == L"--id")
                {
                    sessionId = next();
                }
                else if (argument == L"--session")
                {
                    sessionDirectory = next();
                }
                else if (argument == L"--scene")
                {
                    startOptions.scene = next();
                }
                else if (argument == L"--width")
                {
                    const auto value = std::stoi(next());
                    if (value <= 0)
                    {
                        throw std::invalid_argument(
                            "--width must be greater than zero.");
                    }
                    startOptions.width =
                        static_cast<std::uint32_t>(value);
                }
                else if (argument == L"--height")
                {
                    const auto value = std::stoi(next());
                    if (value <= 0)
                    {
                        throw std::invalid_argument(
                            "--height must be greater than zero.");
                    }
                    startOptions.height =
                        static_cast<std::uint32_t>(value);
                }
                else if (argument == L"--fps")
                {
                    const auto value = std::stoi(next());
                    if (value <= 0)
                    {
                        throw std::invalid_argument(
                            "--fps must be greater than zero.");
                    }
                    startOptions.targetFrameRate =
                        static_cast<std::uint32_t>(value);
                }
                else if (argument == L"--fixed-delta")
                {
                    const auto value = std::stof(next());
                    if (!std::isfinite(value) || value <= 0.0f)
                    {
                        throw std::invalid_argument(
                            "--fixed-delta must be finite and greater than zero.");
                    }
                    startOptions.fixedDeltaTime = value;
                }
                else if (argument == L"--warp")
                {
                    startOptions.warp = true;
                }
                else if (argument == L"--d3ddebug")
                {
                    startOptions.d3dDebug = true;
                }
                else if (argument == L"--deterministic")
                {
                    startOptions.deterministic = true;
                }
                else if (argument == L"--render-every")
                {
                    const auto value = std::stoul(next());
                    if (value == 0 || value > 100'000)
                    {
                        throw std::invalid_argument(
                            "--render-every must be between 1 and 100000.");
                    }
                    startOptions.renderEveryNFrames =
                        static_cast<std::uint32_t>(value);
                }
                else if (argument == L"--no-pace")
                {
                    startOptions.paceFrames = false;
                }
                else if (argument == L"--record")
                {
                    startOptions.recordPath = next();
                }
                else if (argument == L"--spec")
                {
                    specFile = next();
                }
                else if (argument == L"--file")
                {
                    replayFile = next();
                }
                else if (argument == L"--command")
                {
                    commandText = next();
                }
                else if (argument == L"--command-file")
                {
                    commandFile = next();
                }
                else
                {
                    throw unknownOption(argument);
                }
            }

            if (action == L"worker")
            {
                if (sessionDirectory.empty())
                {
                    throw std::invalid_argument(
                        "runtime worker requires --session.");
                }
                return RunRuntimeWorkerSafe(sessionDirectory);
            }

            if (projectRoot.empty())
            {
                throw std::invalid_argument(
                    "runtime "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --project.");
            }
            projectRoot = std::filesystem::weakly_canonical(
                std::filesystem::absolute(projectRoot));

            if (action == L"start")
            {
                return RunRuntimeStart(startOptions);
            }
            if (action == L"test")
            {
                if (specFile.empty())
                {
                    throw std::invalid_argument(
                        "runtime test requires --spec.");
                }
                return RunRuntimeTest(projectRoot, specFile);
            }
            if (action == L"replay")
            {
                if (replayFile.empty())
                {
                    throw std::invalid_argument(
                        "runtime replay requires --file.");
                }
                return RunRuntimeReplay(projectRoot, replayFile);
            }
            if (sessionId.empty())
            {
                throw std::invalid_argument(
                    "runtime "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --id.");
            }
            if (action == L"status")
            {
                return RunRuntimeStatus(projectRoot, sessionId);
            }
            if (action == L"stop")
            {
                return RunRuntimeSend(
                    projectRoot,
                    sessionId,
                    nlohmann::json{ { "op", "stop" } });
            }
            if (action == L"recover")
            {
                return RunRuntimeRecover(projectRoot, sessionId);
            }
            if (action == L"send")
            {
                if (commandText.empty() == commandFile.empty())
                {
                    throw std::invalid_argument(
                        "runtime send requires exactly one of "
                        "--command or --command-file.");
                }
                const auto runtimeCommand = commandText.empty()
                    ? ReadJsonFile(commandFile)
                    : ParseRuntimeCommandText(commandText);
                return RunRuntimeSend(
                    projectRoot,
                    sessionId,
                    runtimeCommand);
            }
            throw std::invalid_argument(
                "Unknown runtime action: "
                + LamaPon::PathToUtf8(
                    std::filesystem::path(action)));
        }

        if (command == L"job")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "job requires start, status, cancel, or list.");
            }
            const std::wstring_view action = arguments[2];
            if (action == L"start")
            {
                if (argumentCount < 4)
                {
                    throw std::invalid_argument(
                        "job start requires an operation.");
                }
                const std::wstring operation = arguments[3];
                std::vector<std::wstring> operationArguments{
                    operation };
                for (int argumentIndex = 4;
                    argumentIndex < argumentCount;
                    ++argumentIndex)
                {
                    operationArguments.push_back(
                        arguments[argumentIndex]);
                }
                return RunJobStart(
                    operation,
                    operationArguments);
            }
            if (action == L"worker")
            {
                std::filesystem::path directory;
                for (int argumentIndex = 3;
                    argumentIndex < argumentCount;
                    ++argumentIndex)
                {
                    const std::wstring_view argument =
                        arguments[argumentIndex];
                    if (argument == L"--job")
                    {
                        if (argumentIndex + 1 >= argumentCount)
                        {
                            throw std::invalid_argument(
                                "--job requires a value.");
                        }
                        directory = arguments[++argumentIndex];
                    }
                    else
                    {
                        throw unknownOption(argument);
                    }
                }
                if (directory.empty())
                {
                    throw std::invalid_argument(
                        "job worker requires --job.");
                }
                return RunJobWorkerSafe(directory);
            }

            std::filesystem::path projectRoot;
            std::wstring jobId;
            for (int argumentIndex = 3;
                argumentIndex < argumentCount;
                ++argumentIndex)
            {
                const std::wstring_view argument =
                    arguments[argumentIndex];
                if (argument == L"--project")
                {
                    if (argumentIndex + 1 >= argumentCount)
                    {
                        throw std::invalid_argument(
                            "--project requires a value.");
                    }
                    projectRoot = arguments[++argumentIndex];
                }
                else if (argument == L"--id")
                {
                    if (argumentIndex + 1 >= argumentCount)
                    {
                        throw std::invalid_argument(
                            "--id requires a value.");
                    }
                    jobId = arguments[++argumentIndex];
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty())
            {
                throw std::invalid_argument(
                    "job "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --project.");
            }
            projectRoot = std::filesystem::weakly_canonical(
                std::filesystem::absolute(projectRoot));
            if (action == L"list")
            {
                return RunJobList(projectRoot);
            }
            if (jobId.empty())
            {
                throw std::invalid_argument(
                    "job "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --id.");
            }
            if (action == L"status")
            {
                return RunJobStatus(projectRoot, jobId);
            }
            if (action == L"cancel")
            {
                return RunJobCancel(projectRoot, jobId);
            }
            throw std::invalid_argument(
                "Unknown job action: "
                + LamaPon::PathToUtf8(
                    std::filesystem::path(action)));
        }

        if (command == L"prefab")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "prefab requires inspect, validate, or patch.");
            }
            const std::wstring action = arguments[2];
            std::filesystem::path projectRoot;
            std::filesystem::path prefabPath;
            std::filesystem::path operations;
            std::filesystem::path output;
            bool dryRun{};
            index = 3;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--path")
                {
                    prefabPath = next();
                }
                else if (argument == L"--operations")
                {
                    operations = next();
                }
                else if (argument == L"--out")
                {
                    output = next();
                }
                else if (argument == L"--dry-run")
                {
                    dryRun = true;
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty() || prefabPath.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "prefab requires --project and --path.");
            }
            if (action == L"inspect")
            {
                return RunPrefabInspect(projectRoot, prefabPath);
            }
            if (action == L"validate")
            {
                return RunPrefabValidate(projectRoot, prefabPath);
            }
            if (action == L"patch")
            {
                if (operations.empty())
                {
                    throw std::invalid_argument(
                        "prefab patch requires --operations.");
                }
                return RunPrefabPatch(
                    projectRoot,
                    prefabPath,
                    operations,
                    output,
                    dryRun);
            }
            throw std::invalid_argument(
                "Unknown prefab action: "
                + LamaPon::PathToUtf8(std::filesystem::path(action)));
        }

        if (command == L"project")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "project requires inspect, list, add, remove, or move.");
            }
            const std::wstring_view action = arguments[2];
            std::filesystem::path projectRoot;
            std::filesystem::path destination;
            bool allowInsideEngineSource{};
            index = 3;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--to")
                {
                    destination = next();
                }
                else if (argument == L"--allow-inside-engine")
                {
                    allowInsideEngineSource = true;
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (action == L"list")
            {
                if (!projectRoot.empty()
                    || !destination.empty()
                    || allowInsideEngineSource)
                {
                    throw std::invalid_argument(
                        "project list does not accept --project.");
                }
                return RunProjectList();
            }
            if (projectRoot.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "project inspect/add/remove/move requires --project.");
            }
            if (action == L"move")
            {
                if (destination.empty())
                {
                    throw std::invalid_argument(
                        "project move requires --to.");
                }
                return RunProjectMove(
                    projectRoot,
                    destination,
                    allowInsideEngineSource);
            }
            if (!destination.empty() || allowInsideEngineSource)
            {
                throw std::invalid_argument(
                    "--to and --allow-inside-engine are for project move.");
            }
            if (action == L"inspect")
            {
                return RunProjectInspect(projectRoot);
            }
            if (action == L"add")
            {
                return RunProjectRegistration(projectRoot, true);
            }
            if (action == L"remove")
            {
                return RunProjectRegistration(projectRoot, false);
            }
            throw std::invalid_argument(
                "Unknown project action: "
                + LamaPon::PathToUtf8(
                    std::filesystem::path(action)));
        }

        if (command == L"component")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "component requires list or schema.");
            }
            const std::wstring action = arguments[2];
            std::string type;
            std::string category;
            index = 3;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--type")
                {
                    type = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                else if (argument == L"--category")
                {
                    category = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            return RunComponentCommand(action, type, category);
        }

        if (command == L"asset")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "asset requires list or inspect.");
            }
            const std::wstring action = arguments[2];
            std::filesystem::path projectRoot;
            std::filesystem::path assetPath;
            std::filesystem::path targetPath;
            std::vector<std::filesystem::path> sources;
            std::string guid;
            std::string importer;
            index = 3;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--path")
                {
                    assetPath = next();
                }
                else if (argument == L"--guid")
                {
                    guid = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                else if (argument == L"--importer")
                {
                    importer = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                else if (argument == L"--source")
                {
                    sources.push_back(next());
                }
                else if (argument == L"--target")
                {
                    targetPath = next();
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "asset requires --project.");
            }
            if (action == L"import")
            {
                return RunAssetImport(
                    projectRoot,
                    sources,
                    targetPath);
            }
            return RunAssetCommand(
                projectRoot,
                action,
                assetPath,
                guid,
                importer);
        }

        if (command == L"script")
        {
            if (argumentCount < 3)
            {
                PrintUsage();
                throw std::invalid_argument(
                    "script requires list or inspect.");
            }
            const std::wstring action = arguments[2];
            std::filesystem::path projectRoot;
            std::filesystem::path scriptPath;
            std::string className;
            bool force{};
            index = 3;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--path")
                {
                    scriptPath = next();
                }
                else if (argument == L"--class")
                {
                    className = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                else if (argument == L"--force")
                {
                    force = true;
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "script requires --project.");
            }
            if (action == L"create")
            {
                return RunScriptCreate(
                    projectRoot,
                    scriptPath,
                    className,
                    force);
            }
            return RunScriptCommand(
                projectRoot,
                action,
                scriptPath);
        }

        if (command == L"test")
        {
            std::filesystem::path projectRoot;
            std::filesystem::path scene;
            std::filesystem::path specification;
            std::filesystem::path report;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--scene")
                {
                    scene = next();
                }
                else if (argument == L"--spec")
                {
                    specification = next();
                }
                else if (argument == L"--report")
                {
                    report = next();
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty() || specification.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "test requires --project and --spec.");
            }
            return RunSceneTests(
                projectRoot,
                scene,
                specification,
                report);
        }

        if (command == L"patch")
        {
            std::filesystem::path projectRoot;
            std::filesystem::path scene;
            std::filesystem::path operations;
            std::filesystem::path output;
            bool dryRun{};
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--scene")
                {
                    scene = next();
                }
                else if (argument == L"--operations")
                {
                    operations = next();
                }
                else if (argument == L"--out")
                {
                    output = next();
                }
                else if (argument == L"--dry-run")
                {
                    dryRun = true;
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty() || operations.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "patch requires --project and --operations.");
            }
            return RunPatch(
                projectRoot,
                scene,
                operations,
                output,
                dryRun);
        }

        if (command == L"inspect"
            || command == L"validate")
        {
            std::filesystem::path projectRoot;
            std::filesystem::path scene;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                else if (argument == L"--scene")
                {
                    scene = next();
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (projectRoot.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "--project is required.");
            }
            return command == L"inspect"
                ? RunInspect(projectRoot, scene)
                : RunValidate(projectRoot, scene);
        }

        if (command == L"render")
        {
            RenderOptions options;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    options.projectRoot = next();
                }
                else if (argument == L"--scene")
                {
                    options.scene = next();
                }
                else if (argument == L"--out")
                {
                    options.outputPng = next();
                }
                else if (argument == L"--width")
                {
                    options.width = static_cast<
                        std::uint32_t>(
                        std::stoul(next()));
                }
                else if (argument == L"--height")
                {
                    options.height = static_cast<
                        std::uint32_t>(
                        std::stoul(next()));
                }
                else if (argument == L"--frames")
                {
                    options.frames = static_cast<
                        std::uint32_t>(
                        std::stoul(next()));
                }
                else if (argument == L"--simulate")
                {
                    options.simulateSeconds =
                        std::stod(next());
                }
                else if (argument == L"--input")
                {
                    // `Jump@0.5:0.2,Fire@1.0` のように、
                    // Action名@秒[:押している秒数][=向き] をカンマ区切りで。
                    // 向きは省略すると+1（正方向）。`=-1`と書くと
                    // `MoveHorizontal=-1@2.0`のように逆方向へ倒せます。
                    const auto specification =
                        LamaPon::WideToUtf8(next());
                    std::size_t start = 0;
                    while (start <= specification.size())
                    {
                        const auto comma =
                            specification.find(',', start);
                        auto item = specification.substr(
                            start,
                            comma == std::string::npos
                                ? std::string::npos
                                : comma - start);
                        start = comma == std::string::npos
                            ? specification.size() + 1
                            : comma + 1;
                        // 前後の空白を落とします。
                        const auto first =
                            item.find_first_not_of(" \t");
                        if (first == std::string::npos)
                        {
                            continue;
                        }
                        item = item.substr(
                            first,
                            item.find_last_not_of(" \t")
                                - first + 1);

                        RenderOptions::InputEvent event;
                        const auto at = item.find('@');
                        if (at == std::string::npos)
                        {
                            throw std::runtime_error(
                                "--input needs Action@seconds"
                                " (for example Jump@0.5): "
                                + item);
                        }
                        // 向きは`@`より前、Action名の後ろに書きます
                        // （`MoveHorizontal=-1@2.0`）。時刻を切り離して
                        // から探さないと、`1.0`の小数点や`:`と混ざります。
                        auto name = item.substr(0, at);
                        const auto equals = name.rfind('=');
                        if (equals != std::string::npos)
                        {
                            event.value = std::stod(
                                name.substr(equals + 1));
                            name = name.substr(0, equals);
                        }
                        event.action = std::move(name);
                        auto timing = item.substr(at + 1);
                        const auto colon = timing.find(':');
                        if (colon != std::string::npos)
                        {
                            event.duration = std::stod(
                                timing.substr(colon + 1));
                            timing = timing.substr(0, colon);
                        }
                        event.at = std::stod(timing);
                        options.inputEvents.push_back(
                            std::move(event));
                    }
                }
                else if (argument == L"--warp")
                {
                    options.warp = true;
                }
                else if (argument == L"--d3ddebug")
                {
                    options.d3dDebug = true;
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (options.projectRoot.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "--project is required.");
            }
            return RunRender(options);
        }
        if (command == L"new")
        {
            NewOptions options;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--dir")
                {
                    options.directory = next();
                }
                else if (argument == L"--name")
                {
                    options.name =
                        LamaPon::PathToUtf8(
                            std::filesystem::path{
                                next() });
                }
                else if (argument == L"--template")
                {
                    const auto value = next();
                    if (value == L"2d")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::TwoDimensional;
                    }
                    else if (value == L"3d")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::ThreeDimensional;
                    }
                    else if (value == L"learning-3d"
                        || value == L"learn"
                        || value == L"tutorial")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::
                                LearningThreeDimensional;
                    }
                    else if (value == L"learning-2d")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::
                                LearningTwoDimensional;
                    }
                    else
                    {
                        throw std::invalid_argument(
                            "--template must be 3d, 2d, learning-3d, or learning-2d.");
                    }
                }
                else if (argument == L"--allow-inside-engine")
                {
                    options.allowInsideEngineSource = true;
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (options.directory.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "--dir is required.");
            }
            return RunNew(options);
        }
        if (command == L"build")
        {
            BuildOptions options;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    options.projectRoot = next();
                }
                else if (argument == L"--config")
                {
                    options.configuration =
                        LamaPon::PathToUtf8(
                            std::filesystem::path{
                                next() });
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (options.projectRoot.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "--project is required.");
            }
            return RunBuild(options);
        }
        if (command == L"export")
        {
            CliExportOptions options;
            for (; index < argumentCount; ++index)
            {
                const std::wstring_view argument =
                    arguments[index];
                if (argument == L"--project")
                {
                    options.projectRoot = next();
                }
                else if (argument == L"--out")
                {
                    options.outputDirectory = next();
                }
                else if (argument == L"--zip")
                {
                    options.zip = true;
                }
                else
                {
                    throw unknownOption(argument);
                }
            }
            if (options.projectRoot.empty())
            {
                PrintUsage();
                throw std::invalid_argument(
                    "--project is required.");
            }
            return RunExport(options);
        }

        PrintUsage();
        throw std::invalid_argument(
            command.empty()
                ? "No command was given."
                : "Unknown command: "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path{
                            command }));
    }
    catch (const std::exception& exception)
    {
        // 失敗してもstdoutの約束（JSONを1つ）は守ります。ここまでの
        // ログも一緒に返すと、AIが原因へ一足で届きます。
        std::size_t errorCount{};
        std::size_t warningCount{};
        auto logs = CollectLogs(errorCount, warningCount);
        const nlohmann::json report{
            { "ok", false },
            { "command",
                command.empty()
                    ? std::string{}
                    : LamaPon::PathToUtf8(
                        std::filesystem::path{
                            command }) },
            { "error", exception.what() },
            { "errorCount", errorCount },
            { "warningCount", warningCount },
            { "logs", std::move(logs) },
        };
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        return 1;
    }
}
