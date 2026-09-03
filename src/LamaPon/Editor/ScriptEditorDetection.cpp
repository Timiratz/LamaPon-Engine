#include "LamaPon/Editor/ScriptEditorDetection.h"

#include <Windows.h>
#include <KnownFolders.h>
#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <string_view>

namespace
{
    std::filesystem::path KnownFolder(const KNOWNFOLDERID& id)
    {
        PWSTR value{};
        const HRESULT result = SHGetKnownFolderPath(
            id,
            0,
            nullptr,
            &value);
        if (FAILED(result) || value == nullptr)
        {
            if (value != nullptr)
            {
                CoTaskMemFree(value);
            }
            return {};
        }
        const std::filesystem::path path{ value };
        CoTaskMemFree(value);
        return path;
    }

    void AddIfExecutableExists(
        std::vector<LamaPon::ScriptEditorOption>& options,
        std::string label,
        const std::filesystem::path& executablePath)
    {
        std::error_code error;
        if (!std::filesystem::is_regular_file(
                executablePath,
                error))
        {
            return;
        }
        // 同じ実行ファイルを重複して追加しない
        // （例: ProgramFilesとProgramFiles(x86)を両方調べるため）。
        const bool alreadyPresent = std::ranges::any_of(
            options,
            [&executablePath](
                const LamaPon::ScriptEditorOption& option)
            {
                return option.executablePath == executablePath;
            });
        if (alreadyPresent)
        {
            return;
        }
        options.push_back(
            {
                std::move(label),
                executablePath
            });
    }

    void DetectVisualStudioCode(
        std::vector<LamaPon::ScriptEditorOption>& options)
    {
        const auto userPrograms =
            KnownFolder(FOLDERID_UserProgramFiles);
        if (!userPrograms.empty())
        {
            AddIfExecutableExists(
                options,
                "Visual Studio Code",
                userPrograms
                    / L"Microsoft VS Code"
                    / L"Code.exe");
            AddIfExecutableExists(
                options,
                "Visual Studio Code - Insiders",
                userPrograms
                    / L"Microsoft VS Code Insiders"
                    / L"Code - Insiders.exe");
        }
        for (const auto& folderId :
            { FOLDERID_ProgramFiles, FOLDERID_ProgramFilesX86 })
        {
            const auto programFiles = KnownFolder(folderId);
            if (programFiles.empty())
            {
                continue;
            }
            AddIfExecutableExists(
                options,
                "Visual Studio Code",
                programFiles
                    / L"Microsoft VS Code"
                    / L"Code.exe");
        }
    }

    // 子プロセスの標準出力を1回のCreateProcessでキャプチャして返します
    // （vswhere.exeの結果取得専用の小さなヘルパー）。
    std::string RunProcessCaptureOutput(
        const std::filesystem::path& executable,
        const std::wstring& arguments)
    {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE readHandle{};
        HANDLE writeHandle{};
        if (!CreatePipe(
            &readHandle,
            &writeHandle,
            &security,
            0))
        {
            return {};
        }
        if (!SetHandleInformation(
            readHandle,
            HANDLE_FLAG_INHERIT,
            0))
        {
            CloseHandle(readHandle);
            CloseHandle(writeHandle);
            return {};
        }

        STARTUPINFOW startupInfo{};
        startupInfo.cb = sizeof(startupInfo);
        startupInfo.dwFlags = STARTF_USESTDHANDLES;
        startupInfo.hStdOutput = writeHandle;
        startupInfo.hStdError = writeHandle;

        std::wstring commandLine =
            L"\"" + executable.wstring() + L"\" " + arguments;

        PROCESS_INFORMATION processInfo{};
        const bool started = CreateProcessW(
            nullptr,
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo);
        CloseHandle(writeHandle);
        if (!started)
        {
            CloseHandle(readHandle);
            return {};
        }

        std::string output;
        std::array<char, 4096> buffer{};
        DWORD bytesRead{};
        while (ReadFile(
            readHandle,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            nullptr)
            && bytesRead > 0)
        {
            output.append(buffer.data(), bytesRead);
        }
        CloseHandle(readHandle);

        WaitForSingleObject(processInfo.hProcess, 10000);
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);
        return output;
    }

    void DetectVisualStudio(
        std::vector<LamaPon::ScriptEditorOption>& options)
    {
        const auto programFilesX86 =
            KnownFolder(FOLDERID_ProgramFilesX86);
        if (programFilesX86.empty())
        {
            return;
        }
        const auto vswhere =
            programFilesX86
            / L"Microsoft Visual Studio"
            / L"Installer"
            / L"vswhere.exe";
        std::error_code error;
        if (!std::filesystem::is_regular_file(vswhere, error))
        {
            return;
        }

        // -requires Microsoft.Component.MSBuildで、C++開発に使えない
        // 素のインストーラーだけの状態などを除外します。
        const std::string output = RunProcessCaptureOutput(
            vswhere,
            L"-all -prerelease -products * "
            L"-requires Microsoft.Component.MSBuild -nologo");

        std::string label;
        std::string productPath;
        const auto flush =
            [&options, &label, &productPath]()
            {
                if (!label.empty() && !productPath.empty())
                {
                    AddIfExecutableExists(
                        options,
                        label,
                        std::filesystem::path(productPath));
                }
                label.clear();
                productPath.clear();
            };

        constexpr std::string_view displayNamePrefix{
            "displayName: "
        };
        constexpr std::string_view productPathPrefix{
            "productPath: "
        };
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                flush();
                continue;
            }
            if (line.starts_with(displayNamePrefix))
            {
                label = line.substr(displayNamePrefix.size());
            }
            else if (line.starts_with(productPathPrefix))
            {
                productPath =
                    line.substr(productPathPrefix.size());
            }
        }
        flush();
    }
}

namespace LamaPon
{
    std::vector<ScriptEditorOption> DetectScriptEditors()
    {
        std::vector<ScriptEditorOption> options;
        DetectVisualStudioCode(options);
        DetectVisualStudio(options);
        return options;
    }
}
