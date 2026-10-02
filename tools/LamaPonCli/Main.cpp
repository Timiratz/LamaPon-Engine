#include "LamaPon/LamaPon.h"
#include "LamaPon/Assets/AssetImporter.h"
#include "LamaPon/Editor/GameExporter.h"
#include "LamaPon/Editor/GameModuleBuilder.h"
#include "LamaPon/Editor/PackageNativeDependencies.h"
#include "LamaPon/Scripting/GameModule.h"
#include "LamaPon/Graphics/PngWriter.h"
#include "LamaPon/Hub/LearningJourney.h"
#include "LamaPon/Hub/ProjectHub.h"
#include "AnalysisCommands.h"
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
// CLI契約: 標準出力はJSON一件、失敗時も公開済みキーを維持します。
using namespace LamaPon::Cli;
namespace
{
    // Progress(message: 進行内容): 処理状況をstderrへ表示します。
    void Progress(const std::string& message)
    {
        std::cerr << message << std::endl;
    }
    // job startはrequest/stateをJSON保存し、専用worker processを起動します。
    // JobFileVersion: job JSON形式の互換version。
    constexpr int JobFileVersion = 1;
    // MakeJobId(): process・時刻・連番から一意なjob IDを作ります。
    [[nodiscard]] std::string MakeJobId()
    {
        // sequence: 同一process内で重複しないIDを作る連番。
        static std::uint64_t sequence{};
        ++sequence;
        // 生成したjob IDを返します。
        return "job-"
            + std::to_string(GetCurrentProcessId())
            + "-"
            + std::to_string(GetTickCount64())
            + "-"
            + std::to_string(sequence);
    }
    // IsSafeJobId(value: 検査するID): path要素として安全な文字だけか返します。
    [[nodiscard]] bool IsSafeJobId(
        const std::wstring_view value)
    {
        // 空値・dot pathはjob directory名に使えません。
        if (value.empty() || value == L"." || value == L"..")
        {
            // 空またはdot pathは拒否します。
            return false;
        }
        // character: 使用可能文字を一つずつ調べるvalue内の文字。
        for (const wchar_t character : value)
        {
            // 英数字・hyphen・underscore以外を検出します。
            if (!(character >= L'a' && character <= L'z')
                && !(character >= L'A' && character <= L'Z')
                && !(character >= L'0' && character <= L'9')
                && character != L'-'
                && character != L'_')
            {
                // 許可外文字を含むため拒否します。
                return false;
            }
        }
        // 全文字が許可されているため受理します。
        return true;
    }
    // JobRoot(projectRoot: jobを保存するproject): job用directoryを返します。
    [[nodiscard]] std::filesystem::path JobRoot(
        const std::filesystem::path& projectRoot)
    {
        // .lamapon/jobs directoryを返します。
        return projectRoot / L".lamapon" / L"jobs";
    }
    // JobDirectory(projectRoot: jobを保存するproject, jobId: job識別子): 検証済みjob pathを返します。
    [[nodiscard]] std::filesystem::path JobDirectory(
        const std::filesystem::path& projectRoot,
        const std::wstring_view jobId)
    {
        // 不正なIDをpath結合前に拒否します。
        if (!IsSafeJobId(jobId))
        {
            // 不正IDを呼び出し元へ通知します。
            throw std::invalid_argument(
                "The job id contains invalid characters.");
        }
        // 検証済みjob directoryを返します。
        return JobRoot(projectRoot) / jobId;
    }
    // RuntimeRoot(projectRoot: runtimeを保存するproject): runtime用directoryを返します。
    [[nodiscard]] std::filesystem::path RuntimeRoot(
        const std::filesystem::path& projectRoot)
    {
        // .lamapon/runtime directoryを返します。
        return projectRoot / L".lamapon" / L"runtime";
    }
    // RuntimeDirectory(projectRoot: runtimeを保存するproject, sessionId: runtime識別子): 検証済みruntime pathを返します。
    [[nodiscard]] std::filesystem::path RuntimeDirectory(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId)
    {
        // runtime session IDもjob IDと同じ文字制約で検査します。
        if (!IsSafeJobId(sessionId))
        {
            // 不正なsession IDを呼び出し元へ通知します。
            throw std::invalid_argument(
                "The runtime session id contains invalid characters.");
        }
        // UTF-8 pathへ変換したruntime session directoryを返します。
        return RuntimeRoot(projectRoot)
            / LamaPon::PathFromUtf8(
                LamaPon::WideToUtf8(sessionId));
    }
    // MakeRuntimeId(): process・時刻・連番から一意なruntime IDを作ります。
    [[nodiscard]] std::string MakeRuntimeId()
    {
        // sequence: 同一process内で重複しないruntime ID連番。
        static std::uint64_t sequence{};
        ++sequence;
        // 生成したruntime IDを返します。
        return "runtime-"
            + std::to_string(GetCurrentProcessId())
            + "-"
            + std::to_string(GetTickCount64())
            + "-"
            + std::to_string(sequence);
    }
    // ReadTail(path: 読み取りfile, maximumBytes: 最大byte数): file末尾のbyte列を返します。
    [[nodiscard]] std::string ReadTail(
        const std::filesystem::path& path,
        const std::size_t maximumBytes = 12000)
    {
        // input: 末尾を読み取るbinary file stream。
        std::ifstream input(path, std::ios::binary);
        // fileを開けない場合は空文字列を返します。
        if (!input)
        {
            // 読み取り失敗を空文字列で表します。
            return {};
        }
        input.seekg(0, std::ios::end);
        // end: file末尾のstream position。
        const auto end = input.tellg();
        // 空fileまたはposition取得失敗なら返す内容はありません。
        if (end <= 0)
        {
            // fileに読み取れるbyteがない場合は空文字列を返します。
            return {};
        }
        // size: stream positionから得たfile byte数。
        const auto size = static_cast<std::uintmax_t>(end);
        // offset: 最大byte数以内に収まる読み始め位置。
        const auto offset = size > maximumBytes
            ? size - maximumBytes
            : 0;
        input.seekg(static_cast<std::streamoff>(offset));
        // result: file末尾から切り出したbyte列。
        std::string result(
            static_cast<std::size_t>(size - offset),
            '\0');
        input.read(
            result.data(),
            static_cast<std::streamsize>(result.size()));
        result.resize(
            static_cast<std::size_t>(input.gcount()));
        // 実際に読み取れたfile末尾の内容を返します。
        return result;
    }
    // QuoteWindowsArgument(value: command line引数): Windows用にescapeしたquoted引数を返します。
    [[nodiscard]] std::wstring QuoteWindowsArgument(
        const std::wstring_view value)
    {
        // result: quoteとescapeを加えた引数文字列。
        std::wstring result{ L"\"" };
        // backslashes: 次の通常文字・quote直前に現れた連続backslash数。
        std::size_t backslashes{};
        // character: Windows規則でescapeする引数内の現在文字。
        for (const wchar_t character : value)
        {
            // 次のquote処理用にbackslash数を記録します。
            if (character == L'\\')
            {
                ++backslashes;
                // 連続backslashは後続quoteとまとめて出力します。
                continue;
            }
            // quote直前のbackslashを倍化してescapeします。
            if (character == L'\"')
            {
                result.append(backslashes * 2 + 1, L'\\');
                result += L'\"';
                backslashes = 0;
                // quote処理後に次の文字を続けます。
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result += character;
        }
        result.append(backslashes * 2, L'\\');
        result += L'\"';
        // Windows command lineで使える引数表現を返します。
        return result;
    }
    // BuildCommandLine(arguments: 個別引数列): Windows向けcommand lineを組み立てます。
    [[nodiscard]] std::wstring BuildCommandLine(
        const std::vector<std::wstring>& arguments)
    {
        // result: space区切りにしたquoted argument列。
        std::wstring result;
        // argument: command lineへ追加する次の個別引数。
        for (const auto& argument : arguments)
        {
            // 2件目以降のargument前にseparatorを挿入します。
            if (!result.empty())
            {
                result += L' ';
            }
            result += QuoteWindowsArgument(argument);
        }
        // 完成したcommand lineを返します。
        return result;
    }
    // JobExecutable(): 現在のCLIと同じdirectoryにあるworker executableを返します。
    [[nodiscard]] std::filesystem::path JobExecutable()
    {
        // executable: 起動対象のLamaPonCli.exe path。
        const auto executable =
            LamaPon::ExecutableDirectory() / L"LamaPonCli.exe";
        // sibling executableがない場合はworkerを起動できません。
        if (!std::filesystem::is_regular_file(executable))
        {
            // worker不足を呼び出し元へ通知します。
            throw std::runtime_error(
                "LamaPonCli.exe was not found next to the current executable: "
                + LamaPon::PathToUtf8(executable));
        }
        // 存在確認済みworker executableを返します。
        return executable;
    }
    // IsJobProcessAlive(processId: job workerのprocess ID): workerが実行中か返します。
    [[nodiscard]] bool IsJobProcessAlive(
        const DWORD processId)
    {
        // Windows process ID 0は実workerを示しません。
        if (processId == 0)
        {
            // 無効なprocess IDは停止済みとして扱います。
            return false;
        }
        // process: 終了状態を調べるため開いたworker handle。
        HANDLE process = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            processId);
        // handle取得失敗時はprocess状態を確認できません。
        if (process == nullptr)
        {
            // 開けないprocessは停止済みとして扱います。
            return false;
        }
        // exitCode: workerが実行中か示す初期status。
        DWORD exitCode = STILL_ACTIVE;
        // queried: workerの終了statusを取得できたか。
        const bool queried =
            GetExitCodeProcess(process, &exitCode) != FALSE;
        CloseHandle(process);
        // status取得成功かつSTILL_ACTIVEならworkerは実行中です。
        return queried && exitCode == STILL_ACTIVE;
    }
    // FindProjectArgument(arguments: CLI引数列): --projectの値を返します。
    [[nodiscard]] std::filesystem::path FindProjectArgument(
        const std::vector<std::wstring>& arguments)
    {
        // index: 値を伴うflagだけを調べる現在位置。
        for (std::size_t index = 0;
            index + 1 < arguments.size();
            ++index)
        {
            // --project flagを見つけたら直後の引数を値として返します。
            if (arguments[index] == L"--project")
            {
                // --projectの直後にあるpathを返します。
                return arguments[index + 1];
            }
        }
        // --project flagがないことを空pathで表します。
        return {};
    }
    // JobState(jobDirectory: job保存先): job state JSONを読み込みます。
    [[nodiscard]] nlohmann::json JobState(
        const std::filesystem::path& jobDirectory)
    {
        // job directory内のstate.jsonを返します。
        return ReadJsonFile(jobDirectory / L"state.json");
    }
    // WriteJobState(jobDirectory: job保存先, state: 保存するjob state): state.jsonを更新します。
    void WriteJobState(
        const std::filesystem::path& jobDirectory,
        const nlohmann::json& state)
    {
        WriteJsonFile(jobDirectory / L"state.json", state);
    }
    // JobReport(command: CLI command, job: job情報): 標準JSON reportを作ります。
    [[nodiscard]] nlohmann::json JobReport(
        const char* command,
        nlohmann::json job)
    {
        // report: CLI契約に沿うjob response object。
        nlohmann::json report{
            { "ok", true },
            { "command", command },
            { "job", std::move(job) },
        };
        // 完成したjob reportを返します。
        return report;
    }
    // RunJobStart(operation: 実行operation, operationArguments: 渡すCLI引数): jobを保存してworkerを起動します。
    [[nodiscard]] int RunJobStart(
        const std::wstring& operation,
        const std::vector<std::wstring>& operationArguments)
    {
        // 受け付けるjob operationか確認します。
        if (operation != L"build"
            && operation != L"render"
            && operation != L"export"
            && operation != L"inspect"
            && operation != L"validate"
            && operation != L"patch"
            && operation != L"test")
        {
            // 未対応operationを起動前に拒否します。
            throw std::invalid_argument(
                "job start supports build, render, export, inspect, validate, patch, and test.");
        }
        // projectArgument: --projectで指定されたpath。
        const auto projectArgument =
            FindProjectArgument(operationArguments);
        // operation argumentsにproject rootが必要です。
        if (projectArgument.empty())
        {
            // project指定不足を呼び出し元へ通知します。
            throw std::invalid_argument(
                "job start requires --project for the operation.");
        }
        // projectRoot: canonical化したjob対象project root。
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(projectArgument));
        // jobRoot: job directoryを格納するroot。
        const auto jobRoot = JobRoot(projectRoot);
        // directoryError: job root作成時のfilesystem error。
        std::error_code directoryError;
        std::filesystem::create_directories(
            jobRoot,
            directoryError);
        // job root directoryの作成結果を調べます。
        if (directoryError)
        {
            // directory作成失敗を呼び出し元へ通知します。
            throw std::runtime_error(
                "Could not create job root: "
                + directoryError.message());
        }
        // jobId: 今回作成する一意なjob ID。
        const auto jobId = MakeJobId();
        // directory: job stateとrequestの保存先。
        const auto directory = jobRoot / LamaPon::PathFromUtf8(jobId);
        std::filesystem::create_directories(directory);
        // arguments: worker requestへ保存するUTF-8引数配列。
        nlohmann::json arguments = nlohmann::json::array();
        // argument: operation argumentsからJSONへ変換する現在のCLI value。
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
        // state: queued jobの初期statusとpolling情報。
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
        // executable: 起動するLamaPonCli worker executable。
        const auto executable = JobExecutable();
        // workerArguments: workerへ渡すjob directory付き引数列。
        std::vector<std::wstring> workerArguments{
            executable.wstring(),
            L"job",
            L"worker",
            L"--job",
            directory.wstring(),
        };
        // commandLine: Windows用にquoteしたworker command。
        std::wstring commandLine =
            BuildCommandLine(workerArguments);
        // startup: worker processの起動設定。
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // process: 起動後に管理するworker process handle。
        PROCESS_INFORMATION process{};
        // worker processの初回起動が成功したか確認します。
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
            // worker起動失敗をWin32 error付きで通知します。
            throw std::runtime_error(
                "Could not start the job worker (Win32 error "
                + std::to_string(GetLastError())
                + ").");
        }
        state["pid"] = process.dwProcessId;
        WriteJobState(directory, state);
        // suspended workerを再開できたか確認します。
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
            // worker再開失敗をstateへ記録して通知します。
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
        // job start commandの成功を返します。
        return 0;
    }
    // RunJobWorker(directory: job directory): requestを実行しstateとresultを保存します。
    [[nodiscard]] int RunJobWorker(
        const std::filesystem::path& directory)
    {
        // state: request実行中に更新するjob status。
        auto state = JobState(directory);
        // 開始前にcancel済みならworker処理を行いません。
        if (state.value("status", std::string{}) == "cancelled")
        {
            // cancel済みjobの終了を成功として返します。
            return 0;
        }
        state["status"] = "running";
        state["progress"] = 0.0;
        state["message"] = "Running";
        state["pid"] = GetCurrentProcessId();
        WriteJobState(directory, state);
        // request: workerが実行するjob request JSON。
        const auto request =
            ReadJsonFile(directory / L"request.json");
        // projectRoot: requestに保存されたproject directory。
        const auto projectRoot =
            LamaPon::PathFromUtf8(
                request.at("project").get<std::string>());
        // arguments: child CLIへ渡すwide argument列。
        std::vector<std::wstring> arguments;
        // value: requestからchild CLIへ復元する次のJSON argument。
        for (const auto& value : request.at("arguments"))
        {
            arguments.push_back(
                LamaPon::Utf8ToWide(value.get<std::string>()));
        }
        // executable: 実行するLamaPonCli executable。
        const auto executable = JobExecutable();
        // resultTemporary: child outputを一時保存するpath。
        const auto resultTemporary =
            directory / L"result.json.tmp";
        // resultPath: child outputの公開先path。
        const auto resultPath =
            directory / L"result.json";
        // progressPath: child stderrを保存するlog path。
        const auto progressPath =
            directory / L"progress.log";
        // removeError: 古いresult file削除時のfilesystem error。
        std::error_code removeError;
        std::filesystem::remove(resultPath, removeError);
        std::filesystem::remove(resultTemporary, removeError);
        // securityAttributes: child processへ継承するhandle設定。
        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength =
            sizeof(securityAttributes);
        securityAttributes.bInheritHandle = TRUE;
        // output: child stdoutを受けるresult file handle。
        HANDLE output = CreateFileW(
            resultTemporary.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // errorOutput: child stderrを受けるprogress log handle。
        HANDLE errorOutput = CreateFileW(
            progressPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // input: child stdinへ渡すNUL handle。
        HANDLE input = CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &securityAttributes,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        // 子processへ渡す3つの標準handleが有効か確認します。
        if (output == INVALID_HANDLE_VALUE
            || errorOutput == INVALID_HANDLE_VALUE
            || input == INVALID_HANDLE_VALUE)
        {
            // 有効なstdout handleを閉じます。
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            // 有効なstderr handleを閉じます。
            if (errorOutput != INVALID_HANDLE_VALUE) CloseHandle(errorOutput);
            // 有効なstdin handleを閉じます。
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            // 必要な出力handleを作れなかったことを通知します。
            throw std::runtime_error(
                "Could not create job output files.");
        }
        // childJob: child process groupを制御するjob object。
        HANDLE childJob = CreateJobObjectW(nullptr, nullptr);
        // 子process用job objectを作成できたか確認します。
        if (childJob == nullptr)
        {
            CloseHandle(output);
            CloseHandle(errorOutput);
            CloseHandle(input);
            // process groupの作成失敗を通知します。
            throw std::runtime_error(
                "Could not create the job process group.");
        }
        // limits: job objectのprocess終了時制約。
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        // 子process終了時にgroupも終了する設定を行います。
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
            // process groupを構成できなかったことを通知します。
            throw std::runtime_error(
                "Could not configure the job process group.");
        }
        // startup: child std handlesを割り当てる起動設定。
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = input;
        startup.hStdOutput = output;
        startup.hStdError = errorOutput;
        // process: job commandを実行するchild process handle。
        PROCESS_INFORMATION process{};
        // commandLine: child CLIへ渡すWindows command line。
        std::wstring commandLine =
            BuildCommandLine(
                [&]
                {
                    // full: executableとrequest argumentsを連結した列。
                    std::vector<std::wstring> full{
                        executable.wstring() };
                    full.insert(
                        full.end(),
                        arguments.begin(),
                        arguments.end());
                    // worker executableを含む完全なargument列を返します。
                    return full;
                }());
        // started: child CLI processを起動できたか。
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
        // request commandをworker processとして起動できたか確認します。
        if (!started)
        {
            CloseHandle(childJob);
            // request commandの起動失敗を通知します。
            throw std::runtime_error(
                "Could not start the job command (Win32 error "
                + std::to_string(GetLastError())
                + ").");
        }
        // 起動したprocessをjob groupへ割り当てます。
        if (!AssignProcessToJobObject(childJob, process.hProcess))
        {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(childJob);
            // process group割り当て失敗を通知します。
            throw std::runtime_error(
                "Could not attach the job command to its process group.");
        }
        CloseHandle(process.hThread);
        // waitResult: child process waitの完了status。
        const DWORD waitResult =
            WaitForSingleObject(process.hProcess, INFINITE);
        // exitCode: child CLIのprocess終了code。
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(process.hProcess);
        CloseHandle(childJob);
        // child processの終了を待った結果を確認します。
        if (waitResult != WAIT_OBJECT_0)
        {
            // process waitの失敗を呼び出し元へ通知します。
            throw std::runtime_error(
                "The job command wait failed.");
        }
        // 一時resultを公開result pathへ移動できたか確認します。
        if (!MoveFileExW(
                resultTemporary.c_str(),
                resultPath.c_str(),
                MOVEFILE_REPLACE_EXISTING
                    | MOVEFILE_WRITE_THROUGH))
        {
            // result fileの不足を呼び出し元へ通知します。
            throw std::runtime_error(
                "The job command did not produce a result file.");
        }
        // result: child CLIが出力したJSON response。
        nlohmann::json result;
        // child processのresult JSONを読み込みます。
        try
        {
            result = ReadJsonFile(resultPath);
        }
        // exception: 壊れたresult fileのerror responseを作ります。
        catch (const std::exception& exception)
        {
            result = {
                { "ok", false },
                { "error", exception.what() },
            };
        }
        state = JobState(directory);
        // 処理中にcancelされた場合はstateを上書きしません。
        if (state.value("status", std::string{}) == "cancelled")
        {
            // cancel済みjobの終了を成功として返します。
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
        // child commandのexit statusをworker statusとして返します。
        return exitCode == 0 ? 0 : 1;
    }
    // RunJobWorkerSafe(directory: job directory): 例外をfailed stateへ記録します。
    [[nodiscard]] int RunJobWorkerSafe(
        const std::filesystem::path& directory)
    {
        // job commandの例外をstateへ記録しながら実行します。
        try
        {
            // worker commandの終了codeを返します。
            return RunJobWorker(directory);
        }
        // exception: worker処理で発生したerrorをfailed stateへ記録します。
        catch (const std::exception& exception)
        {
            // failure stateの保存を試みます。
            try
            {
                // state: worker failureを書き込むjob status。
                auto state = JobState(directory);
                // cancel以外の状態だけfailedへ更新します。
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
            // failure state保存時の例外を処理します。
            catch (const std::exception& stateException)
            {
                Progress(
                    "Could not record job failure: "
                    + std::string(stateException.what()));
            }
            // worker failureを示すexit codeを返します。
            return 1;
        }
    }
    // RunJobStatus(projectRoot: project root, jobId: job ID): job stateとprogress logを返します。
    [[nodiscard]] int RunJobStatus(
        const std::filesystem::path& projectRoot,
        const std::wstring_view jobId)
    {
        // directory: statusを取得するjob directory。
        const auto directory = JobDirectory(projectRoot, jobId);
        // state: status commandが返すjob情報。
        auto state = JobState(directory);
        // status: worker stateの現在値。
        const auto status =
            state.value("status", std::string{});
        // 稼働中扱いのworkerが終了済みなら失敗状態にします。
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
        // logTail: progress log末尾の表示用text。
        const auto logTail =
            ReadTail(directory / L"progress.log");
        // progress logがある場合だけstate responseへ含めます。
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
        // status response commandの成功を返します。
        return 0;
    }
    // RunJobCancel(projectRoot: project root, jobId: job ID): worker停止を要求しjob stateを返します。
    [[nodiscard]] int RunJobCancel(
        const std::filesystem::path& projectRoot,
        const std::wstring_view jobId)
    {
        // directory: cancel対象jobのdirectory。
        const auto directory = JobDirectory(projectRoot, jobId);
        // state: cancel後に返すjob情報。
        auto state = JobState(directory);
        // status: cancel前のjob status。
        const auto status =
            state.value("status", std::string{});
        // queuedまたはrunning jobだけcancelします。
        if (status == "queued" || status == "running")
        {
            // processId: stateに記録されたworker process ID。
            const DWORD processId =
                state.value("pid", 0u);
            // process: 停止対象workerのprocess handle。
            HANDLE process = OpenProcess(
                PROCESS_TERMINATE,
                FALSE,
                processId);
            // worker process handleを取得できた場合に停止します。
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
        // cancel response commandの成功を返します。
        return 0;
    }
    // RunJobList(projectRoot: project root): 保存済みjob一覧をJSONで返します。
    [[nodiscard]] int RunJobList(
        const std::filesystem::path& projectRoot)
    {
        // jobs: responseへ返すjob state配列。
        nlohmann::json jobs = nlohmann::json::array();
        // root: project内job directoryのpath。
        const auto root = JobRoot(projectRoot);
        // iteratorError: directory走査時のfilesystem error。
        std::error_code iteratorError;
        // job root directoryがある場合に一覧化します。
        if (std::filesystem::is_directory(root, iteratorError))
        {
            // entry: job root内を順に確認するdirectory entry。
            for (const auto& entry :
                std::filesystem::directory_iterator(root, iteratorError))
            {
                // directory以外またはiteration errorのentryを飛ばします。
                if (iteratorError || !entry.is_directory())
                {
                    // job stateを持たないentryを無視します。
                    continue;
                }
                // statePath: entry内のjob state JSON path。
                const auto statePath =
                    entry.path() / L"state.json";
                // entryにstate.jsonがある場合だけ読み込みます。
                if (std::filesystem::is_regular_file(statePath))
                {
                    // 読み取り可能なjob stateを一覧へ追加します。
                    try
                    {
                        jobs.push_back(ReadJsonFile(statePath));
                    }
                    // 作成中のstate fileは次の一覧取得で読めるため読み飛ばします。
                    catch (const std::exception&)
                    {
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
        // job list responseを出力したことを成功codeで返します。
        return 0;
    }
    // CreateHiddenWindow(width: window幅, height: window高さ): swap chain用の非表示windowを作ります。
    [[nodiscard]] HWND CreateHiddenWindow(
        const std::uint32_t width,
        const std::uint32_t height)
    {
        // windowClass: swap chain用window class設定。
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = DefWindowProcW;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"LamaPonCliHidden";
        // 非表示window用classを登録できたか確認します。
        if (RegisterClassExW(&windowClass) == 0)
        {
            // window class登録の失敗を通知します。
            throw std::runtime_error(
                "RegisterClassExW failed.");
        }
        // window: swap chainを初期化する非表示window。
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
        // swap chain用windowを作成できたか確認します。
        if (window == nullptr)
        {
            // window作成の失敗を通知します。
            throw std::runtime_error(
                "CreateWindowExW failed.");
        }
        // swap chain用の非表示windowを返します。
        return window;
    }
    struct ImageSummary final
    {
        // meanRed: image内全pixelのred平均値。
        double meanRed{};
        // meanGreen: image内全pixelのgreen平均値。
        double meanGreen{};
        // meanBlue: image内全pixelのblue平均値。
        double meanBlue{};
        // uniqueColors: image内で異なるRGB値の数。
        std::size_t uniqueColors{};
        // magentaPixels: magenta系error shader色のpixel数。
        std::size_t magentaPixels{};
    };
    // Summarize(width: image幅, height: image高さ, pixels: RGBA buffer): pixel統計を返します。
    [[nodiscard]] ImageSummary Summarize(
        const std::uint32_t width,
        const std::uint32_t height,
        const std::vector<std::uint8_t>& pixels)
    {
        // summary: imageから計算した色・error統計。
        ImageSummary summary{};
        // colors: 重複を除いたRGB値集合。
        std::unordered_set<std::uint32_t> colors;
        // count: image内pixel総数。
        const std::size_t count =
            static_cast<std::size_t>(width) * height;
        // totalRed: 全pixelのred合計値。
        double totalRed{};
        // totalGreen: 全pixelのgreen合計値。
        double totalGreen{};
        // totalBlue: 全pixelのblue合計値。
        double totalBlue{};
        // index: 各RGBA pixelを集計するimage buffer位置。
        for (std::size_t index = 0; index < count; ++index)
        {
            // red: 現在pixelのred channel。
            const std::uint8_t red =
                pixels[index * 4];
            // green: 現在pixelのgreen channel。
            const std::uint8_t green =
                pixels[index * 4 + 1];
            // blue: 現在pixelのblue channel。
            const std::uint8_t blue =
                pixels[index * 4 + 2];
            totalRed += red;
            totalGreen += green;
            totalBlue += blue;
            colors.insert(
                (static_cast<std::uint32_t>(red) << 16)
                | (static_cast<std::uint32_t>(green) << 8)
                | blue);
            // error shaderのmagenta色に近いpixelを数えます。
            if (red > 90
                && blue > 90
                && green + 60 < red
                && green + 60 < blue)
            {
                ++summary.magentaPixels;
            }
        }
        // pixelがあるimageだけ平均色を計算します。
        if (count > 0)
        {
            summary.meanRed = totalRed / count;
            summary.meanGreen = totalGreen / count;
            summary.meanBlue = totalBlue / count;
        }
        summary.uniqueColors = colors.size();
        // pixel統計をまとめたsummaryを返します。
        return summary;
    }
    // CollectLogs(errorCount: error数, warningCount: warning数): 非情報logとcounterを返します。
    [[nodiscard]] nlohmann::json CollectLogs(
        std::size_t& errorCount,
        std::size_t& warningCount)
    {
        // logs: responseへ返す非情報log entry列。
        auto logs = nlohmann::json::array();
        // entry: level別に集計する次のlogger entry。
        for (const auto& entry :
            LamaPon::Logger::Instance().Snapshot())
        {
            // 情報logは出力量を抑えるためresponseから除きます。
            if (entry.level == LamaPon::LogLevel::Info)
            {
                // 情報logのerror/warning集計を飛ばします。
                continue;
            }
            // error logだけerror counterへ加算します。
            if (entry.level == LamaPon::LogLevel::Error)
            {
                ++errorCount;
            }
            // error以外の非情報logをwarning counterへ加算します。
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
        // 集計した非情報log JSONを返します。
        return logs;
    }
    // CollectProblems(scene: 描画済みscene): shader・script・colliderの問題をJSON化します。
    [[nodiscard]] nlohmann::json CollectProblems(
        const LamaPon::Scene& scene)
    {
        // problems: 描画後に検出した問題entry列。
        auto problems = nlohmann::json::array();
        // addShaderError(objectName: object名, componentName: component型, shaderPath: shader file, error: compile error): 有効なshader errorをproblem JSONへ追加するlambda。
        const auto addShaderError =
            [&problems](
                const std::string& objectName,
                const char* componentName,
                const std::filesystem::path& shaderPath,
                const std::string& error)
            {
                // shader errorが空ならproblem entryは必要ありません。
                if (error.empty())
                {
                    // 記録対象がないためlambdaを終了します。
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
        // gameObject: 描画errorを確認するscene内object。
        for (const auto& gameObject : scene.GameObjects())
        {
            // name: error reportに載せるobject名。
            const auto& name = gameObject->Name();
            // renderer: MeshRendererのshader errorを調べます。
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
            // renderer: ModelRendererのshader errorを調べます。
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
            // renderer: SpriteRendererのshader errorを調べます。
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
            // particles: ParticleSystemのshader errorを調べます。
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
            // script: 実行時errorを持つNativeScript component。
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
            // collider: load errorを持つMeshCollider component。
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
        // scene内で検出したproblem JSONを返します。
        return problems;
    }
    struct RenderOptions final
    {
        // projectRoot: render対象project root。
        std::filesystem::path projectRoot;
        // scene: 描画するscene file。空なら起動sceneを使います。
        std::filesystem::path scene;
        // outputPng: render imageの出力path。
        std::filesystem::path outputPng{ "render.png" };
        // width: 出力image幅。0ならproject設定値です。
        std::uint32_t width{};
        // height: 出力image高さ。0ならproject設定値です。
        std::uint32_t height{};
        // frames: 撮影前に進めるframe数と撮影frameを決めます。
        std::uint32_t frames{ 4 };
        // simulateSeconds: 撮影前にsceneをupdateする秒数。
        double simulateSeconds{};
        struct InputEvent final
        {
            // action: input元のAction名。
            std::string action;
            // at: scene開始から入力を行う秒数。
            double at{};
            // duration: 入力を維持する秒数。
            double duration{ 0.1 };
            // value: input directionを表す符号。
            double value{ 1.0 };
        };
        // inputEvents: 撮影前に注入するinput event列。
        std::vector<InputEvent> inputEvents;
        // warp: WARP software adapterを選ぶ指定。
        bool warp{};
        // d3dDebug: Direct3D debug layerを有効にする指定。
        bool d3dDebug{};
    };
    // 常駐ランタイムセッション
    // RuntimeFileVersion: runtime session JSONの互換version。
    // EditorのUIを経由せず、LamaPonCli自身がゲームループを持ちます。
    // セッションの入出力はJSONファイルだけなので、別プロセスや外部の自動化ツールからも同じ手順で操作できます。
    constexpr int RuntimeFileVersion = 1;
    struct RuntimeStartOptions final
    {
        // projectRoot: runtime sessionを起動するproject root。
        std::filesystem::path projectRoot;
        // scene: runtimeで開くscene path。
        std::filesystem::path scene;
        // width: runtime windowのpixel幅。
        std::uint32_t width{};
        // height: runtime windowのpixel高さ。
        std::uint32_t height{};
        // targetFrameRate: runtime loopの目標frame rate。
        std::uint32_t targetFrameRate{ 60 };
        // fixedDeltaTime: 固定physics stepの秒数。
        float fixedDeltaTime{};
        // warp: WARP software adapterを選ぶ指定。
        bool warp{};
        // d3dDebug: Direct3D debug layerの有効指定。
        bool d3dDebug{};
        // deterministic: deterministic modeの有効指定。
        bool deterministic{};
        // renderEveryNFrames: N frameごとに描画する間隔。
        std::uint32_t renderEveryNFrames{ 1 };
        // paceFrames: target rateに合わせてframeを待つ指定。
        bool paceFrames{ true };
        // recordPath: runtime command recordの保存先。
        std::filesystem::path recordPath;
        // replayCommands: 起動後に適用するcommand replay列。
        nlohmann::json replayCommands = nlohmann::json::array();
    };
    struct RuntimeSessionHandle final
    {
        // projectRoot: sessionが属するproject root。
        std::filesystem::path projectRoot;
        // directory: session request/stateの保存directory。
        std::filesystem::path directory;
        // sessionId: sessionを識別する一意ID。
        std::string sessionId;
        // state: session statusと操作結果のJSON。
        nlohmann::json state;
    };
    // RuntimeValueJson(value: runtime value variant): 対応するJSON valueへ変換します。
    [[nodiscard]] nlohmann::json RuntimeValueJson(
        const LamaPon::RuntimeGameState::Value& value)
    {
        // result: variantから変換するJSON value。
        nlohmann::json result;
        // visitor(item: variant alternative): active alternativeをJSONへコピーします。
        std::visit(
            [&result](const auto& item)
            {
                result = item;
            },
            value);
        // variant valueをJSONとして返します。
        return result;
    }
    // BuildRuntimeSnapshot(scene: 対象scene, graphics: 描画・入力device, paused: pause状態): runtime観測値をJSON化します。
    [[nodiscard]] nlohmann::json BuildRuntimeSnapshot(
        const LamaPon::Scene& scene,
        const LamaPon::GraphicsDevice& graphics,
        const bool paused)
    {
        // frame: 現在frameのrender統計。
        const auto& frame = graphics.FrameStats();
        // physics: scene physicsの統計。
        const auto& physics = scene.PhysicsStats();
        // visibility: scene rendererの可視性統計。
        const auto& visibility = scene.VisibilityStats();
        // snapshot: screen・renderer・physics・runtime状態のJSON。
        nlohmann::json snapshot{
            { "playing", true },
            { "paused", paused },
            { "scene",
                LamaPon::PathToUtf8(
                    scene.Scenes().CurrentScenePath()) },
            { "objectCount", scene.GameObjects().size() },
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
                { "fixedDeltaTime", scene.PhysicsTiming().fixedDeltaTime },
                { "interpolationAlpha", scene.PhysicsTiming().interpolationAlpha },
                { "simulatedTime", scene.PhysicsTiming().simulatedTime },
                { "presentationTime", scene.PhysicsTiming().PresentationTime() },
                { "discardedDeltaTime", scene.PhysicsTiming().discardedDeltaTime },
                { "discardedTime", scene.PhysicsTiming().discardedTime },
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
        // gameState: snapshotへ返すnamed runtime state。
        auto gameState = nlohmann::json::object();
        // values: runtime stateのkey/value snapshot。
        auto values = scene.Scenes().State().Snapshot();
        // comparator(left: state item, right: state item): key順に並べます。
        std::ranges::sort(
            values,
            [](const auto& left, const auto& right)
            {
                // state itemをkey順に並べてsnapshotを安定させます。
                return left.first < right.first;
            });
        // key/value: 並べ替えたgame stateの項目名と値。
        for (const auto& [key, value] : values)
        {
            gameState[key] = RuntimeValueJson(value);
        }
        snapshot["gameState"] = std::move(gameState);
        // objects: snapshotへ返すobject情報配列。
        auto objects = nlohmann::json::array();
        // object: snapshotへ出力するscene object。
        for (const auto& object : scene.GameObjects())
        {
            // null objectはcomponentとtransformを参照できません。
            if (object == nullptr)
            {
                // 存在しないobjectはsnapshotから飛ばします。
                continue;
            }
            // transform: objectのposition・rotation・scale。
            const auto& transform = object->GetTransform();
            // euler: object rotationのEuler角。
            const auto euler = transform.EulerAngles();
            // components: object component状態の配列。
            auto components = nlohmann::json::array();
            // component: objectごとに状態を記録するcomponent。
            for (const auto& component : object->Components())
            {
                // null componentの状態は出力しません。
                if (component == nullptr)
                {
                    // 存在しないcomponentはsnapshotから飛ばします。
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
        // actions: snapshotへ返すinput action一覧。
        auto actions = nlohmann::json::array();
        // action: snapshotへ書き出すinput action。
        for (const auto& action : graphics.Input().Actions())
        {
            // bindings: actionへ登録されたbinding一覧。
            auto bindings = nlohmann::json::array();
            // binding: actionに登録された次のcontrol binding。
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
        // profileFrames: profilerが保持するframe一覧。
        const auto profileFrames =
            LamaPon::Profiler::Instance().Snapshot();
        // profile: snapshotへ返すlatest profiler情報。
        auto profile = nlohmann::json{
            { "enabled",
                LamaPon::Profiler::Instance().IsEnabled() },
            { "frameIndex", 0 },
            { "milliseconds", 0.0 },
            { "samples", nlohmann::json::array() },
        };
        // profile sampleがある場合だけ最新frameを出力します。
        if (!profileFrames.empty())
        {
            // latest: 最新profiler frameのsample集合。
            const auto& latest = profileFrames.back();
            profile["frameIndex"] = latest.index;
            profile["milliseconds"] = latest.milliseconds;
            // sample: latest profiler frameの次の測定区間。
            for (const auto& sample : latest.samples)
            {
                // entry: profile responseへ書き出すsample。
                auto entry = nlohmann::json{
                    { "name", sample.name },
                    { "milliseconds", sample.milliseconds },
                    { "calls", sample.callCount },
                    { "depth", sample.depth },
                };
                // 最上位区間はparentを省略し、従来の読み手と同じ形にします。
                if (sample.parent
                    != LamaPon::ProfileSample::NoParent)
                {
                    entry["parent"] = sample.parent;
                }
                profile["samples"].push_back(std::move(entry));
            }
        }
        snapshot["profiler"] = std::move(profile);
        // logs: snapshotへ返す非情報log一覧。
        auto logs = nlohmann::json::array();
        // logEntries: loggerから取得した全log entry。
        const auto logEntries = LamaPon::Logger::Instance().Snapshot();
        // firstLog: 直近64件の先頭log iterator。
        const auto firstLog = logEntries.size() > 64
            ? logEntries.end() - 64
            : logEntries.begin();
        // iterator: 直近64件に絞ったlog entry。
        for (auto iterator = firstLog;
            iterator != logEntries.end();
            ++iterator)
        {
            // info logはruntime snapshotのlogsから除きます。
            if (iterator->level == LamaPon::LogLevel::Info)
            {
                // info logをsnapshotへの追加対象から外します。
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
        // runtime snapshot JSONを返します。
        return snapshot;
    }
    // RuntimeState(directory: session directory): 安定するまでstate JSONを読み直します。
    [[nodiscard]] nlohmann::json RuntimeState(
        const std::filesystem::path& directory)
    {
        // path: runtime state JSONのpath。
        const auto path = directory / L"state.json";
        // lastError: state retryで最後に発生したread error。
        std::string lastError;
        // attempt: WebDAV上のstate fileを再読込する試行番号。
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            // 安定したruntime state fileの読み込みを試みます。
            try
            {
                // 読み取りに成功したstate JSONを返します。
                return ReadJsonFile(path);
            }
            // state fileが読めない場合はretry用errorを保存します。
            // exception: state JSONを開けなかった最後のerror。
            catch (const std::exception& exception)
            {
                lastError = exception.what();
                // retry回数が残っていれば短い待機を挟みます。
                if (attempt + 1 < 50)
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                }
            }
        }
        // retry後も読めないstateのerrorを通知します。
        throw std::runtime_error(
            "Could not read stable runtime state after retrying: "
            + lastError);
    }
    // WriteRuntimeState(directory: session directory, state: session JSON): state fileを更新します。
    void WriteRuntimeState(
        const std::filesystem::path& directory,
        const nlohmann::json& state)
    {
        WriteJsonFile(directory / L"state.json", state);
    }
    // RuntimeCommandName(command: runtime command JSON): command名を返します。
    [[nodiscard]] std::string RuntimeCommandName(
        const nlohmann::json& command)
    {
        // op fieldがstringならcommand名として優先します。
        if (command.contains("op") && command.at("op").is_string())
        {
            // op fieldのcommand名を返します。
            return command.at("op").get<std::string>();
        }
        // op fieldがなければtype fieldを返します。
        return command.value("type", std::string{});
    }
    // TrimRuntimeToken(value: 入力token): 前後空白と外側quoteを除いたtextを返します。
    [[nodiscard]] std::string TrimRuntimeToken(
        std::string value)
    {
        // first: token先頭の非空白位置。
        const auto first = value.find_first_not_of(" \t\r\n");
        // 空白trim後にtokenが残っているか確認します。
        if (first == std::string::npos)
        {
            // 空白だけのtokenを空文字列で表します。
            return {};
        }
        // last: token末尾の非空白位置。
        const auto last = value.find_last_not_of(" \t\r\n");
        value = value.substr(first, last - first + 1);
        // 外側のquote pairがあれば取り除きます。
        if (value.size() >= 2
            && ((value.front() == '"' && value.back() == '"')
                || (value.front() == '\'' && value.back() == '\'')))
        {
            // quoteを除いたtokenを返します。
            return value.substr(1, value.size() - 2);
        }
        // 変更していないtokenを返します。
        return value;
    }
    // RuntimeCommandValue(token: command field value): boolean・null・number・stringへ変換します。
    [[nodiscard]] nlohmann::json RuntimeCommandValue(
        const std::string& token)
    {
        // value: quoteを除いたtokenの本文。
        const auto value = TrimRuntimeToken(token);
        // true tokenをJSON booleanとして解釈します。
        if (value == "true")
        {
            // JSON trueを返します。
            return true;
        }
        // false tokenをJSON booleanとして解釈します。
        if (value == "false")
        {
            // JSON falseを返します。
            return false;
        }
        // null tokenをJSON nullとして解釈します。
        if (value == "null")
        {
            // JSON nullを返します。
            return nullptr;
        }
        // 整数として全tokenを変換できるか試します。
        try
        {
            // parsedLength: 整数変換が消費した文字数。
            std::size_t parsedLength{};
            // integer: 整数形式として変換したtoken値。
            const auto integer = std::stoll(value, &parsedLength);
            // token全体が整数なら数値として受理します。
            if (parsedLength == value.size())
            {
                // 変換した整数を返します。
                return integer;
            }
        }
        // 整数にできないtokenは小数変換へ進みます。
        catch (const std::exception&)
        {
        }
        // 浮動小数として全tokenを変換できるか試します。
        try
        {
            // parsedLength: 小数変換が消費した文字数。
            std::size_t parsedLength{};
            // number: 小数形式として変換したtoken値。
            const auto number = std::stod(value, &parsedLength);
            // token全体が数値なら小数値として受理します。
            if (parsedLength == value.size())
            {
                // 変換した小数値を返します。
                return number;
            }
        }
        // 数値にできないtokenは文字列のまま扱います。
        catch (const std::exception&)
        {
        }
        // boolean・null・number以外のtokenを文字列で返します。
        return value;
    }
    // ParseRuntimeCommandText(text: runtime control text): control commandをJSONとして解析します。
    [[nodiscard]] nlohmann::json ParseRuntimeCommandText(
        const std::wstring_view text)
    {
        // source: wide inputをUTF-8へ変換したtext。
        const auto source = LamaPon::WideToUtf8(text);
        // trimmed: whitespaceと外側quoteを除いたcommand text。
        const auto trimmed = TrimRuntimeToken(source);
        // 単独で指定できるruntime control commandか確認します。
        if (trimmed == "pause"
            || trimmed == "resume"
            || trimmed == "step"
            || trimmed == "observe"
            || trimmed == "stop")
        {
            // 単独commandをop付きJSONへ変換します。
            return { { "op", trimmed } };
        }
        // simplified object記法でcommand fieldを含むか確認します。
        if (trimmed.size() >= 2
            && trimmed.front() == '{'
            && trimmed.back() == '}'
            && trimmed.find('"') == std::string::npos)
        {
            // result: 簡易object表記を格納するJSON object。
            nlohmann::json result = nlohmann::json::object();
            // body: 外側braceを除いたcomma区切りfield列。
            const auto body = trimmed.substr(
                1,
                trimmed.size() - 2);
            // start: 次のfieldを探すbody内のoffset。
            std::size_t start{};
            // 簡易object内のcomma区切りfieldを順に解析します。
            while (start <= body.size())
            {
                // separator: 現在fieldを区切るcomma位置。
                const auto separator = body.find(',', start);
                // part: comma間の単一field text。
                const auto part = body.substr(
                    start,
                    separator == std::string::npos
                        ? std::string::npos
                        : separator - start);
                // colon: field内のkeyとvalueを分ける位置。
                const auto colon = part.find(':');
                // fieldにkey/valueを分けるcolonがあるか確認します。
                if (colon == std::string::npos)
                {
                    // colonのないfieldを不正なcommandとして拒否します。
                    throw std::invalid_argument(
                        "runtime --command contains a field without ':'.");
                }
                // key: field名からtrimしたselector key。
                const auto key = TrimRuntimeToken(
                    part.substr(0, colon));
                // field nameをtrimした結果が空か確認します。
                if (key.empty())
                {
                    // 空のfield nameを不正なcommandとして拒否します。
                    throw std::invalid_argument(
                        "runtime --command contains an empty field name.");
                }
                result[key] = RuntimeCommandValue(
                    part.substr(colon + 1));
                // 最後のfieldならobject解析を終了します。
                if (separator == std::string::npos)
                {
                    // 最後のfieldの後に解析を続けません。
                    break;
                }
                start = separator + 1;
            }
            // 解析したcompact command JSONを返します。
            return result;
        }
        // 標準JSON形式としてcommand textを解析します。
        try
        {
            // 有効なJSON commandを返します。
            return nlohmann::json::parse(source);
        }
        // JSON syntax errorをruntime command errorへ変換します。
        catch (const std::exception&)
        {
            // 不正なJSON commandの理由を呼び出し元へ通知します。
            throw std::invalid_argument(
                "runtime --command must be valid JSON (or a compact "
                "operation such as {op:pause}).");
        }
    }
    // RuntimeInputFrames(command: runtime input command): 指定frame数を1〜600へ制限して返します。
    [[nodiscard]] std::uint32_t RuntimeInputFrames(
        const nlohmann::json& command)
    {
        // frames指定がなければ1 frameだけ入力します。
        if (!command.contains("frames"))
        {
            // 既定の1 frameを返します。
            return 1;
        }
        // value: command frames field。
        const auto& value = command.at("frames");
        // framesが正のintegerか確認します。
        if (!value.is_number_integer()
            || value.get<std::int64_t>() < 1)
        {
            // 不正なinput frame数を呼び出し元へ通知します。
            throw std::invalid_argument(
                "input frames must be a positive integer.");
        }
        // 1〜600へ制限したframe数を返します。
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            value.get<std::int64_t>(),
            1,
            600));
    }
    // ApplyRuntimeInput(command: input command, inputSystem: actions, snapshot: frame input, frames: input frame count, error: failure text): controlまたはactionを解決します。
    void ApplyRuntimeInput(
        const nlohmann::json& command,
        const LamaPon::InputSystem& inputSystem,
        LamaPon::InputSnapshot& snapshot,
        std::uint32_t& frames,
        std::string& error)
    {
        // numeric input valueが指定されているか確認します。
        if (!command.contains("value")
            || !command.at("value").is_number())
        {
            error = "input requires a numeric value.";
            // 数値がないinput commandを適用しません。
            return;
        }
        // value: input commandのrequested numeric value。
        const float value = command.at("value").get<float>();
        // input valueがfiniteな数値か確認します。
        if (!std::isfinite(value))
        {
            error = "input value must be finite.";
            // NaN・infinityを含むinputを適用しません。
            return;
        }
        // control: InputSnapshotへ設定するresolved control。
        LamaPon::InputControl control{};
        // controlValue: controlのscaleを反映した入力値。
        float controlValue = value;
        // resolved: controlを有効なbindingへ解決できたか。
        bool resolved = false;
        // control名があればcontrol直接指定を優先します。
        if (command.contains("control")
            && command.at("control").is_string())
        {
            // control nameからInputControlへ解決します。
            try
            {
                control = LamaPon::InputControlFromName(
                    command.at("control").get<std::string>());
                resolved = true;
            }
            // control nameが不明ならerrorへ記録します。
            catch (const std::exception&)
            {
                error = "unknown input control: "
                    + command.at("control").get<std::string>();
            }
        }
        // control指定がない場合はaction名でbindingを探します。
        else if (command.contains("action")
            && command.at("action").is_string())
        {
            // actionName: input commandで指定されたaction名。
            const auto actionName =
                command.at("action").get<std::string>();
            // action: nameで検索したinput action iterator。
            // predicate(candidate: input action): actionNameと一致するentryを選びます。
            const auto action = std::find_if(
                inputSystem.Actions().begin(),
                inputSystem.Actions().end(),
                [&actionName](const auto& candidate)
                {
                    // candidate actionがrequested action名か判定します。
                    return candidate.name == actionName;
                });
            // 指定actionがinput systemに存在するか確認します。
            if (action == inputSystem.Actions().end())
            {
                error = "unknown input action: " + actionName;
            }
            // actionの次のusable bindingを探します。
            else
            {
                // binding: 入力値に使う最初のusable binding。
                // predicate(candidate: input binding): non-zero scaleのbindingを選びます。
                const auto binding = std::find_if(
                    action->bindings.begin(),
                    action->bindings.end(),
                    [](const auto& candidate)
                    {
                        // candidate bindingのscaleが有効か判定します。
                        return std::abs(candidate.scale)
                            > 1.0e-6f;
                    });
                // actionにusable bindingがあるか確認します。
                if (binding == action->bindings.end())
                {
                    error =
                        "input action has no usable binding: "
                        + actionName;
                }
                // usable bindingからcontrolとscaleを取得します。
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
        // controlとactionのどちらも指定されない場合を処理します。
        else
        {
            error = "input requires control or action.";
        }
        // control解決に失敗したinputはsnapshotへ適用しません。
        if (!resolved)
        {
            // 不明control/actionを適用せず戻ります。
            return;
        }
        snapshot.Set(
            control,
            std::clamp(controlValue, -1.0f, 1.0f));
        frames = std::max(
            frames,
            RuntimeInputFrames(command));
    }
    // RuntimeScreenshotPath(directory: session directory, command: screenshot command, sequence: screenshot番号): session内のPNG pathを返します。
    [[nodiscard]] std::filesystem::path RuntimeScreenshotPath(
        const std::filesystem::path& directory,
        const nlohmann::json& command,
        const std::uint64_t sequence)
    {
        // path: session内へ保存するnormalized screenshot path。
        auto path = LamaPon::PathFromUtf8(
            command.value(
                "path",
                std::string{
                    "screenshot-"
                    + std::to_string(sequence)
                    + ".png" }));
        // screenshot pathがrelativeかつ空でないか確認します。
        if (path.empty() || path.is_absolute())
        {
            // session外または空のscreenshot pathを拒否します。
            throw std::invalid_argument(
                "runtime screenshot path must be relative.");
        }
        path = path.lexically_normal();
        // normalize後のpathがsession root外を指すか確認します。
        if (path.native().starts_with(L".."))
        {
            // session directory外への書き込みを拒否します。
            throw std::invalid_argument(
                "runtime screenshot path cannot leave the session folder.");
        }
        // session内に解決したscreenshot pathを返します。
        return directory / path;
    }
    // FindRuntimeObject(scene: active scene, command: selector JSON, allowAll: selector省略可否): 一致objectを返します。
    [[nodiscard]] LamaPon::GameObject* FindRuntimeObject(
        LamaPon::Scene& scene,
        const nlohmann::json& command,
        const bool allowAll = false)
    {
        // selector: command内の明示selectorまたはcommand本体。
        const auto& selector = command.contains("selector")
            ? command.at("selector")
            : command;
        // selectorにobject IDが指定されているか確認します。
        if (selector.contains("id")
            && selector.at("id").is_number_integer())
        {
            // id: selectorから取得したobject ID。
            const auto id = selector.at("id").get<std::int64_t>();
            // selector IDが負数でないか確認します。
            if (id < 0)
            {
                // 負数のobject IDをselectorとして拒否します。
                throw std::invalid_argument(
                    "runtime object id must not be negative.");
            }
            // 一致したIDのscene objectを返します。
            return scene.FindGameObject(
                static_cast<LamaPon::GameObjectId>(id));
        }
        // selector nameが指定されているか確認します。
        if (selector.contains("name")
            && selector.at("name").is_string())
        {
            // nameに一致するscene objectを返します。
            return scene.FindGameObjectByName(
                selector.at("name").get<std::string>());
        }
        // selector tagが指定されているか確認します。
        if (selector.contains("tag")
            && selector.at("tag").is_string())
        {
            // tagに一致するscene objectを返します。
            return scene.FindGameObjectByTag(
                selector.at("tag").get<std::string>());
        }
        // allowAll時はselectorなしを許容します。
        if (allowAll)
        {
            // selector不要のcommandをnullptrで表します。
            return nullptr;
        }
        // object selector不足を呼び出し元へ通知します。
        throw std::invalid_argument(
            "runtime object commands require selector.id, selector.name, or selector.tag.");
    }
    // ReadRuntimeVector(value: JSON vector, destination: float array, count: 要素数, name: field名): finite numberをdestinationへ読み込みます。
    void ReadRuntimeVector(
        const nlohmann::json& value,
        float* destination,
        const std::size_t count,
        const char* name)
    {
        // JSON valueが要求数のnumeric arrayか確認します。
        if (!value.is_array() || value.size() != count)
        {
            // 要素数またはarray型が不正ならvectorを拒否します。
            throw std::invalid_argument(
                std::string{ name }
                + " must be an array of "
                + std::to_string(count)
                + " numbers.");
        }
        // index: vector各要素を順に検証して格納する位置。
        for (std::size_t index = 0; index < count; ++index)
        {
            // 各vector要素がJSON numberか確認します。
            if (!value.at(index).is_number())
            {
                // number以外のvector要素を拒否します。
                throw std::invalid_argument(
                    std::string{ name } + " must contain only numbers.");
            }
            // number: finite vectorへ格納するfloat値。
            const auto number = value.at(index).get<float>();
            // 各vector要素がfiniteか確認します。
            if (!std::isfinite(number))
            {
                // NaN・infinityを含むvectorを拒否します。
                throw std::invalid_argument(
                    std::string{ name } + " must contain finite numbers.");
            }
            destination[index] = number;
        }
    }
    // RuntimeObjectSnapshot(snapshot: object JSON一覧, object: 対象object): IDに対応するsnapshot entryを返します。
    [[nodiscard]] nlohmann::json RuntimeObjectSnapshot(
        const nlohmann::json& snapshot,
        LamaPon::GameObject* object)
    {
        // snapshot対象objectが存在するか確認します。
        if (object == nullptr)
        {
            // objectがない場合に対応snapshotもありません。
            return nullptr;
        }
        // candidate: object IDを照合するsnapshot entry。
        for (const auto& candidate : snapshot.at("objects"))
        {
            // candidate entryのIDがobject IDと一致するか調べます。
            if (candidate.value<std::uint64_t>("id", 0)
                == object->Id())
            {
                // objectに対応するsnapshot entryを返します。
                return candidate;
            }
        }
        // object IDがsnapshot内にないことをnullptrで返します。
        return nullptr;
    }
    // ApplyRuntimeObjectCommand(operation: command名, command: command JSON, scene: active scene, graphics: device, gameModule: module host, state: session JSON): runtime object・state操作を適用します。
    void ApplyRuntimeObjectCommand(
        const std::string& operation,
        const nlohmann::json& command,
        LamaPon::Scene& scene,
        LamaPon::GraphicsDevice& graphics,
        LamaPon::GameModuleHost& gameModule,
        nlohmann::json& state)
    {
        // read-only query commandを処理します。
        if (operation == "query")
        {
            // snapshot: query結果用に取得したruntime snapshot。
            const auto snapshot = BuildRuntimeSnapshot(
                scene,
                graphics,
                false);
            // object: selectorに一致したquery対象object。
            auto* object = FindRuntimeObject(
                scene,
                command,
                true);
            state["lastQuery"] = object == nullptr
                ? snapshot.at("objects")
                : RuntimeObjectSnapshot(snapshot, object);
            // query結果をstateへ記録したため終了します。
            return;
        }
        // object transformまたは基本属性の更新を処理します。
        if (operation == "set-transform"
            || operation == "set-object")
        {
            // object: selectorに一致した変更対象object。
            auto* object = FindRuntimeObject(scene, command);
            // selectorからobjectを解決できたか確認します。
            if (object == nullptr)
            {
                // 一致するobjectがないcommandを拒否します。
                throw std::invalid_argument(
                    "runtime object selector did not match an object.");
            }
            // transform: objectのposition・rotation・scale。
            auto& transform = object->GetTransform();
            // position指定がある場合だけtransformを更新します。
            if (command.contains("position"))
            {
                ReadRuntimeVector(
                    command.at("position"),
                    &transform.position.x,
                    3,
                    "position");
            }
            // scale指定がある場合だけtransformを更新します。
            if (command.contains("scale"))
            {
                ReadRuntimeVector(
                    command.at("scale"),
                    &transform.scale.x,
                    3,
                    "scale");
            }
            // Euler rotation指定を読み取ります。
            if (command.contains("rotationEulerRadians"))
            {
                // euler: rotationEulerRadians fieldの検証結果。
                DirectX::XMFLOAT3 euler{};
                ReadRuntimeVector(
                    command.at("rotationEulerRadians"),
                    &euler.x,
                    3,
                    "rotationEulerRadians");
                transform.SetEulerAngles(euler);
            }
            // quaternion rotation指定を読み取ります。
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
            // object name指定がある場合だけ更新します。
            if (command.contains("name"))
            {
                object->SetName(command.at("name").get<std::string>());
            }
            // object tag指定がある場合だけ更新します。
            if (command.contains("tag"))
            {
                object->SetTag(command.at("tag").get<std::string>());
            }
            // enabled指定がある場合だけ更新します。
            if (command.contains("enabled"))
            {
                object->SetEnabled(command.at("enabled").get<bool>());
            }
            state["lastObject"] = object->Id();
            // object変更結果を記録してcommandを終えます。
            return;
        }
        // runtime game stateのscalar value更新を処理します。
        if (operation == "set-state")
        {
            // set-stateにstring keyとvalueがそろっているか確認します。
            if (!command.contains("key")
                || !command.at("key").is_string()
                || !command.contains("value"))
            {
                // 必須fieldがないset-state commandを拒否します。
                throw std::invalid_argument(
                    "set-state requires string key and value.");
            }
            // key: 更新するruntime stateのkey。
            const auto key = command.at("key").get<std::string>();
            // value: runtime stateへ設定するJSON value。
            const auto& value = command.at("value");
            // runtimeState: sceneが保持するnamed runtime state。
            auto& runtimeState = scene.Scenes().State();
            // boolean valueをruntime stateへ保存します。
            if (value.is_boolean())
            {
                runtimeState.SetBoolean(key, value.get<bool>());
            }
            // integer valueをruntime stateへ保存します。
            else if (value.is_number_integer())
            {
                runtimeState.SetInteger(
                    key,
                    value.get<std::int64_t>());
            }
            // その他のnumeric valueをruntime stateへ保存します。
            else if (value.is_number())
            {
                runtimeState.SetNumber(key, value.get<double>());
            }
            // string valueをruntime stateへ保存します。
            else if (value.is_string())
            {
                runtimeState.SetString(
                    key,
                    value.get<std::string>());
            }
            // null valueをruntime stateからkey削除として扱います。
            else if (value.is_null())
            {
                runtimeState.Remove(key);
            }
            // 対応外のcompound JSON valueを処理します。
            else
            {
                // scalar以外のset-state valueを拒否します。
                throw std::invalid_argument(
                    "set-state value must be a scalar.");
            }
            // game stateを更新したためcommandを終えます。
            return;
        }
        // 現在または指定されたsceneをreloadします。
        if (operation == "reload")
        {
            // requested: 明示sceneまたは現在sceneのUTF-8 path。
            const auto requested = command.value(
                "scene",
                LamaPon::PathToUtf8(
                    scene.Scenes().CurrentScenePath()));
            // scene loadとpending処理の両方が成功したか確認します。
            if (!scene.Scenes().RequestLoad(
                    LamaPon::PathFromUtf8(requested))
                || !scene.Scenes().ProcessPending())
            {
                // scene reload失敗をruntime errorとして通知します。
                throw std::runtime_error(
                    scene.Scenes().LastError().empty()
                        ? "runtime scene reload failed."
                        : scene.Scenes().LastError());
            }
            // sceneをreloadしたためcommandを終えます。
            return;
        }
        // Game Module reload commandを処理します。
        if (operation == "reload-module")
        {
            // Game Moduleをreloadできたか確認します。
            if (!gameModule.Reload())
            {
                // Game Module reload失敗を通知します。
                throw std::runtime_error(
                    gameModule.LastError().empty()
                        ? "runtime Game Module reload failed."
                        : gameModule.LastError());
            }
            state["gameModuleReloaded"] = true;
        }
    }
    // RuntimeFailureState(directory: session directory, exception: failure): failed session stateを作ります。
    [[nodiscard]] nlohmann::json RuntimeFailureState(
        const std::filesystem::path& directory,
        const std::exception& exception)
    {
        // state: runtime worker failureを表すJSON response。
        nlohmann::json state{
            { "version", RuntimeFileVersion },
            { "status", "failed" },
            { "progress", 1.0 },
            { "message", "Failed" },
            { "error", exception.what() },
            { "sessionDirectory",
                LamaPon::PathToUtf8(directory) },
        };
        // 失敗status JSONを返します。
        return state;
    }
    // RunRuntimeWorker(directory: session directory): requestを読み込みruntime game loopを実行します。
    [[nodiscard]] int RunRuntimeWorker(
        const std::filesystem::path& directory)
    {
        // request: session start時に保存されたruntime request。
        const auto request =
            ReadJsonFile(directory / L"request.json");
        // projectRoot: requestのproject pathをcanonical化したroot。
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    LamaPon::PathFromUtf8(
                        request.at("project")
                            .get<std::string>())));
        // settingsPath: project JSON設定のpath。
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // settings: runtime起動に使うproject settings。
        const auto settings =
            LamaPon::LoadProjectSettings(settingsPath);
        // scenePath: 起動sceneとして解決したnormalized path。
        const auto scenePath = NormalizeScenePath(
            projectRoot,
            LamaPon::PathFromUtf8(
                request.value("scene", std::string{}).empty()
                    ? LamaPon::PathToUtf8(settings.startupScene)
                    : request.at("scene").get<std::string>()));
        // width: 設定とrequestから決めたwindow pixel幅。
        const auto width = std::max<std::uint32_t>(
            request.value("width", settings.windowWidth),
            1u);
        // height: 設定とrequestから決めたwindow pixel高さ。
        const auto height = std::max<std::uint32_t>(
            request.value("height", settings.windowHeight),
            1u);
        // targetFrameRate: 1〜240へ制限した目標rate。
        const auto targetFrameRate = std::clamp<std::uint32_t>(
            request.value("targetFrameRate", 60u),
            1u,
            240u);
        // deterministic: requestのdeterministic mode指定。
        const bool deterministic = request.value(
            "deterministic",
            false);
        // fixedDeltaTime: 有効範囲へ制限したphysics step時間。
        const float fixedDeltaTime = std::clamp(
            request.value(
                "fixedDeltaTime",
                1.0f / static_cast<float>(targetFrameRate)),
            0.0001f,
            0.1f);
        // renderEveryNFrames: 描画間隔として使う制限済みframe数。
        const auto renderEveryNFrames = std::clamp<std::uint32_t>(
            request.value("renderEveryNFrames", 1u),
            1u,
            100'000u);
        // paceFrames: frameを目標rateに合わせる指定。
        const bool paceFrames = request.value("paceFrames", true);
        // frame pacingを無効にするならdeterministic modeが必要です。
        if (!paceFrames && !deterministic)
        {
            // 非deterministicのunpaced実行を拒否します。
            throw std::invalid_argument(
                "Unpaced runtime execution requires deterministic mode.");
        }
        // recordPath: replay commandを保存する任意path。
        const auto recordPath = request.value(
            "recordPath",
            std::string{}).empty()
            ? std::filesystem::path{}
            : LamaPon::PathFromUtf8(
                request.at("recordPath").get<std::string>());
        // replayCommands: 起動後に適用するJSON command列。
        const auto replayCommands = request.value(
            "replayCommands",
            nlohmann::json::array());
        // runtime replayCommandsがarrayか確認します。
        if (!replayCommands.is_array())
        {
            // array以外のreplay command列を拒否します。
            throw std::invalid_argument(
                "runtime replayCommands must be an array.");
        }
        // state: 起動中runtime sessionの更新対象JSON。
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
        // window: graphics deviceに渡す非表示window。
        const HWND window = CreateHiddenWindow(width, height);
        static_cast<void>(
            LamaPon::Logger::Instance().SetFilePath(
                directory / L"runtime.log"));
        // graphics: runtime renderingとinputを管理するdevice。
        LamaPon::GraphicsDevice graphics;
        LamaPon::SetGraphicsBackendPackageAssetRoot(
            projectRoot / L"assets");
        graphics.Initialize(
            window,
            width,
            height,
            settings.graphics.renderingApi,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalRenderer);
        graphics.Assets().SetAssetRoot(projectRoot / L"assets");
        // graphicsSettings: runtime用に調整するproject graphics設定。
        auto graphicsSettings = settings.graphics;
        graphicsSettings.vSyncEnabled = false;
        graphics.SetGraphicsSettings(graphicsSettings);
        graphics.SetAsyncShaderCompilationEnabled(false);
        graphics.Input().SetActions(settings.inputActions);
        LamaPon::SetActivePhysicsSettings(settings.physics);
        // gameModule: project native moduleのload/reload host。
        LamaPon::GameModuleHost gameModule;
        // gameModulePath: project内Game Module DLLのpath。
        const auto gameModulePath = projectRoot
            / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        // gameModuleLoaded: DLLを正常loadできたか。
        bool gameModuleLoaded = false;
        // gameModuleError: module loadに失敗または不在の理由。
        std::string gameModuleError;
            // assets/packages内のnative DLLをGame Moduleの探索pathへ登録します。
        gameModule.SetNativeSearchDirectories(
            LamaPon::PackageNativeSearchDirectories(
                LamaPon::ScanPackageNativeDependencies(
                    projectRoot / L"assets").packages));
        // Game Module DLLがある場合だけloadを試みます。
        if (std::filesystem::is_regular_file(gameModulePath))
        {
            gameModuleLoaded = gameModule.Load(gameModulePath);
            // Game Module load失敗をstateに記録します。
            if (!gameModuleLoaded)
            {
                gameModuleError = gameModule.LastError();
                LamaPon::Logger::Instance().Warning(
                    "Game Moduleを読み込めませんでした: "
                    + gameModuleError);
            }
        }
        // DLLがない場合はbuild必要理由を保存します。
        else
        {
            gameModuleError =
                "Game Moduleがありません（先にbuildしてください）。";
        }
        // scene: runtime game loopが更新するactive scene。
        LamaPon::Scene scene(graphics);
        scene.SetRegisteredTags(settings.tags);
        // 初期sceneのloadとpending処理を行います。
        if (!scene.Scenes().RequestLoad(scenePath)
            || !scene.Scenes().ProcessPending())
        {
            // runtime sceneのload失敗を通知します。
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
        // controlPath: 外部commandを書き込むcontrol JSON path。
        const auto controlPath = directory / L"control.json";
        // lastCommandSequence: 二重適用を防ぐ最後のcommand sequence。
        std::uint64_t lastCommandSequence{};
        // paused: scene updateを止めるpause状態。
        bool paused{};
        // stepRequested: pause中に一回だけupdateする指定。
        bool stepRequested{};
        // stopRequested: runtime loopを終了する指定。
        bool stopRequested{};
        // inputFrames: input command後に進めるframe数。
        std::uint32_t inputFrames{};
        // inputSnapshot: 今frameへ適用するinput状態。
        LamaPon::InputSnapshot inputSnapshot;
        // screenshotPath: 次のrender結果を書き出すpath。
        std::filesystem::path screenshotPath;
        // frameNumber: runtime loop開始後のframe通番。
        std::uint64_t frameNumber{};
        // replayIndex: 次に適用するreplay command位置。
        std::size_t replayIndex{};
        // replayDocument: 保存用runtime設定と実行command列。
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
        // writeReplay: recordPathへreplayDocumentを書き出すlambda。
        const auto writeReplay =
            [&recordPath, &replayDocument]
            {
                // recordPathが指定された場合だけreplay JSONを書きます。
                if (!recordPath.empty())
                {
                    WriteJsonFile(recordPath, replayDocument);
                }
            };
        writeReplay();
        // previousUpdate: 前frameのscene update時刻。
        auto previousUpdate = std::chrono::steady_clock::now();
        // previousPresentation: 前回frameをpresentした時刻。
        auto previousPresentation = previousUpdate;
        // lastStateWrite: state JSONを書いた直近時刻。
        auto lastStateWrite = previousUpdate;
        // writeState: state JSONと最終保存時刻を更新するlambda。
        const auto writeState =
            [&state, &directory, &lastStateWrite]
            {
                WriteRuntimeState(directory, state);
                lastStateWrite = std::chrono::steady_clock::now();
            };
        // stop commandが来るまでruntime loopを回します。
        while (!stopRequested)
        {
            // commandChanged: 新commandが適用されたframeか。
            bool commandChanged = false;
            // forceRender: 通常間隔を待たず描画する指定。
            bool forceRender = false;
            // 現在frame以前に指定されたreplay commandを適用します。
            if (replayIndex < replayCommands.size()
                && replayCommands.at(replayIndex).is_object()
                && replayCommands.at(replayIndex).value<std::uint64_t>(
                    "frame",
                    0) <= frameNumber)
            {
                // replayCommand: 現frameで適用するJSON command。
                auto replayCommand = replayCommands.at(replayIndex++)
                    .value(
                        "command",
                        nlohmann::json::object());
                replayCommand["seq"] = lastCommandSequence + 1;
                WriteJsonFile(controlPath, replayCommand);
            }
            // control JSONがある場合に新規commandを読みます。
            if (std::filesystem::is_regular_file(controlPath))
            {
                // control JSONを読み込んでcommandを処理します。
                try
                {
                    // command: control fileから読み込んだruntime operation。
                    const auto command = ReadJsonFile(controlPath);
                    // sequence: commandの重複適用を防ぐsequence number。
                    const auto sequence =
                        command.value<std::uint64_t>("seq", 0);
                    // 未処理のcommand sequenceだけ一度適用します。
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
                        // operation: command JSONから解決したoperation名。
                        const auto operation =
                            RuntimeCommandName(command);
                        // pause commandでruntime更新を停止します。
                        if (operation == "pause")
                        {
                            paused = true;
                        }
                        // resume commandでruntime更新を再開します。
                        else if (operation == "resume")
                        {
                            paused = false;
                        }
                        // step commandはpause中だけ受け付けます。
                        else if (operation == "step")
                        {
                            // step操作にpause状態が必要か確認します。
                            if (!paused)
                            {
                                // running中のstep commandを拒否します。
                                throw std::invalid_argument(
                                    "step requires a paused runtime.");
                            }
                            stepRequested = true;
                        }
                        // input commandをframe snapshotへ適用します。
                        else if (operation == "input")
                        {
                            // inputError: input commandの解決・適用error。
                            std::string inputError;
                            ApplyRuntimeInput(
                                command,
                                graphics.Input(),
                                inputSnapshot,
                                inputFrames,
                                inputError);
                            // input control解決時のerror有無を確認します。
                            if (!inputError.empty())
                            {
                                state["lastCommandOk"] = false;
                                state["lastCommandError"] = inputError;
                            }
                        }
                        // timescale commandを処理します。
                        else if (operation == "timescale")
                        {
                            // timescaleにfinite numberが指定されているか確認します。
                            if (!command.contains("value")
                                || !command.at("value").is_number())
                            {
                                // timescale値がない場合を拒否します。
                                throw std::invalid_argument(
                                    "timescale requires a numeric value.");
                            }
                            // value: timescaleに設定するfinite float値。
                            const float value =
                                command.at("value").get<float>();
                            // timescale valueがfiniteか確認します。
                            if (!std::isfinite(value))
                            {
                                // NaN・infinityのtimescale値を拒否します。
                                throw std::invalid_argument(
                                    "timescale must be finite.");
                            }
                            LamaPon::Time::SetTimeScale(value);
                        }
                        // screenshot commandで次回描画を保存します。
                        else if (operation == "screenshot")
                        {
                            screenshotPath = RuntimeScreenshotPath(
                                directory,
                                command,
                                sequence);
                            forceRender = true;
                        }
                        // observe commandは最新表示を確定してから応答します。
                        else if (operation == "observe")
                        {
                            // 描画を間引く高速テストでも、observeは現在の表示状態まで確定してから応答します。
                            forceRender = true;
                        }
                        // stop commandでruntime loopの終了を要求します。
                        else if (operation == "stop")
                        {
                            stopRequested = true;
                        }
                        // object・state・reload commandを委譲します。
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
                        // 未対応runtime operationを処理します。
                        else
                        {
                            // unknown operationをcommand errorとして拒否します。
                            throw std::invalid_argument(
                                "unknown runtime operation: "
                                + operation);
                        }
                        // recording指定があれば実行commandを保存します。
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
                // command適用errorをsession stateへ記録します。
                catch (const std::exception& exception)
                {
                    state["lastCommandOk"] = false;
                    state["lastCommandError"] = exception.what();
                    commandChanged = true;
                }
            }
            // stop commandを受けた場合は最終stateを保存します。
            if (stopRequested)
            {
                state["status"] = "stopped";
                state["message"] = "Stopped";
                state["replayIndex"] = replayIndex;
                state["replayComplete"] =
                    replayIndex >= replayCommands.size();
                writeState();
                // 最終stateを保存した後loopを終了します。
                break;
            }
            // current: 現frame開始時のsteady clock時刻。
            const auto current = std::chrono::steady_clock::now();
            // elapsed: 前updateからの実時間秒数。
            const auto elapsed = std::chrono::duration<float>(
                current - previousUpdate).count();
            previousUpdate = current;
            // timing: simulation/wall deltaを分けたframe timing。
            const auto timing = LamaPon::Cli::MakeRuntimeFrameTiming(
                elapsed,
                deterministic,
                fixedDeltaTime);
            LamaPon::Time::Detail::AdvanceFrame(
                timing.simulationDeltaSeconds);
            LamaPon::Profiler::Instance().BeginFrame();
            // inputFramesが残る間はsnapshot inputを維持します。
            if (inputFrames > 0)
            {
                graphics.Input().UpdateFromSnapshot(inputSnapshot);
                --inputFrames;
                // 最後のinput frame後にkey/button状態を消します。
                if (inputFrames == 0)
                {
                    // 最終input frame後にkey/button状態を次commandへ持ち越しません。
                    inputSnapshot.values.clear();
                }
            }
            // input frameがなければ空snapshotを適用します。
            else
            {
                graphics.Input().UpdateFromSnapshot({});
            }
            // simulate: scene updateを進めるframeか。
            const bool simulate = !paused || stepRequested;
            stepRequested = false;
            // pause中でないかstep要求時にsceneをupdateします。
            if (simulate)
            {
                scene.Update(LamaPon::Time::DeltaTime());
            }
            // Game Module hot reloadはwall clockで監視します。
            gameModule.PollHotReload(timing.wallDeltaSeconds);
            // renderFrame: 間引き間隔またはforce指定で描画するか。
            const bool renderFrame =
                LamaPon::Cli::ShouldRenderRuntimeFrame(
                    frameNumber,
                    renderEveryNFrames,
                    forceRender);
            // pixels: screenshotへ保存するRGBA backbuffer。
            std::vector<std::uint8_t> pixels;
            // capturedWidth: captureしたbackbufferのpixel幅。
            std::uint32_t capturedWidth{};
            // capturedHeight: captureしたbackbufferのpixel高さ。
            std::uint32_t capturedHeight{};
            // 描画対象フレームに限って描画と提示統計の更新を行います。
            if (renderFrame)
            {
                // clearColor: frame開始時に使うRGBA background。
                const float clearColor[4]{
                    0.025f, 0.035f, 0.055f, 1.0f };
                graphics.BeginFrame(clearColor);
                // 描画を開始してgame frameをrenderします。
                try
                {
                    scene.RenderGameFrame(clearColor);
                }
                // scene render例外をruntime logへ記録します。
                catch (const std::exception& exception)
                {
                    LamaPon::Logger::Instance().Error(
                        std::string{ "Runtime render failed: " }
                        + exception.what());
                }
                // requested screenshotがある時だけbackbufferをcaptureします。
                if (!screenshotPath.empty())
                {
                    pixels = graphics.CaptureBackBuffer(
                        capturedWidth,
                        capturedHeight);
                }
                graphics.EndFrame();
                // presentation: EndFrame後のpresent完了時刻。
                const auto presentation =
                    std::chrono::steady_clock::now();
                // presentationDelta: 前回presentからの経過秒数。
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
            // capture済みscreenshotを保存します。
            if (!screenshotPath.empty())
            {
                // screenshotDirectoryError: screenshot folder作成時のfilesystem error。
                std::error_code screenshotDirectoryError;
                std::filesystem::create_directories(
                    screenshotPath.parent_path(),
                    screenshotDirectoryError);
                // screenshot用directory作成の成否を確認します。
                if (screenshotDirectoryError)
                {
                    // screenshot directoryを作れない場合を通知します。
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
            // stateWriteDue: runtime JSONを外部へ更新するframeか。
            const bool stateWriteDue = frameNumber == 1u
                || commandChanged
                || std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()
                        - lastStateWrite).count() >= 250;
            // state snapshotを外部から観測できる時点で保存します。
            if (stateWriteDue)
            {
                state["runtime"] = BuildRuntimeSnapshot(
                    scene,
                    graphics,
                    paused);
                writeState();
            }
            // frame pacingが有効なruntimeを目標rateで待機させます。
            if (paceFrames)
            {
                // frameDuration: targetFrameRateから決まるframe間隔。
                const auto frameDuration =
                    std::chrono::duration<double>(
                        1.0 / static_cast<double>(targetFrameRate));
                // frameDeadline: 現frameのpaced update終了予定時刻。
                const auto frameDeadline = current
                    + std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                            frameDuration);
                // 次frame deadlineまで時間が残っているか確認します。
                if (std::chrono::steady_clock::now() < frameDeadline)
                {
                    std::this_thread::sleep_until(frameDeadline);
                }
            }
            // pacedでないpause中のruntimeに短くCPUを譲ります。
            else if (paused)
            {
                // pause中はゲーム状態が進まないため、命令待ちでCPUを占有しない程度だけ譲ります。
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(1));
            }
        }
        DestroyWindow(window);
        // runtime loopの正常終了を返します。
        return 0;
    }
    // RunRuntimeWorkerSafe(directory: session directory): runtime例外をfailed stateへ記録します。
    [[nodiscard]] int RunRuntimeWorkerSafe(
        const std::filesystem::path& directory)
    {
        // runtime worker failureをstateへ記録しながら実行します。
        try
        {
            // runtime workerの終了codeを返します。
            return RunRuntimeWorker(directory);
        }
        // runtime起動後の例外をfailed stateへ保存します。
        catch (const std::exception& exception)
        {
            // state: runtime workerのfailed response JSON。
            auto state = RuntimeFailureState(directory, exception);
            // failure stateの書き込みを試みます。
            try
            {
                WriteRuntimeState(directory, state);
            }
            // failure state保存時の例外を処理します。
            catch (const std::exception& stateException)
            {
                Progress(
                    "Could not record runtime failure: "
                    + std::string(stateException.what()));
            }
            // runtime worker failureを示すexit codeを返します。
            return 1;
        }
    }
    // RuntimeReport(command: CLI command, session: session情報): 標準JSON responseを作ります。
    [[nodiscard]] nlohmann::json RuntimeReport(
        const char* command,
        nlohmann::json session)
    {
        // runtime sessionの標準response JSONを返します。
        return {
            { "ok", true },
            { "command", command },
            { "session", std::move(session) },
        };
    }
    // StartRuntimeSession(options: 起動設定): session requestを保存してruntime workerを起動します。
    [[nodiscard]] RuntimeSessionHandle StartRuntimeSession(
        const RuntimeStartOptions& options)
    {
        // unpaced runtimeにはdeterministic modeを要求します。
        if (!options.paceFrames && !options.deterministic)
        {
            // 非deterministicのunpaced sessionを拒否します。
            throw std::invalid_argument(
                "--no-pace (or paceFrames:false) requires"
                " deterministic mode.");
        }
        // projectRoot: optionsからcanonical化するproject path。
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(options.projectRoot));
        // settingsPath: project識別に必要なsettings JSON path。
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // session pathにLamaPon project markerがあるか確認します。
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            // project settingsがないdirectoryを拒否します。
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        // settings: runtime startに使うproject settings。
        const auto settings =
            LamaPon::LoadProjectSettings(settingsPath);
        // root: runtime sessionを格納するproject directory。
        const auto root = RuntimeRoot(projectRoot);
        // directoryError: runtime root作成時のfilesystem error。
        std::error_code directoryError;
        std::filesystem::create_directories(root, directoryError);
        // runtime root directoryの作成結果を確認します。
        if (directoryError)
        {
            // runtime root作成失敗を通知します。
            throw std::runtime_error(
                "Could not create runtime root: "
                + directoryError.message());
        }
        // sessionId: 今回起動するruntime session ID。
        const auto sessionId = MakeRuntimeId();
        // directory: request・control・stateの保存先。
        const auto directory =
            root / LamaPon::PathFromUtf8(sessionId);
        std::filesystem::create_directories(directory);
        // scene: request sceneまたはproject startup scene。
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
        // state: queued runtime sessionの初期status JSON。
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
        // executable: 起動するLamaPonCli worker executable。
        const auto executable = JobExecutable();
        // workerArguments: runtime session path付きworker引数列。
        std::vector<std::wstring> workerArguments{
            executable.wstring(),
            L"runtime",
            L"worker",
            L"--session",
            directory.wstring(),
        };
        // commandLine: Windows用にquoteしたworker command。
        auto commandLine = BuildCommandLine(workerArguments);
        // startup: runtime workerの起動設定。
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // process: 起動したruntime worker process handle。
        PROCESS_INFORMATION process{};
        // runtime worker processの起動に成功したか確認します。
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
            // worker起動失敗をWin32 error付きで通知します。
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
        // 起動済みsession handleを返します。
        return {
            projectRoot,
            directory,
            sessionId,
            std::move(state),
        };
    }
    // RunRuntimeStart(options: runtime起動設定): sessionを起動しpoll情報を返します。
    [[nodiscard]] int RunRuntimeStart(
        const RuntimeStartOptions& options)
    {
        // session: 新規runtime sessionのhandleとstate。
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
        // runtime start responseを出力したことを成功codeで返します。
        return 0;
    }
    // ReadRuntimeStatus(projectRoot: project root, sessionId: session ID): 生存確認済みruntime stateを返します。
    [[nodiscard]] nlohmann::json ReadRuntimeStatus(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId)
    {
        // directory: statusを取得するruntime session directory。
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        // state: 読み込んで必要なら補正するsession state。
        auto state = RuntimeState(directory);
        // status: worker stateの現在値。
        const auto status = state.value("status", std::string{});
        // running中扱いのworkerが終了済みならfailure stateへ更新します。
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
        // 検査・補正したruntime statusを返します。
        return state;
    }
    // RunRuntimeStatus(projectRoot: project root, sessionId: session ID): runtime status JSONを出力します。
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
        // runtime status responseの成功を返します。
        return 0;
    }
    // SendRuntimeCommand(projectRoot: project root, sessionId: session ID, command: runtime operation JSON): 次のcommand sequenceを保存します。
    [[nodiscard]] nlohmann::json SendRuntimeCommand(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId,
        nlohmann::json command)
    {
        // directory: commandを送るsession directory。
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        // state: workerのcommand可否を調べるsession state。
        auto state = RuntimeState(directory);
        // status: runtime sessionの現在status。
        const auto status = state.value("status", std::string{});
        // queued・running・paused sessionだけcommandを受け付けます。
        if (status != "queued"
            && status != "running"
            && status != "paused")
        {
            // 停止済みruntimeへcommandを送るのを拒否します。
            throw std::runtime_error(
                "The runtime session is not running: " + status);
        }
        // session workerが稼働中か確認します。
        if (!IsJobProcessAlive(state.value("pid", 0u)))
        {
            // worker終了後のcommand送信を拒否します。
            throw std::runtime_error(
                "The runtime worker is no longer running.");
        }
        // control fileへJSON objectだけを送るか確認します。
        if (!command.is_object())
        {
            // JSON object以外のruntime commandを拒否します。
            throw std::invalid_argument(
                "The runtime command must be a JSON object.");
        }
        // acknowledged: workerが処理済みと確認したsequence。
        const auto acknowledged =
            state.value<std::uint64_t>("lastCommandSeq", 0);
        // existingSequence: control fileに残るcommand sequence。
        std::uint64_t existingSequence{};
        // 前回のcontrol file sequenceを読み取ります。
        try
        {
            // existing: control file内の未処理runtime command。
            const auto existing =
                ReadJsonFile(directory / L"control.json");
            existingSequence =
                existing.value<std::uint64_t>("seq", 0);
        }
        // control fileがまだない場合は未送信として扱います。
        catch (const std::exception&)
        {
        }
        // 前commandがack前に残っていないか確認します。
        if (existingSequence > acknowledged)
        {
            // ack待ちcommandが残る間の連続送信を拒否します。
            throw std::runtime_error(
                "The previous runtime command is still pending"
                " (seq " + std::to_string(existingSequence) + ").");
        }
        // sequence: 次にworkerへ送る新command sequence。
        const auto sequence =
            std::max(existingSequence, acknowledged) + 1;
        command["seq"] = sequence;
        WriteJsonFile(directory / L"control.json", command);
        // response: workerへ送ったcommandのacknowledgement JSON。
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
        // sequenceを付けて保存したcommand responseを返します。
        return response;
    }
    // RunRuntimeSend(projectRoot: project root, sessionId: session ID, command: runtime operation JSON): commandを保存してackを返します。
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
        // runtime send responseを出力した成功codeを返します。
        return 0;
    }
    // TerminateRuntimeSession(projectRoot: project root, sessionId: session ID, reason: failure reason): workerを終了しfailed stateを記録します。
    void TerminateRuntimeSession(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId,
        const std::string& reason)
    {
        // directory: 終了させるsession directory。
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        // state: worker process情報を持つruntime state。
        auto state = RuntimeState(directory);
        // processId: terminate対象runtime workerのID。
        const auto processId = state.value("pid", 0u);
        // sessionに記録されたworker process IDが有効か調べます。
        if (processId != 0)
        {
            // process: terminate可能なworker process handle。
            HANDLE process = OpenProcess(
                PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                FALSE,
                processId);
            // process terminate handleを取得できた場合だけ終了します。
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
    // RuntimeJsonPath(document: runtime snapshot JSON, path: selector path): 対応するvalueを返します。
    [[nodiscard]] const nlohmann::json* RuntimeJsonPath(
        const nlohmann::json& document,
        const std::string& path)
    {
        // current: document内でpath探索中のJSON value。
        const nlohmann::json* current = &document;
        // start: 現在のdot-separated token開始位置。
        std::size_t start{};
        // JSON pathのdot区切りfieldを順にたどります。
        while (start < path.size())
        {
            // dot: path内でtokenを区切るdot位置。
            const auto dot = path.find('.', start);
            // token: current JSON levelで処理するpath segment。
            const auto token = path.substr(
                start,
                dot == std::string::npos
                    ? std::string::npos
                    : dot - start);
            // empty path tokenはJSON keyとして使えません。
            if (token.empty())
            {
                // empty field tokenをpath不一致として返します。
                return nullptr;
            }
            // bracket: token内array selector開始位置。
            const auto bracket = token.find('[');
            // field: segment内のobject field名。
            const auto field = token.substr(
                0,
                bracket == std::string::npos
                    ? std::string::npos
                    : bracket);
            // current path tokenにobject fieldがあるか確認します。
            if (!field.empty())
            {
                // field名を読む対象がJSON objectか確認します。
                if (!current->is_object())
                {
                    // arrayやscalarをobject fieldとして扱いません。
                    return nullptr;
                }
                // fieldが存在しない場合はliteral key探索も試します。
                if (!current->contains(field))
                {
                    // literal: path残り全体を表すobject key候補。
                    const auto literal = path.substr(start);
                    // dotを含む残り全体のliteral keyがあるか確認します。
                    if (current->contains(literal))
                    {
                        // literal keyへ一致するJSON valueを返します。
                        return &current->at(literal);
                    }
                    // literal keyもないためpath不一致を返します。
                    return nullptr;
                }
                current = &current->at(field);
            }
            // selectorStart: 現在処理するarray selector開始位置。
            std::size_t selectorStart = bracket;
            // current tokenに続くarray selectorを処理します。
            while (selectorStart != std::string::npos)
            {
                // selectorEnd: selectorを閉じるbracket位置。
                const auto selectorEnd = token.find(
                    ']',
                    selectorStart + 1);
                // array selectorを閉じるbracketがあるか確認します。
                if (selectorEnd == std::string::npos)
                {
                    // 閉じbracketがないselectorを拒否します。
                    return nullptr;
                }
                // selector: indexまたはkey=value形式のarray条件。
                const auto selector = token.substr(
                    selectorStart + 1,
                    selectorEnd - selectorStart - 1);
                // current JSON valueがarrayならindex/key検索を行います。
                if (current->is_array())
                {
                    // selectorをnumeric indexとして解釈します。
                    try
                    {
                        // index: array positionとして変換したselector値。
                        const auto index = std::stoull(selector);
                        // numeric indexがarray範囲内か確認します。
                        if (index >= current->size())
                        {
                            // array範囲外indexはpath不一致です。
                            return nullptr;
                        }
                        current = &current->at(index);
                    }
                    // numeric indexでなければkey=value selectorを試します。
                    catch (const std::exception&)
                    {
                        // equal: key/value selector内のseparator位置。
                        const auto equal = selector.find('=');
                        // selectorにkey/value separatorがあるか確認します。
                        if (equal == std::string::npos)
                        {
                            // key/value separatorなしのselectorを拒否します。
                            return nullptr;
                        }
                        // key: object entry内でselectorが照合するfield名。
                        const auto key = selector.substr(0, equal);
                        // expected: selectorが要求するfield string value。
                        const auto expected = TrimRuntimeToken(
                            selector.substr(equal + 1));
                        // match: selector条件を満たしたarray entry。
                        const nlohmann::json* match = nullptr;
                        // item: key=value selectorに一致する次のarray entry。
                        for (const auto& item : *current)
                        {
                            // itemのstring fieldがselector valueに一致するか調べます。
                            if (item.is_object()
                                && item.contains(key)
                                && item.at(key).is_string()
                                && item.at(key).get<std::string>()
                                    == expected)
                            {
                                match = &item;
                                // 最初に一致したarray entryで探索を終えます。
                                break;
                            }
                        }
                        // selectorに一致するarray entryが見つかったか確認します。
                        if (match == nullptr)
                        {
                            // 一致entryがないselectorをpath不一致として返します。
                            return nullptr;
                        }
                        current = match;
                    }
                }
                // array以外のvalueへarray selectorが付いた場合を処理します。
                else
                {
                    // array以外へのselectorを拒否します。
                    return nullptr;
                }
                selectorStart = token.find('[', selectorEnd + 1);
            }
            // pathの最後のdotまで処理したか確認します。
            if (dot == std::string::npos)
            {
                // 残りのpath tokenがないためfield走査を終えます。
                break;
            }
            start = dot + 1;
        }
        // pathが指すJSON valueを返します。
        return current;
    }
    // RuntimeJsonNumberCompare(actual: 実測値, expected: 期待値, operation: 比較operator): 数値条件を判定します。
    [[nodiscard]] bool RuntimeJsonNumberCompare(
        const nlohmann::json& actual,
        const nlohmann::json& expected,
        const std::string& operation)
    {
        // numeric comparisonにはactualとexpectedの両方が必要です。
        if (!actual.is_number() || !expected.is_number())
        {
            // 比較対象がnumberでないことをfalseで返します。
            return false;
        }
        // left: numeric comparisonのactual値。
        const auto left = actual.get<double>();
        // right: numeric comparisonのexpected値。
        const auto right = expected.get<double>();
        // gt operatorなら左値が大きいか判定します。
        if (operation == "gt") return left > right;
        // gte operatorなら左値以上か判定します。
        if (operation == "gte") return left >= right;
        // lt operatorなら左値が小さいか判定します。
        if (operation == "lt") return left < right;
        // 残るlte operatorの比較結果を返します。
        return left <= right;
    }
    // RuntimeAssertionResult(document: snapshot JSON, assertion: assertion JSON): assertion結果entryを返します。
    [[nodiscard]] nlohmann::json RuntimeAssertionResult(
        const nlohmann::json& document,
        const nlohmann::json& assertion)
    {
        // path: assertionで参照するJSON selector path。
        const auto path = assertion.value("path", std::string{});
        // actual: pathが指すJSON valueまたは不在。
        const auto* actual = path.empty()
            ? nullptr
            : RuntimeJsonPath(document, path);
        // exists: selector pathがsnapshotに存在するか。
        const bool exists = actual != nullptr;
        // passed: exists指定で決まる初期assertion結果。
        bool passed = assertion.value("exists", true) == exists;
        // operatorName: assertionが指定する比較operator。
        std::string operatorName;
        // candidate: assertionで有効な比較operator名候補。
        for (const auto* candidate : {
                "equals", "notEquals", "gt", "gte", "lt", "lte",
                "contains" })
        {
            // 最初に指定された比較operatorを探します。
            if (assertion.contains(candidate))
            {
                operatorName = candidate;
                // operatorが見つかった時点で候補走査を終えます。
                break;
            }
        }
        // operator指定がある場合に比較結果を判定します。
        if (!operatorName.empty())
        {
            // expected: operatorでactualと比較するJSON value。
            const auto& expected = assertion.at(operatorName);
            // pathが存在しなければoperator assertionは失敗です。
            if (!exists)
            {
                passed = false;
            }
            // actualとexpectedのJSON equalityを比較します。
            else if (operatorName == "equals")
            {
                passed = *actual == expected;
            }
            // actualとexpectedが異なることを検査します。
            else if (operatorName == "notEquals")
            {
                passed = *actual != expected;
            }
            // 両方stringの場合にsubstring包含を検査します。
            else if (operatorName == "contains")
            {
                passed = actual->is_string()
                    && expected.is_string()
                    && actual->get<std::string>().find(
                        expected.get<std::string>())
                        != std::string::npos;
            }
            // operatorが数値比較なら指定演算子を適用します。
            else
            {
                passed = RuntimeJsonNumberCompare(
                    *actual,
                    expected,
                    operatorName);
            }
        }
        // actual・expected・pass結果を含むassertionを返します。
        return {
            { "path", path },
            { "passed", passed },
            { "actual", actual == nullptr ? nullptr : *actual },
            { "expected", assertion.contains(operatorName)
                ? assertion.at(operatorName)
                : nlohmann::json{} },
        };
    }
    // RuntimeCommandFromStep(step: runtime test step): step commandをJSONへ変換します。
    [[nodiscard]] nlohmann::json RuntimeCommandFromStep(
        const nlohmann::json& step)
    {
        // stepにcommandがあるか確認します。
        if (!step.contains("command"))
        {
            // commandのないstepに空objectを返します。
            return nlohmann::json::object();
        }
        // command: stepに指定されたruntime command value。
        const auto& command = step.at("command");
        // command valueがJSON objectなら直接使います。
        if (command.is_object())
        {
            // object commandをそのまま返します。
            return command;
        }
        // command valueがtextなら簡易表記として解析します。
        if (command.is_string())
        {
            // text commandをruntime JSONへ変換して返します。
            return ParseRuntimeCommandText(
                LamaPon::Utf8ToWide(command.get<std::string>()));
        }
        // objectかtext以外のstep commandを拒否します。
        throw std::invalid_argument(
            "runtime test step command must be an object or string.");
    }
    // RuntimeTestWait(projectRoot: project root, sessionId: session ID, deadline: timeout, minimumFrame: 最小frame, minimumCommandSequence: 最小ack): 条件成立までpollします。
    void RuntimeTestWait(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId,
        const std::chrono::steady_clock::time_point deadline,
        const std::uint64_t minimumFrame = 0,
        const std::uint64_t minimumCommandSequence = 0)
    {
        // command ackまたはminimum frameまで待ちます。
        while (true)
        {
            // state: wait中にpollしたruntime status JSON。
            const auto state = ReadRuntimeStatus(
                projectRoot,
                sessionId);
            // status: pollしたruntime sessionの現在status。
            const auto status = state.value(
                "status",
                std::string{});
            // worker failureを検出してwaitを中止します。
            if (status == "failed")
            {
                // runtime failure stateのerrorを通知します。
                throw std::runtime_error(
                    state.value(
                        "error",
                        std::string{
                            "runtime worker failed." }));
            }
            // commandCompleted: minimum sequenceまでack済みか。
            const bool commandCompleted =
                state.value<std::uint64_t>("lastCommandSeq", 0)
                    >= minimumCommandSequence;
            // runtimeが停止した場合のack状態を確認します。
            if (status == "stopped")
            {
                // 停止前にcommandがackされた場合を確認します。
                if (commandCompleted)
                {
                    // command ack済みなら待機を正常終了します。
                    return;
                }
                // 未ack commandのまま停止したことを通知します。
                throw std::runtime_error(
                    "runtime stopped before acknowledging a command.");
            }
            // 要求frame到達とcommand ackを確認します。
            if (state.value<std::uint64_t>("frame", 0)
                    >= minimumFrame
                && commandCompleted)
            {
                // frame条件とack条件の両方が満たされれば戻ります。
                return;
            }
            // 期限切れでないか確認します。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                // frameまたはack待機のtimeoutを通知します。
                throw std::runtime_error(
                    "runtime test timed out while waiting for a frame"
                    " or command acknowledgement.");
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));
        }
    }
    // ResolveRuntimeInputFile(projectRoot: project root, requested: input path): 存在するpathを解決します。
    [[nodiscard]] std::filesystem::path ResolveRuntimeInputFile(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& requested)
    {
        // requested pathをproject rootと結合する必要があるか判断します。
        if (requested.is_absolute()
            || std::filesystem::exists(requested))
        {
            // absolute pathまたは現在位置から見つかったfileを返します。
            return requested;
        }
        // project root基準に解決したinput fileを返します。
        return projectRoot / requested;
    }
    // RunRuntimeTest(projectRoot: project root, specPath: test spec): sessionを実行しassertion reportを出力します。
    [[nodiscard]] int RunRuntimeTest(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& specPath)
    {
        // spec: runtime testのstepとassertion定義。
        const auto spec = ReadJsonFile(
            ResolveRuntimeInputFile(projectRoot, specPath));
        // options: specから組み立てるruntime起動設定。
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
        // record output指定がある場合に保存pathを解決します。
        if (spec.contains("record"))
        {
            options.recordPath = std::filesystem::absolute(
                projectRoot
                / LamaPon::PathFromUtf8(
                    spec.at("record").get<std::string>()));
        }
        // timeout: test全体へ割り当てる最大待機時間。
        const auto timeout = std::chrono::milliseconds(
            std::max(
                spec.value("timeoutMs", 10000),
                100));
        // deadline: testが完了すべきsteady clock時刻。
        const auto deadline =
            std::chrono::steady_clock::now() + timeout;
        // session: testが起動するruntime session handle。
        RuntimeSessionHandle session;
        // assertions: 各step・final assertionの結果配列。
        nlohmann::json assertions = nlohmann::json::array();
        // passed: すべてのassertionが成功したか。
        bool passed = false;
        // error: test実行またはcleanup時のfailure text。
        std::string error;
        // runtime test sessionを起動してstepを実行します。
        try
        {
            session = StartRuntimeSession(options);
            // sessionId: runtime APIへ渡すwide session identifier。
            const auto sessionId =
                LamaPon::Utf8ToWide(session.sessionId);
            RuntimeTestWait(
                projectRoot,
                sessionId,
                deadline,
                1);
            // specで定義されたruntime test stepを順に実行します。
            for (const auto& step : spec.value(
                "steps",
                nlohmann::json::array()))
            {
                // command: stepから構成したruntime command JSON。
                const auto command = RuntimeCommandFromStep(step);
                // stepに送るruntime commandがある場合だけ送信します。
                if (!command.empty())
                {
                    // response: command送信時に返るack JSON。
                    const auto response = SendRuntimeCommand(
                        projectRoot,
                        sessionId,
                        command);
                    // sequence: workerが処理するcommand acknowledgement番号。
                    const auto sequence =
                        response.at("seq").get<std::uint64_t>();
                    // screenshot commandはPNG保存後にackされます。
                    RuntimeTestWait(
                        projectRoot,
                        sessionId,
                        deadline,
                        0,
                        sequence);
                }
                // waitFrames: step後に進める追加runtime frame数。
                const auto waitFrames = step.value<std::uint64_t>(
                    "waitFrames",
                    1);
                // current: step command後に読むruntime status。
                const auto current = ReadRuntimeStatus(
                    projectRoot,
                    sessionId);
                RuntimeTestWait(
                    projectRoot,
                    sessionId,
                    deadline,
                    current.value<std::uint64_t>("frame", 0)
                        + waitFrames);
                // step内にruntime assertionがある場合に評価します。
                if (step.contains("assert"))
                {
                    // assertion: step後のruntime stateで判定する条件。
                    for (const auto& assertion : step.at("assert"))
                    {
                        // state: 各assertionへ渡す最新runtime status。
                        const auto state = ReadRuntimeStatus(
                            projectRoot,
                            sessionId);
                        assertions.push_back(
                            RuntimeAssertionResult(state, assertion));
                    }
                }
                // stepにwaitMsがあれば指定時間待ちます。
                if (step.contains("waitMs"))
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(
                            step.at("waitMs").get<std::uint32_t>()));
                }
            }
            // finalState: step完了後に評価する最終snapshot。
            const auto finalState = ReadRuntimeStatus(
                projectRoot,
                sessionId);
            // spec全体に定義されたfinal assertionを評価します。
            for (const auto& assertion : spec.value(
                "assert",
                nlohmann::json::array()))
            {
                assertions.push_back(
                    RuntimeAssertionResult(finalState, assertion));
            }
            // predicate(result: runtime assertion result): passed fieldを検査します。
            passed = std::ranges::all_of(
                assertions,
                [](const auto& result)
                {
                    // all assertionがpassedかを判定します。
                    return result.value("passed", false);
                });
            // stopResponse: cleanup stop commandのack JSON。
            const auto stopResponse = SendRuntimeCommand(
                projectRoot,
                sessionId,
                { { "op", "stop" } });
            // cleanupDeadline: stop ack待ちに使うcleanup期限。
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
        // runtime test failureをreport用errorへ保存します。
        catch (const std::exception& exception)
        {
            error = exception.what();
            // sessionが起動済みならfailure後に終了させます。
            if (!session.sessionId.empty())
            {
                // failed workerをterminateしてstateを補修します。
                try
                {
                    TerminateRuntimeSession(
                        projectRoot,
                        LamaPon::Utf8ToWide(session.sessionId),
                        error);
                }
                // cleanup failureは元のtest errorを保持して無視します。
                catch (const std::exception&)
                {
                }
            }
        }
        // report: runtime testの結果とassertion JSON。
        nlohmann::json report{
            { "ok", passed && error.empty() },
            { "command", "runtime test" },
            { "sessionId", session.sessionId },
            { "assertions", std::move(assertions) },
        };
        // failure textがある場合だけreportへ追加します。
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
        // assertion成功とerror不在をtest exit codeで返します。
        return report.at("ok").get<bool>() ? 0 : 1;
    }
    // RunRuntimeReplay(projectRoot: project root, replayPath: replay file): replayを実行して最終statusを返します。
    [[nodiscard]] int RunRuntimeReplay(
        const std::filesystem::path& projectRoot,
        const std::filesystem::path& replayPath)
    {
        // replay: replay fileから読み込むsession設定。
        const auto replay = ReadJsonFile(
            ResolveRuntimeInputFile(projectRoot, replayPath));
        // options: replay設定から作るruntime起動options。
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
        // session: replay実行workerのsession handle。
        const auto session = StartRuntimeSession(options);
        // sessionId: replay workerをpollするwide ID。
        const auto sessionId = LamaPon::Utf8ToWide(session.sessionId);
        // deadline: replay全体の30秒timeout時刻。
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(30'000);
        // replay完了またはworker停止までstatusをpollします。
        while (true)
        {
            // state: replay workerの最新status JSON。
            const auto state = ReadRuntimeStatus(
                projectRoot,
                sessionId);
            // failure・stop・全replay完了のいずれかを検出します。
            if (state.value("status", std::string{}) == "failed"
                || state.value("status", std::string{}) == "stopped"
                || state.value("replayComplete", false))
            {
                // 完了条件を検出したためpoll loopを抜けます。
                break;
            }
            // replay timeout deadlineを超えたか確認します。
            if (std::chrono::steady_clock::now() >= deadline)
            {
                TerminateRuntimeSession(
                    projectRoot,
                    sessionId,
                    "runtime replay timed out.");
                // timeoutしたreplay workerのerrorを通知します。
                throw std::runtime_error(
                    "runtime replay timed out.");
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(25));
        }
        // finalState: replay終了時に取得したsession state。
        const auto finalState = ReadRuntimeStatus(
            projectRoot,
            sessionId);
        // finalStatus: replay完了時のruntime status文字列。
        const auto finalStatus = finalState.value(
            "status",
            std::string{});
        // queued・running・pausedのsessionだけstopを送ります。
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
        // final statusがfailedかをreplay exit codeへ反映します。
        return finalStatus == "failed"
            ? 1
            : 0;
    }
    // RunRuntimeRecover(projectRoot: project root, sessionId: stopped session ID): 保存requestからworkerを再起動します。
    [[nodiscard]] int RunRuntimeRecover(
        const std::filesystem::path& projectRoot,
        const std::wstring_view sessionId)
    {
        // directory: recovery対象session directory。
        const auto directory = RuntimeDirectory(projectRoot, sessionId);
        // state: 旧runtime workerの終了status。
        const auto state = RuntimeState(directory);
        // recovery前に旧workerが停止しているか確認します。
        if (IsJobProcessAlive(state.value("pid", 0u)))
        {
            // 稼働中sessionのrecoveryを拒否します。
            throw std::runtime_error(
                "The runtime session is still running.");
        }
        // request: recovery元sessionのstart request JSON。
        const auto request = ReadJsonFile(directory / L"request.json");
        // options: saved requestから復元するruntime起動設定。
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
        // replacement: 保存済みrequestで起動した新session。
        const auto replacement = StartRuntimeSession(options);
        // response: previous session IDとreplacement sessionを返すJSON。
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
        // runtime recovery responseを出力した成功codeを返します。
        return 0;
    }
    // AssetRecordJson(assetRoot: assets directory, record: asset metadata): file pathと依存情報をJSON化します。
    [[nodiscard]] nlohmann::json AssetRecordJson(
        const std::filesystem::path& assetRoot,
        const LamaPon::AssetRecord& record)
    {
        // dependencies: assetが参照するGUID列。
        nlohmann::json dependencies = nlohmann::json::array();
        // dependency: assetが参照する次のGUID。
        for (const auto& dependency : record.dependencies)
        {
            dependencies.push_back(dependency);
        }
        // dependents: assetを参照するGUID列。
        nlohmann::json dependents = nlohmann::json::array();
        // dependent: このassetを参照する次のGUID。
        for (const auto& dependent : record.dependents)
        {
            dependents.push_back(dependent);
        }
        // absolutePath: asset fileのcanonical absolute path。
        const auto absolutePath =
            std::filesystem::weakly_canonical(
                assetRoot / record.path);
        // metaPath: asset metadata fileのcanonical path。
        const auto metaPath =
            std::filesystem::weakly_canonical(
                assetRoot / record.metaPath);
        // result: CLIへ返すasset record JSON。
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
        // meta JSONが存在する場合だけ詳細を読み込みます。
        if (std::filesystem::is_regular_file(metaPath))
        {
            // asset meta JSONを読み込みます。
            try
            {
                result["meta"] = ReadJsonFile(metaPath);
            }
            // exception: meta file読み込みerrorをreportへ記録します。
            catch (const std::exception& exception)
            {
                result["metaError"] = exception.what();
            }
        }
        // meta fileがないassetはnullとして返します。
        else
        {
            result["meta"] = nullptr;
        }
        // 完成したasset record JSONを返します。
        return result;
    }
    // RunAssetCommand(requestedProject: project root, action: asset command, requestedPath: asset path, requestedGuid: asset GUID, importerFilter: importer name): asset query JSONを出力します。
    [[nodiscard]] int RunAssetCommand(
        const std::filesystem::path& requestedProject,
        const std::wstring& action,
        const std::filesystem::path& requestedPath,
        const std::string& requestedGuid,
        const std::string& importerFilter)
    {
        // projectRoot: asset command対象project root。
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        // assetRoot: project内assets directory。
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        // database: refreshed asset index。
        LamaPon::AssetDatabase database;
        database.SetAssetRoot(assetRoot);
        // refresh: refreshで作成したmetadata等の集計値。
        const auto refresh = database.Refresh(false);
        // asset list actionの集計を行います。
        if (action == L"list")
        {
            // assets: filter後のasset record JSON配列。
            nlohmann::json assets = nlohmann::json::array();
            // importerCounts: importer名ごとのasset件数。
            std::unordered_map<std::string, std::size_t> importerCounts;
            // record: importer別に集計して返すasset。
            for (const auto& record : database.Assets())
            {
                // importerFilterが指定された時は一致recordだけ残します。
                if (!importerFilter.empty()
                    && record.importer != importerFilter)
                {
                    // filter対象外のasset recordを飛ばします。
                    continue;
                }
                ++importerCounts[record.importer];
                assets.push_back(AssetRecordJson(assetRoot, record));
            }
            // counts: responseへ返すimporter count object。
            nlohmann::json counts = nlohmann::json::object();
            // importer/count: 各importerのasset件数。
            for (const auto& [importer, count] : importerCounts)
            {
                counts[importer] = count;
            }
            // report: asset listの公開JSON response。
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
            // asset list JSONを出力した成功codeを返します。
            return 0;
        }
        // asset inspect actionの詳細を返します。
        if (action == L"inspect")
        {
            // pathまたはGUIDのどちらかが必要か確認します。
            if (requestedPath.empty() && requestedGuid.empty())
            {
                // selectorがないasset inspect requestを拒否します。
                throw std::invalid_argument(
                    "asset inspect requires --path or --guid.");
            }
            // record: inspectするasset database record。
            const LamaPon::AssetRecord* record{};
            // GUIDがあればpathより優先してrecordを探します。
            if (!requestedGuid.empty())
            {
                record = database.FindByGuid(requestedGuid);
            }
            // GUID指定がない時はnormalized pathで探します。
            else
            {
                // relative: project-relative normalized asset path。
                const auto relative = NormalizeAssetPath(
                    projectRoot,
                    requestedPath);
                record = database.FindByPath(relative);
            }
            // asset databaseからrecordを解決できたか確認します。
            if (record == nullptr)
            {
                // 存在しないasset selectorを呼び出し元へ通知します。
                throw std::runtime_error(
                    "The requested asset was not found.");
            }
            // report: asset inspectの公開JSON response。
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
            // asset inspect JSONを出力した成功codeを返します。
            return 0;
        }
        // 未対応asset actionを拒否します。
        throw std::invalid_argument(
            "Unknown asset action: "
            + LamaPon::PathToUtf8(std::filesystem::path(action)));
    }
    // RunAssetImport(requestedProject: project root, sources: import元files, requestedTarget: 配置先): import結果JSONを出力します。
    [[nodiscard]] int RunAssetImport(
        const std::filesystem::path& requestedProject,
        const std::vector<std::filesystem::path>& sources,
        const std::filesystem::path& requestedTarget)
    {
        // import元fileが一件以上あるか確認します。
        if (sources.empty())
        {
            // source指定なしのasset importを拒否します。
            throw std::invalid_argument(
                "asset import requires at least one --source.");
        }
        // projectRoot: asset import対象project root。
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        // assetRoot: import fileの配置先directory。
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        // target: project内に解決したimport destination。
        const auto target = NormalizeAssetPath(
            projectRoot,
            requestedTarget.empty()
                ? std::filesystem::path{ L"." }
                : requestedTarget);
        // result: importerの成功・失敗と件数。
        const auto result = LamaPon::AssetImporter::Import(
            sources,
            assetRoot,
            target);
        // files: importされたfile別のresponse entry。
        nlohmann::json files = nlohmann::json::array();
        // file: import結果の次の成功file。
        for (const auto& file : result.files)
        {
            files.push_back({
                { "source", LamaPon::PathToUtf8(file.source) },
                { "path", LamaPon::PathToUtf8(file.path) },
                { "renamed", file.renamed },
            });
        }
        // failures: importできなかったsource別のentry。
        nlohmann::json failures = nlohmann::json::array();
        // failure: import結果の次の失敗file。
        for (const auto& failure : result.failures)
        {
            failures.push_back({
                { "source", LamaPon::PathToUtf8(failure.source) },
                { "message", failure.message },
            });
        }
        // report: asset importの公開JSON response。
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
        // import失敗がなければ成功codeを返します。
        return failures.empty() ? 0 : 1;
    }
    // IsCppIdentifier(value: class name候補): C++ identifierとして有効か判定します。
    [[nodiscard]] bool IsCppIdentifier(
        const std::string& value)
    {
        // 先頭文字がdigitでないidentifierか確認します。
        if (value.empty()
            || (value.front() >= '0' && value.front() <= '9'))
        {
            // emptyまたはdigit開始のC++ identifierを拒否します。
            return false;
        }
        // 全characterがC++ identifier文字か検査します。
        // predicate(character: candidate identifier文字): 使用可能文字か返します。
        return std::ranges::all_of(
            value,
            [](const unsigned char character)
            {
                // character: C++ identifierとして許可する文字か判定します。
                return (character >= 'a' && character <= 'z')
                    || (character >= 'A' && character <= 'Z')
                    || (character >= '0' && character <= '9')
                    || character == '_';
            });
    }
    // RunScriptCreate(requestedProject: project root, requestedPath: 出力path, className: 生成class名, force: 上書き許可): script sourceを生成します。
    [[nodiscard]] int RunScriptCreate(
        const std::filesystem::path& requestedProject,
        const std::filesystem::path& requestedPath,
        const std::string& className,
        const bool force)
    {
        // 入力class名がC++ identifierとして有効か確認します。
        if (!IsCppIdentifier(className))
        {
            // 不正なC++ class名でscriptを生成しません。
            throw std::invalid_argument(
                "script create requires a valid C++ class name.");
        }
        // projectRoot: scriptを生成するproject root。
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        // assetRoot: script sourceを置くassets directory。
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        // relative: assets基準で解決したscript path。
        const auto relative = NormalizeAssetPath(
            projectRoot,
            requestedPath);
        // script sourceの拡張子が.cppか確認します。
        if (relative.extension() != L".cpp")
        {
            // .cpp以外のscript output pathを拒否します。
            throw std::invalid_argument(
                "script create path must use the .cpp extension.");
        }
        // destination: script fileの出力先absolute path。
        const auto destination = assetRoot / relative;
        // existed: destinationに既存regular fileがあるか。
        const bool existed =
            std::filesystem::is_regular_file(destination);
        // 既存scriptをforceなしで上書きするか確認します。
        if (existed && !force)
        {
            // 既存sourceの無断上書きを拒否します。
            throw std::runtime_error(
                "The script already exists; use --force to overwrite: "
                + LamaPon::PathToUtf8(relative));
        }
        // source: assetsへ書き込む生成script source text。
        const std::string source =
            "#include \"LamaPon/LamaPon.h\"\n"
            "\n"
            "class " + className
            + " final : public LamaPon::Script\n"
              "{\n"
              "public:\n"
              "    // Start(): script初期化時に一度呼び出されます。\n"
              "    void Start() override\n"
              "    {\n"
              "    }\n"
              "\n"
              "    // Update(deltaTime: 経過秒数): 各frameのscript更新を行います。\n"
              "    void Update(const float deltaTime) override\n"
              "    {\n"
              "        static_cast<void>(deltaTime);\n"
              "    }\n"
              "};\n"
              "\n"
              "LAMAPON_SCRIPT(" + className + ");\n";
        WriteTextAtomic(destination, source);
        // report: script createの公開JSON response。
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
        // script source generation reportを返します。
        return 0;
    }
    // ReadTextFile(path: 読み取りfile): 内容をbinary-safeなstringで返します。
    [[nodiscard]] std::string ReadTextFile(
        const std::filesystem::path& path)
    {
        // input: 読み取るbinary file stream。
        std::ifstream input(path, std::ios::binary);
        // file全体を読み取ったstringを返します。
        return std::string{
            std::istreambuf_iterator<char>{ input },
            std::istreambuf_iterator<char>{} };
    }
    // ScriptTypesInSource(contents: C++ source): build前にGame.<class> registration名を抽出します。
    [[nodiscard]] nlohmann::json ScriptTypesInSource(
        const std::string& contents)
    {
        // types: source内から抽出したscript registration列。
        nlohmann::json types = nlohmann::json::array();
        struct Macro final
        {
            // name: 登録macro名。
            const char* name;
            // hasExplicitId: macroが明示的なregistration IDを持つか。
            bool hasExplicitId;
        };
        // Macros: 長い名前から走査するregistration macro一覧。
        static constexpr Macro Macros[]{
            { "LAMAPON_SCRIPT_WITH_SCHEMA", true },
            { "LAMAPON_SCRIPT_NAMED", true },
            { "LAMAPON_SCRIPT", false },
        };
        // consumed: 解析済みmacro開始offsetの集合。
        std::vector<std::size_t> consumed;
        // macro: 登録名を探す次のscript registration macro。
        for (const auto& macro : Macros)
        {
            // token: 現在検索しているmacro名。
            const std::string token{ macro.name };
            // search: 次にcontents.findを始めるoffset。
            std::size_t search = 0;
            // contents内でmacro tokenの各出現を探します。
            while ((search = contents.find(token, search))
                != std::string::npos)
            {
                // at: 現macro tokenの開始offset。
                const std::size_t at = search;
                search += token.size();
                // macro tokenの開始位置を一度だけ解析します。
                if (std::ranges::find(consumed, at)
                    != consumed.end())
                {
                    // consumed offsetにあるmacroを二重解析しません。
                    continue;
                }
                // open: macro argumentを始めるopening parenthesis位置。
                const auto open = contents.find_first_not_of(
                    " \t",
                    search);
                // macro nameの直後にopening parenthesisがあるか確認します。
                if (open == std::string::npos
                    || contents[open] != '(')
                {
                    // 呼び出し形でないmacro tokenを飛ばします。
                    continue;
                }
                // close: macro argumentを終えるclosing parenthesis位置。
                const auto close = contents.find(')', open);
                // macro argument listの閉じparenthesisを探します。
                if (close == std::string::npos)
                {
                    // 未完のmacro callを解析対象から除きます。
                    continue;
                }
                consumed.push_back(at);
                // arguments: parenthesis内の未分割argument text。
                auto arguments = contents.substr(
                    open + 1,
                    close - open - 1);
                // parts: trim後に分割したmacro argument列。
                std::vector<std::string> parts;
                // partStart: 次のcomma-separated argument開始位置。
                std::size_t partStart = 0;
                // commaで区切ったmacro argumentsを順に分割します。
                while (partStart <= arguments.size())
                {
                    // comma: 現在argumentの後にあるcomma位置。
                    const auto comma =
                        arguments.find(',', partStart);
                    // part: comma間の一つのmacro argument text。
                    auto part = arguments.substr(
                        partStart,
                        comma == std::string::npos
                            ? std::string::npos
                            : comma - partStart);
                    partStart = comma == std::string::npos
                        ? arguments.size() + 1
                        : comma + 1;
                    // first: argument先頭の非space文字位置。
                    const auto first =
                        part.find_first_not_of(" \t\r\n\"");
                    // trim後のargumentに登録名が残っているか確認します。
                    if (first == std::string::npos)
                    {
                        // 空白だけのargumentはpartsへ追加しません。
                        continue;
                    }
                    // last: argument末尾の非space文字位置。
                    const auto last =
                        part.find_last_not_of(" \t\r\n\"");
                    parts.push_back(
                        part.substr(first, last - first + 1));
                }
                // class名を含むmacro argumentがあるか確認します。
                if (parts.empty())
                {
                    // 登録情報のないmacro callを飛ばします。
                    continue;
                }
                // className: macroに登録されたC++ class名。
                const std::string className = parts.front();
                // id: runtimeで使用するscript registration名。
                const std::string id =
                    macro.hasExplicitId && parts.size() >= 2
                        ? parts[1]
                        : "Game." + className;
                // idはNativeScript componentへ保存するregistration名です。
                types.push_back({
                    { "class", className },
                    { "id", id },
                });
            }
        }
        // macroから抽出したscript registration JSONを返します。
        return types;
    }
    // LineCount(contents: source text): LFと末尾改行を反映した行数を返します。
    [[nodiscard]] std::size_t LineCount(
        const std::string& contents)
    {
        // 空sourceにlineは存在しません。
        if (contents.empty())
        {
            // empty sourceのline countを返します。
            return 0;
        }
        // newlineCount: source内に含まれるLF数。
        const auto newlineCount = static_cast<std::size_t>(
            std::count(contents.begin(), contents.end(), '\n'));
        // 末尾newline有無を反映したline countを返します。
        return contents.back() == '\n'
            ? newlineCount
            : newlineCount + 1;
    }
    // RunScriptCommand(requestedProject: project root, action: script command, requestedPath: script path): script metadata JSONを出力します。
    [[nodiscard]] int RunScriptCommand(
        const std::filesystem::path& requestedProject,
        const std::wstring& action,
        const std::filesystem::path& requestedPath)
    {
        // projectRoot: script command対象project root。
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        // assetRoot: project内assets directory。
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        // database: script asset index。
        LamaPon::AssetDatabase database;
        database.SetAssetRoot(assetRoot);
        static_cast<void>(database.Refresh(false));
        // script list actionの一覧を作ります。
        if (action == L"list")
        {
            // scripts: responseへ返すCppScript asset一覧。
            nlohmann::json scripts = nlohmann::json::array();
            // record: CppScript importerで登録された次のasset。
            for (const auto& record : database.Assets())
            {
                // CppScript importerのassetだけを一覧に残します。
                if (record.importer != "CppScript")
                {
                    // script以外のasset recordを飛ばします。
                    continue;
                }
                // absolute: record.pathを結合したscript source path。
                const auto absolute = assetRoot / record.path;
                // sizeError: script file byte size取得時のfilesystem error。
                std::error_code sizeError;
                // size: script sourceのbyte数。
                const auto size = std::filesystem::file_size(
                    absolute,
                    sizeError);
                // シーンJSONへ書く登録名。
                // scriptTypes.idはscene JSONのNativeScript登録値です。
                scripts.push_back({
                    { "guid", record.guid },
                    { "path", LamaPon::PathToUtf8(record.path) },
                    { "bytes", sizeError ? 0 : size },
                    { "scriptTypes",
                        ScriptTypesInSource(
                            ReadTextFile(absolute)) },
                    { "dependencies", record.dependencies },
                    { "dependents", record.dependents },
                });
            }
            // report: script listの公開JSON response。
            const nlohmann::json report{
                { "ok", true },
                // script inspect JSONを出力した成功codeを返します。
                { "command", "script list" },
                { "project", LamaPon::PathToUtf8(projectRoot) },
                // 未対応script actionを呼び出し元へ通知します。
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
            // script list JSONを出力した成功codeを返します。
            return 0;
        }
        // script inspect actionの詳細を作ります。
        if (action == L"inspect")
        {
            // inspect対象script pathがあるか確認します。
            if (requestedPath.empty())
            {
                // path指定のないscript inspectを拒否します。
                throw std::invalid_argument(
                    "script inspect requires --path.");
            }
            // relative: assets基準で解決したscript path。
            const auto relative = NormalizeAssetPath(
                projectRoot,
                requestedPath);
            // absolute: script sourceの出力内absolute path。
            const auto absolute = assetRoot / relative;
            // 指定されたscript sourceが存在するか確認します。
            if (!std::filesystem::is_regular_file(absolute))
            {
                // 存在しないscript sourceを呼び出し元へ通知します。
                throw std::runtime_error(
                    "The requested script was not found: "
                    + LamaPon::PathToUtf8(relative));
            }
            // contents: script file全体のsource text。
            const std::string contents = ReadTextFile(absolute);
            // record: pathから検索したasset metadata。
            const auto* record = database.FindByPath(relative);
            // report: script inspectの公開JSON response。
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
            // project inspect JSONを出力した成功codeを返します。
            return 0;
        }
        // record: importer名別に数える次のasset。
        throw std::invalid_argument(
            "Unknown script action: "
            + LamaPon::PathToUtf8(std::filesystem::path(action)));
    }
    // RunProjectInspect(requestedProject: project root): settingsとasset indexをJSONで返します。
    [[nodiscard]] int RunProjectInspect(
        const std::filesystem::path& requestedProject)
    {
        // projectRoot: inspect対象projectのcanonical path。
        const auto projectRoot = CanonicalProjectRoot(requestedProject);
        // settingsPath: project settings JSON path。
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // settings: CLI responseへ返すproject JSON。
        const auto settings = ReadJsonFile(settingsPath);
        // parsedSettings: startup sceneを含むvalidated settings。
        const auto parsedSettings = LamaPon::LoadProjectSettings(
            settingsPath);
        // database: project asset index。
        LamaPon::AssetDatabase database;
        // assetRoot: project内assets directory。
        const auto assetRoot =
            std::filesystem::weakly_canonical(
                projectRoot / L"assets");
        database.SetAssetRoot(assetRoot);
        // refresh: asset index refreshの統計値。
        const auto refresh = database.Refresh(false);
        // importerCounts: importerごとのasset件数。
        std::unordered_map<std::string, std::size_t> importerCounts;
        // importerCountsへasset importerごとの件数を集計します。
        for (const auto& record : database.Assets())
        {
            ++importerCounts[record.importer];
        }
        // counts: responseへ返すimporter count object。
        nlohmann::json counts = nlohmann::json::object();
        // importer/count: project内の各asset importer件数。
        for (const auto& [importer, count] : importerCounts)
        {
            counts[importer] = count;
        }
        // report: project inspectの公開JSON response。
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
        // project list JSONを出力した成功codeを返します。
        return 0;
    }
    // RunProjectList(): Hubに登録済みのrecent project一覧を返します。
    [[nodiscard]] int RunProjectList()
    {
        // projects: Hubに登録されたrecent project一覧。
        nlohmann::json projects = nlohmann::json::array();
        // project: Hubが記録する次のrecent project。
        for (const auto& project :
            LamaPon::Hub::LoadRecentProjects())
        {
            projects.push_back({
                { "name", project.name },
                { "path", LamaPon::PathToUtf8(project.path) },
            });
        }
        // report: project listの公開JSON response。
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
        // project registration responseの成功codeを返します。
        return 0;
    }
    // RunProjectRegistration(requestedProject: project root, add: 登録するか): Hub recent listを更新します。
    [[nodiscard]] int RunProjectRegistration(
        const std::filesystem::path& requestedProject,
        const bool add)
    {
        // projectRoot: add/remove対象の正規化済みpath。
        const auto projectRoot = add
            ? CanonicalProjectRoot(requestedProject)
            : std::filesystem::absolute(requestedProject)
                .lexically_normal();
        // add時はcanonical project pathをHubへ登録します。
        if (add)
        {
            LamaPon::Hub::AddRecentProject(projectRoot);
        }
        // addでない時はHubのrecent projectから削除します。
        else
        {
            // 移動・削除済みのパスもHubから掃除できるよう、removeはProjectとして存在することを要求しません。
            LamaPon::Hub::RemoveRecentProject(projectRoot);
        }
        // report: project registrationの公開JSON response。
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
        // project registration JSONを出力した成功codeを返します。
        return 0;
    }
    struct DirectoryInventory final
    {
        // entries: inventoryに含めたfilesystem entry数。
        std::uint64_t entries{};
        // bytes: inventoryに含めたregular file byte数。
        std::uintmax_t bytes{};
        // operator==(other: directory inventory): entry数とbyte数の一致を判定します。
        [[nodiscard]] bool operator==(
            const DirectoryInventory&) const noexcept = default;
    };
    // InventoryDirectory(root: tree root): recursive entry数とregular file byte数を集計します。
    [[nodiscard]] DirectoryInventory InventoryDirectory(
        const std::filesystem::path& root)
    {
        // result: directory内entry数とbyte数。
        DirectoryInventory result;
        // entry: recursive inventoryで調べる次のpath。
        for (const auto& entry :
            std::filesystem::recursive_directory_iterator(root))
        {
            ++result.entries;
            // regular fileのbyte数だけをinventoryへ加算します。
            if (entry.is_regular_file())
            {
                result.bytes += entry.file_size();
            }
        }
        // directory entriesとfile bytesの集計を返します。
        return result;
    }
    // ProjectPathKey(path: filesystem path): case-insensitive comparison用keyを返します。
    [[nodiscard]] std::wstring ProjectPathKey(
        const std::filesystem::path& path)
    {
        // key: path comparison用のcanonical lowercase key。
        auto key = std::filesystem::weakly_canonical(
            std::filesystem::absolute(path)).native();
        std::transform(
            key.begin(),
            key.end(),
            key.begin(),
            // converter(character: path文字): 大文字小文字を揃えます。
            [](const wchar_t character)
            {
                // character: Windows path比較用に小文字化する文字。
                return static_cast<wchar_t>(std::towlower(character));
            });
        // 大小文字を無視できるpath keyを返します。
        return key;
    }
    // IsSameOrNestedProjectPath(candidate: 検査path, parent: 親path): 同一または子pathか返します。
    [[nodiscard]] bool IsSameOrNestedProjectPath(
        const std::filesystem::path& candidate,
        const std::filesystem::path& parent)
    {
        // candidateKey: candidate pathのcomparison key。
        const auto candidateKey = ProjectPathKey(candidate);
        // parentKey: parent directoryのseparator付きcomparison key。
        auto parentKey = ProjectPathKey(parent);
        // candidate pathとparent pathが同一か確認します。
        if (candidateKey == parentKey)
        {
            // 同一pathならnested判定としてtrueを返します。
            return true;
        }
        // 親path separatorを補う必要があるか確認します。
        if (!parentKey.empty()
            && parentKey.back()
                != std::filesystem::path::preferred_separator)
        {
            parentKey.push_back(
                std::filesystem::path::preferred_separator);
        }
        // candidateがparent directory内にあるか返します。
        return candidateKey.starts_with(parentKey);
    }
    // RunProjectMove(requestedSource: source project, requestedDestination: destination path, allowInsideEngineSource: engine tree許可): 検証してprojectを移動します。
    [[nodiscard]] int RunProjectMove(
        const std::filesystem::path& requestedSource,
        const std::filesystem::path& requestedDestination,
        const bool allowInsideEngineSource)
    {
        // source: move元のcanonical project root。
        const auto source = CanonicalProjectRoot(requestedSource);
        // destination: move先のabsolute normalized path。
        const auto destination = std::filesystem::absolute(
            requestedDestination).lexically_normal();
        // sourceとdestinationの重複・入れ子を検査します。
        if (IsSameOrNestedProjectPath(destination, source)
            || IsSameOrNestedProjectPath(source, destination))
        {
            // sourceと重なるmove先を拒否します。
            throw std::invalid_argument(
                "The move destination must not be the source, its child,"
                " or its parent.");
        }
        // engine source tree内へのmoveを許可するか確認します。
        if (!allowInsideEngineSource
            && LamaPon::Hub::IsInsideEngineSourceTree(destination))
        {
            // game projectをengine source treeへ置くのを拒否します。
            throw std::runtime_error(
                "A game project cannot be moved inside the LamaPon"
                " source tree. Choose a folder beside the repository.");
        }
        // restoreEmptyDestination: 失敗時に戻す既存空directoryか。
        const bool restoreEmptyDestination =
            std::filesystem::exists(destination)
            && std::filesystem::is_directory(destination)
            && std::filesystem::is_empty(destination);
        // destinationが未作成または空directoryか確認します。
        if (std::filesystem::exists(destination)
            && !restoreEmptyDestination)
        {
            // 既存の空でないdestinationへのmoveを拒否します。
            throw std::runtime_error(
                "The move destination already exists and is not empty: "
                + LamaPon::PathToUtf8(destination));
        }
        std::filesystem::create_directories(destination.parent_path());
        // staging: destination隣に確保した一時copy path。
        std::filesystem::path staging;
        // staging pathに重複がないよう最大100回予約します。
        for (std::uint32_t attempt{}; attempt < 100u; ++attempt)
        {
            staging = destination.parent_path()
                / (destination.filename().wstring()
                    + L".lamapon-move-staging-"
                    + std::to_wstring(GetCurrentProcessId())
                    + L"-"
                    + std::to_wstring(GetTickCount64() + attempt));
            // 生成したstaging directory名が未使用か確認します。
            if (!std::filesystem::exists(staging))
            {
                // 未使用のstaging pathを確保したため探索を終えます。
                break;
            }
            staging.clear();
        }
        // staging pathを確保できたか確認します。
        if (staging.empty())
        {
            // staging pathを100回探しても空ならmoveできません。
            throw std::runtime_error(
                "Could not reserve a temporary move folder.");
        }
        // stagingExists: cleanup対象staging folderを作成済みか。
        bool stagingExists = false;
        // sourceをstagingへcopyして検証します。
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
            // sourceInventory: source treeのentry数とbyte数。
            const auto sourceInventory = InventoryDirectory(source);
            // stagingInventory: staging copyのentry数とbyte数。
            const auto stagingInventory = InventoryDirectory(staging);
            // staging projectとsource inventoryが一致するか確認します。
            if (!LamaPon::Hub::IsProject(staging)
                || sourceInventory != stagingInventory)
            {
                // 不完全または壊れたstaging copyを公開しません。
                throw std::runtime_error(
                    "The copied project did not pass the integrity check.");
            }
            // 空だったdestination folderを置き換えるか確認します。
            if (restoreEmptyDestination)
            {
                std::filesystem::remove(destination);
            }
            std::filesystem::rename(staging, destination);
            stagingExists = false;
        }
        // copy・検証・rename failure時にcleanupします。
        catch (...)
        {
            // staging copyが作られている場合に一時directoryを削除します。
            if (stagingExists)
            {
                // cleanupError: staging削除時のfilesystem error。
                std::error_code cleanupError;
                std::filesystem::remove_all(staging, cleanupError);
            }
            // 空destinationをremove済みならfailure後に復元します。
            if (restoreEmptyDestination
                && !std::filesystem::exists(destination))
            {
                // restoreError: 空destination復元時のfilesystem error。
                std::error_code restoreError;
                std::filesystem::create_directories(
                    destination,
                    restoreError);
            }
            // cleanup後に元のmove failureを再送出します。
            throw;
        }
        // sourceInventory: move元projectの最終inventory。
        const auto sourceInventory = InventoryDirectory(source);
        // destinationInventory: move先projectの最終inventory。
        const auto destinationInventory = InventoryDirectory(destination);
        // verified: destinationがprojectかつsourceと一致するか。
        const bool verified = LamaPon::Hub::IsProject(destination)
            && sourceInventory == destinationInventory;
        // hubUpdated: Hub recent list更新に成功したか。
        bool hubUpdated = false;
        // warning: copy後のHub/removal failure説明。
        std::string warning;
        // copy後の検証に成功した場合だけHubを更新します。
        if (verified)
        {
            // Hub recent listをdestinationへ切り替えます。
            try
            {
                LamaPon::Hub::AddRecentProject(destination);
                LamaPon::Hub::RemoveRecentProject(source);
                hubUpdated = true;
            }
            // exception: copy済みprojectを残しHub失敗をwarning化します。
            catch (const std::exception& exception)
            {
                warning = std::string{
                    "The project was copied, but Hub registration failed: " }
                    + exception.what();
            }
        }
        // sourceRemoved: move元directoryを完全に削除できたか。
        bool sourceRemoved = false;
        // 検証済みdestinationの場合だけsourceを削除します。
        if (verified)
        {
            // removeError: source tree削除時のfilesystem error。
            std::error_code removeError;
            std::filesystem::remove_all(source, removeError);
            sourceRemoved = !removeError
                && !std::filesystem::exists(source);
            // source tree全体を削除できたか確認します。
            if (!sourceRemoved)
            {
                // 既存warningとの間にseparatorを加えます。
                if (!warning.empty())
                {
                    warning += ' ';
                }
                warning +=
                    "The verified destination is intact, but the source"
                    " could not be completely removed.";
            }
        }
        // 最終検証に失敗した場合はsourceを保持します。
        else
        {
            warning =
                "The destination is intact, but the final integrity check"
                " changed; the source was kept.";
        }
        // ok: destination検証とsource削除の両方が完了したか。
        const bool ok = verified && sourceRemoved;
        // report: project moveの公開JSON response。
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
        // warningがある場合だけreportへ含めます。
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
        // moveとsource removalの両方が成功したか返します。
        return ok ? 0 : 1;
    }
    // RunRender(options: render settings): sceneを描画しimage・log・problem reportを出力します。
    [[nodiscard]] int RunRender(const RenderOptions& options)
    {
        // projectRoot: render対象projectのcanonical root。
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    options.projectRoot));
        // settingsPath: project settings JSON path。
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // project settings fileがあるか確認します。
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            // LamaPon projectではないdirectoryを拒否します。
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        // settings: render device設定とstartup scene。
        const LamaPon::ProjectSettings settings =
            LamaPon::LoadProjectSettings(settingsPath);
        // width: render target幅。
        const std::uint32_t width =
            options.width != 0
                ? options.width
                : settings.windowWidth;
        // height: render target高さ。
        const std::uint32_t height =
            options.height != 0
                ? options.height
                : settings.windowHeight;
        // scenePath: 描画に使うnormalized scene path。
        const auto scenePath = NormalizeScenePath(
            projectRoot,
            options.scene.empty()
                ? settings.startupScene
                : options.scene);
        Progress(
            "project: " + LamaPon::PathToUtf8(projectRoot));
        Progress(
            "scene:   " + LamaPon::PathToUtf8(scenePath));
        LamaPon::GraphicsDevice::SetPreferWarpAdapter(
            options.warp);
        LamaPon::GraphicsDevice::SetEnableDebugLayer(
            options.d3dDebug);
        // window: graphics device初期化用の非表示window。
        const HWND window =
            CreateHiddenWindow(width, height);
        // graphics: render・asset・inputを管理するdevice。
        LamaPon::GraphicsDevice graphics;
        LamaPon::SetGraphicsBackendPackageAssetRoot(
            projectRoot / L"assets");
        graphics.Initialize(
            window,
            width,
            height,
            settings.graphics.renderingApi,
            LamaPon::GraphicsStartupProfile::
                AllowD3D12ExperimentalRenderer);
        graphics.Assets().SetAssetRoot(
            projectRoot / L"assets");
        // graphicsSettings: render用に調整するproject graphics設定。
        auto graphicsSettings = settings.graphics;
        graphicsSettings.vSyncEnabled = false;
        graphics.SetGraphicsSettings(graphicsSettings);
        graphics.SetAsyncShaderCompilationEnabled(false);
        LamaPon::SetActivePhysicsSettings(settings.physics);
        // gameModule: projectのNativeScript DLLを管理するhost。
        LamaPon::GameModuleHost gameModule;
        // gameModulePath: project native Game Module DLL path。
        const auto gameModulePath = projectRoot
            / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        // gameModuleLoaded: Game Module DLLを正常loadできたか。
        bool gameModuleLoaded = false;
        // gameModuleError: Game Moduleの不在またはload error。
        std::string gameModuleError;
        gameModule.SetNativeSearchDirectories(
            LamaPon::PackageNativeSearchDirectories(
                LamaPon::ScanPackageNativeDependencies(
                    projectRoot / L"assets").packages));
        // Game Module DLLが存在する場合だけloadします。
        if (std::filesystem::exists(gameModulePath))
        {
            gameModuleLoaded =
                gameModule.Load(gameModulePath);
            // DLLが見つかってもloadできない場合を記録します。
            if (!gameModuleLoaded)
            {
                gameModuleError = gameModule.LastError();
                LamaPon::Logger::Instance().Warning(
                    "Game Moduleを読み込めませんでした。C++ Script"
                    "は動きません: " + gameModuleError);
            }
        }
        // Game Module DLLがない場合の理由を保持します。
        else
        {
            gameModuleError =
                "Game Moduleがありません（先にbuildしてください）: "
                + LamaPon::PathToUtf8(gameModulePath);
            // Scriptを使っていないプロジェクトでは正常なので、情報として残すだけにします。
            LamaPon::Logger::Instance().Info(gameModuleError);
        }
        // scene: render対象の読み込み済みscene。
        LamaPon::Scene scene(graphics);
        scene.SetRegisteredTags(settings.tags);
        // 指定sceneのloadをqueueします。
        if (!scene.Scenes().RequestLoad(scenePath))
        {
            // scene load requestに失敗した理由を通知します。
            throw std::runtime_error(
                scene.Scenes().LastError());
        }
        // queueしたsceneを同期処理し、読み込み完了を確認します。
        if (!scene.Scenes().ProcessPending())
        {
            // queueしたsceneが同期loadできたか確認します。
            throw std::runtime_error(
                scene.Scenes().LastError().empty()
                    ? "The scene did not finish loading."
                    : scene.Scenes().LastError());
        }
        // main cameraがsceneにあるか確認します。
        if (scene.MainCamera() == nullptr)
        {
            LamaPon::Logger::Instance().Warning(
                "シーンにメインカメラがありません。"
                "画面はクリア色のままになります。");
        }
        // sceneHasScripts: scene内にNativeScript componentがあるか。
        bool sceneHasScripts = false;
        // gameObject: NativeScript componentを探すscene object。
        for (const auto& gameObject : scene.GameObjects())
        {
            // gameObjectがscript判定できる有効objectか確認します。
            if (gameObject != nullptr
                && gameObject->GetComponent<
                    LamaPon::NativeScriptComponent>()
                    != nullptr)
            {
                sceneHasScripts = true;
                // scriptを持つobjectを見つけたため走査を終えます。
                break;
            }
        }
        // --inputのAction名を、実際に押すControlへ解決します。
        // ResolvedInput: scene input eventの解決済み内容。
        struct ResolvedInput final
        {
            // controls: 要求方向に一致する押下Control列。
            std::vector<LamaPon::InputControl> controls;
            // from: scene開始後のinput開始時刻。
            double from{};
            // to: scene開始後のinput終了時刻。
            double to{};
            // action: project settings上のinput Action名。
            std::string action;
            // value: input directionを表す正負符号。
            double value{ 1.0 };
        };
        // resolvedInputs: controlへ変換済みのinput event列。
        std::vector<ResolvedInput> resolvedInputs;
        // inputEnd: 最後に解決したinput event終了時刻。
        double inputEnd = 0.0;
        // event: action bindingを解決する次のinput event。
        for (const auto& event : options.inputEvents)
        {
            // found: event.actionに一致するsettings action iterator。
            const auto found = std::find_if(
                settings.inputActions.begin(),
                settings.inputActions.end(),
                [&event](const auto& action)
                {
                    // action: event action名と一致するdefinitionか判定します。
                    return action.name == event.action;
                });
            // project settingsにaction definitionがあるか確認します。
            if (found == settings.inputActions.end())
            {
                // 未定義action名のinput eventを拒否します。
                throw std::runtime_error(
                    "--input names an action that is not in "
                    "the project settings: " + event.action);
            }
            // resolved: controlを割り当てるinput event。
            ResolvedInput resolved;
            resolved.action = event.action;
            resolved.from = event.at;
            resolved.to = event.at
                + std::max(event.duration, 1.0 / 60.0);
            resolved.value = event.value < 0.0 ? -1.0 : 1.0;
            // wantsNegative: 負方向のbindingを選ぶか。
            const bool wantsNegative = resolved.value < 0.0;
            // binding: Actionに登録された現在のControl binding。
            for (const auto& binding : found->bindings)
            {
                // scaleの符号が要求方向と一致するか確認します。
                if (wantsNegative
                    ? binding.scale < 0.0f
                    : binding.scale > 0.0f)
                {
                    resolved.controls.push_back(
                        binding.control);
                }
            }
            // 解決後にControlが一つもないeventを拒否します。
            if (resolved.controls.empty())
            {
                // 対応する方向bindingがないことを通知します。
                throw std::runtime_error(
                    std::string{ "--input action has no " }
                    + (wantsNegative ? "negative" : "positive")
                    + " binding: " + event.action);
            }
            inputEnd = std::max(inputEnd, resolved.to);
            resolvedInputs.push_back(std::move(resolved));
        }
        // step: simulate update一回分の秒数。
        const float step = 1.0f / 60.0f;
        // steps: simulateSecondsから算出したupdate回数。
        auto steps = static_cast<std::uint32_t>(
            std::max(options.simulateSeconds, 0.0) * 60.0);
        // simulate未指定なら入力終了後までsimulationを延長します。
        if (!resolvedInputs.empty()
            && options.simulateSeconds <= 0.0)
        {
            // needed: input終了後の余韻を含めた最低update回数。
            const auto needed = static_cast<std::uint32_t>(
                (inputEnd + 0.25) * 60.0);
            steps = std::max(steps, needed);
        }
        // ScriptのStartを実行できるよう最低1回updateします。
        if (sceneHasScripts && steps == 0u)
        {
            steps = 1u;
        }
        // 必要なsimulation frameだけsceneを更新します。
        if (steps > 0u)
        {
            Progress(
                "simulate: "
                + std::to_string(steps) + " steps");
            // index: simulation updateのframe番号。
            for (std::uint32_t index = 0;
                index < steps;
                ++index)
            {
                // 押下時間帯をsnapshotへ反映し、Action遷移を再現します。
                if (!resolvedInputs.empty())
                {
                    // now: 現在のsimulation時刻。
                    const double now =
                        static_cast<double>(index) / 60.0;
                    // snapshot: 現在押下中のControl状態。
                    LamaPon::InputSnapshot snapshot;
                    // resolved: 適用時刻を確認するinput event。
                    for (const auto& resolved : resolvedInputs)
                    {
                        // eventの有効時間外なら現在frameへ反映しません。
                        if (now < resolved.from
                            || now >= resolved.to)
                        {
                            // 有効時間外のevent処理を次へ進めます。
                            continue;
                        }
                        // control: 現在押下するresolved Control。
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
        // clearColor: render targetを消去するRGBA値。
        const float clearColor[4]{
            0.025f, 0.035f, 0.055f, 1.0f };
        // pixels: captureするrender targetのRGBA bytes。
        std::vector<std::uint8_t> pixels;
        // capturedWidth: captureしたbackbufferの幅。
        std::uint32_t capturedWidth{};
        // capturedHeight: captureしたbackbufferの高さ。
        std::uint32_t capturedHeight{};
        // previousPresentation: 前回EndFrame完了時刻。
        auto previousPresentation =
            std::chrono::steady_clock::now();
        // frame: render loopの現在frame index。
        for (std::uint32_t frame = 0;
            frame < std::max(options.frames, 1u);
            ++frame)
        {
            // frameStart: 現frameの描画開始時刻。
            const auto frameStart =
                std::chrono::steady_clock::now();
            // フォールバック描画数は撮影フレームだけを計測します。
            if (frame + 1 >= std::max(options.frames, 1u))
            {
                graphics.ResetShaderFallbackDraws();
            }
            graphics.BeginFrame(clearColor);
            scene.RenderGameFrame(clearColor);
            // 撮影フレームだけ画像を取得します。
            if (frame + 1 >= std::max(options.frames, 1u))
            {
                pixels = graphics.CaptureBackBuffer(
                    capturedWidth,
                    capturedHeight);
            }
            graphics.EndFrame();
            // presentation: EndFrame後のpresent完了時刻。
            const auto presentation =
                std::chrono::steady_clock::now();
            graphics.RecordFrameStatistics(
                std::chrono::duration<float>(
                    presentation - previousPresentation).count(),
                std::chrono::duration<float, std::milli>(
                    presentation - frameStart).count());
            previousPresentation = presentation;
            // 次frameのScriptへ提示済みframe統計を渡します。
            if (frame + 1 < std::max(options.frames, 1u))
            {
                LamaPon::Time::Detail::AdvanceFrame(0.0f);
                scene.Update(0.0f);
            }
        }
        // outputPng: screenshotのabsolute output path。
        const auto outputPng =
            std::filesystem::absolute(options.outputPng);
        {
            // error: screenshot出力directory作成時のfilesystem error。
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
        // summary: capture画像から算出したcolor統計。
        const auto summary = Summarize(
            capturedWidth,
            capturedHeight,
            pixels);
        // errorCount: render中に記録したerror log数。
        std::size_t errorCount{};
        // warningCount: render中に記録したwarning log数。
        std::size_t warningCount{};
        // logs: non-info logger entryのJSON配列。
        auto logs = CollectLogs(errorCount, warningCount);
        // Shader初回compile結果を含めるため描画後にproblemを集めます。
        // problems: 描画後に検出したscene・shader問題。
        auto problems = CollectProblems(scene);
        // report.gameModule: Script実行可否とDLL診断情報。
        // report.shaderFallbackDraws: 撮影frameのfallback shader描画数。
        // report.problems: 描画で検出したscene問題。
        const nlohmann::json report{
            { "ok", true },
            { "command", "render" },
            { "project",
                LamaPon::PathToUtf8(projectRoot) },
            { "scene", LamaPon::PathToUtf8(scenePath) },
            { "frames", std::max(options.frames, 1u) },
            { "simulatedSeconds",
                static_cast<double>(steps) / 60.0 },
            { "gameModule", {
                { "loaded", gameModuleLoaded },
                { "sceneHasScripts", sceneHasScripts },
                { "path",
                    LamaPon::PathToUtf8(gameModulePath) },
                { "reason", gameModuleError } } },
            { "inputEvents", [&resolvedInputs]
                {
                    // events: JSON化する解決済みinput event列。
                    nlohmann::json events = nlohmann::json::array();
                    // resolved: JSONへ書き出すinput event。
                    for (const auto& resolved : resolvedInputs)
                    {
                        events.push_back({
                            { "action", resolved.action },
                            { "from", resolved.from },
                            { "to", resolved.to },
                            { "value", resolved.value } });
                    }
                    // reportへ解決済みevent一覧を返します。
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
            { "shaderFallbackDraws",
                graphics.FrameStats()
                    .shaderFallbackDraws },
            { "problems", std::move(problems) },
            { "errorCount", errorCount },
            { "warningCount", warningCount },
            { "logs", std::move(logs) },
        };
        // 不正UTF-8を置換し、stdoutのJSON契約を保ちます。
        std::cout
            << report.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
            << std::endl;
        // render reportを出力したことを終了code 0で示します。
        return 0;
    }
    // NewOptions: project作成先とtemplate設定。
    struct NewOptions final
    {
        // directory: 新規projectを作るdirectory。
        std::filesystem::path directory;
        // name: 空ならdirectory名をproject名に使います。
        std::string name;
        // projectTemplate: 初期sceneとassetを選ぶtemplate。
        LamaPon::Hub::ProjectTemplate projectTemplate{
            LamaPon::Hub::ProjectTemplate::LearningThreeDimensional
        };
        // allowInsideEngineSource: engine source内への作成を許すか。
        bool allowInsideEngineSource{};
    };
    // ProjectTemplateName(projectTemplate: project template): CLI用template名を返します。
    [[nodiscard]] std::string ProjectTemplateName(
        const LamaPon::Hub::ProjectTemplate projectTemplate)
    {
        // 選択templateに対応するCLI名を返します。
        switch (projectTemplate)
        {
        // 2D学習templateのCLI名を返します。
        case LamaPon::Hub::ProjectTemplate::LearningTwoDimensional:
            // 2D learning templateのCLI名を返します。
            return "learning-2d";
        // 3D学習templateのCLI名を返します。
        case LamaPon::Hub::ProjectTemplate::LearningThreeDimensional:
            // 3D learning templateのCLI名を返します。
            return "learning-3d";
        // 2D templateのCLI名を返します。
        case LamaPon::Hub::ProjectTemplate::TwoDimensional:
            // 2D templateのCLI名を返します。
            return "2d";
        // 3D templateと未知値のCLI名を返します。
        case LamaPon::Hub::ProjectTemplate::ThreeDimensional:
        default:
            // 3D templateを既定のCLI名で返します。
            return "3d";
        }
    }
    // RunNew(options: project作成設定): templateからprojectを作成し結果をJSON出力します。
    [[nodiscard]] int RunNew(const NewOptions& options)
    {
        // projectRoot: 作成するprojectのabsolute root。
        const auto projectRoot =
            std::filesystem::absolute(options.directory)
                .lexically_normal();
        // name: 指定名またはdirectory名から決めたproject名。
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
        // CLI作成projectをHubのrecent listへ登録します。
        LamaPon::Hub::AddRecentProject(projectRoot);
        Progress(
            "created: "
            + LamaPon::PathToUtf8(projectRoot));
        // errorCount: 作成中に記録したerror log数。
        std::size_t errorCount{};
        // warningCount: 作成中に記録したwarning log数。
        std::size_t warningCount{};
        // logs: non-info logger entryのJSON配列。
        auto logs = CollectLogs(errorCount, warningCount);
        // report.startupScene: 新規projectに用意される起動scene。
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
        // project作成reportを出力したことを終了code 0で示します。
        return 0;
    }
    // LearningStepJson(step: curriculum step, completed: 完了状態): 学習stepをJSON化します。
    [[nodiscard]] nlohmann::json LearningStepJson(
        const LamaPon::Hub::LearningStep& step,
        const bool completed)
    {
        // step metadataと完了状態を一つのJSON objectにします。
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
    // LearningStatusJson(projectRoot: 対象project, command: 呼び出しcommand): 学習進捗をJSON化します。
    [[nodiscard]] nlohmann::json LearningStatusJson(
        const std::filesystem::path& projectRoot,
        const std::string& command)
    {
        // journey: projectの学習curriculum。
        const auto journey =
            LamaPon::Hub::LoadLearningJourney(projectRoot);
        // progress: 保存済みの学習進捗。
        const auto progress =
            LamaPon::Hub::LoadLearningProgress(projectRoot);
        // status: 完了数・次step・選択roleを含む進捗状態。
        const auto status =
            LamaPon::Hub::GetLearningStatus(projectRoot);
        // completed: 完了済みstep IDの検索集合。
        const std::unordered_set<std::string> completed{
            progress.completedStepIds.begin(),
            progress.completedStepIds.end()
        };
        // steps: curriculum順に並べるJSON step一覧。
        nlohmann::json steps = nlohmann::json::array();
        // remainingMinutes: 未完了stepの見積もり時間合計。
        std::uint32_t remainingMinutes{};
        // step: JSONへ変換するcurriculum項目。
        for (const auto& step : journey.steps)
        {
            // isCompleted: 現stepが完了済みか。
            const bool isCompleted = completed.contains(step.id);
            steps.push_back(LearningStepJson(step, isCompleted));
            // 未完了stepの所要時間だけを残り時間へ加算します。
            if (!isCompleted)
            {
                remainingMinutes += step.estimatedMinutes;
            }
        }
        // nextStep: 次の学習step、または未設定を表すnull。
        nlohmann::json nextStep = nullptr;
        // 次stepがある場合は未完了としてJSON化します。
        if (status.nextStep.has_value())
        {
            nextStep = LearningStepJson(*status.nextStep, false);
        }
        // percent: 完了stepの整数percent。
        const std::size_t percent = status.totalSteps == 0
            ? 0
            : status.completedSteps * 100 / status.totalSteps;
        // journey・progressと利用可能な次stepをまとめて返します。
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
    // RunLearn(action: 学習操作, requestedProject: project path, requestedStep: 完了step, requestedRole: 学習role): 学習commandを実行します。
    [[nodiscard]] int RunLearn(
        const std::wstring_view action,
        const std::filesystem::path& requestedProject,
        const std::string& requestedStep,
        const std::string& requestedRole)
    {
        // projectRoot: 検証済みの学習対象project。
        const auto projectRoot =
            CanonicalProjectRoot(requestedProject);
        // doctorはcurriculum・project状態を診断して終了します。
        if (action == L"doctor")
        {
            // diagnosis: curriculum filesと参照先の検証結果。
            auto diagnosis =
                LamaPon::Hub::DiagnoseLearningJourney(projectRoot);
            // module: C++ Game Moduleのbuild状態。
            const auto module =
                LamaPon::InspectGameModuleBuildState(projectRoot);
            // Script sourceがあるprojectだけbuild診断を追加します。
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
            // checks: doctor診断項目のJSON一覧。
            nlohmann::json checks = nlohmann::json::array();
            // check: JSONへ変換する診断項目。
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
            // report: doctor診断と次に実行するcommand。
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
            // 診断結果がreadyの場合だけ成功codeを返します。
            return diagnosis.ready ? 0 : 1;
        }
        // initialized: 今回journey filesを新規作成したか。
        bool initialized = false;
        // initはjourneyがない場合にだけ初期化します。
        if (action == L"init")
        {
            // 未初期化projectなら学習journeyを作成します。
            if (!LamaPon::Hub::HasLearningJourney(projectRoot))
            {
                LamaPon::Hub::InitializeLearningJourney(projectRoot);
                initialized = true;
            }
        }
        // init以外ではjourney未設定のprojectを拒否します。
        else if (!LamaPon::Hub::HasLearningJourney(projectRoot))
        {
            // journey未初期化時に案内付きのerrorを返します。
            throw std::runtime_error(
                "This project has no learning journey. Run:"
                " LamaPonCli learn init --project <dir>");
        }
        // completedStep: 今回完了扱いにするstep ID。
        std::string completedStep;
        // completeは指定step、または現在の次stepを完了します。
        if (action == L"complete")
        {
            completedStep = requestedStep;
            // step未指定ならcurriculum上の次stepを選びます。
            if (completedStep.empty())
            {
                // status: 完了対象となる次stepを含む進捗状態。
                const auto status =
                    LamaPon::Hub::GetLearningStatus(projectRoot);
                // 次stepがなければcompletedStepは空のままです。
                if (status.nextStep.has_value())
                {
                    completedStep = status.nextStep->id;
                }
            }
            // 完了対象があるときだけprogress fileを更新します。
            if (!completedStep.empty())
            {
                LamaPon::Hub::CompleteLearningStep(
                    projectRoot,
                    completedStep);
            }
        }
        // roleは必須role名をprojectのprogressへ保存します。
        else if (action == L"role")
        {
            // 空のrole名はprogressを変更する前に拒否します。
            if (requestedRole.empty())
            {
                // --roleの欠落をCLI usage errorとして通知します。
                throw std::invalid_argument(
                    "learn role requires --role.");
            }
            LamaPon::Hub::SetLearningRole(
                projectRoot,
                requestedRole);
        }
        // resetはcurriculumを残してprogressだけ削除します。
        else if (action == L"reset")
        {
            LamaPon::Hub::ResetLearningProgress(projectRoot);
        }
        // 未対応actionをstatus/initの既定経路へ通しません。
        else if (action != L"status" && action != L"init")
        {
            // サポートされないlearn actionを拒否します。
            throw std::invalid_argument(
                "learn requires status, init, complete, role, reset, or doctor.");
        }
        // report: 実行後の学習状況とcommand名。
        auto report = LearningStatusJson(
            projectRoot,
            "learn " + LamaPon::PathToUtf8(
                std::filesystem::path(action)));
        // init結果には今回初期化したかを加えます。
        if (action == L"init")
        {
            report["initialized"] = initialized;
        }
        // complete結果には実際に完了したstep IDを加えます。
        if (action == L"complete")
        {
            report["completedStep"] = completedStep.empty()
                ? nlohmann::json(nullptr)
                : nlohmann::json(completedStep);
        }
        // reset成功をresponseへ明示します。
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
        // 学習responseを出力したことを終了code 0で示します。
        return 0;
    }
    // BuildOptions: Game Moduleをbuildするprojectとconfiguration。
    struct BuildOptions final
    {
        // projectRoot: build対象LamaPon project。
        std::filesystem::path projectRoot;
        // configuration: CLIと一致させるRelease/Debug構成。
        std::string configuration{
            LAMAPON_BUILD_CONFIGURATION };
    };
    // LooksLikeValidUtf8(bytes: 検査するbyte列): UTF-8として妥当か返します。
    [[nodiscard]] bool LooksLikeValidUtf8(
        const std::string_view bytes) noexcept
    {
        // index: 検査中のUTF-8先頭byte位置。
        std::size_t index = 0;
        // 残りbyteがある間、UTF-8 sequenceを順に検査します。
        while (index < bytes.size())
        {
            // lead: 現在sequenceの先頭byte。
            const auto lead =
                static_cast<unsigned char>(bytes[index]);
            // continuation: leadに続くUTF-8 byte数。
            std::size_t continuation = 0;
            // ASCII byteは後続byteを持ちません。
            if (lead < 0x80)
            {
                continuation = 0;
            }
            // 2-byte UTF-8 prefixを判定します。
            else if ((lead & 0xE0) == 0xC0)
            {
                continuation = 1;
            }
            // 3-byte UTF-8 prefixを判定します。
            else if ((lead & 0xF0) == 0xE0)
            {
                continuation = 2;
            }
            // 4-byte UTF-8 prefixを判定します。
            else if ((lead & 0xF8) == 0xF0)
            {
                continuation = 3;
            }
            // UTF-8 prefixではないbyteを拒否します。
            else
            {
                // 不正な先頭byteを検出したためfalseを返します。
                return false;
            }
            // sequence途中でbyte列が終わる場合は不正です。
            if (index + continuation >= bytes.size()
                && continuation > 0)
            {
                // 後続byteが不足しているためfalseを返します。
                return false;
            }
            // offset: continuation byteの現在位置。
            for (std::size_t offset = 1;
                offset <= continuation;
                ++offset)
            {
                // continuation byteが10xxxxxx形式か確認します。
                if ((static_cast<unsigned char>(
                        bytes[index + offset])
                    & 0xC0) != 0x80)
                {
                    // 不正なcontinuation byteを検出したためfalseを返します。
                    return false;
                }
            }
            index += continuation + 1;
        }
        // 全sequenceを読み切ったためUTF-8として受理します。
        return true;
    }
    // AcpToUtf8(bytes: ANSI code pageのbyte列): UTF-8文字列へ変換します。
    [[nodiscard]] std::string AcpToUtf8(
        const std::string_view bytes)
    {
        // 空byte列は空文字列として返します。
        if (bytes.empty())
        {
            // 変換対象がないため空文字列を返します。
            return {};
        }
        // wideLength: Windows APIが必要とするUTF-16 code unit数。
        const int wideLength = MultiByteToWideChar(
            CP_ACP,
            0,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0);
        // byte列をUTF-16へ変換できたか確認します。
        if (wideLength <= 0)
        {
            // Windows変換失敗を空文字列で表します。
            return {};
        }
        // wide: ANSI byte列を変換するUTF-16 buffer。
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
        // UTF-16 bufferを共通UTF-8形式へ変換します。
        return LamaPon::WideToUtf8(wide);
    }
    // ReadBuildLog(path: build log file): 混在encodingのlogをUTF-8化します。
    [[nodiscard]] std::string ReadBuildLog(
        const std::filesystem::path& path)
    {
        // input: encoding判定前のbinary log stream。
        std::ifstream input(path, std::ios::binary);
        // bytes: file全体から読んだ未変換byte列。
        const std::string bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>() };
        // result: UTF-8へ変換して連結するlog内容。
        std::string result;
        result.reserve(bytes.size());
        // begin: 次に変換する行の開始byte位置。
        std::size_t begin = 0;
        // log末尾まで一行ずつencodingを判定します。
        while (begin <= bytes.size())
        {
            // end: 現在行の終端byte位置。
            std::size_t end = bytes.find('\n', begin);
            // last: 現在行がfile最後の行か。
            const bool last = end == std::string::npos;
            // newlineがない最後の行はfile末尾を終端にします。
            if (last)
            {
                end = bytes.size();
            }
            // line: CRを除去してencoding判定する現在行。
            std::string_view line{
                bytes.data() + begin,
                end - begin };
            // Windows line endingのCRを取り除きます。
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            // UTF-8行はそのまま、他の行はANSI code pageから変換します。
            if (LooksLikeValidUtf8(line))
            {
                result.append(line);
            }
            // UTF-8として不正な行をANSI code pageから変換します。
            else
            {
                result.append(AcpToUtf8(line));
            }
            // 最終行を処理したらline loopを終えます。
            if (last)
            {
                // 最終行を読み終えたためloopを終了します。
                break;
            }
            result.push_back('\n');
            begin = end + 1;
        }
        // 行ごとに変換したbuild logを返します。
        return result;
    }
    // ExtractErrorLines(log: build log): error行を最大50件JSON化します。
    [[nodiscard]] nlohmann::json ExtractErrorLines(
        const std::string& log)
    {
        // lines: build failureの診断へ添付するerror行。
        auto lines = nlohmann::json::array();
        // begin: 次に調べるlog行の開始位置。
        std::size_t begin = 0;
        // 最大50行までlogを走査します。
        while (begin < log.size() && lines.size() < 50)
        {
            // end: 現在行の終端位置。
            std::size_t end = log.find('\n', begin);
            // 最終行ではlog末尾を終端として扱います。
            if (end == std::string::npos)
            {
                end = log.size();
            }
            // line: CR除去後にerror判定するlog行。
            std::string line =
                log.substr(begin, end - begin);
            // Windows line endingのCRをerror判定前に除去します。
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            // compiler・linkerのerror表記に一致する行を集めます。
            if (LamaPon::Cli::IsBuildErrorLine(line))
            {
                lines.push_back(line);
            }
            begin = end + 1;
        }
        // 見つかったerror行のJSON配列を返します。
        return lines;
    }
    // RunBuild(options: Game Module build設定): 同期build結果をJSON出力します。
    [[nodiscard]] int RunBuild(const BuildOptions& options)
    {
        // projectRoot: build対象projectのcanonical root。
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    options.projectRoot));
        // settingsPath: project識別に使う設定file path。
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // project settingsが存在しないdirectoryを拒否します。
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            // project settings欠落をbuild errorとして通知します。
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        // engineRoot: build commandへ渡す既定source root。
        auto engineRoot = std::filesystem::path{
            LAMAPON_DEFAULT_PROJECT_ROOT
        };
        // installedEngineRoot: 実行中CLIのdirectory。
        const auto installedEngineRoot =
            LamaPon::ExecutableDirectory();
        // CLI隣にbuild filesがあればinstalled engineを使います。
        if (std::filesystem::is_regular_file(
                installedEngineRoot
                    / L"tools"
                    / L"ProjectGameModule"
                    / L"CMakeLists.txt"))
        {
            engineRoot = installedEngineRoot;
        }
        // buildCommand: builderが解決したtool・path・arguments。
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
        // touchedStaleSources: timestampに頼らず再build対象にしたsource数。
        const int touchedStaleSources =
            LamaPon::RefreshStaleGameModuleSources(
                projectRoot,
                buildCommand.buildDirectory);
        // timeError: build開始前のDLL時刻取得error。
        std::error_code timeError;
        // moduleTimeBefore: build開始前のmodule更新時刻。
        const auto moduleTimeBefore =
            std::filesystem::last_write_time(
                buildCommand.outputModule,
                timeError);
        // comSpec: build commandを実行するWindows shell path。
        std::wstring comSpec(MAX_PATH, L'\0');
        // comSpecLength: environmentから読んだComSpec文字数。
        const DWORD comSpecLength =
            GetEnvironmentVariableW(
                L"ComSpec",
                comSpec.data(),
                MAX_PATH);
        comSpec.resize(
            comSpecLength > 0 && comSpecLength < MAX_PATH
                ? comSpecLength
                : 0);
        // ComSpec未設定ならWindows標準cmd.exeを使います。
        if (comSpec.empty())
        {
            comSpec = L"C:\\Windows\\System32\\cmd.exe";
        }
        // commandLine: shell pathとbuild argumentsを結合した起動文字列。
        std::wstring commandLine =
            L"\"" + comSpec + L"\" "
            + buildCommand.parameters;
        // startup: build child processの起動設定。
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        // process: 起動したbuild processのhandle情報。
        PROCESS_INFORMATION process{};
        // build child processをproject rootから起動します。
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
            // shell起動失敗をbuild errorとして通知します。
            throw std::runtime_error(
                "Could not start the build process.");
        }
        CloseHandle(process.hThread);
        // waitResult: 15分以内にbuild processが終了したか。
        const DWORD waitResult = WaitForSingleObject(
            process.hProcess,
            15u * 60u * 1000u);
        // timeout時はchildを停止してlog pathを返します。
        if (waitResult != WAIT_OBJECT_0)
        {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hProcess);
            // child processを停止したことをtimeout errorで通知します。
            throw std::runtime_error(
                "The build did not finish within 15"
                " minutes. See the log: "
                + LamaPon::PathToUtf8(
                    buildCommand.logPath));
        }
        // exitCode: build child processの終了code。
        DWORD exitCode = 1;
        static_cast<void>(GetExitCodeProcess(
            process.hProcess,
            &exitCode));
        CloseHandle(process.hProcess);
        // log: ANSI/UTF-8混在をUTF-8化したbuild出力。
        const std::string log =
            ReadBuildLog(buildCommand.logPath);
        // buildErrors: compiler・linkerのerror行。
        auto buildErrors = ExtractErrorLines(log);
        // existsError: output module確認時のfilesystem error。
        std::error_code existsError;
        // moduleExists: build output DLLが存在するか。
        bool moduleExists =
            std::filesystem::is_regular_file(
                buildCommand.outputModule,
                existsError);
        // attempt: module statを再確認する回数（最大10回）。
        for (int attempt = 0;
            !moduleExists && exitCode == 0 && attempt < 10;
            ++attempt)
        {
            // retry間隔を200ms空けます。
            Sleep(200);
            existsError.clear();
            moduleExists =
                std::filesystem::is_regular_file(
                    buildCommand.outputModule,
                    existsError);
        }
        // runtimeCompatible: DLLを現runtimeで起動できる見込みがあるか。
        bool runtimeCompatible = moduleExists;
        // runtimeTimeError: runtime DLL時刻取得error。
        std::error_code runtimeTimeError;
        // runtimePath: 起動時に照合するruntime DLL path。
        const auto runtimePath =
            LamaPon::ExecutableDirectory()
            / L"LamaPonRuntime.dll";
        // runtimeTime: runtime DLLの更新時刻。
        const auto runtimeTime = std::filesystem::last_write_time(
            runtimePath,
            runtimeTimeError);
        // compatibilityModuleTimeError: module時刻取得error。
        std::error_code compatibilityModuleTimeError;
        // compatibilityModuleTime: build output DLLの更新時刻。
        const auto compatibilityModuleTime =
            std::filesystem::last_write_time(
                buildCommand.outputModule,
                compatibilityModuleTimeError);
        // runtimeより古いDLLはbuild成功扱いにしません。
        if (runtimeTimeError || compatibilityModuleTimeError
            || compatibilityModuleTime < runtimeTime)
        {
            runtimeCompatible = false;
            buildErrors.push_back(
                "Game Module is still older than LamaPonRuntime.dll;"
                " the build did not relink the module.");
        }
        // 新しい時刻の古いDLLを除外するため埋め込みAPI versionも確認します。
        if (runtimeCompatible)
        {
            // builtApiVersion: output DLL内から読んだAPI version。
            const auto builtApiVersion =
                LamaPon::ReadGameModuleApiVersion(
                    buildCommand.outputModule);
            // DLL情報を読めない場合は互換性なしとします。
            if (!builtApiVersion.has_value())
            {
                runtimeCompatible = false;
                buildErrors.push_back(
                    "Game Module could not be inspected after the"
                    " build; it may be corrupt or missing"
                    " LamaPonGetGameModule.");
            }
            // DLL versionがengine要求値と一致するか確認します。
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
        // ok: process成功・DLL存在・runtime互換性を満たすか。
        const bool ok = exitCode == 0
            && moduleExists
            && runtimeCompatible;
        // afterError: build後のDLL時刻取得error。
        std::error_code afterError;
        // moduleTimeAfter: build後のoutput DLL更新時刻。
        const auto moduleTimeAfter =
            std::filesystem::last_write_time(
                buildCommand.outputModule,
                afterError);
        // moduleUpdated: build中にoutput DLL時刻が変化したか。
        const bool moduleUpdated =
            moduleExists
            && !afterError
            && (timeError
                || moduleTimeAfter != moduleTimeBefore);
        // report: build結果・DLL互換性・診断行。
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
        // error行だけで原因を示せない場合にlog末尾も添付します。
        if (!ok)
        {
            // tailLimit: reportへ含める最大log byte数。
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
        // build成功を0、build failureを1で返します。
        return ok ? 0 : 1;
    }
    // CliExportOptions: project export先・archive・署名設定。
    struct CliExportOptions final
    {
        // projectRoot: export対象LamaPon project。
        std::filesystem::path projectRoot;
        // outputDirectory: 空ならproject/exportへ書き出します。
        std::filesystem::path outputDirectory;
        // zip: 配布archiveも生成するか。
        bool zip{};
        // signing: 実行fileへ適用する署名設定。
        LamaPon::GameSigningOptions signing;
    };
    // RunExport(options: project export設定): 配布packageを作成しJSON報告します。
    [[nodiscard]] int RunExport(
        const CliExportOptions& options)
    {
        // projectRoot: export対象projectのcanonical root。
        const auto projectRoot =
            std::filesystem::weakly_canonical(
                std::filesystem::absolute(
                    options.projectRoot));
        // settingsPath: project識別に使う設定file path。
        const auto settingsPath =
            projectRoot / L".lamapon" / L"project.json";
        // project settingsが存在しないdirectoryを拒否します。
        if (!std::filesystem::is_regular_file(settingsPath))
        {
            // project settings欠落をexport errorとして通知します。
            throw std::runtime_error(
                "The folder is not a LamaPon project"
                " (missing .lamapon/project.json): "
                + LamaPon::PathToUtf8(projectRoot));
        }
        // settings: export packageへ反映するproject設定。
        const LamaPon::ProjectSettings settings =
            LamaPon::LoadProjectSettings(settingsPath);
        // gameModulePath: projectのC++ Script module DLL。
        const auto gameModulePath = projectRoot
            / L".lamapon" / L"bin"
            / L"LamaPonGameModule.dll";
        // gameModuleIncluded: export対象にmodule DLLが存在するか。
        const bool gameModuleIncluded =
            std::filesystem::is_regular_file(
                gameModulePath);
        // moduleがないprojectはScriptなしでexportされます。
        if (!gameModuleIncluded)
        {
            LamaPon::Logger::Instance().Warning(
                "Game Moduleが見つからないため、C++ Script"
                "無しで書き出します: "
                + LamaPon::PathToUtf8(gameModulePath));
        }
        // exportOptions: engine・assets・settingsをまとめたpackage設定。
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
        exportOptions.signing = options.signing;
        Progress(
            "export: "
            + LamaPon::PathToUtf8(
                exportOptions.outputDirectory));
        // result: package作成後のoutput pathと容量情報。
        const LamaPon::GameExportResult result =
            LamaPon::ExportGamePackage(exportOptions);
        // errorCount: export中に記録したerror log数。
        std::size_t errorCount{};
        // warningCount: export中に記録したwarning log数。
        std::size_t warningCount{};
        // logs: non-info logger entryのJSON配列。
        auto logs = CollectLogs(errorCount, warningCount);
        // report: package outputと署名・log状態。
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
            { "codeSigned", options.signing.enabled },
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
        // export reportの出力成功を終了code 0で返します。
        return 0;
    }
    // PrintUsage(): 利用可能なcommandとoption一覧をstderrへ表示します。
    void PrintUsage()
    {
        std::cerr <<
            "LamaPonCli - LamaPon projects without the"
            " editor GUI\n"
            "\n"
            "usage:\n"
            "  LamaPonCli version\n"
            "  LamaPonCli profile analyze <capture.json> [--first N] [--last M] [--top K]\n"
            "  LamaPonCli profile compare <a.json> <b.json> [--top K]\n"
            "  LamaPonCli memory summary <snapshot.json> [--top K]\n"
            "  LamaPonCli memory compare <a.json> <b.json> [--top K]\n"
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
            "  --d3ddebug        enable the Direct3D debug"
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
            "  --signtool <exe>  absolute Windows SDK signtool.exe path\n"
            "  --sign-cert-sha1 <hash>  Current User certificate thumbprint\n"
            "  --timestamp-url <url>   HTTPS RFC 3161 timestamp service\n"
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
            "  --d3ddebug          enable the Direct3D debug layer\n"
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
// wmain(argumentCount: argument数, arguments: CLI argv): commandを実行してJSON結果を返します。
int wmain(const int argumentCount, wchar_t** arguments)
{
    // stdout JSONはUTF-8のまま、Windows consoleにはUTF-8 code pageを設定します。
    SetConsoleOutputCP(CP_UTF8);
    // comResult: COM apartment初期化の結果。
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<void>(comResult);
    // command: 実行する最上位CLI command。
    const std::wstring_view command =
        argumentCount >= 2 ? arguments[1] : L"";
    // command処理中の例外をJSON error responseへ変換します。
    try
    {
        // index: 次に値を読むcommand-line argument位置。
        int index = 2;
        // next(index): 次の値付きoption argumentを取得します。
        const auto next =
            [&index, argumentCount, arguments]()
                -> std::wstring
            {
                // 必要なsubcommand argument数を確認します。
                if (index + 1 >= argumentCount)
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        LamaPon::PathToUtf8(
                            std::filesystem::path{
                                arguments[index] })
                        + " requires a value.");
                }
                // 取得したoption valueを呼び出し元へ返します。
                return arguments[++index];
            };
        // unknownOption(argument: 未知option): usage errorを作ります。
        const auto unknownOption =
            [](const std::wstring_view argument)
                -> std::invalid_argument
            {
                // 計算した値を呼び出し元へ返します。
                return std::invalid_argument(
                    "Unknown option: "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path{
                        argument }));
            };
        // build metadataとpackage互換versionを返します。
        if (command == L"version"
            || command == L"--version")
        {
            // build: version responseへ含めるbuild metadata。
            const auto& build = LamaPon::GetBuildInfo();
            // response: stdoutへ返すJSON response。
            const nlohmann::json response{
                { "ok", true },
                { "command", "version" },
                { "label", LamaPon::FormatBuildLabel() },
                { "branch", std::string(build.branch) },
                { "commit", std::string(build.commitFull) },
                { "commitShort", std::string(build.commit) },
                { "commitSubject", std::string(build.commitSubject) },
                { "dirty", build.dirty },
                { "compatibilityVersion",
                    std::string(LamaPon::VersionString) },
            };
            std::cout << response.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
                << std::endl;
            // commandを正常終了します。
            return 0;
        }
        // profile・memory subcommandへ残りargumentを渡します。
        if (command == L"profile" || command == L"memory")
        {
            // analysisArguments: analysis commandへ渡す残りargument列。
            std::vector<std::wstring_view> analysisArguments;
            // 残りのcommand-line argumentを順に解析します。
            // argument: analysis commandへ渡す現在argv index。
            for (int argument = 2; argument < argumentCount; ++argument)
            {
                analysisArguments.emplace_back(arguments[argument]);
            }
            // response: stdoutへ返すJSON response。
            const auto response = LamaPon::Cli::RunAnalysisCommand(
                command,
                analysisArguments);
            std::cout << response.dump(
                2,
                ' ',
                false,
                nlohmann::json::error_handler_t::replace)
                << std::endl;
            // commandを正常終了します。
            return 0;
        }
        // learn commandのoptionを解析します。
        if (command == L"learn")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "learn requires status, init, complete, role, reset, or doctor.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring_view action = arguments[2];
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // step: 学習上で完了するstep ID。
            std::string step;
            // role: projectへ設定する学習role名。
            std::string role;
            index = 3;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument = arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --step optionを取り込みます。
                else if (argument == L"--step")
                {
                    step = LamaPon::WideToUtf8(next());
                }
                // --role optionを取り込みます。
                else if (argument == L"--role")
                {
                    role = LamaPon::WideToUtf8(next());
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty())
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "learn requires --project.");
            }
            // stepが指定されているか確認します。
            if (!step.empty() && action != L"complete")
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--step is only valid for learn complete.");
            }
            // roleが指定されているか確認します。
            if (!role.empty() && action != L"role")
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--role is only valid for learn role.");
            }
            // 選択したcommandの終了codeを返します。
            return RunLearn(action, projectRoot, step, role);
        }
        // runtime commandのoptionを解析します。
        if (command == L"runtime")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "runtime requires start, status, send, stop, or worker.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring action = arguments[2];
            // startOptions: runtime start用の設定値。
            RuntimeStartOptions startOptions;
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // sessionId: 操作対象runtime session ID。
            std::wstring sessionId;
            // sessionDirectory: workerが使うruntime session directory。
            std::filesystem::path sessionDirectory;
            // commandFile: runtime commandを読むJSON file path。
            std::filesystem::path commandFile;
            // specFile: runtime test specification file path。
            std::filesystem::path specFile;
            // replayFile: 再生するruntime recording path。
            std::filesystem::path replayFile;
            // commandText: runtime send用のinline JSON text。
            std::wstring commandText;
            index = 3;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument = arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                    startOptions.projectRoot = projectRoot;
                }
                // --id optionを取り込みます。
                else if (argument == L"--id")
                {
                    sessionId = next();
                }
                // --session optionを取り込みます。
                else if (argument == L"--session")
                {
                    sessionDirectory = next();
                }
                // --scene optionを取り込みます。
                else if (argument == L"--scene")
                {
                    startOptions.scene = next();
                }
                // --width optionを取り込みます。
                else if (argument == L"--width")
                {
                    // value: --widthのparsed numeric value。
                    const auto value = std::stoi(next());
                    // option値が許可範囲内か検証します。
                    if (value <= 0)
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--width must be greater than zero.");
                    }
                    startOptions.width =
                        static_cast<std::uint32_t>(value);
                }
                // --height optionを取り込みます。
                else if (argument == L"--height")
                {
                    // value: --heightのparsed numeric value。
                    const auto value = std::stoi(next());
                    // option値が許可範囲内か検証します。
                    if (value <= 0)
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--height must be greater than zero.");
                    }
                    startOptions.height =
                        static_cast<std::uint32_t>(value);
                }
                // --fps optionを取り込みます。
                else if (argument == L"--fps")
                {
                    // value: --fpsのparsed numeric value。
                    const auto value = std::stoi(next());
                    // option値が許可範囲内か検証します。
                    if (value <= 0)
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--fps must be greater than zero.");
                    }
                    startOptions.targetFrameRate =
                        static_cast<std::uint32_t>(value);
                }
                // --fixed-delta optionを取り込みます。
                else if (argument == L"--fixed-delta")
                {
                    // value: --fixed-deltaのparsed numeric value。
                    const auto value = std::stof(next());
                    // option値が許可範囲内か検証します。
                    if (!std::isfinite(value) || value <= 0.0f)
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--fixed-delta must be finite and greater than zero.");
                    }
                    startOptions.fixedDeltaTime = value;
                }
                // --warp optionを取り込みます。
                else if (argument == L"--warp")
                {
                    startOptions.warp = true;
                }
                // --d3ddebug optionを取り込みます。
                else if (argument == L"--d3ddebug")
                {
                    startOptions.d3dDebug = true;
                }
                // --deterministic optionを取り込みます。
                else if (argument == L"--deterministic")
                {
                    startOptions.deterministic = true;
                }
                // --render-every optionを取り込みます。
                else if (argument == L"--render-every")
                {
                    // value: --render-everyのparsed numeric value。
                    const auto value = std::stoul(next());
                    // option値が許可範囲内か検証します。
                    if (value == 0 || value > 100'000)
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--render-every must be between 1 and 100000.");
                    }
                    startOptions.renderEveryNFrames =
                        static_cast<std::uint32_t>(value);
                }
                // --no-pace optionを取り込みます。
                else if (argument == L"--no-pace")
                {
                    startOptions.paceFrames = false;
                }
                // --record optionを取り込みます。
                else if (argument == L"--record")
                {
                    startOptions.recordPath = next();
                }
                // --spec optionを取り込みます。
                else if (argument == L"--spec")
                {
                    specFile = next();
                }
                // --file optionを取り込みます。
                else if (argument == L"--file")
                {
                    replayFile = next();
                }
                // --command optionを取り込みます。
                else if (argument == L"--command")
                {
                    commandText = next();
                }
                // --command-file optionを取り込みます。
                else if (argument == L"--command-file")
                {
                    commandFile = next();
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // worker actionを実行します。
            if (action == L"worker")
            {
                // sessionDirectoryが指定されているか確認します。
                if (sessionDirectory.empty())
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "runtime worker requires --session.");
                }
                // 選択したcommandの終了codeを返します。
                return RunRuntimeWorkerSafe(sessionDirectory);
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty())
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "runtime "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --project.");
            }
            projectRoot = std::filesystem::weakly_canonical(
                std::filesystem::absolute(projectRoot));
            // start actionを実行します。
            if (action == L"start")
            {
                // 選択したcommandの終了codeを返します。
                return RunRuntimeStart(startOptions);
            }
            // test actionを実行します。
            if (action == L"test")
            {
                // specFileが指定されているか確認します。
                if (specFile.empty())
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "runtime test requires --spec.");
                }
                // 選択したcommandの終了codeを返します。
                return RunRuntimeTest(projectRoot, specFile);
            }
            // replay actionを実行します。
            if (action == L"replay")
            {
                // replayFileが指定されているか確認します。
                if (replayFile.empty())
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "runtime replay requires --file.");
                }
                // 選択したcommandの終了codeを返します。
                return RunRuntimeReplay(projectRoot, replayFile);
            }
            // sessionIdが指定されているか確認します。
            if (sessionId.empty())
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "runtime "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --id.");
            }
            // status actionを実行します。
            if (action == L"status")
            {
                // 選択したcommandの終了codeを返します。
                return RunRuntimeStatus(projectRoot, sessionId);
            }
            // stop actionを実行します。
            if (action == L"stop")
            {
                // 選択したcommandの終了codeを返します。
                return RunRuntimeSend(
                    projectRoot,
                    sessionId,
                    nlohmann::json{ { "op", "stop" } });
            }
            // recover actionを実行します。
            if (action == L"recover")
            {
                // 選択したcommandの終了codeを返します。
                return RunRuntimeRecover(projectRoot, sessionId);
            }
            // send actionを実行します。
            if (action == L"send")
            {
                // commandTextが指定されているか確認します。
                if (commandText.empty() == commandFile.empty())
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "runtime send requires exactly one of "
                        "--command or --command-file.");
                }
                // runtimeCommand: 検証済みruntime操作JSON。
                const auto runtimeCommand = commandText.empty()
                    ? ReadJsonFile(commandFile)
                    : ParseRuntimeCommandText(commandText);
                // 選択したcommandの終了codeを返します。
                return RunRuntimeSend(
                    projectRoot,
                    sessionId,
                    runtimeCommand);
            }
            // 条件違反を呼び出し元へ通知します。
            throw std::invalid_argument(
                "Unknown runtime action: "
                + LamaPon::PathToUtf8(
                    std::filesystem::path(action)));
        }
        // job commandのoptionを解析します。
        if (command == L"job")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "job requires start, status, cancel, or list.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring_view action = arguments[2];
            // start actionを実行します。
            if (action == L"start")
            {
                // 必要なsubcommand argument数を確認します。
                if (argumentCount < 4)
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "job start requires an operation.");
                }
                // operation: job workerで起動するoperation名。
                const std::wstring operation = arguments[3];
                // operationArguments: workerへ渡すoperation argument列。
                std::vector<std::wstring> operationArguments{
                    operation };
                // argumentIndex: workerへ渡す現在argv位置。
                for (int argumentIndex = 4;
                    argumentIndex < argumentCount;
                    ++argumentIndex)
                {
                    operationArguments.push_back(
                        arguments[argumentIndex]);
                }
                // 選択したcommandの終了codeを返します。
                return RunJobStart(
                    operation,
                    operationArguments);
            }
            // worker actionを実行します。
            if (action == L"worker")
            {
                // directory: job workerのsession directory。
                std::filesystem::path directory;
                // argumentIndex: workerへ渡す現在argv位置。
                for (int argumentIndex = 3;
                    argumentIndex < argumentCount;
                    ++argumentIndex)
                {
                    // argument: 現在解析中のcommand-line option。
                    const std::wstring_view argument =
                        arguments[argumentIndex];
                    // --job optionを取り込みます。
                    if (argument == L"--job")
                    {
                        // 必要なsubcommand argument数を確認します。
                        if (argumentIndex + 1 >= argumentCount)
                        {
                            // 条件違反を呼び出し元へ通知します。
                            throw std::invalid_argument(
                                "--job requires a value.");
                        }
                        directory = arguments[++argumentIndex];
                    }
                    // 直前条件に該当しない場合を処理します。
                    else
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw unknownOption(argument);
                    }
                }
                // directoryが指定されているか確認します。
                if (directory.empty())
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "job worker requires --job.");
                }
                // 選択したcommandの終了codeを返します。
                return RunJobWorkerSafe(directory);
            }
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // jobId: status/cancel対象job ID。
            std::wstring jobId;
            // argumentIndex: workerへ渡す現在argv位置。
            for (int argumentIndex = 3;
                argumentIndex < argumentCount;
                ++argumentIndex)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[argumentIndex];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    // 必要なsubcommand argument数を確認します。
                    if (argumentIndex + 1 >= argumentCount)
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--project requires a value.");
                    }
                    projectRoot = arguments[++argumentIndex];
                }
                // --id optionを取り込みます。
                else if (argument == L"--id")
                {
                    // 必要なsubcommand argument数を確認します。
                    if (argumentIndex + 1 >= argumentCount)
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--id requires a value.");
                    }
                    jobId = arguments[++argumentIndex];
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty())
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "job "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --project.");
            }
            projectRoot = std::filesystem::weakly_canonical(
                std::filesystem::absolute(projectRoot));
            // list actionを実行します。
            if (action == L"list")
            {
                // 選択したcommandの終了codeを返します。
                return RunJobList(projectRoot);
            }
            // jobIdが指定されているか確認します。
            if (jobId.empty())
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "job "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path(action))
                    + " requires --id.");
            }
            // status actionを実行します。
            if (action == L"status")
            {
                // 選択したcommandの終了codeを返します。
                return RunJobStatus(projectRoot, jobId);
            }
            // cancel actionを実行します。
            if (action == L"cancel")
            {
                // 選択したcommandの終了codeを返します。
                return RunJobCancel(projectRoot, jobId);
            }
            // 条件違反を呼び出し元へ通知します。
            throw std::invalid_argument(
                "Unknown job action: "
                + LamaPon::PathToUtf8(
                    std::filesystem::path(action)));
        }
        // prefab commandのoptionを解析します。
        if (command == L"prefab")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "prefab requires inspect, validate, or patch.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring action = arguments[2];
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // prefabPath: inspect・patch対象Prefab path。
            std::filesystem::path prefabPath;
            // operations: 適用するPrefab patch operation file。
            std::filesystem::path operations;
            // output: patch結果を書き出すpath。
            std::filesystem::path output;
            // dryRun: patchを保存せず検証するか。
            bool dryRun{};
            index = 3;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --path optionを取り込みます。
                else if (argument == L"--path")
                {
                    prefabPath = next();
                }
                // --operations optionを取り込みます。
                else if (argument == L"--operations")
                {
                    operations = next();
                }
                // --out optionを取り込みます。
                else if (argument == L"--out")
                {
                    output = next();
                }
                // --dry-run optionを取り込みます。
                else if (argument == L"--dry-run")
                {
                    dryRun = true;
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty() || prefabPath.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "prefab requires --project and --path.");
            }
            // inspect actionを実行します。
            if (action == L"inspect")
            {
                // 選択したcommandの終了codeを返します。
                return RunPrefabInspect(projectRoot, prefabPath);
            }
            // validate actionを実行します。
            if (action == L"validate")
            {
                // 選択したcommandの終了codeを返します。
                return RunPrefabValidate(projectRoot, prefabPath);
            }
            // patch actionを実行します。
            if (action == L"patch")
            {
                // operationsが指定されているか確認します。
                if (operations.empty())
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "prefab patch requires --operations.");
                }
                // 選択したcommandの終了codeを返します。
                return RunPrefabPatch(
                    projectRoot,
                    prefabPath,
                    operations,
                    output,
                    dryRun);
            }
            // 条件違反を呼び出し元へ通知します。
            throw std::invalid_argument(
                "Unknown prefab action: "
                + LamaPon::PathToUtf8(std::filesystem::path(action)));
        }
        // project commandのoptionを解析します。
        if (command == L"project")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "project requires inspect, list, add, remove, or move.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring_view action = arguments[2];
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // destination: project move先directory。
            std::filesystem::path destination;
            // allowInsideEngineSource: engine source tree内の配置を許すか。
            bool allowInsideEngineSource{};
            index = 3;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --to optionを取り込みます。
                else if (argument == L"--to")
                {
                    destination = next();
                }
                // --allow-inside-engine optionを取り込みます。
                else if (argument == L"--allow-inside-engine")
                {
                    allowInsideEngineSource = true;
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // list actionを実行します。
            if (action == L"list")
            {
                // projectRootが指定されているか確認します。
                if (!projectRoot.empty()
                    || !destination.empty()
                    || allowInsideEngineSource)
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "project list does not accept --project.");
                }
                // 選択したcommandの終了codeを返します。
                return RunProjectList();
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "project inspect/add/remove/move requires --project.");
            }
            // move actionを実行します。
            if (action == L"move")
            {
                // destinationが指定されているか確認します。
                if (destination.empty())
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw std::invalid_argument(
                        "project move requires --to.");
                }
                // 選択したcommandの終了codeを返します。
                return RunProjectMove(
                    projectRoot,
                    destination,
                    allowInsideEngineSource);
            }
            // destinationが指定されているか確認します。
            if (!destination.empty() || allowInsideEngineSource)
            {
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--to and --allow-inside-engine are for project move.");
            }
            // inspect actionを実行します。
            if (action == L"inspect")
            {
                // 選択したcommandの終了codeを返します。
                return RunProjectInspect(projectRoot);
            }
            // add actionを実行します。
            if (action == L"add")
            {
                // 選択したcommandの終了codeを返します。
                return RunProjectRegistration(projectRoot, true);
            }
            // remove actionを実行します。
            if (action == L"remove")
            {
                // 選択したcommandの終了codeを返します。
                return RunProjectRegistration(projectRoot, false);
            }
            // 条件違反を呼び出し元へ通知します。
            throw std::invalid_argument(
                "Unknown project action: "
                + LamaPon::PathToUtf8(
                    std::filesystem::path(action)));
        }
        // component commandのoptionを解析します。
        if (command == L"component")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "component requires list or schema.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring action = arguments[2];
            // type: inspectするcomponent type名。
            std::string type;
            // category: filterするcomponent category名。
            std::string category;
            index = 3;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --type optionを取り込みます。
                if (argument == L"--type")
                {
                    type = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                // --category optionを取り込みます。
                else if (argument == L"--category")
                {
                    category = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // 選択したcommandの終了codeを返します。
            return RunComponentCommand(action, type, category);
        }
        // asset commandのoptionを解析します。
        if (command == L"asset")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "asset requires list or inspect.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring action = arguments[2];
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // assetPath: inspect対象asset path。
            std::filesystem::path assetPath;
            // targetPath: asset import先directory。
            std::filesystem::path targetPath;
            // sources: import対象source file列。
            std::vector<std::filesystem::path> sources;
            // guid: inspect対象asset GUID。
            std::string guid;
            // importer: asset listを絞るimporter名。
            std::string importer;
            index = 3;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --path optionを取り込みます。
                else if (argument == L"--path")
                {
                    assetPath = next();
                }
                // --guid optionを取り込みます。
                else if (argument == L"--guid")
                {
                    guid = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                // --importer optionを取り込みます。
                else if (argument == L"--importer")
                {
                    importer = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                // --source optionを取り込みます。
                else if (argument == L"--source")
                {
                    sources.push_back(next());
                }
                // --target optionを取り込みます。
                else if (argument == L"--target")
                {
                    targetPath = next();
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "asset requires --project.");
            }
            // import actionを実行します。
            if (action == L"import")
            {
                // 選択したcommandの終了codeを返します。
                return RunAssetImport(
                    projectRoot,
                    sources,
                    targetPath);
            }
            // 選択したcommandの終了codeを返します。
            return RunAssetCommand(
                projectRoot,
                action,
                assetPath,
                guid,
                importer);
        }
        // script commandのoptionを解析します。
        if (command == L"script")
        {
            // 必要なsubcommand argument数を確認します。
            if (argumentCount < 3)
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "script requires list or inspect.");
            }
            // action: command内で実行するsubcommand。
            const std::wstring action = arguments[2];
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // scriptPath: inspect・create対象script path。
            std::filesystem::path scriptPath;
            // className: 生成するScript class名。
            std::string className;
            // force: 既存script fileのoverwriteを許すか。
            bool force{};
            index = 3;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --path optionを取り込みます。
                else if (argument == L"--path")
                {
                    scriptPath = next();
                }
                // --class optionを取り込みます。
                else if (argument == L"--class")
                {
                    className = LamaPon::PathToUtf8(
                        std::filesystem::path{ next() });
                }
                // --force optionを取り込みます。
                else if (argument == L"--force")
                {
                    force = true;
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "script requires --project.");
            }
            // create actionを実行します。
            if (action == L"create")
            {
                // 選択したcommandの終了codeを返します。
                return RunScriptCreate(
                    projectRoot,
                    scriptPath,
                    className,
                    force);
            }
            // 選択したcommandの終了codeを返します。
            return RunScriptCommand(
                projectRoot,
                action,
                scriptPath);
        }
        // test commandのoptionを解析します。
        if (command == L"test")
        {
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // scene: inspect・test・patch対象scene path。
            std::filesystem::path scene;
            // specification: assertion specification JSON path。
            std::filesystem::path specification;
            // report: reportを書き出すproject内path。
            std::filesystem::path report;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --scene optionを取り込みます。
                else if (argument == L"--scene")
                {
                    scene = next();
                }
                // --spec optionを取り込みます。
                else if (argument == L"--spec")
                {
                    specification = next();
                }
                // --report optionを取り込みます。
                else if (argument == L"--report")
                {
                    report = next();
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty() || specification.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "test requires --project and --spec.");
            }
            // 選択したcommandの終了codeを返します。
            return RunSceneTests(
                projectRoot,
                scene,
                specification,
                report);
        }
        // patch commandのoptionを解析します。
        if (command == L"patch")
        {
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // scene: inspect・test・patch対象scene path。
            std::filesystem::path scene;
            // operations: 適用するPrefab patch operation file。
            std::filesystem::path operations;
            // output: patch結果を書き出すpath。
            std::filesystem::path output;
            // dryRun: patchを保存せず検証するか。
            bool dryRun{};
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --scene optionを取り込みます。
                else if (argument == L"--scene")
                {
                    scene = next();
                }
                // --operations optionを取り込みます。
                else if (argument == L"--operations")
                {
                    operations = next();
                }
                // --out optionを取り込みます。
                else if (argument == L"--out")
                {
                    output = next();
                }
                // --dry-run optionを取り込みます。
                else if (argument == L"--dry-run")
                {
                    dryRun = true;
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty() || operations.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "patch requires --project and --operations.");
            }
            // 選択したcommandの終了codeを返します。
            return RunPatch(
                projectRoot,
                scene,
                operations,
                output,
                dryRun);
        }
        // inspect commandのoptionを解析します。
        if (command == L"inspect"
            || command == L"validate")
        {
            // projectRoot: --projectで指定された対象project path。
            std::filesystem::path projectRoot;
            // scene: inspect・test・patch対象scene path。
            std::filesystem::path scene;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    projectRoot = next();
                }
                // --scene optionを取り込みます。
                else if (argument == L"--scene")
                {
                    scene = next();
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (projectRoot.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--project is required.");
            }
            // 計算した値を呼び出し元へ返します。
            return command == L"inspect"
                ? RunInspect(projectRoot, scene)
                : RunValidate(projectRoot, scene);
        }
        // render commandのoptionを解析します。
        if (command == L"render")
        {
            // options: 現在のcommand用に解析したoption設定。
            RenderOptions options;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    options.projectRoot = next();
                }
                // --scene optionを取り込みます。
                else if (argument == L"--scene")
                {
                    options.scene = next();
                }
                // --out optionを取り込みます。
                else if (argument == L"--out")
                {
                    options.outputPng = next();
                }
                // --width optionを取り込みます。
                else if (argument == L"--width")
                {
                    options.width = static_cast<
                        std::uint32_t>(
                        std::stoul(next()));
                }
                // --height optionを取り込みます。
                else if (argument == L"--height")
                {
                    options.height = static_cast<
                        std::uint32_t>(
                        std::stoul(next()));
                }
                // --frames optionを取り込みます。
                else if (argument == L"--frames")
                {
                    options.frames = static_cast<
                        std::uint32_t>(
                        std::stoul(next()));
                }
                // --simulate optionを取り込みます。
                else if (argument == L"--simulate")
                {
                    options.simulateSeconds =
                        std::stod(next());
                }
                // --input optionを取り込みます。
                else if (argument == L"--input")
                {
                    // --inputはAction@秒[:hold][=向き]をcomma区切りで受け取ります。
                    // specification: parseする--input全体のUTF-8 text。
                    const auto specification =
                        LamaPon::WideToUtf8(next());
                    // start: 次の--input eventの開始offset。
                    std::size_t start = 0;
                    // 条件に応じた処理を行います。
                    while (start <= specification.size())
                    {
                        // comma: 現在の--input eventを区切るcomma位置。
                        const auto comma =
                            specification.find(',', start);
                        // item: trim前の現在の--input event text。
                        auto item = specification.substr(
                            start,
                            comma == std::string::npos
                                ? std::string::npos
                                : comma - start);
                        start = comma == std::string::npos
                            ? specification.size() + 1
                            : comma + 1;
                        // first: --input item先頭の非空白位置。
                        const auto first =
                            item.find_first_not_of(" \t");
                        // command条件を満たす場合の処理へ進みます。
                        if (first == std::string::npos)
                        {
                            // 現在要素を飛ばして次へ進みます。
                            continue;
                        }
                        item = item.substr(
                            first,
                            item.find_last_not_of(" \t")
                                - first + 1);
                        RenderOptions::InputEvent event;
                        // at: Action名と開始時刻のseparator位置。
                        const auto at = item.find('@');
                        // command条件を満たす場合の処理へ進みます。
                        if (at == std::string::npos)
                        {
                            // 条件違反を呼び出し元へ通知します。
                            throw std::runtime_error(
                                "--input needs Action@seconds"
                                " (for example Jump@0.5): "
                                + item);
                        }
                        // name: @より前のAction名と任意のdirection。
                        auto name = item.substr(0, at);
                        // equals: Action名とinput方向を分けるseparator位置。
                        const auto equals = name.rfind('=');
                        // command条件を満たす場合の処理へ進みます。
                        if (equals != std::string::npos)
                        {
                            event.value = std::stod(
                                name.substr(equals + 1));
                            name = name.substr(0, equals);
                        }
                        event.action = std::move(name);
                        // timing: event開始時刻とhold時間のtext。
                        auto timing = item.substr(at + 1);
                        // colon: 開始時刻とhold時間のseparator位置。
                        const auto colon = timing.find(':');
                        // command条件を満たす場合の処理へ進みます。
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
                // --warp optionを取り込みます。
                else if (argument == L"--warp")
                {
                    options.warp = true;
                }
                // --d3ddebug optionを取り込みます。
                else if (argument == L"--d3ddebug")
                {
                    options.d3dDebug = true;
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (options.projectRoot.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--project is required.");
            }
            // 選択したcommandの終了codeを返します。
            return RunRender(options);
        }
        // new commandのoptionを解析します。
        if (command == L"new")
        {
            // options: 現在のcommand用に解析したoption設定。
            NewOptions options;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --dir optionを取り込みます。
                if (argument == L"--dir")
                {
                    options.directory = next();
                }
                // --name optionを取り込みます。
                else if (argument == L"--name")
                {
                    options.name =
                        LamaPon::PathToUtf8(
                            std::filesystem::path{
                                next() });
                }
                // --template optionを取り込みます。
                else if (argument == L"--template")
                {
                    // value: --templateのparsed numeric value。
                    const auto value = next();
                    // option値が許可範囲内か検証します。
                    if (value == L"2d")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::TwoDimensional;
                    }
                    // option値が許可範囲内か検証します。
                    else if (value == L"3d")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::ThreeDimensional;
                    }
                    // option値が許可範囲内か検証します。
                    else if (value == L"learning-3d"
                        || value == L"learn"
                        || value == L"tutorial")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::
                                LearningThreeDimensional;
                    }
                    // option値が許可範囲内か検証します。
                    else if (value == L"learning-2d")
                    {
                        options.projectTemplate =
                            LamaPon::Hub::ProjectTemplate::
                                LearningTwoDimensional;
                    }
                    // 直前条件に該当しない場合を処理します。
                    else
                    {
                        // 条件違反を呼び出し元へ通知します。
                        throw std::invalid_argument(
                            "--template must be 3d, 2d, learning-3d, or learning-2d.");
                    }
                }
                // --allow-inside-engine optionを取り込みます。
                else if (argument == L"--allow-inside-engine")
                {
                    options.allowInsideEngineSource = true;
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // directoryが指定されているか確認します。
            if (options.directory.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--dir is required.");
            }
            // 選択したcommandの終了codeを返します。
            return RunNew(options);
        }
        // build commandのoptionを解析します。
        if (command == L"build")
        {
            // options: 現在のcommand用に解析したoption設定。
            BuildOptions options;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    options.projectRoot = next();
                }
                // --config optionを取り込みます。
                else if (argument == L"--config")
                {
                    options.configuration =
                        LamaPon::PathToUtf8(
                            std::filesystem::path{
                                next() });
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (options.projectRoot.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--project is required.");
            }
            // 選択したcommandの終了codeを返します。
            return RunBuild(options);
        }
        // export commandのoptionを解析します。
        if (command == L"export")
        {
            // options: 現在のcommand用に解析したoption設定。
            CliExportOptions options;
            // 残りのcommand-line argumentを順に解析します。
            for (; index < argumentCount; ++index)
            {
                // argument: 現在解析中のcommand-line option。
                const std::wstring_view argument =
                    arguments[index];
                // --project optionを取り込みます。
                if (argument == L"--project")
                {
                    options.projectRoot = next();
                }
                // --out optionを取り込みます。
                else if (argument == L"--out")
                {
                    options.outputDirectory = next();
                }
                // --zip optionを取り込みます。
                else if (argument == L"--zip")
                {
                    options.zip = true;
                }
                // --signtool optionを取り込みます。
                else if (argument == L"--signtool")
                {
                    options.signing.enabled = true;
                    options.signing.signToolPath = next();
                }
                // --sign-cert-sha1 optionを取り込みます。
                else if (argument == L"--sign-cert-sha1")
                {
                    options.signing.enabled = true;
                    // value: --sign-cert-sha1のparsed numeric value。
                    const auto value = next();
                    options.signing.certificateSha1 =
                        LamaPon::WideToUtf8(value);
                }
                // --timestamp-url optionを取り込みます。
                else if (argument == L"--timestamp-url")
                {
                    options.signing.enabled = true;
                    // value: --timestamp-urlのparsed numeric value。
                    const auto value = next();
                    options.signing.timestampUrl =
                        LamaPon::WideToUtf8(value);
                }
                // 直前条件に該当しない場合を処理します。
                else
                {
                    // 条件違反を呼び出し元へ通知します。
                    throw unknownOption(argument);
                }
            }
            // projectRootが指定されているか確認します。
            if (options.projectRoot.empty())
            {
                PrintUsage();
                // 条件違反を呼び出し元へ通知します。
                throw std::invalid_argument(
                    "--project is required.");
            }
            // 選択したcommandの終了codeを返します。
            return RunExport(options);
        }
        PrintUsage();
        // 条件違反を呼び出し元へ通知します。
        throw std::invalid_argument(
            command.empty()
                ? "No command was given."
                : "Unknown command: "
                    + LamaPon::PathToUtf8(
                        std::filesystem::path{
                            command }));
    }
    // command処理の例外をJSON error responseへ変換します。
    catch (const std::exception& exception)
    {
        // 失敗時もstdoutにはJSONを1件だけ出力し、診断に必要なログを同じレポートへ含めます。
        // errorCount: render中に記録したerror log数。
        std::size_t errorCount{};
        // warningCount: render中に記録したwarning log数。
        std::size_t warningCount{};
        // logs: non-info logger entryのJSON配列。
        auto logs = CollectLogs(errorCount, warningCount);
        // report: reportを書き出すproject内path。
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
        // 計算した値を呼び出し元へ返します。
        return 1;
    }
}
